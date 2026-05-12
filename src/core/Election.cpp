#include "core/Election.hpp"

#include <algorithm>

namespace easyfailover {

std::optional<CandidateNode> chooseHighestPriorityHealthyNode(
    const std::vector<CandidateNode>& candidates) {
    // TODO: Add quorum and split-brain protection before this controls real VIP ownership.
    // TODO: Account for observed heartbeat state and fencing when a real consensus model exists.
    const auto winner = std::max_element(
        candidates.begin(), candidates.end(), [](const CandidateNode& left, const CandidateNode& right) {
            if (!left.healthy && right.healthy) {
                return true;
            }
            if (left.healthy && !right.healthy) {
                return false;
            }
            if (!left.healthy && !right.healthy) {
                return false;
            }
            if (left.priority != right.priority) {
                return left.priority < right.priority;
            }
            return left.node_id > right.node_id;
        });

    if (winner == candidates.end() || !winner->healthy) {
        return std::nullopt;
    }

    return *winner;
}

} // namespace easyfailover
