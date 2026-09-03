// SPDX-License-Identifier: MIT
//
// A small tool for looking at the table rather than measuring it: what shape
// the trie takes, what a given address resolves to, and whether the structure
// agrees with a brute-force reference.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "rcufib/fib.hpp"
#include "rcufib/mrt.hpp"
#include "rcufib/synthetic.hpp"
#include "rcufib/trie.hpp"

using namespace rcufib;

namespace {

void usage() {
    std::printf(
        "rcufib - inspect a longest-prefix-match forwarding table\n\n"
        "  rcufib info    [--prefixes N] [--mrt PATH]          table shape and depth\n"
        "  rcufib lookup  [--prefixes N] [--mrt PATH] ADDR...  resolve addresses\n"
        "  rcufib verify  [--prefixes N]                       check against brute force\n"
        "  rcufib dump    [--prefixes N] [--limit N]           print the table\n\n"
        "Without --mrt the table is generated to match the real global table's\n"
        "length distribution and clustering, deterministically from a seed.\n");
}

struct args {
    std::size_t prefix_count = 100'000;
    std::size_t limit = 20;
    std::uint64_t seed = 0x5EED;
    std::string mrt_path;
    std::vector<std::string> positional;
};

args parse(int argc, char** argv, int from) {
    args out;
    for (int i = from; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--prefixes" && i + 1 < argc) {
            out.prefix_count = static_cast<std::size_t>(std::atoll(argv[++i]));
        } else if (arg == "--limit" && i + 1 < argc) {
            out.limit = static_cast<std::size_t>(std::atoll(argv[++i]));
        } else if (arg == "--seed" && i + 1 < argc) {
            out.seed = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg == "--mrt" && i + 1 < argc) {
            out.mrt_path = argv[++i];
        } else {
            out.positional.push_back(arg);
        }
    }
    return out;
}

/// Returns an empty vector and prints why, if the table cannot be built.
std::vector<ipv4_prefix> load_table(const args& options) {
    if (options.mrt_path.empty()) {
        return generate_prefixes({.prefix_count = options.prefix_count, .seed = options.seed});
    }
    auto loaded = load_mrt_prefixes(options.mrt_path, options.prefix_count);
    if (!loaded.ok()) {
        std::fprintf(stderr, "error: %s\n", loaded.error.c_str());
        return {};
    }
    std::printf("Read %zu records from %s (%zu skipped), %zu IPv4 prefixes\n", loaded.records_read,
                options.mrt_path.c_str(), loaded.records_skipped, loaded.prefixes.size());
    std::sort(loaded.prefixes.begin(), loaded.prefixes.end());
    return loaded.prefixes;
}

fib_entry entry_for(std::size_t index) {
    return fib_entry{.hop = next_hop{.gateway = static_cast<std::uint32_t>(0x0A000000U + index),
                                     .interface = static_cast<std::uint32_t>(index % 32),
                                     .label = 0,
                                     .metric = 0},
                     .source = protocol::bgp,
                     .generation = 1};
}

int command_info(const args& options) {
    const auto prefixes = load_table(options);
    if (prefixes.empty()) return 1;

    radix_trie<ipv4_address, fib_entry> trie;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < prefixes.size(); ++i) trie.insert(prefixes[i], entry_for(i));
    const auto build = std::chrono::steady_clock::now() - started;

    const auto depth = trie.measure_depth();

    std::printf("\nprefixes           %zu\n", trie.size());
    std::printf("trie nodes         %zu (%.2f per prefix)\n", trie.node_count(),
                static_cast<double>(trie.node_count()) / static_cast<double>(trie.size()));
    std::printf(
        "build time         %.3f s (%.0f routes/s)\n", std::chrono::duration<double>(build).count(),
        static_cast<double>(prefixes.size()) / std::chrono::duration<double>(build).count());
    std::printf("mean lookup depth  %.2f nodes\n", depth.mean_depth);
    std::printf("max lookup depth   %zu nodes\n", depth.max_depth);

    std::printf("\nmask length distribution\n");
    std::map<std::uint8_t, std::size_t> by_length;
    for (const auto& prefix : prefixes) ++by_length[prefix.length()];
    for (const auto& [length, count] : by_length) {
        const double share = static_cast<double>(count) / static_cast<double>(prefixes.size());
        std::printf("  /%-3u %8zu  %5.1f%%  ", length, count, share * 100.0);
        for (int bar = 0; bar < static_cast<int>(share * 200.0); ++bar) std::putchar('#');
        std::putchar('\n');
    }

    std::printf("\nlookup depth distribution\n");
    for (std::size_t d = 0; d < depth.histogram.size(); ++d) {
        if (depth.histogram[d] == 0) continue;
        const double share =
            static_cast<double>(depth.histogram[d]) / static_cast<double>(trie.size());
        std::printf("  %3zu %8zu  %5.1f%%  ", d, depth.histogram[d], share * 100.0);
        for (int bar = 0; bar < static_cast<int>(share * 200.0); ++bar) std::putchar('#');
        std::putchar('\n');
    }
    return 0;
}

