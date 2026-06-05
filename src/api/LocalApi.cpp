#include "api/LocalApi.hpp"

#include "config/Config.hpp"
#include "core/NodeState.hpp"
#include "health/HealthCheck.hpp"
#include "runtime/DaemonRuntime.hpp"
#include "runtime/RuntimeLog.hpp"
#include "runtime/ShutdownSignal.hpp"

#include <cstdint>
#include <exception>
#include <array>
#include <sstream>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace easyfailover {

namespace {

constexpr auto kAcceptPollTimeoutMs = 250;
constexpr auto kReadPollTimeoutMs = 1000;

[[nodiscard]] std::string healthDetailForStatusResponse(const Config& config,
                                                        const HealthCheckResult& health) {
    if (config.health.command.empty()) {
        return health.detail;
    }
    if (health.detail == "health check not evaluated") {
        return health.detail;
    }

    return "health command detail redacted";
}

[[nodiscard]] LocalApiEventField stringField(std::string name, std::string value) {
    return LocalApiEventField{.name = std::move(name), .value = std::move(value)};
}

[[nodiscard]] LocalApiEventField boolField(std::string name, const bool value) {
    return LocalApiEventField{.name = std::move(name), .value = value};
}

[[nodiscard]] LocalApiEventField intField(std::string name, const std::int64_t value) {
    return LocalApiEventField{.name = std::move(name), .value = value};
}

[[nodiscard]] std::string jsonString(const std::string_view value) {
    auto output = std::ostringstream{};
    output << '"';
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (byte < 0x20) {
                constexpr auto kHexDigits = std::string_view{"0123456789abcdef"};
                output << "\\u00" << kHexDigits.at(byte >> 4U)
                       << kHexDigits.at(byte & 0x0FU);
            } else {
                output << character;
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

[[nodiscard]] const char* jsonBool(const bool value) {
    return value ? "true" : "false";
}

[[nodiscard]] std::string errorEnvelope(const std::string_view code,
                                        const std::string_view message) {
    auto output = std::ostringstream{};
    output << "{\"error\":{\"code\":" << jsonString(code)
           << ",\"message\":" << jsonString(message)
           << ",\"details\":[]}}";
    return output.str();
}

[[nodiscard]] LocalApiHttpResponse jsonResponse(const int status_code, std::string body) {
    return LocalApiHttpResponse{.status_code = status_code,
                                .content_type = "application/json",
                                .body = std::move(body)};
}

[[nodiscard]] std::string httpStatusText(const int status_code) {
    switch (status_code) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 500:
        return "Internal Server Error";
    default:
        return "OK";
    }
}

[[nodiscard]] std::string httpResponseBytes(const LocalApiHttpResponse& response) {
    auto output = std::ostringstream{};
    output << "HTTP/1.1 " << response.status_code << ' '
           << httpStatusText(response.status_code) << "\r\n"
           << "Content-Type: " << response.content_type << "\r\n"
           << "Content-Length: " << response.body.size() << "\r\n"
           << "Access-Control-Allow-Origin: http://localhost:3000\r\n"
           << "Connection: close\r\n"
           << "\r\n"
           << response.body;
    return output.str();
}

[[nodiscard]] std::string eventFieldValueJson(const LocalApiEventFieldValue& value) {
    return std::visit(
        [](const auto& concrete) -> std::string {
            using Value = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<Value, std::string>) {
                return jsonString(concrete);
            } else if constexpr (std::is_same_v<Value, bool>) {
                return jsonBool(concrete);
            } else {
                return std::to_string(concrete);
            }
        },
        value);
}

[[nodiscard]] std::string stringVectorJson(const std::vector<std::string>& values) {
    auto output = std::ostringstream{};
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            output << ',';
        }
        output << jsonString(values.at(index));
    }
    output << ']';
    return output.str();
}

