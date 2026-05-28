#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace easyfailover {

struct ApiConfig;
struct Config;
struct DaemonLifecycleResult;
struct HealthCheckResult;
enum class NodeState;

enum class LocalApiStartupState {
    Disabled,
    Ready,
    Rejected,
};

struct LocalApiStartupResult {
    LocalApiStartupState state = LocalApiStartupState::Disabled;
    std::string bind;
    std::string detail;
};

struct LocalApiStatusNode {
    std::string id;
    int priority = 0;
    std::string state;
    bool healthy = false;
};

struct LocalApiStatusVip {
    std::string address;
    std::string interface;
    bool local_owner = false;
};

struct LocalApiStatusLifecycle {
    std::string initial_state;
    std::string final_state;
    std::string detail;
    bool dry_run = true;
    bool started = false;
    bool iteration_ran = false;
    bool stopped = false;
};

struct LocalApiStatusHeartbeat {
    std::string bind;
    std::int64_t interval_ms = 0;
    std::int64_t timeout_ms = 0;
    int peers_observed = 0;
};

struct LocalApiStatusHealth {
    std::string status;
    std::string detail;
};

struct LocalApiStatusResponse {
    LocalApiStatusNode node;
    LocalApiStatusVip vip;
    LocalApiStatusLifecycle lifecycle;
    LocalApiStatusHeartbeat heartbeat;
    LocalApiStatusHealth health;
};

[[nodiscard]] constexpr std::string_view toString(const LocalApiStartupState state) {
    switch (state) {
    case LocalApiStartupState::Disabled:
        return "disabled";
    case LocalApiStartupState::Ready:
        return "ready";
    case LocalApiStartupState::Rejected:
        return "rejected";
    }

    return "unknown";
}

[[nodiscard]] LocalApiStartupResult evaluateLocalApiStartup(const ApiConfig& config);

[[nodiscard]] LocalApiStatusResponse buildLocalApiStatusResponse(
    const Config& config,
    const DaemonLifecycleResult& lifecycle,
    const HealthCheckResult& health,
    NodeState local_node_state,
    bool dry_run,
    int peers_observed = 0);

} // namespace easyfailover
