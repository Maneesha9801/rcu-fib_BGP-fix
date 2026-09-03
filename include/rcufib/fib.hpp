// SPDX-License-Identifier: MIT
#pragma once

/// \file
/// Four forwarding tables that differ only in how readers and the writer stay
/// out of each other's way.
///
/// Every variant wraps the same trie and stores the same entries, so a
/// benchmark that swaps one for another is measuring the synchronisation
/// strategy and nothing else. That is the entire point: the interesting claim
/// is not "a trie is fast", it is "which of these lets a dataplane keep
/// forwarding at line rate while the control plane reprograms it".
///
/// | Variant          | Reader cost                | Reader blocks? | Reclamation |
/// |------------------|----------------------------|----------------|-------------|
/// | `mutex_fib`      | exclusive lock              | yes            | immediate   |
/// | `shared_mutex_fib` | shared lock (atomic RMW)  | yes, on writes | immediate   |
/// | `seqlock_fib`    | two loads + retry loop      | no, but retries| deferred    |
/// | `rcu_fib`        | two stores + fence          | never          | deferred    |
///
/// Note what the last column costs the seqlock: it removes the writer's
/// exclusion of readers, but a reader may still be walking a node the writer
/// unlinked, so it needs exactly the same deferred reclamation RCU does. It
/// inherits the hard part and adds retries on top of it.

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string_view>

#include "rcufib/epoch.hpp"
#include "rcufib/route.hpp"
#include "rcufib/trie.hpp"

#if defined(__aarch64__) || defined(_M_ARM64)
#define RCUFIB_SPIN_HINT() __asm__ __volatile__("yield" ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
#define RCUFIB_SPIN_HINT() __asm__ __volatile__("pause" ::: "memory")
#else
#define RCUFIB_SPIN_HINT() ((void)0)
#endif

namespace rcufib {

struct fib_stats {
    std::uint64_t lookups = 0;
    std::uint64_t updates = 0;
    std::uint64_t retries = 0;  ///< seqlock only
};

// ---------------------------------------------------------------------------
// mutex_fib: the naive baseline.
// ---------------------------------------------------------------------------

/// One lock for everything. Readers exclude each other as well as the writer,
/// which is the shape a first implementation usually has and the shape that
/// collapses first under churn.
template <class Address>
class mutex_fib {
public:
    using address_type = Address;
    using prefix_type = basic_prefix<Address>;
    using update_type = basic_fib_update<Address>;
    static constexpr std::string_view name = "mutex";

    /// Call \p fn with the matching entry, or nullptr. The entry stays valid
    /// only for the duration of the call.
    template <class Fn>
    void visit(const Address& address, Fn&& fn) const {
        const std::lock_guard lock(mutex_);
        fn(trie_.lookup(address));
    }

    [[nodiscard]] std::optional<fib_entry> lookup(const Address& address) const {
        const std::lock_guard lock(mutex_);
        const fib_entry* entry = trie_.lookup(address);
        if (entry == nullptr) return std::nullopt;
        return *entry;
    }

    bool insert(const prefix_type& prefix, const fib_entry& entry) {
        const std::lock_guard lock(mutex_);
        return trie_.insert(prefix, entry);
    }

    bool erase(const prefix_type& prefix) {
        const std::lock_guard lock(mutex_);
        return trie_.erase(prefix);
    }

    /// Apply a batch under a single lock acquisition. Batching is the first
    /// optimisation anyone reaches for, and measuring it separately keeps the
    /// comparison honest: it genuinely helps, and it still stalls readers for
    /// as long as the batch takes.
    std::size_t apply(std::span<const update_type> updates) {
        const std::lock_guard lock(mutex_);
        std::size_t applied = 0;
        for (const auto& update : updates) applied += apply_locked(update);
        return applied;
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return trie_.size();
    }

    [[nodiscard]] fib_stats stats() const noexcept { return {}; }

private:
    bool apply_locked(const update_type& update) {
        return update.kind == update_kind::add ? trie_.insert(update.prefix, update.entry)
                                               : trie_.erase(update.prefix);
    }

