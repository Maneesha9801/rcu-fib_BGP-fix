// SPDX-License-Identifier: MIT
#include "rcufib/epoch.hpp"

#include <array>
#include <stdexcept>
#include <thread>

namespace rcufib {
namespace {

/// Domain slot allocation.
///
/// Each domain takes an index so a thread can find its record with an array
/// lookup rather than hashing the domain pointer - the read path runs once per
/// packet lookup and cannot afford a map. The generation counter distinguishes
/// this domain from a later one that reuses the same index, which is what
/// stops a thread exiting after its domain died from touching freed memory.
struct slot_state {
    bool taken = false;
    std::uint64_t generation = 0;
};

std::mutex& slot_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::array<slot_state, max_epoch_domains>& slots() {
    static std::array<slot_state, max_epoch_domains> table{};
    return table;
}

std::pair<std::size_t, std::uint64_t> acquire_slot() {
    const std::lock_guard lock(slot_mutex());
    auto& table = slots();
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (!table[i].taken) {
            table[i].taken = true;
            ++table[i].generation;
            return {i, table[i].generation};
        }
    }
    throw std::runtime_error("rcufib: more than max_epoch_domains live epoch domains");
}

void release_slot(std::size_t index) {
    const std::lock_guard lock(slot_mutex());
    slots()[index].taken = false;
}

/// True only if the slot still belongs to the same domain instance.
bool slot_is_live(std::size_t index, std::uint64_t generation) {
    const std::lock_guard lock(slot_mutex());
    const auto& slot = slots()[index];
    return slot.taken && slot.generation == generation;
}

}  // namespace

/// Per-thread, per-domain record cache.
///
/// The destructor returns each record to its domain's pool when the thread
/// exits. It deliberately does not free the record: a writer scanning the
/// record list may be reading it at this instant, and records are only torn
/// down with the domain itself.
struct thread_slots {
    struct entry {
        epoch_domain* domain = nullptr;
        void* record = nullptr;
        std::uint64_t generation = 0;
    };

    std::array<entry, max_epoch_domains> entries{};

    ~thread_slots() {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            auto& slot = entries[i];
            if (slot.domain == nullptr || slot.record == nullptr) continue;
            // Only touch the domain if it is still the one we registered with.
            if (!slot_is_live(i, slot.generation)) continue;
            slot.domain->release_thread_record(slot.record);
        }
    }
};

namespace {
thread_local thread_slots tls_slots;
}

epoch_domain::epoch_domain() {
    const auto [index, generation] = acquire_slot();
    id_ = index;
    generation_ = generation;
}

epoch_domain::~epoch_domain() {
    // Releasing the slot first means any thread that exits from here on sees a
    // stale generation and leaves us alone.
    release_slot(id_);

    // Nothing may be reading by now; that is the documented contract. Free
    // every record and everything still waiting in its bags.
    auto* record = records_.load(std::memory_order_acquire);
    while (record != nullptr) {
        auto* next = record->next.load(std::memory_order_relaxed);
        for (auto& bag : record->bags) {
            for (auto& object : bag) object.deleter(object.pointer);
        }
        delete record;
        record = next;
    }
    for (auto& object : orphans_) object.deleter(object.pointer);
    orphans_.clear();

    if (tls_slots.entries[id_].domain == this) {
        tls_slots.entries[id_] = {};
    }
}

void epoch_domain::release_thread_record(void* raw) {
    auto* record = static_cast<thread_record*>(raw);
    // Hand any remaining garbage to the domain: this thread will not be back
    // to free it, and dropping it would leak.
    {
        const std::lock_guard lock(orphan_mutex_);
        for (auto& bag : record->bags) {
            orphans_.insert(orphans_.end(), bag.begin(), bag.end());
            bag.clear();
        }
    }
    record->state.store(0, std::memory_order_release);
    record->in_use.store(false, std::memory_order_release);
}

epoch_domain::thread_record& epoch_domain::local_record() {
    auto& slot = tls_slots.entries[id_];
    if (slot.domain == this && slot.generation == generation_ && slot.record != nullptr) {
        return *static_cast<thread_record*>(slot.record);
    }

    // Reuse a record left behind by a thread that has exited.
    for (auto* candidate = records_.load(std::memory_order_acquire); candidate != nullptr;
         candidate = candidate->next.load(std::memory_order_relaxed)) {
        bool expected = false;
        if (candidate->in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                      std::memory_order_relaxed)) {
            candidate->nesting = 0;
            slot = {this, candidate, generation_};
            return *candidate;
        }
    }

    auto* record = new thread_record();
    auto* head = records_.load(std::memory_order_relaxed);
    do {
        record->next.store(head, std::memory_order_relaxed);
    } while (!records_.compare_exchange_weak(head, record, std::memory_order_release,
                                             std::memory_order_relaxed));

    threads_.fetch_add(1, std::memory_order_relaxed);
    slot = {this, record, generation_};
    return *record;
}

