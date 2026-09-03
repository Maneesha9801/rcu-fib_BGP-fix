// SPDX-License-Identifier: MIT
#pragma once

/// \file
/// A path-compressed binary trie for longest-prefix match.
///
/// Path compression matters here for a reason specific to routing tables: the
/// global table is extremely sparse in the upper bits. An uncompressed binary
/// trie over 32-bit keys would spend most of its nodes on single-child chains
/// that exist only to consume bits nobody branches on. Compressing those into
/// one node cuts both the node count and, more importantly, the number of
/// dependent loads a lookup performs - which is what a lookup actually costs,
/// because every level is a cache miss waiting on the level above.
///
/// Concurrency contract: **many readers, one writer**. Readers need no lock and
/// never block. Writers must be serialised by the caller - RCU makes reads
/// lock-free, not writes. Every structural change is published by a single
/// release-store of a pointer into a slot readers load with acquire, so a
/// reader observes either the old shape or the new one and never a half-built
/// node.
///
/// Memory freed by a mutation is handed to a `Reclaimer` policy rather than
/// deleted, because whether it is safe to free immediately depends entirely on
/// how readers are synchronised - which is the variable this project exists to
/// measure.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "rcufib/epoch.hpp"
#include "rcufib/prefix.hpp"

namespace rcufib {

/// Frees immediately. Correct only when readers are excluded during mutation,
/// which is the case for the lock-based baselines.
struct immediate_reclaim {
    template <class T>
    void retire(T* pointer) const {
        delete pointer;
    }
};

/// Defers freeing until every reader that could hold a reference has finished.
class epoch_reclaim {
public:
    explicit epoch_reclaim(epoch_domain& domain) noexcept : domain_(&domain) {}

    template <class T>
    void retire(T* pointer) const {
        domain_->retire(pointer);
    }

    [[nodiscard]] epoch_domain& domain() const noexcept { return *domain_; }

private:
    epoch_domain* domain_;
};

template <class Address, class Value, class Reclaimer = immediate_reclaim>
class radix_trie {
public:
    using address_type = Address;
    using value_type = Value;
    using prefix_type = basic_prefix<Address>;
    static constexpr std::size_t max_bits = Address::bit_count;

    struct node {
        /// Immutable once published: the bits this node represents.
        Address key;
        std::uint8_t key_length = 0;
        /// Null for an internal node that exists only to branch.
        std::atomic<Value*> value{nullptr};
        std::atomic<node*> child[2]{nullptr, nullptr};

        node(Address k, std::uint8_t length) noexcept : key(k.masked(length)), key_length(length) {}

        ~node() { delete value.load(std::memory_order_relaxed); }
    };

    explicit radix_trie(Reclaimer reclaimer = Reclaimer{}) noexcept
        : reclaimer_(std::move(reclaimer)) {}

    radix_trie(const radix_trie&) = delete;
    radix_trie& operator=(const radix_trie&) = delete;

    ~radix_trie() { destroy(root_.load(std::memory_order_relaxed)); }

    // ---------------------------------------------------------------- reading

    /// Longest-prefix match. Lock-free; safe to call concurrently with a writer
    /// provided the caller holds whatever reader-side protection the Reclaimer
    /// requires.
    [[nodiscard]] const Value* lookup(const Address& address) const noexcept {
        const Value* best = nullptr;
        const node* current = root_.load(std::memory_order_acquire);
        while (current != nullptr) {
            // The node's key is the common prefix of its whole subtree. If the
            // address does not share it, nothing below can match either.
            if (!current->key.matches(address, current->key_length)) break;

            if (const Value* value = current->value.load(std::memory_order_acquire);
                value != nullptr) {
                best = value;  // deeper matches overwrite this, which is the LPM rule
            }
            if (current->key_length >= max_bits) break;
            current =
                current->child[address.bit(current->key_length)].load(std::memory_order_acquire);
        }
        return best;
    }

