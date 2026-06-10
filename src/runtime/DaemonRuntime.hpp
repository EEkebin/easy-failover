#pragma once

#include "config/Config.hpp"
#include "core/FailoverDecision.hpp"
#include "discovery/DiscoveryTransport.hpp"
#include "health/HealthCheck.hpp"
#include "heartbeat/HeartbeatLoop.hpp"
#include "platform/VipManager.hpp"
#include "platform/VipOwnershipProbe.hpp"
#include "runtime/ShutdownSignal.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace easyfailover {

struct DaemonLoopResult;

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
    bool run_until_shutdown = false;
    std::int64_t inter_iteration_delay_ms = 0;
    std::int64_t logical_iteration_elapsed_ms = 0;
    std::size_t max_recorded_observations = 0;
};

struct DaemonLifecycleRequest {
    const Config& config;
    DaemonRuntimeOptions options;
    DaemonLifecycleState initial_state = DaemonLifecycleState::Stopped;
    ShutdownSignalState* shutdown_state = nullptr;
    bool config_prevalidated = false;
    bool local_healthy = true;
    std::vector<PeerStatus> peer_statuses;
};

struct DaemonLifecycleResult {
    DaemonLifecycleState initial_state = DaemonLifecycleState::Stopped;
    DaemonLifecycleState final_state = DaemonLifecycleState::Stopped;
    bool started = false;
    bool iteration_ran = false;
    bool stopped = false;
    std::vector<std::string> validation_errors;
    std::vector<VipOperationResult> vip_operations;
    bool local_vip_owner_known = false;
    bool local_vip_owner = false;
    LocalNodeStatus local_status;
    FailoverDecision failover_decision;
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
    bool timed_out = false;
    std::int64_t timeout_ms = 0;
    std::string peer_address;
    std::optional<PeerStatus> peer_status;
    std::string error;
};

struct FailoverDecisionObservation {
    std::size_t iteration_index = 0;
    std::int64_t elapsed_ms = 0;
    LocalNodeStatus local_status;
    std::vector<PeerStatus> peer_statuses;
    FailoverDecision decision;
};

struct DaemonLoopRequest {
    const Config& config;
    DaemonLoopOptions options;
    DaemonLifecycleState initial_state = DaemonLifecycleState::Stopped;
    ShutdownSignalState* shutdown_state = nullptr;
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
    std::vector<HeartbeatSendObservation> heartbeat_sends;
    std::vector<HeartbeatReceiveStateObservation> heartbeat_receive_states;
    std::vector<FailoverDecisionObservation> failover_decisions;
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
    VipManager& vip_manager,
    VipOwnershipProbe& ownership_probe);

[[nodiscard]] DaemonLoopResult runDaemonRuntimeLoop(const DaemonLoopRequest& request,
                                                   VipManager& vip_manager,
                                                   VipOwnershipProbe& ownership_probe,
                                                   HeartbeatTransport& heartbeat_transport);

// Live discovery wiring for the loop. When `transport` is non-null and `[discovery].enabled` is
// set, the loop broadcasts a signed beacon each interval and folds verified beacons from peers
// into the election. A default (transport == nullptr) disables discovery, so existing callers and
// tests are unaffected.
struct DiscoveryContext {
    DiscoveryTransport* transport = nullptr;
    std::string self_mac; // this node's stable identity (MAC of the VIP interface)
    std::string secret;   // shared cluster secret, already loaded from secret_file
};

[[nodiscard]] DaemonLoopResult runDaemonRuntimeLoop(
    const DaemonLoopRequest& request,
    VipManager& vip_manager,
    VipOwnershipProbe& ownership_probe,
    HeartbeatTransport& heartbeat_transport,
    const std::function<void(const DaemonLoopResult&)>& result_observer,
    const DiscoveryContext& discovery = {});

} // namespace easyfailover
