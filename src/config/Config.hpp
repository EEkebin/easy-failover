#pragma once

#include <cstdint>
#include <stdexcept>
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
    // Minimum nodes (this node + observed peers) that must be visible before this node may own the
    // VIP. 0 = automatic strict majority (floor(N/2)+1, where N = peers + 1). Only enforced when
    // require_quorum is true.
    int quorum_size = 0;
};

struct ApiConfig {
    bool enabled = false;
    std::string bind = "127.0.0.1:8743";
    bool read_only = true;
    std::string auth_token_file;
};

struct MutationSafetyConfig {
    bool allow_network_mutation = false;
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
    MutationSafetyConfig mutation_safety;
    std::vector<PeerConfig> peers;

    [[nodiscard]] std::vector<std::string> validate() const;
};

class ConfigParseError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class ConfigDecodeError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] Config loadConfigFromFile(const std::string& path);
[[nodiscard]] Config loadConfigFromTomlString(const std::string& content);

} // namespace easyfailover
