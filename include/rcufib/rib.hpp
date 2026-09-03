// SPDX-License-Identifier: MIT
#pragma once

/// \file
/// The RIB, and the synchroniser that programs it into a FIB.
///
/// The RIB holds every route every protocol has offered, including the ones
/// that lost. The FIB holds only the winners. Keeping them separate is not
/// bookkeeping pedantry: when an IS-IS route is withdrawn, the BGP route that
/// was sitting behind it must become the forwarding entry immediately, and
/// that is only possible if the loser was retained.
///
/// The synchroniser between them is where convergence time is won or lost. A
/// BGP session flap touches the same prefix repeatedly - added, withdrawn,
/// re-added as a different path - and programming each of those into the
/// forwarding plane in turn is wasted work that the dataplane pays for. The
/// synchroniser therefore tracks *which prefixes changed*, not *what changed*,
/// and resolves each one once at flush time against the RIB's current state.
/// Fifty updates to one prefix cost one FIB write.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rcufib/route.hpp"

namespace rcufib {

struct rib_stats {
    std::uint64_t routes_offered = 0;
    std::uint64_t routes_withdrawn = 0;
    std::uint64_t best_path_changes = 0;
    std::uint64_t prefixes = 0;
};

/// Routes for one prefix, ordered so that the winner is first.
template <class Address>
class rib_entry {
public:
    using route_type = basic_route<Address>;

    /// Insert or replace this protocol's offering. Returns true when the
    /// winning route changed as a result.
    bool offer(const route_type& route) {
        const auto previous = best();
        auto it = std::find_if(routes_.begin(), routes_.end(),
                               [&](const route_type& r) { return r.source == route.source; });
        if (it != routes_.end()) {
            *it = route;
        } else {
            routes_.push_back(route);
        }
        sort();
        return changed(previous);
    }

    /// Remove this protocol's offering. Returns true when the winner changed.
    bool withdraw(protocol source) {
        const auto previous = best();
        const auto removed = std::erase_if(
            routes_, [&](const route_type& r) { return r.source == source; });
        if (removed == 0) return false;
        return changed(previous);
    }

    [[nodiscard]] std::optional<route_type> best() const {
        if (routes_.empty()) return std::nullopt;
        return routes_.front();
    }

    [[nodiscard]] bool empty() const noexcept { return routes_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return routes_.size(); }
    [[nodiscard]] const std::vector<route_type>& routes() const noexcept { return routes_; }

private:
    void sort() {
        std::stable_sort(routes_.begin(), routes_.end(),
                         [](const route_type& a, const route_type& b) {
                             return a.preferred_over(b);
                         });
    }

    bool changed(const std::optional<route_type>& previous) const {
        const auto now = best();
        if (previous.has_value() != now.has_value()) return true;
        if (!now.has_value()) return false;
        return now->source != previous->source || !(now->hop == previous->hop);
    }

    std::vector<route_type> routes_;
};

template <class Address>
class rib {
public:
    using address_type = Address;
    using prefix_type = basic_prefix<Address>;
    using route_type = basic_route<Address>;

    /// Offer a route. Returns true when this changed the winning route for the
    /// prefix, which is the only case the FIB needs to hear about.
    bool add(const route_type& route) {
        ++stats_.routes_offered;
        auto& entry = table_[route.prefix];
        const bool changed = entry.offer(route);
        if (changed) ++stats_.best_path_changes;
        return changed;
    }

    /// Withdraw one protocol's route for a prefix.
    bool remove(const prefix_type& prefix, protocol source) {
        auto it = table_.find(prefix);
        if (it == table_.end()) return false;
        ++stats_.routes_withdrawn;
        const bool changed = it->second.withdraw(source);
        if (it->second.empty()) table_.erase(it);
        if (changed) ++stats_.best_path_changes;
        return changed;
    }

    [[nodiscard]] std::optional<route_type> best(const prefix_type& prefix) const {
        const auto it = table_.find(prefix);
        if (it == table_.end()) return std::nullopt;
        return it->second.best();
    }

