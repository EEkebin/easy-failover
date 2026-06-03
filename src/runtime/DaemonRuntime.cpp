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
                                   .detail = "max iterations completed"};

    auto current_state = request.initial_state;
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
    }

    return result;
}

} // namespace easyfailover
