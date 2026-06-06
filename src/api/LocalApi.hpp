#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace easyfailover {

struct ApiConfig;
struct Config;
struct DaemonLifecycleResult;
struct HealthCheckResult;
struct RuntimeLogContext;
class ShutdownSignalState;
struct VipOperationResult;
enum class NodeState;

enum class LocalApiStartupState {
    Disabled,
    Ready,
    Rejected,
};

struct LocalApiStartupResult {
    LocalApiStartupState state = LocalApiStartupState::Disabled;
    std::string bind;
    std::string detail;
};

struct LocalApiStatusNode {
    std::string id;
    int priority = 0;
    std::string state;
    bool healthy = false;
};

struct LocalApiStatusVip {
    std::string address;
    std::string interface;
    bool local_owner = false;
};

struct LocalApiStatusLifecycle {
    std::string initial_state;
    std::string final_state;
    std::string detail;
    bool dry_run = true;
    bool started = false;
    bool iteration_ran = false;
    bool stopped = false;
};

struct LocalApiStatusHeartbeat {
    std::string bind;
    std::int64_t interval_ms = 0;
    std::int64_t timeout_ms = 0;
    int peers_observed = 0;
};

struct LocalApiStatusHealth {
    std::string status;
    std::string detail;
};

struct LocalApiStatusResponse {
    LocalApiStatusNode node;
    LocalApiStatusVip vip;
    LocalApiStatusLifecycle lifecycle;
    LocalApiStatusHeartbeat heartbeat;
    LocalApiStatusHealth health;
};

struct LocalApiConfigVip {
    std::string address;
    std::string interface;
};

struct LocalApiConfigHeartbeat {
    std::string bind;
    std::int64_t interval_ms = 0;
    std::int64_t timeout_ms = 0;
};

struct LocalApiConfigHealth {
    bool command_redacted = false;
    std::int64_t interval_ms = 0;
    std::int64_t timeout_ms = 0;
};

struct LocalApiConfigElection {
    bool require_quorum = false;
    bool preempt = true;
};

struct LocalApiConfigApi {
    bool enabled = false;
    std::string bind;
    bool read_only = true;
};

struct LocalApiConfigMutationSafety {
    bool allow_network_mutation = false;
};

struct LocalApiConfigPeer {
    std::string id;
    std::string address;
};

struct LocalApiConfigResponse {
    std::string node_id;
    int priority = 0;
    LocalApiConfigVip vip;
    LocalApiConfigHeartbeat heartbeat;
    LocalApiConfigHealth health;
    LocalApiConfigElection election;
    LocalApiConfigApi api;
    LocalApiConfigMutationSafety mutation_safety;
    std::vector<LocalApiConfigPeer> peers;
};

struct LocalApiConfigValidateRequest {
    std::string format;
    std::string config;
};

enum class LocalApiConfigValidateOutcome {
    Completed,
    RequestError,
};

struct LocalApiConfigValidateResponse {
    LocalApiConfigValidateOutcome outcome = LocalApiConfigValidateOutcome::Completed;
    bool valid = false;
    std::vector<std::string> errors;
    std::string error_code;
    std::string error_message;
};

using LocalApiEventFieldValue = std::variant<std::string, bool, std::int64_t>;

struct LocalApiEventField {
    std::string name;
    LocalApiEventFieldValue value;
};

struct LocalApiEvent {
    std::uint64_t sequence = 0;
    std::string event;
    std::string level = "info";
    std::string message;
    std::vector<LocalApiEventField> fields;
};

struct LocalApiEventsResponse {
    std::vector<LocalApiEvent> events;
};

struct LocalApiHttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

struct LocalApiHttpResponse {
    int status_code = 200;
    std::string content_type = "application/json";
    std::string body;
};

struct LocalApiHttpSnapshot {
    LocalApiStatusResponse status;
    LocalApiConfigResponse config;
    LocalApiEventsResponse events;
};

