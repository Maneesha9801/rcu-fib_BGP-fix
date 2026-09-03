// SPDX-License-Identifier: MIT
#include "rcufib/address.hpp"

#include <charconv>
#include <cstdio>

namespace rcufib {
namespace {

std::optional<std::uint64_t> parse_uint(std::string_view text, int base, std::uint64_t max) {
    if (text.empty() || text.size() > 8) return std::nullopt;
    std::uint64_t value = 0;
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto result = std::from_chars(first, last, value, base);
    if (result.ec != std::errc{} || result.ptr != last || value > max) return std::nullopt;
    return value;
}

std::optional<ipv4_address> parse_v4(std::string_view text) {
    std::uint32_t value = 0;
    std::size_t start = 0;
    for (int octet = 0; octet < 4; ++octet) {
        const bool last = octet == 3;
        const std::size_t dot = text.find('.', start);
        // Exactly three dots: a missing one means too few octets, an extra one
        // after the fourth means trailing junk.
        if (last != (dot == std::string_view::npos)) return std::nullopt;

        const std::string_view part =
            last ? text.substr(start) : text.substr(start, dot - start);
        // Reject leading zeros: "010" is ambiguous between decimal and octal
        // across implementations, and silently guessing is worse than refusing.
        if (part.size() > 1 && part.front() == '0') return std::nullopt;

        const auto parsed = parse_uint(part, 10, 255);
        if (!parsed) return std::nullopt;
        value = (value << 8) | static_cast<std::uint32_t>(*parsed);
        if (!last) start = dot + 1;
    }
    return ipv4_address::from_v4(value);
}

std::optional<ipv6_address> parse_v6(std::string_view text) {
    std::array<std::uint16_t, 8> groups{};
    int head = 0;                 // groups before "::"
    int tail = 0;                 // groups after "::"
    std::array<std::uint16_t, 8> tail_groups{};
    bool seen_compression = false;

    auto* target = &head;
    auto* store = &groups;

    std::size_t pos = 0;
    if (text.starts_with("::")) {
        seen_compression = true;
        pos = 2;
        if (pos == text.size()) return ipv6_address{};
        // Everything after a leading "::" is a suffix, so it must accumulate
        // into the tail. Writing it to the head would render "::1" as "1::".
        target = &tail;
        store = &tail_groups;
    }

    while (pos < text.size()) {
        if (text[pos] == ':') {
            if (seen_compression) return std::nullopt;  // only one "::" allowed
            seen_compression = true;
            target = &tail;
            store = &tail_groups;
            ++pos;
            if (pos == text.size()) break;
            continue;
        }

        const std::size_t colon = text.find(':', pos);
        std::string_view part = text.substr(
            pos, colon == std::string_view::npos ? std::string_view::npos : colon - pos);

        // A trailing dotted-quad, as in "::ffff:192.0.2.1".
        if (part.find('.') != std::string_view::npos) {
            const auto embedded = parse_v4(part);
            if (!embedded || *target > 6) return std::nullopt;
            const std::uint32_t raw = embedded->to_v4();
            (*store)[static_cast<std::size_t>((*target)++)] = static_cast<std::uint16_t>(raw >> 16);
            (*store)[static_cast<std::size_t>((*target)++)] =
                static_cast<std::uint16_t>(raw & 0xFFFF);
            pos = text.size();
            break;
        }

        const auto group = parse_uint(part, 16, 0xFFFF);
        if (!group || *target >= 8) return std::nullopt;
        (*store)[static_cast<std::size_t>((*target)++)] = static_cast<std::uint16_t>(*group);

        if (colon == std::string_view::npos) {
            pos = text.size();
            break;
        }
        pos = colon + 1;
        if (pos == text.size()) return std::nullopt;  // dangling ':'
    }

    if (!seen_compression && head != 8) return std::nullopt;
    if (head + tail > 8) return std::nullopt;

    std::array<std::uint16_t, 8> full{};
    for (int i = 0; i < head; ++i) full[static_cast<std::size_t>(i)] = groups[static_cast<std::size_t>(i)];
    for (int i = 0; i < tail; ++i)
        full[static_cast<std::size_t>(8 - tail + i)] = tail_groups[static_cast<std::size_t>(i)];

    ipv6_address::storage_type words{};
    for (std::size_t i = 0; i < 8; ++i) {
        words[i / 4] |= static_cast<std::uint64_t>(full[i]) << (48 - 16 * (i % 4));
    }
    return ipv6_address(words);
}

}  // namespace

template <>
std::string basic_address<32>::to_string() const {
    const std::uint32_t v = to_v4();
    char buffer[16];
    const int written = std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", (v >> 24) & 0xFF,
                                      (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return std::string(buffer, static_cast<std::size_t>(written));
}

template <>
std::string basic_address<128>::to_string() const {
    std::array<std::uint16_t, 8> groups{};
    for (std::size_t i = 0; i < 8; ++i) {
        groups[i] = static_cast<std::uint16_t>((words_[i / 4] >> (48 - 16 * (i % 4))) & 0xFFFF);
    }

    // RFC 5952: compress the longest run of zero groups, leftmost on a tie,
    // and only when it covers more than one group.
    int best_start = -1;
    int best_len = 0;
    int run_start = -1;
    int run_len = 0;
    for (int i = 0; i < 8; ++i) {
        if (groups[static_cast<std::size_t>(i)] == 0) {
            if (run_start < 0) run_start = i;
            ++run_len;
            if (run_len > best_len) {
                best_len = run_len;
                best_start = run_start;
            }
        } else {
            run_start = -1;
            run_len = 0;
        }
    }
    if (best_len < 2) best_start = -1;

    std::string out;
    out.reserve(40);
    for (int i = 0; i < 8; ++i) {
        if (i == best_start) {
            out += "::";
            i += best_len - 1;
            continue;
        }
        if (!out.empty() && !out.ends_with("::")) out += ':';
        char buffer[5];
        const int written =
            std::snprintf(buffer, sizeof(buffer), "%x", groups[static_cast<std::size_t>(i)]);
        out.append(buffer, static_cast<std::size_t>(written));
    }
    return out.empty() ? "::" : out;
}

template <>
std::optional<basic_address<32>> basic_address<32>::parse(std::string_view text) {
    return parse_v4(text);
}

template <>
std::optional<basic_address<128>> basic_address<128>::parse(std::string_view text) {
    return parse_v6(text);
}

template class basic_address<32>;
template class basic_address<128>;

}  // namespace rcufib
