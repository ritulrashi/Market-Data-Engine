#include "protocol/tick.hpp"
#include "ring_buffer/spmc_ring_buffer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using mde::SpmcRingBuffer;

TEST(RingBuffer, EmptyRingHasNothingToRead) {
    // Regression: slots used to start at sequence 0, the same as a fresh
    // consumer's cursor, so this returned true with an all-zero item.
    SpmcRingBuffer<std::uint64_t, 8> rb;
    auto c = rb.create_consumer();
    std::uint64_t v = 777;
    EXPECT_FALSE(rb.try_read(c, v));
    EXPECT_EQ(v, 777u);
    EXPECT_EQ(c.dropped, 0u);
}

TEST(RingBuffer, SingleThreadedOrder) {
    SpmcRingBuffer<std::uint64_t, 16> rb;
    auto c = rb.create_consumer();
    for (std::uint64_t i = 0; i < 10; ++i) rb.publish(i);

    for (std::uint64_t i = 0; i < 10; ++i) {
        std::uint64_t v = ~0ull;
        ASSERT_TRUE(rb.try_read(c, v));
        EXPECT_EQ(v, i);
    }
    std::uint64_t v = ~0ull;
    EXPECT_FALSE(rb.try_read(c, v)); // caught up, nothing new
    EXPECT_EQ(c.dropped, 0u);
}

TEST(RingBuffer, OrderPreservedAcrossManyWraparounds) {
    SpmcRingBuffer<std::uint64_t, 8> rb;
    auto c = rb.create_consumer();
    std::uint64_t next = 0;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        rb.publish(i);
        if (i % 5 == 4) { // read in bursts, never falling a full lap behind
            std::uint64_t v;
            while (rb.try_read(c, v)) ASSERT_EQ(v, next++);
        }
    }
    std::uint64_t v;
    while (rb.try_read(c, v)) ASSERT_EQ(v, next++);
    EXPECT_EQ(next, 1000u);
    EXPECT_EQ(c.dropped, 0u);
}

TEST(RingBuffer, LateConsumerSeesOnlyNewItems) {
    SpmcRingBuffer<std::uint64_t, 16> rb;
    for (std::uint64_t i = 0; i < 5; ++i) rb.publish(i);
    auto c = rb.create_consumer();
    std::uint64_t v;
    EXPECT_FALSE(rb.try_read(c, v));
    rb.publish(42);
    ASSERT_TRUE(rb.try_read(c, v));
    EXPECT_EQ(v, 42u);
}

TEST(RingBuffer, DropOldestAfterSeveralLaps) {
    constexpr std::size_t cap = 8;
    SpmcRingBuffer<std::uint64_t, cap> rb;
    auto c = rb.create_consumer();

    const std::uint64_t total = cap * 3;
    for (std::uint64_t i = 0; i < total; ++i) rb.publish(i); // producer never blocks

    std::uint64_t v = ~0ull;
    ASSERT_TRUE(rb.try_read(c, v));
    EXPECT_EQ(c.dropped, total - cap);
    EXPECT_EQ(v, total - cap); // oldest surviving item after the overwrite

    for (std::size_t i = 1; i < cap; ++i) {
        ASSERT_TRUE(rb.try_read(c, v));
        EXPECT_EQ(v, total - cap + i);
    }
    EXPECT_FALSE(rb.try_read(c, v));
    EXPECT_EQ(c.dropped, total - cap);
}

TEST(RingBuffer, DropOldestDropsOnlyOverwrittenItems) {
    // Regression: one item past capacity used to drop a whole lap (read 1
    // item, dropped 8). Only the single overwritten item should be lost.
    SpmcRingBuffer<std::uint64_t, 8> rb;
    auto c = rb.create_consumer();
    for (std::uint64_t i = 100; i < 109; ++i) rb.publish(i);

    std::vector<std::uint64_t> got;
    std::uint64_t v;
    while (rb.try_read(c, v)) got.push_back(v);
    EXPECT_EQ(got, (std::vector<std::uint64_t>{101, 102, 103, 104, 105, 106, 107, 108}));
    EXPECT_EQ(c.dropped, 1u);
}

TEST(RingBuffer, DropCountersArePerConsumer) {
    SpmcRingBuffer<std::uint64_t, 8> rb;
    auto fast = rb.create_consumer();
    auto slow = rb.create_consumer();
    std::uint64_t v;
    for (std::uint64_t i = 0; i < 20; ++i) {
        rb.publish(i);
        ASSERT_TRUE(rb.try_read(fast, v));
        EXPECT_EQ(v, i);
    }
    ASSERT_TRUE(rb.try_read(slow, v));
    EXPECT_EQ(v, 12u);
    EXPECT_EQ(slow.dropped, 12u);
    EXPECT_EQ(fast.dropped, 0u);
}

TEST(RingBuffer, TickMessagePayloadRoundTrip) {
    SpmcRingBuffer<mde::TickMessage, 4> rb;
    auto c = rb.create_consumer();
    mde::TickMessage in{9, 101.25, 300, 1234567890123ull};
    rb.publish(in);
    mde::TickMessage out{};
    ASSERT_TRUE(rb.try_read(c, out));
    EXPECT_EQ(out.symbol_id, 9u);
    EXPECT_EQ(out.price, 101.25);
    EXPECT_EQ(out.size, 300u);
    EXPECT_EQ(out.timestamp_ns, 1234567890123ull);
}

