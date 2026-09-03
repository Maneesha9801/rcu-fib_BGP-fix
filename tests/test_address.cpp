// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "rcufib/prefix.hpp"

using namespace rcufib;

namespace {

ipv4_address v4(const char* text) { return *ipv4_address::parse(text); }
ipv6_address v6(const char* text) { return *ipv6_address::parse(text); }

}  // namespace

TEST(Address, ParsesAndFormatsIpv4) {
    EXPECT_EQ(v4("192.0.2.1").to_string(), "192.0.2.1");
    EXPECT_EQ(v4("0.0.0.0").to_string(), "0.0.0.0");
    EXPECT_EQ(v4("255.255.255.255").to_string(), "255.255.255.255");
}

TEST(Address, RejectsMalformedIpv4) {
    for (const char* bad : {"", "1.2.3", "1.2.3.4.5", "256.0.0.1", "1.2.3.-1", "a.b.c.d",
                            "1.2.3.4 ", "010.1.1.1", "1..2.3"}) {
        EXPECT_FALSE(ipv4_address::parse(bad).has_value()) << "accepted " << bad;
    }
}

TEST(Address, ParsesAndFormatsIpv6) {
    EXPECT_EQ(v6("2001:db8::1").to_string(), "2001:db8::1");
    EXPECT_EQ(v6("::").to_string(), "::");
    EXPECT_EQ(v6("::1").to_string(), "::1");
    EXPECT_EQ(v6("fe80:0:0:0:1:2:3:4").to_string(), "fe80::1:2:3:4");
}

TEST(Address, ParsesIpv6WithEmbeddedIpv4) {
    // The low 32 bits must land where a v4-mapped address puts them.
    const auto mapped = v6("::ffff:192.0.2.1");
    EXPECT_EQ(mapped.words()[1] & 0xFFFFFFFFULL, 0xC0000201ULL);
}

TEST(Address, RejectsMalformedIpv6) {
    for (const char* bad : {"2001:db8::1::2", "2001:db8:", "12345::", "zz::1"}) {
        EXPECT_FALSE(ipv6_address::parse(bad).has_value()) << "accepted " << bad;
    }
}

TEST(Address, BitIndexesFromTheMostSignificantEnd) {
    const auto address = v4("128.0.0.1");
    EXPECT_TRUE(address.bit(0));    // the leading 1 of 128
    EXPECT_FALSE(address.bit(1));
    EXPECT_TRUE(address.bit(31));   // the trailing 1
}

TEST(Address, CommonPrefixLength) {
    EXPECT_EQ(v4("10.0.0.0").common_prefix_length(v4("10.0.0.0")), 32u);
    EXPECT_EQ(v4("10.0.0.0").common_prefix_length(v4("10.128.0.0")), 8u);
    EXPECT_EQ(v4("0.0.0.0").common_prefix_length(v4("128.0.0.0")), 0u);
    EXPECT_EQ(v4("192.168.1.0").common_prefix_length(v4("192.168.1.128")), 24u);
}

TEST(Address, CommonPrefixLengthSpansIpv6Words) {
    // Identical in the first 64 bits, differing in the second: the count must
    // continue past the word boundary rather than stopping at it.
    EXPECT_EQ(v6("2001:db8::1").common_prefix_length(v6("2001:db8::2")), 126u);
    EXPECT_EQ(v6("2001:db8::").common_prefix_length(v6("2001:db9::")), 31u);
}

TEST(Address, MaskingClearsHostBits) {
    EXPECT_EQ(v4("192.168.1.130").masked(24), v4("192.168.1.0"));
    EXPECT_EQ(v4("192.168.1.130").masked(0), v4("0.0.0.0"));
    EXPECT_EQ(v4("192.168.1.130").masked(32), v4("192.168.1.130"));
    EXPECT_EQ(v6("2001:db8:1234::1").masked(32), v6("2001:db8::"));
}

TEST(Prefix, NormalisesHostBitsOnConstruction) {
    // 10.1.2.3/8 and 10.0.0.0/8 denote the same range and must compare equal.
    EXPECT_EQ(ipv4_prefix(v4("10.1.2.3"), 8), ipv4_prefix(v4("10.0.0.0"), 8));
}

TEST(Prefix, Containment) {
    const ipv4_prefix ten_eight(v4("10.0.0.0"), 8);
    EXPECT_TRUE(ten_eight.contains(v4("10.255.255.255")));
    EXPECT_FALSE(ten_eight.contains(v4("11.0.0.1")));

    const ipv4_prefix default_route(ipv4_address{}, 0);
    EXPECT_TRUE(default_route.contains(v4("8.8.8.8")));
    EXPECT_TRUE(default_route.is_default());
}

TEST(Prefix, Covering) {
    const ipv4_prefix outer(v4("10.0.0.0"), 8);
    const ipv4_prefix inner(v4("10.1.0.0"), 16);
    EXPECT_TRUE(outer.covers(inner));
    EXPECT_FALSE(inner.covers(outer));
    EXPECT_TRUE(outer.covers(outer));
}

TEST(Prefix, ParseRoundTrips) {
    for (const char* text : {"0.0.0.0/0", "10.0.0.0/8", "192.168.1.0/24", "1.2.3.4/32"}) {
        const auto parsed = ipv4_prefix::parse(text);
        ASSERT_TRUE(parsed.has_value()) << text;
        EXPECT_EQ(parsed->to_string(), text);
    }
    EXPECT_EQ(ipv6_prefix::parse("2001:db8::/32")->to_string(), "2001:db8::/32");
}

TEST(Prefix, BareAddressIsAHostRoute) {
    const auto parsed = ipv4_prefix::parse("1.2.3.4");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->length(), 32);
    EXPECT_TRUE(parsed->is_host());
}

TEST(Prefix, RejectsOverlongMask) {
    EXPECT_FALSE(ipv4_prefix::parse("10.0.0.0/33").has_value());
    EXPECT_FALSE(ipv6_prefix::parse("2001:db8::/129").has_value());
    EXPECT_FALSE(ipv4_prefix::parse("10.0.0.0/").has_value());
}

TEST(Prefix, OrdersCoveringPrefixesFirst) {
    const ipv4_prefix outer(v4("10.0.0.0"), 8);
    const ipv4_prefix inner(v4("10.0.0.0"), 16);
    EXPECT_LT(outer, inner);
}
