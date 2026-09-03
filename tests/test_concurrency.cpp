// SPDX-License-Identifier: MIT
//
// The tests that matter. Everything here is written to be run under
// ThreadSanitizer and AddressSanitizer, because the failure modes these guard
// against - a reader following a pointer the writer just freed, a half-built
// node becoming visible - do not reliably reproduce without them.
//
// The central invariant is self-consistency: the writer derives every entry's
// gateway from its generation by a fixed function, so any entry a reader
// observes must satisfy that relation. A torn read, a freed entry or a
// partially published node all break it.

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <thread>
#include <vector>

#include "rcufib/fib.hpp"
#include "rcufib/rib.hpp"
#include "rcufib/synthetic.hpp"

using namespace rcufib;
using namespace std::chrono_literals;

namespace {

/// The relation every published entry must satisfy.
constexpr std::uint32_t gateway_for(std::uint64_t generation) {
    return static_cast<std::uint32_t>(generation * 2654435761ULL);
}

fib_entry entry_for(std::uint64_t generation) {
    return fib_entry{.hop = next_hop{.gateway = gateway_for(generation),
                                     .interface = static_cast<std::uint32_t>(generation & 0xFFU),
                                     .label = 0,
                                     .metric = 0},
                     .source = protocol::bgp,
                     .generation = generation};
}

bool consistent(const fib_entry& entry) {
    return entry.hop.gateway == gateway_for(entry.generation) &&
           entry.hop.interface == static_cast<std::uint32_t>(entry.generation & 0xFFU);
}

ipv4_prefix p4(const char* text) {
    return *ipv4_prefix::parse(text);
}

/// Shorter runs under a sanitizer: the point there is to exercise the
/// interleavings, and the instrumentation is 10-20x slower.
constexpr std::chrono::milliseconds run_time() {
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    return 300ms;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
    return 300ms;
#else
    return 700ms;
#endif
#else
    return 700ms;
#endif
}

}  // namespace

template <class Fib>
class ConcurrencyTest : public ::testing::Test {};

using fib_types = ::testing::Types<mutex_fib<ipv4_address>, shared_mutex_fib<ipv4_address>,
                                   seqlock_fib<ipv4_address>, rcu_fib<ipv4_address>>;

class fib_names {
public:
    template <class T>
    static std::string GetName(int) {
        return std::string(T::name);
    }
};

TYPED_TEST_SUITE(ConcurrencyTest, fib_types, fib_names);

TYPED_TEST(ConcurrencyTest, ReadersNeverObserveAnInconsistentEntry) {
    TypeParam fib;
    const auto prefixes = generate_prefixes({.prefix_count = 4'000, .seed = 21});
    const auto traffic = generate_traffic(prefixes, 8'000, 21);

    std::uint64_t generation = 0;
    for (const auto& prefix : prefixes) fib.insert(prefix, entry_for(++generation));

    constexpr int reader_count = 3;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> inconsistencies{0};
    std::atomic<std::uint64_t> writes{0};
    std::barrier sync(reader_count + 2);

    std::vector<std::thread> readers;
    readers.reserve(reader_count);
    for (int r = 0; r < reader_count; ++r) {
        readers.emplace_back([&, r] {
            sync.arrive_and_wait();
            std::size_t index = static_cast<std::size_t>(r) * 97;
            std::uint64_t local_reads = 0;
            std::uint64_t local_bad = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto& address = traffic[index++ % traffic.size()];
                fib.visit(address, [&](const fib_entry* found) {
                    if (found == nullptr) return;
                    // Copy before checking: the entry is only guaranteed valid
                    // inside this callback.
                    const fib_entry snapshot = *found;
                    if (!consistent(snapshot)) ++local_bad;
                });
                ++local_reads;
            }
            reads.fetch_add(local_reads, std::memory_order_relaxed);
            inconsistencies.fetch_add(local_bad, std::memory_order_relaxed);
        });
    }

    std::thread writer([&] {
        sync.arrive_and_wait();
        std::size_t index = 0;
        std::uint64_t local_writes = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const auto& prefix = prefixes[index++ % prefixes.size()];
            fib.insert(prefix, entry_for(++generation));
            ++local_writes;
        }
        writes.fetch_add(local_writes, std::memory_order_relaxed);
    });

    sync.arrive_and_wait();
    std::this_thread::sleep_for(run_time());
    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) reader.join();
    writer.join();

    EXPECT_EQ(inconsistencies.load(), 0u)
        << "a reader saw an entry whose fields did not agree with each other";
    EXPECT_GT(reads.load(), 0u);
    EXPECT_GT(writes.load(), 0u);
}

