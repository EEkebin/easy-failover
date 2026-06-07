#include "core/FailoverDecision.hpp"

#include "core/Election.hpp"

#include <optional>
#include <string>

namespace easyfailover {

int quorumThreshold(const ElectionPolicy& policy) {
    if (policy.quorum_size > 0) {
        return policy.quorum_size;
    }
    const auto size = policy.cluster_size > 0 ? policy.cluster_size : 1;
    return size / 2 + 1;
}

int observedClusterMembers(const std::vector<PeerStatus>& peers) {
    auto observed = 1; // this node
    for (const auto& peer : peers) {
        if (peer.heartbeat_seen) {
            ++observed;
        }
    }
    return observed;
}

namespace {

// node_id of some observed, healthy peer currently advertising itself as master, if any.
[[nodiscard]] std::optional<std::string> healthyPeerMaster(const std::vector<PeerStatus>& peers) {
    for (const auto& peer : peers) {
        if (peer.healthy && peer.heartbeat_seen && peer.state == NodeState::Master) {
            return peer.node_id;
        }
    }
    return std::nullopt;
}

// True when the named peer is an observed, healthy master.
[[nodiscard]] bool peerIsHealthyMaster(const std::vector<PeerStatus>& peers,
                                       const std::string& node_id) {
    for (const auto& peer : peers) {
        if (peer.node_id == node_id && peer.healthy && peer.heartbeat_seen &&
            peer.state == NodeState::Master) {
            return true;
        }
    }
    return false;
}

} // namespace

FailoverDecision decideFailoverAction(const LocalNodeStatus& local,
                                      const std::vector<PeerStatus>& peers,
                                      const ElectionPolicy& policy) {
    if (!local.healthy) {
        return FailoverDecision{.action = FailoverAction::EnterFault,
                                .selected_master = std::nullopt,
                                .reason = "local node is unhealthy"};
    }

    // Quorum gate: a node may not own the VIP unless it can observe enough of the cluster. A node
    // that holds the VIP but loses quorum demotes and self-releases (fencing on isolation).
    if (policy.require_quorum) {
        const auto observed = observedClusterMembers(peers);
        const auto threshold = quorumThreshold(policy);
        if (observed < threshold) {
            const auto suffix = " (observed=" + std::to_string(observed) +
                                ", need=" + std::to_string(threshold) + ")";
            if (local.state == NodeState::Master) {
                return FailoverDecision{.action = FailoverAction::BecomeBackup,
                                        .selected_master = std::nullopt,
                                        .reason = "quorum lost; releasing VIP" + suffix};
            }
            return FailoverDecision{.action = FailoverAction::StayBackup,
                                    .selected_master = std::nullopt,
                                    .reason = "no quorum; refusing to claim VIP" + suffix};
        }
    }

    std::vector<CandidateNode> candidates;
    candidates.emplace_back(
        CandidateNode{.node_id = local.node_id, .priority = local.priority, .healthy = true});

    for (const auto& peer : peers) {
        if (!peer.healthy || !peer.heartbeat_seen) {
            continue;
        }

        candidates.emplace_back(
            CandidateNode{.node_id = peer.node_id, .priority = peer.priority, .healthy = true});
    }

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    const auto& selected = winner.value();

    if (selected.node_id == local.node_id) {
        if (local.state == NodeState::Master) {
            return FailoverDecision{.action = FailoverAction::StayMaster,
                                    .selected_master = selected.node_id,
                                    .reason = "local node remains highest-priority healthy candidate"};
        }

        // Non-preemptive: do not displace an existing healthy master even though this node now
        // outranks it. A failed master stops sending heartbeats, after which this node takes over.
        if (!policy.preempt) {
            if (const auto master_id = healthyPeerMaster(peers); master_id.has_value()) {
                return FailoverDecision{.action = FailoverAction::StayBackup,
                                        .selected_master = master_id,
                                        .reason = "preempt disabled; deferring to existing healthy master"};
            }
        }

        return FailoverDecision{.action = FailoverAction::BecomeMaster,
                                .selected_master = selected.node_id,
                                .reason = "local node is highest-priority healthy candidate"};
    }

    // A peer is the highest-priority healthy candidate.
    if (local.state == NodeState::Master) {
        // Non-preemptive: an incumbent healthy master keeps the VIP rather than hand it to a
        // higher-priority peer. It still yields if that peer is itself already master (so a
        // post-partition dual master resolves deterministically to the higher-priority node), and
        // always yields under preemption. Health loss and quorum loss are handled earlier.
        if (!policy.preempt && !peerIsHealthyMaster(peers, selected.node_id)) {
            return FailoverDecision{.action = FailoverAction::StayMaster,
                                    .selected_master = local.node_id,
                                    .reason = "preempt disabled; incumbent master retains VIP"};
        }

        return FailoverDecision{.action = FailoverAction::BecomeBackup,
                                .selected_master = selected.node_id,
                                .reason = "peer is highest-priority healthy candidate"};
    }

    return FailoverDecision{.action = FailoverAction::StayBackup,
                            .selected_master = selected.node_id,
                            .reason = "peer remains highest-priority healthy candidate"};
}

} // namespace easyfailover
