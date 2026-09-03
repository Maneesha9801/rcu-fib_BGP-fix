// SPDX-License-Identifier: MIT
//
// The benchmark driver.
//
// It answers three questions, in the order an engineer asks them:
//
//   1. How fast does each table look up when nothing is changing?
//   2. How much of that survives while the control plane reprograms it?
//   3. How long does a full table reprogram take with the dataplane running?
//
// The second is the one that matters. Every design here looks fine on question
// one.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "workload.hpp"

using namespace rcufib;
using namespace rcufib::bench;

namespace {

struct options {
    workload_config config{};
    std::string mrt_path;
    bool rate_sweep = false;
    bool csv = false;
};

void print_usage() {
    std::printf(
        "rcufib_bench - measure what synchronisation costs a forwarding table\n\n"
        "  --prefixes N     routes in the table            (default 200000)\n"
        "  --readers N      reader threads                 (default 4)\n"
        "  --duration MS    measured window per scenario   (default 3000)\n"
        "  --batch N        updates per FIB flush          (default 64)\n"
        "  --mrt PATH       load a real table from an MRT dump instead\n"
        "  --sweep          also sweep the update rate\n"
        "  --csv            emit CSV instead of tables\n"
        "  --help\n");
}

bool parse_args(int argc, char** argv, options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](long long fallback) -> long long {
            return i + 1 < argc ? std::atoll(argv[++i]) : fallback;
        };
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return false;
        }
        if (arg == "--prefixes") {
            out.config.prefix_count = static_cast<std::size_t>(next(200'000));
        } else if (arg == "--readers") {
            out.config.reader_threads = static_cast<std::size_t>(next(4));
        } else if (arg == "--duration") {
            out.config.duration = std::chrono::milliseconds(next(3000));
        } else if (arg == "--batch") {
            out.config.batch_size = static_cast<std::size_t>(next(64));
        } else if (arg == "--mrt") {
            if (i + 1 < argc) out.mrt_path = argv[++i];
        } else if (arg == "--sweep") {
            out.rate_sweep = true;
        } else if (arg == "--csv") {
            out.csv = true;
        } else {
            std::fprintf(stderr, "unknown option: %s\n\n", arg.c_str());
            print_usage();
            return false;
        }
    }
    return true;
}

void rule(int width = 96) {
    for (int i = 0; i < width; ++i) std::putchar('-');
    std::putchar('\n');
}

void print_throughput_header() {
    std::printf("%-14s %12s %12s %10s %10s %10s %10s %12s\n", "variant", "lookups M/s", "updates/s",
                "p50 ns", "p99 ns", "p99.9 ns", "max ns", "retries");
    rule();
}

void print_throughput_row(const workload_result& r) {
    std::printf("%-14s %12.2f %12.0f %10.0f %10.0f %10.0f %10.0f %12llu\n", r.variant.c_str(),
                r.mlookups_per_second(), r.updates_per_second, r.latency.p50, r.latency.p99,
                r.latency.p999, r.latency.max, static_cast<unsigned long long>(r.seqlock_retries));
}

/// The headline number: how much read throughput the table loses when the
/// control plane starts writing to it.
double degradation_percent(double idle, double churn) {
    if (idle <= 0.0) return 0.0;
    return (idle - churn) / idle * 100.0;
}

template <class Fib>
void run_variant(const dataset& data, const options& opts, std::vector<workload_result>& idle_out,
                 std::vector<workload_result>& churn_out) {
    workload_config idle = opts.config;
    idle.writer = false;
    idle_out.push_back(run<Fib>(data, idle));

    workload_config churn = opts.config;
    churn.writer = true;
    churn_out.push_back(run<Fib>(data, churn));
}

template <class Fib>
void run_sweep(const dataset& data, const options& opts, double idle_rate) {
    static constexpr std::uint64_t rates[] = {1'000, 10'000, 100'000, 0};
    for (const auto rate : rates) {
        workload_config config = opts.config;
        config.writer = true;
        config.target_updates_per_second = rate;
        config.duration = std::chrono::milliseconds(1500);
        const auto result = run<Fib>(data, config);
        std::printf("%-14s %14s %12.2f %11.1f%% %10.0f %10.0f\n", result.variant.c_str(),
                    rate == 0 ? "unbounded" : std::to_string(rate).c_str(),
                    result.mlookups_per_second(),
                    degradation_percent(idle_rate, result.lookups_per_second), result.latency.p99,
                    result.latency.p999);
    }
}

}  // namespace

