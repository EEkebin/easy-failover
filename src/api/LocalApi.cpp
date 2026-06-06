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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <optional>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace easyfailover {

namespace {

constexpr auto kAcceptPollTimeoutMs = 250;
constexpr auto kReadPollTimeoutMs = 1000;
constexpr auto kMaxApplyBodyBytes = std::size_t{1U << 20U};
constexpr auto kMaxRequestBytes = kMaxApplyBodyBytes + 8192U;

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

[[nodiscard]] std::string authErrorEnvelope(const std::string_view code,
                                            const std::string_view message) {
    // Auth failures intentionally omit the details array so the response never leaks why a token
    // was rejected beyond the missing/invalid distinction.
    auto output = std::ostringstream{};
    output << "{\"error\":{\"code\":" << jsonString(code)
           << ",\"message\":" << jsonString(message) << "}}";
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
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 500:
        return "Internal Server Error";
    default:
        return "Internal Server Error";
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
        case 'u': {
            if (index + 4 >= body.size()) {
                return {};
            }
            auto code_point = std::uint32_t{0};
            for (auto hex_i = std::size_t{0}; hex_i < 4; ++hex_i) {
                const auto hex_ch = body.at(++index);
                std::uint32_t nibble = 0;
                if (hex_ch >= '0' && hex_ch <= '9') {
                    nibble = static_cast<std::uint32_t>(hex_ch - '0');
                } else if (hex_ch >= 'a' && hex_ch <= 'f') {
                    nibble = static_cast<std::uint32_t>(hex_ch - 'a') + 10U;
                } else if (hex_ch >= 'A' && hex_ch <= 'F') {
                    nibble = static_cast<std::uint32_t>(hex_ch - 'A') + 10U;
                } else {
                    return {};
                }
                code_point = (code_point << 4U) | nibble;
            }
            if (code_point < 0x80U) {
                output.push_back(static_cast<char>(code_point));
            } else if (code_point < 0x800U) {
                output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
                output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            } else {
                output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
                output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
                output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            }
            break;
        }
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

    const auto headers_end = raw.find("\r\n\r\n");
    const auto headers_limit = headers_end == std::string::npos ? raw.size() : headers_end;
    constexpr auto kAuthHeader = std::string_view{"authorization:"};
    auto line_start = request_line_end + 2;
    while (line_start < headers_limit) {
        auto line_end = raw.find("\r\n", line_start);
        if (line_end == std::string::npos || line_end > headers_limit) {
            line_end = headers_limit;
        }
        const auto line = std::string_view{raw}.substr(line_start, line_end - line_start);
        auto lowered = std::string{};
        lowered.reserve(kAuthHeader.size());
        for (std::size_t index = 0; index < line.size() && index < kAuthHeader.size(); ++index) {
            lowered.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(line.at(index)))));
        }
        if (lowered.rfind(kAuthHeader, 0) == 0) {
            const auto value = line.substr(kAuthHeader.size());
            const auto value_start = value.find_first_not_of(" \t");
            if (value_start != std::string_view::npos) {
                request.authorization = std::string{value.substr(value_start)};
            }
        }
        if (line_end == headers_limit) {
            break;
        }
        line_start = line_end + 2;
    }

    if (headers_end != std::string::npos) {
        request.body = raw.substr(headers_end + 4);
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
    while (data.size() < kMaxRequestBytes) {
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

// Atomically replaces the file at target_path with new_contents. Writes a uniquely-named temp file
// in the same directory, fsyncs it, then rename()s it over the target so a reader never observes a
// partially-written config. Before replacing, copies the previous config (if any) to backup_path.
// Returns false (with no partial target write) on any I/O failure.
[[nodiscard]] bool atomicWriteConfigWithBackup(const std::string& target_path,
                                               const std::string& new_contents,
                                               const std::string& backup_path) {
    namespace fs = std::filesystem;
    auto error = std::error_code{};
    const auto target = fs::path{target_path};
    auto directory = target.parent_path();
    if (directory.empty()) {
        directory = fs::path{"."};
    }

    // Back up the previous config first, so the backup always reflects the config being replaced.
    if (fs::exists(target, error) && !error) {
        fs::copy_file(target, fs::path{backup_path}, fs::copy_options::overwrite_existing, error);
        if (error) {
            return false;
        }
    }

    auto random_device = std::random_device{};
    const auto temp_path =
        (directory / (target.filename().string() + ".tmp." +
                      std::to_string(random_device()) + std::to_string(::getpid())))
            .string();

    const auto temp_fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (temp_fd < 0) {
        return false;
    }

    auto cleanup_temp = true;
    const auto remove_temp = [&] {
        if (cleanup_temp) {
            ::unlink(temp_path.c_str());
        }
    };

    auto written = std::size_t{0};
    while (written < new_contents.size()) {
        const auto chunk =
            ::write(temp_fd, new_contents.data() + written, new_contents.size() - written);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            closeFd(temp_fd);
            remove_temp();
            return false;
        }
        written += static_cast<std::size_t>(chunk);
    }

    if (::fsync(temp_fd) != 0) {
        closeFd(temp_fd);
        remove_temp();
        return false;
    }
    closeFd(temp_fd);

    if (::rename(temp_path.c_str(), target_path.c_str()) != 0) {
        remove_temp();
        return false;
    }
    cleanup_temp = false;

    // Best-effort durability of the rename via the containing directory.
    const auto dir_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        static_cast<void>(::fsync(dir_fd));
        closeFd(dir_fd);
    }

    return true;
}

} // namespace

