#include "api/LocalApi.hpp"
#include "config/Config.hpp"
#include "core/Election.hpp"
#include "core/FailoverDecision.hpp"
#include "heartbeat/HeartbeatLoop.hpp"
#include "heartbeat/HeartbeatMessage.hpp"
#include "heartbeat/HeartbeatTransport.hpp"
#include "health/HealthCheck.hpp"
#include "platform/NetworkCommandRunner.hpp"
#include "platform/linux/LinuxHealthCommandRunner.hpp"
#include "platform/linux/LinuxVipManager.hpp"
#include "runtime/DaemonRuntime.hpp"
#include "runtime/RuntimeLog.hpp"
#include "runtime/ShutdownSignal.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using easyfailover::CandidateNode;
using easyfailover::CommandResult;
using easyfailover::Config;
using easyfailover::DaemonLifecycleRequest;
using easyfailover::DaemonLifecycleResult;
using easyfailover::DaemonLifecycleState;
using easyfailover::DaemonLoopOptions;
using easyfailover::DaemonLoopRequest;
using easyfailover::DaemonLoopStopReason;
using easyfailover::DaemonRuntimeOptions;
using easyfailover::FailoverAction;
using easyfailover::HealthCommandRunner;
using easyfailover::HealthConfig;
using easyfailover::HealthStatus;
using easyfailover::DisabledHeartbeatTransport;
using easyfailover::HeartbeatDatagram;
using easyfailover::HeartbeatMessage;
using easyfailover::HeartbeatReceiveResult;
using easyfailover::HeartbeatSendResult;
using easyfailover::HeartbeatTransport;
using easyfailover::kHeartbeatTransportDisabledError;
using easyfailover::LinuxHealthCommandRunner;
using easyfailover::LinuxVipManager;
using easyfailover::LinuxVipManagerOptions;
using easyfailover::LocalApiEvent;
using easyfailover::LocalApiEventField;
using easyfailover::LocalNodeStatus;
using easyfailover::LocalApiStartupState;
using easyfailover::LocalApiConfigValidateOutcome;
using easyfailover::LocalApiConfigValidateRequest;
using easyfailover::DryRunNetworkCommandRunner;
using easyfailover::NetworkCommandRequest;
using easyfailover::NetworkCommandResult;
using easyfailover::NetworkCommandRunner;
using easyfailover::NodeState;
using easyfailover::PeerConfig;
using easyfailover::PeerStatus;
using easyfailover::RuntimeLogContext;
using easyfailover::ShutdownSignal;
using easyfailover::ShutdownSignalState;
using easyfailover::VipOperationType;
using easyfailover::VipOperationResult;
using easyfailover::VipManager;
using easyfailover::buildLocalApiConfigResponse;
using easyfailover::buildLocalApiConfigValidateResponse;
using easyfailover::buildLocalApiEventsResponse;
using easyfailover::buildLocalApiLifecycleEvent;
using easyfailover::buildLocalApiStatusResponse;
using easyfailover::buildLocalApiVipOperationEvent;
using easyfailover::chooseHighestPriorityHealthyNode;
using easyfailover::decideFailoverAction;
using easyfailover::evaluateLocalApiStartup;
using easyfailover::evaluateHealthCheck;
using easyfailover::formatRuntimeLifecycleEvent;
using easyfailover::formatRuntimeVipOperationEvent;
using easyfailover::parseHeartbeatMessage;
using easyfailover::peerStatusFromHeartbeat;
using easyfailover::pollShutdownSignals;
using easyfailover::resetPendingShutdownSignalForTest;
using easyfailover::runDaemonLifecycleOnce;
using easyfailover::runDaemonRuntimeLoop;
using easyfailover::runHeartbeatLoopOnce;
using easyfailover::serializeHeartbeatMessage;
using easyfailover::loadConfigFromFile;
using easyfailover::loadConfigFromTomlString;

class TestRunner {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    template <typename Func>
    void run(const std::string_view name, Func&& func) {
        const auto failures_before = failures_;
        try {
            func();
            if (failures_ == failures_before) {
                std::cout << "PASS: " << name << '\n';
            } else {
                std::cerr << "FAIL: " << name << '\n';
            }
        } catch (const std::exception& error) {
            ++failures_;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures_;
            std::cerr << "FAIL: " << name << ": unknown exception\n";
        }
    }

    [[nodiscard]] int failures() const {
        return failures_;
    }

  private:
    int failures_ = 0;
};

bool contains(const std::vector<std::string>& values, const std::string_view expected) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

const LocalApiEventField* findEventField(const LocalApiEvent& event, const std::string_view name) {
    const auto match = std::find_if(event.fields.begin(), event.fields.end(), [name](const auto& field) {
        return field.name == name;
    });

    if (match == event.fields.end()) {
        return nullptr;
    }

    return &*match;
}

class FakeHealthCommandRunner final : public HealthCommandRunner {
  public:
    [[nodiscard]] CommandResult run(const std::string& command,
                                    const std::int64_t timeout_ms) override {
        was_called = true;
        observed_command = command;
        observed_timeout_ms = timeout_ms;
        return result;
    }

    CommandResult result;
    bool was_called = false;
    std::string observed_command;
    std::int64_t observed_timeout_ms = 0;
};

class FakeHeartbeatTransport final : public HeartbeatTransport {
  public:
    struct ConfiguredSendResult {
        bool sent = true;
        std::string error;
    };

    [[nodiscard]] HeartbeatSendResult send(const std::string& peer_address,
                                           const std::string& payload) override {
        send_called = true;
        sent_datagrams.push_back(HeartbeatDatagram{.peer_address = peer_address,
                                                   .payload = payload});
        sent_peer_address = peer_address;
        sent_payload = payload;
        return HeartbeatSendResult{.sent = send_result.sent,
                                   .peer_address = peer_address,
                                   .payload = payload,
                                   .error = send_result.error};
    }

    [[nodiscard]] HeartbeatReceiveResult receive(const std::int64_t timeout_ms) override {
        receive_called = true;
        observed_timeout_ms = timeout_ms;
        return HeartbeatReceiveResult{.datagram = receive_result.datagram,
                                      .timed_out = receive_result.timed_out,
                                      .timeout_ms = timeout_ms,
                                      .error = receive_result.error};
    }

    ConfiguredSendResult send_result;
    HeartbeatReceiveResult receive_result;
    bool send_called = false;
    bool receive_called = false;
    std::vector<HeartbeatDatagram> sent_datagrams;
    std::string sent_peer_address;
    std::string sent_payload;
    std::int64_t observed_timeout_ms = 0;
};

class FakeNetworkCommandRunner final : public NetworkCommandRunner {
  public:
    [[nodiscard]] NetworkCommandResult run(const NetworkCommandRequest& request) override {
        was_called = true;
        observed_request = request;
        observed_requests.push_back(request);
        return result;
    }

    NetworkCommandResult result;
    bool was_called = false;
    NetworkCommandRequest observed_request;
    std::vector<NetworkCommandRequest> observed_requests;
};

class FakeVipManager final : public VipManager {
  public:
    [[nodiscard]] VipOperationResult addVip(const std::string& address,
                                            const std::string& interface) override {
        add_called = true;
        add_address = address;
        add_interface = interface;
        operations.push_back(VipOperationType::Add);
        return VipOperationResult{.request = {.type = VipOperationType::Add,
                                              .address = address,
                                              .interface = interface,
                                              .dry_run = operation_dry_run},
                                  .success = add_success,
                                  .dry_run = operation_dry_run,
                                  .commands = {},
                                  .error = add_error};
    }

    [[nodiscard]] VipOperationResult removeVip(const std::string& address,
                                               const std::string& interface) override {
        remove_called = true;
        operations.push_back(VipOperationType::Remove);
        return VipOperationResult{.request = {.type = VipOperationType::Remove,
                                              .address = address,
                                              .interface = interface,
                                              .dry_run = operation_dry_run},
                                  .success = true,
                                  .dry_run = operation_dry_run,
                                  .commands = {},
                                  .error = ""};
    }

    [[nodiscard]] VipOperationResult announceVip(const std::string& address,
                                                 const std::string& interface) override {
        announce_called = true;
        announce_address = address;
        announce_interface = interface;
        operations.push_back(VipOperationType::Announce);
        return VipOperationResult{.request = {.type = VipOperationType::Announce,
                                              .address = address,
                                              .interface = interface,
                                              .dry_run = operation_dry_run},
                                  .success = announce_success,
                                  .dry_run = operation_dry_run,
                                  .commands = {},
                                  .error = announce_error};
    }

    bool add_called = false;
    bool remove_called = false;
    bool announce_called = false;
    std::string add_address;
    std::string add_interface;
    std::string announce_address;
    std::string announce_interface;
    bool operation_dry_run = true;
    bool add_success = true;
    bool announce_success = true;
    std::string add_error;
    std::string announce_error;
    std::vector<VipOperationType> operations;
};

Config validConfig() {
    Config config;
    config.node_id = "node-a";
    config.priority = 100;
    config.vip.address = "10.0.0.50/24";
    config.vip.interface = "eth0";
    config.heartbeat.bind = "0.0.0.0:7432";
    config.heartbeat.interval_ms = 1000;
    config.heartbeat.timeout_ms = 3000;
    config.health.command = "curl -fsS http://127.0.0.1:8080/health";
    config.health.interval_ms = 1000;
    config.health.timeout_ms = 2000;
    config.api.enabled = false;
    config.api.bind = "127.0.0.1:8743";
    config.peers = {PeerConfig{.id = "node-b", .address = "10.0.0.12:7432"}};
    return config;
}

void testValidConfigHasNoValidationErrors(TestRunner& runner) {
    const auto config = validConfig();
    runner.expect(config.validate().empty(), "valid config should not produce validation errors");
}

void testConfigDefaultsMutationSafetyDisabled(TestRunner& runner) {
    const auto config = Config{};
    runner.expect(!config.mutation_safety.allow_network_mutation,
                  "mutation safety should default to disabled network mutation");
}

void testConfigValidationRequiredFields(TestRunner& runner) {
    auto config = validConfig();
    config.node_id.clear();
    config.priority = 0;
    config.vip.address.clear();
    config.vip.interface.clear();
    config.heartbeat.bind.clear();
    config.heartbeat.interval_ms = 0;
    config.heartbeat.timeout_ms = 0;
    config.health.interval_ms = 0;
    config.health.timeout_ms = 0;
    config.peers = {PeerConfig{.id = "", .address = ""}};
    config.api.enabled = true;
    config.api.bind.clear();

    const auto errors = config.validate();
    runner.expect(contains(errors, "node_id must not be empty"), "node_id should be required");
    runner.expect(contains(errors, "priority must be positive"), "positive priority should be required");
    runner.expect(contains(errors, "vip.address must not be empty"), "VIP address should be required");
    runner.expect(contains(errors, "vip.interface must not be empty"), "VIP interface should be required");
    runner.expect(contains(errors, "heartbeat.bind must not be empty"),
                  "heartbeat bind should be required");
    runner.expect(contains(errors, "heartbeat.interval_ms must be positive"),
                  "heartbeat interval should be required");
    runner.expect(contains(errors, "heartbeat.timeout_ms must be positive"),
                  "heartbeat timeout should be required");
    runner.expect(contains(errors, "health.interval_ms must be positive"),
                  "health interval should be required");
    runner.expect(contains(errors, "health.timeout_ms must be positive"),
                  "health timeout should be required");
    runner.expect(contains(errors, "peers[].id must not be empty"), "peer id should be required");
    runner.expect(contains(errors, "peers[].address must not be empty"),
                  "peer address should be required");
    runner.expect(contains(errors, "api.bind must not be empty when api.enabled is true"),
                  "API bind should be required when API is enabled");
}

void testConfigAllowsApiWriteModeForStartupRejection(TestRunner& runner) {
    auto config = validConfig();
    config.api.enabled = true;
    config.api.read_only = false;

    runner.expect(config.validate().empty(),
                  "config validation should allow API write mode for startup rejection");
}

void testConfigParsesMutationSafetyEnabled(TestRunner& runner) {
    const auto config = loadConfigFromTomlString("node_id = \"node-a\"\n"
                                                "priority = 100\n"
                                                "\n"
                                                "[vip]\n"
                                                "address = \"10.0.0.50/24\"\n"
                                                "interface = \"eth0\"\n"
                                                "\n"
                                                "[mutation_safety]\n"
                                                "allow_network_mutation = true\n"
                                                "\n"
                                                "[[peers]]\n"
                                                "id = \"node-b\"\n"
                                                "address = \"10.0.0.12:7432\"\n");

    runner.expect(config.mutation_safety.allow_network_mutation,
                  "explicit mutation safety opt-in should parse true");
    runner.expect(config.validate().empty(),
                  "explicit mutation safety opt-in should not fail validation");
}

void testConfigParsesMutationSafetyDisabled(TestRunner& runner) {
    const auto config = loadConfigFromTomlString("node_id = \"node-a\"\n"
                                                "priority = 100\n"
                                                "\n"
                                                "[vip]\n"
                                                "address = \"10.0.0.50/24\"\n"
                                                "interface = \"eth0\"\n"
                                                "\n"
                                                "[mutation_safety]\n"
                                                "allow_network_mutation = false\n"
                                                "\n"
                                                "[[peers]]\n"
                                                "id = \"node-b\"\n"
                                                "address = \"10.0.0.12:7432\"\n");

    runner.expect(!config.mutation_safety.allow_network_mutation,
                  "explicit mutation safety disable should parse false");
    runner.expect(config.validate().empty(),
                  "explicit mutation safety disable should not fail validation");
}

void testLocalApiStartupDisabled(TestRunner& runner) {
    const auto config = validConfig();

    const auto result = evaluateLocalApiStartup(config.api);

    runner.expect(result.state == LocalApiStartupState::Disabled,
                  "disabled API config should produce disabled startup state");
    runner.expect(result.bind == config.api.bind,
                  "disabled API startup should preserve configured bind for logging");
    runner.expect(result.detail == "local API disabled",
                  "disabled API startup should report stable detail");
}

void testLocalApiStartupReadyForReadOnlyEnabledConfig(TestRunner& runner) {
    auto config = validConfig();
    config.api.enabled = true;
    config.api.read_only = true;

    const auto result = evaluateLocalApiStartup(config.api);

    runner.expect(result.state == LocalApiStartupState::Ready,
                  "read-only enabled API config should be ready for future listener");
    runner.expect(result.bind == config.api.bind,
                  "read-only API startup should preserve configured bind");
    runner.expect(result.detail == "local API skeleton ready; listener not implemented",
                  "read-only API startup should report stable skeleton detail");
}

void testLocalApiStartupRejectsWriteMode(TestRunner& runner) {
    auto config = validConfig();
    config.api.enabled = true;
    config.api.read_only = false;

    const auto result = evaluateLocalApiStartup(config.api);

    runner.expect(result.state == LocalApiStartupState::Rejected,
                  "enabled write-mode API config should be rejected by startup logic");
    runner.expect(result.bind == config.api.bind,
                  "rejected API startup should preserve configured bind");
    runner.expect(result.detail ==
                      "local API write mode requires authentication and write-behavior design",
                  "rejected API startup should report stable safety detail");
}

void testLocalApiConfigValidateOutcomeToString(TestRunner& runner) {
    runner.expect(easyfailover::toString(LocalApiConfigValidateOutcome::Completed) == "completed",
                  "completed config validation outcome should have stable string");
    runner.expect(easyfailover::toString(LocalApiConfigValidateOutcome::RequestError) ==
                      "request_error",
                  "request error config validation outcome should have stable string");
}

void testLocalApiStatusResponseMapsCurrentRuntimeState(TestRunner& runner) {
    const auto config = validConfig();
    const auto lifecycle = DaemonLifecycleResult{
        .initial_state = DaemonLifecycleState::Stopped,
        .final_state = DaemonLifecycleState::Stopped,
        .started = true,
        .iteration_ran = true,
        .stopped = true,
        .validation_errors = {},
        .vip_operations = {},
        .detail = "dry-run lifecycle iteration completed"};
    const auto health = easyfailover::HealthCheckResult{
        .status = HealthStatus::Healthy,
        .detail = "health command exited successfully"};

    const auto response = buildLocalApiStatusResponse(config, lifecycle, health, NodeState::Backup,
                                                      true, 2);

    runner.expect(response.node.id == config.node_id, "status response should include node id");
    runner.expect(response.node.priority == config.priority,
                  "status response should include node priority");
    runner.expect(response.node.state == "backup", "status response should include node state");
    runner.expect(response.node.healthy, "status response should map healthy status");
    runner.expect(response.vip.address == config.vip.address,
                  "status response should include VIP address");
    runner.expect(response.vip.interface == config.vip.interface,
                  "status response should include VIP interface");
    runner.expect(!response.vip.local_owner,
                  "status response should conservatively avoid claiming VIP ownership");
    runner.expect(response.lifecycle.initial_state == "stopped",
                  "status response should include lifecycle initial state");
    runner.expect(response.lifecycle.final_state == "stopped",
                  "status response should include lifecycle final state");
    runner.expect(response.lifecycle.detail == lifecycle.detail,
                  "status response should include lifecycle detail");
    runner.expect(response.lifecycle.dry_run, "status response should include dry-run mode");
    runner.expect(response.lifecycle.started, "status response should include started flag");
    runner.expect(response.lifecycle.iteration_ran,
                  "status response should include iteration flag");
    runner.expect(response.lifecycle.stopped, "status response should include stopped flag");
    runner.expect(response.heartbeat.bind == config.heartbeat.bind,
                  "status response should include heartbeat bind");
    runner.expect(response.heartbeat.interval_ms == config.heartbeat.interval_ms,
                  "status response should include heartbeat interval");
    runner.expect(response.heartbeat.timeout_ms == config.heartbeat.timeout_ms,
                  "status response should include heartbeat timeout");
    runner.expect(response.heartbeat.peers_observed == 2,
                  "status response should include observed peer count");
    runner.expect(response.health.status == "healthy",
                  "status response should include health status");
    runner.expect(response.health.detail == "health command detail redacted",
                  "status response should redact configured health command detail");
}

