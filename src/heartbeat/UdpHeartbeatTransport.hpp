#pragma once

#include "heartbeat/HeartbeatTransport.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace easyfailover {

struct UdpHeartbeatEndpoint {
    std::string host;
    std::uint16_t port = 0;
    std::string normalized_address;
};

struct UdpHeartbeatEndpointParseResult {
    std::optional<UdpHeartbeatEndpoint> endpoint;
    std::string error;
};

[[nodiscard]] UdpHeartbeatEndpointParseResult
parseUdpHeartbeatEndpoint(std::string_view address);

class UdpHeartbeatTransport final : public HeartbeatTransport {
  public:
    explicit UdpHeartbeatTransport(std::string bind_address);
    ~UdpHeartbeatTransport() override;

    UdpHeartbeatTransport(const UdpHeartbeatTransport&) = delete;
    UdpHeartbeatTransport& operator=(const UdpHeartbeatTransport&) = delete;

    UdpHeartbeatTransport(UdpHeartbeatTransport&& other) noexcept;
    UdpHeartbeatTransport& operator=(UdpHeartbeatTransport&& other) noexcept;

    [[nodiscard]] HeartbeatSendResult send(const std::string& peer_address,
                                           const std::string& payload) override;

    [[nodiscard]] HeartbeatReceiveResult receive(std::int64_t timeout_ms) override;

    [[nodiscard]] const std::string& localAddress() const noexcept;

  private:
    int socket_fd_ = -1;
    std::string local_address_;
};

} // namespace easyfailover
