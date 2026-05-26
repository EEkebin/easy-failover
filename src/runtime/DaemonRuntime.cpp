#include "runtime/DaemonRuntime.hpp"

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

} // namespace easyfailover