void epoch_domain::pin(thread_record& record) noexcept {
    const std::uint64_t epoch = global_epoch_.load(std::memory_order_relaxed);
    record.state.store(epoch | pinned_bit, std::memory_order_relaxed);

    // This fence is the whole correctness argument for the read side.
    //
    // The announcement above is a store; the reader's first load of a shared
    // pointer follows it. A writer does the mirror image: it stores the new
    // pointer, then loads reader announcements. Without a sequentially
    // consistent fence on both sides this is the classic store-buffer shape -
    // both the store and the load may be reordered, and the writer can conclude
    // that no reader is pinned at the moment a reader is about to load a
    // pointer the writer is about to free.
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void epoch_domain::unpin(thread_record& record) noexcept {
    // Release, so everything the reader loaded happens-before a writer
    // observing it as unpinned and freeing the memory it was reading.
    record.state.store(0, std::memory_order_release);
}

epoch_domain::read_guard::read_guard(epoch_domain& domain) noexcept : domain_(domain) {
    auto& record = domain_.local_record();
    record_ = &record;
    outermost_ = record.nesting == 0;
    if (outermost_) domain_.pin(record);
    ++record.nesting;
}

epoch_domain::read_guard::~read_guard() noexcept {
    auto& record = *static_cast<thread_record*>(record_);
    --record.nesting;
    if (outermost_) domain_.unpin(record);
}

bool epoch_domain::is_pinned() const noexcept {
    const auto& slot = tls_slots.entries[id_];
    if (slot.domain != this || slot.record == nullptr || slot.generation != generation_) {
        return false;
    }
    return static_cast<thread_record*>(slot.record)->nesting > 0;
}

void epoch_domain::retire_raw(void* pointer, void (*deleter)(void*)) {
    if (pointer == nullptr) return;
    auto& record = local_record();
    const std::uint64_t epoch = global_epoch_.load(std::memory_order_acquire);
    record.bags[epoch % epoch_ring_size].push_back({pointer, deleter});
    retired_.fetch_add(1, std::memory_order_relaxed);

    // Attempting a scan on every retire would turn a burst of withdrawals into
    // a burst of full thread-list walks, so amortise it.
    if (since_attempt_.fetch_add(1, std::memory_order_relaxed) + 1 >= reclaim_interval) {
        since_attempt_.store(0, std::memory_order_relaxed);
        try_reclaim();
    }
}

bool epoch_domain::try_advance() {
    const std::uint64_t epoch = global_epoch_.load(std::memory_order_acquire);

    // Mirror of the reader's fence: publish everything this thread did before
    // reading the announcements, so the two cannot be reordered into a state
    // where each side misses the other.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    for (auto* record = records_.load(std::memory_order_acquire); record != nullptr;
         record = record->next.load(std::memory_order_relaxed)) {
        const std::uint64_t state = record->state.load(std::memory_order_acquire);
        if ((state & pinned_bit) == 0) continue;  // not reading
        if ((state & ~pinned_bit) != epoch) {
            // A reader is still pinned to an older epoch. Advancing now would
            // let us free memory it can still reach.
            failed_advances_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    std::uint64_t expected = epoch;
    if (!global_epoch_.compare_exchange_strong(expected, epoch + 1, std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
        // Someone else advanced it, which is just as good.
        return true;
    }
    advances_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::size_t epoch_domain::free_bag(std::vector<retired_object>& bag) {
    const std::size_t count = bag.size();
    for (auto& object : bag) object.deleter(object.pointer);
    bag.clear();
    if (count != 0) reclaimed_.fetch_add(count, std::memory_order_relaxed);
    return count;
}

std::size_t epoch_domain::take_orphans() {
    std::vector<retired_object> adopted;
    {
        const std::lock_guard lock(orphan_mutex_);
        if (orphans_.empty()) return 0;
        adopted.swap(orphans_);
    }
    // Orphans came from a thread that has exited, so it is no longer pinned
    // anywhere and a grace period has necessarily already covered them.
    return free_bag(adopted);
}

std::size_t epoch_domain::try_reclaim() {
    if (!try_advance()) return 0;

    auto& record = local_record();
    const std::uint64_t epoch = global_epoch_.load(std::memory_order_acquire);

    // Having reached epoch E, everything retired during E-2 is unreachable: no
    // reader can still be pinned earlier than E-1. Index (E+1) % 3 is that bag,
    // because (E+1) and (E-2) are congruent modulo 3.
    std::size_t freed = free_bag(record.bags[(epoch + 1) % epoch_ring_size]);
    freed += take_orphans();
    return freed;
}

void epoch_domain::synchronize() {
    // One advance per epoch in the ring, plus one, guarantees every bag that
    // existed on entry has rotated out of the danger window whatever epoch we
    // started in.
    for (std::uint64_t rotation = 0; rotation < epoch_ring_size + 1; ++rotation) {
        int spins = 0;
        while (!try_advance()) {
            if (++spins > 64) {
                std::this_thread::yield();
                spins = 0;
            }
        }
    }

    auto& record = local_record();
    for (auto& bag : record.bags) free_bag(bag);
    take_orphans();
}

std::size_t epoch_domain::pending() const {
    std::size_t total = 0;
    for (auto* record = records_.load(std::memory_order_acquire); record != nullptr;
         record = record->next.load(std::memory_order_relaxed)) {
        for (const auto& bag : record->bags) total += bag.size();
    }
    const std::lock_guard lock(orphan_mutex_);
    return total + orphans_.size();
}

reclamation_stats epoch_domain::stats() const noexcept {
    return reclamation_stats{
        .retired = retired_.load(std::memory_order_relaxed),
        .reclaimed = reclaimed_.load(std::memory_order_relaxed),
        .epoch_advances = advances_.load(std::memory_order_relaxed),
        .failed_advances = failed_advances_.load(std::memory_order_relaxed),
        .threads_registered = threads_.load(std::memory_order_relaxed),
    };
}

epoch_domain& default_domain() {
    static epoch_domain domain;
    return domain;
}

}  // namespace rcufib