bool constantTimeTokenEquals(const std::string_view presented, const std::string_view expected) {
    // Compare over the fixed length of the expected token. We always iterate the full expected
    // length and never break early, so the running time does not depend on the position of the
    // first mismatch. A length difference is folded into the accumulator so unequal-length inputs
    // also fail without revealing how many bytes matched.
    auto difference = static_cast<unsigned int>(presented.size() ^ expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto presented_byte =
            index < presented.size() ? static_cast<unsigned char>(presented[index]) : 0U;
        const auto expected_byte = static_cast<unsigned char>(expected[index]);
        difference |= static_cast<unsigned int>(presented_byte ^ expected_byte);
    }
    return difference == 0U;
}

std::optional<std::string> loadApiAuthToken(const std::string& path) {
    auto stream = std::ifstream{path, std::ios::binary};
    if (!stream.is_open()) {
        return std::nullopt;
    }

    auto buffer = std::ostringstream{};
    buffer << stream.rdbuf();
    if (stream.bad()) {
        return std::nullopt;
    }

    auto token = buffer.str();
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r' ||
                              token.back() == ' ' || token.back() == '\t')) {
        token.pop_back();
    }
    return token;
}

std::optional<std::string> parseBearerToken(const std::string_view authorization_header) {
    constexpr auto kScheme = std::string_view{"Bearer "};
    if (authorization_header.size() <= kScheme.size()) {
        return std::nullopt;
    }
    // Scheme name is case-insensitive per RFC 7235.
    for (std::size_t index = 0; index < kScheme.size() - 1; ++index) {
        if (std::tolower(static_cast<unsigned char>(authorization_header[index])) !=
            std::tolower(static_cast<unsigned char>(kScheme[index]))) {
            return std::nullopt;
        }
    }
    if (authorization_header[kScheme.size() - 1] != ' ') {
        return std::nullopt;
    }

    auto token = std::string{authorization_header.substr(kScheme.size())};
    const auto first = token.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return std::nullopt;
    }
    const auto last = token.find_last_not_of(" \t");
    token = token.substr(first, last - first + 1);
    if (token.empty()) {
        return std::nullopt;
    }
    return token;
}

LocalApiStartupResult evaluateLocalApiStartup(const ApiConfig& config) {
    if (!config.enabled) {
        return LocalApiStartupResult{.state = LocalApiStartupState::Disabled,
                                     .bind = config.bind,
                                     .detail = "local API disabled"};
    }

    if (config.read_only) {
        return LocalApiStartupResult{.state = LocalApiStartupState::Ready,
                                     .bind = config.bind,
                                     .detail = "local API read-only listener ready"};
    }

    // Write mode: fail closed unless a readable, non-empty token file is configured.
    if (config.auth_token_file.empty()) {
        return LocalApiStartupResult{
            .state = LocalApiStartupState::Rejected,
            .bind = config.bind,
            .detail = "write mode requires a readable api.auth_token_file"};
    }

    const auto token = loadApiAuthToken(config.auth_token_file);
    if (!token.has_value()) {
        return LocalApiStartupResult{
            .state = LocalApiStartupState::Rejected,
            .bind = config.bind,
            .detail = "write mode requires a readable api.auth_token_file"};
    }
    if (token->empty()) {
        return LocalApiStartupResult{
            .state = LocalApiStartupState::Rejected,
            .bind = config.bind,
            .detail = "write mode requires a non-empty api.auth_token_file"};
    }

    return LocalApiStartupResult{.state = LocalApiStartupState::Ready,
                                 .bind = config.bind,
                                 .detail = "local API write listener ready"};
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
                                 .read_only = config.api.read_only,
                                 .auth_token_configured = !config.api.auth_token_file.empty()},
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

