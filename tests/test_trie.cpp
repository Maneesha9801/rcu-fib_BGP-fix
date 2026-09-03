// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <map>
#include <random>

#include "rcufib/synthetic.hpp"
#include "rcufib/trie.hpp"

using namespace rcufib;

namespace {

using trie_type = radix_trie<ipv4_address, int>;

ipv4_address v4(const char* text) { return *ipv4_address::parse(text); }
ipv4_prefix p4(const char* text) { return *ipv4_prefix::parse(text); }

int looked_up(const trie_type& trie, const char* address) {
    const int* value = trie.lookup(v4(address));
    return value == nullptr ? -1 : *value;
}

/// Longest-prefix match by brute force, for differential testing.
int reference_lookup(const std::map<ipv4_prefix, int>& table, ipv4_address address) {
    int best = -1;
    int best_length = -1;
    for (const auto& [prefix, value] : table) {
        if (prefix.contains(address) && static_cast<int>(prefix.length()) > best_length) {
            best_length = prefix.length();
            best = value;
        }
    }
    return best;
}

}  // namespace

TEST(Trie, EmptyLookupReturnsNothing) {
    trie_type trie;
    EXPECT_EQ(trie.lookup(v4("1.2.3.4")), nullptr);
    EXPECT_TRUE(trie.empty());
    EXPECT_EQ(trie.node_count(), 0u);
}

TEST(Trie, LongestPrefixWins) {
    trie_type trie;
    trie.insert(p4("0.0.0.0/0"), 1);
    trie.insert(p4("10.0.0.0/8"), 2);
    trie.insert(p4("10.1.0.0/16"), 3);
    trie.insert(p4("10.1.2.0/24"), 4);

    EXPECT_EQ(looked_up(trie, "10.1.2.3"), 4);
    EXPECT_EQ(looked_up(trie, "10.1.3.1"), 3);
    EXPECT_EQ(looked_up(trie, "10.2.0.1"), 2);
    EXPECT_EQ(looked_up(trie, "8.8.8.8"), 1);
}

TEST(Trie, InsertionOrderDoesNotMatter) {
    // The same set built least- and most-specific first must behave identically.
    trie_type forward;
    trie_type reverse;
    forward.insert(p4("10.0.0.0/8"), 1);
    forward.insert(p4("10.1.0.0/16"), 2);
    forward.insert(p4("10.1.2.0/24"), 3);
    reverse.insert(p4("10.1.2.0/24"), 3);
    reverse.insert(p4("10.1.0.0/16"), 2);
    reverse.insert(p4("10.0.0.0/8"), 1);

    for (const char* address : {"10.1.2.3", "10.1.3.3", "10.9.9.9", "11.0.0.1"}) {
        EXPECT_EQ(looked_up(forward, address), looked_up(reverse, address)) << address;
    }
    EXPECT_EQ(forward.size(), reverse.size());
}

TEST(Trie, WithoutADefaultRouteAMissIsAMiss) {
    trie_type trie;
    trie.insert(p4("10.0.0.0/8"), 1);
    EXPECT_EQ(looked_up(trie, "11.0.0.1"), -1);
}

TEST(Trie, ReplacingAValueDoesNotChangeSize) {
    trie_type trie;
    EXPECT_TRUE(trie.insert(p4("10.0.0.0/8"), 1));
    EXPECT_FALSE(trie.insert(p4("10.0.0.0/8"), 2));
    EXPECT_EQ(trie.size(), 1u);
    EXPECT_EQ(looked_up(trie, "10.0.0.1"), 2);
}

TEST(Trie, HostAndDefaultRoutesAreTheExtremes) {
    trie_type trie;
    trie.insert(p4("0.0.0.0/0"), 1);
    trie.insert(p4("1.2.3.4/32"), 2);
    EXPECT_EQ(looked_up(trie, "1.2.3.4"), 2);
    EXPECT_EQ(looked_up(trie, "1.2.3.5"), 1);
}

TEST(Trie, FindExactIgnoresLessSpecificMatches) {
    trie_type trie;
    trie.insert(p4("10.0.0.0/8"), 1);
    EXPECT_NE(trie.find_exact(p4("10.0.0.0/8")), nullptr);
    // 10.1.0.0/16 is covered by the /8 but is not itself present.
    EXPECT_EQ(trie.find_exact(p4("10.1.0.0/16")), nullptr);
    EXPECT_EQ(trie.find_exact(p4("11.0.0.0/8")), nullptr);
}

TEST(Trie, EraseFallsBackToTheCoveringRoute) {
    trie_type trie;
    trie.insert(p4("10.0.0.0/8"), 1);
    trie.insert(p4("10.1.0.0/16"), 2);

    EXPECT_EQ(looked_up(trie, "10.1.2.3"), 2);
    EXPECT_TRUE(trie.erase(p4("10.1.0.0/16")));
    EXPECT_EQ(looked_up(trie, "10.1.2.3"), 1);
    EXPECT_EQ(trie.size(), 1u);
}

TEST(Trie, ErasingSomethingAbsentIsHarmless) {
    trie_type trie;
    trie.insert(p4("10.0.0.0/8"), 1);
    EXPECT_FALSE(trie.erase(p4("11.0.0.0/8")));
    // A branch node exists here but holds no route, so erasing it must fail.
    trie.insert(p4("10.1.0.0/16"), 2);
    trie.insert(p4("10.2.0.0/16"), 3);
    EXPECT_FALSE(trie.erase(p4("10.0.0.0/9")));
    EXPECT_EQ(trie.size(), 3u);
}

