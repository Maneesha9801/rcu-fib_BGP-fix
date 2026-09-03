// SPDX-License-Identifier: MIT
#include "rcufib/synthetic.hpp"

#include <algorithm>
#include <random>
#include <unordered_set>

namespace rcufib {
namespace {

/// Address ranges no global table carries, skipped so the generated space
/// resembles the routable one.
bool is_reserved(std::uint32_t first_octet) {
    return first_octet == 0 || first_octet == 10 || first_octet == 127 || first_octet == 100 ||
           first_octet == 169 || first_octet == 172 || first_octet == 192 || first_octet >= 224;
}

}  // namespace

const std::vector<length_share>& global_table_length_distribution() {
    // Rounded from a RouteViews full-table snapshot. The shares need not sum to
    // exactly one; the generator normalises them.
    static const std::vector<length_share> table = {
        {8, 0.002},  {9, 0.001},  {10, 0.002}, {11, 0.003}, {12, 0.005}, {13, 0.008},
        {14, 0.014}, {15, 0.017}, {16, 0.055}, {17, 0.017}, {18, 0.022}, {19, 0.038},
        {20, 0.055}, {21, 0.049}, {22, 0.098}, {23, 0.075}, {24, 0.520}, {25, 0.004},
        {26, 0.004}, {27, 0.004}, {28, 0.003}, {29, 0.002}, {30, 0.001},
    };
    return table;
}

std::vector<ipv4_prefix> generate_prefixes(const synthetic_options& options) {
    std::mt19937_64 rng(options.seed);

    const auto& distribution = global_table_length_distribution();
    std::vector<double> weights;
    weights.reserve(distribution.size());
    for (const auto& entry : distribution) weights.push_back(entry.share);
    std::discrete_distribution<std::size_t> length_picker(weights.begin(), weights.end());

    // Top-level allocations. Prefixes are carved from these rather than spread
    // uniformly, which is what gives the trie its real depth and sparsity.
    struct block {
        std::uint32_t base;
        std::uint8_t length;
    };
    std::vector<block> blocks;
    blocks.reserve(options.allocation_blocks);
    std::uniform_int_distribution<std::uint32_t> octet(1, 223);
    std::uniform_int_distribution<std::uint8_t> block_length(8, 12);
    while (blocks.size() < options.allocation_blocks) {
        const std::uint32_t first = octet(rng);
        if (is_reserved(first)) continue;
        const std::uint8_t length = block_length(rng);
        const std::uint32_t raw = (first << 24) | (static_cast<std::uint32_t>(rng()) & 0x00FFFFFFU);
        blocks.push_back({ipv4_address::from_v4(raw).masked(length).to_v4(), length});
    }

    std::unordered_set<ipv4_prefix> seen;
    std::vector<ipv4_prefix> out;
    out.reserve(options.prefix_count);
    seen.reserve(options.prefix_count * 2);

    if (options.include_default_route) {
        const ipv4_prefix def(ipv4_address{}, 0);
        seen.insert(def);
        out.push_back(def);
    }

    std::uniform_int_distribution<std::size_t> block_picker(0, blocks.size() - 1);
    // Bound the attempts so an over-subscribed block set cannot spin forever.
    const std::size_t attempt_limit = options.prefix_count * 20 + 1000;
    std::size_t attempts = 0;

    while (out.size() < options.prefix_count && attempts < attempt_limit) {
        ++attempts;
        const auto& chosen = blocks[block_picker(rng)];
        const std::uint8_t length = distribution[length_picker(rng)].length;
        if (length < chosen.length) continue;

        // Random host bits below the allocation, then mask back to `length`.
        const std::uint32_t span = length - chosen.length;
        const std::uint32_t random_bits =
            span == 0
                ? 0U
                : static_cast<std::uint32_t>(rng() & ((1ULL << span) - 1ULL)) << (32U - length);
        const ipv4_prefix candidate(ipv4_address::from_v4(chosen.base | random_bits), length);
        if (seen.insert(candidate).second) out.push_back(candidate);
    }

    std::sort(out.begin(), out.end());
    return out;
}

std::vector<ipv4_address> generate_traffic(const std::vector<ipv4_prefix>& prefixes,
                                           std::size_t count, std::uint64_t seed,
                                           double hit_ratio) {
    std::mt19937_64 rng(seed ^ 0xA11CE);
    std::vector<ipv4_address> out;
    out.reserve(count);
    if (prefixes.empty()) return out;

    std::uniform_int_distribution<std::size_t> pick(0, prefixes.size() - 1);
    std::uniform_real_distribution<double> coin(0.0, 1.0);

    for (std::size_t i = 0; i < count; ++i) {
        if (coin(rng) >= hit_ratio) {
            out.push_back(ipv4_address::from_v4(static_cast<std::uint32_t>(rng())));
            continue;
        }
        const auto& prefix = prefixes[pick(rng)];
        // A uniformly random address inside the prefix.
        const std::uint32_t host_bits = 32U - prefix.length();
        const std::uint32_t mask =
            host_bits >= 32 ? 0xFFFFFFFFU : ((1ULL << host_bits) - 1ULL) & 0xFFFFFFFFU;
        const std::uint32_t host = static_cast<std::uint32_t>(rng()) & mask;
        out.push_back(ipv4_address::from_v4(prefix.address().to_v4() | host));
    }
    return out;
}

std::vector<std::size_t> generate_churn_indices(std::size_t prefix_count, std::size_t churn_count,
                                                std::uint64_t seed, double hot_fraction) {
    std::vector<std::size_t> out;
    if (prefix_count == 0) return out;
    out.reserve(churn_count);

    std::mt19937_64 rng(seed ^ 0xC0FFEE);
    const std::size_t hot_size = std::max<std::size_t>(
        1, static_cast<std::size_t>(static_cast<double>(prefix_count) * hot_fraction));

    // The hot set is a contiguous slice chosen once: a flapping session
    // re-advertises the prefixes behind it, and those are adjacent in the table
    // because they came from the same allocations.
    std::uniform_int_distribution<std::size_t> start_pick(0, prefix_count - 1);
    const std::size_t hot_start = start_pick(rng);

    std::uniform_int_distribution<std::size_t> hot_pick(0, hot_size - 1);
    std::uniform_int_distribution<std::size_t> cold_pick(0, prefix_count - 1);
    std::uniform_real_distribution<double> coin(0.0, 1.0);

    for (std::size_t i = 0; i < churn_count; ++i) {
        // 90/10: most churn lands on the hot set, with a long tail elsewhere.
        if (coin(rng) < 0.9) {
            out.push_back((hot_start + hot_pick(rng)) % prefix_count);
        } else {
            out.push_back(cold_pick(rng));
        }
    }
    return out;
}

}  // namespace rcufib
