// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "rcufib/fib.hpp"
#include "rcufib/rib.hpp"
#include "rcufib/synthetic.hpp"

using namespace rcufib;

namespace {

ipv4_address v4(const char* text) { return *ipv4_address::parse(text); }
ipv4_prefix p4(const char* text) { return *ipv4_prefix::parse(text); }

route make_route(const char* prefix, protocol source, std::uint32_t gateway,
                 std::uint32_t metric = 10) {
    return route{.prefix = p4(prefix),
                 .hop = next_hop{.gateway = gateway, .interface = 1, .label = 0, .metric = 0},
                 .source = source,
                 .metric = metric};
}

}  // namespace

TEST(Rib, LowerAdministrativeDistanceWins) {
    rib<ipv4_address> table;
    EXPECT_TRUE(table.add(make_route("10.0.0.0/8", protocol::bgp, 1)));
    // IS-IS has a lower distance than BGP, so it takes over the prefix.
    EXPECT_TRUE(table.add(make_route("10.0.0.0/8", protocol::isis, 2)));

    const auto best = table.best(p4("10.0.0.0/8"));
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->source, protocol::isis);
    EXPECT_EQ(best->hop.gateway, 2u);
}

TEST(Rib, ALosingRouteIsRetainedAndTakesOverOnWithdrawal) {
    // The entire reason a RIB exists separately from a FIB.
    rib<ipv4_address> table;
    table.add(make_route("10.0.0.0/8", protocol::bgp, 1));
    table.add(make_route("10.0.0.0/8", protocol::isis, 2));
    ASSERT_EQ(table.best(p4("10.0.0.0/8"))->source, protocol::isis);

    EXPECT_TRUE(table.remove(p4("10.0.0.0/8"), protocol::isis));
    const auto best = table.best(p4("10.0.0.0/8"));
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->source, protocol::bgp) << "the BGP route should have been kept";
    EXPECT_EQ(best->hop.gateway, 1u);
}

TEST(Rib, MetricBreaksTiesWithinAProtocol) {
    rib<ipv4_address> table;
    table.add(make_route("10.0.0.0/8", protocol::isis, 1, /*metric=*/100));
    // Same protocol, so this replaces rather than competes.
    table.add(make_route("10.0.0.0/8", protocol::isis, 2, /*metric=*/10));
    EXPECT_EQ(table.best(p4("10.0.0.0/8"))->hop.gateway, 2u);
    EXPECT_EQ(table.entry(p4("10.0.0.0/8"))->size(), 1u);
}

TEST(Rib, ReofferingAnIdenticalRouteIsNotAChange) {
    rib<ipv4_address> table;
    EXPECT_TRUE(table.add(make_route("10.0.0.0/8", protocol::bgp, 1)));
    EXPECT_FALSE(table.add(make_route("10.0.0.0/8", protocol::bgp, 1)))
        << "an unchanged re-advertisement should not reprogram the FIB";
}

TEST(Rib, RemovingTheLastRouteDropsThePrefix) {
    rib<ipv4_address> table;
    table.add(make_route("10.0.0.0/8", protocol::bgp, 1));
    EXPECT_TRUE(table.remove(p4("10.0.0.0/8"), protocol::bgp));
    EXPECT_FALSE(table.best(p4("10.0.0.0/8")).has_value());
    EXPECT_EQ(table.size(), 0u);
}

TEST(Rib, WithdrawingSomethingAbsentIsHarmless) {
    rib<ipv4_address> table;
    EXPECT_FALSE(table.remove(p4("10.0.0.0/8"), protocol::bgp));
    table.add(make_route("10.0.0.0/8", protocol::bgp, 1));
    EXPECT_FALSE(table.remove(p4("10.0.0.0/8"), protocol::isis));
    EXPECT_TRUE(table.best(p4("10.0.0.0/8")).has_value());
}

// --------------------------------------------------------------- synchroniser

class SyncTest : public ::testing::Test {
protected:
    rib<ipv4_address> source;
    rcu_fib<ipv4_address> target;
    fib_synchroniser<ipv4_address, rcu_fib<ipv4_address>> sync{source, target};
};

TEST_F(SyncTest, ProgramsTheWinningRoute) {
    source.add(make_route("10.0.0.0/8", protocol::bgp, 7));
    sync.mark_dirty(p4("10.0.0.0/8"));
    EXPECT_EQ(sync.flush(), 1u);

    const auto found = target.lookup(v4("10.1.1.1"));
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->hop.gateway, 7u);
    EXPECT_EQ(found->source, protocol::bgp);
}