namespace {

[[nodiscard]] LocalApiConfigApplyResult authFailureApplyResult(
    const LocalApiConfigApplyOutcome outcome,
    const int status_code,
    const std::string_view audit_reason,
    const std::string_view audit_detail) {
    return LocalApiConfigApplyResult{.outcome = outcome,
                                     .status_code = status_code,
                                     .applied = false,
                                     .errors = {},
                                     .applied_path = {},
                                     .backup_path = {},
                                     .error_code = {},
                                     .error_message = {},
                                     .audit_outcome = "rejected",
                                     .audit_reason = std::string{audit_reason},
                                     .audit_detail = std::string{audit_detail}};
}

[[nodiscard]] LocalApiConfigApplyResult requestErrorApplyResult(const std::string_view error_code,
                                                                const std::string_view error_message,
                                                                const std::string_view audit_detail) {
    return LocalApiConfigApplyResult{.outcome = LocalApiConfigApplyOutcome::RequestError,
                                     .status_code = 400,
                                     .applied = false,
                                     .errors = {},
                                     .applied_path = {},
                                     .backup_path = {},
                                     .error_code = std::string{error_code},
                                     .error_message = std::string{error_message},
                                     .audit_outcome = "rejected",
                                     .audit_reason = "bad_request",
                                     .audit_detail = std::string{audit_detail}};
}

} // namespace

LocalApiConfigApplyResult buildLocalApiConfigApplyResult(const LocalApiHttpRequest& request,
                                                         const LocalApiWriteContext& write_context) {
    constexpr auto kMaxBodyBytes = kMaxApplyBodyBytes;

    // Authentication first: never inspect the body before the token is verified.
    if (request.authorization.empty()) {
        return authFailureApplyResult(LocalApiConfigApplyOutcome::Unauthorized, 401, "unauthorized",
                                      "missing bearer token");
    }
    const auto presented = parseBearerToken(request.authorization);
    if (!presented.has_value()) {
        return authFailureApplyResult(LocalApiConfigApplyOutcome::Unauthorized, 401, "unauthorized",
                                      "malformed authorization header");
    }
    if (write_context.auth_token.empty() ||
        !constantTimeTokenEquals(*presented, write_context.auth_token)) {
        return authFailureApplyResult(LocalApiConfigApplyOutcome::Forbidden, 403, "forbidden",
                                      "invalid credentials");
    }

    if (request.body.size() > kMaxBodyBytes) {
        return requestErrorApplyResult("payload_too_large", "request body exceeds size limit",
                                       "request body exceeds size limit");
    }

    const auto format = parseJsonStringValue(request.body, "format");
    const auto config_text = parseJsonStringValue(request.body, "config");
    if (format.empty() || config_text.empty()) {
        return requestErrorApplyResult("bad_request",
                                       "request body must include format and config strings",
                                       "missing format or config");
    }

    const auto validation = buildLocalApiConfigValidateResponse(
        LocalApiConfigValidateRequest{.format = format, .config = config_text});
    if (validation.outcome == LocalApiConfigValidateOutcome::RequestError) {
        return requestErrorApplyResult(validation.error_code, validation.error_message,
                                       "config request error");
    }

    if (!validation.valid) {
        // Validation failures are not request errors: report applied=false with errors, 200.
        return LocalApiConfigApplyResult{.outcome = LocalApiConfigApplyOutcome::ValidationFailed,
                                         .status_code = 200,
                                         .applied = false,
                                         .errors = validation.errors,
                                         .applied_path = {},
                                         .backup_path = {},
                                         .error_code = {},
                                         .error_message = {},
                                         .audit_outcome = "rejected",
                                         .audit_reason = "validation_failed",
                                         .audit_detail = "submitted config failed validation"};
    }

    if (write_context.config_path.empty()) {
        return LocalApiConfigApplyResult{.outcome = LocalApiConfigApplyOutcome::InternalError,
                                         .status_code = 500,
                                         .applied = false,
                                         .errors = {},
                                         .applied_path = {},
                                         .backup_path = {},
                                         .error_code = "internal_error",
                                         .error_message = "no config path configured for apply",
                                         .audit_outcome = "error",
                                         .audit_reason = "internal_error",
                                         .audit_detail = "no config path configured"};
    }

    const auto backup_path = write_context.config_path + ".bak";
    if (!atomicWriteConfigWithBackup(write_context.config_path, config_text, backup_path)) {
        return LocalApiConfigApplyResult{.outcome = LocalApiConfigApplyOutcome::InternalError,
                                         .status_code = 500,
                                         .applied = false,
                                         .errors = {},
                                         .applied_path = {},
                                         .backup_path = {},
                                         .error_code = "internal_error",
                                         .error_message = "failed to persist config",
                                         .audit_outcome = "error",
                                         .audit_reason = "internal_error",
                                         .audit_detail = "failed to persist config"};
    }

    return LocalApiConfigApplyResult{.outcome = LocalApiConfigApplyOutcome::Applied,
                                     .status_code = 200,
                                     .applied = true,
                                     .errors = {},
                                     .applied_path = write_context.config_path,
                                     .backup_path = backup_path,
                                     .error_code = {},
                                     .error_message = {},
                                     .audit_outcome = "applied",
                                     .audit_reason = "ok",
                                     .audit_detail = "config persisted; takes effect on restart"};
}