void testLocalApiStatusResponseOmitsSensitiveHealthCommand(TestRunner& runner) {
    auto config = validConfig();
    config.health.command = "curl -H 'Authorization: Bearer secret' http://127.0.0.1/health";
    const auto lifecycle = DaemonLifecycleResult{};
    const auto health = easyfailover::HealthCheckResult{.status = HealthStatus::Unhealthy,
                                                        .detail = "Authorization Bearer secret"};

    const auto response = buildLocalApiStatusResponse(config, lifecycle, health, NodeState::Fault,
                                                      false);

    runner.expect(response.node.state == "fault", "status response should include fault state");
    runner.expect(!response.node.healthy, "status response should map unhealthy status");
    runner.expect(response.health.detail.find("Authorization") == std::string::npos,
                  "status response should not expose health command headers");
    runner.expect(response.health.detail.find("secret") == std::string::npos,
                  "status response should not expose health command secrets");
    runner.expect(response.health.detail == "health command detail redacted",
                  "status response should redact configured health command detail");
    runner.expect(!response.lifecycle.dry_run, "status response should include non-dry-run mode");
    runner.expect(response.heartbeat.peers_observed == 0,
                  "status response should default observed peer count to zero");
}

void testLocalApiStatusResponseKeepsNoCommandHealthDetail(TestRunner& runner) {
    auto config = validConfig();
    config.health.command.clear();
    const auto lifecycle = DaemonLifecycleResult{};
    const auto health = easyfailover::HealthCheckResult{.status = HealthStatus::Healthy,
                                                        .detail = "no health command configured"};

    const auto response = buildLocalApiStatusResponse(config, lifecycle, health, NodeState::Backup,
                                                      true);

    runner.expect(response.health.detail == health.detail,
                  "status response should preserve no-command health detail");
}

void testLocalApiConfigResponseMapsEffectiveConfig(TestRunner& runner) {
    auto config = validConfig();
    config.election.require_quorum = true;
    config.election.preempt = false;
    config.api.enabled = true;
    config.mutation_safety.allow_network_mutation = true;
    config.peers.push_back(PeerConfig{.id = "node-c", .address = "10.0.0.13:7432"});

    const auto response = buildLocalApiConfigResponse(config);

    runner.expect(response.node_id == config.node_id, "config response should include node id");
    runner.expect(response.priority == config.priority,
                  "config response should include node priority");
    runner.expect(response.vip.address == config.vip.address,
                  "config response should include VIP address");
    runner.expect(response.vip.interface == config.vip.interface,
                  "config response should include VIP interface");
    runner.expect(response.heartbeat.bind == config.heartbeat.bind,
                  "config response should include heartbeat bind");
    runner.expect(response.heartbeat.interval_ms == config.heartbeat.interval_ms,
                  "config response should include heartbeat interval");
    runner.expect(response.heartbeat.timeout_ms == config.heartbeat.timeout_ms,
                  "config response should include heartbeat timeout");
    runner.expect(response.health.interval_ms == config.health.interval_ms,
                  "config response should include health interval");
    runner.expect(response.health.timeout_ms == config.health.timeout_ms,
                  "config response should include health timeout");
    runner.expect(response.election.require_quorum == config.election.require_quorum,
                  "config response should include quorum setting");
    runner.expect(response.election.preempt == config.election.preempt,
                  "config response should include preemption setting");
    runner.expect(response.api.enabled == config.api.enabled,
                  "config response should include API enabled flag");
    runner.expect(response.api.bind == config.api.bind,
                  "config response should include API bind");
    runner.expect(response.api.read_only == config.api.read_only,
                  "config response should include API read-only flag");
    runner.expect(response.mutation_safety.allow_network_mutation ==
                      config.mutation_safety.allow_network_mutation,
                  "config response should include mutation safety gate");
    runner.expect(response.peers.size() == config.peers.size(),
                  "config response should include peers");
    runner.expect(response.peers.at(0).id == "node-b",
                  "config response should include first peer id");
    runner.expect(response.peers.at(1).address == "10.0.0.13:7432",
                  "config response should include second peer address");
}

void testLocalApiConfigResponseRedactsHealthCommand(TestRunner& runner) {
    auto config = validConfig();
    config.health.command = "curl -H 'Authorization: Bearer secret' http://127.0.0.1/health";

    const auto response = buildLocalApiConfigResponse(config);

    runner.expect(response.health.command_redacted,
                  "config response should report redacted health command");
}

void testLocalApiConfigResponseReportsNoHealthCommand(TestRunner& runner) {
    auto config = validConfig();
    config.health.command.clear();

    const auto response = buildLocalApiConfigResponse(config);

    runner.expect(!response.health.command_redacted,
                  "config response should report no health command when command is empty");
}

void testLocalApiConfigValidateAcceptsValidToml(TestRunner& runner) {
    const auto response = buildLocalApiConfigValidateResponse(
        LocalApiConfigValidateRequest{.format = "toml",
                                      .config = "node_id = \"node-a\"\n"
                                                "priority = 100\n"
                                                "\n"
                                                "[vip]\n"
                                                "address = \"10.0.0.50/24\"\n"
                                                "interface = \"eth0\"\n"
                                                "\n"
                                                "[[peers]]\n"
                                                "id = \"node-b\"\n"
                                                "address = \"10.0.0.12:7432\"\n"});

    runner.expect(response.outcome == LocalApiConfigValidateOutcome::Completed,
                  "valid TOML request should complete validation");
    runner.expect(response.valid, "valid TOML request should be valid");
    runner.expect(response.errors.empty(), "valid TOML request should not return errors");
    runner.expect(response.error_code.empty(), "valid TOML request should not set error code");
}

void testLocalApiConfigValidateReportsValidationErrors(TestRunner& runner) {
    const auto response = buildLocalApiConfigValidateResponse(
        LocalApiConfigValidateRequest{.format = "toml",
                                      .config = "node_id = \"node-a\"\n"
                                                "priority = 100\n"
                                                "\n"
                                                "[vip]\n"
                                                "address = \"10.0.0.50/24\"\n"
                                                "interface = \"eth0\"\n"});

    runner.expect(response.outcome == LocalApiConfigValidateOutcome::Completed,
                  "well-formed invalid config should complete validation");
    runner.expect(!response.valid, "invalid candidate config should report invalid");
    runner.expect(contains(response.errors, "at least one peer must be configured"),
                  "invalid candidate config should return validation errors");
    runner.expect(response.error_code.empty(),
                  "validation failure should not be modeled as request error");
}

void testLocalApiConfigValidateRejectsUnsupportedFormat(TestRunner& runner) {
    const auto response = buildLocalApiConfigValidateResponse(
        LocalApiConfigValidateRequest{.format = "json", .config = "{}"});

    runner.expect(response.outcome == LocalApiConfigValidateOutcome::RequestError,
                  "unsupported format should be request error");
    runner.expect(!response.valid, "unsupported format should not be valid");
    runner.expect(response.errors.empty(), "unsupported format should not return config errors");
    runner.expect(response.error_code == "unsupported_format",
                  "unsupported format should return stable error code");
}

void testLocalApiConfigValidateRejectsEmptyConfig(TestRunner& runner) {
    const auto response = buildLocalApiConfigValidateResponse(
        LocalApiConfigValidateRequest{.format = "toml", .config = ""});

    runner.expect(response.outcome == LocalApiConfigValidateOutcome::RequestError,
                  "empty candidate config should be request error");
    runner.expect(response.error_code == "missing_config",
                  "empty candidate config should return stable error code");
}

void testLocalApiConfigValidateRejectsMalformedToml(TestRunner& runner) {
    const auto response = buildLocalApiConfigValidateResponse(
        LocalApiConfigValidateRequest{.format = "toml", .config = "[vip\naddress = \"bad\"\n"});

    runner.expect(response.outcome == LocalApiConfigValidateOutcome::RequestError,
                  "malformed TOML should be request error");
    runner.expect(!response.valid, "malformed TOML should not be valid");
    runner.expect(response.errors.empty(), "malformed TOML should not return config errors");
    runner.expect(response.error_code == "invalid_toml",
                  "malformed TOML should return stable error code");
    runner.expect(response.error_message.find("Failed to parse candidate config") !=
                      std::string::npos,
                  "malformed TOML should include parse detail");
}

void testLocalApiConfigValidateRejectsInvalidConfigShape(TestRunner& runner) {
    const auto response = buildLocalApiConfigValidateResponse(
        LocalApiConfigValidateRequest{.format = "toml",
                                      .config = "node_id = \"node-a\"\n"
                                                "priority = \"high\"\n"
                                                "\n"
                                                "[vip]\n"
                                                "address = \"10.0.0.50/24\"\n"
                                                "interface = \"eth0\"\n"});

    runner.expect(response.outcome == LocalApiConfigValidateOutcome::RequestError,
                  "invalid config shape should be request error");
    runner.expect(response.error_code == "invalid_config_shape",
                  "invalid config shape should return stable error code");
    runner.expect(response.error_message.find("Invalid type for config key: priority") !=
                      std::string::npos,
                  "invalid config shape should include decode detail");
}

void testLocalApiConfigValidateRejectsInvalidMutationSafetyShape(TestRunner& runner) {
    const auto response = buildLocalApiConfigValidateResponse(
        LocalApiConfigValidateRequest{.format = "toml",
                                      .config = "node_id = \"node-a\"\n"
                                                "priority = 100\n"
                                                "\n"
                                                "[vip]\n"
                                                "address = \"10.0.0.50/24\"\n"
                                                "interface = \"eth0\"\n"
                                                "\n"
                                                "[mutation_safety]\n"
                                                "allow_network_mutation = \"yes\"\n"
                                                "\n"
                                                "[[peers]]\n"
                                                "id = \"node-b\"\n"
                                                "address = \"10.0.0.12:7432\"\n"});

    runner.expect(response.outcome == LocalApiConfigValidateOutcome::RequestError,
                  "invalid mutation safety shape should be request error");
    runner.expect(response.error_code == "invalid_config_shape",
                  "invalid mutation safety shape should return stable error code");
    runner.expect(response.error_message.find(
                      "Invalid type for config key: allow_network_mutation") != std::string::npos,
                  "invalid mutation safety shape should include decode detail");
}

void testLocalApiConfigValidateRejectsInvalidPeersShape(TestRunner& runner) {
    const auto response = buildLocalApiConfigValidateResponse(
        LocalApiConfigValidateRequest{.format = "toml",
                                      .config = "node_id = \"node-a\"\n"
                                                "priority = 100\n"
                                                "peers = \"node-b\"\n"
                                                "\n"
                                                "[vip]\n"
                                                "address = \"10.0.0.50/24\"\n"
                                                "interface = \"eth0\"\n"});

    runner.expect(response.outcome == LocalApiConfigValidateOutcome::RequestError,
                  "invalid peers shape should be request error");
    runner.expect(response.error_code == "invalid_config_shape",
                  "invalid peers shape should return stable error code");
    runner.expect(response.error_message.find("Invalid type for config key: peers") !=
                      std::string::npos,
                  "invalid peers shape should include decode detail");
}

void testLocalApiEventsResponseDefaultsEmpty(TestRunner& runner) {
    const auto response = buildLocalApiEventsResponse();

    runner.expect(response.events.empty(),
                  "events response should default empty before an event buffer exists");
}

void testLocalApiLifecycleEventMapsRuntimeLogFields(TestRunner& runner) {
    FakeVipManager vip_manager;
    const auto result = runDaemonLifecycleOnce(
        DaemonLifecycleRequest{.config = validConfig(),
                               .options = DaemonRuntimeOptions{.dry_run = true},
                               .initial_state = DaemonLifecycleState::Stopped},
        vip_manager);

    const auto event = buildLocalApiLifecycleEvent(
        42, result, RuntimeLogContext{.node_id = "node-a", .dry_run = true});

    runner.expect(event.sequence == 42, "lifecycle event should preserve sequence");
    runner.expect(event.event == "daemon_lifecycle_result",
                  "lifecycle event should preserve event name");
    runner.expect(event.level == "info", "lifecycle event should default to info");
    runner.expect(event.message == formatRuntimeLifecycleEvent(
                                       result,
                                       RuntimeLogContext{.node_id = "node-a", .dry_run = true}),
                  "lifecycle event should reuse stable runtime log message");

    const auto* node_id = findEventField(event, "node_id");
    runner.expect(node_id != nullptr, "lifecycle event should include node_id field");
    runner.expect(node_id != nullptr &&
                      std::get_if<std::string>(&node_id->value) != nullptr &&
                      *std::get_if<std::string>(&node_id->value) == "node-a",
                  "lifecycle node_id field should be string value");

    const auto* dry_run = findEventField(event, "dry_run");
    runner.expect(dry_run != nullptr, "lifecycle event should include dry_run field");
    runner.expect(dry_run != nullptr &&
                      std::get_if<bool>(&dry_run->value) != nullptr &&
                      *std::get_if<bool>(&dry_run->value),
                  "lifecycle dry_run field should be bool value");

    const auto* vip_operations = findEventField(event, "vip_operations");
    runner.expect(vip_operations != nullptr,
                  "lifecycle event should include vip_operations field");
    runner.expect(vip_operations != nullptr &&
                      std::get_if<std::int64_t>(&vip_operations->value) != nullptr &&
                      *std::get_if<std::int64_t>(&vip_operations->value) == 2,
                  "lifecycle vip_operations field should be integer value");
}

void testLocalApiVipOperationEventMapsRuntimeLogFields(TestRunner& runner) {
    const auto result = VipOperationResult{
        .request = {.type = VipOperationType::Announce,
                    .address = "10.0.0.50/24",
                    .interface = "eth0",
                    .dry_run = true},
        .success = false,
        .dry_run = true,
        .commands = {},
        .error = "announce failed",
    };

    const auto event = buildLocalApiVipOperationEvent(43, result, 1);

    runner.expect(event.sequence == 43, "VIP event should preserve sequence");
    runner.expect(event.event == "vip_operation", "VIP event should preserve event name");
    runner.expect(event.level == "info", "VIP event should default to info");
    runner.expect(event.message == formatRuntimeVipOperationEvent(result, 1),
                  "VIP event should reuse stable runtime log message");

    const auto* operation = findEventField(event, "operation");
    runner.expect(operation != nullptr, "VIP event should include operation field");
    runner.expect(operation != nullptr &&
                      std::get_if<std::string>(&operation->value) != nullptr &&
                      *std::get_if<std::string>(&operation->value) == "announce",
                  "VIP operation field should be string value");

    const auto* success = findEventField(event, "success");
    runner.expect(success != nullptr, "VIP event should include success field");
    runner.expect(success != nullptr &&
                      std::get_if<bool>(&success->value) != nullptr &&
                      !*std::get_if<bool>(&success->value),
                  "VIP success field should be bool value");

    const auto* commands = findEventField(event, "commands");
    runner.expect(commands != nullptr, "VIP event should include command count field");
    runner.expect(commands != nullptr &&
                      std::get_if<std::int64_t>(&commands->value) != nullptr &&
                      *std::get_if<std::int64_t>(&commands->value) == 0,
                  "VIP commands field should be integer value");
    runner.expect(findEventField(event, "error") == nullptr,
                  "VIP structured fields should not expose free-form error detail");
}

void testInvalidHeartbeatConfigFixture(TestRunner& runner) {
    const auto config = loadConfigFromFile("tests/fixtures/config/invalid-heartbeat.toml");
    const auto errors = config.validate();

    runner.expect(contains(errors, "heartbeat.bind must not be empty"),
                  "invalid heartbeat fixture should require bind");
    runner.expect(contains(errors, "heartbeat.interval_ms must be positive"),
                  "invalid heartbeat fixture should require positive interval");
    runner.expect(contains(errors, "heartbeat.timeout_ms must be positive"),
                  "invalid heartbeat fixture should require positive timeout");
}

