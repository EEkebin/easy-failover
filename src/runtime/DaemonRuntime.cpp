#include "runtime/DaemonRuntime.hpp"

namespace easyfailover {

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

    result.validation_errors = request.config.validate();
    if (!result.validation_errors.empty()) {
        result.final_state = DaemonLifecycleState::Faulted;
        result.detail = "config validation failed";
        return result;
    }

    result.started = request.initial_state == DaemonLifecycleState::Stopped;
    result.iteration_ran = true;

    if (request.options.dry_run) {
        const auto add_result =
            vip_manager.addVip(request.config.vip.address, request.config.vip.interface);
        result.vip_operations.push_back(add_result);
        if (!add_result.dry_run) {
            result.final_state = DaemonLifecycleState::Faulted;
            result.detail = "dry-run lifecycle received non-dry-run VIP operation";
            return result;
        }

        const auto announce_result =
            vip_manager.announceVip(request.config.vip.address, request.config.vip.interface);
        result.vip_operations.push_back(announce_result);
        if (!announce_result.dry_run) {
            result.final_state = DaemonLifecycleState::Faulted;
            result.detail = "dry-run lifecycle received non-dry-run VIP operation";
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
