#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace mde {

struct ClientResult {
    std::uint64_t received = 0;
    // A strided sample of end-to-end latencies (recv wall clock - producer
    // publish timestamp), in nanoseconds. Not every message is sampled --
    // storing every latency at multi-million-messages-per-second throughput
    // would dominate memory and skew the measurement itself.
    std::vector<std::int64_t> latency_samples_ns;
};

// Connects to the broadcast server, reads the fixed-size tick stream, and
// measures received throughput and per-message end-to-end latency.
class TickClient {
public:
    TickClient(std::string host, std::uint16_t port);

    // Blocks for approximately `duration`, then disconnects and returns
    // what it measured.
    ClientResult run_for(std::chrono::steady_clock::duration duration);

private:
    std::string host_;
    std::uint16_t port_;
};

} // namespace mde
