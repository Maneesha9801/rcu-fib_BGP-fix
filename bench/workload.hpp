// SPDX-License-Identifier: MIT
#pragma once

/// \file
/// The measurement harness.
///
/// One workload, four tables. Every variant sees the same prefixes, the same
/// lookup keys and the same churn sequence, so a difference in the numbers is a
/// difference in synchronisation and nothing else.
///
/// Two things the harness is careful about, because getting them wrong is how
/// benchmarks come to say whatever their author hoped:
///
///  * **The reader must not be optimisable away.** Every lookup folds a field
///    of the result into a per-thread accumulator that is published at the end,
///    so the compiler cannot elide the walk.
///  * **Latency sampling must not distort throughput.** Timing every lookup
///    would cost more than the lookup does, so one in every N is timed and the
///    rest are only counted.

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rcufib/epoch.hpp"
#include "rcufib/fib.hpp"
#include "rcufib/rib.hpp"
#include "rcufib/synthetic.hpp"

namespace rcufib::bench {

using clock_type = std::chrono::steady_clock;

struct workload_config {
    std::size_t prefix_count = 200'000;
    std::size_t reader_threads = 4;
    std::chrono::milliseconds duration{3000};
    std::chrono::milliseconds warmup{300};

    /// Whether a writer runs alongside the readers.
    bool writer = true;
    /// Updates per flush. 1 means "program each change as it arrives".
    std::size_t batch_size = 64;
    /// Target update rate. 0 means as fast as the writer can go.
    std::uint64_t target_updates_per_second = 0;

    std::uint64_t seed = 0x5EED;
    /// One lookup in this many is timed.
    std::size_t latency_sample_every = 128;
    /// Cap on retained samples per reader, so a long run stays bounded.
    std::size_t max_latency_samples = 200'000;
};

struct latency_summary {
    double p50 = 0.0;
    double p90 = 0.0;
    double p99 = 0.0;
    double p999 = 0.0;
    double max = 0.0;
    std::size_t samples = 0;
};

struct workload_result {
    std::string variant;
    double duration_s = 0.0;
    std::uint64_t lookups = 0;
    std::uint64_t updates = 0;
    std::uint64_t misses = 0;
    double lookups_per_second = 0.0;
    double updates_per_second = 0.0;
    latency_summary latency;
    std::uint64_t seqlock_retries = 0;
    std::uint64_t reclaim_backlog = 0;
    std::uint64_t failed_advances = 0;

