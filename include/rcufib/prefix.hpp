// SPDX-License-Identifier: MIT
#pragma once

/// \file
/// Network prefixes: an address plus a mask length.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "rcufib/address.hpp"

namespace rcufib {

template <class Address>
class basic_prefix {
public:
    using address_type = Address;
    static constexpr std::size_t max_length = Address::bit_count;

    constexpr basic_prefix() noexcept = default;

    /// Host bits beyond \p length are cleared on construction, so two prefixes
    /// that denote the same range always compare equal regardless of how the
    /// caller wrote them.
    constexpr basic_prefix(Address addr, std::uint8_t length) noexcept
        : address_(addr.masked(length)), length_(length) {}

    [[nodiscard]] constexpr const Address& address() const noexcept { return address_; }
    [[nodiscard]] constexpr std::uint8_t length() const noexcept { return length_; }

    /// True when \p addr falls inside this prefix.
    [[nodiscard]] constexpr bool contains(const Address& addr) const noexcept {
        return address_.common_prefix_length(addr) >= length_;
    }

    /// True when \p other is this prefix or a more specific one inside it.
    [[nodiscard]] constexpr bool covers(const basic_prefix& other) const noexcept {
        return other.length_ >= length_ && contains(other.address_);
    }

    [[nodiscard]] constexpr bool is_default() const noexcept { return length_ == 0; }
    [[nodiscard]] constexpr bool is_host() const noexcept { return length_ == max_length; }

    friend constexpr bool operator==(const basic_prefix&, const basic_prefix&) = default;

    /// Ordered by address then length, which puts a covering prefix immediately
    /// before the prefixes it covers - convenient for tests and for dumps.
    friend constexpr auto operator<=>(const basic_prefix& a, const basic_prefix& b) noexcept {
        if (const auto cmp = a.address_ <=> b.address_; cmp != 0) return cmp;
        return a.length_ <=> b.length_;
    }

    [[nodiscard]] std::string to_string() const {
        return address_.to_string() + "/" + std::to_string(length_);
    }

    /// Parse "10.0.0.0/8" or "2001:db8::/32". A bare address is treated as a
    /// host route.
    [[nodiscard]] static std::optional<basic_prefix> parse(std::string_view text) {
        const std::size_t slash = text.find('/');
        const std::string_view addr_text = text.substr(0, slash);
        const auto addr = Address::parse(addr_text);
        if (!addr) return std::nullopt;
        if (slash == std::string_view::npos) return basic_prefix(*addr, max_length);

        const std::string_view len_text = text.substr(slash + 1);
        if (len_text.empty() || len_text.size() > 3) return std::nullopt;
        unsigned length = 0;
        for (const char c : len_text) {
            if (c < '0' || c > '9') return std::nullopt;
            length = length * 10 + static_cast<unsigned>(c - '0');
        }
        if (length > max_length) return std::nullopt;
        return basic_prefix(*addr, static_cast<std::uint8_t>(length));
    }

private:
    Address address_{};
    std::uint8_t length_{0};
};

using ipv4_prefix = basic_prefix<ipv4_address>;
using ipv6_prefix = basic_prefix<ipv6_address>;

}  // namespace rcufib

template <class Address>
struct std::hash<rcufib::basic_prefix<Address>> {
    std::size_t operator()(const rcufib::basic_prefix<Address>& prefix) const noexcept {
        std::size_t seed = prefix.length();
        for (const auto word : prefix.address().words()) {
            // Boost's mix: adequate here, and the alternative is pulling in a
            // hashing dependency for a map that is not on the hot path.
            seed ^=
                static_cast<std::size_t>(word) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};
