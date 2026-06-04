#include "runtime/DaemonRuntime.hpp"

#include <chrono>
#include <thread>

namespace easyfailover {

namespace {

[[nodiscard]] bool recordDryRunVipOperation(DaemonLifecycleResult& lifecycle_result,
                                            const VipOperationResult& operation_result) {
    lifecycle_result.vip_operations.push_back(operation_result);
    if (!operation_result.dry_run) {
        lifecycle_result.final_state = DaemonLifecycleState::Faulted;
        lifecycle_result.detail = "dry-run lifecycle received non-dry-run VIP operation";
        return false;
    }

    if (!operation_result.success) {
        lifecycle_result.final_state = DaemonLifecycleState::Faulted;
        lifecycle_result.detail = operation_result.error.empty()
                                      ? "dry-run lifecycle VIP operation failed"
                                      : operation_result.error;
        return false;
    }

    return true;
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

[[nodiscard]] std::int64_t effectiveIterationElapsedMs(const DaemonLoopOptions& options) {
    const auto elapsed_ms = options.logical_iteration_elapsed_ms > 0
                                ? options.logical_iteration_elapsed_ms
                                : options.inter_iteration_delay_ms;
    return elapsed_ms > 0 ? elapsed_ms : 0;
}

} // namespace

DaemonLifecycleResult runDaemonLifecycleOnce(const DaemonLifecycleRequest& request,
                                             VipManager& vip_manager) {
    auto result = DaemonLifecycleResult{.initial_state = request.initial_state,
                                        .final_state = request.initial_state,
                                        .started = false,
                                        .iteration_ran = false,
                                        .stopped = false,
                                        .validation_errors = {},
                                        .vip_operations = {},
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

    if (request.options.dry_run) {
        const auto add_result =
            vip_manager.addVip(request.config.vip.address, request.config.vip.interface);
        if (!recordDryRunVipOperation(result, add_result)) {
            return result;
        }

        const auto announce_result =
            vip_manager.announceVip(request.config.vip.address, request.config.vip.interface);
        if (!recordDryRunVipOperation(result, announce_result)) {
            return result;
        }
        result.detail = "dry-run lifecycle iteration completed";
    } else {
        result.detail = "real VIP movement is not implemented yet; no network state changed";
    }

    result.final_state = DaemonLifecycleState::Stopped;
    result.stopped = true;
    return result;
}

DaemonLoopResult runDaemonRuntimeLoop(const DaemonLoopRequest& request, VipManager& vip_manager) {
    auto result = DaemonLoopResult{.initial_state = request.initial_state,
                                   .final_state = request.initial_state,
                                   .stop_reason = DaemonLoopStopReason::MaxIterations,
                                   .iterations_ran = 0,
                                   .validation_errors = {},
                                   .vip_operations = {},
                                   .health_schedules = {},
                                   .heartbeat_send_schedules = {},
                                   .detail = "max iterations completed"};

    if (request.shutdown_state != nullptr && request.shutdown_state->shutdownRequested()) {
        result.final_state = DaemonLifecycleState::Stopped;
        result.stop_reason = DaemonLoopStopReason::ShutdownRequested;
        result.detail = std::string{request.shutdown_state->reason()};
        return result;
    }

    auto current_state = request.initial_state;
    auto elapsed_ms = std::int64_t{0};
    const auto iteration_elapsed_ms = effectiveIterationElapsedMs(request.options);
    auto health_check_was_due = false;
    auto last_due_elapsed_ms = std::int64_t{0};
    auto heartbeat_send_was_due = false;
    auto last_heartbeat_send_due_elapsed_ms = std::int64_t{0};
    for (std::size_t index = 0; index < request.options.max_iterations; ++index) {
        if (request.shutdown_state != nullptr && request.shutdown_state->shutdownRequested()) {
            result.final_state = DaemonLifecycleState::Stopped;
            result.stop_reason = DaemonLoopStopReason::ShutdownRequested;
            result.detail = std::string{request.shutdown_state->reason()};
            return result;
        }

        const auto lifecycle_result = runDaemonLifecycleOnce(
            DaemonLifecycleRequest{.config = request.config,
                                   .options = request.options.runtime_options,
                                   .initial_state = current_state,
                                   .shutdown_state = request.shutdown_state,
                                   .config_prevalidated = request.config_prevalidated},
            vip_manager);

        current_state = lifecycle_result.final_state;
        result.final_state = lifecycle_result.final_state;
        result.detail = lifecycle_result.detail;
        result.validation_errors.insert(result.validation_errors.end(),
                                        lifecycle_result.validation_errors.begin(),
                                        lifecycle_result.validation_errors.end());
        result.vip_operations.insert(result.vip_operations.end(),
                                     lifecycle_result.vip_operations.begin(),
                                     lifecycle_result.vip_operations.end());

        if (lifecycle_result.iteration_ran) {
            auto health_schedule =
                evaluateHealthSchedule(request.config, result.iterations_ran, elapsed_ms,
                                       health_check_was_due, last_due_elapsed_ms);
            if (health_schedule.due) {
                health_check_was_due = true;
                last_due_elapsed_ms = elapsed_ms;
            }
            result.health_schedules.push_back(health_schedule);

            auto heartbeat_schedule = evaluateHeartbeatSendSchedule(
                request.config, result.iterations_ran, elapsed_ms, heartbeat_send_was_due,
                last_heartbeat_send_due_elapsed_ms);
            if (heartbeat_schedule.due) {
                heartbeat_send_was_due = true;
                last_heartbeat_send_due_elapsed_ms = elapsed_ms;
            }
            result.heartbeat_send_schedules.push_back(heartbeat_schedule);
            ++result.iterations_ran;
        }

        if (lifecycle_result.final_state == DaemonLifecycleState::Faulted) {
            result.stop_reason = DaemonLoopStopReason::LifecycleFaulted;
            return result;
        }

        if (index + 1 == request.options.max_iterations) {
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

} // namespace easyfailover
