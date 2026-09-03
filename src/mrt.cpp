// SPDX-License-Identifier: MIT
#include "rcufib/mrt.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_set>

#if RCUFIB_HAVE_ZLIB
#include <zlib.h>
#endif

namespace rcufib {
namespace {

constexpr std::size_t mrt_header_size = 12;
constexpr std::uint16_t type_table_dump_v2 = 13;
constexpr std::uint16_t td2_rib_ipv4_unicast = 2;
constexpr std::uint16_t td2_rib_ipv4_multicast = 3;
/// A single record larger than this is a corrupt length field, not a record.
constexpr std::uint32_t max_record_size = 16U * 1024U * 1024U;

std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

/// Minimal byte source so the caller need not care whether the dump is
/// compressed.
class reader {
public:
    virtual ~reader() = default;
    /// Fill exactly \p size bytes, or return false at end of file.
    virtual bool read_exact(std::uint8_t* out, std::size_t size) = 0;
};

class file_reader final : public reader {
public:
    explicit file_reader(std::FILE* handle) noexcept : handle_(handle) {}
    ~file_reader() override {
        if (handle_ != nullptr) std::fclose(handle_);
    }

    bool read_exact(std::uint8_t* out, std::size_t size) override {
        return std::fread(out, 1, size, handle_) == size;
    }

private:
    std::FILE* handle_;
};

#if RCUFIB_HAVE_ZLIB
class gzip_reader final : public reader {
public:
    explicit gzip_reader(gzFile handle) noexcept : handle_(handle) {}
    ~gzip_reader() override {
        if (handle_ != nullptr) gzclose(handle_);
    }

    bool read_exact(std::uint8_t* out, std::size_t size) override {
        std::size_t done = 0;
        while (done < size) {
            const int got = gzread(handle_, out + done, static_cast<unsigned>(size - done));
            if (got <= 0) return false;
            done += static_cast<std::size_t>(got);
        }
        return true;
    }

private:
    gzFile handle_;
};
#endif

bool ends_with(const std::string& text, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

std::unique_ptr<reader> open_dump(const std::string& path, std::string& error) {
    if (ends_with(path, ".gz")) {
#if RCUFIB_HAVE_ZLIB
        gzFile handle = gzopen(path.c_str(), "rb");
        if (handle == nullptr) {
            error = "cannot open gzip dump: " + path;
            return nullptr;
        }
        return std::make_unique<gzip_reader>(handle);
#else
        error = "this build has no zlib, so it cannot read " + path +
                " directly; decompress it first (gunzip -c dump.gz > dump.mrt)";
        return nullptr;
#endif
    }
    if (ends_with(path, ".bz2")) {
        error = "bzip2 dumps are not read directly; decompress first (bzcat dump.bz2 > dump.mrt)";
        return nullptr;
    }

    std::FILE* handle = std::fopen(path.c_str(), "rb");
    if (handle == nullptr) {
        error = "cannot open dump: " + path;
        return nullptr;
    }
    return std::make_unique<file_reader>(handle);
}

}  // namespace

bool mrt_supports_gzip() noexcept {
#if RCUFIB_HAVE_ZLIB
    return true;
#else
    return false;
#endif
}

mrt_result load_mrt_prefixes(const std::string& path, std::size_t limit) {
    mrt_result result;
    auto source = open_dump(path, result.error);
    if (source == nullptr) return result;

    std::array<std::uint8_t, mrt_header_size> header{};
    std::vector<std::uint8_t> body;
    std::unordered_set<ipv4_prefix> seen;

    while (source->read_exact(header.data(), header.size())) {
        const std::uint16_t type = read_be16(header.data() + 4);
        const std::uint16_t subtype = read_be16(header.data() + 6);
        const std::uint32_t length = read_be32(header.data() + 8);

        if (length > max_record_size) {
            result.error = "implausible MRT record length " + std::to_string(length) + " after " +
                           std::to_string(result.records_read) + " records";
            return result;
        }

        body.resize(length);
        if (length != 0 && !source->read_exact(body.data(), length)) break;
        ++result.records_read;

        if (type != type_table_dump_v2 ||
            (subtype != td2_rib_ipv4_unicast && subtype != td2_rib_ipv4_multicast)) {
            ++result.records_skipped;
            continue;
        }

        // RIB_IPV4_UNICAST: sequence(4) prefix-length(1) prefix(ceil(len/8))
        // then the RIB entries, which the prefix set does not need.
        if (body.size() < 5) {
            ++result.records_skipped;
            continue;
        }
        const std::uint8_t prefix_length = body[4];
        if (prefix_length > 32) {
            ++result.records_skipped;
            continue;
        }
        const std::size_t prefix_bytes = (static_cast<std::size_t>(prefix_length) + 7U) / 8U;
        if (body.size() < 5 + prefix_bytes) {
            ++result.records_skipped;
            continue;
        }

        std::uint32_t address = 0;
        for (std::size_t i = 0; i < prefix_bytes; ++i) {
            address |= static_cast<std::uint32_t>(body[5 + i]) << (24U - 8U * i);
        }

        const ipv4_prefix prefix(ipv4_address::from_v4(address), prefix_length);
        if (seen.insert(prefix).second) {
            result.prefixes.push_back(prefix);
            if (limit != 0 && result.prefixes.size() >= limit) break;
        }
    }

    if (result.prefixes.empty() && result.error.empty()) {
        result.error = "no IPv4 RIB entries found in " + path +
                       "; is it a TABLE_DUMP_V2 snapshot rather than an updates file?";
    }
    return result;
}

}  // namespace rcufib
