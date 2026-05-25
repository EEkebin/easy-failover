#include "heartbeat/HeartbeatTransport.hpp"

namespace easyfailover {

HeartbeatSendResult DisabledHeartbeatTransport::send(const std::string& peer_address,
                                                     const std::string& payload) {
    return HeartbeatSendResult{.sent = false,
                               .peer_address = peer_address,
                               .payload = payload,
                               .error = std::string{kHeartbeatTransportDisabledError}};
}

HeartbeatReceiveResult DisabledHeartbeatTransport::receive(const std::int64_t timeout_ms) {
    return HeartbeatReceiveResult{.datagram = std::nullopt,
                                  .timed_out = false,
                                  .timeout_ms = timeout_ms,
                                  .error = std::string{kHeartbeatTransportDisabledError}};
}

} // namespace easyfailover
