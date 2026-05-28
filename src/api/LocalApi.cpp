#include "api/LocalApi.hpp"

#include "config/Config.hpp"
#include "core/NodeState.hpp"
#include "health/HealthCheck.hpp"
#include "runtime/DaemonRuntime.hpp"

#include <exception>
#include <utility>
#include <vector>

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

LocalApiConfigResponse buildLocalApiConfigResponse(const Config& config) {
    auto peers = std::vector<LocalApiConfigPeer>{};
    peers.reserve(config.peers.size());
    for (const auto& peer : config.peers) {
        peers.push_back(LocalApiConfigPeer{.id = peer.id, .address = peer.address});
    }

    return LocalApiConfigResponse{
        .node_id = config.node_id,
        .priority = config.priority,
        .vip = LocalApiConfigVip{.address = config.vip.address,
                                 .interface = config.vip.interface},
        .heartbeat = LocalApiConfigHeartbeat{.bind = config.heartbeat.bind,
                                             .interval_ms = config.heartbeat.interval_ms,
                                             .timeout_ms = config.heartbeat.timeout_ms},
        .health = LocalApiConfigHealth{.command_redacted = !config.health.command.empty(),
                                       .interval_ms = config.health.interval_ms,
                                       .timeout_ms = config.health.timeout_ms},
        .election = LocalApiConfigElection{.require_quorum = config.election.require_quorum,
                                           .preempt = config.election.preempt},
        .api = LocalApiConfigApi{.enabled = config.api.enabled,
                                 .bind = config.api.bind,
                                 .read_only = config.api.read_only},
        .peers = std::move(peers)};
}

LocalApiConfigValidateResponse buildLocalApiConfigValidateResponse(
    const LocalApiConfigValidateRequest& request) {
    if (request.format != "toml") {
        return LocalApiConfigValidateResponse{
            .outcome = LocalApiConfigValidateOutcome::RequestError,
            .valid = false,
            .errors = {},
            .error_code = "unsupported_format",
            .error_message = "config validation format must be toml"};
    }

    if (request.config.empty()) {
        return LocalApiConfigValidateResponse{.outcome = LocalApiConfigValidateOutcome::RequestError,
                                              .valid = false,
                                              .errors = {},
                                              .error_code = "missing_config",
                                              .error_message = "config must not be empty"};
    }

    try {
        const auto candidate = loadConfigFromTomlString(request.config);
        auto errors = candidate.validate();
        return LocalApiConfigValidateResponse{.outcome = LocalApiConfigValidateOutcome::Completed,
                                              .valid = errors.empty(),
                                              .errors = std::move(errors),
                                              .error_code = {},
                                              .error_message = {}};
    } catch (const ConfigParseError& error) {
        return LocalApiConfigValidateResponse{.outcome = LocalApiConfigValidateOutcome::RequestError,
                                              .valid = false,
                                              .errors = {},
                                              .error_code = "invalid_toml",
                                              .error_message = error.what()};
    } catch (const ConfigDecodeError& error) {
        return LocalApiConfigValidateResponse{.outcome = LocalApiConfigValidateOutcome::RequestError,
                                              .valid = false,
                                              .errors = {},
                                              .error_code = "invalid_config_shape",
                                              .error_message = error.what()};
    } catch (const std::exception& error) {
        return LocalApiConfigValidateResponse{.outcome = LocalApiConfigValidateOutcome::RequestError,
                                              .valid = false,
                                              .errors = {},
                                              .error_code = "validation_failed",
                                              .error_message = error.what()};
    }
}

} // namespace easyfailover
