#include "producer/tick_producer.hpp"

#include <chrono>
#include <cmath>
#include <thread>

namespace mde {

namespace {
constexpr double kDt = 1e-4;      // synthetic per-tick time step for the OU process
constexpr double kPriceFloor = 0.01; // prices can't go negative or to zero
} // namespace

TickProducer::TickProducer(TickRingBuffer& ring, std::size_t symbol_count,
                            std::uint64_t rate_per_sec, std::uint64_t seed)
    : ring_(ring), rate_per_sec_(rate_per_sec), rng_(seed == 0 ? std::random_device{}() : seed) {
    std::uniform_real_distribution<double> price_dist(20.0, 500.0);
    std::uniform_real_distribution<double> theta_dist(0.5, 3.0);
    std::uniform_real_distribution<double> vol_dist(0.05, 0.30); // as a fraction of price

    symbols_.reserve(symbol_count);
    for (std::size_t i = 0; i < symbol_count; ++i) {
        const double start_price = price_dist(rng_);
        SymbolState s;
        s.name = "SYM" + std::to_string(i);
        s.price = start_price;
        s.mean = start_price;
        s.theta = theta_dist(rng_);
        s.sigma = start_price * vol_dist(rng_);
        symbols_.push_back(std::move(s));
    }
}

void TickProducer::step_symbol(SymbolState& s) {
    // Discretized Ornstein-Uhlenbeck step: dx = theta*(mu - x)*dt + sigma*sqrt(dt)*Z
    const double drift = s.theta * (s.mean - s.price) * kDt;
    const double diffusion = s.sigma * std::sqrt(kDt) * normal_dist_(rng_);
    s.price += drift + diffusion;
    if (s.price < kPriceFloor) {
        s.price = kPriceFloor;
    }
}

void TickProducer::run(std::stop_token stop_token, std::uint64_t max_ticks) {
    const std::size_t n = symbols_.size();
    std::size_t next_symbol = 0;

    const bool throttled = rate_per_sec_ > 0;
    const std::chrono::nanoseconds one_second_ns = std::chrono::seconds(1);
    const std::chrono::nanoseconds interval =
        throttled ? std::chrono::nanoseconds(one_second_ns.count() /
                                              static_cast<std::int64_t>(rate_per_sec_))
                  : std::chrono::nanoseconds(0);
    auto next_send_time = std::chrono::steady_clock::now();

    std::uint64_t published = 0;
    while (!stop_token.stop_requested() && (max_ticks == 0 || published < max_ticks)) {
        SymbolState& s = symbols_[next_symbol];
        next_symbol = (next_symbol + 1) % n;

        step_symbol(s);

        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();

        TickMessage tick{};
        tick.symbol_id = static_cast<std::uint32_t>(&s - symbols_.data());
        tick.price = s.price;
        tick.size = size_dist_(rng_);
        tick.timestamp_ns = static_cast<std::uint64_t>(now_ns);

        ring_.publish(tick);
        ticks_published_.fetch_add(1, std::memory_order_relaxed);
        ++published;

        if (throttled) {
            next_send_time += interval;
            std::this_thread::sleep_until(next_send_time);
        }
    }
}

} // namespace mde
