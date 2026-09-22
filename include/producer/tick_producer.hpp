#pragma once

#include "protocol/tick.hpp"
#include "ring_buffer/spmc_ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <random>
#include <stop_token>
#include <string>
#include <vector>

namespace mde {

// Ring buffer capacity is fixed at compile time (power-of-two requirement)
// and shared by every binary that talks to it.
inline constexpr std::size_t kRingBufferCapacity = 65536;
using TickRingBuffer = SpmcRingBuffer<TickMessage, kRingBufferCapacity>;

// Per-symbol Ornstein-Uhlenbeck (mean-reverting) price process state.
struct SymbolState {
    std::string name;
    double price; // current price
    double mean;  // long-run mean the price reverts toward (mu)
    double theta; // reversion speed
    double sigma; // volatility
};

// Generates synthetic tick data for a configurable set of symbols and
// publishes it into the shared SPMC ring buffer. Runs on its own thread.
class TickProducer {
public:
    // rate_per_sec == 0 means unthrottled: publish as fast as possible.
    TickProducer(TickRingBuffer& ring, std::size_t symbol_count,
                 std::uint64_t rate_per_sec, std::uint64_t seed = 0);

    void run(std::stop_token stop_token);

    std::uint64_t ticks_published() const noexcept {
        return ticks_published_.load(std::memory_order_relaxed);
    }

    const std::vector<SymbolState>& symbols() const noexcept { return symbols_; }

private:
    void step_symbol(SymbolState& s);

    TickRingBuffer& ring_;
    std::vector<SymbolState> symbols_;
    std::uint64_t rate_per_sec_;
    std::atomic<std::uint64_t> ticks_published_{0};
    std::mt19937_64 rng_;
    std::uniform_int_distribution<std::uint32_t> size_dist_{1, 1000};
    std::normal_distribution<double> normal_dist_{0.0, 1.0};
};

} // namespace mde