    /// Exact-prefix lookup, as a routing protocol would do when withdrawing.
    [[nodiscard]] const Value* find_exact(const prefix_type& prefix) const noexcept {
        const node* current = root_.load(std::memory_order_acquire);
        while (current != nullptr) {
            if (current->key_length > prefix.length()) return nullptr;
            if (!current->key.matches(prefix.address(), current->key_length)) return nullptr;
            if (current->key_length == prefix.length()) {
                return current->value.load(std::memory_order_acquire);
            }
            current = current->child[prefix.address().bit(current->key_length)].load(
                std::memory_order_acquire);
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t node_count() const noexcept {
        return nodes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// Visit every stored prefix in lexicographic order. Not concurrency-safe
    /// against a writer; intended for dumps, tests and comparisons.
    template <class Fn>
    void for_each(Fn&& fn) const {
        walk(root_.load(std::memory_order_acquire), fn);
    }

    struct depth_stats {
        std::size_t max_depth = 0;
        double mean_depth = 0.0;
        /// Count of stored prefixes reachable at each depth.
        std::vector<std::size_t> histogram;
    };

    /// How many nodes a lookup traverses to reach each stored prefix.
    ///
    /// This is the number path compression exists to reduce, and the one that
    /// predicts lookup cost: every level is a dependent load, so depth is
    /// cache misses, not comparisons.
    [[nodiscard]] depth_stats measure_depth() const {
        depth_stats stats;
        std::size_t total = 0;
        std::size_t count = 0;
        measure(root_.load(std::memory_order_acquire), 1, stats, total, count);
        stats.mean_depth =
            count == 0 ? 0.0 : static_cast<double>(total) / static_cast<double>(count);
        return stats;
    }

    // ---------------------------------------------------------------- writing

    /// Insert or replace. Returns true if this created a new prefix rather than
    /// updating an existing one. Writers must be serialised by the caller.
    bool insert(const prefix_type& prefix, Value value) {
        return insert_boxed(prefix, new Value(std::move(value)));
    }

    /// Remove a prefix. Returns true if it was present.
    bool erase(const prefix_type& prefix) {
        // Locate the node and its parent chain. Depth is bounded by the address
        // width, so a fixed walk is enough and there is no recursion.
        std::atomic<node*>* slot = &root_;
        node* parent = nullptr;
        std::atomic<node*>* parent_slot = nullptr;

        node* current = slot->load(std::memory_order_relaxed);
        while (current != nullptr) {
            if (current->key_length > prefix.length()) return false;
            if (!current->key.matches(prefix.address(), current->key_length)) return false;
            if (current->key_length == prefix.length()) break;

            parent = current;
            parent_slot = slot;
            slot = &current->child[prefix.address().bit(current->key_length)];
            current = slot->load(std::memory_order_relaxed);
        }
        if (current == nullptr) return false;

        Value* old = current->value.exchange(nullptr, std::memory_order_acq_rel);
        if (old == nullptr) return false;  // an internal node, not a real route
        reclaimer_.retire(old);
        size_.fetch_sub(1, std::memory_order_relaxed);

        prune(current, slot, parent, parent_slot);
        return true;
    }

    /// Remove every prefix. Not concurrency-safe against readers unless the
    /// Reclaimer defers.
    void clear() {
        node* old_root = root_.exchange(nullptr, std::memory_order_acq_rel);
        retire_subtree(old_root);
        size_.store(0, std::memory_order_relaxed);
        nodes_.store(0, std::memory_order_relaxed);
    }

private:
    // Insert a pre-boxed value so insert() owns the single allocation.
    bool insert_boxed(const prefix_type& prefix, Value* boxed) {
        std::atomic<node*>* slot = &root_;

        for (;;) {
            node* current = slot->load(std::memory_order_relaxed);

            if (current == nullptr) {
                auto* fresh = new node(prefix.address(), prefix.length());
                fresh->value.store(boxed, std::memory_order_relaxed);
                // Release: everything written into `fresh` above must be
                // visible to any reader that acquires this pointer.
                slot->store(fresh, std::memory_order_release);
                nodes_.fetch_add(1, std::memory_order_relaxed);
                size_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }

            const std::size_t shared = current->key.common_prefix_length(prefix.address());
            const std::size_t usable = std::min<std::size_t>(current->key_length, prefix.length());

            if (shared < usable) {
                // The two keys diverge inside the span they share, so neither
                // contains the other. Interpose a branch node above both.
                split(slot, current, prefix, boxed, shared);
                return true;
            }

            if (current->key_length == prefix.length()) {
                // Exact node already exists: swap the value in place. One
                // atomic store, no structural change, no reader disturbed.
                Value* old = current->value.exchange(boxed, std::memory_order_acq_rel);
                if (old == nullptr) {
                    size_.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                reclaimer_.retire(old);
                return false;
            }

            if (current->key_length < prefix.length()) {
                // Descend: the new prefix lives somewhere under this node.
                slot = &current->child[prefix.address().bit(current->key_length)];
                continue;
            }

            // The new prefix is less specific than this node and contains it,
            // so it becomes this node's new parent.
            auto* fresh = new node(prefix.address(), prefix.length());
            fresh->value.store(boxed, std::memory_order_relaxed);
            fresh->child[current->key.bit(prefix.length())].store(current,
                                                                  std::memory_order_relaxed);
            slot->store(fresh, std::memory_order_release);
            nodes_.fetch_add(1, std::memory_order_relaxed);
            size_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    /// Interpose a branch node covering the bits `existing` and the new prefix
    /// agree on, with the two of them as its children.
    void split(std::atomic<node*>* slot, node* existing, const prefix_type& prefix, Value* boxed,
               std::size_t shared) {
        auto* branch = new node(prefix.address(), static_cast<std::uint8_t>(shared));
        auto* leaf = new node(prefix.address(), prefix.length());
        leaf->value.store(boxed, std::memory_order_relaxed);

        const bool existing_bit = existing->key.bit(shared);
        branch->child[existing_bit].store(existing, std::memory_order_relaxed);
        branch->child[!existing_bit].store(leaf, std::memory_order_relaxed);

        // Single release-store publishes both new nodes at once: a reader sees
        // either the old subtree rooted at `existing` or the new one rooted at
        // `branch`, never a partially linked mixture.
        slot->store(branch, std::memory_order_release);
        nodes_.fetch_add(2, std::memory_order_relaxed);
        size_.fetch_add(1, std::memory_order_relaxed);
    }

    /// After removing a value, drop nodes that no longer earn their place.
    void prune(node* current, std::atomic<node*>* slot, node* parent,
               std::atomic<node*>* parent_slot) {
        node* left = current->child[0].load(std::memory_order_relaxed);
        node* right = current->child[1].load(std::memory_order_relaxed);

        if (left != nullptr && right != nullptr) {
            // Still a necessary branch point; it just no longer holds a route.
            return;
        }

        node* survivor = (left != nullptr) ? left : right;
        // Unlink first, then retire: a reader arriving after this store cannot
        // reach `current`, and one that arrived before is covered by the
        // reclaimer's grace period.
        slot->store(survivor, std::memory_order_release);
        reclaimer_.retire(current);
        nodes_.fetch_sub(1, std::memory_order_relaxed);

        // Removing a child may have left the parent a valueless single-child
        // node, which path compression should collapse away too.
        if (parent == nullptr || parent_slot == nullptr) return;
        if (parent->value.load(std::memory_order_relaxed) != nullptr) return;

        node* parent_left = parent->child[0].load(std::memory_order_relaxed);
        node* parent_right = parent->child[1].load(std::memory_order_relaxed);
        if (parent_left != nullptr && parent_right != nullptr) return;

        node* only = (parent_left != nullptr) ? parent_left : parent_right;
        parent_slot->store(only, std::memory_order_release);
        reclaimer_.retire(parent);
        nodes_.fetch_sub(1, std::memory_order_relaxed);
    }

    template <class Fn>
    void walk(const node* current, Fn& fn) const {
        if (current == nullptr) return;
        if (const Value* value = current->value.load(std::memory_order_acquire); value != nullptr) {
            fn(prefix_type(current->key, current->key_length), *value);
        }
        walk(current->child[0].load(std::memory_order_acquire), fn);
        walk(current->child[1].load(std::memory_order_acquire), fn);
    }

    void measure(const node* current, std::size_t depth, depth_stats& stats, std::size_t& total,
                 std::size_t& count) const {
        if (current == nullptr) return;
        if (current->value.load(std::memory_order_acquire) != nullptr) {
            if (stats.histogram.size() <= depth) stats.histogram.resize(depth + 1, 0);
            ++stats.histogram[depth];
            stats.max_depth = std::max(stats.max_depth, depth);
            total += depth;
            ++count;
        }
        measure(current->child[0].load(std::memory_order_acquire), depth + 1, stats, total, count);
        measure(current->child[1].load(std::memory_order_acquire), depth + 1, stats, total, count);
    }

    void destroy(node* current) noexcept {
        if (current == nullptr) return;
        destroy(current->child[0].load(std::memory_order_relaxed));
        destroy(current->child[1].load(std::memory_order_relaxed));
        delete current;
    }

    void retire_subtree(node* current) {
        if (current == nullptr) return;
        retire_subtree(current->child[0].load(std::memory_order_relaxed));
        retire_subtree(current->child[1].load(std::memory_order_relaxed));
        // The node destructor frees its value, so retiring the node is enough.
        reclaimer_.retire(current);
    }

    std::atomic<node*> root_{nullptr};
    std::atomic<std::size_t> size_{0};
    std::atomic<std::size_t> nodes_{0};
    Reclaimer reclaimer_;
};

}  // namespace rcufib
