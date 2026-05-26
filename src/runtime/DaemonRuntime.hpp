#pragma once

#include "config/Config.hpp"
#include "platform/VipManager.hpp"
#include "runtime/ShutdownSignal.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace easyfailover {

enum class DaemonLifecycleState {
    Stopped,
    Running,
    Faulted,
};

struct DaemonRuntimeOptions {
    bool dry_run = true;
};

struct DaemonLifecycleRequest {
    const Config& config;
    DaemonRuntimeOptions options;
    DaemonLifecycleState initial_state = DaemonLifecycleState::Stopped;
    const ShutdownSignalState* shutdown_state = nullptr;
    bool config_prevalidated = false;
};

struct DaemonLifecycleResult {
    DaemonLifecycleState initial_state = DaemonLifecycleState::Stopped;
    DaemonLifecycleState final_state = DaemonLifecycleState::Stopped;
    bool started = false;
    bool iteration_ran = false;
    bool stopped = false;
    std::vector<std::string> validation_errors;
    std::vector<VipOperationResult> vip_operations;
    std::string detail;
};

[[nodiscard]] constexpr std::string_view toString(const DaemonLifecycleState state) {
    switch (state) {
    case DaemonLifecycleState::Stopped:
        return "stopped";
    case DaemonLifecycleState::Running:
        return "running";
    case DaemonLifecycleState::Faulted:
        return "faulted";
    }

    return "unknown";
}

[[nodiscard]] DaemonLifecycleResult runDaemonLifecycleOnce(
    const DaemonLifecycleRequest& request,
    VipManager& vip_manager);

} // namespace easyfailover
