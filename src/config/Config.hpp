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
    std::string bind = "0.0.0.0:7432";
    std::int64_t interval_ms = 1000;
    std::int64_t timeout_ms = 3000;
};

struct HealthConfig {
    std::string command;
    std::int64_t interval_ms = 1000;
    std::int64_t timeout_ms = 2000;
};

struct ElectionConfig {
    bool require_quorum = false;
    bool preempt = true;
};

struct ApiConfig {
    bool enabled = false;
    std::string bind = "127.0.0.1:8743";
    bool read_only = true;
};

struct PeerConfig {
    std::string id;
    std::string address;
};

struct Config {
    std::string node_id;
    int priority = 100;
    VipConfig vip;
    HeartbeatConfig heartbeat;
    HealthConfig health;
    ElectionConfig election;
    ApiConfig api;
    std::vector<PeerConfig> peers;

    [[nodiscard]] std::vector<std::string> validate() const;
};

[[nodiscard]] Config loadConfigFromFile(const std::string& path);
[[nodiscard]] Config loadConfigFromTomlString(const std::string& content);

} // namespace easyfailover
