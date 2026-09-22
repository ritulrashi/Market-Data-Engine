#include "client/tick_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
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

double percentile_ns(std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const double rank = p * (static_cast<double>(sorted.size()) - 1.0);
    const std::size_t lo = static_cast<std::size_t>(rank);
    const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double frac = rank - static_cast<double>(lo);
    return static_cast<double>(sorted[lo]) * (1.0 - frac) + static_cast<double>(sorted[hi]) * frac;
}
} // namespace

int main(int argc, char** argv) {
    const std::string host = arg_str(argc, argv, "--host", "127.0.0.1");
    const std::uint16_t port = static_cast<std::uint16_t>(arg_u64(argc, argv, "--port", 9001));
    const std::size_t clients = static_cast<std::size_t>(arg_u64(argc, argv, "--clients", 1));
    const std::uint64_t duration_s = arg_u64(argc, argv, "--duration", 20);

    std::vector<mde::ClientResult> results(clients);
    std::vector<std::jthread> threads;
    threads.reserve(clients);

    const auto duration = std::chrono::seconds(duration_s);
    for (std::size_t i = 0; i < clients; ++i) {
        threads.emplace_back([&, i] {
            mde::TickClient client(host, port);
            results[i] = client.run_for(duration);
        });
    }
    for (auto& t : threads) t.join();

    std::uint64_t total_received = 0;
    std::vector<std::int64_t> pooled_latency_ns;
    for (auto& r : results) {
        total_received += r.received;
        pooled_latency_ns.insert(pooled_latency_ns.end(), r.latency_samples_ns.begin(),
                                  r.latency_samples_ns.end());
    }
    std::sort(pooled_latency_ns.begin(), pooled_latency_ns.end());

    const double p50_us = percentile_ns(pooled_latency_ns, 0.50) / 1000.0;
    const double p99_us = percentile_ns(pooled_latency_ns, 0.99) / 1000.0;
    const double p999_us = percentile_ns(pooled_latency_ns, 0.999) / 1000.0;

    const double aggregate_tps = static_cast<double>(total_received) / static_cast<double>(duration_s);
    const double per_consumer_tps = clients > 0 ? aggregate_tps / static_cast<double>(clients) : 0.0;

    std::cout << "CLIENTS=" << clients << " DURATION_S=" << duration_s
              << " TOTAL_RECEIVED=" << total_received << " AGGREGATE_TPS=" << aggregate_tps
              << " PER_CONSUMER_TPS=" << per_consumer_tps << " SAMPLES=" << pooled_latency_ns.size()
              << " P50_US=" << p50_us << " P99_US=" << p99_us << " P999_US=" << p999_us << std::endl;

    for (std::size_t i = 0; i < clients; ++i) {
        std::cout << "  client[" << i << "] received=" << results[i].received << std::endl;
    }

    return 0;
}
