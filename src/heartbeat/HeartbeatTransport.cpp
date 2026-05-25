#include "heartbeat/HeartbeatTransport.hpp"

namespace easyfailover {

HeartbeatSendResult DisabledHeartbeatTransport::send(const std::string& peer_address,
                                                     const std::string& payload) {
    static_cast<void>(peer_address);
    static_cast<void>(payload);
    return HeartbeatSendResult{.sent = false,
                               .error = "heartbeat transport is disabled"};
}

HeartbeatReceiveResult DisabledHeartbeatTransport::receive(const std::int64_t timeout_ms) {
    static_cast<void>(timeout_ms);
    return HeartbeatReceiveResult{.datagram = std::nullopt,
                                  .timed_out = false,
                                  .error = "heartbeat transport is disabled"};
}

} // namespace easyfailover