TEST(Trie, ErasingEverythingEmptiesTheStructure) {
    trie_type trie;
    const char* prefixes[] = {"0.0.0.0/0", "10.0.0.0/8", "10.1.0.0/16", "192.168.0.0/16"};
    for (const char* text : prefixes) trie.insert(p4(text), 1);
    for (const char* text : prefixes) EXPECT_TRUE(trie.erase(p4(text))) << text;

    EXPECT_TRUE(trie.empty());
    EXPECT_EQ(trie.node_count(), 0u) << "pruning should leave no orphan nodes behind";
    EXPECT_EQ(trie.lookup(v4("10.1.2.3")), nullptr);
}

TEST(Trie, SplittingKeepsBothSiblingsReachable) {
    // These share 10.0.0.0/13 and diverge below it, forcing a branch node.
    trie_type trie;
    trie.insert(p4("10.1.0.0/16"), 1);
    trie.insert(p4("10.2.0.0/16"), 2);
    EXPECT_EQ(looked_up(trie, "10.1.0.1"), 1);
    EXPECT_EQ(looked_up(trie, "10.2.0.1"), 2);
    EXPECT_EQ(trie.size(), 2u);
    EXPECT_EQ(trie.node_count(), 3u) << "two leaves and one branch";
}

TEST(Trie, PathCompressionKeepsNodeCountNearPrefixCount) {
    trie_type trie;
    const auto prefixes = generate_prefixes({.prefix_count = 20'000, .seed = 7});
    for (std::size_t i = 0; i < prefixes.size(); ++i) trie.insert(prefixes[i], static_cast<int>(i));

    // An uncompressed binary trie over this set would need far more than two
    // nodes per prefix; compression is what keeps lookups shallow.
    const double ratio =
        static_cast<double>(trie.node_count()) / static_cast<double>(trie.size());
    EXPECT_LT(ratio, 2.0) << "nodes per prefix = " << ratio;
}

TEST(Trie, ForEachEnumeratesEverythingInOrder) {
    trie_type trie;
    std::map<ipv4_prefix, int> expected;
    const auto prefixes = generate_prefixes({.prefix_count = 500, .seed = 11});
    for (std::size_t i = 0; i < prefixes.size(); ++i) {
        trie.insert(prefixes[i], static_cast<int>(i));
        expected[prefixes[i]] = static_cast<int>(i);
    }

    std::map<ipv4_prefix, int> seen;
    std::vector<ipv4_prefix> order;
    trie.for_each([&](const ipv4_prefix& prefix, const int& value) {
        seen[prefix] = value;
        order.push_back(prefix);
    });

    EXPECT_EQ(seen, expected);
    EXPECT_TRUE(std::is_sorted(order.begin(), order.end()));
}

TEST(Trie, ClearRemovesEverything) {
    trie_type trie;
    for (const auto& prefix : generate_prefixes({.prefix_count = 200, .seed = 3})) {
        trie.insert(prefix, 1);
    }
    trie.clear();
    EXPECT_TRUE(trie.empty());
    EXPECT_EQ(trie.node_count(), 0u);
    EXPECT_EQ(trie.lookup(v4("10.0.0.1")), nullptr);
}

TEST(Trie, MatchesBruteForceUnderRandomChurn) {
    // The differential test: thousands of interleaved inserts and erases,
    // checked against an O(n) reference after every batch.
    std::mt19937 rng(20230715);
    std::map<ipv4_prefix, int> reference;
    trie_type trie;
    int next_value = 1;

    for (int round = 0; round < 120; ++round) {
        for (int i = 0; i < 30; ++i) {
            if (rng() % 4 == 0 && !reference.empty()) {
                auto it = reference.begin();
                std::advance(it, rng() % reference.size());
                const auto victim = it->first;
                EXPECT_EQ(trie.erase(victim), reference.erase(victim) > 0);
            } else {
                const ipv4_prefix prefix(ipv4_address::from_v4(rng()),
                                         static_cast<std::uint8_t>(rng() % 33));
                const bool was_new = reference.find(prefix) == reference.end();
                EXPECT_EQ(trie.insert(prefix, next_value), was_new);
                reference[prefix] = next_value++;
            }
        }

        ASSERT_EQ(trie.size(), reference.size()) << "round " << round;
        for (int probe = 0; probe < 200; ++probe) {
            const auto address = ipv4_address::from_v4(rng());
            const int* found = trie.lookup(address);
            ASSERT_EQ(found == nullptr ? -1 : *found, reference_lookup(reference, address))
                << "round " << round << " address " << address.to_string();
        }
    }
}

TEST(Trie, WorksForIpv6) {
    radix_trie<ipv6_address, int> trie;
    trie.insert(*ipv6_prefix::parse("2001:db8::/32"), 1);
    trie.insert(*ipv6_prefix::parse("2001:db8:1::/48"), 2);
    trie.insert(*ipv6_prefix::parse("::/0"), 0);

    const int* found = trie.lookup(*ipv6_address::parse("2001:db8:1::5"));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, 2);

    found = trie.lookup(*ipv6_address::parse("2001:db8:2::5"));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, 1);

    found = trie.lookup(*ipv6_address::parse("2600::1"));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, 0);
}
