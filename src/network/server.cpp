#include "network/server.hpp"
#include "protocol/tick.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

namespace mde {

namespace {
constexpr int kMaxEvents = 256;
constexpr std::size_t kMaxBatchPerIter = 8192;      // ticks drained per client per loop pass
// Backpressure cap per client. Kept small on purpose: once a slow client
// has this much unsent data queued, the worker stops reading the ring for
// it, so under overload the ring's drop-oldest policy discards stale ticks
// instead of this queue delivering them late. (Was 8 MB, ~350k ticks,
// which added ~1 s of queueing latency under overload.)
constexpr std::size_t kMaxPendingBytes = 64 * 1024;

void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
} // namespace

struct BroadcastServer::Client {
    int fd;
    TickRingBuffer::ConsumerHandle consumer;
    std::vector<std::uint8_t> pending;
    std::size_t pending_offset = 0;
    std::uint64_t bytes_flushed = 0;
    bool epollout_armed = false;
};

struct BroadcastServer::Worker {
    int epoll_fd = -1;
    std::mutex clients_mutex;
    std::unordered_map<int, std::unique_ptr<Client>> clients;
    std::unique_ptr<std::jthread> thread;

    // Accumulated stats for clients that have already disconnected, so
    // aggregate totals don't shrink when a client goes away.
    std::atomic<std::uint64_t> closed_sent{0};
    std::atomic<std::uint64_t> closed_dropped{0};
};

BroadcastServer::BroadcastServer(TickRingBuffer& ring, std::uint16_t port, std::size_t worker_count)
    : ring_(ring), port_(port), worker_count_(worker_count == 0 ? 1 : worker_count) {}

BroadcastServer::~BroadcastServer() { stop(); }

void BroadcastServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
    }
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(listen_fd_, 1024) < 0) {
        const std::string err = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("bind()/listen() failed: " + err);
    }
    // Port 0 asks the kernel for an ephemeral port; record what we got.
    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    set_nonblocking(listen_fd_);

    workers_.reserve(worker_count_);
    for (std::size_t i = 0; i < worker_count_; ++i) {
        auto w = std::make_unique<Worker>();
        w->epoll_fd = ::epoll_create1(0);
        if (w->epoll_fd < 0) {
            throw std::runtime_error(std::string("epoll_create1() failed: ") + std::strerror(errno));
        }
        Worker* wp = w.get();
        w->thread = std::make_unique<std::jthread>([this, wp, i](std::stop_token st) {
            // Named so per-thread profiles (perf, top -H) are readable.
            const std::string name = "mde-worker-" + std::to_string(i);
            ::pthread_setname_np(::pthread_self(), name.c_str());
            epoll_event events[kMaxEvents];
            while (!st.stop_requested()) {
                int n = ::epoll_wait(wp->epoll_fd, events, kMaxEvents, /*timeout_ms=*/0);
                for (int i = 0; i < n; ++i) {
                    const int fd = events[i].data.fd;
                    const uint32_t ev = events[i].events;
                    std::unique_lock lock(wp->clients_mutex);
                    auto it = wp->clients.find(fd);
                    if (it == wp->clients.end()) continue;
                    Client& c = *it->second;

                    bool remove = false;
                    if (ev & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                        remove = true;
                    } else {
                        if (ev & EPOLLIN) {
                            char buf[64];
                            ssize_t r = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                            if (r == 0) {
                                remove = true;
                            } else if (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
                                remove = true;
                            }
                        }
                        if (!remove && (ev & EPOLLOUT)) {
                            // flush handled in the drain pass below
                        }
                    }
                    if (remove) {
                        wp->closed_sent.fetch_add(c.bytes_flushed / kWireMessageSize,
                                                    std::memory_order_relaxed);
                        wp->closed_dropped.fetch_add(c.consumer.dropped, std::memory_order_relaxed);
                        ::epoll_ctl(wp->epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                        ::close(fd);
                        wp->clients.erase(it);
                    }
                }

                // Drain the ring buffer for every client this worker owns.
                std::unique_lock lock(wp->clients_mutex);
                for (auto it = wp->clients.begin(); it != wp->clients.end();) {
                    Client& c = *it->second;
                    bool broken = false;

                    // Flush anything left over from a previous pass first.
                    auto flush = [&]() -> void {
                        while (c.pending_offset < c.pending.size()) {
                            ssize_t sent = ::send(c.fd, c.pending.data() + c.pending_offset,
                                                   c.pending.size() - c.pending_offset,
                                                   MSG_NOSIGNAL | MSG_DONTWAIT);
                            if (sent > 0) {
                                c.pending_offset += static_cast<std::size_t>(sent);
                                c.bytes_flushed += static_cast<std::uint64_t>(sent);
                                continue;
                            }
                            if (sent < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) break;
                            if (sent < 0 && errno == EINTR) continue;
                            broken = true;
                            break;
                        }
                        if (c.pending_offset == c.pending.size()) {
                            c.pending.clear();
                            c.pending_offset = 0;
                        }
                    };

                    flush();

                    if (!broken) {
                        TickMessage tick;
                        std::size_t batch = 0;
                        while (!broken && batch < kMaxBatchPerIter &&
                               (c.pending.size() - c.pending_offset) < kMaxPendingBytes) {
                            if (!ring_.try_read(c.consumer, tick)) break;
                            const std::size_t at = c.pending.size();
                            c.pending.resize(at + kWireMessageSize);
                            encode_tick(tick, c.pending.data() + at);
                            ++batch;
                        }
                        flush();
                    }

                    const bool has_pending = c.pending_offset < c.pending.size();
                    if (has_pending != c.epollout_armed) {
                        epoll_event mod{};
                        mod.data.fd = c.fd;
                        mod.events = EPOLLIN | EPOLLRDHUP | (has_pending ? EPOLLOUT : 0u);
                        ::epoll_ctl(wp->epoll_fd, EPOLL_CTL_MOD, c.fd, &mod);
                        c.epollout_armed = has_pending;
                    }

                    if (broken) {
                        wp->closed_sent.fetch_add(c.bytes_flushed / kWireMessageSize,
                                                    std::memory_order_relaxed);
                        wp->closed_dropped.fetch_add(c.consumer.dropped, std::memory_order_relaxed);
                        ::epoll_ctl(wp->epoll_fd, EPOLL_CTL_DEL, c.fd, nullptr);
                        ::close(c.fd);
                        it = wp->clients.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        });
        workers_.push_back(std::move(w));
    }

    acceptor_thread_ = std::make_unique<std::jthread>(
        [this](std::stop_token st) { acceptor_loop(st); });

    started_ = true;
}

void BroadcastServer::acceptor_loop(std::stop_token st) {
    ::pthread_setname_np(::pthread_self(), "mde-acceptor");
    int aep = ::epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    ::epoll_ctl(aep, EPOLL_CTL_ADD, listen_fd_, &ev);

    std::size_t rr = 0;
    epoll_event events[16];
    while (!st.stop_requested()) {
        int n = ::epoll_wait(aep, events, 16, /*timeout_ms=*/50);
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd != listen_fd_) continue;
            for (;;) {
                int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
                if (fd < 0) break; // EAGAIN or transient error: nothing more to accept right now

                int one = 1;
                ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

                Worker* w = workers_[rr % workers_.size()].get();
                rr++;

                auto client = std::make_unique<Client>();
                client->fd = fd;
                client->consumer = ring_.create_consumer();

                {
                    std::unique_lock lock(w->clients_mutex);
                    w->clients.emplace(fd, std::move(client));
                }

                epoll_event cev{};
                cev.events = EPOLLIN | EPOLLRDHUP;
                cev.data.fd = fd;
                ::epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, fd, &cev);

                total_accepted_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    ::close(aep);
}

void BroadcastServer::stop() {
    if (!started_) return;
    started_ = false;

    if (acceptor_thread_) {
        acceptor_thread_->request_stop();
        acceptor_thread_->join();
        acceptor_thread_.reset();
    }
    for (auto& w : workers_) {
        w->thread->request_stop();
        w->thread->join();
        for (auto& [fd, client] : w->clients) {
            ::close(fd);
        }
        w->clients.clear();
        ::close(w->epoll_fd);
    }
    workers_.clear();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

BroadcastServer::Stats BroadcastServer::stats() const {
    Stats s{};
    s.total_accepted = total_accepted_.load(std::memory_order_relaxed);
    for (auto& w : workers_) {
        s.total_sent += w->closed_sent.load(std::memory_order_relaxed);
        s.total_dropped += w->closed_dropped.load(std::memory_order_relaxed);
        std::unique_lock lock(w->clients_mutex);
        s.active_clients += w->clients.size();
        for (auto& [fd, c] : w->clients) {
            s.total_sent += c->bytes_flushed / kWireMessageSize;
            s.total_dropped += c->consumer.dropped;
        }
    }
    return s;
}

} // namespace mde