    mutable std::mutex mutex_;
    radix_trie<Address, fib_entry, immediate_reclaim> trie_;
};

// ---------------------------------------------------------------------------
// shared_mutex_fib: readers share, the writer still excludes them all.
// ---------------------------------------------------------------------------

/// The obvious improvement over a plain mutex, and the one that hides the
/// problem until the update rate rises. Readers no longer serialise against
/// each other, but they still contend on the lock's own cache line, and a
/// writer stalls every reader for the length of its critical section.
template <class Address>
class shared_mutex_fib {
public:
    using address_type = Address;
    using prefix_type = basic_prefix<Address>;
    using update_type = basic_fib_update<Address>;
    static constexpr std::string_view name = "shared_mutex";

    template <class Fn>
    void visit(const Address& address, Fn&& fn) const {
        const std::shared_lock lock(mutex_);
        fn(trie_.lookup(address));
    }

    [[nodiscard]] std::optional<fib_entry> lookup(const Address& address) const {
        const std::shared_lock lock(mutex_);
        const fib_entry* entry = trie_.lookup(address);
        if (entry == nullptr) return std::nullopt;
        return *entry;
    }

    bool insert(const prefix_type& prefix, const fib_entry& entry) {
        const std::unique_lock lock(mutex_);
        return trie_.insert(prefix, entry);
    }

    bool erase(const prefix_type& prefix) {
        const std::unique_lock lock(mutex_);
        return trie_.erase(prefix);
    }

    std::size_t apply(std::span<const update_type> updates) {
        const std::unique_lock lock(mutex_);
        std::size_t applied = 0;
        for (const auto& update : updates) {
            applied += update.kind == update_kind::add ? trie_.insert(update.prefix, update.entry)
                                                       : trie_.erase(update.prefix);
        }
        return applied;
    }

    [[nodiscard]] std::size_t size() const {
        const std::shared_lock lock(mutex_);
        return trie_.size();
    }

    [[nodiscard]] fib_stats stats() const noexcept { return {}; }

private:
    mutable std::shared_mutex mutex_;
    radix_trie<Address, fib_entry, immediate_reclaim> trie_;
};

// ---------------------------------------------------------------------------
// seqlock_fib: readers never block, but they do retry.
// ---------------------------------------------------------------------------

/// A sequence lock over the trie, with epoch reclamation underneath.
///
/// The reclamation is not optional. A reader mid-walk can be holding a node
/// the writer just unlinked, and no amount of sequence checking makes freeing
/// that node safe - the reader dereferences it *before* it gets to re-read the
/// sequence. So this variant carries RCU's deferred-free machinery and adds a
/// retry loop, which is why it is a useful control rather than a competitor.
template <class Address>
class seqlock_fib {
public:
    using address_type = Address;
    using prefix_type = basic_prefix<Address>;
    using update_type = basic_fib_update<Address>;
    static constexpr std::string_view name = "seqlock";

    explicit seqlock_fib(epoch_domain& domain = default_domain())
        : domain_(domain), trie_(epoch_reclaim(domain)) {}

