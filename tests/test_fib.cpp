// SPDX-License-Identifier: MIT
//
// The four FIB variants must be indistinguishable through their interface.
// That is the premise the whole benchmark rests on: if they behaved
// differently, comparing their throughput would be comparing two different
// things rather than two synchronisation strategies.

#include <gtest/gtest.h>

#include <vector>

#include "rcufib/fib.hpp"
#include "rcufib/synthetic.hpp"

using namespace rcufib;

namespace {

ipv4_address v4(const char* text) { return *ipv4_address::parse(text); }
ipv4_prefix p4(const char* text) { return *ipv4_prefix::parse(text); }

fib_entry make_entry(std::uint32_t gateway, protocol source = protocol::bgp) {
    return fib_entry{.hop = next_hop{.gateway = gateway, .interface = 1, .label = 0, .metric = 10},
                     .source = source,
                     .generation = gateway};
}

}  // namespace

template <class Fib>
class FibTest : public ::testing::Test {
protected:
    Fib fib;
};

using fib_types = ::testing::Types<mutex_fib<ipv4_address>, shared_mutex_fib<ipv4_address>,
                                   seqlock_fib<ipv4_address>, rcu_fib<ipv4_address>>;

class fib_names {
public:
    template <class T>
    static std::string GetName(int) {
        return std::string(T::name);
    }
};

TYPED_TEST_SUITE(FibTest, fib_types, fib_names);

TYPED_TEST(FibTest, EmptyTableMissesEverything) {
    EXPECT_FALSE(this->fib.lookup(v4("1.2.3.4")).has_value());
    EXPECT_EQ(this->fib.size(), 0u);
}

TYPED_TEST(FibTest, LongestPrefixWins) {
    this->fib.insert(p4("0.0.0.0/0"), make_entry(1));
    this->fib.insert(p4("10.0.0.0/8"), make_entry(2));
    this->fib.insert(p4("10.1.2.0/24"), make_entry(3));

    EXPECT_EQ(this->fib.lookup(v4("10.1.2.3"))->hop.gateway, 3u);
    EXPECT_EQ(this->fib.lookup(v4("10.9.9.9"))->hop.gateway, 2u);
    EXPECT_EQ(this->fib.lookup(v4("8.8.8.8"))->hop.gateway, 1u);
    EXPECT_EQ(this->fib.size(), 3u);
}

TYPED_TEST(FibTest, VisitSeesTheSameEntryAsLookup) {
    this->fib.insert(p4("10.0.0.0/8"), make_entry(42));

    std::uint32_t seen = 0;
    this->fib.visit(v4("10.1.1.1"), [&](const fib_entry* entry) {
        ASSERT_NE(entry, nullptr);
        seen = entry->hop.gateway;
    });
    EXPECT_EQ(seen, 42u);

    bool missed = false;
    this->fib.visit(v4("11.1.1.1"), [&](const fib_entry* entry) { missed = entry == nullptr; });
    EXPECT_TRUE(missed);
}

TYPED_TEST(FibTest, ReinsertingReplacesTheEntry) {
    EXPECT_TRUE(this->fib.insert(p4("10.0.0.0/8"), make_entry(1)));
    EXPECT_FALSE(this->fib.insert(p4("10.0.0.0/8"), make_entry(2)));
    EXPECT_EQ(this->fib.lookup(v4("10.0.0.1"))->hop.gateway, 2u);
    EXPECT_EQ(this->fib.size(), 1u);
}

TYPED_TEST(FibTest, EraseFallsBackToTheCoveringRoute) {
    this->fib.insert(p4("10.0.0.0/8"), make_entry(1));
    this->fib.insert(p4("10.1.0.0/16"), make_entry(2));

    EXPECT_TRUE(this->fib.erase(p4("10.1.0.0/16")));
    EXPECT_EQ(this->fib.lookup(v4("10.1.2.3"))->hop.gateway, 1u);
    EXPECT_FALSE(this->fib.erase(p4("10.1.0.0/16")));
}