void testInvalidHealthConfigFixture(TestRunner& runner) {
    const auto config = loadConfigFromFile("tests/fixtures/config/invalid-health.toml");
    const auto errors = config.validate();

    runner.expect(contains(errors, "health.interval_ms must be positive"),
                  "invalid health fixture should require positive interval");
    runner.expect(contains(errors, "health.timeout_ms must be positive"),
                  "invalid health fixture should require positive timeout");
}

void testInvalidApiConfigFixture(TestRunner& runner) {
    const auto config = loadConfigFromFile("tests/fixtures/config/invalid-api.toml");
    const auto errors = config.validate();

    runner.expect(contains(errors, "api.bind must not be empty when api.enabled is true"),
                  "invalid API fixture should require bind when enabled");
}

void testMinimalConfigAppliesDefaults(TestRunner& runner) {
    const auto config = loadConfigFromFile("tests/fixtures/config/minimal.toml");

    runner.expect(!config.node_id.empty(), "minimal config should default node_id from hostname");
    runner.expect(config.priority == 100, "minimal config should default priority");
    runner.expect(config.vip.address == "10.0.0.50/24", "minimal config should load VIP address");
    runner.expect(config.vip.interface == "eth0", "minimal config should load VIP interface");
    runner.expect(config.heartbeat.bind == "0.0.0.0:7432",
                  "minimal config should default heartbeat bind");
    runner.expect(config.heartbeat.interval_ms == 1000,
                  "minimal config should default heartbeat interval");
    runner.expect(config.heartbeat.timeout_ms == 3000,
                  "minimal config should default heartbeat timeout");
    runner.expect(config.health.command.empty(),
                  "minimal config should default to no health command");
    runner.expect(config.health.interval_ms == 1000,
                  "minimal config should default health interval");
    runner.expect(config.health.timeout_ms == 2000,
                  "minimal config should default health timeout");
    runner.expect(!config.election.require_quorum,
                  "minimal config should default quorum requirement off");
    runner.expect(config.election.preempt, "minimal config should default preemption on");
    runner.expect(!config.api.enabled, "minimal config should default API disabled");
    runner.expect(config.api.bind == "127.0.0.1:8743", "minimal config should default API bind");
    runner.expect(config.api.read_only, "minimal config should default API read-only");
    runner.expect(!config.mutation_safety.allow_network_mutation,
                  "minimal config should default network mutation safety gate off");
    runner.expect(config.peers.size() == 2, "minimal config should load peers");
    runner.expect(config.validate().empty(), "minimal config with defaults should validate");
}

void testConfigRequiresAtLeastOnePeer(TestRunner& runner) {
    const auto config = loadConfigFromFile("tests/fixtures/config/no-peers.toml");
    const auto errors = config.validate();

    runner.expect(contains(errors, "at least one peer must be configured"),
                  "config without peers should fail validation");
}

void testConfigRejectsInvalidOptionalScalarType(TestRunner& runner) {
    bool threw = false;
    try {
        static_cast<void>(loadConfigFromFile("tests/fixtures/config/invalid-priority-type.toml"));
    } catch (const std::runtime_error& error) {
        threw = true;
        runner.expect(std::string{error.what()}.find("Invalid type for config key: priority") !=
                          std::string::npos,
                      "invalid optional scalar type should report config key");
    }
    runner.expect(threw, "invalid optional scalar type should throw");
}

void testConfigRejectsInvalidOptionalTableType(TestRunner& runner) {
    bool threw = false;
    try {
        static_cast<void>(
            loadConfigFromFile("tests/fixtures/config/invalid-heartbeat-table-type.toml"));
    } catch (const std::runtime_error& error) {
        threw = true;
        runner.expect(std::string{error.what()}.find("Invalid type for TOML table: heartbeat") !=
                          std::string::npos,
                      "invalid optional table type should report table key");
    }
    runner.expect(threw, "invalid optional table type should throw");
}

void testSampleConfigLoads(TestRunner& runner) {
    const auto config = loadConfigFromFile("configs/easy-failover.toml");

    runner.expect(config.node_id == "node-a", "sample node_id should load");
    runner.expect(config.priority == 100, "sample priority should load");
    runner.expect(config.vip.address == "10.0.0.50/24", "sample VIP address should load");
    runner.expect(config.vip.interface == "eth0", "sample VIP interface should load");
    runner.expect(config.heartbeat.bind == "0.0.0.0:7432", "sample heartbeat bind should load");
    runner.expect(config.health.command == "curl -fsS http://127.0.0.1:8080/health",
                  "sample health command should load");
    runner.expect(config.election.preempt, "sample election preempt should load");
    runner.expect(!config.election.require_quorum, "sample election quorum should load");
    runner.expect(!config.api.enabled, "sample API enabled flag should load");
    runner.expect(config.api.read_only, "sample API read-only flag should load");
    runner.expect(!config.mutation_safety.allow_network_mutation,
                  "sample mutation safety gate should load disabled");
    runner.expect(config.peers.size() == 2, "sample peers should load");
    runner.expect(config.validate().empty(), "loaded sample config should validate");
}

void testHealthCheckEmptyCommandIsHealthy(TestRunner& runner) {
    HealthConfig config;
    config.command.clear();

    FakeHealthCommandRunner command_runner;
    const auto result = evaluateHealthCheck(config, command_runner);

    runner.expect(result.status == HealthStatus::Healthy,
                  "empty health command should be treated as healthy");
    runner.expect(!command_runner.was_called, "empty health command should not invoke runner");
}

void testHealthCheckPassesCommandAndTimeout(TestRunner& runner) {
    HealthConfig config;
    config.command = "curl -fsS http://127.0.0.1:8080/health";
    config.timeout_ms = 2500;

    FakeHealthCommandRunner command_runner;
    command_runner.result = CommandResult{.exit_code = 0, .timed_out = false, .error = ""};
    static_cast<void>(evaluateHealthCheck(config, command_runner));

    runner.expect(command_runner.was_called, "configured health command should invoke runner");
    runner.expect(command_runner.observed_command == config.command,
                  "health command should be passed to runner");
    runner.expect(command_runner.observed_timeout_ms == config.timeout_ms,
                  "health timeout should be passed to runner");
}

void testHealthCheckSuccessfulExitIsHealthy(TestRunner& runner) {
    HealthConfig config;
    config.command = "true";

    FakeHealthCommandRunner command_runner;
    command_runner.result = CommandResult{.exit_code = 0, .timed_out = false, .error = ""};

    const auto result = evaluateHealthCheck(config, command_runner);
    runner.expect(result.status == HealthStatus::Healthy,
                  "zero health command exit code should be healthy");
}

void testHealthCheckNonzeroExitIsUnhealthy(TestRunner& runner) {
    HealthConfig config;
    config.command = "false";

    FakeHealthCommandRunner command_runner;
    command_runner.result = CommandResult{.exit_code = 1, .timed_out = false, .error = ""};

    const auto result = evaluateHealthCheck(config, command_runner);
    runner.expect(result.status == HealthStatus::Unhealthy,
                  "nonzero health command exit code should be unhealthy");
}

void testHealthCheckTimeoutIsUnhealthy(TestRunner& runner) {
    HealthConfig config;
    config.command = "sleep 10";

    FakeHealthCommandRunner command_runner;
    command_runner.result = CommandResult{.exit_code = 0, .timed_out = true, .error = ""};

    const auto result = evaluateHealthCheck(config, command_runner);
    runner.expect(result.status == HealthStatus::Unhealthy,
                  "timed out health command should be unhealthy");
}

void testHealthCheckRunnerErrorIsUnhealthy(TestRunner& runner) {
    HealthConfig config;
    config.command = "missing-health-command";

    FakeHealthCommandRunner command_runner;
    command_runner.result =
        CommandResult{.exit_code = 127, .timed_out = false, .error = "command not found"};

    const auto result = evaluateHealthCheck(config, command_runner);
    runner.expect(result.status == HealthStatus::Unhealthy,
                  "runner error should make health check unhealthy");
}

void testLinuxHealthRunnerSuccessfulCommandIsHealthy(TestRunner& runner) {
    HealthConfig config;
    config.command = "true";
    config.timeout_ms = 1000;

    LinuxHealthCommandRunner command_runner;
    const auto result = evaluateHealthCheck(config, command_runner);

    runner.expect(result.status == HealthStatus::Healthy,
                  "Linux runner should treat true as healthy");
}

void testLinuxHealthRunnerFailingCommandIsUnhealthy(TestRunner& runner) {
    HealthConfig config;
    config.command = "false";
    config.timeout_ms = 1000;

    LinuxHealthCommandRunner command_runner;
    const auto result = evaluateHealthCheck(config, command_runner);

    runner.expect(result.status == HealthStatus::Unhealthy,
                  "Linux runner should treat false as unhealthy");
}

void testLinuxHealthRunnerMissingCommandIsUnhealthy(TestRunner& runner) {
    HealthConfig config;
    config.command = "/nonexistent/easy_failover_health_command";
    config.timeout_ms = 1000;

    LinuxHealthCommandRunner command_runner;
    const auto result = evaluateHealthCheck(config, command_runner);

    runner.expect(result.status == HealthStatus::Unhealthy,
                  "Linux runner should treat missing command as unhealthy");
}

void testLinuxHealthRunnerTimeoutIsUnhealthy(TestRunner& runner) {
    HealthConfig config;
    config.command = "sleep 2";
    config.timeout_ms = 50;

    LinuxHealthCommandRunner command_runner;
    const auto result = evaluateHealthCheck(config, command_runner);

    runner.expect(result.status == HealthStatus::Unhealthy,
                  "Linux runner timeout should be unhealthy");
    runner.expect(result.detail == "health command timed out",
                  "Linux runner timeout should report timeout detail");
}

void testHeartbeatMessageRoundTrip(TestRunner& runner) {
    const HeartbeatMessage message{.node_id = "node-a",
                                   .priority = 150,
                                   .healthy = true,
                                   .state = NodeState::Master};

    const auto payload = serializeHeartbeatMessage(message);
    const auto result = parseHeartbeatMessage(payload);

    runner.expect(result.message.has_value(), "serialized heartbeat should parse");
    if (!result.message.has_value()) {
        return;
    }

    runner.expect(result.message->node_id == "node-a", "heartbeat node_id should round trip");
    runner.expect(result.message->priority == 150, "heartbeat priority should round trip");
    runner.expect(result.message->healthy, "heartbeat health should round trip");
    runner.expect(result.message->state == NodeState::Master, "heartbeat state should round trip");
}

void testHeartbeatMessageRejectsUnsupportedVersion(TestRunner& runner) {
    const auto result = parseHeartbeatMessage(
        "version = 2\n"
        "type = \"heartbeat\"\n"
        "node_id = \"node-a\"\n"
        "priority = 100\n"
        "healthy = true\n"
        "state = \"backup\"\n");

    runner.expect(!result.message.has_value(), "unsupported heartbeat version should fail");
    runner.expect(result.error == "unsupported heartbeat.version",
                  "unsupported heartbeat version should report a stable error");
}

void testHeartbeatMessageRejectsInvalidType(TestRunner& runner) {
    const auto result = parseHeartbeatMessage(
        "version = 1\n"
        "type = \"status\"\n"
        "node_id = \"node-a\"\n"
        "priority = 100\n"
        "healthy = true\n"
        "state = \"backup\"\n");

    runner.expect(!result.message.has_value(), "invalid heartbeat type should fail");
    runner.expect(result.error == "heartbeat.type must be 'heartbeat'",
                  "invalid heartbeat type should report a stable error");
}

void testHeartbeatMessageRejectsInvalidFields(TestRunner& runner) {
    const auto missing_node = parseHeartbeatMessage(
        "version = 1\n"
        "type = \"heartbeat\"\n"
        "priority = 100\n"
        "healthy = true\n"
        "state = \"backup\"\n");
    runner.expect(!missing_node.message.has_value(), "missing node_id should fail");
    runner.expect(missing_node.error == "heartbeat.node_id must not be empty",
                  "missing node_id should report a stable error");

    const auto invalid_priority = parseHeartbeatMessage(
        "version = 1\n"
        "type = \"heartbeat\"\n"
        "node_id = \"node-a\"\n"
        "priority = 0\n"
        "healthy = true\n"
        "state = \"backup\"\n");
    runner.expect(!invalid_priority.message.has_value(), "invalid priority should fail");
    runner.expect(invalid_priority.error == "heartbeat.priority must be positive",
                  "invalid priority should report a stable error");

    const auto out_of_range_priority = parseHeartbeatMessage(
        "version = 1\n"
        "type = \"heartbeat\"\n"
        "node_id = \"node-a\"\n"
        "priority = 2147483648\n"
        "healthy = true\n"
        "state = \"backup\"\n");
    runner.expect(!out_of_range_priority.message.has_value(),
                  "out-of-range priority should fail");
    runner.expect(out_of_range_priority.error == "heartbeat.priority must be within int range",
                  "out-of-range priority should report a stable error");

    const auto invalid_state = parseHeartbeatMessage(
        "version = 1\n"
        "type = \"heartbeat\"\n"
        "node_id = \"node-a\"\n"
        "priority = 100\n"
        "healthy = true\n"
        "state = \"unknown\"\n");
    runner.expect(!invalid_state.message.has_value(), "invalid state should fail");
    runner.expect(invalid_state.error == "heartbeat.state is invalid",
                  "invalid state should report a stable error");
}

void testHeartbeatMessageRejectsInvalidScalarTypes(TestRunner& runner) {
    const auto invalid_version = parseHeartbeatMessage(
        "version = \"1\"\n"
        "type = \"heartbeat\"\n"
        "node_id = \"node-a\"\n"
        "priority = 100\n"
        "healthy = true\n"
        "state = \"backup\"\n");
    runner.expect(!invalid_version.message.has_value(), "invalid version type should fail");
    runner.expect(invalid_version.error == "heartbeat.version must be an integer",
                  "invalid version type should report a stable error");

    const auto invalid_priority_type = parseHeartbeatMessage(
        "version = 1\n"
        "type = \"heartbeat\"\n"
        "node_id = \"node-a\"\n"
        "priority = \"100\"\n"
        "healthy = true\n"
        "state = \"backup\"\n");
    runner.expect(!invalid_priority_type.message.has_value(),
                  "invalid priority type should fail");
    runner.expect(invalid_priority_type.error == "heartbeat.priority must be an integer",
                  "invalid priority type should report a stable error");

    const auto invalid_healthy_type = parseHeartbeatMessage(
        "version = 1\n"
        "type = \"heartbeat\"\n"
        "node_id = \"node-a\"\n"
        "priority = 100\n"
        "healthy = \"true\"\n"
        "state = \"backup\"\n");
    runner.expect(!invalid_healthy_type.message.has_value(),
                  "invalid healthy type should fail");
    runner.expect(invalid_healthy_type.error == "heartbeat.healthy must be a boolean",
                  "invalid healthy type should report a stable error");

    const auto missing_state = parseHeartbeatMessage(
        "version = 1\n"
        "type = \"heartbeat\"\n"
        "node_id = \"node-a\"\n"
        "priority = 100\n"
        "healthy = true\n");
    runner.expect(!missing_state.message.has_value(), "missing state should fail");
    runner.expect(missing_state.error == "heartbeat.state must be a string",
                  "missing state should report a stable error");

    const auto invalid_state_type = parseHeartbeatMessage(
        "version = 1\n"
        "type = \"heartbeat\"\n"
        "node_id = \"node-a\"\n"
        "priority = 100\n"
        "healthy = true\n"
        "state = true\n");
    runner.expect(!invalid_state_type.message.has_value(), "invalid state type should fail");
    runner.expect(invalid_state_type.error == "heartbeat.state must be a string",
                  "invalid state type should report a stable error");
}

void testHeartbeatMessageRejectsMalformedPayload(TestRunner& runner) {
    const auto result = parseHeartbeatMessage("version = ");

    runner.expect(!result.message.has_value(), "malformed heartbeat payload should fail");
    runner.expect(result.error.starts_with("failed to parse heartbeat message:"),
                  "malformed heartbeat payload should report parse error");
}

void testHeartbeatMessageConvertsToPeerStatus(TestRunner& runner) {
    const HeartbeatMessage message{.node_id = "node-b",
                                   .priority = 200,
                                   .healthy = false,
                                   .state = NodeState::Fault};

    const auto peer = peerStatusFromHeartbeat(message);

    runner.expect(peer.node_id == "node-b", "heartbeat peer status should include node_id");
    runner.expect(peer.priority == 200, "heartbeat peer status should include priority");
    runner.expect(!peer.healthy, "heartbeat peer status should include health");
    runner.expect(peer.heartbeat_seen, "heartbeat peer status should mark heartbeat seen");
}