TEST_F(SyncTest, RepeatedMarksCollapseToOneWrite) {
    // The coalescing claim, stated as a test: a flapping prefix costs one FIB
    // write no matter how many times the control plane touched it.
    source.add(make_route("10.0.0.0/8", protocol::bgp, 1));
    for (int i = 0; i < 100; ++i) sync.mark_dirty(p4("10.0.0.0/8"));

    EXPECT_EQ(sync.pending(), 1u);
    EXPECT_EQ(sync.flush(), 1u);
    EXPECT_EQ(sync.stats().updates_programmed, 1u);
    EXPECT_EQ(sync.stats().coalesced, 99u);
    EXPECT_GT(sync.stats().coalesce_ratio(), 0.98);
}

TEST_F(SyncTest, OnlyTheFinalStateReachesTheFib) {
    // The prefix churns through several next-hops before the flush. None of the
    // intermediate ones should ever have been programmed.
    for (std::uint32_t gateway = 1; gateway <= 5; ++gateway) {
        source.add(make_route("10.0.0.0/8", protocol::bgp, gateway));
        sync.mark_dirty(p4("10.0.0.0/8"));
    }
    EXPECT_EQ(sync.flush(), 1u);
    EXPECT_EQ(target.lookup(v4("10.0.0.1"))->hop.gateway, 5u);
}

TEST_F(SyncTest, AnAddFollowedByAWithdrawalCostsNothingButARemove) {
    source.add(make_route("10.0.0.0/8", protocol::bgp, 1));
    sync.mark_dirty(p4("10.0.0.0/8"));
    source.remove(p4("10.0.0.0/8"), protocol::bgp);
    sync.mark_dirty(p4("10.0.0.0/8"));

    EXPECT_EQ(sync.flush(), 1u);
    EXPECT_FALSE(target.lookup(v4("10.0.0.1")).has_value());
    EXPECT_EQ(sync.stats().adds, 0u) << "the add was never programmed";
    EXPECT_EQ(sync.stats().removes, 1u);
}

TEST_F(SyncTest, WithdrawalRevealsTheNextBestRoute) {
    source.add(make_route("10.0.0.0/8", protocol::bgp, 1));
    source.add(make_route("10.0.0.0/8", protocol::isis, 2));
    sync.mark_dirty(p4("10.0.0.0/8"));
    sync.flush();
    ASSERT_EQ(target.lookup(v4("10.0.0.1"))->source, protocol::isis);

    source.remove(p4("10.0.0.0/8"), protocol::isis);
    sync.mark_dirty(p4("10.0.0.0/8"));
    sync.flush();

    const auto found = target.lookup(v4("10.0.0.1"));
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->source, protocol::bgp) << "must fall back, not black-hole";
}

TEST_F(SyncTest, BoundedFlushLeavesTheRemainderPending) {
    const auto prefixes = generate_prefixes({.prefix_count = 100, .seed = 9});
    for (const auto& prefix : prefixes) {
        source.add(route{.prefix = prefix, .hop = {}, .source = protocol::bgp, .metric = 0});
        sync.mark_dirty(prefix);
    }

    EXPECT_EQ(sync.flush(10), 10u);
    EXPECT_EQ(sync.pending(), prefixes.size() - 10);
    EXPECT_EQ(sync.flush_all(32), prefixes.size() - 10);
    EXPECT_EQ(sync.pending(), 0u);
    EXPECT_EQ(target.size(), prefixes.size());
}

TEST_F(SyncTest, GenerationsIncreaseSoStaleEntriesAreIdentifiable) {
    source.add(make_route("10.0.0.0/8", protocol::bgp, 1));
    sync.mark_dirty(p4("10.0.0.0/8"));
    sync.flush();
    const auto first = target.lookup(v4("10.0.0.1"))->generation;

    source.add(make_route("10.0.0.0/8", protocol::bgp, 2));
    sync.mark_dirty(p4("10.0.0.0/8"));
    sync.flush();
    EXPECT_GT(target.lookup(v4("10.0.0.1"))->generation, first);
}

TEST_F(SyncTest, FlushingNothingIsCheap) {
    EXPECT_EQ(sync.flush(), 0u);
    EXPECT_EQ(sync.stats().flushes, 0u);
}

TEST(SyncWithLockedFib, WorksIdenticallyAgainstAMutexTable) {
    // The synchroniser must not depend on which FIB is underneath it.
    rib<ipv4_address> source;
    mutex_fib<ipv4_address> target;
    fib_synchroniser<ipv4_address, mutex_fib<ipv4_address>> sync(source, target);

    source.add(make_route("10.0.0.0/8", protocol::bgp, 3));
    sync.mark_dirty(p4("10.0.0.0/8"));
    EXPECT_EQ(sync.flush(), 1u);
    EXPECT_EQ(target.lookup(v4("10.0.0.1"))->hop.gateway, 3u);
}
