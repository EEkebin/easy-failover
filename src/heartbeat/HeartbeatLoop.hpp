#pragma once

#include "config/Config.hpp"
#include "core/FailoverDecision.hpp"
#include "heartbeat/HeartbeatTransport.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace easyfailover {

struct HeartbeatSendObservation {
    std::string peer_id;
    std::string peer_address;
    bool sent = false;
    std::string error;
};

struct HeartbeatReceiveObservation {
    bool attempted = false;
    bool timed_out = false;
    std::int64_t timeout_ms = 0;
    std::string peer_address;
    std::string payload;
    std::optional<PeerStatus> peer_status;
    std::string error;
};

struct HeartbeatLoopResult {
    std::vector<HeartbeatSendObservation> sends;
    HeartbeatReceiveObservation receive;
};

[[nodiscard]] HeartbeatLoopResult runHeartbeatLoopOnce(const LocalNodeStatus& local,
                                                       const std::vector<PeerConfig>& peers,
                                                       std::int64_t receive_timeout_ms,
                                                       HeartbeatTransport& transport);

} // namespace easyfailover