void testHeartbeatTransportSendContract(TestRunner& runner) {
    FakeHeartbeatTransport transport;
    HeartbeatTransport& base = transport;

    const auto result = base.send("10.0.0.12:7432", "payload");

    runner.expect(result.sent, "fake heartbeat transport should return configured send result");
    runner.expect(result.peer_address == "10.0.0.12:7432",
                  "heartbeat send result should include peer address");
    runner.expect(result.payload == "payload", "heartbeat send result should include payload");
    runner.expect(transport.send_called, "heartbeat transport send should be called");
    runner.expect(transport.sent_peer_address == "10.0.0.12:7432",
                  "heartbeat transport should receive peer address");
    runner.expect(transport.sent_payload == "payload",
                  "heartbeat transport should receive serialized payload");
}

void testHeartbeatTransportReceiveContract(TestRunner& runner) {
    FakeHeartbeatTransport transport;
    transport.receive_result =
        HeartbeatReceiveResult{.datagram = HeartbeatDatagram{.peer_address = "10.0.0.12:7432",
                                                             .payload = "payload"},
                               .timed_out = false,
                               .error = ""};
    HeartbeatTransport& base = transport;

    const auto result = base.receive(250);

    runner.expect(transport.receive_called, "heartbeat transport receive should be called");
    runner.expect(transport.observed_timeout_ms == 250,
                  "heartbeat transport should receive timeout budget");
    runner.expect(result.datagram.has_value(), "heartbeat transport should return datagram");
    runner.expect(result.timeout_ms == 250,
                  "heartbeat receive result should include timeout budget");
    if (!result.datagram.has_value()) {
        return;
    }

    runner.expect(result.datagram->peer_address == "10.0.0.12:7432",
                  "heartbeat receive result should include sender address");
    runner.expect(result.datagram->payload == "payload",
                  "heartbeat receive result should include payload");
}

void testHeartbeatTransportReceiveTimeoutContract(TestRunner& runner) {
    FakeHeartbeatTransport transport;
    transport.receive_result =
        HeartbeatReceiveResult{.datagram = std::nullopt, .timed_out = true, .error = ""};
    HeartbeatTransport& base = transport;

    const auto result = base.receive(1);

    runner.expect(!result.datagram.has_value(),
                  "heartbeat receive timeout should not return a datagram");
    runner.expect(result.timed_out, "heartbeat receive timeout should be reported");
    runner.expect(result.timeout_ms == 1,
                  "heartbeat receive timeout should include timeout budget");
    runner.expect(result.error.empty(), "heartbeat receive timeout should not be an error");
}

void testDisabledHeartbeatTransportReportsDisabled(TestRunner& runner) {
    DisabledHeartbeatTransport transport;

    const auto send_result = transport.send("10.0.0.12:7432", "payload");
    runner.expect(!send_result.sent, "disabled heartbeat transport should not send");
    runner.expect(send_result.peer_address == "10.0.0.12:7432",
                  "disabled heartbeat send should echo peer address");
    runner.expect(send_result.payload == "payload",
                  "disabled heartbeat send should echo payload");
    runner.expect(send_result.error == kHeartbeatTransportDisabledError,
                  "disabled heartbeat send should report stable error");

    const auto receive_result = transport.receive(1);
    runner.expect(!receive_result.datagram.has_value(),
                  "disabled heartbeat transport should not receive datagrams");
    runner.expect(!receive_result.timed_out,
                  "disabled heartbeat transport should report disabled, not timeout");
    runner.expect(receive_result.timeout_ms == 1,
                  "disabled heartbeat receive should echo timeout budget");
    runner.expect(receive_result.error == kHeartbeatTransportDisabledError,
                  "disabled heartbeat receive should report stable error");
}

LocalNodeStatus healthyLocalStatus() {
    return LocalNodeStatus{.node_id = "node-a",
                           .priority = 100,
                           .healthy = true,
                           .state = NodeState::Backup};
}

std::vector<PeerConfig> heartbeatPeers() {
    return std::vector<PeerConfig>{
        PeerConfig{.id = "node-b", .address = "10.0.0.12:7432"},
        PeerConfig{.id = "node-c", .address = "10.0.0.13:7432"},
    };
}

void testHeartbeatLoopSendsLocalHeartbeatToPeers(TestRunner& runner) {
    FakeHeartbeatTransport transport;

    const auto result = runHeartbeatLoopOnce(healthyLocalStatus(), heartbeatPeers(), 25, transport);

    runner.expect(result.sends.size() == 2, "heartbeat loop should report send for each peer");
    runner.expect(transport.sent_datagrams.size() == 2,
                  "heartbeat loop should send heartbeat to each peer");
    if (result.sends.size() != 2 || transport.sent_datagrams.size() != 2) {
        return;
    }

    runner.expect(result.sends[0].peer_id == "node-b",
                  "heartbeat loop should preserve first peer id");
    runner.expect(result.sends[0].peer_address == "10.0.0.12:7432",
                  "heartbeat loop should report first peer address");
    runner.expect(result.sends[0].sent, "heartbeat loop should report successful send");
    runner.expect(result.sends[1].peer_id == "node-c",
                  "heartbeat loop should preserve second peer id");
    runner.expect(result.sends[1].peer_address == "10.0.0.13:7432",
                  "heartbeat loop should report second peer address");

    const auto parsed = parseHeartbeatMessage(transport.sent_datagrams[0].payload);
    runner.expect(parsed.message.has_value(), "heartbeat loop should send parseable payload");
    if (!parsed.message.has_value()) {
        return;
    }

    runner.expect(parsed.message->node_id == "node-a",
                  "heartbeat loop should send local node id");
    runner.expect(parsed.message->priority == 100,
                  "heartbeat loop should send local priority");
    runner.expect(parsed.message->healthy, "heartbeat loop should send local health");
    runner.expect(parsed.message->state == NodeState::Backup,
                  "heartbeat loop should send local node state");
}

void testHeartbeatLoopReceivesPeerStatus(TestRunner& runner) {
    FakeHeartbeatTransport transport;
    const auto peer_payload = serializeHeartbeatMessage(HeartbeatMessage{.node_id = "node-b",
                                                                         .priority = 200,
                                                                         .healthy = true,
                                                                         .state = NodeState::Master});
    transport.receive_result =
        HeartbeatReceiveResult{.datagram = HeartbeatDatagram{.peer_address = "10.0.0.12:7432",
                                                             .payload = peer_payload},
                               .timed_out = false,
                               .timeout_ms = 50,
                               .error = ""};

    const auto result = runHeartbeatLoopOnce(healthyLocalStatus(), heartbeatPeers(), 50, transport);

    runner.expect(result.receive.attempted, "heartbeat loop should attempt receive");
    runner.expect(result.receive.peer_address == "10.0.0.12:7432",
                  "heartbeat loop should report sender address");
    runner.expect(result.receive.peer_status.has_value(),
                  "heartbeat loop should convert received heartbeat to peer status");
    if (!result.receive.peer_status.has_value()) {
        return;
    }

    runner.expect(result.receive.peer_status->node_id == "node-b",
                  "heartbeat loop should preserve received node id");
    runner.expect(result.receive.peer_status->priority == 200,
                  "heartbeat loop should preserve received priority");
    runner.expect(result.receive.peer_status->healthy,
                  "heartbeat loop should preserve received health");
    runner.expect(result.receive.peer_status->heartbeat_seen,
                  "heartbeat loop should mark received heartbeat seen");
}

void testHeartbeatLoopReportsMalformedReceive(TestRunner& runner) {
    FakeHeartbeatTransport transport;
    transport.receive_result =
        HeartbeatReceiveResult{.datagram = HeartbeatDatagram{.peer_address = "10.0.0.12:7432",
                                                             .payload = "version = "},
                               .timed_out = false,
                               .timeout_ms = 50,
                               .error = ""};

    const auto result = runHeartbeatLoopOnce(healthyLocalStatus(), heartbeatPeers(), 50, transport);

    runner.expect(!result.receive.peer_status.has_value(),
                  "malformed heartbeat should not produce peer status");
    runner.expect(result.receive.error.starts_with("failed to parse heartbeat message:"),
                  "malformed heartbeat should report parse error");
}

void testHeartbeatLoopReportsReceiveTimeout(TestRunner& runner) {
    FakeHeartbeatTransport transport;
    transport.receive_result =
        HeartbeatReceiveResult{.datagram = std::nullopt,
                               .timed_out = true,
                               .timeout_ms = 50,
                               .error = ""};

    const auto result = runHeartbeatLoopOnce(healthyLocalStatus(), heartbeatPeers(), 50, transport);

    runner.expect(result.receive.attempted, "heartbeat loop should attempt timeout receive");
    runner.expect(result.receive.timed_out, "heartbeat loop should report receive timeout");
    runner.expect(result.receive.timeout_ms == 50,
                  "heartbeat loop should preserve receive timeout budget");
    runner.expect(!result.receive.peer_status.has_value(),
                  "heartbeat timeout should not produce peer status");
}

void testHeartbeatLoopReportsTransportErrors(TestRunner& runner) {
    FakeHeartbeatTransport transport;
    transport.send_result = FakeHeartbeatTransport::ConfiguredSendResult{.sent = false,
                                                                         .error = "send failed"};
    transport.receive_result =
        HeartbeatReceiveResult{.datagram = std::nullopt,
                               .timed_out = false,
                               .timeout_ms = 50,
                               .error = "receive failed"};

    const auto result = runHeartbeatLoopOnce(healthyLocalStatus(), heartbeatPeers(), 50, transport);

    runner.expect(result.sends.size() == 2,
                  "heartbeat transport error should still report all sends");
    if (result.sends.size() == 2) {
        runner.expect(!result.sends[0].sent, "failed send should be reported");
        runner.expect(result.sends[0].error == "send failed",
                      "send error should be preserved");
    }
    runner.expect(result.receive.error == "receive failed",
                  "receive transport error should be preserved");
    runner.expect(!result.receive.peer_status.has_value(),
                  "receive transport error should not produce peer status");
}

void testNetworkCommandRunnerCapturesCommandMetadata(TestRunner& runner) {
    FakeNetworkCommandRunner runner_impl;
    runner_impl.result =
        NetworkCommandResult{.request = NetworkCommandRequest{.executable = "ip",
                                                              .arguments = {"addr", "show"},
                                                              .dry_run = false},
                             .exit_code = 0,
                             .executed = true,
                             .dry_run = false,
                             .error = ""};
    NetworkCommandRunner& base = runner_impl;

    const auto request = NetworkCommandRequest{.executable = "ip",
                                               .arguments = {"addr", "add", "10.0.0.50/24",
                                                             "dev", "eth0"},
                                               .dry_run = false};
    const auto result = base.run(request);

    runner.expect(runner_impl.was_called, "network command runner should be called");
    runner.expect(runner_impl.observed_request.executable == "ip",
                  "network command runner should receive executable");
    runner.expect(runner_impl.observed_request.arguments.size() == 5,
                  "network command runner should receive arguments");
    runner.expect(runner_impl.observed_request.arguments[0] == "addr",
                  "network command runner should preserve first argument");
    runner.expect(!runner_impl.observed_request.dry_run,
                  "network command runner should receive dry-run flag");
    runner.expect(result.executed, "fake network command runner should return configured result");
}

void testNetworkCommandRunnerReportsFailure(TestRunner& runner) {
    FakeNetworkCommandRunner runner_impl;
    runner_impl.result =
        NetworkCommandResult{.request = NetworkCommandRequest{.executable = "ip",
                                                              .arguments = {"addr", "add"},
                                                              .dry_run = false},
                             .exit_code = 2,
                             .executed = true,
                             .dry_run = false,
                             .error = "ip failed"};

    const auto result = runner_impl.run(NetworkCommandRequest{.executable = "ip",
                                                              .arguments = {"addr", "add"},
                                                              .dry_run = false});

    runner.expect(result.executed, "failed network command should still report execution");
    runner.expect(result.exit_code == 2, "network command failure should report exit code");
    runner.expect(result.error == "ip failed", "network command failure should report error");
}

void testDryRunNetworkCommandRunnerDoesNotExecute(TestRunner& runner) {
    DryRunNetworkCommandRunner runner_impl;
    const auto request = NetworkCommandRequest{.executable = "arping",
                                               .arguments = {"-A", "-I", "eth0", "10.0.0.50"},
                                               .dry_run = true};

    const auto result = runner_impl.run(request);

    runner.expect(!result.executed, "dry-run network command should not execute");
    runner.expect(result.dry_run, "dry-run network command should report dry-run");
    runner.expect(result.exit_code == 0, "dry-run network command should report success");
    runner.expect(result.error.empty(), "dry-run network command should not report error");
    runner.expect(result.request.executable == "arping",
                  "dry-run network command should preserve executable");
    runner.expect(result.request.arguments.size() == 4,
                  "dry-run network command should preserve arguments");
    runner.expect(result.request.arguments[0] == "-A",
                  "dry-run network command should preserve argument order");
}

void testDryRunNetworkCommandRunnerForcesNonExecution(TestRunner& runner) {
    DryRunNetworkCommandRunner runner_impl;
    const auto request = NetworkCommandRequest{.executable = "ip",
                                               .arguments = {"addr", "del", "10.0.0.50/24",
                                                             "dev", "eth0"},
                                               .dry_run = false};

    const auto result = runner_impl.run(request);

    runner.expect(!result.executed,
                  "dry-run implementation should not execute even for non-dry-run request");
    runner.expect(result.dry_run,
                  "dry-run implementation should report its effective dry-run behavior");
    runner.expect(!result.request.dry_run,
                  "dry-run implementation should preserve requested dry-run flag");
}

void testLinuxVipManagerBuildsAddCommand(TestRunner& runner) {
    FakeNetworkCommandRunner command_runner;
    command_runner.result =
        NetworkCommandResult{.request = NetworkCommandRequest{.executable = "ip",
                                                              .arguments = {"addr", "add"},
                                                              .dry_run = true},
                             .exit_code = 0,
                             .executed = false,
                             .dry_run = true,
                             .error = ""};
    LinuxVipManager manager{command_runner};

    const auto result = manager.addVip("10.0.0.50/24", "eth0");

    runner.expect(result.success, "add VIP dry-run should report success");
    runner.expect(result.request.type == VipOperationType::Add,
                  "add VIP result should preserve operation type");
    runner.expect(result.dry_run, "add VIP should default to dry-run");
    runner.expect(command_runner.was_called, "add VIP should invoke network command runner");
    runner.expect(command_runner.observed_request.executable == "ip",
                  "add VIP should use ip executable");
    runner.expect(command_runner.observed_request.arguments ==
                      std::vector<std::string>{"addr", "add", "10.0.0.50/24", "dev", "eth0"},
                  "add VIP should build ip addr add command");
    runner.expect(command_runner.observed_request.dry_run,
                  "add VIP command should default to dry-run");
}

void testLinuxVipManagerBuildsRemoveCommandWithExplicitMutation(TestRunner& runner) {
    FakeNetworkCommandRunner command_runner;
    command_runner.result =
        NetworkCommandResult{.request = NetworkCommandRequest{.executable = "ip",
                                                              .arguments = {"addr", "del"},
                                                              .dry_run = false},
                             .exit_code = 0,
                             .executed = true,
                             .dry_run = false,
                             .error = ""};
    LinuxVipManager manager{
        command_runner,
        LinuxVipManagerOptions{.allow_network_mutation = true, .dry_run = false}};

    const auto result = manager.removeVip("10.0.0.50/24", "eth0");

    runner.expect(result.success, "remove VIP should report success when command succeeds");
    runner.expect(!result.dry_run, "remove VIP should allow non-dry-run only after explicit opt-in");
    runner.expect(command_runner.observed_request.executable == "ip",
                  "remove VIP should use ip executable");
    runner.expect(command_runner.observed_request.arguments ==
                      std::vector<std::string>{"addr", "del", "10.0.0.50/24", "dev", "eth0"},
                  "remove VIP should build ip addr del command");
    runner.expect(!command_runner.observed_request.dry_run,
                  "remove VIP command should allow mutation after explicit opt-in");
}

void testLinuxVipManagerSafetyGateForcesDryRun(TestRunner& runner) {
    FakeNetworkCommandRunner command_runner;
    command_runner.result =
        NetworkCommandResult{.request = NetworkCommandRequest{.executable = "ip",
                                                              .arguments = {"addr", "del"},
                                                              .dry_run = true},
                             .exit_code = 0,
                             .executed = false,
                             .dry_run = true,
                             .error = ""};
    LinuxVipManager manager{
        command_runner,
        LinuxVipManagerOptions{.allow_network_mutation = false, .dry_run = false}};

    const auto result = manager.removeVip("10.0.0.50/24", "eth0");

    runner.expect(result.dry_run, "safety gate should force dry-run without mutation opt-in");
    runner.expect(command_runner.observed_request.dry_run,
                  "safety gate should force command request dry-run");
}

