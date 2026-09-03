// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

#include "rcufib/epoch.hpp"

using namespace rcufib;

namespace {

/// Counts its own destruction so a test can observe exactly when reclamation
/// happened rather than inferring it.
struct tracked {
    static std::atomic<int> live;
    static std::atomic<int> destroyed;

    tracked() { live.fetch_add(1, std::memory_order_relaxed); }
    ~tracked() {
        live.fetch_sub(1, std::memory_order_relaxed);
        destroyed.fetch_add(1, std::memory_order_relaxed);
    }

    static void reset() {
        live.store(0, std::memory_order_relaxed);
        destroyed.store(0, std::memory_order_relaxed);
    }
};

std::atomic<int> tracked::live{0};
std::atomic<int> tracked::destroyed{0};

}  // namespace

TEST(Epoch, RetiredObjectsAreEventuallyFreed) {
    tracked::reset();
    {
        epoch_domain domain;
        for (int i = 0; i < 10; ++i) domain.retire(new tracked());
        EXPECT_EQ(tracked::live.load(), 10) << "retire must not free immediately";
        domain.synchronize();
        EXPECT_EQ(tracked::live.load(), 0);
        EXPECT_EQ(domain.pending(), 0u);
    }
    EXPECT_EQ(tracked::live.load(), 0);
}

TEST(Epoch, NothingIsFreedWhileAReaderIsPinned) {
    tracked::reset();
    epoch_domain domain;

    std::atomic<bool> pinned{false};
    std::atomic<bool> release{false};

    std::thread reader([&] {
        const epoch_domain::read_guard guard(domain);
        pinned.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    });

    while (!pinned.load(std::memory_order_acquire)) std::this_thread::yield();

    for (int i = 0; i < 5; ++i) domain.retire(new tracked());
    // The reader is pinned to the current epoch, so no amount of trying can
    // make it safe to free what was retired in that epoch.
    for (int attempt = 0; attempt < 10; ++attempt) domain.try_reclaim();
    EXPECT_EQ(tracked::live.load(), 5);
    EXPECT_GT(domain.stats().failed_advances, 0u);

    release.store(true, std::memory_order_release);
    reader.join();

    domain.synchronize();
    EXPECT_EQ(tracked::live.load(), 0);
}

TEST(Epoch, GuardsNest) {
    epoch_domain domain;
    EXPECT_FALSE(domain.is_pinned());
    {
        const epoch_domain::read_guard outer(domain);
        EXPECT_TRUE(domain.is_pinned());
        {
            const epoch_domain::read_guard inner(domain);
            EXPECT_TRUE(domain.is_pinned());
        }
        // Leaving the inner guard must not unpin the thread; the outer one is
        // still reading.
        EXPECT_TRUE(domain.is_pinned());
    }
    EXPECT_FALSE(domain.is_pinned());
}

TEST(Epoch, EpochAdvancesWhenNobodyIsReading) {
    epoch_domain domain;
    const auto before = domain.current_epoch();
    domain.try_reclaim();
    EXPECT_GT(domain.current_epoch(), before);
}

TEST(Epoch, StatsAccountForEveryRetiredObject) {
    tracked::reset();
    epoch_domain domain;
    for (int i = 0; i < 100; ++i) domain.retire(new tracked());
    domain.synchronize();

    const auto stats = domain.stats();
    EXPECT_EQ(stats.retired, 100u);
    EXPECT_EQ(stats.reclaimed, 100u);
    EXPECT_EQ(stats.backlog(), 0u);
    EXPECT_EQ(tracked::destroyed.load(), 100);
}

TEST(Epoch, RetiringNullIsIgnored) {
    epoch_domain domain;
    domain.retire<tracked>(nullptr);
    EXPECT_EQ(domain.stats().retired, 0u);
}

TEST(Epoch, GarbageFromAnExitedThreadIsAdopted) {
    tracked::reset();
    epoch_domain domain;

    // A thread retires objects and then exits without reclaiming them. Its
    // bags must not be lost with it.
    std::thread worker([&] {
        for (int i = 0; i < 20; ++i) domain.retire(new tracked());
    });
    worker.join();

    EXPECT_GT(domain.pending(), 0u);
    domain.synchronize();
    EXPECT_EQ(tracked::live.load(), 0) << "orphaned garbage was leaked";
    EXPECT_EQ(domain.pending(), 0u);
}

TEST(Epoch, RecordsAreReusedByLaterThreads) {
    epoch_domain domain;
    for (int round = 0; round < 8; ++round) {
        std::thread worker([&] { const epoch_domain::read_guard guard(domain); });
        worker.join();
    }
    // Eight sequential threads should share a small number of records rather
    // than allocating one each and leaking them.
    EXPECT_LE(domain.stats().threads_registered, 3u);
}

TEST(Epoch, ManyReadersAndOneWriterMakeProgress) {
    tracked::reset();
    epoch_domain domain;

    constexpr int reader_count = 4;
    constexpr int iterations = 2000;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};

    std::vector<std::thread> readers;
    readers.reserve(reader_count);
    for (int i = 0; i < reader_count; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const epoch_domain::read_guard guard(domain);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int i = 0; i < iterations; ++i) {
        domain.retire(new tracked());
        domain.try_reclaim();
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) reader.join();

    domain.synchronize();
    EXPECT_EQ(tracked::live.load(), 0);
    EXPECT_GT(reads.load(), 0u);
    // Readers pin only briefly, so the writer should have been able to advance
    // repeatedly rather than being starved.
    EXPECT_GT(domain.stats().epoch_advances, 10u);
}

TEST(Epoch, SeparateDomainsDoNotInterfere) {
    tracked::reset();
    epoch_domain first;
    epoch_domain second;

    first.retire(new tracked());
    second.retire(new tracked());

    // Pinning one domain says nothing about the other.
    {
        const epoch_domain::read_guard guard(first);
        EXPECT_TRUE(first.is_pinned());
        EXPECT_FALSE(second.is_pinned());
    }

    first.synchronize();
    second.synchronize();
    EXPECT_EQ(tracked::live.load(), 0);
}

TEST(Epoch, DomainDestructionFreesWhatIsStillPending) {
    tracked::reset();
    {
        epoch_domain domain;
        for (int i = 0; i < 7; ++i) domain.retire(new tracked());
        EXPECT_EQ(tracked::live.load(), 7);
    }
    EXPECT_EQ(tracked::live.load(), 0) << "the destructor must not leak pending garbage";
}

TEST(Epoch, ConcurrentRetireFromManyThreadsLosesNothing) {
    tracked::reset();
    epoch_domain domain;

    constexpr int threads = 4;
    constexpr int per_thread = 500;
    std::barrier sync(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            sync.arrive_and_wait();
            for (int i = 0; i < per_thread; ++i) {
                domain.retire(new tracked());
                if (i % 32 == 0) domain.try_reclaim();
            }
            // Reclaim this thread's own bags before it exits; anything left
            // becomes an orphan the domain adopts.
            domain.synchronize();
        });
    }
    for (auto& worker : workers) worker.join();

    domain.synchronize();
    EXPECT_EQ(domain.stats().retired, threads * per_thread);
    EXPECT_EQ(tracked::live.load(), 0);
}
