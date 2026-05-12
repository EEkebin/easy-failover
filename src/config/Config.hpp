#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace easyfailover {

struct VipConfig {
    std::string address;
    std::string interface;
};

struct HeartbeatConfig {
    std::string bind;
    std::int64_t interval_ms = 0;
    std::int64_t timeout_ms = 0;
};

struct HealthConfig {
    std::string command;
    std::int64_t interval_ms = 0;
    std::int64_t timeout_ms = 0;
};

struct ElectionConfig {
    bool require_quorum = false;
    bool preempt = true;
};

struct ApiConfig {
    bool enabled = false;
    std::string bind;
    bool read_only = true;
};

struct PeerConfig {
    std::string id;
    std::string address;
};

struct Config {
    std::string node_id;
    int priority = 0;
    VipConfig vip;
    HeartbeatConfig heartbeat;
    HealthConfig health;
    ElectionConfig election;
    ApiConfig api;
    std::vector<PeerConfig> peers;

    [[nodiscard]] std::vector<std::string> validate() const;
};

[[nodiscard]] Config loadConfigFromFile(const std::string& path);

} // namespace easyfailover