void testLinuxVipManagerDefaultRunnerSafetyGateBlocksMutation(TestRunner& runner) {
    LinuxVipManager manager{
        LinuxVipManagerOptions{.allow_network_mutation = false, .dry_run = false}};

    const auto result = manager.removeVip("10.0.0.50/24", "eth0");

    runner.expect(result.success, "default runner safety-gated VIP operation should succeed");
    runner.expect(result.dry_run, "default runner should keep VIP operation dry-run");
    runner.expect(result.commands.size() == 1,
                  "default runner safety-gated VIP operation should report command result");
    runner.expect(result.commands.at(0).request.dry_run,
                  "default config should force command request dry-run");
    runner.expect(!result.commands.at(0).executed,
                  "default dry-run runner should not execute command");
}

void testLinuxVipManagerDefaultRunnerAllowsExplicitMutationRequest(TestRunner& runner) {
    LinuxVipManager manager{
        LinuxVipManagerOptions{.allow_network_mutation = true, .dry_run = false}};

    const auto result = manager.removeVip("10.0.0.50/24", "eth0");

    runner.expect(result.success, "explicit mutation request should succeed with dry-run runner");
    runner.expect(!result.dry_run,
                  "explicit opt-in plus runtime non-dry-run should request real VIP operation");
    runner.expect(result.commands.size() == 1,
                  "explicit mutation request should report command result");
    runner.expect(!result.commands.at(0).request.dry_run,
                  "explicit opt-in plus runtime non-dry-run should request non-dry-run command");
    runner.expect(result.commands.at(0).dry_run,
                  "default dry-run command runner should still report non-execution");
    runner.expect(!result.commands.at(0).executed,
                  "default dry-run command runner should not execute command");
}

void testLinuxVipManagerRuntimeDryRunOverridesMutationOptIn(TestRunner& runner) {
    LinuxVipManager manager{
        LinuxVipManagerOptions{.allow_network_mutation = true, .dry_run = true}};

    const auto result = manager.addVip("10.0.0.50/24", "eth0");

    runner.expect(result.success, "runtime dry-run override should still succeed");
    runner.expect(result.dry_run, "runtime dry-run should override mutation opt-in");
    runner.expect(result.commands.size() == 1,
                  "runtime dry-run override should report command result");
    runner.expect(result.commands.at(0).request.dry_run,
                  "runtime dry-run should force command request dry-run");
}

void testLinuxVipManagerBuildsAnnounceCommand(TestRunner& runner) {
    FakeNetworkCommandRunner command_runner;
    command_runner.result =
        NetworkCommandResult{.request = NetworkCommandRequest{.executable = "arping",
                                                              .arguments = {"-A"},
                                                              .dry_run = true},
                             .exit_code = 0,
                             .executed = false,
                             .dry_run = true,
                             .error = ""};
    LinuxVipManager manager{command_runner};

    const auto result = manager.announceVip("10.0.0.50/24", "eth0");

    runner.expect(result.success, "announce VIP dry-run should report success");
    runner.expect(result.request.type == VipOperationType::Announce,
                  "announce VIP result should preserve operation type");
    runner.expect(command_runner.observed_request.executable == "arping",
                  "announce VIP should use arping executable");
    runner.expect(command_runner.observed_request.arguments ==
                      std::vector<std::string>{"-A", "-c", "3", "-I", "eth0", "10.0.0.50"},
                  "announce VIP should build gratuitous ARP command without CIDR prefix");
}

void testLinuxVipManagerRejectsInvalidInputs(TestRunner& runner) {
    FakeNetworkCommandRunner command_runner;
    LinuxVipManager manager{command_runner};

    const auto missing_address = manager.addVip("", "eth0");
    runner.expect(!missing_address.success, "missing VIP address should fail validation");
    runner.expect(missing_address.error == "vip address must not be empty",
                  "missing VIP address should report stable error");

    const auto missing_interface = manager.addVip("10.0.0.50/24", "");
    runner.expect(!missing_interface.success, "missing VIP interface should fail validation");
    runner.expect(missing_interface.error == "vip interface must not be empty",
                  "missing VIP interface should report stable error");

    runner.expect(!command_runner.was_called, "invalid VIP operations should not run commands");
}

void testLinuxVipManagerPropagatesCommandFailure(TestRunner& runner) {
    FakeNetworkCommandRunner command_runner;
    command_runner.result =
        NetworkCommandResult{.request = NetworkCommandRequest{.executable = "ip",
                                                              .arguments = {"addr", "add"},
                                                              .dry_run = false},
                             .exit_code = 2,
                             .executed = true,
                             .dry_run = false,
                             .error = "ip failed"};
    LinuxVipManager manager{
        command_runner,
        LinuxVipManagerOptions{.allow_network_mutation = true, .dry_run = false}};

    const auto result = manager.addVip("10.0.0.50/24", "eth0");

    runner.expect(!result.success, "VIP operation should fail when command fails");
    runner.expect(result.error == "ip failed", "VIP operation should preserve command error");
    runner.expect(result.commands.size() == 1, "VIP operation should report command result");
}

void testDaemonLifecycleStartsRunsDryRunAndStops(TestRunner& runner) {
    FakeVipManager vip_manager;
    const auto config = validConfig();

    const auto result = runDaemonLifecycleOnce(
        DaemonLifecycleRequest{.config = config,
                               .options = DaemonRuntimeOptions{.dry_run = true},
                               .initial_state = DaemonLifecycleState::Stopped},
        vip_manager);

    runner.expect(result.initial_state == DaemonLifecycleState::Stopped,
                  "daemon lifecycle should preserve initial stopped state");
    runner.expect(result.started, "daemon lifecycle should report start from stopped state");
    runner.expect(result.iteration_ran, "daemon lifecycle should run one iteration");
    runner.expect(result.stopped, "daemon lifecycle should stop after one skeleton iteration");
    runner.expect(result.final_state == DaemonLifecycleState::Stopped,
                  "daemon lifecycle skeleton should finish stopped");
    runner.expect(result.detail == "dry-run lifecycle iteration completed",
                  "daemon lifecycle should report dry-run completion");
    runner.expect(vip_manager.add_called, "dry-run lifecycle should request VIP add");
    runner.expect(vip_manager.announce_called, "dry-run lifecycle should request VIP announce");
    runner.expect(!vip_manager.remove_called, "dry-run lifecycle should not request VIP remove");
    runner.expect(vip_manager.add_address == config.vip.address,
                  "daemon lifecycle should pass VIP address to add");
    runner.expect(vip_manager.add_interface == config.vip.interface,
                  "daemon lifecycle should pass VIP interface to add");
    runner.expect(result.vip_operations.size() == 2,
                  "daemon lifecycle should report dry-run VIP operations");
}

void testDaemonLifecycleNonDryRunSafetyGateKeepsVipOperationsDryRun(TestRunner& runner) {
    FakeVipManager vip_manager;

    const auto result = runDaemonLifecycleOnce(
        DaemonLifecycleRequest{.config = validConfig(),
                               .options = DaemonRuntimeOptions{.dry_run = false},
                               .initial_state = DaemonLifecycleState::Stopped},
        vip_manager);

    runner.expect(result.started, "non-dry-run lifecycle should still start skeleton");
    runner.expect(result.iteration_ran, "non-dry-run lifecycle should run one skeleton iteration");
    runner.expect(vip_manager.add_called, "non-dry-run lifecycle should request VIP add");
    runner.expect(vip_manager.announce_called,
                  "non-dry-run lifecycle should request VIP announce");
    runner.expect(result.vip_operations.size() == 2,
                  "non-dry-run lifecycle should report VIP operations");
    runner.expect(result.vip_operations.at(0).dry_run,
                  "safety-gated non-dry-run lifecycle should report dry-run add");
    runner.expect(result.vip_operations.at(1).dry_run,
                  "safety-gated non-dry-run lifecycle should report dry-run announce");
    runner.expect(result.detail ==
                      "mutation safety gate kept lifecycle VIP operations in dry-run mode",
                  "non-dry-run lifecycle should report safety gate detail");
}

void testDaemonLifecycleNonDryRunAcceptsRealVipOperations(TestRunner& runner) {
    FakeVipManager vip_manager;
    vip_manager.operation_dry_run = false;

    const auto result = runDaemonLifecycleOnce(
        DaemonLifecycleRequest{.config = validConfig(),
                               .options = DaemonRuntimeOptions{.dry_run = false},
                               .initial_state = DaemonLifecycleState::Stopped},
        vip_manager);

    runner.expect(result.final_state == DaemonLifecycleState::Stopped,
                  "non-dry-run lifecycle should allow real VIP operations");
    runner.expect(result.stopped, "non-dry-run lifecycle should stop cleanly after real VIP ops");
    runner.expect(vip_manager.add_called, "non-dry-run lifecycle should request real VIP add");
    runner.expect(vip_manager.announce_called,
                  "non-dry-run lifecycle should request real VIP announce");
    runner.expect(result.vip_operations.size() == 2,
                  "non-dry-run lifecycle should report real VIP operations");
    runner.expect(!result.vip_operations.at(0).dry_run,
                  "non-dry-run lifecycle should preserve real VIP add observation");
    runner.expect(!result.vip_operations.at(1).dry_run,
                  "non-dry-run lifecycle should preserve real VIP announce observation");
    runner.expect(result.detail == "real VIP lifecycle iteration completed",
                  "non-dry-run lifecycle should report real VIP completion");
}

void testDaemonLifecycleRejectsInvalidConfig(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.node_id.clear();

    const auto result = runDaemonLifecycleOnce(
        DaemonLifecycleRequest{.config = config,
                               .options = DaemonRuntimeOptions{.dry_run = true},
                               .initial_state = DaemonLifecycleState::Stopped},
        vip_manager);

    runner.expect(!result.validation_errors.empty(),
                  "daemon lifecycle should report validation errors");
    runner.expect(result.final_state == DaemonLifecycleState::Faulted,
                  "invalid config should fault lifecycle");
    runner.expect(!result.iteration_ran, "invalid config should not run lifecycle iteration");
    runner.expect(!vip_manager.add_called, "invalid config should not run VIP operations");
    runner.expect(result.detail == "config validation failed",
                  "invalid config should report stable detail");
}

void testDaemonLifecycleRejectsFaultedInitialState(TestRunner& runner) {
    FakeVipManager vip_manager;

    const auto result = runDaemonLifecycleOnce(
        DaemonLifecycleRequest{.config = validConfig(),
                               .options = DaemonRuntimeOptions{.dry_run = true},
                               .initial_state = DaemonLifecycleState::Faulted},
        vip_manager);

    runner.expect(result.final_state == DaemonLifecycleState::Faulted,
                  "faulted lifecycle should stay faulted");
    runner.expect(!result.started, "faulted lifecycle should not start");
    runner.expect(!result.iteration_ran, "faulted lifecycle should not run iteration");
    runner.expect(!vip_manager.add_called, "faulted lifecycle should not run VIP operations");
    runner.expect(result.detail == "daemon lifecycle cannot start from faulted state",
                  "faulted lifecycle should report stable detail");
}

void testDaemonLifecycleRejectsNonDryRunVipResult(TestRunner& runner) {
    FakeVipManager vip_manager;
    vip_manager.operation_dry_run = false;

    const auto result = runDaemonLifecycleOnce(
        DaemonLifecycleRequest{.config = validConfig(),
                               .options = DaemonRuntimeOptions{.dry_run = true},
                               .initial_state = DaemonLifecycleState::Stopped},
        vip_manager);

    runner.expect(result.final_state == DaemonLifecycleState::Faulted,
                  "dry-run lifecycle should fault on non-dry-run VIP result");
    runner.expect(result.iteration_ran,
                  "dry-run lifecycle should report attempted iteration before safety fault");
    runner.expect(!result.stopped,
                  "dry-run lifecycle safety fault should not report clean stop");
    runner.expect(result.detail == "dry-run lifecycle received non-dry-run VIP operation",
                  "dry-run lifecycle should report stable VIP safety detail");
    runner.expect(result.vip_operations.size() == 1,
                  "dry-run lifecycle should stop after first unsafe VIP operation");
    runner.expect(!vip_manager.announce_called,
                  "dry-run lifecycle should not continue after unsafe VIP operation");
}

void testDaemonLifecycleFaultsOnVipAddFailure(TestRunner& runner) {
    FakeVipManager vip_manager;
    vip_manager.add_success = false;
    vip_manager.add_error = "add failed";

    const auto result = runDaemonLifecycleOnce(
        DaemonLifecycleRequest{.config = validConfig(),
                               .options = DaemonRuntimeOptions{.dry_run = true},
                               .initial_state = DaemonLifecycleState::Stopped},
        vip_manager);

    runner.expect(result.final_state == DaemonLifecycleState::Faulted,
                  "dry-run lifecycle should fault on VIP add failure");
    runner.expect(result.detail == "add failed",
                  "dry-run lifecycle should preserve VIP add failure detail");
    runner.expect(result.vip_operations.size() == 1,
                  "dry-run lifecycle should stop after VIP add failure");
    runner.expect(!vip_manager.announce_called,
                  "dry-run lifecycle should not announce after VIP add failure");
}

void testDaemonLifecycleFaultsOnVipAnnounceFailure(TestRunner& runner) {
    FakeVipManager vip_manager;
    vip_manager.announce_success = false;

    const auto result = runDaemonLifecycleOnce(
        DaemonLifecycleRequest{.config = validConfig(),
                               .options = DaemonRuntimeOptions{.dry_run = true},
                               .initial_state = DaemonLifecycleState::Stopped},
        vip_manager);

    runner.expect(result.final_state == DaemonLifecycleState::Faulted,
                  "dry-run lifecycle should fault on VIP announce failure");
    runner.expect(result.detail == "dry-run lifecycle VIP operation failed",
                  "dry-run lifecycle should use stable detail when VIP error is empty");
    runner.expect(result.vip_operations.size() == 2,
                  "dry-run lifecycle should preserve add and announce observations");
    runner.expect(vip_manager.announce_called,
                  "dry-run lifecycle should report announce attempt before failure");
}

void testDaemonLifecycleStopsWhenShutdownAlreadyRequested(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto shutdown_state = ShutdownSignalState{};
    shutdown_state.requestShutdown(ShutdownSignal::Terminate);

    const auto result = runDaemonLifecycleOnce(
        DaemonLifecycleRequest{.config = validConfig(),
                               .options = DaemonRuntimeOptions{.dry_run = true},
                               .initial_state = DaemonLifecycleState::Stopped,
                               .shutdown_state = &shutdown_state},
        vip_manager);

    runner.expect(result.final_state == DaemonLifecycleState::Stopped,
                  "shutdown-requested lifecycle should remain stopped");
    runner.expect(!result.started, "shutdown-requested lifecycle should not start");
    runner.expect(!result.iteration_ran,
                  "shutdown-requested lifecycle should not run an iteration");
    runner.expect(result.stopped, "shutdown-requested lifecycle should report clean stop");
    runner.expect(!vip_manager.add_called,
                  "shutdown-requested lifecycle should not run VIP operations");
    runner.expect(result.detail == "shutdown requested by terminate signal",
                  "shutdown-requested lifecycle should report shutdown detail");
}

void testDaemonLifecycleStateNames(TestRunner& runner) {
    runner.expect(easyfailover::toString(DaemonLifecycleState::Stopped) == "stopped",
                  "stopped lifecycle state should stringify");
    runner.expect(easyfailover::toString(DaemonLifecycleState::Running) == "running",
                  "running lifecycle state should stringify");
    runner.expect(easyfailover::toString(DaemonLifecycleState::Faulted) == "faulted",
                  "faulted lifecycle state should stringify");
}

void testDaemonRuntimeLoopRunsBoundedIterations(TestRunner& runner) {
    FakeVipManager vip_manager;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3}},
        vip_manager);

    runner.expect(result.iterations_ran == 3,
                  "daemon runtime loop should run the configured bounded iterations");
    runner.expect(result.stop_reason == DaemonLoopStopReason::MaxIterations,
                  "daemon runtime loop should stop after max iterations");
    runner.expect(result.final_state == DaemonLifecycleState::Stopped,
                  "bounded daemon runtime loop should finish stopped");
    runner.expect(result.detail == "max iterations completed",
                  "bounded daemon runtime loop should report max-iteration completion");
    runner.expect(result.vip_operations.size() == 6,
                  "daemon runtime loop should aggregate VIP operations across iterations");
    runner.expect(result.health_schedules.size() == 3,
                  "daemon runtime loop should record health schedule observations");
    runner.expect(result.heartbeat_send_schedules.size() == 3,
                  "daemon runtime loop should record heartbeat send schedule observations");
    runner.expect(result.heartbeat_receive_states.size() == 3,
                  "daemon runtime loop should record heartbeat receive state observations");
    runner.expect(result.failover_decisions.size() == 3,
                  "daemon runtime loop should record failover decision observations");
}

