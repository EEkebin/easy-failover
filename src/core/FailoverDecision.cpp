#include "core/FailoverDecision.hpp"

#include "core/Election.hpp"

namespace easyfailover {

FailoverDecision decideFailoverAction(const LocalNodeStatus& local,
                                      const std::vector<PeerStatus>& peers) {
    if (!local.healthy) {
        return FailoverDecision{.action = FailoverAction::EnterFault,
                                .selected_master = std::nullopt,
                                .reason = "local node is unhealthy"};
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
    if (!winner.has_value()) {
        return FailoverDecision{.action = FailoverAction::EnterFault,
                                .selected_master = std::nullopt,
                                .reason = "no healthy failover candidates"};
    }

    if (winner->node_id == local.node_id) {
        if (local.state == NodeState::Master) {
            return FailoverDecision{.action = FailoverAction::StayMaster,
                                    .selected_master = winner->node_id,
                                    .reason = "local node remains highest-priority healthy candidate"};
        }

        return FailoverDecision{.action = FailoverAction::BecomeMaster,
                                .selected_master = winner->node_id,
                                .reason = "local node is highest-priority healthy candidate"};
    }

    if (local.state == NodeState::Master) {
        return FailoverDecision{.action = FailoverAction::BecomeBackup,
                                .selected_master = winner->node_id,
                                .reason = "peer is highest-priority healthy candidate"};
    }

    return FailoverDecision{.action = FailoverAction::StayBackup,
                            .selected_master = winner->node_id,
                            .reason = "peer remains highest-priority healthy candidate"};
}

} // namespace easyfailover
