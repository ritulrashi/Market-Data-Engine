// End-to-end tests: real TickProducer thread -> SPMC ring -> BroadcastServer
// (epoll workers) -> real TCP sockets -> TickClient.
#include "client/tick_client.hpp"
#include "network/server.hpp"
#include "producer/tick_producer.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

namespace {
constexpr std::size_t kSymbols = 10;

struct Engine {
    explicit Engine(std::uint64_t rate, std::size_t workers = 2)
        : ring(std::make_unique<mde::TickRingBuffer>()),
          producer(*ring, kSymbols, rate, /*seed=*/1234),
          server(*ring, /*port=*/0, workers) {
        producer_thread = std::jthread([this](std::stop_token st) { producer.run(st); });
        server.start();
    }
    ~Engine() {
        server.stop();
        producer_thread.request_stop();
        producer_thread.join();
    }
    std::uint16_t port() const { return server.port(); }

    std::unique_ptr<mde::TickRingBuffer> ring;
    mde::TickProducer producer;
    mde::BroadcastServer server;
    std::jthread producer_thread;
};

// Checks the stream a client receives: valid fields, symbols strictly
// round-robin (the producer emits 0,1,...,9,0,1,... so any gap, duplicate
// or reordering breaks the pattern) and non-decreasing timestamps.
struct StreamChecker {
    std::uint64_t count = 0;
    std::uint64_t order_breaks = 0;
    std::uint64_t invalid = 0;
    std::uint64_t last_ts = 0;
    std::uint32_t last_sym = 0;

    void operator()(const mde::TickMessage& t) {
        if (t.symbol_id >= kSymbols || !(t.price > 0.0) || t.size < 1 || t.size > 1000) ++invalid;
        if (count > 0 && (t.symbol_id != (last_sym + 1) % kSymbols || t.timestamp_ns < last_ts))
            ++order_breaks;
        last_sym = t.symbol_id;
        last_ts = t.timestamp_ns;
        ++count;
    }
};

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

bool wait_for_active_clients(mde::BroadcastServer& s, std::uint64_t n,
                             std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (s.stats().active_clients == n) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}
} // namespace

TEST(Integration, AllClientsReceiveCorrectOrderedTicks) {
    Engine e(/*rate=*/20000);
    constexpr int kClients = 3;
    std::vector<StreamChecker> checks(kClients);
    std::vector<mde::ClientResult> results(kClients);
    {
        std::vector<std::jthread> ts;
        for (int i = 0; i < kClients; ++i) {
            ts.emplace_back([&, i] {
                mde::TickClient c("127.0.0.1", e.port());
                results[i] = c.run_for(1500ms, std::ref(checks[i]));
            });
        }
    }
    for (int i = 0; i < kClients; ++i) {
        SCOPED_TRACE(i);
        EXPECT_GT(results[i].received, 10000u); // ~30k expected at 20k/s for 1.5 s
        EXPECT_EQ(checks[i].count, results[i].received);
        EXPECT_EQ(checks[i].order_breaks, 0u);
        EXPECT_EQ(checks[i].invalid, 0u);
    }
    EXPECT_EQ(e.server.stats().total_dropped, 0u);
    EXPECT_EQ(e.server.stats().total_accepted, static_cast<std::uint64_t>(kClients));
}

TEST(Integration, ClientDisconnectMidStreamOthersKeepReceiving) {
    Engine e(/*rate=*/20000);
    StreamChecker b_check, c_check;
    std::atomic<std::uint64_t> a_closed_at{0};
    std::atomic<std::uint64_t> b_after{0}, c_after{0};

    auto counting = [&](StreamChecker& chk, std::atomic<std::uint64_t>& after) {
        return [&](const mde::TickMessage& t) {
            chk(t);
            const auto closed = a_closed_at.load();
            if (closed != 0 && t.timestamp_ns > closed) after.fetch_add(1, std::memory_order_relaxed);
        };
    };

    mde::ClientResult rb, rc;
    std::jthread tb([&] { rb = mde::TickClient("127.0.0.1", e.port()).run_for(1500ms, counting(b_check, b_after)); });
    std::jthread tc([&] { rc = mde::TickClient("127.0.0.1", e.port()).run_for(1500ms, counting(c_check, c_after)); });

    mde::ClientResult ra = mde::TickClient("127.0.0.1", e.port()).run_for(300ms);
    a_closed_at.store(now_ns());
    EXPECT_GT(ra.received, 0u);

    // The server must notice the disconnect and drop to 2 clients while B
    // and C are still connected.
    EXPECT_TRUE(wait_for_active_clients(e.server, 2));

    tb.join();
    tc.join();
    // ~1.2 s x 20k/s = ~24k ticks published after A left; require a clear
    // majority actually arrived at each remaining client.
    EXPECT_GT(b_after.load(), 10000u);
    EXPECT_GT(c_after.load(), 10000u);
    EXPECT_EQ(b_check.order_breaks, 0u);
    EXPECT_EQ(c_check.order_breaks, 0u);
    EXPECT_EQ(e.server.stats().total_dropped, 0u);
}

