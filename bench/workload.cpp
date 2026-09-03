// SPDX-License-Identifier: MIT
#include "workload.hpp"

#include <algorithm>

#include "rcufib/mrt.hpp"

namespace rcufib::bench {

dataset build_dataset(const workload_config& config) {
    dataset data;
    data.prefixes = generate_prefixes({.prefix_count = config.prefix_count, .seed = config.seed});
    // Enough distinct keys that a reader is not replaying a handful of cache-hot
    // lookups, but not so many that the key array itself dominates the cache.
    data.traffic = generate_traffic(data.prefixes, 1u << 20, config.seed);
    data.churn = generate_churn_indices(data.prefixes.size(), 1u << 18, config.seed);
    return data;
}

std::string load_real_table(dataset& data, const std::string& path, const workload_config& config) {
    auto loaded = load_mrt_prefixes(path, config.prefix_count);
    if (!loaded.ok()) return loaded.error;
    if (loaded.prefixes.empty()) return "the dump contained no IPv4 prefixes";

    data.prefixes = std::move(loaded.prefixes);
    std::sort(data.prefixes.begin(), data.prefixes.end());
    data.traffic = generate_traffic(data.prefixes, 1u << 20, config.seed);
    data.churn = generate_churn_indices(data.prefixes.size(), 1u << 18, config.seed);
    return {};
}

latency_summary summarise(std::vector<std::uint64_t>& samples) {
    latency_summary summary;
    summary.samples = samples.size();
    if (samples.empty()) return summary;

    std::sort(samples.begin(), samples.end());

    const auto quantile = [&samples](double q) {
        const auto count = samples.size();
        // Nearest-rank: with a sorted sample the qth percentile is the element
        // at ceil(q*n), which needs no interpolation and cannot invent a value
        // that was never measured.
        auto index = static_cast<std::size_t>(q * static_cast<double>(count));
        if (index >= count) index = count - 1;
        return static_cast<double>(samples[index]);
    };

    summary.p50 = quantile(0.50);
    summary.p90 = quantile(0.90);
    summary.p99 = quantile(0.99);
    summary.p999 = quantile(0.999);
    summary.max = static_cast<double>(samples.back());
    return summary;
}

}  // namespace rcufib::bench
