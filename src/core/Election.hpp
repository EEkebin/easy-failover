#pragma once

#include <optional>
#include <string>
#include <vector>

namespace easyfailover {

struct CandidateNode {
    std::string node_id;
    int priority = 0;
    bool healthy = false;
};

[[nodiscard]] std::optional<CandidateNode> chooseHighestPriorityHealthyNode(
    const std::vector<CandidateNode>& candidates);

} // namespace easyfailover