int main(int argc, char** argv) {
    options opts;
    if (!parse_args(argc, argv, opts)) return 0;

    std::printf("Building the route table...\n");
    dataset data = build_dataset(opts.config);

    if (!opts.mrt_path.empty()) {
        const auto error = load_real_table(data, opts.mrt_path, opts.config);
        if (!error.empty()) {
            std::fprintf(stderr, "could not load %s: %s\n", opts.mrt_path.c_str(), error.c_str());
            return 1;
        }
        std::printf("Loaded %zu prefixes from %s\n", data.prefixes.size(), opts.mrt_path.c_str());
    }

    std::printf("\n%zu prefixes, %zu reader threads, %lld ms per scenario, batch %zu\n",
                data.prefixes.size(), opts.config.reader_threads,
                static_cast<long long>(opts.config.duration.count()), opts.config.batch_size);
    std::printf("Hardware concurrency: %u\n\n", std::thread::hardware_concurrency());

    std::vector<workload_result> idle;
    std::vector<workload_result> churn;

    run_variant<mutex_fib<ipv4_address>>(data, opts, idle, churn);
    run_variant<shared_mutex_fib<ipv4_address>>(data, opts, idle, churn);
    run_variant<seqlock_fib<ipv4_address>>(data, opts, idle, churn);
    run_variant<rcu_fib<ipv4_address>>(data, opts, idle, churn);

    std::printf("== Scenario 1: readers only, no control-plane activity ==\n\n");
    print_throughput_header();
    for (const auto& result : idle) print_throughput_row(result);

    std::printf("\n== Scenario 2: readers with a writer reprogramming continuously ==\n\n");
    print_throughput_header();
    for (const auto& result : churn) print_throughput_row(result);

    std::printf("\n== What churn costs each design ==\n\n");
    std::printf("%-14s %14s %14s %14s\n", "variant", "idle M/s", "under churn", "degradation");
    rule(60);
    for (std::size_t i = 0; i < idle.size(); ++i) {
        std::printf("%-14s %14.2f %14.2f %13.1f%%\n", idle[i].variant.c_str(),
                    idle[i].mlookups_per_second(), churn[i].mlookups_per_second(),
                    degradation_percent(idle[i].lookups_per_second, churn[i].lookups_per_second));
    }

    std::printf("\n== Scenario 3: full table reprogram with the dataplane running ==\n\n");
    std::printf("%-14s %10s %12s %16s %18s\n", "variant", "routes", "seconds", "routes/s",
                "reader M/s during");
    rule(76);
    const auto convergence = std::vector<convergence_result>{
        measure_convergence<mutex_fib<ipv4_address>>(data, opts.config),
        measure_convergence<shared_mutex_fib<ipv4_address>>(data, opts.config),
        measure_convergence<seqlock_fib<ipv4_address>>(data, opts.config),
        measure_convergence<rcu_fib<ipv4_address>>(data, opts.config),
    };
    for (const auto& result : convergence) {
        std::printf("%-14s %10zu %12.3f %16.0f %18.2f\n", result.variant.c_str(), result.routes,
                    result.seconds, result.routes_per_second,
                    result.reader_lookups_per_second / 1e6);
    }

    if (opts.rate_sweep) {
        std::printf("\n== Read throughput against update rate ==\n\n");
        std::printf("%-14s %14s %12s %12s %10s %10s\n", "variant", "updates/s", "lookups M/s",
                    "degradation", "p99 ns", "p99.9 ns");
        rule(80);
        run_sweep<mutex_fib<ipv4_address>>(data, opts, idle[0].lookups_per_second);
        run_sweep<shared_mutex_fib<ipv4_address>>(data, opts, idle[1].lookups_per_second);
        run_sweep<seqlock_fib<ipv4_address>>(data, opts, idle[2].lookups_per_second);
        run_sweep<rcu_fib<ipv4_address>>(data, opts, idle[3].lookups_per_second);
    }

    if (opts.csv) {
        std::printf(
            "\nvariant,scenario,lookups_per_second,updates_per_second,p50_ns,p99_ns,p999_ns\n");
        for (const auto& result : idle) {
            std::printf("%s,idle,%.0f,%.0f,%.0f,%.0f,%.0f\n", result.variant.c_str(),
                        result.lookups_per_second, result.updates_per_second, result.latency.p50,
                        result.latency.p99, result.latency.p999);
        }
        for (const auto& result : churn) {
            std::printf("%s,churn,%.0f,%.0f,%.0f,%.0f,%.0f\n", result.variant.c_str(),
                        result.lookups_per_second, result.updates_per_second, result.latency.p50,
                        result.latency.p99, result.latency.p999);
        }
    }

    std::printf("\n");
    return 0;
}
