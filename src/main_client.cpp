#include "client/tick_client.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
std::string arg_str(int argc, char** argv, const std::string& name, std::string def) {
    for (int i = 1; i < argc - 1; ++i) {
        if (name == argv[i]) return argv[i + 1];
    }
    return def;
}
std::uint64_t arg_u64(int argc, char** argv, const std::string& name, std::uint64_t def) {
    for (int i = 1; i < argc - 1; ++i) {
        if (name == argv[i]) return std::stoull(argv[i + 1]);
    }
    return def;
}

// Nearest-rank percentile: returns an actual observed sample (no
// interpolation). Reorders `v` partially via nth_element.
std::int64_t percentile_ns(std::vector<std::int64_t>& v, double p) {
    if (v.empty()) return 0;
    std::size_t rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size())));
    if (rank == 0) rank = 1;
    auto nth = v.begin() + static_cast<std::ptrdiff_t>(rank - 1);
    std::nth_element(v.begin(), nth, v.end());
    return *nth;
}
} // namespace

int main(int argc, char** argv) {
    const std::string host = arg_str(argc, argv, "--host", "127.0.0.1");
    const std::uint16_t port = static_cast<std::uint16_t>(arg_u64(argc, argv, "--port", 9001));
    const std::size_t clients = static_cast<std::size_t>(arg_u64(argc, argv, "--clients", 1));
    const std::uint64_t duration_s = arg_u64(argc, argv, "--duration", 20);
    const bool record_latency = arg_u64(argc, argv, "--record-latency", 1) != 0;

    std::vector<mde::ClientResult> results(clients);
    std::vector<std::jthread> threads;
    threads.reserve(clients);

    const auto duration = std::chrono::seconds(duration_s);
    for (std::size_t i = 0; i < clients; ++i) {
        threads.emplace_back([&, i] {
            mde::TickClient client(host, port, record_latency);
            results[i] = client.run_for(duration);
        });
    }
    for (auto& t : threads) t.join();

    std::uint64_t total_received = 0;
    double sum_elapsed_s = 0.0;
    std::size_t total_samples = 0;
    for (auto& r : results) total_samples += r.latency_count();
    std::vector<std::int64_t> pooled_latency_ns;
    pooled_latency_ns.reserve(total_samples);
    for (auto& r : results) {
        total_received += r.received;
        sum_elapsed_s += std::chrono::duration<double>(r.elapsed).count();
        for (auto& chunk : r.latency_chunks) {
            pooled_latency_ns.insert(pooled_latency_ns.end(), chunk.begin(), chunk.end());
            chunk = {}; // free as we go to limit peak memory
        }
    }

    const double p50_us = static_cast<double>(percentile_ns(pooled_latency_ns, 0.50)) / 1000.0;
    const double p99_us = static_cast<double>(percentile_ns(pooled_latency_ns, 0.99)) / 1000.0;
    const double p999_us = static_cast<double>(percentile_ns(pooled_latency_ns, 0.999)) / 1000.0;
    const double max_us = pooled_latency_ns.empty()
                              ? 0.0
                              : static_cast<double>(*std::max_element(pooled_latency_ns.begin(),
                                                                      pooled_latency_ns.end())) / 1000.0;

    // Each client's throughput uses its own measured connect-to-close time;
    // aggregate = sum of per-client rates.
    double aggregate_tps = 0.0;
    for (auto& r : results) {
        const double s = std::chrono::duration<double>(r.elapsed).count();
        if (s > 0) aggregate_tps += static_cast<double>(r.received) / s;
    }
    const double per_consumer_tps = clients > 0 ? aggregate_tps / static_cast<double>(clients) : 0.0;

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "CLIENTS=" << clients << " DURATION_S=" << duration_s
              << " MEAN_ELAPSED_S=" << std::setprecision(3)
              << (clients ? sum_elapsed_s / static_cast<double>(clients) : 0.0) << std::setprecision(1)
              << " TOTAL_RECEIVED=" << total_received << " AGGREGATE_TPS=" << aggregate_tps
              << " PER_CONSUMER_TPS=" << per_consumer_tps << " SAMPLES=" << pooled_latency_ns.size()
              << " P50_US=" << p50_us << " P99_US=" << p99_us << " P999_US=" << p999_us
              << " MAX_US=" << max_us << std::endl;

    for (std::size_t i = 0; i < clients; ++i) {
        std::cout << "  client[" << i << "] received=" << results[i].received
                  << " elapsed_s=" << std::setprecision(3)
                  << std::chrono::duration<double>(results[i].elapsed).count()
                  << std::setprecision(1) << std::endl;
    }

    return 0;
}