[[nodiscard]] std::string parseJsonStringValue(const std::string_view body,
                                               const std::string_view key) {
    const auto key_pattern = "\"" + std::string{key} + "\"";
    auto key_position = body.find(key_pattern);
    if (key_position == std::string_view::npos) {
        return {};
    }

    auto colon_position = body.find(':', key_position + key_pattern.size());
    if (colon_position == std::string_view::npos) {
        return {};
    }

    auto value_position = body.find_first_not_of(" \t\r\n", colon_position + 1);
    if (value_position == std::string_view::npos || body.at(value_position) != '"') {
        return {};
    }
    ++value_position;

    auto output = std::string{};
    for (auto index = value_position; index < body.size(); ++index) {
        const auto character = body.at(index);
        if (character == '"') {
            return output;
        }
        if (character != '\\') {
            output.push_back(character);
            continue;
        }
        if (index + 1 >= body.size()) {
            return {};
        }
        const auto escaped = body.at(++index);
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            output.push_back(escaped);
            break;
        case 'b':
            output.push_back('\b');
            break;
        case 'f':
            output.push_back('\f');
            break;
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        case 't':
            output.push_back('\t');
            break;
        default:
            return {};
        }
    }

    return {};
}

[[nodiscard]] std::string trimQueryString(const std::string& path) {
    const auto query_start = path.find('?');
    if (query_start == std::string::npos) {
        return path;
    }
    return path.substr(0, query_start);
}

[[nodiscard]] std::optional<LocalApiHttpRequest> parseHttpRequest(const std::string& raw) {
    const auto request_line_end = raw.find("\r\n");
    if (request_line_end == std::string::npos) {
        return std::nullopt;
    }

    auto request_line = std::istringstream{raw.substr(0, request_line_end)};
    auto request = LocalApiHttpRequest{};
    request_line >> request.method >> request.path;
    if (request.method.empty() || request.path.empty()) {
        return std::nullopt;
    }

    const auto body_start = raw.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        request.body = raw.substr(body_start + 4);
    }

    return request;
}