TYPED_TEST(FibTest, BatchApplyMatchesIndividualCalls) {
    std::vector<basic_fib_update<ipv4_address>> batch;
    batch.push_back({p4("10.0.0.0/8"), make_entry(1), update_kind::add});
    batch.push_back({p4("192.168.0.0/16"), make_entry(2), update_kind::add});
    batch.push_back({p4("172.16.0.0/12"), make_entry(3), update_kind::add});

    EXPECT_EQ(this->fib.apply(batch), 3u);
    EXPECT_EQ(this->fib.size(), 3u);
    EXPECT_EQ(this->fib.lookup(v4("192.168.1.1"))->hop.gateway, 2u);

    std::vector<basic_fib_update<ipv4_address>> removals;
    removals.push_back({p4("10.0.0.0/8"), {}, update_kind::remove});
    EXPECT_EQ(this->fib.apply(removals), 1u);
    EXPECT_FALSE(this->fib.lookup(v4("10.0.0.1")).has_value());
}

TYPED_TEST(FibTest, HoldsAFullSyntheticTable) {
    const auto prefixes = generate_prefixes({.prefix_count = 20'000, .seed = 5});
    std::vector<basic_fib_update<ipv4_address>> batch;
    batch.reserve(prefixes.size());
    for (std::size_t i = 0; i < prefixes.size(); ++i) {
        batch.push_back({prefixes[i], make_entry(static_cast<std::uint32_t>(i)), update_kind::add});
    }
    this->fib.apply(batch);
    EXPECT_EQ(this->fib.size(), prefixes.size());

    // Every generated prefix must resolve to something: the table contains a
    // default route, so a lookup inside any prefix cannot miss.
    const auto traffic = generate_traffic(prefixes, 2000, 5);
    for (const auto& address : traffic) {
        const auto found = this->fib.lookup(address);
        EXPECT_TRUE(found.has_value()) << address.to_string();
    }
}

TYPED_TEST(FibTest, PreservesTheProtocolThatProgrammedTheEntry) {
    this->fib.insert(p4("10.0.0.0/8"), make_entry(1, protocol::isis));
    EXPECT_EQ(this->fib.lookup(v4("10.0.0.1"))->source, protocol::isis);
}

// The RCU variant carries reclamation machinery the others do not, so it gets
// a few checks of its own.

TEST(RcuFib, DeferredReclamationHappensAfterAGracePeriod) {
    epoch_domain domain;
    rcu_fib<ipv4_address> fib(domain);

    fib.insert(p4("10.0.0.0/8"), make_entry(1));
    fib.insert(p4("10.0.0.0/8"), make_entry(2));  // replaces, retiring the old value
    EXPECT_GT(domain.stats().retired, 0u);

    domain.synchronize();
    EXPECT_EQ(domain.pending(), 0u);
    EXPECT_EQ(fib.lookup(v4("10.0.0.1"))->hop.gateway, 2u);
}

TEST(RcuFib, ChurnDoesNotGrowTheBacklogWithoutBound) {
    epoch_domain domain;
    rcu_fib<ipv4_address> fib(domain);

    // With no reader holding an epoch open, the writer's own reclamation should
    // keep pace with its churn rather than accumulating indefinitely.
    for (int i = 0; i < 5000; ++i) {
        fib.insert(p4("10.0.0.0/8"), make_entry(static_cast<std::uint32_t>(i)));
    }
    EXPECT_LT(domain.pending(), 500u) << "backlog = " << domain.pending();

    domain.synchronize();
    EXPECT_EQ(domain.pending(), 0u);
}

TEST(SeqlockFib, CountsItsRetries) {
    // With no concurrent writer there is nothing to retry against, so the
    // counter must stay at zero - a retry here would mean the read path is
    // spuriously restarting.
    epoch_domain domain;
    seqlock_fib<ipv4_address> fib(domain);
    fib.insert(p4("10.0.0.0/8"), make_entry(1));
    for (int i = 0; i < 1000; ++i) (void)fib.lookup(v4("10.0.0.1"));
    EXPECT_EQ(fib.stats().retries, 0u);
}