TEST(Integration, AbruptResetDisconnectIsHandled) {
    Engine e(/*rate=*/20000);
    StreamChecker chk;
    mde::ClientResult r;
    std::jthread survivor([&] { r = mde::TickClient("127.0.0.1", e.port()).run_for(1200ms, std::ref(chk)); });

    // Raw socket that closes with RST (SO_LINGER 0) instead of FIN.
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(e.port());
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_TRUE(wait_for_active_clients(e.server, 2));
    std::this_thread::sleep_for(200ms);
    linger lg{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    ::close(fd);

    EXPECT_TRUE(wait_for_active_clients(e.server, 1));
    survivor.join();
    EXPECT_GT(r.received, 10000u);
    EXPECT_EQ(chk.order_breaks, 0u);
}

TEST(Integration, SlowClientIsDroppedOldestWithoutAffectingFastClient) {
    // One client never reads. Once its socket buffers and the server's
    // 64 KB per-client queue fill, its ring cursor gets lapped and the
    // ring's drop-oldest policy kicks in. The fast client must be
    // unaffected: complete, ordered stream.
    Engine e(/*rate=*/200000);
    int slow = ::socket(AF_INET, SOCK_STREAM, 0);
    int small = 4096;
    ::setsockopt(slow, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(e.port());
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(::connect(slow, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    StreamChecker chk;
    auto r = mde::TickClient("127.0.0.1", e.port()).run_for(2500ms, std::ref(chk));

    // Drops are counted lazily: a consumer only discovers it was lapped on
    // its next read attempt, and the server stops reading the ring for a
    // client whose queue is full. Drain the slow socket so the server
    // resumes reading for it and records the lap.
    std::vector<char> sink(1 << 16);
    const auto drain_until = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < drain_until) {
        if (::recv(slow, sink.data(), sink.size(), MSG_DONTWAIT) <= 0) std::this_thread::sleep_for(1ms);
    }
    const auto stats = e.server.stats();
    ::close(slow);

    EXPECT_GT(stats.total_dropped, 0u) << "slow client was never lapped";
    EXPECT_GT(r.received, 100000u);
    EXPECT_EQ(chk.order_breaks, 0u) << "fast client lost or reordered ticks";
    EXPECT_EQ(chk.invalid, 0u);
}

TEST(Integration, InboundClientBytesAreIgnored) {
    // The protocol is one-way; a client that writes to the server must be
    // neither disconnected nor stalled.
    Engine e(/*rate=*/20000);
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(e.port());
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_TRUE(wait_for_active_clients(e.server, 1));
    const char hello[] = "hello server";
    for (int i = 0; i < 20; ++i) {
        ASSERT_GT(::send(fd, hello, sizeof(hello), MSG_NOSIGNAL), 0);
        std::this_thread::sleep_for(10ms);
    }
    std::vector<char> buf(1 << 16);
    std::size_t got = 0;
    const auto until = std::chrono::steady_clock::now() + 300ms;
    while (std::chrono::steady_clock::now() < until) {
        ssize_t n = ::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT);
        if (n > 0) got += static_cast<std::size_t>(n);
        else std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(e.server.stats().active_clients, 1u);
    EXPECT_GT(got, 24u * 1000);
    ::close(fd);
}

TEST(Integration, ClientSeesServerShutdown) {
    auto e = std::make_unique<Engine>(/*rate=*/20000);
    mde::ClientResult r;
    std::jthread t([&] { r = mde::TickClient("127.0.0.1", e->port()).run_for(5s); });
    ASSERT_TRUE(wait_for_active_clients(e->server, 1));
    std::this_thread::sleep_for(200ms);
    e->server.stop();
    t.join();
    EXPECT_TRUE(r.server_closed);
    EXPECT_LT(r.elapsed, 4s);
    EXPECT_GT(r.received, 0u);
}

TEST(Integration, ConnectToClosedPortThrows) {
    std::uint16_t port;
    {
        Engine e(/*rate=*/1000);
        port = e.port();
    }
    mde::TickClient c("127.0.0.1", port);
    EXPECT_THROW(c.run_for(100ms), std::runtime_error);
}
