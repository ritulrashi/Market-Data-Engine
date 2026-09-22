#include "client/tick_client.hpp"
#include "protocol/tick.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace mde {

TickClient::TickClient(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port) {}

ClientResult TickClient::run_for(std::chrono::steady_clock::duration duration) {
    ClientResult result;
    result.latency_samples_ns.reserve(1u << 20);

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

    constexpr std::uint64_t kSampleStride = 64;
    std::uint64_t idx = 0;

    std::uint8_t buf[kWireMessageSize];
    std::size_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + duration;

    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) break;

        ssize_t n = ::recv(fd, buf + got, kWireMessageSize - got, 0);
        if (n > 0) {
            got += static_cast<std::size_t>(n);
            if (got < kWireMessageSize) continue;

            const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
            TickMessage tick;
            std::memcpy(&tick, buf, kWireMessageSize);
            got = 0;

            ++result.received;
            if ((idx++ % kSampleStride) == 0) {
                result.latency_samples_ns.push_back(static_cast<std::int64_t>(now_ns) -
                                                      static_cast<std::int64_t>(tick.timestamp_ns));
            }
            continue;
        }
        if (n == 0) break; // server closed the connection
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue; // recv timeout, recheck deadline
        break; // real error
    }

    ::close(fd);
    return result;
}

} // namespace mde