void testDaemonRuntimeLoopSchedulesFirstHealthCheckDue(TestRunner& runner) {
    FakeVipManager vip_manager;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 1}},
        vip_manager);

    runner.expect(result.health_schedules.size() == 1,
                  "daemon runtime loop should record first health schedule observation");
    runner.expect(result.health_schedules.at(0).iteration_index == 0,
                  "first health schedule observation should report iteration index");
    runner.expect(result.health_schedules.at(0).elapsed_ms == 0,
                  "first health schedule observation should start at zero elapsed time");
    runner.expect(result.health_schedules.at(0).interval_ms == validConfig().health.interval_ms,
                  "health schedule observation should report configured interval");
    runner.expect(result.health_schedules.at(0).due,
                  "first health schedule observation should be due");
    runner.expect(result.health_schedules.at(0).command_configured,
                  "sample health command should be reported as configured");
}

void testDaemonRuntimeLoopDoesNotScheduleHealthBeforeInterval(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.health.interval_ms = 1000;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3,
                                                       .logical_iteration_elapsed_ms = 250}},
        vip_manager);

    runner.expect(result.health_schedules.size() == 3,
                  "daemon runtime loop should record every health schedule observation");
    runner.expect(result.health_schedules.at(0).due,
                  "first health schedule observation should be due");
    runner.expect(!result.health_schedules.at(1).due,
                  "health schedule should not be due before configured interval");
    runner.expect(!result.health_schedules.at(2).due,
                  "health schedule should remain not due before configured interval");
    runner.expect(result.health_schedules.at(2).elapsed_ms == 500,
                  "health schedule should report deterministic logical elapsed time");
}

void testDaemonRuntimeLoopSchedulesHealthAtInterval(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.health.interval_ms = 1000;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3,
                                                       .logical_iteration_elapsed_ms = 500}},
        vip_manager);

    runner.expect(result.health_schedules.size() == 3,
                  "daemon runtime loop should record every health schedule observation");
    runner.expect(result.health_schedules.at(0).due,
                  "first health schedule observation should be due");
    runner.expect(!result.health_schedules.at(1).due,
                  "health schedule should not be due before interval");
    runner.expect(result.health_schedules.at(2).due,
                  "health schedule should be due when interval elapses");
    runner.expect(result.health_schedules.at(2).elapsed_ms == 1000,
                  "health schedule should report elapsed interval boundary");
}

void testDaemonRuntimeLoopUsesDelayForDefaultHealthElapsed(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.health.interval_ms = 2;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3,
                                                       .inter_iteration_delay_ms = 1}},
        vip_manager);

    runner.expect(result.health_schedules.size() == 3,
                  "daemon runtime loop should record every default health schedule observation");
    runner.expect(result.health_schedules.at(0).due,
                  "first default health schedule observation should be due");
    runner.expect(!result.health_schedules.at(1).due,
                  "default health schedule should not be due before interval");
    runner.expect(result.health_schedules.at(2).due,
                  "default health schedule should be due after loop delay reaches interval");
    runner.expect(result.health_schedules.at(2).elapsed_ms == 2,
                  "default health schedule should use inter-iteration delay as elapsed time");
}

void testDaemonRuntimeLoopClampsNegativeHealthElapsed(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.health.interval_ms = 1;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3,
                                                       .inter_iteration_delay_ms = -5,
                                                       .logical_iteration_elapsed_ms = -10}},
        vip_manager);

    runner.expect(result.health_schedules.size() == 3,
                  "daemon runtime loop should record negative elapsed schedule observations");
    runner.expect(result.health_schedules.at(0).elapsed_ms == 0,
                  "first negative elapsed schedule observation should start at zero");
    runner.expect(result.health_schedules.at(1).elapsed_ms == 0,
                  "negative elapsed schedule should not move time backward");
    runner.expect(result.health_schedules.at(2).elapsed_ms == 0,
                  "negative elapsed schedule should remain clamped at zero");
    runner.expect(!result.health_schedules.at(1).due,
                  "negative elapsed schedule should not become due before interval");
}

void testDaemonRuntimeLoopSchedulesEmptyHealthCommand(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.health.command.clear();

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 1}},
        vip_manager);

    runner.expect(result.health_schedules.size() == 1,
                  "daemon runtime loop should schedule even when health command is empty");
    runner.expect(result.health_schedules.at(0).due,
                  "empty health command schedule should still be due on first iteration");
    runner.expect(!result.health_schedules.at(0).command_configured,
                  "empty health command schedule should report no configured command");
}

void testDaemonRuntimeLoopSchedulesFirstHeartbeatSendDue(TestRunner& runner) {
    FakeVipManager vip_manager;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 1}},
        vip_manager);

    runner.expect(result.heartbeat_send_schedules.size() == 1,
                  "daemon runtime loop should record first heartbeat send schedule observation");
    runner.expect(result.heartbeat_send_schedules.at(0).iteration_index == 0,
                  "first heartbeat send schedule observation should report iteration index");
    runner.expect(result.heartbeat_send_schedules.at(0).elapsed_ms == 0,
                  "first heartbeat send schedule observation should start at zero elapsed time");
    runner.expect(result.heartbeat_send_schedules.at(0).interval_ms ==
                      validConfig().heartbeat.interval_ms,
                  "heartbeat send schedule observation should report configured interval");
    runner.expect(result.heartbeat_send_schedules.at(0).due,
                  "first heartbeat send schedule observation should be due");
    runner.expect(result.heartbeat_send_schedules.at(0).peers_configured,
                  "sample peers should be reported as configured");
    runner.expect(result.heartbeat_send_schedules.at(0).expected_send_count ==
                      validConfig().peers.size(),
                  "heartbeat send schedule should report expected peer send count");
}

void testDaemonRuntimeLoopDoesNotScheduleHeartbeatSendBeforeInterval(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.heartbeat.interval_ms = 1000;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3,
                                                       .logical_iteration_elapsed_ms = 250}},
        vip_manager);

    runner.expect(result.heartbeat_send_schedules.size() == 3,
                  "daemon runtime loop should record every heartbeat send schedule observation");
    runner.expect(result.heartbeat_send_schedules.at(0).due,
                  "first heartbeat send schedule observation should be due");
    runner.expect(!result.heartbeat_send_schedules.at(1).due,
                  "heartbeat send schedule should not be due before configured interval");
    runner.expect(!result.heartbeat_send_schedules.at(2).due,
                  "heartbeat send schedule should remain not due before configured interval");
    runner.expect(result.heartbeat_send_schedules.at(2).elapsed_ms == 500,
                  "heartbeat send schedule should report deterministic logical elapsed time");
}

void testDaemonRuntimeLoopSchedulesHeartbeatSendAtInterval(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.heartbeat.interval_ms = 1000;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3,
                                                       .logical_iteration_elapsed_ms = 500}},
        vip_manager);

    runner.expect(result.heartbeat_send_schedules.size() == 3,
                  "daemon runtime loop should record every heartbeat send schedule observation");
    runner.expect(result.heartbeat_send_schedules.at(0).due,
                  "first heartbeat send schedule observation should be due");
    runner.expect(!result.heartbeat_send_schedules.at(1).due,
                  "heartbeat send schedule should not be due before interval");
    runner.expect(result.heartbeat_send_schedules.at(2).due,
                  "heartbeat send schedule should be due when interval elapses");
    runner.expect(result.heartbeat_send_schedules.at(2).elapsed_ms == 1000,
                  "heartbeat send schedule should report elapsed interval boundary");
}

void testDaemonRuntimeLoopUsesDelayForDefaultHeartbeatSendElapsed(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.heartbeat.interval_ms = 2;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3,
                                                       .inter_iteration_delay_ms = 1}},
        vip_manager);

    runner.expect(result.heartbeat_send_schedules.size() == 3,
                  "daemon runtime loop should record every default heartbeat send observation");
    runner.expect(result.heartbeat_send_schedules.at(0).due,
                  "first default heartbeat send schedule observation should be due");
    runner.expect(!result.heartbeat_send_schedules.at(1).due,
                  "default heartbeat send schedule should not be due before interval");
    runner.expect(result.heartbeat_send_schedules.at(2).due,
                  "default heartbeat send schedule should be due after loop delay reaches interval");
    runner.expect(result.heartbeat_send_schedules.at(2).elapsed_ms == 2,
                  "default heartbeat send schedule should use inter-iteration delay as elapsed time");
}

void testDaemonRuntimeLoopRejectsHeartbeatSendWithEmptyPeers(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.peers.clear();

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 1}},
        vip_manager);

    runner.expect(result.iterations_ran == 0,
                  "daemon runtime loop should not run iterations for empty peers");
    runner.expect(result.stop_reason == DaemonLoopStopReason::LifecycleFaulted,
                  "daemon runtime loop should fault invalid empty peer config");
    runner.expect(contains(result.validation_errors, "at least one peer must be configured"),
                  "daemon runtime loop should preserve empty peer validation error");
    runner.expect(result.heartbeat_send_schedules.empty(),
                  "daemon runtime loop should not schedule heartbeat sends for invalid config");
}

void testDaemonRuntimeLoopRecordsHeartbeatReceiveState(TestRunner& runner) {
    FakeVipManager vip_manager;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 1}},
        vip_manager);

    runner.expect(result.heartbeat_receive_states.size() == 1,
                  "daemon runtime loop should record first heartbeat receive state observation");
    runner.expect(result.heartbeat_receive_states.at(0).iteration_index == 0,
                  "first heartbeat receive state observation should report iteration index");
    runner.expect(result.heartbeat_receive_states.at(0).elapsed_ms == 0,
                  "first heartbeat receive state observation should start at zero elapsed time");
    runner.expect(!result.heartbeat_receive_states.at(0).receive_attempted,
                  "model-only runtime should not attempt heartbeat receive");
}

void testDaemonRuntimeLoopHeartbeatReceiveStateDefaultsNoPeerStatus(TestRunner& runner) {
    FakeVipManager vip_manager;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 1}},
        vip_manager);

    runner.expect(!result.heartbeat_receive_states.at(0).peer_status.has_value(),
                  "model-only runtime should report no observed peer status");
}

void testDaemonRuntimeLoopHeartbeatReceiveStateTracksElapsed(TestRunner& runner) {
    FakeVipManager vip_manager;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3,
                                                       .logical_iteration_elapsed_ms = 250}},
        vip_manager);

    runner.expect(result.heartbeat_receive_states.size() == 3,
                  "daemon runtime loop should record every heartbeat receive state observation");
    runner.expect(result.heartbeat_receive_states.at(0).elapsed_ms == 0,
                  "first heartbeat receive state should start at zero");
    runner.expect(result.heartbeat_receive_states.at(1).elapsed_ms == 250,
                  "second heartbeat receive state should report logical elapsed time");
    runner.expect(result.heartbeat_receive_states.at(2).elapsed_ms == 500,
                  "third heartbeat receive state should report logical elapsed time");
}

void testDaemonRuntimeLoopDoesNotRecordHeartbeatReceiveStateForInvalidConfig(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.peers.clear();

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 1}},
        vip_manager);

    runner.expect(result.stop_reason == DaemonLoopStopReason::LifecycleFaulted,
                  "daemon runtime loop should fault invalid config before receive observations");
    runner.expect(result.heartbeat_receive_states.empty(),
                  "daemon runtime loop should not record heartbeat receive state for invalid config");
}

void testDaemonRuntimeLoopRecordsFailoverDecisionObservation(TestRunner& runner) {
    FakeVipManager vip_manager;
    const auto config = validConfig();

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 1}},
        vip_manager);

    runner.expect(result.failover_decisions.size() == 1,
                  "daemon runtime loop should record first failover decision observation");
    const auto& observation = result.failover_decisions.at(0);
    runner.expect(observation.iteration_index == 0,
                  "first failover decision observation should report iteration index");
    runner.expect(observation.elapsed_ms == 0,
                  "first failover decision observation should start at zero elapsed time");
    runner.expect(observation.local_status.node_id == config.node_id,
                  "failover decision observation should record local node id");
    runner.expect(observation.local_status.priority == config.priority,
                  "failover decision observation should record local priority");
    runner.expect(observation.local_status.healthy,
                  "model-only failover decision should treat local node as healthy");
    runner.expect(observation.local_status.state == NodeState::Backup,
                  "model-only failover decision should start from backup state");
}

void testDaemonRuntimeLoopFailoverDecisionDefaultsNoPeerInputs(TestRunner& runner) {
    FakeVipManager vip_manager;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 1}},
        vip_manager);

    runner.expect(result.failover_decisions.at(0).peer_statuses.empty(),
                  "model-only failover decision should default to no peer inputs");
}

void testDaemonRuntimeLoopFailoverDecisionDefaultsBecomeMaster(TestRunner& runner) {
    FakeVipManager vip_manager;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 1}},
        vip_manager);

    const auto& decision = result.failover_decisions.at(0).decision;
    runner.expect(decision.action == FailoverAction::BecomeMaster,
                  "model-only failover decision should promote local backup without live peers");
    runner.expect(decision.selected_master == "node-a",
                  "model-only failover decision should select local node as master");
}

void testDaemonRuntimeLoopFailoverDecisionTracksElapsed(TestRunner& runner) {
    FakeVipManager vip_manager;

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3,
                                                       .logical_iteration_elapsed_ms = 250}},
        vip_manager);

    runner.expect(result.failover_decisions.size() == 3,
                  "daemon runtime loop should record every failover decision observation");
    runner.expect(result.failover_decisions.at(0).elapsed_ms == 0,
                  "first failover decision should start at zero");
    runner.expect(result.failover_decisions.at(1).elapsed_ms == 250,
                  "second failover decision should report logical elapsed time");
    runner.expect(result.failover_decisions.at(2).elapsed_ms == 500,
                  "third failover decision should report logical elapsed time");
}

void testDaemonRuntimeLoopDoesNotRecordFailoverDecisionForInvalidConfig(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.peers.clear();

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 1}},
        vip_manager);

    runner.expect(result.stop_reason == DaemonLoopStopReason::LifecycleFaulted,
                  "daemon runtime loop should fault invalid config before failover decisions");
    runner.expect(result.failover_decisions.empty(),
                  "daemon runtime loop should not record failover decisions for invalid config");
}

void testDaemonRuntimeLoopStopsBeforeFirstIterationOnShutdown(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto shutdown_state = ShutdownSignalState{};
    shutdown_state.requestShutdown(ShutdownSignal::Interrupt);

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3},
                          .shutdown_state = &shutdown_state},
        vip_manager);

    runner.expect(result.iterations_ran == 0,
                  "daemon runtime loop should stop before first iteration on shutdown");
    runner.expect(result.stop_reason == DaemonLoopStopReason::ShutdownRequested,
                  "daemon runtime loop should report shutdown stop reason");
    runner.expect(result.detail == "shutdown requested by interrupt signal",
                  "daemon runtime loop should preserve shutdown detail");
    runner.expect(result.vip_operations.empty(),
                  "daemon runtime loop should not run VIP operations after shutdown");
    runner.expect(!vip_manager.add_called,
                  "daemon runtime loop should not call VIP manager after shutdown");
}

void testDaemonRuntimeLoopStopsOnShutdownWithZeroMaxIterations(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto shutdown_state = ShutdownSignalState{};
    shutdown_state.requestShutdown(ShutdownSignal::Terminate);

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 0},
                          .shutdown_state = &shutdown_state},
        vip_manager);

    runner.expect(result.iterations_ran == 0,
                  "zero-iteration runtime loop should not run lifecycle iterations");
    runner.expect(result.stop_reason == DaemonLoopStopReason::ShutdownRequested,
                  "zero-iteration runtime loop should still report pending shutdown");
    runner.expect(result.detail == "shutdown requested by terminate signal",
                  "zero-iteration runtime loop should preserve shutdown detail");
    runner.expect(result.vip_operations.empty(),
                  "zero-iteration runtime loop should not run VIP operations");
    runner.expect(!vip_manager.add_called,
                  "zero-iteration runtime loop should not call VIP manager");
}

void testDaemonRuntimeLoopStopsOnLifecycleFault(TestRunner& runner) {
    FakeVipManager vip_manager;
    vip_manager.add_success = false;
    vip_manager.add_error = "add failed";

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = validConfig(),
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3}},
        vip_manager);

    runner.expect(result.iterations_ran == 1,
                  "daemon runtime loop should stop after the first faulting iteration");
    runner.expect(result.stop_reason == DaemonLoopStopReason::LifecycleFaulted,
                  "daemon runtime loop should report lifecycle fault stop reason");
    runner.expect(result.final_state == DaemonLifecycleState::Faulted,
                  "daemon runtime loop should preserve faulted final state");
    runner.expect(result.detail == "add failed",
                  "daemon runtime loop should preserve lifecycle fault detail");
    runner.expect(result.vip_operations.size() == 1,
                  "daemon runtime loop should aggregate operations from the faulting iteration");
    runner.expect(result.failover_decisions.empty(),
                  "daemon runtime loop should not record failover decisions for faulted iterations");
}

