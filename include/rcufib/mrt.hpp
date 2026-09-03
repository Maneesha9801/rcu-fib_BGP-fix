// SPDX-License-Identifier: MIT
#pragma once

/// \file
/// Reading real routing tables out of MRT dumps (RFC 6396).
///
/// The synthetic generator exists so the benchmark runs anywhere; this exists
/// so the numbers can be checked against the actual global table. RouteViews
/// and RIPE RIS publish full RIB snapshots every couple of hours, and a
/// `TABLE_DUMP_V2` file from either is roughly a million prefixes with the real
/// distribution and the real clustering - which is the only way to be certain
/// the synthetic table is not quietly flattering the structure.
///
/// Only what the benchmark needs is decoded: the prefix set from a RIB
/// snapshot. Path attributes are skipped rather than parsed.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rcufib/prefix.hpp"

namespace rcufib {

struct mrt_result {
    std::vector<ipv4_prefix> prefixes;
    std::size_t records_read = 0;
    std::size_t records_skipped = 0;
    /// Empty on success; otherwise what went wrong and where.
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Load the IPv4 prefixes from a `TABLE_DUMP_V2` RIB snapshot.
///
/// Accepts a plain file or, when built with zlib, a gzip-compressed one.
/// \p limit caps how many prefixes are returned; 0 means no cap.
[[nodiscard]] mrt_result load_mrt_prefixes(const std::string& path, std::size_t limit = 0);

/// True when this build can read gzip-compressed dumps directly.
[[nodiscard]] bool mrt_supports_gzip() noexcept;

}  // namespace rcufib