std::string serializeLocalApiConfigApplyResult(const LocalApiConfigApplyResult& result) {
    auto output = std::ostringstream{};
    output << "{\"applied\":" << jsonBool(result.applied)
           << ",\"errors\":" << stringVectorJson(result.errors)
           << ",\"applied_path\":" << jsonString(result.applied_path)
           << ",\"backup_path\":" << jsonString(result.backup_path)
           << ",\"effective\":" << jsonString("daemon restart or reload required")
           << ",\"detail\":" << jsonString(result.applied
                                               ? "config persisted and previous config backed up"
                                               : "config not applied")
           << "}";
    return output.str();
}

std::string formatLocalApiWriteAttemptEvent(const LocalApiHttpRequest& request,
                                            const LocalApiConfigApplyResult& result) {
    auto output = std::ostringstream{};
    output << "event=api_write_attempt"
           << " actor=" << jsonString("bearer-token")
           << " outcome=" << result.audit_outcome
           << " reason=" << result.audit_reason
           << " target=" << jsonString("config_apply")
           << " method=" << request.method
           << " path=" << jsonString(trimQueryString(request.path))
           << " status=" << result.status_code
           << " dry_run=false"
           << " detail=" << jsonString(result.audit_detail);
    return output.str();
}

LocalApiEvent buildLocalApiWriteAttemptEvent(const std::uint64_t sequence,
                                             const LocalApiHttpRequest& request,
                                             const LocalApiConfigApplyResult& result) {
    return LocalApiEvent{
        .sequence = sequence,
        .event = "api_write_attempt",
        .level = result.applied ? "info" : "warn",
        .message = formatLocalApiWriteAttemptEvent(request, result),
        .fields =
            {
                stringField("actor", "bearer-token"),
                stringField("outcome", result.audit_outcome),
                stringField("reason", result.audit_reason),
                stringField("target", "config_apply"),
                stringField("method", request.method),
                stringField("path", trimQueryString(request.path)),
                intField("status", static_cast<std::int64_t>(result.status_code)),
                boolField("dry_run", false),
                stringField("detail", result.audit_detail),
            }};
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
           << ",\"auth_token_configured\":" << jsonBool(response.api.auth_token_configured)
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

LocalApiHttpResponse buildLocalApiHttpResponse(
    const LocalApiHttpRequest& request,
    const LocalApiHttpSnapshot& snapshot,
    const LocalApiWriteContext& write_context,
    const std::function<void(const LocalApiHttpRequest&, const LocalApiConfigApplyResult&)>&
        emit_audit) {
    const auto path = trimQueryString(request.path);

    if (path == "/api/v1/config/apply") {
        if (!write_context.write_enabled) {
            // In read-only mode the apply endpoint is not exposed at all.
            return jsonResponse(404, errorEnvelope("not_found", "endpoint not found"));
        }
        if (request.method != "POST") {
            return jsonResponse(405, errorEnvelope("method_not_allowed",
                                                   "endpoint only supports POST"));
        }

        const auto result = buildLocalApiConfigApplyResult(request, write_context);
        if (emit_audit) {
            emit_audit(request, result);
        }

        switch (result.outcome) {
        case LocalApiConfigApplyOutcome::Unauthorized:
            return jsonResponse(401, authErrorEnvelope("unauthorized",
                                                       "authentication required for write operations"));
        case LocalApiConfigApplyOutcome::Forbidden:
            return jsonResponse(403, authErrorEnvelope("forbidden", "invalid credentials"));
        case LocalApiConfigApplyOutcome::RequestError:
            return jsonResponse(400, errorEnvelope(result.error_code, result.error_message));
        case LocalApiConfigApplyOutcome::InternalError:
            return jsonResponse(500, errorEnvelope(result.error_code, result.error_message));
        case LocalApiConfigApplyOutcome::ValidationFailed:
        case LocalApiConfigApplyOutcome::Applied:
            return jsonResponse(200, serializeLocalApiConfigApplyResult(result));
        }
        return jsonResponse(500, errorEnvelope("internal_error", "unexpected apply outcome"));
    }

    return buildLocalApiHttpResponse(request, snapshot);
}

LocalApiHttpServeResult serveLocalApiHttpWithMode(
    const std::string_view bind,
    const LocalApiHttpSnapshotProvider& snapshot_provider,
    const bool serve_once,
    ShutdownSignalState* shutdown_state,
    const LocalApiHttpStartupObserver& startup_observer,
    const LocalApiWriteContext& write_context,
    const std::function<void(const LocalApiHttpRequest&, const LocalApiConfigApplyResult&)>&
        emit_audit) {
    auto startup_reported = false;
    const auto startupResult = [&](LocalApiHttpServeResult result) {
        if (!startup_reported && startup_observer) {
            startup_observer(result);
            startup_reported = true;
        }
        return result;
    };

    const auto parsed_bind = parseBind(bind);
    if (!parsed_bind.has_value()) {
        return startupResult(LocalApiHttpServeResult{.started = false,
                                                     .served_request = false,
                                                     .error = "local API bind must be host:port"});
    }

    const auto server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return startupResult(LocalApiHttpServeResult{.started = false,
                                                     .served_request = false,
                                                     .error = std::strerror(errno)});
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
        return startupResult(
            LocalApiHttpServeResult{.started = false,
                                    .served_request = false,
                                    .error = "local API bind host must be an IPv4 address"});
    }

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return startupResult(LocalApiHttpServeResult{.started = false,
                                                     .served_request = false,
                                                     .error = std::strerror(errno)});
    }

    if (::listen(server_fd, 8) != 0) {
        return startupResult(LocalApiHttpServeResult{.started = false,
                                                     .served_request = false,
                                                     .error = std::strerror(errno)});
    }
    startupResult(LocalApiHttpServeResult{.started = true, .served_request = false, .error = ""});

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
        const auto snapshot = snapshot_provider();
        const auto response =
            parsed_request.has_value()
                ? buildLocalApiHttpResponse(*parsed_request, snapshot, write_context, emit_audit)
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
    return serveLocalApiHttpWithMode(bind, [&snapshot] { return snapshot; }, true, nullptr, {}, {},
                                     {});
}

