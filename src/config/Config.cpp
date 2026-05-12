#include "config/Config.hpp"

#include <stdexcept>
#include <string_view>

#include <toml++/toml.hpp>

namespace easyfailover {

namespace {

std::string requiredString(const toml::table& table, const std::string_view key) {
    return table[key].value_or(std::string{});
}

std::int64_t requiredInt(const toml::table& table, const std::string_view key) {
    return table[key].value_or<std::int64_t>(0);
}

bool optionalBool(const toml::table& table, const std::string_view key, const bool fallback) {
    return table[key].value_or(fallback);
}

const toml::table& requiredTable(const toml::table& root, const std::string_view key) {
    const auto* table = root[key].as_table();
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
    if (heartbeat.interval_ms <= 0) {
        errors.emplace_back("heartbeat.interval_ms must be positive");
    }
    if (heartbeat.timeout_ms <= 0) {
        errors.emplace_back("heartbeat.timeout_ms must be positive");
    }
    for (const auto& peer : peers) {
        if (peer.id.empty()) {
            errors.emplace_back("peers[].id must not be empty");
        }
        if (peer.address.empty()) {
            errors.emplace_back("peers[].address must not be empty");
        }
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
    config.node_id = requiredString(root, "node_id");
    config.priority = static_cast<int>(requiredInt(root, "priority"));

    const auto& vip = requiredTable(root, "vip");
    config.vip.address = requiredString(vip, "address");
    config.vip.interface = requiredString(vip, "interface");

    const auto& heartbeat = requiredTable(root, "heartbeat");
    config.heartbeat.bind = requiredString(heartbeat, "bind");
    config.heartbeat.interval_ms = requiredInt(heartbeat, "interval_ms");
    config.heartbeat.timeout_ms = requiredInt(heartbeat, "timeout_ms");

    const auto& health = requiredTable(root, "health");
    config.health.command = requiredString(health, "command");
    config.health.interval_ms = requiredInt(health, "interval_ms");
    config.health.timeout_ms = requiredInt(health, "timeout_ms");

    const auto& election = requiredTable(root, "election");
    config.election.require_quorum = optionalBool(election, "require_quorum", false);
    config.election.preempt = optionalBool(election, "preempt", true);

    const auto& api = requiredTable(root, "api");
    config.api.enabled = optionalBool(api, "enabled", false);
    config.api.bind = requiredString(api, "bind");
    config.api.read_only = optionalBool(api, "read_only", true);

    if (const auto* peers = root["peers"].as_array(); peers != nullptr) {
        for (const auto& peer_node : *peers) {
            const auto* peer_table = peer_node.as_table();
            if (peer_table == nullptr) {
                throw std::runtime_error("Each peers entry must be a TOML table");
            }

            PeerConfig peer;
            peer.id = requiredString(*peer_table, "id");
            peer.address = requiredString(*peer_table, "address");
            config.peers.emplace_back(std::move(peer));
        }
    }

    return config;
}

} // namespace easyfailover