int command_lookup(const args& options) {
    if (options.positional.empty()) {
        std::fprintf(stderr, "error: give at least one address to look up\n");
        return 1;
    }
    const auto prefixes = load_table(options);
    if (prefixes.empty()) return 1;

    rcu_fib<ipv4_address> fib;
    std::vector<basic_fib_update<ipv4_address>> batch;
    batch.reserve(prefixes.size());
    for (std::size_t i = 0; i < prefixes.size(); ++i) {
        batch.push_back({prefixes[i], entry_for(i), update_kind::add});
    }
    fib.apply(batch);

    std::printf("\n%-18s %-20s %-14s %-10s\n", "address", "matched prefix", "next-hop", "protocol");
    for (int i = 0; i < 66; ++i) std::putchar('-');
    std::putchar('\n');

    int failures = 0;
    for (const auto& text : options.positional) {
        const auto address = ipv4_address::parse(text);
        if (!address) {
            std::printf("%-18s %s\n", text.c_str(), "(not a valid IPv4 address)");
            ++failures;
            continue;
        }
        const auto found = fib.lookup(*address);
        if (!found) {
            std::printf("%-18s %-20s\n", text.c_str(), "no route");
            continue;
        }
        // Recover which prefix matched by finding the longest one containing it.
        ipv4_prefix matched;
        for (const auto& prefix : prefixes) {
            if (prefix.contains(*address) && prefix.length() >= matched.length()) matched = prefix;
        }
        const auto gateway = ipv4_address::from_v4(found->hop.gateway);
        std::printf("%-18s %-20s %-14s %-10s\n", text.c_str(), matched.to_string().c_str(),
                    gateway.to_string().c_str(), to_string(found->source));
    }
    return failures == 0 ? 0 : 1;
}

int command_verify(const args& options) {
    const auto prefixes = load_table(options);
    if (prefixes.empty()) return 1;

    radix_trie<ipv4_address, std::size_t> trie;
    std::map<ipv4_prefix, std::size_t> reference;
    for (std::size_t i = 0; i < prefixes.size(); ++i) {
        trie.insert(prefixes[i], i);
        reference[prefixes[i]] = i;
    }

    const auto traffic = generate_traffic(prefixes, 50'000, options.seed);
    std::size_t mismatches = 0;
    for (const auto& address : traffic) {
        // Brute force: the longest prefix in the reference that contains it.
        const std::size_t* got = trie.lookup(address);
        long long want = -1;
        int best_length = -1;
        for (const auto& [prefix, index] : reference) {
            if (prefix.contains(address) && static_cast<int>(prefix.length()) > best_length) {
                best_length = prefix.length();
                want = static_cast<long long>(index);
            }
        }
        const long long have = got == nullptr ? -1 : static_cast<long long>(*got);
        if (have != want) {
            if (mismatches < 5) {
                std::printf("mismatch at %s: trie=%lld brute-force=%lld\n",
                            address.to_string().c_str(), have, want);
            }
            ++mismatches;
        }
    }

    std::printf("\nchecked %zu lookups against %zu prefixes: %zu mismatches\n", traffic.size(),
                prefixes.size(), mismatches);
    return mismatches == 0 ? 0 : 1;
}

int command_dump(const args& options) {
    const auto prefixes = load_table(options);
    if (prefixes.empty()) return 1;

    radix_trie<ipv4_address, fib_entry> trie;
    for (std::size_t i = 0; i < prefixes.size(); ++i) trie.insert(prefixes[i], entry_for(i));

    std::size_t printed = 0;
    trie.for_each([&](const ipv4_prefix& prefix, const fib_entry& entry) {
        if (printed++ >= options.limit) return;
        std::printf("%-20s via %-16s %s\n", prefix.to_string().c_str(),
                    ipv4_address::from_v4(entry.hop.gateway).to_string().c_str(),
                    to_string(entry.source));
    });
    if (trie.size() > options.limit) {
        std::printf("... %zu more (use --limit)\n", trie.size() - options.limit);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        usage();
        return 0;
    }

    const args options = parse(argc, argv, 2);
    if (command == "info") return command_info(options);
    if (command == "lookup") return command_lookup(options);
    if (command == "verify") return command_verify(options);
    if (command == "dump") return command_dump(options);

    std::fprintf(stderr, "unknown command: %s\n\n", command.c_str());
    usage();
    return 1;
}
