#include "runtime/DaemonRuntime.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <thread>
#include <utility>

namespace easyfailover {

namespace {

[[nodiscard]] bool recordVipOperation(DaemonLifecycleResult& lifecycle_result,
                                      const VipOperationResult& operation_result,
                                      const bool require_dry_run) {
    lifecycle_result.vip_operations.push_back(operation_result);
    if (require_dry_run && !operation_result.dry_run) {
        lifecycle_result.final_state = DaemonLifecycleState::Faulted;
        lifecycle_result.detail = "dry-run lifecycle received non-dry-run VIP operation";
        return false;
    }

    if (!operation_result.success) {
        lifecycle_result.final_state = DaemonLifecycleState::Faulted;
        lifecycle_result.detail =
            operation_result.error.empty()
                ? (require_dry_run ? "dry-run lifecycle VIP operation failed"
                                   : "lifecycle VIP operation failed")
                : operation_result.error;
        return false;
    }

    return true;
}

template <typename T>
void pushObservation(std::vector<T>& observations,
                     T observation,
                     const std::size_t max_recorded_observations) {
    observations.push_back(std::move(observation));
    if (max_recorded_observations > 0 && observations.size() > max_recorded_observations) {
        observations.erase(observations.begin());
    }
}

template <typename T>
void appendObservations(std::vector<T>& observations,
                        const std::vector<T>& new_observations,
                        const std::size_t max_recorded_observations) {
    for (const auto& observation : new_observations) {
        pushObservation(observations, observation, max_recorded_observations);
    }
}

void publishLoopResult(const std::function<void(const DaemonLoopResult&)>& observer,
                       const DaemonLoopResult& result) {
    if (observer) {
        observer(result);
    }
}

[[nodiscard]] HealthScheduleObservation evaluateHealthSchedule(
    const Config& config,
    const std::size_t iteration_index,
    const std::int64_t elapsed_ms,
    const bool health_check_was_due,
    const std::int64_t last_due_elapsed_ms) {
    const auto due =
        !health_check_was_due || elapsed_ms - last_due_elapsed_ms >= config.health.interval_ms;
    return HealthScheduleObservation{.iteration_index = iteration_index,
                                     .elapsed_ms = elapsed_ms,
                                     .interval_ms = config.health.interval_ms,
                                     .due = due,
                                     .command_configured = !config.health.command.empty()};
}

[[nodiscard]] HeartbeatSendScheduleObservation evaluateHeartbeatSendSchedule(
    const Config& config,
    const std::size_t iteration_index,
    const std::int64_t elapsed_ms,
    const bool heartbeat_send_was_due,
    const std::int64_t last_due_elapsed_ms) {
    const auto due =
        !heartbeat_send_was_due || elapsed_ms - last_due_elapsed_ms >= config.heartbeat.interval_ms;
    return HeartbeatSendScheduleObservation{.iteration_index = iteration_index,
                                            .elapsed_ms = elapsed_ms,
                                            .interval_ms = config.heartbeat.interval_ms,
                                            .due = due,
                                            .peers_configured = !config.peers.empty(),
                                            .expected_send_count = config.peers.size()};
}

struct RecentPeerStatus {
    PeerStatus status;
    std::int64_t last_seen_elapsed_ms = 0;
};

[[nodiscard]] bool peerIsConfigured(const Config& config, const std::string& node_id) {
    return std::any_of(config.peers.begin(), config.peers.end(),
                       [&node_id](const PeerConfig& peer) { return peer.id == node_id; });
}

void expirePeers(std::map<std::string, RecentPeerStatus>& recent_peers,
                 const std::int64_t elapsed_ms,
                 const std::int64_t timeout_ms) {
    for (auto iter = recent_peers.begin(); iter != recent_peers.end();) {
        if (elapsed_ms - iter->second.last_seen_elapsed_ms >= timeout_ms) {
            iter = recent_peers.erase(iter);
        } else {
            ++iter;
        }
    }
}

[[nodiscard]] std::vector<PeerStatus> activePeerStatuses(
    const std::map<std::string, RecentPeerStatus>& recent_peers) {
    auto peers = std::vector<PeerStatus>{};
    peers.reserve(recent_peers.size());
    for (const auto& [_, recent] : recent_peers) {
        peers.push_back(recent.status);
    }

    return peers;
}

[[nodiscard]] HeartbeatReceiveStateObservation heartbeatReceiveStateFromLoopResult(
    const std::size_t iteration_index,
    const std::int64_t elapsed_ms,
    const HeartbeatLoopResult& heartbeat_result) {
    return HeartbeatReceiveStateObservation{.iteration_index = iteration_index,
                                            .elapsed_ms = elapsed_ms,
                                            .receive_attempted =
                                                heartbeat_result.receive.attempted,
                                            .timed_out = heartbeat_result.receive.timed_out,
                                            .timeout_ms = heartbeat_result.receive.timeout_ms,
                                            .peer_address =
                                                heartbeat_result.receive.peer_address,
                                            .peer_status =
                                                heartbeat_result.receive.peer_status,
                                            .error = heartbeat_result.receive.error};
}

[[nodiscard]] LocalNodeStatus localStatusFromOwnership(const Config& config,
                                                       const bool local_healthy,
                                                       const bool local_vip_owner) {
    return LocalNodeStatus{.node_id = config.node_id,
                           .priority = config.priority,
                           .healthy = local_healthy,
                           .state = local_vip_owner ? NodeState::Master : NodeState::Backup};
}

[[nodiscard]] bool wantsMaster(const FailoverAction action) {
    return action == FailoverAction::BecomeMaster || action == FailoverAction::StayMaster;
}

[[nodiscard]] bool wantsBackup(const FailoverAction action) {
    return action == FailoverAction::BecomeBackup || action == FailoverAction::StayBackup;
}

[[nodiscard]] std::string lifecycleDetailForOperations(
    const DaemonLifecycleRequest& request,
    const std::vector<VipOperationResult>& operations) {
    const auto all_operations_dry_run =
        std::all_of(operations.begin(), operations.end(),
                    [](const VipOperationResult& operation) { return operation.dry_run; });
    if (request.options.dry_run) {
        return "dry-run lifecycle iteration completed";
    }
    if (all_operations_dry_run) {
        return "mutation safety gate kept lifecycle VIP operations in dry-run mode";
    }

    return "real VIP lifecycle iteration completed";
}

[[nodiscard]] ElectionPolicy electionPolicyFromConfig(const Config& config) {
    return ElectionPolicy{.require_quorum = config.election.require_quorum,
                          .quorum_size = config.election.quorum_size,
                          .cluster_size = static_cast<int>(config.peers.size()) + 1,
                          .preempt = config.election.preempt};
}

[[nodiscard]] FailoverDecisionObservation evaluateFailoverDecision(
    const Config& config,
    const bool local_healthy,
    const bool local_vip_owner,
    const HeartbeatReceiveStateObservation& receive_state,
    std::vector<PeerStatus> peer_statuses) {
    auto local_status = localStatusFromOwnership(config, local_healthy, local_vip_owner);

    auto decision =
        decideFailoverAction(local_status, peer_statuses, electionPolicyFromConfig(config));
    return FailoverDecisionObservation{.iteration_index = receive_state.iteration_index,
                                       .elapsed_ms = receive_state.elapsed_ms,
                                       .local_status = std::move(local_status),
                                       .peer_statuses = std::move(peer_statuses),
                                       .decision = std::move(decision)};
}

[[nodiscard]] std::int64_t effectiveIterationElapsedMs(const DaemonLoopOptions& options) {
    const auto elapsed_ms = options.logical_iteration_elapsed_ms > 0
                                ? options.logical_iteration_elapsed_ms
                                : options.inter_iteration_delay_ms;
    return elapsed_ms > 0 ? elapsed_ms : 0;
}

[[nodiscard]] bool realNetworkMutationRequested(const DaemonLoopRequest& request) {
    return !request.options.runtime_options.dry_run &&
           request.config.mutation_safety.allow_network_mutation;
}

[[nodiscard]] bool heartbeatSendFailed(const HeartbeatLoopResult& heartbeat_result) {
    return std::any_of(heartbeat_result.sends.begin(), heartbeat_result.sends.end(),
                       [](const HeartbeatSendObservation& observation) {
                           return !observation.sent || !observation.error.empty();
                       });
}

} // namespace

DaemonLifecycleResult runDaemonLifecycleOnce(const DaemonLifecycleRequest& request,
                                             VipManager& vip_manager,
                                             VipOwnershipProbe& ownership_probe) {
    auto result = DaemonLifecycleResult{.initial_state = request.initial_state,
                                        .final_state = request.initial_state,
                                        .started = false,
                                        .iteration_ran = false,
                                        .stopped = false,
                                        .validation_errors = {},
                                        .vip_operations = {},
                                        .local_vip_owner_known = false,
                                        .local_vip_owner = false,
                                        .local_status = {},
                                        .failover_decision = {},
                                        .detail = ""};

    if (request.initial_state == DaemonLifecycleState::Faulted) {
        result.detail = "daemon lifecycle cannot start from faulted state";
        return result;
    }

    if (!request.config_prevalidated) {
        result.validation_errors = request.config.validate();
        if (!result.validation_errors.empty()) {
            result.final_state = DaemonLifecycleState::Faulted;
            result.detail = "config validation failed";
            return result;
        }
    }

    if (request.shutdown_state != nullptr && request.shutdown_state->shutdownRequested()) {
        result.final_state = DaemonLifecycleState::Stopped;
        result.stopped = true;
        result.detail = std::string{request.shutdown_state->reason()};
        return result;
    }

    result.started = request.initial_state == DaemonLifecycleState::Stopped;
    result.iteration_ran = true;

    const auto require_dry_run =
        request.options.dry_run || !request.config.mutation_safety.allow_network_mutation;

    const auto ownership =
        ownership_probe.probeLocalVip(request.config.vip.address, request.config.vip.interface);
    if (!ownership.success) {
        if (!request.options.dry_run) {
            result.final_state = DaemonLifecycleState::Faulted;
            result.detail =
                ownership.error.empty() ? "VIP ownership probe failed" : ownership.error;
            return result;
        }

        result.local_vip_owner_known = false;
        result.local_vip_owner = false;
    } else {
        result.local_vip_owner_known = true;
        result.local_vip_owner = ownership.local_owner;
    }
    result.local_status = localStatusFromOwnership(request.config, request.local_healthy,
                                                  result.local_vip_owner);
    result.failover_decision = decideFailoverAction(result.local_status, request.peer_statuses,
                                                    electionPolicyFromConfig(request.config));

    if (result.failover_decision.action == FailoverAction::EnterFault) {
        result.final_state = DaemonLifecycleState::Faulted;
        result.detail = result.failover_decision.reason.empty()
                            ? "failover decision entered fault"
                            : result.failover_decision.reason;
        return result;
    }

    if (wantsMaster(result.failover_decision.action) && !result.local_vip_owner) {
        const auto add_result =
            vip_manager.addVip(request.config.vip.address, request.config.vip.interface);
        if (!recordVipOperation(result, add_result, require_dry_run)) {
            return result;
        }

        const auto announce_result =
            vip_manager.announceVip(request.config.vip.address, request.config.vip.interface);
        if (!recordVipOperation(result, announce_result, require_dry_run)) {
            return result;
        }
    } else if (wantsBackup(result.failover_decision.action) && result.local_vip_owner) {
        const auto remove_result =
            vip_manager.removeVip(request.config.vip.address, request.config.vip.interface);
        if (!recordVipOperation(result, remove_result, require_dry_run)) {
            return result;
        }
    }

    result.detail = lifecycleDetailForOperations(request, result.vip_operations);
    result.final_state = DaemonLifecycleState::Stopped;
    result.stopped = true;
    return result;
}

DaemonLoopResult runDaemonRuntimeLoop(
    const DaemonLoopRequest& request,
    VipManager& vip_manager,
    VipOwnershipProbe& ownership_probe,
    HeartbeatTransport& heartbeat_transport,
    const std::function<void(const DaemonLoopResult&)>& result_observer) {
    auto result = DaemonLoopResult{.initial_state = request.initial_state,
                                   .final_state = request.initial_state,
                                   .stop_reason = DaemonLoopStopReason::MaxIterations,
                                   .iterations_ran = 0,
                                   .validation_errors = {},
                                   .vip_operations = {},
                                   .health_schedules = {},
                                   .heartbeat_send_schedules = {},
                                   .heartbeat_sends = {},
                                   .heartbeat_receive_states = {},
                                   .failover_decisions = {},
                                   .detail = "max iterations completed"};

    if (request.shutdown_state != nullptr && request.shutdown_state->shutdownRequested()) {
        result.final_state = DaemonLifecycleState::Stopped;
        result.stop_reason = DaemonLoopStopReason::ShutdownRequested;
        result.detail = std::string{request.shutdown_state->reason()};
        return result;
    }

    if (!request.config_prevalidated) {
        result.validation_errors = request.config.validate();
        if (!result.validation_errors.empty()) {
            result.final_state = DaemonLifecycleState::Faulted;
            result.stop_reason = DaemonLoopStopReason::LifecycleFaulted;
            result.detail = "config validation failed";
            return result;
        }
    }

    auto current_state = request.initial_state;
    auto elapsed_ms = std::int64_t{0};
    const auto iteration_elapsed_ms = effectiveIterationElapsedMs(request.options);
    auto health_check_was_due = false;
    auto last_due_elapsed_ms = std::int64_t{0};
    auto heartbeat_send_was_due = false;
    auto last_heartbeat_send_due_elapsed_ms = std::int64_t{0};
    auto recent_peers = std::map<std::string, RecentPeerStatus>{};
    auto local_status = LocalNodeStatus{.node_id = request.config.node_id,
                                        .priority = request.config.priority,
                                        .healthy = true,
                                        .state = NodeState::Backup};
    auto real_mutation_warmup_complete = false;
    for (std::size_t index = 0;
         request.options.run_until_shutdown || index < request.options.max_iterations; ++index) {
        if (request.shutdown_state != nullptr) {
            pollShutdownSignals(*request.shutdown_state);
        }
        if (request.shutdown_state != nullptr && request.shutdown_state->shutdownRequested()) {
            result.final_state = DaemonLifecycleState::Stopped;
            result.stop_reason = DaemonLoopStopReason::ShutdownRequested;
            result.detail = std::string{request.shutdown_state->reason()};
            return result;
        }

        expirePeers(recent_peers, elapsed_ms, request.config.heartbeat.timeout_ms);

        auto heartbeat_schedule = evaluateHeartbeatSendSchedule(
            request.config, result.iterations_ran, elapsed_ms, heartbeat_send_was_due,
            last_heartbeat_send_due_elapsed_ms);
        auto heartbeat_receive_state =
            HeartbeatReceiveStateObservation{.iteration_index = result.iterations_ran,
                                             .elapsed_ms = elapsed_ms,
                                             .receive_attempted = false,
                                             .timed_out = false,
                                             .timeout_ms = request.config.heartbeat.timeout_ms,
                                             .peer_address = "",
                                             .peer_status = std::nullopt,
                                             .error = ""};
        if (heartbeat_schedule.due) {
            heartbeat_send_was_due = true;
            last_heartbeat_send_due_elapsed_ms = elapsed_ms;

            const auto heartbeat_result = runHeartbeatLoopOnce(
                local_status, request.config.peers, request.config.heartbeat.timeout_ms,
                heartbeat_transport);
            appendObservations(result.heartbeat_sends, heartbeat_result.sends,
                               request.options.max_recorded_observations);
            heartbeat_receive_state = heartbeatReceiveStateFromLoopResult(
                result.iterations_ran, elapsed_ms, heartbeat_result);
            if (realNetworkMutationRequested(request) &&
                (heartbeatSendFailed(heartbeat_result) || !heartbeat_receive_state.error.empty())) {
                pushObservation(result.heartbeat_send_schedules, heartbeat_schedule,
                                request.options.max_recorded_observations);
                pushObservation(result.heartbeat_receive_states, heartbeat_receive_state,
                                request.options.max_recorded_observations);
                result.final_state = DaemonLifecycleState::Faulted;
                result.stop_reason = DaemonLoopStopReason::LifecycleFaulted;
                result.detail = "heartbeat transport failed before real VIP mutation";
                return result;
            }
            if (heartbeat_result.receive.peer_status.has_value() &&
                peerIsConfigured(request.config,
                                 heartbeat_result.receive.peer_status->node_id)) {
                recent_peers[heartbeat_result.receive.peer_status->node_id] =
                    RecentPeerStatus{.status = *heartbeat_result.receive.peer_status,
                                     .last_seen_elapsed_ms = elapsed_ms};
            }
        }
        expirePeers(recent_peers, elapsed_ms, request.config.heartbeat.timeout_ms);

        const auto peer_statuses = activePeerStatuses(recent_peers);
        auto lifecycle_config = request.config;
        const auto heartbeat_cycle_warmed_up =
            heartbeat_schedule.due && heartbeat_receive_state.receive_attempted &&
            heartbeat_receive_state.error.empty();
        if (realNetworkMutationRequested(request) && !real_mutation_warmup_complete) {
            lifecycle_config.mutation_safety.allow_network_mutation = false;
        }
        const auto lifecycle_result = runDaemonLifecycleOnce(
            DaemonLifecycleRequest{.config = lifecycle_config,
                                   .options = request.options.runtime_options,
                                   .initial_state = current_state,
                                   .shutdown_state = request.shutdown_state,
                                   .config_prevalidated = true,
                                   .local_healthy = true,
                                   .peer_statuses = peer_statuses},
            vip_manager, ownership_probe);

        current_state = lifecycle_result.final_state;
        result.final_state = lifecycle_result.final_state;
        result.detail = lifecycle_result.detail;
        appendObservations(result.validation_errors, lifecycle_result.validation_errors,
                           request.options.max_recorded_observations);
        appendObservations(result.vip_operations, lifecycle_result.vip_operations,
                           request.options.max_recorded_observations);

        if (lifecycle_result.iteration_ran) {
            local_status = lifecycle_result.local_status;

            auto health_schedule =
                evaluateHealthSchedule(request.config, result.iterations_ran, elapsed_ms,
                                       health_check_was_due, last_due_elapsed_ms);
            if (health_schedule.due) {
                health_check_was_due = true;
                last_due_elapsed_ms = elapsed_ms;
            }
            pushObservation(result.health_schedules, health_schedule,
                            request.options.max_recorded_observations);

            pushObservation(result.heartbeat_send_schedules, heartbeat_schedule,
                            request.options.max_recorded_observations);

            pushObservation(result.heartbeat_receive_states, heartbeat_receive_state,
                            request.options.max_recorded_observations);
            if (lifecycle_result.final_state != DaemonLifecycleState::Faulted) {
                pushObservation(
                    result.failover_decisions,
                    evaluateFailoverDecision(request.config, lifecycle_result.local_status.healthy,
                                             lifecycle_result.local_vip_owner,
                                             heartbeat_receive_state, peer_statuses),
                    request.options.max_recorded_observations);
            }
            ++result.iterations_ran;
        }

        if (lifecycle_result.final_state == DaemonLifecycleState::Faulted) {
            result.stop_reason = DaemonLoopStopReason::LifecycleFaulted;
            publishLoopResult(result_observer, result);
            return result;
        }
        publishLoopResult(result_observer, result);
        if (realNetworkMutationRequested(request) && heartbeat_cycle_warmed_up) {
            real_mutation_warmup_complete = true;
        }

        if (!request.options.run_until_shutdown && index + 1 == request.options.max_iterations) {
            result.stop_reason = DaemonLoopStopReason::MaxIterations;
            result.detail = "max iterations completed";
            return result;
        }

        if (request.options.inter_iteration_delay_ms > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds{request.options.inter_iteration_delay_ms});
        }
        if (lifecycle_result.iteration_ran) {
            elapsed_ms += iteration_elapsed_ms;
        }
    }

    return result;
}

DaemonLoopResult runDaemonRuntimeLoop(const DaemonLoopRequest& request,
                                      VipManager& vip_manager,
                                      VipOwnershipProbe& ownership_probe,
                                      HeartbeatTransport& heartbeat_transport) {
    return runDaemonRuntimeLoop(request, vip_manager, ownership_probe, heartbeat_transport, {});
}

} // namespace easyfailover
