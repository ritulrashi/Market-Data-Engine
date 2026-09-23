#pragma once

#include "protocol/tick.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mde {

struct ClientResult {
    std::uint64_t received = 0;
    // Wall time from successful connect() to disconnect, used as the
    // denominator for throughput.
    std::chrono::nanoseconds elapsed{0};
    // End-to-end latency of *every* received message (recv wall clock -
    // producer publish timestamp), in nanoseconds, in arrival order.
    std::vector<std::int64_t> latency_ns;
    bool server_closed = false; // true if the server closed the connection first
};

// Connects to the broadcast server, reads the fixed-size tick stream, and
// measures received throughput and per-message end-to-end latency.
class TickClient {
public:
    using TickCallback = std::function<void(const TickMessage&)>;

    TickClient(std::string host, std::uint16_t port);

    // Blocks for approximately `duration` (or until the server disconnects),
    // then closes the connection and returns what it measured. If
    // `on_tick` is set it is called for every decoded tick, in order.
    ClientResult run_for(std::chrono::steady_clock::duration duration,
                         const TickCallback& on_tick = {});

private:
    std::string host_;
    std::uint16_t port_;
};

} // namespace mde
