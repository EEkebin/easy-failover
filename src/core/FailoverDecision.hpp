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
    // Last state this peer advertised over heartbeat. Used to honor non-preemptive election
    // (do not displace an existing healthy master when election.preempt is false).
    NodeState state = NodeState::Backup;
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

// Runtime election/quorum knobs derived from ElectionConfig plus the configured cluster size.
// A default-constructed policy disables quorum and preempts (the pre-quorum behavior).
struct ElectionPolicy {
    bool require_quorum = false;
    int quorum_size = 0;   // 0 = automatic strict majority
    int cluster_size = 1;  // total configured nodes = peers + self
    bool preempt = true;   // when false, do not displace an existing healthy master
};

// Strict-majority threshold for a cluster of `cluster_size` nodes: floor(N/2) + 1.
[[nodiscard]] int quorumThreshold(const ElectionPolicy& policy);

// Number of cluster members this node can currently observe: itself plus peers with a fresh
// heartbeat. Reachability (not health) — a reachable-but-unhealthy peer still proves no partition.
[[nodiscard]] int observedClusterMembers(const std::vector<PeerStatus>& peers);

[[nodiscard]] FailoverDecision decideFailoverAction(const LocalNodeStatus& local,
                                                    const std::vector<PeerStatus>& peers);

[[nodiscard]] FailoverDecision decideFailoverAction(const LocalNodeStatus& local,
                                                    const std::vector<PeerStatus>& peers,
                                                    const ElectionPolicy& policy);

} // namespace easyfailover
