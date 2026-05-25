#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace easyfailover {

constexpr std::string_view kHeartbeatTransportDisabledError = "heartbeat transport is disabled";

struct HeartbeatDatagram {
    std::string peer_address;
    std::string payload;
};

struct HeartbeatSendResult {
    bool sent = false;
    std::string peer_address;
    std::string payload;
    std::string error;
};

struct HeartbeatReceiveResult {
    std::optional<HeartbeatDatagram> datagram;
    bool timed_out = false;
    std::int64_t timeout_ms = 0;
    std::string error;
};

class HeartbeatTransport {
  public:
    virtual ~HeartbeatTransport() = default;

    [[nodiscard]] virtual HeartbeatSendResult send(const std::string& peer_address,
                                                   const std::string& payload) = 0;

    [[nodiscard]] virtual HeartbeatReceiveResult receive(std::int64_t timeout_ms) = 0;
};

class DisabledHeartbeatTransport final : public HeartbeatTransport {
  public:
    [[nodiscard]] HeartbeatSendResult send(const std::string& peer_address,
                                           const std::string& payload) override;

    [[nodiscard]] HeartbeatReceiveResult receive(std::int64_t timeout_ms) override;
};

} // namespace easyfailover