TYPED_TEST(ConcurrencyTest, StructuralChurnDoesNotCorruptTheTable) {
    // Inserts and erases reshape the trie - splitting nodes, pruning them,
    // collapsing parents - all while readers are walking it.
    TypeParam fib;
    const auto prefixes = generate_prefixes({.prefix_count = 2'000, .seed = 33});
    const auto traffic = generate_traffic(prefixes, 4'000, 33);

    std::uint64_t generation = 0;
    for (const auto& prefix : prefixes) fib.insert(prefix, entry_for(++generation));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> inconsistencies{0};
    std::atomic<std::uint64_t> reads{0};
    std::barrier sync(3);

    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&, r] {
            sync.arrive_and_wait();
            std::size_t index = static_cast<std::size_t>(r) * 31;
            std::uint64_t local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                fib.visit(traffic[index++ % traffic.size()], [&](const fib_entry* found) {
                    if (found != nullptr && !consistent(*found)) {
                        inconsistencies.fetch_add(1, std::memory_order_relaxed);
                    }
                });
                ++local;
            }
            reads.fetch_add(local, std::memory_order_relaxed);
        });
    }

    std::thread writer([&] {
        sync.arrive_and_wait();
        std::size_t index = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const auto& prefix = prefixes[index % prefixes.size()];
            if (index % 3 == 0) {
                fib.erase(prefix);
            } else {
                fib.insert(prefix, entry_for(++generation));
            }
            ++index;
        }
    });

    std::this_thread::sleep_for(run_time());
    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) reader.join();
    writer.join();

    EXPECT_EQ(inconsistencies.load(), 0u);
    EXPECT_GT(reads.load(), 0u);
}

