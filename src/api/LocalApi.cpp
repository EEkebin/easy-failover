#include "api/LocalApi.hpp"

#include "config/Config.hpp"
#include "core/NodeState.hpp"
#include "health/HealthCheck.hpp"
#include "runtime/DaemonRuntime.hpp"

namespace easyfailover {

namespace {

[[nodiscard]] std::string healthDetailForStatusResponse(const Config& config,
                                                        const HealthCheckResult& health) {
    if (config.health.command.empty()) {
        return health.detail;
    }

    return "health command detail redacted";
}

} // namespace

LocalApiStartupResult evaluateLocalApiStartup(const ApiConfig& config) {
    if (!config.enabled) {
        return LocalApiStartupResult{.state = LocalApiStartupState::Disabled,
                                     .bind = config.bind,
                                     .detail = "local API disabled"};
    }

    if (!config.read_only) {
        return LocalApiStartupResult{
            .state = LocalApiStartupState::Rejected,
            .bind = config.bind,
            .detail = "local API write mode requires authentication and write-behavior design"};
    }

    return LocalApiStartupResult{.state = LocalApiStartupState::Ready,
                                 .bind = config.bind,
                                 .detail = "local API skeleton ready; listener not implemented"};
}

LocalApiStatusResponse buildLocalApiStatusResponse(const Config& config,
                                                   const DaemonLifecycleResult& lifecycle,
                                                   const HealthCheckResult& health,
                                                   const NodeState local_node_state,
                                                   const bool dry_run,
                                                   const int peers_observed) {
    return LocalApiStatusResponse{
        .node = LocalApiStatusNode{.id = config.node_id,
                                   .priority = config.priority,
                                   .state = std::string{toString(local_node_state)},
                                   .healthy = health.status == HealthStatus::Healthy},
        .vip = LocalApiStatusVip{.address = config.vip.address,
                                 .interface = config.vip.interface,
                                 .local_owner = false},
        .lifecycle = LocalApiStatusLifecycle{
            .initial_state = std::string{toString(lifecycle.initial_state)},
            .final_state = std::string{toString(lifecycle.final_state)},
            .detail = lifecycle.detail,
            .dry_run = dry_run,
            .started = lifecycle.started,
            .iteration_ran = lifecycle.iteration_ran,
            .stopped = lifecycle.stopped},
        .heartbeat = LocalApiStatusHeartbeat{.bind = config.heartbeat.bind,
                                             .interval_ms = config.heartbeat.interval_ms,
                                             .timeout_ms = config.heartbeat.timeout_ms,
                                             .peers_observed = peers_observed},
        .health = LocalApiStatusHealth{.status = std::string{toString(health.status)},
                                       .detail = healthDetailForStatusResponse(config, health)}};
}

} // namespace easyfailover