LocalApiHttpServeResult serveLocalApiHttp(const std::string_view bind,
                                          const LocalApiHttpSnapshot& snapshot) {
    return serveLocalApiHttpWithMode(bind, [&snapshot] { return snapshot; }, false, nullptr, {}, {},
                                     {});
}

LocalApiHttpServeResult serveLocalApiHttp(const std::string_view bind,
                                          const LocalApiHttpSnapshot& snapshot,
                                          ShutdownSignalState* shutdown_state) {
    return serveLocalApiHttpWithMode(bind, [&snapshot] { return snapshot; }, false, shutdown_state,
                                     {}, {}, {});
}

LocalApiHttpServeResult serveLocalApiHttp(
    const std::string_view bind,
    const LocalApiHttpSnapshotProvider& snapshot_provider,
    ShutdownSignalState* shutdown_state) {
    return serveLocalApiHttpWithMode(bind, snapshot_provider, false, shutdown_state, {}, {}, {});
}

LocalApiHttpServeResult serveLocalApiHttp(
    const std::string_view bind,
    const LocalApiHttpSnapshotProvider& snapshot_provider,
    ShutdownSignalState* shutdown_state,
    const LocalApiHttpStartupObserver& startup_observer) {
    return serveLocalApiHttpWithMode(bind, snapshot_provider, false, shutdown_state,
                                     startup_observer, {}, {});
}

LocalApiHttpServeResult serveLocalApiHttp(
    const std::string_view bind,
    const LocalApiHttpSnapshotProvider& snapshot_provider,
    ShutdownSignalState* shutdown_state,
    const LocalApiHttpStartupObserver& startup_observer,
    const LocalApiWriteContext& write_context,
    const std::function<void(const LocalApiHttpRequest&, const LocalApiConfigApplyResult&)>&
        emit_audit) {
    return serveLocalApiHttpWithMode(bind, snapshot_provider, false, shutdown_state,
                                     startup_observer, write_context, emit_audit);
}

} // namespace easyfailover
