#pragma once

#include <string_view>

namespace easyfailover {

enum class NodeState { Backup, Candidate, Master, Fault };

[[nodiscard]] constexpr std::string_view toString(const NodeState state) {
    switch (state) {
    case NodeState::Backup:
        return "backup";
    case NodeState::Candidate:
        return "candidate";
    case NodeState::Master:
        return "master";
    case NodeState::Fault:
        return "fault";
    }

    return "unknown";
}

} // namespace easyfailover
