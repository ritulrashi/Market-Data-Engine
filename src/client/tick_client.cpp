#include "client/tick_client.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace mde {

TickClient::TickClient(std::string host, std::uint16_t port, bool record_latency)
    : host_(std::move(host)), port_(port), record_latency_(record_latency) {}

ClientResult TickClient::run_for(std::chrono::steady_clock::duration duration,
                                 const TickCallback& on_tick) {
    ClientResult result;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("invalid host address: " + host_);
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error(std::string("connect() failed: ") + std::strerror(errno));
    }

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    timeval tv{};
    tv.tv_usec = 200000; // 200ms, so a blocked recv still rechecks the deadline
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Read in large chunks and decode every complete message in the chunk:
    // one recv() per 24-byte message made the client, not the server, the
    // throughput bottleneck.
    constexpr std::size_t kBufSize = 64 * 1024;
    std::vector<std::uint8_t> buf(kBufSize);
    std::size_t have = 0; // bytes currently buffered (a partial message may carry over)

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + duration;

    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::recv(fd, buf.data() + have, kBufSize - have, 0);
        if (n > 0) {
            have += static_cast<std::size_t>(n);
            const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            std::size_t off = 0;
            for (; off + kWireMessageSize <= have; off += kWireMessageSize) {
                const TickMessage tick = decode_tick(buf.data() + off);
                ++result.received;
                if (record_latency_) {
                    if (result.latency_chunks.empty() ||
                        result.latency_chunks.back().size() == ClientResult::kLatencyChunk) {
                        result.latency_chunks.emplace_back().reserve(ClientResult::kLatencyChunk);
                    }
                    result.latency_chunks.back().push_back(static_cast<std::int64_t>(now_ns) -
                                                           static_cast<std::int64_t>(tick.timestamp_ns));
                }
                if (on_tick) on_tick(tick);
            }
            // Move any trailing partial message to the front of the buffer.
            if (off < have) std::memmove(buf.data(), buf.data() + off, have - off);
            have -= off;
            continue;
        }
        if (n == 0) {
            result.server_closed = true;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue; // recv timeout, recheck deadline
        break; // real error
    }

    result.elapsed = std::chrono::steady_clock::now() - start;
    ::close(fd);
    return result;
}

} // namespace mde