    template <class Fn>
    void visit(const Address& address, Fn&& fn) const {
        const epoch_domain::read_guard guard(domain_);
        for (;;) {
            const std::uint64_t before = sequence_.load(std::memory_order_acquire);
            if ((before & 1U) != 0) {  // a writer is mid-update
                retries_.fetch_add(1, std::memory_order_relaxed);
                RCUFIB_SPIN_HINT();
                continue;
            }
            const fib_entry* entry = trie_.lookup(address);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == before) {
                fn(entry);
                return;
            }
            retries_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::optional<fib_entry> lookup(const Address& address) const {
        std::optional<fib_entry> out;
        visit(address, [&](const fib_entry* entry) {
            if (entry != nullptr) out = *entry;
        });
        return out;
    }

    bool insert(const prefix_type& prefix, const fib_entry& entry) {
        const std::lock_guard writer(writer_mutex_);
        const write_section section(sequence_);
        return trie_.insert(prefix, entry);
    }

    bool erase(const prefix_type& prefix) {
        const std::lock_guard writer(writer_mutex_);
        const write_section section(sequence_);
        return trie_.erase(prefix);
    }

    std::size_t apply(std::span<const update_type> updates) {
        const std::lock_guard writer(writer_mutex_);
        const write_section section(sequence_);
        std::size_t applied = 0;
        for (const auto& update : updates) {
            applied += update.kind == update_kind::add ? trie_.insert(update.prefix, update.entry)
                                                       : trie_.erase(update.prefix);
        }
        return applied;
    }

    [[nodiscard]] std::size_t size() const { return trie_.size(); }

    [[nodiscard]] fib_stats stats() const noexcept {
        return {.lookups = 0, .updates = 0, .retries = retries_.load(std::memory_order_relaxed)};
    }

private:
    /// Raises the sequence to odd for the duration of a write and back to even
    /// afterwards, so a reader that saw either value knows whether the table
    /// moved under it.
    struct write_section {
        explicit write_section(std::atomic<std::uint64_t>& sequence) noexcept : sequence_(sequence) {
            sequence_.fetch_add(1, std::memory_order_release);
            std::atomic_thread_fence(std::memory_order_release);
        }
        ~write_section() noexcept { sequence_.fetch_add(1, std::memory_order_release); }

        write_section(const write_section&) = delete;
        write_section& operator=(const write_section&) = delete;

        std::atomic<std::uint64_t>& sequence_;
    };

    epoch_domain& domain_;
    mutable std::atomic<std::uint64_t> sequence_{0};
    mutable std::atomic<std::uint64_t> retries_{0};
    std::mutex writer_mutex_;
    radix_trie<Address, fib_entry, epoch_reclaim> trie_;
};

// ---------------------------------------------------------------------------
// rcu_fib: readers never block and never retry.
// ---------------------------------------------------------------------------

/// The point of the exercise.
///
/// A lookup pins an epoch - two relaxed stores and a fence, touching only this
/// thread's own cache line - walks the trie, and unpins. There is no atomic
/// read-modify-write, nothing shared between readers to contend on, and no
/// path by which a writer can make a reader wait. Writers are serialised
/// against each other by a mutex, because RCU makes reads lock-free, not
/// writes; in a router that costs nothing, since one thread programs the FIB.
template <class Address>
class rcu_fib {
public:
    using address_type = Address;
    using prefix_type = basic_prefix<Address>;
    using update_type = basic_fib_update<Address>;
    static constexpr std::string_view name = "rcu";

    explicit rcu_fib(epoch_domain& domain = default_domain())
        : domain_(domain), trie_(epoch_reclaim(domain)) {}

    template <class Fn>
    void visit(const Address& address, Fn&& fn) const {
        const epoch_domain::read_guard guard(domain_);
        fn(trie_.lookup(address));
    }

    [[nodiscard]] std::optional<fib_entry> lookup(const Address& address) const {
        const epoch_domain::read_guard guard(domain_);
        const fib_entry* entry = trie_.lookup(address);
        if (entry == nullptr) return std::nullopt;
        return *entry;
    }

    bool insert(const prefix_type& prefix, const fib_entry& entry) {
        const std::lock_guard writer(writer_mutex_);
        return trie_.insert(prefix, entry);
    }

    bool erase(const prefix_type& prefix) {
        const std::lock_guard writer(writer_mutex_);
        return trie_.erase(prefix);
    }

    /// Batching here is bookkeeping, not contention relief: readers were never
    /// blocked in the first place. It exists so the comparison against the
    /// locked variants uses an identical call pattern.
    std::size_t apply(std::span<const update_type> updates) {
        const std::lock_guard writer(writer_mutex_);
        std::size_t applied = 0;
        for (const auto& update : updates) {
            applied += update.kind == update_kind::add ? trie_.insert(update.prefix, update.entry)
                                                       : trie_.erase(update.prefix);
        }
        return applied;
    }

    [[nodiscard]] std::size_t size() const { return trie_.size(); }
    [[nodiscard]] std::size_t node_count() const { return trie_.node_count(); }
    [[nodiscard]] epoch_domain& domain() const noexcept { return domain_; }

    /// Free whatever the last grace period made safe. A control plane would
    /// call this from its housekeeping loop.
    std::size_t reclaim() { return domain_.try_reclaim(); }

    [[nodiscard]] fib_stats stats() const noexcept { return {}; }

private:
    epoch_domain& domain_;
    std::mutex writer_mutex_;
    radix_trie<Address, fib_entry, epoch_reclaim> trie_;
};

}  // namespace rcufib
