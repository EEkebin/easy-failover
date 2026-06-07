#include "heartbeat/HeartbeatMessage.hpp"

#include <cstdint>
#include <limits>
#include <sstream>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>

namespace easyfailover {
namespace {

constexpr std::string_view kHeartbeatType = "heartbeat";

[[nodiscard]] std::optional<NodeState> nodeStateFromString(const std::string_view value) {
    if (value == toString(NodeState::Backup)) {
        return NodeState::Backup;
    }
    if (value == toString(NodeState::Candidate)) {
        return NodeState::Candidate;
    }
    if (value == toString(NodeState::Master)) {
        return NodeState::Master;
    }
    if (value == toString(NodeState::Fault)) {
        return NodeState::Fault;
    }

    return std::nullopt;
}

[[nodiscard]] HeartbeatParseResult parseError(std::string error) {
    return HeartbeatParseResult{.message = std::nullopt, .error = std::move(error)};
}

[[nodiscard]] std::optional<std::int64_t> intValue(const toml::table& table,
                                                   const std::string_view key) {
    if (const auto value = table[key].value<std::int64_t>(); value.has_value()) {
        return value;
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> stringValue(const toml::table& table,
                                                     const std::string_view key) {
    if (const auto value = table[key].value<std::string>(); value.has_value()) {
        return value;
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<bool> boolValue(const toml::table& table,
                                            const std::string_view key) {
    if (const auto value = table[key].value<bool>(); value.has_value()) {
        return value;
    }

    return std::nullopt;
}

} // namespace

std::string serializeHeartbeatMessage(const HeartbeatMessage& message) {
    toml::table table{
        {"version", kHeartbeatMessageVersion},
        {"type", std::string{kHeartbeatType}},
        {"node_id", message.node_id},
        {"priority", message.priority},
        {"healthy", message.healthy},
        {"state", std::string{toString(message.state)}},
    };

    std::ostringstream output;
    output << table;
    output << '\n';
    return output.str();
}

HeartbeatParseResult parseHeartbeatMessage(const std::string& payload) {
    toml::table table;
    try {
        table = toml::parse(payload);
    } catch (const toml::parse_error& error) {
        return parseError("failed to parse heartbeat message: " +
                          std::string{error.description()});
    }

    const auto version = intValue(table, "version");
    if (!version.has_value()) {
        return parseError("heartbeat.version must be an integer");
    }
    if (*version != kHeartbeatMessageVersion) {
        return parseError("unsupported heartbeat.version");
    }

    const auto type = stringValue(table, "type");
    if (!type.has_value() || *type != kHeartbeatType) {
        return parseError("heartbeat.type must be 'heartbeat'");
    }

    const auto node_id = stringValue(table, "node_id");
    if (!node_id.has_value() || node_id->empty()) {
        return parseError("heartbeat.node_id must not be empty");
    }

    const auto priority = intValue(table, "priority");
    if (!priority.has_value()) {
        return parseError("heartbeat.priority must be an integer");
    }
    if (*priority <= 0) {
        return parseError("heartbeat.priority must be positive");
    }
    if (*priority > std::numeric_limits<int>::max()) {
        return parseError("heartbeat.priority must be within int range");
    }

    const auto healthy = boolValue(table, "healthy");
    if (!healthy.has_value()) {
        return parseError("heartbeat.healthy must be a boolean");
    }

    const auto state = stringValue(table, "state");
    if (!state.has_value()) {
        return parseError("heartbeat.state must be a string");
    }

    const auto parsed_state = nodeStateFromString(*state);
    if (!parsed_state.has_value()) {
        return parseError("heartbeat.state is invalid");
    }

    return HeartbeatParseResult{.message = HeartbeatMessage{.node_id = *node_id,
                                                            .priority = static_cast<int>(*priority),
                                                            .healthy = *healthy,
                                                            .state = *parsed_state},
                                .error = ""};
}

PeerStatus peerStatusFromHeartbeat(const HeartbeatMessage& message) {
    return PeerStatus{.node_id = message.node_id,
                      .priority = message.priority,
                      .healthy = message.healthy,
                      .heartbeat_seen = true,
                      .state = message.state};
}

} // namespace easyfailover
