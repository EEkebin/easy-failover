#include "api/LocalApi.hpp"

#include "config/Config.hpp"
#include "core/NodeState.hpp"
#include "health/HealthCheck.hpp"
#include "runtime/DaemonRuntime.hpp"
#include "runtime/RuntimeLog.hpp"

#include <cstdint>
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

[[nodiscard]] LocalApiEventField stringField(std::string name, std::string value) {
    return LocalApiEventField{.name = std::move(name), .value = std::move(value)};
}

[[nodiscard]] LocalApiEventField boolField(std::string name, const bool value) {
    return LocalApiEventField{.name = std::move(name), .value = value};
}

[[nodiscard]] LocalApiEventField intField(std::string name, const std::int64_t value) {
    return LocalApiEventField{.name = std::move(name), .value = value};
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
                                 .local_owner = lifecycle.local_vip_owner},
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
        .mutation_safety =
            LocalApiConfigMutationSafety{
                .allow_network_mutation = config.mutation_safety.allow_network_mutation},
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

LocalApiEventsResponse buildLocalApiEventsResponse() {
    return LocalApiEventsResponse{};
}

LocalApiEventsResponse buildLocalApiEventsResponse(std::vector<LocalApiEvent> events) {
    return LocalApiEventsResponse{.events = std::move(events)};
}

LocalApiEvent buildLocalApiLifecycleEvent(const std::uint64_t sequence,
                                          const DaemonLifecycleResult& result,
                                          const RuntimeLogContext& context) {
    return LocalApiEvent{
        .sequence = sequence,
        .event = "daemon_lifecycle_result",
        .level = "info",
        .message = formatRuntimeLifecycleEvent(result, context),
        .fields =
            {
                stringField("node_id", std::string{context.node_id}),
                stringField("initial_state", std::string{toString(result.initial_state)}),
                stringField("final_state", std::string{toString(result.final_state)}),
                boolField("started", result.started),
                boolField("iteration_ran", result.iteration_ran),
                boolField("stopped", result.stopped),
                boolField("dry_run", context.dry_run),
                intField("validation_errors",
                         static_cast<std::int64_t>(result.validation_errors.size())),
                intField("vip_operations", static_cast<std::int64_t>(result.vip_operations.size())),
                boolField("local_vip_owner_known", result.local_vip_owner_known),
                boolField("local_vip_owner", result.local_vip_owner),
                stringField("local_node_state", std::string{toString(result.local_status.state)}),
                stringField("failover_action",
                            std::string{toString(result.failover_decision.action)}),
                stringField("detail", result.detail),
            }};
}

LocalApiEvent buildLocalApiVipOperationEvent(const std::uint64_t sequence,
                                             const VipOperationResult& result,
                                             const std::size_t zero_based_index) {
    return LocalApiEvent{
        .sequence = sequence,
        .event = "vip_operation",
        .level = "info",
        .message = formatRuntimeVipOperationEvent(result, zero_based_index),
        .fields =
            {
                intField("zero_based_index", static_cast<std::int64_t>(zero_based_index)),
                stringField("operation", std::string{toString(result.request.type)}),
                stringField("address", result.request.address),
                stringField("interface", result.request.interface),
                boolField("request_dry_run", result.request.dry_run),
                boolField("result_dry_run", result.dry_run),
                boolField("success", result.success),
                intField("commands", static_cast<std::int64_t>(result.commands.size())),
            }};
}

} // namespace easyfailover
