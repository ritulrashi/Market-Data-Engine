#include "network/server.hpp"
#include "producer/tick_producer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};
void handle_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

std::uint16_t arg_u16(int argc, char** argv, const std::string& name, std::uint16_t def) {
    for (int i = 1; i < argc - 1; ++i) {
        if (name == argv[i]) return static_cast<std::uint16_t>(std::stoul(argv[i + 1]));
    }
    return def;
}
std::uint64_t arg_u64(int argc, char** argv, const std::string& name, std::uint64_t def) {
    for (int i = 1; i < argc - 1; ++i) {
        if (name == argv[i]) return std::stoull(argv[i + 1]);
    }
    return def;
}
} // namespace

int main(int argc, char** argv) {
    const std::uint16_t port = arg_u16(argc, argv, "--port", 9001);
    const std::size_t symbols = static_cast<std::size_t>(arg_u64(argc, argv, "--symbols", 10));
    const std::uint64_t rate = arg_u64(argc, argv, "--rate", 0); // 0 = unthrottled
    const std::size_t workers = static_cast<std::size_t>(
        arg_u64(argc, argv, "--workers", std::max(1u, std::thread::hardware_concurrency() / 2)));
    const std::uint64_t seed = arg_u64(argc, argv, "--seed", 0);

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    mde::TickRingBuffer ring;
    mde::TickProducer producer(ring, symbols, rate, seed);

    std::stop_source producer_stop;
    std::jthread producer_thread([&](std::stop_token) {
        ::pthread_setname_np(::pthread_self(), "mde-producer");
        producer.run(producer_stop.get_token());
    });

    mde::BroadcastServer server(ring, port, workers);
    server.start();

    std::cerr << "market_data_server listening on port " << port << " with " << workers
              << " worker thread(s), " << symbols << " symbols, ring capacity "
              << mde::kRingBufferCapacity << ", rate="
              << (rate == 0 ? std::string("unthrottled") : std::to_string(rate) + "/s") << "\n";

    const auto start = std::chrono::steady_clock::now();
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        const auto stats = server.stats();
        std::cout << "STATS t=" << uptime << " produced=" << producer.ticks_published()
                  << " clients=" << stats.active_clients << " accepted=" << stats.total_accepted
                  << " sent=" << stats.total_sent << " dropped=" << stats.total_dropped
                  << std::endl;
    }

    std::cerr << "shutting down...\n";
    producer_stop.request_stop();
    producer_thread.join();

    // Snapshot stats before tearing down worker threads/sockets -- stop()
    // closes every remaining client without preserving per-client counters.
    const auto final_stats = server.stats();
    server.stop();

    std::cout << "FINAL produced=" << producer.ticks_published()
              << " accepted=" << final_stats.total_accepted << " sent=" << final_stats.total_sent
              << " dropped=" << final_stats.total_dropped << std::endl;

    return 0;
}
