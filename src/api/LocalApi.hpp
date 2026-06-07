#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
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
    int quorum_size = 0;
};

struct LocalApiConfigApi {
    bool enabled = false;
    std::string bind;
    bool read_only = true;
    bool auth_token_configured = false;
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

enum class LocalApiConfigApplyOutcome {
    Applied,
    ValidationFailed,
    RequestError,
    Unauthorized,
    Forbidden,
    InternalError,
};

struct LocalApiConfigApplyResult {
    LocalApiConfigApplyOutcome outcome = LocalApiConfigApplyOutcome::InternalError;
    int status_code = 500;
    bool applied = false;
    std::vector<std::string> errors;
    std::string applied_path;
    std::string backup_path;
    std::string error_code;
    std::string error_message;
    // Stable, secret-free audit detail and reason for the api_write_attempt event.
    std::string audit_outcome;
    std::string audit_reason;
    std::string audit_detail;
};

// Context required to authenticate and apply a config write. The token is loaded once at
// startup and must never be serialized into a response or logged.
struct LocalApiWriteContext {
    bool write_enabled = false;
    std::string config_path;
    std::string auth_token;
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
    std::string authorization;
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

[[nodiscard]] constexpr std::string_view toString(const LocalApiConfigApplyOutcome outcome) {
    switch (outcome) {
    case LocalApiConfigApplyOutcome::Applied:
        return "applied";
    case LocalApiConfigApplyOutcome::ValidationFailed:
        return "validation_failed";
    case LocalApiConfigApplyOutcome::RequestError:
        return "request_error";
    case LocalApiConfigApplyOutcome::Unauthorized:
        return "unauthorized";
    case LocalApiConfigApplyOutcome::Forbidden:
        return "forbidden";
    case LocalApiConfigApplyOutcome::InternalError:
        return "internal_error";
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

// Constant-time comparison of two byte strings. Always inspects the full fixed length of the
// loaded/expected token so the running time does not reveal how many leading bytes matched, and
// never short-circuits. Used only on the secret token path.
[[nodiscard]] bool constantTimeTokenEquals(std::string_view presented, std::string_view expected);

// Reads a bearer token from a file, trimming trailing whitespace/newline. Returns std::nullopt if
// the file cannot be read; an empty (post-trim) file yields an empty string.
[[nodiscard]] std::optional<std::string> loadApiAuthToken(const std::string& path);

// Extracts the bearer token from an Authorization header value. Returns std::nullopt when the
// scheme is not "Bearer" or the token is empty/malformed.
[[nodiscard]] std::optional<std::string> parseBearerToken(std::string_view authorization_header);

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

// Authenticates the presented bearer token against the write context, validates the submitted
// config, and on success atomically persists it (temp file + fsync + rename) while keeping a
// backup of the previous config. The daemon does not hot-reload; the new config takes effect on
// restart/reload. Never writes the token or raw config secrets into the returned audit fields.
[[nodiscard]] LocalApiConfigApplyResult buildLocalApiConfigApplyResult(
    const LocalApiHttpRequest& request,
    const LocalApiWriteContext& write_context);

[[nodiscard]] std::string serializeLocalApiConfigApplyResult(
    const LocalApiConfigApplyResult& result);

// Builds the structured api_write_attempt audit event for a config apply attempt (success or
// failure, including auth rejection). The token value is never included.
[[nodiscard]] LocalApiEvent buildLocalApiWriteAttemptEvent(
    std::uint64_t sequence,
    const LocalApiHttpRequest& request,
    const LocalApiConfigApplyResult& result);

[[nodiscard]] std::string formatLocalApiWriteAttemptEvent(
    const LocalApiHttpRequest& request,
    const LocalApiConfigApplyResult& result);

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

// Write-aware routing: read endpoints behave exactly as the read-only overload, and the
// authenticated POST /api/v1/config/apply endpoint is enabled when write_context.write_enabled is
// true. An emit_audit callback (if set) receives every apply attempt's audit result so the serve
// loop can log it and surface it in the events feed.
[[nodiscard]] LocalApiHttpResponse buildLocalApiHttpResponse(
    const LocalApiHttpRequest& request,
    const LocalApiHttpSnapshot& snapshot,
    const LocalApiWriteContext& write_context,
    const std::function<void(const LocalApiHttpRequest&, const LocalApiConfigApplyResult&)>&
        emit_audit);

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

[[nodiscard]] LocalApiHttpServeResult serveLocalApiHttp(
    std::string_view bind,
    const LocalApiHttpSnapshotProvider& snapshot_provider,
    ShutdownSignalState* shutdown_state,
    const LocalApiHttpStartupObserver& startup_observer,
    const LocalApiWriteContext& write_context,
    const std::function<void(const LocalApiHttpRequest&, const LocalApiConfigApplyResult&)>&
        emit_audit);

} // namespace easyfailover