TYPED_TEST(ConcurrencyTest, ConcurrentWritersAreSerialisedSafely) {
    // RCU makes reads lock-free, not writes. Several writers must still be able
    // to share the table without corrupting it.
    TypeParam fib;
    const auto prefixes = generate_prefixes({.prefix_count = 1'000, .seed = 44});

    constexpr int writer_count = 3;
    constexpr int per_writer = 300;
    std::barrier sync(writer_count);
    std::atomic<std::uint64_t> generation{0};

    std::vector<std::thread> writers;
    writers.reserve(writer_count);
    for (int w = 0; w < writer_count; ++w) {
        writers.emplace_back([&, w] {
            sync.arrive_and_wait();
            for (int i = 0; i < per_writer; ++i) {
                const auto& prefix = prefixes[(static_cast<std::size_t>(w) * per_writer +
                                               static_cast<std::size_t>(i)) %
                                              prefixes.size()];
                fib.insert(prefix, entry_for(generation.fetch_add(1) + 1));
            }
        });
    }
    for (auto& writer : writers) writer.join();

    // Whatever the interleaving, every surviving entry must be self-consistent.
    std::uint64_t checked = 0;
    for (const auto& prefix : prefixes) {
        if (const auto found = fib.lookup(prefix.address()); found.has_value()) {
            EXPECT_TRUE(consistent(*found)) << prefix.to_string();
            ++checked;
        }
    }
    EXPECT_GT(checked, 0u);
}

// ---------------------------------------------------------------- RCU specific

TEST(RcuConcurrency, APinnedReaderKeepsRetiredMemoryAlive) {
    // The use-after-free that EBR exists to prevent. Under AddressSanitizer,
    // a reclamation bug here is a hard failure rather than a flake.
    epoch_domain domain;
    rcu_fib<ipv4_address> fib(domain);
    const auto prefix = p4("10.0.0.0/8");
    fib.insert(prefix, entry_for(1));

    std::atomic<bool> reader_ready{false};
    std::atomic<bool> writer_done{false};
    std::atomic<std::uint64_t> observed{0};

    std::thread reader([&] {
        const epoch_domain::read_guard guard(domain);
        // Take a raw pointer *inside* the guard and keep dereferencing it while
        // the writer replaces and retires it underneath.
        const fib_entry* held = nullptr;
        fib.visit(ipv4_address::from_v4(0x0A000001), [&](const fib_entry* found) { held = found; });
        ASSERT_NE(held, nullptr);

        reader_ready.store(true, std::memory_order_release);
        while (!writer_done.load(std::memory_order_acquire)) {
            // Still inside the guard, so this must remain valid memory.
            observed.store(held->generation, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        EXPECT_TRUE(consistent(*held));
    });

    while (!reader_ready.load(std::memory_order_acquire)) std::this_thread::yield();
    for (std::uint64_t i = 2; i < 5000; ++i) {
        fib.insert(prefix, entry_for(i));
        fib.reclaim();
    }
    writer_done.store(true, std::memory_order_release);
    reader.join();

    EXPECT_GT(observed.load(), 0u);
    domain.synchronize();
}

TEST(RcuConcurrency, ALongPinnedReaderStallsReclamationButNotWriters) {
    // The documented cost of EBR: a reader that holds an epoch open delays
    // freeing for everyone. Writers must still make progress regardless.
    epoch_domain domain;
    rcu_fib<ipv4_address> fib(domain);
    const auto prefixes = generate_prefixes({.prefix_count = 500, .seed = 55});
    for (const auto& prefix : prefixes) fib.insert(prefix, entry_for(1));

    std::atomic<bool> pinned{false};
    std::atomic<bool> release{false};
    std::thread sleeper([&] {
        const epoch_domain::read_guard guard(domain);
        pinned.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!pinned.load(std::memory_order_acquire)) std::this_thread::yield();

    std::uint64_t generation = 1;
    for (int i = 0; i < 3000; ++i) {
        fib.insert(prefixes[static_cast<std::size_t>(i) % prefixes.size()],
                   entry_for(++generation));
    }

    // Writes went through; the garbage is simply still waiting.
    EXPECT_GT(domain.pending(), 0u) << "a pinned reader should have held reclamation back";
    EXPECT_GT(domain.stats().failed_advances, 0u);

    release.store(true, std::memory_order_release);
    sleeper.join();

    domain.synchronize();
    EXPECT_EQ(domain.pending(), 0u) << "the backlog must drain once the reader leaves";
}

TEST(RcuConcurrency, ReadersKeepRunningThroughAFullTableReprogram) {
    // The scenario the project is named for: the control plane replaces the
    // whole forwarding table while the dataplane keeps looking up.
    epoch_domain domain;
    rcu_fib<ipv4_address> fib(domain);
    rib<ipv4_address> source;
    fib_synchroniser<ipv4_address, rcu_fib<ipv4_address>> sync(source, fib);

    const auto prefixes = generate_prefixes({.prefix_count = 5'000, .seed = 66});
    const auto traffic = generate_traffic(prefixes, 10'000, 66);
    for (const auto& prefix : prefixes) {
        source.add(route{
            .prefix = prefix,
            .hop = next_hop{.gateway = gateway_for(1), .interface = 1, .label = 0, .metric = 0},
            .source = protocol::bgp,
            .metric = 0});
        sync.mark_dirty(prefix);
    }
    sync.flush_all();

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> misses{0};

    std::thread reader([&] {
        std::size_t index = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            fib.visit(traffic[index++ % traffic.size()], [&](const fib_entry* found) {
                if (found == nullptr) misses.fetch_add(1, std::memory_order_relaxed);
            });
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Reprogram every prefix twice over, in batches, exactly as a convergence
    // event would.
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& prefix : prefixes) {
            source.add(
                route{.prefix = prefix,
                      .hop = next_hop{.gateway = gateway_for(static_cast<std::uint64_t>(pass) + 2),
                                      .interface = 1,
                                      .label = 0,
                                      .metric = 0},
                      .source = protocol::bgp,
                      .metric = static_cast<std::uint32_t>(pass) + 1});
            sync.mark_dirty(prefix);
        }
        sync.flush_all(512);
    }

    stop.store(true, std::memory_order_relaxed);
    reader.join();

    EXPECT_GT(reads.load(), 0u);
    // A default route is always present, so the table can never be entirely
    // absent from the reader's point of view.
    EXPECT_EQ(misses.load(), 0u) << "the table went blind during reprogramming";
    domain.synchronize();
}

TEST(SeqlockConcurrency, RetriesHappenUnderChurnButReadsStillSucceed) {
    epoch_domain domain;
    seqlock_fib<ipv4_address> fib(domain);
    const auto prefixes = generate_prefixes({.prefix_count = 1'000, .seed = 77});
    for (const auto& prefix : prefixes) fib.insert(prefix, entry_for(1));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};

    std::thread reader([&] {
        std::size_t index = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            fib.visit(prefixes[index++ % prefixes.size()].address(), [](const fib_entry*) {});
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::uint64_t generation = 1;
    for (int i = 0; i < 20'000; ++i) {
        fib.insert(prefixes[static_cast<std::size_t>(i) % prefixes.size()],
                   entry_for(++generation));
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    EXPECT_GT(reads.load(), 0u) << "the reader must not livelock";
    domain.synchronize();
}