void testDaemonRuntimeLoopPreservesValidationErrors(TestRunner& runner) {
    FakeVipManager vip_manager;
    auto config = validConfig();
    config.node_id.clear();

    const auto result = runDaemonRuntimeLoop(
        DaemonLoopRequest{.config = config,
                          .options = DaemonLoopOptions{.runtime_options = {.dry_run = true},
                                                       .max_iterations = 3}},
        vip_manager);

    runner.expect(result.iterations_ran == 0,
                  "daemon runtime loop should not count invalid config as a completed iteration");
    runner.expect(result.stop_reason == DaemonLoopStopReason::LifecycleFaulted,
                  "daemon runtime loop should stop on invalid config fault");
    runner.expect(contains(result.validation_errors, "node_id must not be empty"),
                  "daemon runtime loop should preserve validation errors");
    runner.expect(result.detail == "config validation failed",
                  "daemon runtime loop should preserve validation failure detail");
    runner.expect(!vip_manager.add_called,
                  "daemon runtime loop should not run VIP operations after validation failure");
}

void testDaemonRuntimeLoopStopReasonNames(TestRunner& runner) {
    runner.expect(easyfailover::toString(DaemonLoopStopReason::MaxIterations) == "max_iterations",
                  "max-iterations stop reason should stringify");
    runner.expect(easyfailover::toString(DaemonLoopStopReason::ShutdownRequested) ==
                      "shutdown_requested",
                  "shutdown stop reason should stringify");
    runner.expect(easyfailover::toString(DaemonLoopStopReason::LifecycleFaulted) ==
                      "lifecycle_faulted",
                  "lifecycle-faulted stop reason should stringify");
}

void testShutdownSignalStateDefaultsToNotRequested(TestRunner& runner) {
    const auto shutdown_state = ShutdownSignalState{};

    runner.expect(!shutdown_state.shutdownRequested(),
                  "shutdown state should default to not requested");
    runner.expect(shutdown_state.signal() == ShutdownSignal::None,
                  "shutdown state should default to no signal");
    runner.expect(shutdown_state.reason() == "shutdown not requested",
                  "shutdown state should report not-requested reason");
}

void testShutdownSignalStateRecordsRequestedSignal(TestRunner& runner) {
    auto shutdown_state = ShutdownSignalState{};
    shutdown_state.requestShutdown(ShutdownSignal::Interrupt);

    runner.expect(shutdown_state.shutdownRequested(),
                  "shutdown state should report requested shutdown");
    runner.expect(shutdown_state.signal() == ShutdownSignal::Interrupt,
                  "shutdown state should preserve requested signal");
    runner.expect(shutdown_state.reason() == "shutdown requested by interrupt signal",
                  "shutdown state should report interrupt reason");
    runner.expect(easyfailover::toString(shutdown_state.signal()) == "interrupt",
                  "shutdown signal should stringify");
}

void testShutdownSignalPollKeepsNoSignalClear(TestRunner& runner) {
    resetPendingShutdownSignalForTest();
    auto shutdown_state = ShutdownSignalState{};

    pollShutdownSignals(shutdown_state);

    runner.expect(!shutdown_state.shutdownRequested(),
                  "poll without pending signal should not request shutdown");
}

void testRuntimeLifecycleLogEventIncludesStableFields(TestRunner& runner) {
    FakeVipManager vip_manager;
    const auto result = runDaemonLifecycleOnce(
        DaemonLifecycleRequest{.config = validConfig(),
                               .options = DaemonRuntimeOptions{.dry_run = true},
                               .initial_state = DaemonLifecycleState::Stopped},
        vip_manager);

    const auto event = formatRuntimeLifecycleEvent(
        result, RuntimeLogContext{.node_id = "node-a", .dry_run = true});

    runner.expect(event.find("event=daemon_lifecycle_result") != std::string::npos,
                  "runtime lifecycle event should include event name");
    runner.expect(event.find("node_id=\"node-a\"") != std::string::npos,
                  "runtime lifecycle event should include node id");
    runner.expect(event.find("initial_state=stopped") != std::string::npos,
                  "runtime lifecycle event should include initial state");
    runner.expect(event.find("final_state=stopped") != std::string::npos,
                  "runtime lifecycle event should include final state");
    runner.expect(event.find("started=true") != std::string::npos,
                  "runtime lifecycle event should include started flag");
    runner.expect(event.find("iteration_ran=true") != std::string::npos,
                  "runtime lifecycle event should include iteration flag");
    runner.expect(event.find("stopped=true") != std::string::npos,
                  "runtime lifecycle event should include stopped flag");
    runner.expect(event.find("dry_run=true") != std::string::npos,
                  "runtime lifecycle event should include dry-run flag");
    runner.expect(event.find("validation_errors=0") != std::string::npos,
                  "runtime lifecycle event should include validation error count");
    runner.expect(event.find("vip_operations=2") != std::string::npos,
                  "runtime lifecycle event should include VIP operation count");
    runner.expect(event.find("detail=\"dry-run lifecycle iteration completed\"") !=
                      std::string::npos,
                  "runtime lifecycle event should include quoted detail");
}

void testRuntimeVipOperationLogEventIncludesStableFields(TestRunner& runner) {
    const auto result = VipOperationResult{
        .request = {.type = VipOperationType::Announce,
                    .address = "10.0.0.50/24",
                    .interface = "eth0",
                    .dry_run = true},
        .success = false,
        .dry_run = true,
        .commands = {},
        .error = "announce failed",
    };

    const auto event = formatRuntimeVipOperationEvent(result, 1);

    runner.expect(event.find("event=vip_operation") != std::string::npos,
                  "runtime VIP event should include event name");
    runner.expect(event.find("zero_based_index=1") != std::string::npos,
                  "runtime VIP event should include zero-based operation index");
    runner.expect(event.find("operation=announce") != std::string::npos,
                  "runtime VIP event should include operation type");
    runner.expect(event.find("address=\"10.0.0.50/24\"") != std::string::npos,
                  "runtime VIP event should include address");
    runner.expect(event.find("interface=\"eth0\"") != std::string::npos,
                  "runtime VIP event should include interface");
    runner.expect(event.find("request_dry_run=true") != std::string::npos,
                  "runtime VIP event should include request dry-run state");
    runner.expect(event.find("result_dry_run=true") != std::string::npos,
                  "runtime VIP event should include result dry-run state");
    runner.expect(event.find("success=false") != std::string::npos,
                  "runtime VIP event should include success state");
    runner.expect(event.find("commands=0") != std::string::npos,
                  "runtime VIP event should include command count");
    runner.expect(event.find("error=\"announce failed\"") != std::string::npos,
                  "runtime VIP event should include quoted error");
}

void testRuntimeLogEventEscapesQuotedValues(TestRunner& runner) {
    const auto result = DaemonLifecycleResult{
        .initial_state = DaemonLifecycleState::Stopped,
        .final_state = DaemonLifecycleState::Faulted,
        .started = false,
        .iteration_ran = false,
        .stopped = false,
        .validation_errors = {},
        .vip_operations = {},
        .detail = "failed \"quoted\"\n\r\tdetail\x1B\x7F",
    };

    const auto event = formatRuntimeLifecycleEvent(
        result, RuntimeLogContext{.node_id = "node\\a", .dry_run = false});

    runner.expect(event.find("node_id=\"node\\\\a\"") != std::string::npos,
                  "runtime event should escape backslashes");
    runner.expect(event.find("detail=\"failed \\\"quoted\\\"\\n\\r\\tdetail\\x1B\\x7F\"") !=
                      std::string::npos,
                  "runtime event should escape quotes and control characters");
    runner.expect(event.find('\n') == std::string::npos,
                  "runtime event should not contain raw newlines");
    runner.expect(event.find('\r') == std::string::npos,
                  "runtime event should not contain raw carriage returns");
    runner.expect(event.find('\t') == std::string::npos,
                  "runtime event should not contain raw tabs");
    runner.expect(event.find('\x1B') == std::string::npos,
                  "runtime event should not contain raw escape characters");
    runner.expect(event.find('\x7F') == std::string::npos,
                  "runtime event should not contain raw delete characters");
}

void testElectionChoosesHighestHealthyPriority(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-b", .priority = 200, .healthy = true},
        CandidateNode{.node_id = "node-c", .priority = 50, .healthy = true},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "healthy candidates should produce a winner");
    runner.expect(winner->node_id == "node-b", "highest priority healthy node should win");
}

void testElectionIgnoresUnhealthyNodes(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-b", .priority = 500, .healthy = false},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "healthy candidate should win over unhealthy candidate");
    runner.expect(winner->node_id == "node-a", "unhealthy high-priority node should be ignored");
}

void testElectionTieBreaksByLowestNodeId(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-c", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-b", .priority = 100, .healthy = true},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "healthy tied candidates should produce a winner");
    runner.expect(winner->node_id == "node-a", "lexicographically lowest node_id should win ties");
}

void testElectionReturnsNoWinnerWithoutHealthyNodes(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = false},
        CandidateNode{.node_id = "node-b", .priority = 200, .healthy = false},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(!winner.has_value(), "no healthy candidates should produce no winner");
}

void testElectionEmptyCandidatesReturnsNoWinner(TestRunner& runner) {
    const std::vector<CandidateNode> candidates;

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(!winner.has_value(), "empty candidate list should produce no winner");
}

void testElectionTieBreakIsIndependentOfInputOrder(TestRunner& runner) {
    std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-z", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-m", .priority = 100, .healthy = true},
    };

    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateNode& left, const CandidateNode& right) {
                  return left.node_id < right.node_id;
              });

    do {
        const auto winner = chooseHighestPriorityHealthyNode(candidates);
        runner.expect(winner.has_value(), "healthy tied candidates should produce a winner");
        runner.expect(winner->node_id == "node-a",
                      "tie-break should choose lexicographically lowest node_id regardless of input order");
    } while (std::next_permutation(
        candidates.begin(), candidates.end(),
        [](const CandidateNode& left, const CandidateNode& right) {
            return left.node_id < right.node_id;
        }));
}

void testElectionDuplicateNodeIdsUseHighestPriorityDuplicate(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-a", .priority = 200, .healthy = true},
        CandidateNode{.node_id = "node-b", .priority = 150, .healthy = true},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "duplicate candidate ids should still produce a winner");
    runner.expect(winner->node_id == "node-a", "highest-priority duplicate should win");
    runner.expect(winner->priority == 200, "winning duplicate should keep its priority");
}

void testElectionNonPositivePriorityCurrentBehavior(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 0, .healthy = true},
        CandidateNode{.node_id = "node-b", .priority = -10, .healthy = true},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "healthy non-positive priority candidates can still win");
    runner.expect(winner->node_id == "node-a", "priority zero should beat lower healthy priority");
}

void testElectionUnhealthyDuplicateDoesNotOverrideHealthyDuplicate(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 1000, .healthy = false},
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "healthy duplicate should produce a winner");
    runner.expect(winner->node_id == "node-a", "healthy duplicate should win");
    runner.expect(winner->priority == 100, "unhealthy duplicate priority should be ignored");
}

LocalNodeStatus localStatus(const std::string& node_id, const int priority, const bool healthy,
                            const easyfailover::NodeState state) {
    return LocalNodeStatus{
        .node_id = node_id,
        .priority = priority,
        .healthy = healthy,
        .state = state,
    };
}

PeerStatus peerStatus(const std::string& node_id, const int priority, const bool healthy,
                      const bool heartbeat_seen) {
    return PeerStatus{
        .node_id = node_id,
        .priority = priority,
        .healthy = healthy,
        .heartbeat_seen = heartbeat_seen,
    };
}

void testUnhealthyLocalEntersFault(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, false, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers{peerStatus("node-b", 200, true, true)};

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::EnterFault,
                  "unhealthy local node should enter fault");
    runner.expect(!decision.selected_master.has_value(), "fault decision should not select a master");
}

void testHealthyLocalWithoutLivePeersBecomesMaster(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, true, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers;

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::BecomeMaster,
                  "healthy local node should become master when no peers are live");
    runner.expect(decision.selected_master == "node-a", "local node should be selected as master");
}

void testLocalWinnerStaysMaster(TestRunner& runner) {
    const auto local = localStatus("node-a", 200, true, easyfailover::NodeState::Master);
    const std::vector<PeerStatus> peers{peerStatus("node-b", 100, true, true)};

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::StayMaster,
                  "winning local master should stay master");
    runner.expect(decision.selected_master == "node-a", "local master should remain selected");
}

void testPeerWinnerKeepsLocalBackup(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, true, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers{peerStatus("node-b", 200, true, true)};

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::StayBackup,
                  "backup should stay backup when peer wins");
    runner.expect(decision.selected_master == "node-b", "winning peer should be selected");
}

void testPeerWinnerDemotesLocalMaster(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, true, easyfailover::NodeState::Master);
    const std::vector<PeerStatus> peers{peerStatus("node-b", 200, true, true)};

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::BecomeBackup,
                  "local master should become backup when peer wins");
    runner.expect(decision.selected_master == "node-b", "winning peer should be selected");
}

void testDecisionIgnoresUnhealthyAndMissingHeartbeatPeers(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, true, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers{
        peerStatus("node-b", 500, false, true),
        peerStatus("node-c", 400, true, false),
    };

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::BecomeMaster,
                  "ignored peers should allow local node to win");
    runner.expect(decision.selected_master == "node-a", "local node should be selected");
}

void testDecisionTieBreaksByLowestNodeId(TestRunner& runner) {
    const auto local = localStatus("node-b", 100, true, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers{
        peerStatus("node-a", 100, true, true),
        peerStatus("node-c", 100, true, true),
    };

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::StayBackup,
                  "local backup should stay backup when peer wins tie-break");
    runner.expect(decision.selected_master == "node-a",
                  "lexicographically lowest node_id should win tie-break");
}

void testDecisionSelectsLocalNodeWhenLocalWinsTieBreak(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, true, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers{
        peerStatus("node-b", 100, true, true),
        peerStatus("node-c", 100, true, true),
    };

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::BecomeMaster,
                  "local node should become master when it wins tie-break");
    runner.expect(decision.selected_master == "node-a", "local node should be selected");
}

} // namespace

