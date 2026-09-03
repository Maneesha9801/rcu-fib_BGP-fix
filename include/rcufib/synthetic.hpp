// SPDX-License-Identifier: MIT
#pragma once

/// \file
/// A generator for route tables that look like the real thing.
///
/// Benchmarking a longest-prefix-match structure on uniformly random prefixes
/// flatters it badly. The real global table is neither uniform in length - more
/// than half of it is /24s - nor uniform in address space, because prefixes are
/// carved out of allocations and so cluster into deep, sparse subtrees. Both
/// properties change the shape of the trie, the depth of a lookup and the cache
/// behaviour of a walk, so a generator that ignores them measures the wrong
/// structure.
///
/// This produces a table with the observed length distribution and hierarchical
/// clustering, deterministically from a seed, so the numbers in the README can
/// be reproduced without downloading a 100 MB dump.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rcufib/prefix.hpp"

namespace rcufib {

struct synthetic_options {
    std::size_t prefix_count = 200'000;
    std::uint64_t seed = 0x5EED;
    /// Number of top-level allocations the prefixes are carved from. Fewer
    /// blocks means deeper, denser subtrees.
    std::size_t allocation_blocks = 400;
    bool include_default_route = true;
};

/// The share of the global IPv4 table at each mask length, rounded from a
/// RouteViews snapshot. The /24 spike and the /16 bump are the features that
/// matter; everything else is filler.
struct length_share {
    std::uint8_t length;
    double share;
};

[[nodiscard]] const std::vector<length_share>& global_table_length_distribution();

/// Generate a deduplicated prefix set. The result is sorted, so a caller can
/// rely on the order across runs.
[[nodiscard]] std::vector<ipv4_prefix> generate_prefixes(const synthetic_options& options);

/// Generate lookup keys.
///
/// \p hit_ratio controls how many addresses fall inside a generated prefix.
/// Real traffic almost always hits, and a benchmark of misses would mostly
/// measure how fast the trie can fail.
[[nodiscard]] std::vector<ipv4_address> generate_traffic(const std::vector<ipv4_prefix>& prefixes,
                                                         std::size_t count, std::uint64_t seed,
                                                         double hit_ratio = 0.99);

/// A stream of churn: which prefixes a burst of protocol activity touches.
///
/// Real churn is not spread evenly. A session flap re-advertises the same few
/// thousand prefixes repeatedly, which is what makes coalescing worth doing, so
/// the generator draws from a hot subset with a long tail rather than uniformly.
[[nodiscard]] std::vector<std::size_t> generate_churn_indices(std::size_t prefix_count,
                                                              std::size_t churn_count,
                                                              std::uint64_t seed,
                                                              double hot_fraction = 0.05);

}  // namespace rcufib
