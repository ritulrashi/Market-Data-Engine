#include "producer/tick_producer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
// Publishes exactly `n` ticks (n <= ring capacity, so nothing can be
// overwritten), then drains the ring.
std::vector<mde::TickMessage> produce(std::size_t symbols, std::uint64_t n, std::uint64_t seed,
                                      std::vector<mde::SymbolState>* final_state = nullptr) {
    auto ring = std::make_unique<mde::TickRingBuffer>();
    auto consumer = ring->create_consumer();
    mde::TickProducer producer(*ring, symbols, /*rate_per_sec=*/0, seed);
    producer.run(std::stop_token{}, n);
    EXPECT_EQ(producer.ticks_published(), n);
    std::vector<mde::TickMessage> out;
    mde::TickMessage m;
    while (ring->try_read(consumer, m)) out.push_back(m);
    EXPECT_EQ(out.size(), n);
    EXPECT_EQ(consumer.dropped, 0u);
    if (final_state) *final_state = producer.symbols();
    return out;
}
} // namespace

TEST(Producer, TicksRoundRobinSymbolsWithValidFields) {
    auto ticks = produce(10, 20000, 42);
    std::uint64_t prev_ts = 0;
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        const auto& t = ticks[i];
        ASSERT_EQ(t.symbol_id, i % 10);
        ASSERT_GE(t.size, 1u);
        ASSERT_LE(t.size, 1000u);
        ASSERT_GT(t.price, 0.0);
        ASSERT_GE(t.timestamp_ns, prev_ts);
        prev_ts = t.timestamp_ns;
    }
}

TEST(Producer, SameSeedGivesSamePricePath) {
    auto a = produce(10, 5000, 7);
    auto b = produce(10, 5000, 7);
    for (std::size_t i = 0; i < 5000; ++i) {
        ASSERT_EQ(a[i].price, b[i].price);
        ASSERT_EQ(a[i].size, b[i].size);
    }
}

TEST(Producer, PricesStayPositiveAndNearMean) {
    // Sanity check on the OU walk after 6k steps per symbol: prices stay
    // positive and within 50% of their long-run mean. (This does not prove
    // mean reversion statistically; it catches a broken/exploding walk.)
    std::vector<mde::SymbolState> st;
    auto ticks = produce(10, 60000, 11, &st);
    ASSERT_EQ(st.size(), 10u);
    for (const auto& s : st) {
        EXPECT_GT(s.price, 0.0) << s.name;
        EXPECT_LT(std::abs(s.price - s.mean), 0.5 * s.mean) << s.name;
    }
}

TEST(Producer, ThrottledRateIsApproximatelyHonored) {
    auto ring = std::make_unique<mde::TickRingBuffer>();
    mde::TickProducer producer(*ring, 10, /*rate_per_sec=*/10000, 1);
    std::stop_source stop;
    std::jthread t([&] { producer.run(stop.get_token()); });
    std::this_thread::sleep_for(500ms);
    stop.request_stop();
    t.join();
    // 0.5 s at 10k/s = ~5000. Generous bounds: this is a shared VM.
    EXPECT_GT(producer.ticks_published(), 3000u);
    EXPECT_LT(producer.ticks_published(), 7000u);
}
