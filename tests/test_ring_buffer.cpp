#include "ring_buffer/spmc_ring_buffer.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using mde::SpmcRingBuffer;

void test_single_threaded_order() {
    // Payload type must be 8-byte-aligned with a size that's a multiple of
    // 8 bytes (word-atomic slot copies) -- std::uint64_t stands in for
    // TickMessage here since the test only cares about ordering.
    SpmcRingBuffer<std::uint64_t, 16> rb;
    auto c = rb.create_consumer();
    for (std::uint64_t i = 0; i < 10; ++i) rb.publish(i);

    for (std::uint64_t i = 0; i < 10; ++i) {
        std::uint64_t v = ~0ull;
        CHECK(rb.try_read(c, v));
        CHECK(v == i);
    }
    std::uint64_t v = ~0ull;
    CHECK(!rb.try_read(c, v)); // caught up, nothing new
}

void test_drop_detection() {
    constexpr std::size_t cap = 8;
    SpmcRingBuffer<std::uint64_t, cap> rb;
    auto c = rb.create_consumer();

    const std::uint64_t total = cap * 3;
    for (std::uint64_t i = 0; i < total; ++i) rb.publish(i); // never read while publishing -> overflow

    std::uint64_t v = ~0ull;
    CHECK(rb.try_read(c, v));
    CHECK(c.dropped == total - cap);
    CHECK(v == total - cap); // oldest surviving item after the overwrite

    // The rest of the surviving window should read back in order with no
    // further drops.
    for (std::size_t i = 1; i < cap; ++i) {
        CHECK(rb.try_read(c, v));
        CHECK(v == total - cap + i);
    }
    CHECK(c.dropped == total - cap);
}

namespace {
// Padded to a full cache line per consumer so each thread's counters live
// on distinct cache lines. Without this, race detectors that track shared
// state at cache-line (rather than exact byte) granularity -- Helgrind, in
// particular -- report false-positive "races" between threads writing to
// logically disjoint, but physically adjacent, vector elements.
struct alignas(64) ConsumerStats {
    std::uint64_t received = 0;
    char monotonic_ok = 1;
};
} // namespace

void test_multi_consumer_concurrent() {
    constexpr std::size_t cap = 1024;
    constexpr int num_consumers = 8;
    constexpr std::uint64_t num_items = 200000;

    SpmcRingBuffer<std::uint64_t, cap> rb;
    std::vector<SpmcRingBuffer<std::uint64_t, cap>::ConsumerHandle> handles(num_consumers);
    for (auto& h : handles) h = rb.create_consumer();

    std::atomic<bool> start{false};
    std::vector<ConsumerStats> stats(num_consumers);
    std::vector<std::thread> consumers;

    for (int ci = 0; ci < num_consumers; ++ci) {
        consumers.emplace_back([&, ci] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::uint64_t prev = 0;
            bool have_prev = false;
            while (handles[ci].next_seq < num_items) {
                std::uint64_t v = 0;
                if (rb.try_read(handles[ci], v)) {
                    ++stats[ci].received;
                    if (have_prev && v <= prev) stats[ci].monotonic_ok = 0;
                    prev = v;
                    have_prev = true;
                }
            }
        });
    }

    std::thread producer([&] {
        start.store(true, std::memory_order_release);
        for (std::uint64_t i = 0; i < num_items; ++i) rb.publish(i);
    });

    producer.join();
    for (auto& t : consumers) t.join();

    for (int ci = 0; ci < num_consumers; ++ci) {
        CHECK(stats[ci].monotonic_ok == 1);
        // Every published item is accounted for: either the consumer read it,
        // or it was overwritten before the consumer got to it (dropped). No
        // item can silently vanish from the accounting.
        CHECK(stats[ci].received + handles[ci].dropped == num_items);
    }
}