[[nodiscard]] std::optional<std::pair<std::string, int>> parseBind(
    const std::string_view bind) {
    const auto separator = bind.rfind(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= bind.size()) {
        return std::nullopt;
    }

    auto host = std::string{bind.substr(0, separator)};
    auto port_text = std::string{bind.substr(separator + 1)};
    try {
        const auto port = std::stoi(port_text);
        if (port <= 0 || port > 65535) {
            return std::nullopt;
        }
        return std::pair<std::string, int>{std::move(host), port};
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::size_t> parseContentLength(const std::string& raw) {
    const auto headers_end = raw.find("\r\n\r\n");
    if (headers_end == std::string::npos) {
        return std::nullopt;
    }

    constexpr auto kHeader = std::string_view{"Content-Length:"};
    auto search_start = raw.find("\r\n");
    while (search_start != std::string::npos && search_start + 2 < headers_end) {
        const auto line_start = search_start + 2;
        const auto line_end = raw.find("\r\n", line_start);
        if (line_end == std::string::npos || line_end > headers_end) {
            break;
        }

        const auto line = std::string_view{raw}.substr(line_start, line_end - line_start);
        if (line.rfind(kHeader, 0) == 0) {
            const auto value_start = line.find_first_not_of(" \t", kHeader.size());
            if (value_start == std::string_view::npos) {
                return std::nullopt;
            }
            try {
                return static_cast<std::size_t>(
                    std::stoull(std::string{line.substr(value_start)}));
            } catch (...) {
                return std::nullopt;
            }
        }
        search_start = line_end;
    }

    return std::size_t{0};
}

[[nodiscard]] std::string readRequestBytes(const int client_fd) {
    auto data = std::string{};
    auto buffer = std::array<char, 4096>{};
    while (data.size() < 65536) {
        auto poll_fd = pollfd{.fd = client_fd, .events = POLLIN, .revents = 0};
        const auto poll_result = ::poll(&poll_fd, 1, kReadPollTimeoutMs);
        if (poll_result <= 0 || (poll_fd.revents & POLLIN) == 0) {
            break;
        }

        const auto read_count = ::recv(client_fd, buffer.data(), buffer.size(), 0);
        if (read_count <= 0) {
            break;
        }
        data.append(buffer.data(), static_cast<std::size_t>(read_count));
        const auto headers_end = data.find("\r\n\r\n");
        if (headers_end == std::string::npos) {
            continue;
        }
        const auto content_length = parseContentLength(data);
        if (!content_length.has_value()) {
            break;
        }
        const auto body_bytes_read = data.size() - headers_end - 4;
        if (body_bytes_read >= *content_length) {
            break;
        }
    }
    return data;
}

[[nodiscard]] bool shutdownRequested(ShutdownSignalState* shutdown_state) {
    if (shutdown_state == nullptr) {
        return false;
    }
    pollShutdownSignals(*shutdown_state);
    return shutdown_state->shutdownRequested();
}

void closeFd(const int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

} // namespace

LocalApiStartupResult evaluateLocalApiStartup(const ApiConfig& config) {
    if (!config.enabled) {
        return LocalApiStartupResult{.state = LocalApiStartupState::Disabled,
                                     .bind = config.bind,
                                     .detail = "local API disabled"};
    }

    if (!config.read_only) {
        return LocalApiStartupResult{
            .state = LocalApiStartupState::Rejected,
            .bind = config.bind,
            .detail = "local API write mode requires authentication and write-behavior design"};
    }

    return LocalApiStartupResult{.state = LocalApiStartupState::Ready,
                                 .bind = config.bind,
                                 .detail = "local API read-only listener ready"};
}

LocalApiStatusResponse buildLocalApiStatusResponse(const Config& config,
                                                   const DaemonLifecycleResult& lifecycle,
                                                   const HealthCheckResult& health,
                                                   const NodeState local_node_state,
                                                   const bool dry_run,
                                                   const int peers_observed) {
    return LocalApiStatusResponse{
        .node = LocalApiStatusNode{.id = config.node_id,
                                   .priority = config.priority,
                                   .state = std::string{toString(local_node_state)},
                                   .healthy = health.status == HealthStatus::Healthy},
        .vip = LocalApiStatusVip{.address = config.vip.address,
                                 .interface = config.vip.interface,
                                 .local_owner = lifecycle.local_vip_owner},
        .lifecycle = LocalApiStatusLifecycle{
            .initial_state = std::string{toString(lifecycle.initial_state)},
            .final_state = std::string{toString(lifecycle.final_state)},
            .detail = lifecycle.detail,
            .dry_run = dry_run,
            .started = lifecycle.started,
            .iteration_ran = lifecycle.iteration_ran,
            .stopped = lifecycle.stopped},
        .heartbeat = LocalApiStatusHeartbeat{.bind = config.heartbeat.bind,
                                             .interval_ms = config.heartbeat.interval_ms,
                                             .timeout_ms = config.heartbeat.timeout_ms,
                                             .peers_observed = peers_observed},
        .health = LocalApiStatusHealth{.status = std::string{toString(health.status)},
                                       .detail = healthDetailForStatusResponse(config, health)}};
}

LocalApiConfigResponse buildLocalApiConfigResponse(const Config& config) {
    auto peers = std::vector<LocalApiConfigPeer>{};
    peers.reserve(config.peers.size());
    for (const auto& peer : config.peers) {
        peers.push_back(LocalApiConfigPeer{.id = peer.id, .address = peer.address});
    }

    return LocalApiConfigResponse{
        .node_id = config.node_id,
        .priority = config.priority,
        .vip = LocalApiConfigVip{.address = config.vip.address,
                                 .interface = config.vip.interface},
        .heartbeat = LocalApiConfigHeartbeat{.bind = config.heartbeat.bind,
                                             .interval_ms = config.heartbeat.interval_ms,
                                             .timeout_ms = config.heartbeat.timeout_ms},
        .health = LocalApiConfigHealth{.command_redacted = !config.health.command.empty(),
                                       .interval_ms = config.health.interval_ms,
                                       .timeout_ms = config.health.timeout_ms},
        .election = LocalApiConfigElection{.require_quorum = config.election.require_quorum,
                                           .preempt = config.election.preempt},
        .api = LocalApiConfigApi{.enabled = config.api.enabled,
                                 .bind = config.api.bind,
                                 .read_only = config.api.read_only},
        .mutation_safety =
            LocalApiConfigMutationSafety{
                .allow_network_mutation = config.mutation_safety.allow_network_mutation},
        .peers = std::move(peers)};
}

LocalApiConfigValidateResponse buildLocalApiConfigValidateResponse(
    const LocalApiConfigValidateRequest& request) {
    if (request.format != "toml") {
        return LocalApiConfigValidateResponse{
            .outcome = LocalApiConfigValidateOutcome::RequestError,
            .valid = false,
            .errors = {},
            .error_code = "unsupported_format",
            .error_message = "config validation format must be toml"};
    }

    if (request.config.empty()) {
        return LocalApiConfigValidateResponse{.outcome = LocalApiConfigValidateOutcome::RequestError,
                                              .valid = false,
                                              .errors = {},
                                              .error_code = "missing_config",
                                              .error_message = "config must not be empty"};
    }

    try {
        const auto candidate = loadConfigFromTomlString(request.config);
        auto errors = candidate.validate();
        return LocalApiConfigValidateResponse{.outcome = LocalApiConfigValidateOutcome::Completed,
                                              .valid = errors.empty(),
                                              .errors = std::move(errors),
                                              .error_code = {},
                                              .error_message = {}};
    } catch (const ConfigParseError& error) {
        return LocalApiConfigValidateResponse{.outcome = LocalApiConfigValidateOutcome::RequestError,
                                              .valid = false,
                                              .errors = {},
                                              .error_code = "invalid_toml",
                                              .error_message = error.what()};
    } catch (const ConfigDecodeError& error) {
        return LocalApiConfigValidateResponse{.outcome = LocalApiConfigValidateOutcome::RequestError,
                                              .valid = false,
                                              .errors = {},
                                              .error_code = "invalid_config_shape",
                                              .error_message = error.what()};
    } catch (const std::exception& error) {
        return LocalApiConfigValidateResponse{.outcome = LocalApiConfigValidateOutcome::RequestError,
                                              .valid = false,
                                              .errors = {},
                                              .error_code = "validation_failed",
                                              .error_message = error.what()};
    }
}

LocalApiEventsResponse buildLocalApiEventsResponse() {
    return LocalApiEventsResponse{};
}

LocalApiEventsResponse buildLocalApiEventsResponse(std::vector<LocalApiEvent> events) {
    return LocalApiEventsResponse{.events = std::move(events)};
}

LocalApiEvent buildLocalApiLifecycleEvent(const std::uint64_t sequence,
                                          const DaemonLifecycleResult& result,
                                          const RuntimeLogContext& context) {
    return LocalApiEvent{
        .sequence = sequence,
        .event = "daemon_lifecycle_result",
        .level = "info",
        .message = formatRuntimeLifecycleEvent(result, context),
        .fields =
            {
                stringField("node_id", std::string{context.node_id}),
                stringField("initial_state", std::string{toString(result.initial_state)}),
                stringField("final_state", std::string{toString(result.final_state)}),
                boolField("started", result.started),
                boolField("iteration_ran", result.iteration_ran),
                boolField("stopped", result.stopped),
                boolField("dry_run", context.dry_run),
                intField("validation_errors",
                         static_cast<std::int64_t>(result.validation_errors.size())),
                intField("vip_operations", static_cast<std::int64_t>(result.vip_operations.size())),
                boolField("local_vip_owner_known", result.local_vip_owner_known),
                boolField("local_vip_owner", result.local_vip_owner),
                stringField("local_node_state", std::string{toString(result.local_status.state)}),
                stringField("failover_action",
                            std::string{toString(result.failover_decision.action)}),
                stringField("detail", result.detail),
            }};
}

LocalApiEvent buildLocalApiVipOperationEvent(const std::uint64_t sequence,
                                             const VipOperationResult& result,
                                             const std::size_t zero_based_index) {
    return LocalApiEvent{
        .sequence = sequence,
        .event = "vip_operation",
        .level = "info",
        .message = formatRuntimeVipOperationEvent(result, zero_based_index),
        .fields =
            {
                intField("zero_based_index", static_cast<std::int64_t>(zero_based_index)),
                stringField("operation", std::string{toString(result.request.type)}),
                stringField("address", result.request.address),
                stringField("interface", result.request.interface),
                boolField("request_dry_run", result.request.dry_run),
                boolField("result_dry_run", result.dry_run),
                boolField("success", result.success),
                intField("commands", static_cast<std::int64_t>(result.commands.size())),
            }};
}

std::string serializeLocalApiStatusResponse(const LocalApiStatusResponse& response) {
    auto output = std::ostringstream{};
    output << "{\"node\":{\"id\":" << jsonString(response.node.id)
           << ",\"priority\":" << response.node.priority
           << ",\"state\":" << jsonString(response.node.state)
           << ",\"healthy\":" << jsonBool(response.node.healthy)
           << "},\"vip\":{\"address\":" << jsonString(response.vip.address)
           << ",\"interface\":" << jsonString(response.vip.interface)
           << ",\"local_owner\":" << jsonBool(response.vip.local_owner)
           << "},\"lifecycle\":{\"initial_state\":"
           << jsonString(response.lifecycle.initial_state)
           << ",\"final_state\":" << jsonString(response.lifecycle.final_state)
           << ",\"detail\":" << jsonString(response.lifecycle.detail)
           << ",\"dry_run\":" << jsonBool(response.lifecycle.dry_run)
           << ",\"started\":" << jsonBool(response.lifecycle.started)
           << ",\"iteration_ran\":" << jsonBool(response.lifecycle.iteration_ran)
           << ",\"stopped\":" << jsonBool(response.lifecycle.stopped)
           << "},\"heartbeat\":{\"bind\":" << jsonString(response.heartbeat.bind)
           << ",\"interval_ms\":" << response.heartbeat.interval_ms
           << ",\"timeout_ms\":" << response.heartbeat.timeout_ms
           << ",\"peers_observed\":" << response.heartbeat.peers_observed
           << "},\"health\":{\"status\":" << jsonString(response.health.status)
           << ",\"detail\":" << jsonString(response.health.detail) << "}}";
    return output.str();
}

std::string serializeLocalApiConfigResponse(const LocalApiConfigResponse& response) {
    auto output = std::ostringstream{};
    output << "{\"node_id\":" << jsonString(response.node_id)
           << ",\"priority\":" << response.priority
           << ",\"vip\":{\"address\":" << jsonString(response.vip.address)
           << ",\"interface\":" << jsonString(response.vip.interface)
           << "},\"heartbeat\":{\"bind\":" << jsonString(response.heartbeat.bind)
           << ",\"interval_ms\":" << response.heartbeat.interval_ms
           << ",\"timeout_ms\":" << response.heartbeat.timeout_ms
           << "},\"health\":{\"command_redacted\":"
           << jsonBool(response.health.command_redacted)
           << ",\"interval_ms\":" << response.health.interval_ms
           << ",\"timeout_ms\":" << response.health.timeout_ms
           << "},\"election\":{\"require_quorum\":"
           << jsonBool(response.election.require_quorum)
           << ",\"preempt\":" << jsonBool(response.election.preempt)
           << "},\"api\":{\"enabled\":" << jsonBool(response.api.enabled)
           << ",\"bind\":" << jsonString(response.api.bind)
           << ",\"read_only\":" << jsonBool(response.api.read_only)
           << "},\"mutation_safety\":{\"allow_network_mutation\":"
           << jsonBool(response.mutation_safety.allow_network_mutation)
           << "},\"peers\":[";
    for (std::size_t index = 0; index < response.peers.size(); ++index) {
        if (index > 0) {
            output << ',';
        }
        output << "{\"id\":" << jsonString(response.peers.at(index).id)
               << ",\"address\":" << jsonString(response.peers.at(index).address) << '}';
    }
    output << "]}";
    return output.str();
}

std::string serializeLocalApiConfigValidateResponse(
    const LocalApiConfigValidateResponse& response) {
    auto output = std::ostringstream{};
    output << "{\"outcome\":" << jsonString(toString(response.outcome))
           << ",\"valid\":" << jsonBool(response.valid)
           << ",\"errors\":" << stringVectorJson(response.errors)
           << ",\"error_code\":" << jsonString(response.error_code)
           << ",\"error_message\":" << jsonString(response.error_message) << '}';
    return output.str();
}

std::string serializeLocalApiEventsResponse(const LocalApiEventsResponse& response) {
    auto output = std::ostringstream{};
    output << "{\"events\":[";
    for (std::size_t event_index = 0; event_index < response.events.size(); ++event_index) {
        if (event_index > 0) {
            output << ',';
        }
        const auto& event = response.events.at(event_index);
        output << "{\"sequence\":" << event.sequence
               << ",\"event\":" << jsonString(event.event)
               << ",\"level\":" << jsonString(event.level)
               << ",\"message\":" << jsonString(event.message)
               << ",\"fields\":{";
        for (std::size_t field_index = 0; field_index < event.fields.size(); ++field_index) {
            if (field_index > 0) {
                output << ',';
            }
            output << jsonString(event.fields.at(field_index).name) << ':'
                   << eventFieldValueJson(event.fields.at(field_index).value);
        }
        output << "}}";
    }
    output << "]}";
    return output.str();
}

LocalApiHttpResponse buildLocalApiHttpResponse(const LocalApiHttpRequest& request,
                                               const LocalApiHttpSnapshot& snapshot) {
    const auto path = trimQueryString(request.path);
    if (path == "/api/v1/status") {
        if (request.method != "GET") {
            return jsonResponse(405, errorEnvelope("method_not_allowed",
                                                   "endpoint only supports GET"));
        }
        return jsonResponse(200, serializeLocalApiStatusResponse(snapshot.status));
    }

    if (path == "/api/v1/config") {
        if (request.method != "GET") {
            return jsonResponse(405, errorEnvelope("method_not_allowed",
                                                   "endpoint only supports GET"));
        }
        return jsonResponse(200, serializeLocalApiConfigResponse(snapshot.config));
    }

    if (path == "/api/v1/events") {
        if (request.method != "GET") {
            return jsonResponse(405, errorEnvelope("method_not_allowed",
                                                   "endpoint only supports GET"));
        }
        return jsonResponse(200, serializeLocalApiEventsResponse(snapshot.events));
    }

    if (path == "/api/v1/config/validate") {
        if (request.method != "POST") {
            return jsonResponse(405, errorEnvelope("method_not_allowed",
                                                   "endpoint only supports POST"));
        }

        const auto format = parseJsonStringValue(request.body, "format");
        const auto config = parseJsonStringValue(request.body, "config");
        if (format.empty() || config.empty()) {
            return jsonResponse(400, errorEnvelope("bad_request",
                                                   "request body must include format and config strings"));
        }

        const auto validation = buildLocalApiConfigValidateResponse(
            LocalApiConfigValidateRequest{.format = format, .config = config});
        if (validation.outcome == LocalApiConfigValidateOutcome::RequestError) {
            return jsonResponse(400, serializeLocalApiConfigValidateResponse(validation));
        }
        return jsonResponse(200, serializeLocalApiConfigValidateResponse(validation));
    }

    return jsonResponse(404, errorEnvelope("not_found", "endpoint not found"));
}

LocalApiHttpServeResult serveLocalApiHttpWithMode(const std::string_view bind,
                                                  const LocalApiHttpSnapshot& snapshot,
                                                  const bool serve_once,
                                                  ShutdownSignalState* shutdown_state) {
    const auto parsed_bind = parseBind(bind);
    if (!parsed_bind.has_value()) {
        return LocalApiHttpServeResult{.started = false,
                                       .served_request = false,
                                       .error = "local API bind must be host:port"};
    }

    const auto server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return LocalApiHttpServeResult{.started = false,
                                       .served_request = false,
                                       .error = std::strerror(errno)};
    }

    const auto close_server = std::unique_ptr<int, void (*)(int*)>{
        new int{server_fd}, [](int* fd) {
            closeFd(*fd);
            delete fd;
        }};

    auto reuse = int{1};
    static_cast<void>(
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));

    auto address = sockaddr_in{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(parsed_bind->second));
    if (::inet_pton(AF_INET, parsed_bind->first.c_str(), &address.sin_addr) != 1) {
        return LocalApiHttpServeResult{.started = false,
                                       .served_request = false,
                                       .error = "local API bind host must be an IPv4 address"};
    }

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return LocalApiHttpServeResult{.started = false,
                                       .served_request = false,
                                       .error = std::strerror(errno)};
    }

    if (::listen(server_fd, 8) != 0) {
        return LocalApiHttpServeResult{.started = false,
                                       .served_request = false,
                                       .error = std::strerror(errno)};
    }

    auto served_request = false;
    while (true) {
        if (shutdownRequested(shutdown_state)) {
            return LocalApiHttpServeResult{.started = true,
                                           .served_request = served_request,
                                           .error = ""};
        }

        auto poll_fd = pollfd{.fd = server_fd, .events = POLLIN, .revents = 0};
        const auto poll_result = ::poll(&poll_fd, 1, kAcceptPollTimeoutMs);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return LocalApiHttpServeResult{.started = true,
                                           .served_request = served_request,
                                           .error = std::strerror(errno)};
        }
        if (poll_result == 0) {
            continue;
        }
        if ((poll_fd.revents & POLLIN) == 0) {
            continue;
        }

        const auto client_fd = ::accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return LocalApiHttpServeResult{.started = true,
                                           .served_request = served_request,
                                           .error = std::strerror(errno)};
        }

        const auto close_client = std::unique_ptr<int, void (*)(int*)>{
            new int{client_fd}, [](int* fd) {
                closeFd(*fd);
                delete fd;
            }};

        const auto raw_request = readRequestBytes(client_fd);
        const auto parsed_request = parseHttpRequest(raw_request);
        const auto response =
            parsed_request.has_value()
                ? buildLocalApiHttpResponse(*parsed_request, snapshot)
                : jsonResponse(400,
                               errorEnvelope("bad_request", "request must be valid HTTP"));
        const auto bytes = httpResponseBytes(response);
        static_cast<void>(::send(client_fd, bytes.data(), bytes.size(), MSG_NOSIGNAL));
        served_request = true;
        if (serve_once) {
            return LocalApiHttpServeResult{.started = true,
                                           .served_request = served_request,
                                           .error = ""};
        }
    }
}

LocalApiHttpServeResult serveLocalApiHttpOnce(const std::string_view bind,
                                              const LocalApiHttpSnapshot& snapshot) {
    return serveLocalApiHttpWithMode(bind, snapshot, true, nullptr);
}

LocalApiHttpServeResult serveLocalApiHttp(const std::string_view bind,
                                          const LocalApiHttpSnapshot& snapshot) {
    return serveLocalApiHttpWithMode(bind, snapshot, false, nullptr);
}

LocalApiHttpServeResult serveLocalApiHttp(const std::string_view bind,
                                          const LocalApiHttpSnapshot& snapshot,
                                          ShutdownSignalState* shutdown_state) {
    return serveLocalApiHttpWithMode(bind, snapshot, false, shutdown_state);
}

} // namespace easyfailover
