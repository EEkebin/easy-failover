#include "discovery/LinuxUdpDiscoveryTransport.hpp"

#include "heartbeat/UdpHeartbeatTransport.hpp"

#include <array>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace easyfailover {

namespace {

constexpr int kMaxDrainPerCall = 64;
constexpr std::size_t kRecvBufferBytes = 2048;

} // namespace

LinuxUdpDiscoveryTransport::LinuxUdpDiscoveryTransport(const std::string& bind_address) {
    const auto parsed = parseUdpHeartbeatEndpoint(bind_address);
    if (!parsed.endpoint.has_value()) {
        return; // leave socket_fd_ = -1; transport degrades to a no-op
    }
    port_ = parsed.endpoint->port;

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }

    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    const std::string& host = parsed.endpoint->host;
    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return;
    }
    socket_fd_ = fd;
}

LinuxUdpDiscoveryTransport::~LinuxUdpDiscoveryTransport() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
    }
}

void LinuxUdpDiscoveryTransport::broadcast(const std::string& payload) {
    if (socket_fd_ < 0) {
        return;
    }
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port_);
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST); // 255.255.255.255 (local segment)
    ::sendto(socket_fd_, payload.data(), payload.size(), 0,
             reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
}

std::vector<std::string> LinuxUdpDiscoveryTransport::drain() {
    std::vector<std::string> out;
    if (socket_fd_ < 0) {
        return out;
    }
    std::array<char, kRecvBufferBytes> buffer{};
    for (int i = 0; i < kMaxDrainPerCall; ++i) {
        const ssize_t n = ::recvfrom(socket_fd_, buffer.data(), buffer.size(), 0, nullptr, nullptr);
        if (n <= 0) {
            break; // EWOULDBLOCK/EAGAIN (no more) or error
        }
        out.emplace_back(buffer.data(), static_cast<std::size_t>(n));
    }
    return out;
}

} // namespace easyfailover
