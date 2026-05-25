#pragma once

#include "core/FailoverDecision.hpp"
#include "core/NodeState.hpp"

#include <optional>
#include <string>

namespace easyfailover {

constexpr int kHeartbeatMessageVersion = 1;

struct HeartbeatMessage {
    int version = kHeartbeatMessageVersion;
    std::string node_id;
    int priority = 0;
    bool healthy = false;
    NodeState state = NodeState::Backup;
};

struct HeartbeatParseResult {
    std::optional<HeartbeatMessage> message;
    std::string error;
};

[[nodiscard]] std::string serializeHeartbeatMessage(const HeartbeatMessage& message);
[[nodiscard]] HeartbeatParseResult parseHeartbeatMessage(const std::string& payload);
[[nodiscard]] PeerStatus peerStatusFromHeartbeat(const HeartbeatMessage& message);

} // namespace easyfailover
