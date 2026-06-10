#pragma once

#include "discovery/DiscoveryTransport.hpp"

#include <cstdint>
#include <string>

namespace easyfailover {

// UDP broadcast discovery transport. Binds a datagram socket to the configured `host:port` with
// SO_REUSEADDR/SO_REUSEPORT (so multiple nodes — and multiple processes in a test — can share the
// port) and SO_BROADCAST, broadcasts beacons to 255.255.255.255:port, and drains received
// datagrams non-blockingly. Constructing it never throws; a failed socket setup degrades to a
// no-op transport (broadcast does nothing, drain returns empty) so discovery can't crash the daemon.
class LinuxUdpDiscoveryTransport final : public DiscoveryTransport {
  public:
    explicit LinuxUdpDiscoveryTransport(const std::string& bind_address);
    ~LinuxUdpDiscoveryTransport() override;

    LinuxUdpDiscoveryTransport(const LinuxUdpDiscoveryTransport&) = delete;
    LinuxUdpDiscoveryTransport& operator=(const LinuxUdpDiscoveryTransport&) = delete;

    void broadcast(const std::string& payload) override;
    [[nodiscard]] std::vector<std::string> drain() override;

    [[nodiscard]] bool ok() const noexcept { return socket_fd_ >= 0; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  private:
    int socket_fd_ = -1;
    std::uint16_t port_ = 0;
};

} // namespace easyfailover