namespace {
// Padded to a cache line per consumer so Helgrind (which tracks at
// cache-line granularity) does not report false sharing between threads
// writing logically disjoint vector elements as races.
struct alignas(64) ConsumerStats {
    std::uint64_t received = 0;
    std::uint64_t order_violations = 0;
    std::uint64_t torn = 0;
};

// Four words that must always carry the same value: any mix of words from
// two different publishes is a torn (corrupted) read.
struct Wide {
    std::uint64_t w[4];
};

template <typename T, std::size_t Cap, typename Make, typename Check>
void run_concurrent(int num_consumers, std::uint64_t num_items, Make make, Check check,
                    std::vector<ConsumerStats>& stats,
                    std::vector<typename SpmcRingBuffer<T, Cap>::ConsumerHandle>& handles) {
    auto rb = std::make_unique<SpmcRingBuffer<T, Cap>>();
    handles.assign(num_consumers, {});
    for (auto& h : handles) h = rb->create_consumer();
    stats.assign(num_consumers, {});

    std::atomic<int> ready{0};
    std::vector<std::thread> consumers;
    for (int ci = 0; ci < num_consumers; ++ci) {
        consumers.emplace_back([&, ci] {
            ready.fetch_add(1);
            std::uint64_t prev = 0;
            bool have_prev = false;
            while (handles[ci].next_seq < num_items) {
                T v;
                if (rb->try_read(handles[ci], v)) {
                    ++stats[ci].received;
                    std::uint64_t key = 0;
                    if (!check(v, key)) ++stats[ci].torn;
                    if (have_prev && key <= prev) ++stats[ci].order_violations;
                    prev = key;
                    have_prev = true;
                }
            }
        });
    }
    while (ready.load() < num_consumers) std::this_thread::yield();
    for (std::uint64_t i = 0; i < num_items; ++i) rb->publish(make(i));
    for (auto& t : consumers) t.join();
}
} // namespace

TEST(RingBufferConcurrent, MultiConsumerOrderedAndFullyAccounted) {
    constexpr int kConsumers = 8;
    constexpr std::uint64_t kItems = 200000;
    std::vector<ConsumerStats> stats;
    std::vector<SpmcRingBuffer<std::uint64_t, 1024>::ConsumerHandle> handles;
    run_concurrent<std::uint64_t, 1024>(
        kConsumers, kItems, [](std::uint64_t i) { return i; },
        [](std::uint64_t v, std::uint64_t& key) { key = v; return true; }, stats, handles);

    for (int ci = 0; ci < kConsumers; ++ci) {
        EXPECT_EQ(stats[ci].order_violations, 0u) << "consumer " << ci;
        // Every published item is either read or counted as dropped.
        EXPECT_EQ(stats[ci].received + handles[ci].dropped, kItems) << "consumer " << ci;
    }
}

TEST(RingBufferConcurrent, NoTornReadsUnderHeavyLapping) {
    // Tiny ring + fast producer guarantees constant overwrites of slots
    // that consumers are in the middle of copying.
    constexpr int kConsumers = 6;
    constexpr std::uint64_t kItems = 300000;
    std::vector<ConsumerStats> stats;
    std::vector<SpmcRingBuffer<Wide, 16>::ConsumerHandle> handles;
    run_concurrent<Wide, 16>(
        kConsumers, kItems, [](std::uint64_t i) { return Wide{{i, i, i, i}}; },
        [](const Wide& v, std::uint64_t& key) {
            key = v.w[0];
            return v.w[1] == v.w[0] && v.w[2] == v.w[0] && v.w[3] == v.w[0];
        },
        stats, handles);

    std::uint64_t total_dropped = 0;
    for (int ci = 0; ci < kConsumers; ++ci) {
        EXPECT_EQ(stats[ci].torn, 0u) << "consumer " << ci;
        EXPECT_EQ(stats[ci].order_violations, 0u) << "consumer " << ci;
        EXPECT_EQ(stats[ci].received + handles[ci].dropped, kItems) << "consumer " << ci;
        total_dropped += handles[ci].dropped;
    }
    RecordProperty("total_dropped", std::to_string(total_dropped));
}

TEST(RingBufferConcurrent, NoDropsWhenRingNeverFills) {
    // 50k items into a 64k ring cannot overflow, so every consumer must see
    // every item exactly once, in order.
    constexpr int kConsumers = 4;
    constexpr std::uint64_t kItems = 50000;
    std::vector<ConsumerStats> stats;
    std::vector<SpmcRingBuffer<std::uint64_t, 65536>::ConsumerHandle> handles;
    run_concurrent<std::uint64_t, 65536>(
        kConsumers, kItems, [](std::uint64_t i) { return i; },
        [](std::uint64_t v, std::uint64_t& key) { key = v; return true; }, stats, handles);
    for (int ci = 0; ci < kConsumers; ++ci) {
        EXPECT_EQ(stats[ci].received, kItems);
        EXPECT_EQ(handles[ci].dropped, 0u);
        EXPECT_EQ(stats[ci].order_violations, 0u);
    }
}
