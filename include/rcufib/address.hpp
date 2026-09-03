// SPDX-License-Identifier: MIT
#pragma once

/// \file
/// Fixed-width network addresses.
///
/// Templated on bit width so that one trie implementation serves both IPv4 and
/// IPv6. Storage is big-endian by word - word 0 holds the most significant
/// bits - because every operation a longest-prefix-match trie performs walks
/// from the most significant bit downward, and matching the layout to the
/// access pattern keeps `bit()` and `common_prefix_length()` branch-free.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rcufib {

template <std::size_t Bits>
class basic_address {
    static_assert(Bits % 64 == 0 || Bits == 32, "only 32-bit and 64-bit multiples are supported");

public:
    static constexpr std::size_t bit_count = Bits;
    static constexpr std::size_t word_count = (Bits + 63) / 64;
    using word_type = std::uint64_t;
    using storage_type = std::array<word_type, word_count>;

    constexpr basic_address() noexcept : words_{} {}
    explicit constexpr basic_address(storage_type words) noexcept : words_(words) {}

    /// Construct from a host-order 32-bit value. IPv4 only.
    static constexpr basic_address from_v4(std::uint32_t value) noexcept
        requires(Bits == 32)
    {
        return basic_address(storage_type{static_cast<word_type>(value)});
    }

    constexpr std::uint32_t to_v4() const noexcept
        requires(Bits == 32)
    {
        return static_cast<std::uint32_t>(words_[0]);
    }

    /// Bit \p index counted from the most significant bit, which is index 0.
    [[nodiscard]] constexpr bool bit(std::size_t index) const noexcept {
        const std::size_t word = index / bits_per_word();
        const std::size_t offset = index % bits_per_word();
        // Shift so that offset 0 selects the word's most significant *used* bit.
        return ((words_[word] >> (bits_per_word() - 1 - offset)) & 1U) != 0;
    }

    /// Number of leading bits this address shares with \p other, capped at Bits.
    [[nodiscard]] constexpr std::size_t common_prefix_length(
        const basic_address& other) const noexcept {
        std::size_t total = 0;
        for (std::size_t i = 0; i < word_count; ++i) {
            const word_type diff = words_[i] ^ other.words_[i];
            if (diff != 0) {
                // countl_zero over the full 64-bit word, then discount the
                // padding bits that a 32-bit address does not use.
                const std::size_t lead =
                    static_cast<std::size_t>(std::countl_zero(diff)) - padding_bits();
                return std::min(total + lead, bit_count);
            }
            total += bits_per_word();
            if (total >= bit_count) break;
        }
        return bit_count;
    }

    /// A copy with every bit at or beyond \p length cleared.
    [[nodiscard]] constexpr basic_address masked(std::size_t length) const noexcept {
        basic_address out;
        if (length >= bit_count) return *this;
        for (std::size_t i = 0; i < word_count; ++i) {
            const std::size_t consumed = i * bits_per_word();
            if (length <= consumed) break;
            const std::size_t keep = std::min(length - consumed, bits_per_word());
            if (keep == bits_per_word()) {
                out.words_[i] = words_[i];
            } else {
                const word_type mask = ~word_type{0} << (bits_per_word() - keep);
                out.words_[i] = words_[i] & mask;
            }
        }
        return out;
    }

    /// True when the two addresses agree on their first \p length bits.
    [[nodiscard]] constexpr bool matches(const basic_address& other,
                                         std::size_t length) const noexcept {
        return length == 0 || common_prefix_length(other) >= length;
    }

    [[nodiscard]] constexpr const storage_type& words() const noexcept { return words_; }

    friend constexpr bool operator==(const basic_address&, const basic_address&) = default;
    friend constexpr auto operator<=>(const basic_address& a, const basic_address& b) noexcept {
        return a.words_ <=> b.words_;
    }

    /// Dotted-quad or RFC 5952 text.
    [[nodiscard]] std::string to_string() const;
    /// Parse dotted-quad (v4) or colon-hex (v6). Returns nullopt on malformed input.
    [[nodiscard]] static std::optional<basic_address> parse(std::string_view text);

private:
    static constexpr std::size_t bits_per_word() noexcept {
        return Bits == 32 ? 32 : 64;
    }
    /// Unused high bits inside the storage word, for a 32-bit address in a
    /// 64-bit slot.
    static constexpr std::size_t padding_bits() noexcept {
        return Bits == 32 ? 32 : 0;
    }

    storage_type words_{};
};

using ipv4_address = basic_address<32>;
using ipv6_address = basic_address<128>;

// Declared before the explicit instantiations below: to_string and parse are
// specialised per width in address.cpp, and the compiler must know that before
// it is told to instantiate the class.
template <>
std::string basic_address<32>::to_string() const;
template <>
std::string basic_address<128>::to_string() const;
template <>
std::optional<basic_address<32>> basic_address<32>::parse(std::string_view text);
template <>
std::optional<basic_address<128>> basic_address<128>::parse(std::string_view text);

extern template class basic_address<32>;
extern template class basic_address<128>;

}  // namespace rcufib
