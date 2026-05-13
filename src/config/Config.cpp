#include "config/Config.hpp"

#include "platform/Hostname.hpp"

#include <optional>
#include <stdexcept>
#include <string_view>

#include <toml++/toml.hpp>

namespace easyfailover {

namespace {

template <typename T>
std::optional<T> typedValueIfPresent(const toml::table& table, const std::string_view key) {
    const auto* node = table.get(key);
    if (node == nullptr) {
        return std::nullopt;
    }

    const auto value = node->value<T>();
    if (!value.has_value()) {
        throw std::runtime_error("Invalid type for config key: " + std::string{key});
    }

    return value;
}

std::string stringValueOrEmpty(const toml::table& table, const std::string_view key) {
    return typedValueIfPresent<std::string>(table, key).value_or(std::string{});
}

std::string optionalString(const toml::table& table, const std::string_view key,
                           const std::string& fallback) {
    return typedValueIfPresent<std::string>(table, key).value_or(fallback);
}

std::int64_t optionalInt(const toml::table& table, const std::string_view key,
                         const std::int64_t fallback) {
    return typedValueIfPresent<std::int64_t>(table, key).value_or(fallback);
}

bool optionalBool(const toml::table& table, const std::string_view key, const bool fallback) {
    return typedValueIfPresent<bool>(table, key).value_or(fallback);
}

const toml::table* optionalTable(const toml::table& root, const std::string_view key) {
    const auto* node = root.get(key);
    if (node == nullptr) {
        return nullptr;
    }

    const auto* table = node->as_table();
    if (table == nullptr) {
        throw std::runtime_error("Invalid type for TOML table: " + std::string{key});
    }

    return table;
}

const toml::table& requiredTable(const toml::table& root, const std::string_view key) {
    const auto* table = optionalTable(root, key);
    if (table == nullptr) {
        throw std::runtime_error("Missing required TOML table: " + std::string{key});
    }
    return *table;
}

} // namespace

std::vector<std::string> Config::validate() const {
    std::vector<std::string> errors;

    if (node_id.empty()) {
        errors.emplace_back("node_id must not be empty");
    }
    if (priority <= 0) {
        errors.emplace_back("priority must be positive");
    }
    if (vip.address.empty()) {
        errors.emplace_back("vip.address must not be empty");
    }
    if (vip.interface.empty()) {
        errors.emplace_back("vip.interface must not be empty");
    }
    if (heartbeat.bind.empty()) {
        errors.emplace_back("heartbeat.bind must not be empty");
    }
    if (heartbeat.interval_ms <= 0) {
        errors.emplace_back("heartbeat.interval_ms must be positive");
    }
    if (heartbeat.timeout_ms <= 0) {
        errors.emplace_back("heartbeat.timeout_ms must be positive");
    }
    if (health.interval_ms <= 0) {
        errors.emplace_back("health.interval_ms must be positive");
    }
    if (health.timeout_ms <= 0) {
        errors.emplace_back("health.timeout_ms must be positive");
    }
    for (const auto& peer : peers) {
        if (peer.id.empty()) {
            errors.emplace_back("peers[].id must not be empty");
        }
        if (peer.address.empty()) {
            errors.emplace_back("peers[].address must not be empty");
        }
    }
    if (peers.empty()) {
        errors.emplace_back("at least one peer must be configured");
    }
    if (api.enabled && api.bind.empty()) {
        errors.emplace_back("api.bind must not be empty when api.enabled is true");
    }

    return errors;
}

Config loadConfigFromFile(const std::string& path) {
    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& error) {
        throw std::runtime_error("Failed to parse config '" + path + "': " +
                                 std::string{error.description()});
    }

    Config config;
    config.node_id = getSystemHostname().value_or(std::string{});
    config.node_id = optionalString(root, "node_id", config.node_id);
    config.priority = static_cast<int>(optionalInt(root, "priority", config.priority));

    const auto& vip = requiredTable(root, "vip");
    config.vip.address = stringValueOrEmpty(vip, "address");
    config.vip.interface = stringValueOrEmpty(vip, "interface");

    if (const auto* heartbeat = optionalTable(root, "heartbeat"); heartbeat != nullptr) {
        config.heartbeat.bind = optionalString(*heartbeat, "bind", config.heartbeat.bind);
        config.heartbeat.interval_ms =
            optionalInt(*heartbeat, "interval_ms", config.heartbeat.interval_ms);
        config.heartbeat.timeout_ms =
            optionalInt(*heartbeat, "timeout_ms", config.heartbeat.timeout_ms);
    }

    if (const auto* health = optionalTable(root, "health"); health != nullptr) {
        config.health.command = optionalString(*health, "command", config.health.command);
        config.health.interval_ms = optionalInt(*health, "interval_ms", config.health.interval_ms);
        config.health.timeout_ms = optionalInt(*health, "timeout_ms", config.health.timeout_ms);
    }

    if (const auto* election = optionalTable(root, "election"); election != nullptr) {
        config.election.require_quorum =
            optionalBool(*election, "require_quorum", config.election.require_quorum);
        config.election.preempt = optionalBool(*election, "preempt", config.election.preempt);
    }

    if (const auto* api = optionalTable(root, "api"); api != nullptr) {
        config.api.enabled = optionalBool(*api, "enabled", config.api.enabled);
        config.api.bind = optionalString(*api, "bind", config.api.bind);
        config.api.read_only = optionalBool(*api, "read_only", config.api.read_only);
    }

    if (const auto* peers = root["peers"].as_array(); peers != nullptr) {
        for (const auto& peer_node : *peers) {
            const auto* peer_table = peer_node.as_table();
            if (peer_table == nullptr) {
                throw std::runtime_error("Each peers entry must be a TOML table");
            }

            PeerConfig peer;
            peer.id = stringValueOrEmpty(*peer_table, "id");
            peer.address = stringValueOrEmpty(*peer_table, "address");
            config.peers.emplace_back(std::move(peer));
        }
    }

    return config;
}

} // namespace easyfailover