int main() {
    TestRunner runner;

    runner.run("valid config has no validation errors", [&runner] {
        testValidConfigHasNoValidationErrors(runner);
    });
    runner.run("config defaults mutation safety disabled", [&runner] {
        testConfigDefaultsMutationSafetyDisabled(runner);
    });
    runner.run("config validation reports required fields", [&runner] {
        testConfigValidationRequiredFields(runner);
    });
    runner.run("config allows API write mode for startup rejection", [&runner] {
        testConfigAllowsApiWriteModeForStartupRejection(runner);
    });
    runner.run("config parses mutation safety enabled", [&runner] {
        testConfigParsesMutationSafetyEnabled(runner);
    });
    runner.run("config parses mutation safety disabled", [&runner] {
        testConfigParsesMutationSafetyDisabled(runner);
    });
    runner.run("local API startup disabled", [&runner] { testLocalApiStartupDisabled(runner); });
    runner.run("local API startup ready for read-only enabled config", [&runner] {
        testLocalApiStartupReadyForReadOnlyEnabledConfig(runner);
    });
    runner.run("local API startup rejects write mode", [&runner] {
        testLocalApiStartupRejectsWriteMode(runner);
    });
    runner.run("local API config validate outcome toString", [&runner] {
        testLocalApiConfigValidateOutcomeToString(runner);
    });
    runner.run("local API status response maps current runtime state", [&runner] {
        testLocalApiStatusResponseMapsCurrentRuntimeState(runner);
    });
    runner.run("local API status response omits sensitive health command", [&runner] {
        testLocalApiStatusResponseOmitsSensitiveHealthCommand(runner);
    });
    runner.run("local API status response keeps no-command health detail", [&runner] {
        testLocalApiStatusResponseKeepsNoCommandHealthDetail(runner);
    });
    runner.run("local API config response maps effective config", [&runner] {
        testLocalApiConfigResponseMapsEffectiveConfig(runner);
    });
    runner.run("local API config response redacts health command", [&runner] {
        testLocalApiConfigResponseRedactsHealthCommand(runner);
    });
    runner.run("local API config response reports no health command", [&runner] {
        testLocalApiConfigResponseReportsNoHealthCommand(runner);
    });
    runner.run("local API config validate accepts valid TOML", [&runner] {
        testLocalApiConfigValidateAcceptsValidToml(runner);
    });
    runner.run("local API config validate reports validation errors", [&runner] {
        testLocalApiConfigValidateReportsValidationErrors(runner);
    });
    runner.run("local API config validate rejects unsupported format", [&runner] {
        testLocalApiConfigValidateRejectsUnsupportedFormat(runner);
    });
    runner.run("local API config validate rejects empty config", [&runner] {
        testLocalApiConfigValidateRejectsEmptyConfig(runner);
    });
    runner.run("local API config validate rejects malformed TOML", [&runner] {
        testLocalApiConfigValidateRejectsMalformedToml(runner);
    });
    runner.run("local API config validate rejects invalid config shape", [&runner] {
        testLocalApiConfigValidateRejectsInvalidConfigShape(runner);
    });
    runner.run("local API config validate rejects invalid mutation safety shape", [&runner] {
        testLocalApiConfigValidateRejectsInvalidMutationSafetyShape(runner);
    });
    runner.run("local API config validate rejects invalid peers shape", [&runner] {
        testLocalApiConfigValidateRejectsInvalidPeersShape(runner);
    });
    runner.run("local API events response defaults empty", [&runner] {
        testLocalApiEventsResponseDefaultsEmpty(runner);
    });
    runner.run("local API lifecycle event maps runtime log fields", [&runner] {
        testLocalApiLifecycleEventMapsRuntimeLogFields(runner);
    });
    runner.run("local API VIP operation event maps runtime log fields", [&runner] {
        testLocalApiVipOperationEventMapsRuntimeLogFields(runner);
    });
    runner.run("invalid heartbeat config fixture reports validation errors", [&runner] {
        testInvalidHeartbeatConfigFixture(runner);
    });
    runner.run("invalid health config fixture reports validation errors", [&runner] {
        testInvalidHealthConfigFixture(runner);
    });
    runner.run("invalid API config fixture reports validation errors", [&runner] {
        testInvalidApiConfigFixture(runner);
    });
    runner.run("minimal config applies defaults", [&runner] {
        testMinimalConfigAppliesDefaults(runner);
    });
    runner.run("config requires at least one peer", [&runner] {
        testConfigRequiresAtLeastOnePeer(runner);
    });
    runner.run("config rejects invalid optional scalar type", [&runner] {
        testConfigRejectsInvalidOptionalScalarType(runner);
    });
    runner.run("config rejects invalid optional table type", [&runner] {
        testConfigRejectsInvalidOptionalTableType(runner);
    });
    runner.run("sample config loads", [&runner] { testSampleConfigLoads(runner); });
    runner.run("health check empty command is healthy", [&runner] {
        testHealthCheckEmptyCommandIsHealthy(runner);
    });
    runner.run("health check passes command and timeout", [&runner] {
        testHealthCheckPassesCommandAndTimeout(runner);
    });
    runner.run("health check successful exit is healthy", [&runner] {
        testHealthCheckSuccessfulExitIsHealthy(runner);
    });
    runner.run("health check nonzero exit is unhealthy", [&runner] {
        testHealthCheckNonzeroExitIsUnhealthy(runner);
    });
    runner.run("health check timeout is unhealthy", [&runner] {
        testHealthCheckTimeoutIsUnhealthy(runner);
    });
    runner.run("health check runner error is unhealthy", [&runner] {
        testHealthCheckRunnerErrorIsUnhealthy(runner);
    });
    runner.run("Linux health runner successful command is healthy", [&runner] {
        testLinuxHealthRunnerSuccessfulCommandIsHealthy(runner);
    });
    runner.run("Linux health runner failing command is unhealthy", [&runner] {
        testLinuxHealthRunnerFailingCommandIsUnhealthy(runner);
    });
    runner.run("Linux health runner missing command is unhealthy", [&runner] {
        testLinuxHealthRunnerMissingCommandIsUnhealthy(runner);
    });
    runner.run("Linux health runner timeout is unhealthy", [&runner] {
        testLinuxHealthRunnerTimeoutIsUnhealthy(runner);
    });
    runner.run("heartbeat message round trip", [&runner] {
        testHeartbeatMessageRoundTrip(runner);
    });
    runner.run("heartbeat message rejects unsupported version", [&runner] {
        testHeartbeatMessageRejectsUnsupportedVersion(runner);
    });
    runner.run("heartbeat message rejects invalid type", [&runner] {
        testHeartbeatMessageRejectsInvalidType(runner);
    });
    runner.run("heartbeat message rejects invalid fields", [&runner] {
        testHeartbeatMessageRejectsInvalidFields(runner);
    });
    runner.run("heartbeat message rejects invalid scalar types", [&runner] {
        testHeartbeatMessageRejectsInvalidScalarTypes(runner);
    });
    runner.run("heartbeat message rejects malformed payload", [&runner] {
        testHeartbeatMessageRejectsMalformedPayload(runner);
    });
    runner.run("heartbeat message converts to peer status", [&runner] {
        testHeartbeatMessageConvertsToPeerStatus(runner);
    });
    runner.run("heartbeat transport send contract", [&runner] {
        testHeartbeatTransportSendContract(runner);
    });
    runner.run("heartbeat transport receive contract", [&runner] {
        testHeartbeatTransportReceiveContract(runner);
    });
    runner.run("heartbeat transport receive timeout contract", [&runner] {
        testHeartbeatTransportReceiveTimeoutContract(runner);
    });
    runner.run("disabled heartbeat transport reports disabled", [&runner] {
        testDisabledHeartbeatTransportReportsDisabled(runner);
    });
    runner.run("heartbeat loop sends local heartbeat to peers", [&runner] {
        testHeartbeatLoopSendsLocalHeartbeatToPeers(runner);
    });
    runner.run("heartbeat loop receives peer status", [&runner] {
        testHeartbeatLoopReceivesPeerStatus(runner);
    });
    runner.run("heartbeat loop reports malformed receive", [&runner] {
        testHeartbeatLoopReportsMalformedReceive(runner);
    });
    runner.run("heartbeat loop reports receive timeout", [&runner] {
        testHeartbeatLoopReportsReceiveTimeout(runner);
    });
    runner.run("heartbeat loop reports transport errors", [&runner] {
        testHeartbeatLoopReportsTransportErrors(runner);
    });
    runner.run("network command runner captures command metadata", [&runner] {
        testNetworkCommandRunnerCapturesCommandMetadata(runner);
    });
    runner.run("network command runner reports failure", [&runner] {
        testNetworkCommandRunnerReportsFailure(runner);
    });
    runner.run("dry-run network command runner does not execute", [&runner] {
        testDryRunNetworkCommandRunnerDoesNotExecute(runner);
    });
    runner.run("dry-run network command runner forces non-execution", [&runner] {
        testDryRunNetworkCommandRunnerForcesNonExecution(runner);
    });
    runner.run("Linux VIP manager builds add command", [&runner] {
        testLinuxVipManagerBuildsAddCommand(runner);
    });
    runner.run("Linux VIP manager builds remove command with explicit mutation", [&runner] {
        testLinuxVipManagerBuildsRemoveCommandWithExplicitMutation(runner);
    });
    runner.run("Linux VIP manager safety gate forces dry-run", [&runner] {
        testLinuxVipManagerSafetyGateForcesDryRun(runner);
    });
    runner.run("Linux VIP manager default runner safety gate blocks mutation", [&runner] {
        testLinuxVipManagerDefaultRunnerSafetyGateBlocksMutation(runner);
    });
    runner.run("Linux VIP manager default runner allows explicit mutation request", [&runner] {
        testLinuxVipManagerDefaultRunnerAllowsExplicitMutationRequest(runner);
    });
    runner.run("Linux VIP manager runtime dry-run overrides mutation opt-in", [&runner] {
        testLinuxVipManagerRuntimeDryRunOverridesMutationOptIn(runner);
    });
    runner.run("Linux VIP manager builds announce command", [&runner] {
        testLinuxVipManagerBuildsAnnounceCommand(runner);
    });
    runner.run("Linux VIP manager rejects invalid inputs", [&runner] {
        testLinuxVipManagerRejectsInvalidInputs(runner);
    });
    runner.run("Linux VIP manager propagates command failure", [&runner] {
        testLinuxVipManagerPropagatesCommandFailure(runner);
    });
    runner.run("daemon lifecycle starts runs dry-run and stops", [&runner] {
        testDaemonLifecycleStartsRunsDryRunAndStops(runner);
    });
    runner.run("daemon lifecycle non-dry-run safety gate keeps VIP operations dry-run", [&runner] {
        testDaemonLifecycleNonDryRunSafetyGateKeepsVipOperationsDryRun(runner);
    });
    runner.run("daemon lifecycle non-dry-run accepts real VIP operations", [&runner] {
        testDaemonLifecycleNonDryRunAcceptsRealVipOperations(runner);
    });
    runner.run("daemon lifecycle rejects invalid config", [&runner] {
        testDaemonLifecycleRejectsInvalidConfig(runner);
    });
    runner.run("daemon lifecycle rejects faulted initial state", [&runner] {
        testDaemonLifecycleRejectsFaultedInitialState(runner);
    });
    runner.run("daemon lifecycle rejects non-dry-run VIP result", [&runner] {
        testDaemonLifecycleRejectsNonDryRunVipResult(runner);
    });
    runner.run("daemon lifecycle faults on VIP add failure", [&runner] {
        testDaemonLifecycleFaultsOnVipAddFailure(runner);
    });
    runner.run("daemon lifecycle faults on VIP announce failure", [&runner] {
        testDaemonLifecycleFaultsOnVipAnnounceFailure(runner);
    });
    runner.run("daemon lifecycle stops when shutdown already requested", [&runner] {
        testDaemonLifecycleStopsWhenShutdownAlreadyRequested(runner);
    });
    runner.run("daemon lifecycle state names", [&runner] {
        testDaemonLifecycleStateNames(runner);
    });
    runner.run("daemon runtime loop runs bounded iterations", [&runner] {
        testDaemonRuntimeLoopRunsBoundedIterations(runner);
    });
    runner.run("daemon runtime loop schedules first health check due", [&runner] {
        testDaemonRuntimeLoopSchedulesFirstHealthCheckDue(runner);
    });
    runner.run("daemon runtime loop does not schedule health before interval", [&runner] {
        testDaemonRuntimeLoopDoesNotScheduleHealthBeforeInterval(runner);
    });
    runner.run("daemon runtime loop schedules health at interval", [&runner] {
        testDaemonRuntimeLoopSchedulesHealthAtInterval(runner);
    });
    runner.run("daemon runtime loop uses delay for default health elapsed", [&runner] {
        testDaemonRuntimeLoopUsesDelayForDefaultHealthElapsed(runner);
    });
    runner.run("daemon runtime loop clamps negative health elapsed", [&runner] {
        testDaemonRuntimeLoopClampsNegativeHealthElapsed(runner);
    });
    runner.run("daemon runtime loop schedules empty health command", [&runner] {
        testDaemonRuntimeLoopSchedulesEmptyHealthCommand(runner);
    });
    runner.run("daemon runtime loop schedules first heartbeat send due", [&runner] {
        testDaemonRuntimeLoopSchedulesFirstHeartbeatSendDue(runner);
    });
    runner.run("daemon runtime loop does not schedule heartbeat send before interval", [&runner] {
        testDaemonRuntimeLoopDoesNotScheduleHeartbeatSendBeforeInterval(runner);
    });
    runner.run("daemon runtime loop schedules heartbeat send at interval", [&runner] {
        testDaemonRuntimeLoopSchedulesHeartbeatSendAtInterval(runner);
    });
    runner.run("daemon runtime loop uses delay for default heartbeat send elapsed", [&runner] {
        testDaemonRuntimeLoopUsesDelayForDefaultHeartbeatSendElapsed(runner);
    });
    runner.run("daemon runtime loop rejects heartbeat send with empty peers", [&runner] {
        testDaemonRuntimeLoopRejectsHeartbeatSendWithEmptyPeers(runner);
    });
    runner.run("daemon runtime loop records heartbeat receive state", [&runner] {
        testDaemonRuntimeLoopRecordsHeartbeatReceiveState(runner);
    });
    runner.run("daemon runtime loop heartbeat receive state defaults no peer status", [&runner] {
        testDaemonRuntimeLoopHeartbeatReceiveStateDefaultsNoPeerStatus(runner);
    });
    runner.run("daemon runtime loop heartbeat receive state tracks elapsed", [&runner] {
        testDaemonRuntimeLoopHeartbeatReceiveStateTracksElapsed(runner);
    });
    runner.run("daemon runtime loop does not record heartbeat receive state for invalid config",
               [&runner] {
                   testDaemonRuntimeLoopDoesNotRecordHeartbeatReceiveStateForInvalidConfig(runner);
               });
    runner.run("daemon runtime loop records failover decision observation", [&runner] {
        testDaemonRuntimeLoopRecordsFailoverDecisionObservation(runner);
    });
    runner.run("daemon runtime loop failover decision defaults no peer inputs", [&runner] {
        testDaemonRuntimeLoopFailoverDecisionDefaultsNoPeerInputs(runner);
    });
    runner.run("daemon runtime loop failover decision defaults become master", [&runner] {
        testDaemonRuntimeLoopFailoverDecisionDefaultsBecomeMaster(runner);
    });
    runner.run("daemon runtime loop failover decision tracks elapsed", [&runner] {
        testDaemonRuntimeLoopFailoverDecisionTracksElapsed(runner);
    });
    runner.run("daemon runtime loop does not record failover decision for invalid config",
               [&runner] {
                   testDaemonRuntimeLoopDoesNotRecordFailoverDecisionForInvalidConfig(runner);
               });
    runner.run("daemon runtime loop stops before first iteration on shutdown", [&runner] {
        testDaemonRuntimeLoopStopsBeforeFirstIterationOnShutdown(runner);
    });
    runner.run("daemon runtime loop stops on shutdown with zero max iterations", [&runner] {
        testDaemonRuntimeLoopStopsOnShutdownWithZeroMaxIterations(runner);
    });
    runner.run("daemon runtime loop stops on lifecycle fault", [&runner] {
        testDaemonRuntimeLoopStopsOnLifecycleFault(runner);
    });
    runner.run("daemon runtime loop preserves validation errors", [&runner] {
        testDaemonRuntimeLoopPreservesValidationErrors(runner);
    });
    runner.run("daemon runtime loop stop reason names", [&runner] {
        testDaemonRuntimeLoopStopReasonNames(runner);
    });
    runner.run("shutdown signal state defaults to not requested", [&runner] {
        testShutdownSignalStateDefaultsToNotRequested(runner);
    });
    runner.run("shutdown signal state records requested signal", [&runner] {
        testShutdownSignalStateRecordsRequestedSignal(runner);
    });
    runner.run("shutdown signal poll keeps no signal clear", [&runner] {
        testShutdownSignalPollKeepsNoSignalClear(runner);
    });
    runner.run("runtime lifecycle log event includes stable fields", [&runner] {
        testRuntimeLifecycleLogEventIncludesStableFields(runner);
    });
    runner.run("runtime VIP operation log event includes stable fields", [&runner] {
        testRuntimeVipOperationLogEventIncludesStableFields(runner);
    });
    runner.run("runtime log event escapes quoted values", [&runner] {
        testRuntimeLogEventEscapesQuotedValues(runner);
    });
    runner.run("election chooses highest healthy priority", [&runner] {
        testElectionChoosesHighestHealthyPriority(runner);
    });
    runner.run("election ignores unhealthy nodes", [&runner] {
        testElectionIgnoresUnhealthyNodes(runner);
    });
    runner.run("election tie-breaks by lowest node id", [&runner] {
        testElectionTieBreaksByLowestNodeId(runner);
    });
    runner.run("election returns no winner without healthy nodes", [&runner] {
        testElectionReturnsNoWinnerWithoutHealthyNodes(runner);
    });
    runner.run("election empty candidates returns no winner", [&runner] {
        testElectionEmptyCandidatesReturnsNoWinner(runner);
    });
    runner.run("election tie-break is independent of input order", [&runner] {
        testElectionTieBreakIsIndependentOfInputOrder(runner);
    });
    runner.run("election duplicate node ids use highest-priority duplicate", [&runner] {
        testElectionDuplicateNodeIdsUseHighestPriorityDuplicate(runner);
    });
    runner.run("election non-positive priority current behavior", [&runner] {
        testElectionNonPositivePriorityCurrentBehavior(runner);
    });
    runner.run("election unhealthy duplicate does not override healthy duplicate", [&runner] {
        testElectionUnhealthyDuplicateDoesNotOverrideHealthyDuplicate(runner);
    });
    runner.run("unhealthy local enters fault", [&runner] { testUnhealthyLocalEntersFault(runner); });
    runner.run("healthy local without live peers becomes master", [&runner] {
        testHealthyLocalWithoutLivePeersBecomesMaster(runner);
    });
    runner.run("local winner stays master", [&runner] { testLocalWinnerStaysMaster(runner); });
    runner.run("peer winner keeps local backup", [&runner] { testPeerWinnerKeepsLocalBackup(runner); });
    runner.run("peer winner demotes local master", [&runner] {
        testPeerWinnerDemotesLocalMaster(runner);
    });
    runner.run("decision ignores unhealthy and missing-heartbeat peers", [&runner] {
        testDecisionIgnoresUnhealthyAndMissingHeartbeatPeers(runner);
    });
    runner.run("decision tie-breaks by lowest node id", [&runner] {
        testDecisionTieBreaksByLowestNodeId(runner);
    });
    runner.run("decision selects local node when local wins tie-break", [&runner] {
        testDecisionSelectsLocalNodeWhenLocalWinsTieBreak(runner);
    });

    if (runner.failures() != 0) {
        std::cerr << runner.failures() << " test failure(s)\n";
        return 1;
    }

    return 0;
}