    [[nodiscard]] double mlookups_per_second() const noexcept { return lookups_per_second / 1e6; }
};

struct convergence_result {
    std::string variant;
    std::size_t routes = 0;
    double seconds = 0.0;
    double routes_per_second = 0.0;
    std::uint64_t lookups_during = 0;
    double reader_lookups_per_second = 0.0;
};

/// Prefixes, lookup keys and a churn sequence, generated once and shared.
struct dataset {
    std::vector<ipv4_prefix> prefixes;
    std::vector<ipv4_address> traffic;
    std::vector<std::size_t> churn;
};

[[nodiscard]] dataset build_dataset(const workload_config& config);

/// Replace the generated prefixes with a real table read from an MRT dump.
/// Returns an error string, empty on success.
[[nodiscard]] std::string load_real_table(dataset& data, const std::string& path,
                                          const workload_config& config);

[[nodiscard]] latency_summary summarise(std::vector<std::uint64_t>& samples);

/// Construct a FIB, passing an epoch domain to the variants that need one.
template <class Fib>
[[nodiscard]] std::unique_ptr<Fib> make_fib(epoch_domain& domain) {
    if constexpr (std::constructible_from<Fib, epoch_domain&>) {
        return std::make_unique<Fib>(domain);
    } else {
        return std::make_unique<Fib>();
    }
}

namespace detail {

/// Fill a table with one entry per prefix, before measurement starts.
template <class Fib>
void preload(Fib& fib, const dataset& data) {
    std::vector<basic_fib_update<ipv4_address>> batch;
    batch.reserve(data.prefixes.size());
    for (std::size_t i = 0; i < data.prefixes.size(); ++i) {
        batch.push_back(
            {data.prefixes[i],
             fib_entry{.hop = next_hop{.gateway = static_cast<std::uint32_t>(i),
                                       .interface = static_cast<std::uint32_t>(i & 0xFFU),
                                       .label = 0,
                                       .metric = 0},
                       .source = protocol::bgp,
                       .generation = 1},
             update_kind::add});
    }
    fib.apply(batch);
}

template <class Fib>
std::uint64_t retries_of(const Fib& fib) {
    return fib.stats().retries;
}

}  // namespace detail

/// Run readers, and optionally a writer, for the configured duration.
template <class Fib>
[[nodiscard]] workload_result run(const dataset& data, const workload_config& config) {
    epoch_domain domain;
    auto fib = make_fib<Fib>(domain);
    detail::preload(*fib, data);

    workload_result result;
    result.variant = std::string(Fib::name);

    const std::size_t reader_count = std::max<std::size_t>(1, config.reader_threads);
    const std::size_t participants = reader_count + (config.writer ? 1 : 0) + 1;

    std::atomic<bool> stop{false};
    std::atomic<bool> measuring{false};
    std::atomic<std::uint64_t> total_lookups{0};
    std::atomic<std::uint64_t> total_misses{0};
    std::atomic<std::uint64_t> total_updates{0};
    std::atomic<std::uint64_t> sink{0};
    std::barrier sync(static_cast<std::ptrdiff_t>(participants));

    std::vector<std::vector<std::uint64_t>> samples(reader_count);
    std::vector<std::thread> readers;
    readers.reserve(reader_count);

    for (std::size_t r = 0; r < reader_count; ++r) {
        readers.emplace_back([&, r] {
            auto& local_samples = samples[r];
            local_samples.reserve(std::min<std::size_t>(config.max_latency_samples, 1u << 16));

            // Each reader starts at a different offset so they are not walking
            // the same cache lines in lockstep.
            std::size_t index = r * 7919;
            std::uint64_t lookups = 0;
            std::uint64_t misses = 0;
            std::uint64_t accumulator = 0;
            std::uint64_t since_sample = 0;

            sync.arrive_and_wait();  // warmup begins
            bool counted = false;

            while (!stop.load(std::memory_order_relaxed)) {
                const bool measure_now = measuring.load(std::memory_order_relaxed);
                if (measure_now && !counted) {
                    // Discard whatever the warmup accumulated.
                    lookups = 0;
                    misses = 0;
                    local_samples.clear();
                    counted = true;
                }

                const auto& address = data.traffic[index++ % data.traffic.size()];
                const bool timed = ++since_sample >= config.latency_sample_every &&
                                   local_samples.size() < config.max_latency_samples;

                if (timed) {
                    since_sample = 0;
                    const auto started = clock_type::now();
                    fib->visit(address, [&](const fib_entry* entry) {
                        if (entry == nullptr) {
                            ++misses;
                        } else {
                            accumulator += entry->hop.gateway;
                        }
                    });
                    const auto elapsed = clock_type::now() - started;
                    if (measure_now) {
                        local_samples.push_back(static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
                    }
                } else {
                    fib->visit(address, [&](const fib_entry* entry) {
                        if (entry == nullptr) {
                            ++misses;
                        } else {
                            accumulator += entry->hop.gateway;
                        }
                    });
                }
                ++lookups;
            }

            total_lookups.fetch_add(lookups, std::memory_order_relaxed);
            total_misses.fetch_add(misses, std::memory_order_relaxed);
            // Publishing the accumulator is what stops the optimiser deciding
            // the lookups had no effect.
            sink.fetch_add(accumulator, std::memory_order_relaxed);
        });
    }

    std::thread writer;
    if (config.writer) {
        writer = std::thread([&] {
            rib<ipv4_address> source;
            fib_synchroniser<ipv4_address, Fib> sync_engine(source, *fib);
            std::uint64_t updates = 0;
            std::size_t index = 0;
            std::uint32_t revision = 2;

            const auto interval =
                config.target_updates_per_second == 0
                    ? std::chrono::nanoseconds::zero()
                    : std::chrono::nanoseconds(1'000'000'000ULL / config.target_updates_per_second);
            auto next_due = clock_type::now();

            sync.arrive_and_wait();
            bool counted = false;

            while (!stop.load(std::memory_order_relaxed)) {
                if (measuring.load(std::memory_order_relaxed) && !counted) {
                    updates = 0;
                    counted = true;
                }

                for (std::size_t i = 0; i < config.batch_size; ++i) {
                    const std::size_t which = data.churn[index++ % data.churn.size()];
                    const auto& prefix = data.prefixes[which % data.prefixes.size()];
                    source.add(route{
                        .prefix = prefix,
                        .hop =
                            next_hop{.gateway = revision, .interface = 1, .label = 0, .metric = 0},
                        .source = protocol::bgp,
                        .metric = 0});
                    sync_engine.mark_dirty(prefix);
                    ++revision;
                }
                updates += sync_engine.flush();

                if (interval.count() != 0) {
                    next_due += interval * static_cast<long>(config.batch_size);
                    std::this_thread::sleep_until(next_due);
                }
            }
            total_updates.fetch_add(updates, std::memory_order_relaxed);
        });
    }

    sync.arrive_and_wait();
    std::this_thread::sleep_for(config.warmup);
    measuring.store(true, std::memory_order_relaxed);
    const auto started = clock_type::now();
    std::this_thread::sleep_for(config.duration);
    const auto elapsed = clock_type::now() - started;
    stop.store(true, std::memory_order_relaxed);

    for (auto& reader : readers) reader.join();
    if (writer.joinable()) writer.join();

    result.duration_s = std::chrono::duration<double>(elapsed).count();
    result.lookups = total_lookups.load();
    result.misses = total_misses.load();
    result.updates = total_updates.load();
    result.lookups_per_second = static_cast<double>(result.lookups) / result.duration_s;
    result.updates_per_second = static_cast<double>(result.updates) / result.duration_s;

    std::vector<std::uint64_t> merged;
    for (auto& local : samples) merged.insert(merged.end(), local.begin(), local.end());
    result.latency = summarise(merged);

    result.seqlock_retries = detail::retries_of(*fib);
    result.reclaim_backlog = domain.pending();
    result.failed_advances = domain.stats().failed_advances;

    // Drop the table before the domain, and drain what that retires.
    fib.reset();
    domain.synchronize();
    return result;
}

/// Time a full-table reprogram, with readers running throughout.
///
/// This is the convergence measurement: not "how fast can a trie insert", but
/// "how long does the control plane take to install a table while the
/// dataplane is using it" - which is where a reader-blocking design pays.
template <class Fib>
[[nodiscard]] convergence_result measure_convergence(const dataset& data,
                                                     const workload_config& config) {
    epoch_domain domain;
    auto fib = make_fib<Fib>(domain);
    detail::preload(*fib, data);

    convergence_result result;
    result.variant = std::string(Fib::name);
    result.routes = data.prefixes.size();

    const std::size_t reader_count = config.reader_threads;
    std::atomic<bool> stop{false};
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> lookups{0};
    std::atomic<std::uint64_t> sink{0};

    std::vector<std::thread> readers;
    readers.reserve(reader_count);
    for (std::size_t r = 0; r < reader_count; ++r) {
        readers.emplace_back([&, r] {
            std::size_t index = r * 7919;
            std::uint64_t local = 0;
            std::uint64_t accumulator = 0;
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            while (!stop.load(std::memory_order_relaxed)) {
                fib->visit(data.traffic[index++ % data.traffic.size()],
                           [&](const fib_entry* entry) {
                               if (entry != nullptr) accumulator += entry->hop.gateway;
                           });
                ++local;
            }
            lookups.fetch_add(local, std::memory_order_relaxed);
            sink.fetch_add(accumulator, std::memory_order_relaxed);
        });
    }

    rib<ipv4_address> source;
    fib_synchroniser<ipv4_address, Fib> sync_engine(source, *fib);
    for (const auto& prefix : data.prefixes) {
        source.add(route{.prefix = prefix,
                         .hop = next_hop{.gateway = 99, .interface = 2, .label = 0, .metric = 0},
                         .source = protocol::isis,
                         .metric = 5});
        sync_engine.mark_dirty(prefix);
    }

    go.store(true, std::memory_order_release);
    // Let the readers actually get going before the clock starts.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto started = clock_type::now();
    sync_engine.flush_all(config.batch_size);
    const auto elapsed = clock_type::now() - started;

    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) reader.join();

    result.seconds = std::chrono::duration<double>(elapsed).count();
    result.routes_per_second = static_cast<double>(result.routes) / result.seconds;
    result.lookups_during = lookups.load();
    result.reader_lookups_per_second = static_cast<double>(result.lookups_during) / result.seconds;

    fib.reset();
    domain.synchronize();
    return result;
}

}  // namespace rcufib::bench
