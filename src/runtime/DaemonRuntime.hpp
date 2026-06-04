#pragma once

#include "config/Config.hpp"
#include "platform/VipManager.hpp"
#include "runtime/ShutdownSignal.hpp"

#include <cstddef>
#include <cstdint>
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

enum class DaemonLoopStopReason {
    MaxIterations,
    ShutdownRequested,
    LifecycleFaulted,
};

[[nodiscard]] constexpr std::string_view toString(const DaemonLoopStopReason reason) {
    switch (reason) {
    case DaemonLoopStopReason::MaxIterations:
        return "max_iterations";
    case DaemonLoopStopReason::ShutdownRequested:
        return "shutdown_requested";
    case DaemonLoopStopReason::LifecycleFaulted:
        return "lifecycle_faulted";
    }

    return "unknown";
}

struct DaemonLoopOptions {
    DaemonRuntimeOptions runtime_options;
    std::size_t max_iterations = 1;
    std::int64_t inter_iteration_delay_ms = 0;
    std::int64_t logical_iteration_elapsed_ms = 0;
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

struct HealthScheduleObservation {
    std::size_t iteration_index = 0;
    std::int64_t elapsed_ms = 0;
    std::int64_t interval_ms = 0;
    bool due = false;
    bool command_configured = false;
};

struct HeartbeatSendScheduleObservation {
    std::size_t iteration_index = 0;
    std::int64_t elapsed_ms = 0;
    std::int64_t interval_ms = 0;
    bool due = false;
    bool peers_configured = false;
    std::size_t expected_send_count = 0;
};

struct HeartbeatReceiveStateObservation {
    std::size_t iteration_index = 0;
    std::int64_t elapsed_ms = 0;
    bool receive_attempted = false;
    bool peer_status_observed = false;
    std::string peer_id;
    bool heartbeat_seen = false;
};

struct DaemonLoopRequest {
    const Config& config;
    DaemonLoopOptions options;
    DaemonLifecycleState initial_state = DaemonLifecycleState::Stopped;
    const ShutdownSignalState* shutdown_state = nullptr;
    bool config_prevalidated = false;
};

struct DaemonLoopResult {
    DaemonLifecycleState initial_state = DaemonLifecycleState::Stopped;
    DaemonLifecycleState final_state = DaemonLifecycleState::Stopped;
    DaemonLoopStopReason stop_reason = DaemonLoopStopReason::MaxIterations;
    std::size_t iterations_ran = 0;
    std::vector<std::string> validation_errors;
    std::vector<VipOperationResult> vip_operations;
    std::vector<HealthScheduleObservation> health_schedules;
    std::vector<HeartbeatSendScheduleObservation> heartbeat_send_schedules;
    std::vector<HeartbeatReceiveStateObservation> heartbeat_receive_states;
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

[[nodiscard]] DaemonLoopResult runDaemonRuntimeLoop(const DaemonLoopRequest& request,
                                                   VipManager& vip_manager);

} // namespace easyfailover