    [[nodiscard]] const rib_entry<Address>* entry(const prefix_type& prefix) const {
        const auto it = table_.find(prefix);
        return it == table_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::size_t size() const noexcept { return table_.size(); }
    [[nodiscard]] bool empty() const noexcept { return table_.empty(); }

    [[nodiscard]] rib_stats stats() const noexcept {
        rib_stats out = stats_;
        out.prefixes = table_.size();
        return out;
    }

    template <class Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [prefix, entry] : table_) fn(prefix, entry);
    }

    void clear() {
        table_.clear();
        stats_ = {};
    }

private:
    std::unordered_map<prefix_type, rib_entry<Address>> table_;
    rib_stats stats_{};
};

struct sync_stats {
    std::uint64_t marked = 0;      ///< prefixes marked dirty
    std::uint64_t coalesced = 0;   ///< marks absorbed by an already-dirty prefix
    std::uint64_t flushes = 0;
    std::uint64_t updates_programmed = 0;
    std::uint64_t adds = 0;
    std::uint64_t removes = 0;

    /// Fraction of marks that never became a FIB write. This is the number the
    /// coalescing exists to produce.
    [[nodiscard]] double coalesce_ratio() const noexcept {
        const auto total = marked + coalesced;
        return total == 0 ? 0.0 : static_cast<double>(coalesced) / static_cast<double>(total);
    }
};

/// Drains RIB changes into a FIB in batches, resolving each dirty prefix once.
template <class Address, class Fib>
class fib_synchroniser {
public:
    using prefix_type = basic_prefix<Address>;
    using update_type = basic_fib_update<Address>;

    fib_synchroniser(rib<Address>& source, Fib& target) : rib_(source), fib_(target) {}

    /// Note that a prefix's best route may have changed. Cheap and idempotent:
    /// marking the same prefix a hundred times before the next flush costs one
    /// FIB write, which is the whole point.
    void mark_dirty(const prefix_type& prefix) {
        if (dirty_.insert(prefix).second) {
            ++stats_.marked;
        } else {
            ++stats_.coalesced;
        }
    }

    /// Program up to \p limit dirty prefixes. Returns how many were written.
    ///
    /// A bounded flush matters under churn: an unbounded one would hold the
    /// writer inside a single batch for as long as the burst lasts, and for the
    /// lock-based FIBs that is exactly how long readers stall.
    std::size_t flush(std::size_t limit = 0) {
        if (dirty_.empty()) return 0;
        ++stats_.flushes;

        batch_.clear();
        const std::size_t take = limit == 0 ? dirty_.size() : std::min(limit, dirty_.size());
        batch_.reserve(take);

        auto it = dirty_.begin();
        for (std::size_t i = 0; i < take && it != dirty_.end(); ++i) {
            const prefix_type prefix = *it;
            it = dirty_.erase(it);

            // Resolve once, now, against whatever the RIB currently says. Any
            // intermediate states this prefix passed through never reach the
            // forwarding plane.
            if (const auto best = rib_.best(prefix); best.has_value()) {
                batch_.push_back(update_type{
                    .prefix = prefix,
                    .entry = fib_entry{.hop = best->hop,
                                       .source = best->source,
                                       .generation = ++generation_},
                    .kind = update_kind::add});
                ++stats_.adds;
            } else {
                batch_.push_back(update_type{
                    .prefix = prefix, .entry = {}, .kind = update_kind::remove});
                ++stats_.removes;
            }
        }

        fib_.apply(batch_);
        stats_.updates_programmed += batch_.size();
        return batch_.size();
    }

    /// Flush until nothing is pending.
    std::size_t flush_all(std::size_t batch_size = 1024) {
        std::size_t total = 0;
        while (!dirty_.empty()) total += flush(batch_size);
        return total;
    }

    [[nodiscard]] std::size_t pending() const noexcept { return dirty_.size(); }
    [[nodiscard]] sync_stats stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    rib<Address>& rib_;
    Fib& fib_;
    std::unordered_set<prefix_type> dirty_;
    std::vector<update_type> batch_;
    sync_stats stats_{};
    std::uint64_t generation_ = 0;
};

}  // namespace rcufib
