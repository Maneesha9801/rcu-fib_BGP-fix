// SPDX-License-Identifier: MIT
#pragma once

/// \file
/// Routes, next-hops and the forwarding entries derived from them.

#include <cstdint>
#include <string>

#include "rcufib/prefix.hpp"

namespace rcufib {

/// Routing protocols, ordered so that a smaller administrative distance wins.
enum class protocol : std::uint8_t {
    connected = 0,
    local = 1,
    static_route = 2,
    isis = 3,
    ospf = 4,
    bgp = 5,
};

[[nodiscard]] constexpr std::uint8_t administrative_distance(protocol source) noexcept {
    // The conventional values; a control plane compares these before metrics.
    switch (source) {
        case protocol::connected: return 0;
        case protocol::local: return 0;
        case protocol::static_route: return 1;
        case protocol::isis: return 115;
        case protocol::ospf: return 110;
        case protocol::bgp: return 200;
    }
    return 255;
}

[[nodiscard]] constexpr const char* to_string(protocol source) noexcept {
    switch (source) {
        case protocol::connected: return "connected";
        case protocol::local: return "local";
        case protocol::static_route: return "static";
        case protocol::isis: return "isis";
        case protocol::ospf: return "ospf";
        case protocol::bgp: return "bgp";
    }
    return "unknown";
}

/// One forwarding decision.
///
/// Deliberately small and trivially copyable: it is what a lookup returns, so
/// it wants to fit alongside the trie node in cache rather than chase another
/// pointer.
struct next_hop {
    std::uint32_t gateway = 0;    ///< IPv4 next-hop, host order
    std::uint32_t interface = 0;  ///< outgoing interface index
    std::uint32_t label = 0;      ///< MPLS label, 0 when unlabelled
    std::uint16_t metric = 0;

    friend constexpr bool operator==(const next_hop&, const next_hop&) = default;
};

static_assert(sizeof(next_hop) <= 16, "next_hop should stay small enough to copy freely");

/// A forwarding table entry: the resolved next-hop plus the provenance a
/// diagnostic tool needs to explain it.
struct fib_entry {
    next_hop hop{};
    protocol source = protocol::bgp;
    /// Bumped on every reprogramming, so a test or benchmark can tell a stale
    /// entry from a current one that happens to hold equal values.
    std::uint64_t generation = 0;

    friend constexpr bool operator==(const fib_entry&, const fib_entry&) = default;
};

/// A route as a protocol offers it, before best-path selection.
template <class Address>
struct basic_route {
    basic_prefix<Address> prefix{};
    next_hop hop{};
    protocol source = protocol::bgp;
    std::uint32_t metric = 0;

    [[nodiscard]] std::uint8_t distance() const noexcept {
        return administrative_distance(source);
    }

    /// True when this route should beat \p other for the same prefix:
    /// administrative distance first, then protocol metric, exactly as a
    /// control plane decides which protocol owns a prefix.
    [[nodiscard]] bool preferred_over(const basic_route& other) const noexcept {
        if (distance() != other.distance()) return distance() < other.distance();
        return metric < other.metric;
    }
};

using route = basic_route<ipv4_address>;
using route6 = basic_route<ipv6_address>;

/// What the synchroniser asks the FIB to do.
enum class update_kind : std::uint8_t { add, remove };

template <class Address>
struct basic_fib_update {
    basic_prefix<Address> prefix{};
    fib_entry entry{};
    update_kind kind = update_kind::add;
};

using fib_update = basic_fib_update<ipv4_address>;

}  // namespace rcufib
