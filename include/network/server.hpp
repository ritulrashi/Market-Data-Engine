#pragma once

#include "producer/tick_producer.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

namespace mde {

// Epoll-based async TCP server that broadcasts every tick published to the
// shared ring buffer to every connected client (full SPMC broadcast, no
// per-symbol filtering).
//
// Threading model: one acceptor thread owns the listening socket and
// round-robins newly accepted connections across a fixed pool of worker
// threads. Each worker owns its own epoll instance (level-triggered) and a
// subset of clients; each client holds its own SpmcRingBuffer consumer
// cursor, so worker threads read the shared ring buffer concurrently and
// independently -- this is the real multi-consumer concurrency the ring
// buffer is designed for. A per-client byte queue absorbs partial/blocked
// TCP writes; EPOLLOUT is used to resume flushing when the socket is
// writable again. Disconnects are detected via EPOLLHUP/EPOLLRDHUP/EPOLLERR
// or a zero-byte read, and clean up that client's socket and ring-buffer
// cursor without affecting any other client.
class BroadcastServer {
public:
    BroadcastServer(TickRingBuffer& ring, std::uint16_t port, std::size_t worker_count);
    ~BroadcastServer();

    BroadcastServer(const BroadcastServer&) = delete;
    BroadcastServer& operator=(const BroadcastServer&) = delete;

    // Opens the listening socket and starts the acceptor + worker threads.
    void start();

    // Signals every thread to stop and joins them. Safe to call from the
    // destructor if not called explicitly.
    void stop();

    struct Stats {
        std::uint64_t active_clients;
        std::uint64_t total_accepted;
        std::uint64_t total_sent;   // ticks actually flushed to a client socket
        std::uint64_t total_dropped; // ticks a consumer never got to read (overwritten)
    };
    Stats stats() const;

private:
    struct Worker;
    struct Client;

    void acceptor_loop(std::stop_token st);

    TickRingBuffer& ring_;
    std::uint16_t port_;
    std::size_t worker_count_;
    int listen_fd_ = -1;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::unique_ptr<std::jthread> acceptor_thread_;
    std::atomic<std::uint64_t> total_accepted_{0};
    bool started_ = false;
};

} // namespace mde
