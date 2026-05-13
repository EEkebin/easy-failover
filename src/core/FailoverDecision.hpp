#pragma once

#include "core/NodeState.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace easyfailover {

struct LocalNodeStatus {
    std::string node_id;
    int priority = 0;
    bool healthy = false;
    NodeState state = NodeState::Backup;
};

struct PeerStatus {
    std::string node_id;
    int priority = 0;
    bool healthy = false;
    bool heartbeat_seen = false;
};

enum class FailoverAction { StayBackup, BecomeMaster, StayMaster, BecomeBackup, EnterFault };

[[nodiscard]] constexpr std::string_view toString(const FailoverAction action) {
    switch (action) {
    case FailoverAction::StayBackup:
        return "stay_backup";
    case FailoverAction::BecomeMaster:
        return "become_master";
    case FailoverAction::StayMaster:
        return "stay_master";
    case FailoverAction::BecomeBackup:
        return "become_backup";
    case FailoverAction::EnterFault:
        return "enter_fault";
    }

    return "unknown";
}

struct FailoverDecision {
    FailoverAction action = FailoverAction::StayBackup;
    std::optional<std::string> selected_master;
    std::string reason;
};

[[nodiscard]] FailoverDecision decideFailoverAction(const LocalNodeStatus& local,
                                                    const std::vector<PeerStatus>& peers);

} // namespace easyfailover
