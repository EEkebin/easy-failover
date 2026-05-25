#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace easyfailover {

struct HeartbeatDatagram {
    std::string peer_address;
    std::string payload;
};

struct HeartbeatSendResult {
    bool sent = false;
    std::string error;
};

struct HeartbeatReceiveResult {
    std::optional<HeartbeatDatagram> datagram;
    bool timed_out = false;
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