struct LocalApiHttpServeResult {
    bool started = false;
    bool served_request = false;
    std::string error;
};

using LocalApiHttpSnapshotProvider = std::function<LocalApiHttpSnapshot()>;
using LocalApiHttpStartupObserver = std::function<void(const LocalApiHttpServeResult&)>;

[[nodiscard]] constexpr std::string_view toString(const LocalApiStartupState state) {
    switch (state) {
    case LocalApiStartupState::Disabled:
        return "disabled";
    case LocalApiStartupState::Ready:
        return "ready";
    case LocalApiStartupState::Rejected:
        return "rejected";
    }

    return "unknown";
}

[[nodiscard]] constexpr std::string_view toString(const LocalApiConfigValidateOutcome outcome) {
    switch (outcome) {
    case LocalApiConfigValidateOutcome::Completed:
        return "completed";
    case LocalApiConfigValidateOutcome::RequestError:
        return "request_error";
    }

    return "unknown";
}

[[nodiscard]] LocalApiStartupResult evaluateLocalApiStartup(const ApiConfig& config);

[[nodiscard]] LocalApiStatusResponse buildLocalApiStatusResponse(
    const Config& config,
    const DaemonLifecycleResult& lifecycle,
    const HealthCheckResult& health,
    NodeState local_node_state,
    bool dry_run,
    int peers_observed = 0);

[[nodiscard]] LocalApiConfigResponse buildLocalApiConfigResponse(const Config& config);

[[nodiscard]] LocalApiConfigValidateResponse buildLocalApiConfigValidateResponse(
    const LocalApiConfigValidateRequest& request);

[[nodiscard]] LocalApiEventsResponse buildLocalApiEventsResponse();
[[nodiscard]] LocalApiEventsResponse buildLocalApiEventsResponse(std::vector<LocalApiEvent> events);

[[nodiscard]] LocalApiEvent buildLocalApiLifecycleEvent(
    std::uint64_t sequence,
    const DaemonLifecycleResult& result,
    const RuntimeLogContext& context);

[[nodiscard]] LocalApiEvent buildLocalApiVipOperationEvent(
    std::uint64_t sequence,
    const VipOperationResult& result,
    std::size_t zero_based_index);

[[nodiscard]] std::string serializeLocalApiStatusResponse(const LocalApiStatusResponse& response);
[[nodiscard]] std::string serializeLocalApiConfigResponse(const LocalApiConfigResponse& response);
[[nodiscard]] std::string serializeLocalApiConfigValidateResponse(
    const LocalApiConfigValidateResponse& response);
[[nodiscard]] std::string serializeLocalApiEventsResponse(const LocalApiEventsResponse& response);

[[nodiscard]] LocalApiHttpResponse buildLocalApiHttpResponse(
    const LocalApiHttpRequest& request,
    const LocalApiHttpSnapshot& snapshot);

[[nodiscard]] LocalApiHttpServeResult serveLocalApiHttpOnce(
    std::string_view bind,
    const LocalApiHttpSnapshot& snapshot);

[[nodiscard]] LocalApiHttpServeResult serveLocalApiHttp(
    std::string_view bind,
    const LocalApiHttpSnapshot& snapshot);

[[nodiscard]] LocalApiHttpServeResult serveLocalApiHttp(
    std::string_view bind,
    const LocalApiHttpSnapshot& snapshot,
    ShutdownSignalState* shutdown_state);

[[nodiscard]] LocalApiHttpServeResult serveLocalApiHttp(
    std::string_view bind,
    const LocalApiHttpSnapshotProvider& snapshot_provider,
    ShutdownSignalState* shutdown_state);

[[nodiscard]] LocalApiHttpServeResult serveLocalApiHttp(
    std::string_view bind,
    const LocalApiHttpSnapshotProvider& snapshot_provider,
    ShutdownSignalState* shutdown_state,
    const LocalApiHttpStartupObserver& startup_observer);

} // namespace easyfailover
