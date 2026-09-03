// SPDX-License-Identifier: MIT
#pragma once

/// \file
/// Epoch-based reclamation (EBR).
///
/// The hard part of a lock-free data structure is not publishing a change; it
/// is knowing when the memory the change displaced can be freed. A reader that
/// loaded a node pointer a nanosecond before the writer unlinked it is still
/// walking that node, and freeing it is a use-after-free that no amount of
/// atomics on the pointer itself will prevent.
///
/// EBR answers that question with a counter. Readers announce which epoch they
/// are reading in; a writer that wants to free memory retired during epoch E
/// waits until every reader has been seen in an epoch later than E. Three
/// epochs suffice: by the time the global epoch has advanced twice, no reader
/// can still hold a reference to anything retired two epochs ago.
///
/// The cost model is what makes this the right choice for a forwarding table:
/// the read side is two relaxed stores and a fence, with no atomic
/// read-modify-write, no cache line bounced between cores, and - crucially -
/// no possibility of a reader blocking behind a writer. The price is that
/// reclamation is deferred, and a thread that pins an epoch and then stalls
/// will hold up freeing for everyone. For a dataplane whose readers are short,
/// bounded lookups, that trade is heavily in our favour.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace rcufib {

/// Maximum number of domains a single thread can participate in. One is the
/// realistic case; the rest exist so tests can use isolated domains.
inline constexpr std::size_t max_epoch_domains = 8;

/// Number of epochs kept in flight. Three is the minimum that lets a writer
/// advance the epoch while readers are still pinned to the previous one.
inline constexpr std::uint64_t epoch_ring_size = 3;

struct reclamation_stats {
    std::uint64_t retired = 0;    ///< objects handed to retire()
    std::uint64_t reclaimed = 0;  ///< objects actually freed
    std::uint64_t epoch_advances = 0;
    std::uint64_t failed_advances = 0;  ///< a reader was still pinned behind
    std::uint64_t threads_registered = 0;

    [[nodiscard]] std::uint64_t backlog() const noexcept { return retired - reclaimed; }
};

class epoch_domain {
public:
    epoch_domain();
    ~epoch_domain();

    epoch_domain(const epoch_domain&) = delete;
    epoch_domain& operator=(const epoch_domain&) = delete;

    /// Pins the calling thread to the current epoch for its lifetime.
    ///
    /// Cheap and reentrant: nesting only bumps a counter. Every read of a
    /// structure protected by this domain must happen inside one, and no
    /// pointer loaded inside one may outlive it.
    class read_guard {
    public:
        explicit read_guard(epoch_domain& domain) noexcept;
        ~read_guard() noexcept;

        read_guard(const read_guard&) = delete;
        read_guard& operator=(const read_guard&) = delete;

    private:
        epoch_domain& domain_;
        /// Cached so the destructor need not repeat the thread-local lookup.
        void* record_;
        bool outermost_;
    };

    /// Schedule \p pointer for deletion once every current reader has finished.
    template <class T>
    void retire(T* pointer) {
        if (pointer == nullptr) return;
        retire_raw(pointer, [](void* raw) { delete static_cast<T*>(raw); });
    }

    /// Schedule \p pointer for deletion with an explicit deleter.
    void retire_raw(void* pointer, void (*deleter)(void*));

    /// Try to advance the epoch and free whatever that makes safe.
    /// Returns the number of objects freed. Never blocks.
    std::size_t try_reclaim();

    /// Block until a full grace period has elapsed and everything retired
    /// before the call has been freed. Intended for shutdown and tests, not
    /// for the steady-state path.
    void synchronize();

    [[nodiscard]] std::uint64_t current_epoch() const noexcept {
        return global_epoch_.load(std::memory_order_acquire);
    }

    [[nodiscard]] reclamation_stats stats() const noexcept;

    /// True when the calling thread currently holds a read_guard. Debug aid;
    /// the lookup paths assert on it.
    [[nodiscard]] bool is_pinned() const noexcept;

    /// Objects awaiting reclamation across all threads.
    [[nodiscard]] std::size_t pending() const;

    /// Implementation detail, public only so the thread-exit hook can reach it:
    /// return a record to the pool and adopt whatever garbage it still held.
    void release_thread_record(void* record);

private:
    struct retired_object {
        void* pointer;
        void (*deleter)(void*);
    };

    /// One per participating thread. Allocated on first use, reused after the
    /// thread exits, and never freed before the domain is - a reader may be
    /// examining a record concurrently with a writer's scan.
    struct thread_record {
        /// Low bits hold the epoch; the top bit marks the thread as pinned.
        /// Packing both into one word means announcing a pin is a single store
        /// rather than two that a writer could observe half of.
        std::atomic<std::uint64_t> state{0};
        std::atomic<bool> in_use{true};
        std::atomic<thread_record*> next{nullptr};
        std::uint32_t nesting = 0;

        std::vector<retired_object> bags[epoch_ring_size];
    };

    static constexpr std::uint64_t pinned_bit = std::uint64_t{1} << 63;

    friend class read_guard;

    thread_record& local_record();
    void pin(thread_record& record) noexcept;
    void unpin(thread_record& record) noexcept;
    bool try_advance();
    std::size_t free_bag(std::vector<retired_object>& bag);
    std::size_t take_orphans();

    std::size_t id_ = 0;
    std::atomic<std::uint64_t> global_epoch_{0};
    std::atomic<thread_record*> records_{nullptr};

    mutable std::mutex orphan_mutex_;
    std::vector<retired_object> orphans_;
    /// Distinguishes this domain from a later one that reuses the same slot,
    /// so a thread exiting after the domain died cannot touch freed memory.
    std::uint64_t generation_ = 0;

    std::atomic<std::uint64_t> retired_{0};
    std::atomic<std::uint64_t> reclaimed_{0};
    std::atomic<std::uint64_t> advances_{0};
    std::atomic<std::uint64_t> failed_advances_{0};
    std::atomic<std::uint64_t> threads_{0};

    /// Retires between reclamation attempts. Trying on every retire would turn
    /// a burst of route withdrawals into a burst of full thread scans.
    static constexpr std::size_t reclaim_interval = 64;
    std::atomic<std::size_t> since_attempt_{0};
};

/// The domain a process uses unless it says otherwise.
epoch_domain& default_domain();

/// Convenience alias so call sites read as `rcu_guard guard(domain);`.
using rcu_guard = epoch_domain::read_guard;

}  // namespace rcufib
