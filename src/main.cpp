#include "api/LocalApi.hpp"
#include "config/Config.hpp"
#include "health/HealthCheck.hpp"
#include "heartbeat/HeartbeatTransport.hpp"
#include "heartbeat/UdpHeartbeatTransport.hpp"
#include "platform/linux/LinuxVipOwnershipProbe.hpp"
#include "platform/linux/LinuxVipManager.hpp"
#include "runtime/DaemonRuntime.hpp"
#include "runtime/RuntimeLog.hpp"
#include "runtime/ShutdownSignal.hpp"

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace {

constexpr auto kVersion = "easy-failover 0.1.0";

void initializeLogging() {
    auto logger = spdlog::stdout_color_mt("easy-failover");
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::info);
}

void logValidationErrors(const std::vector<std::string>& errors) {
    for (const auto& error : errors) {
        spdlog::error("config validation: {}", error);
    }
}

easyfailover::LocalApiHttpSnapshot buildHttpSnapshotFromLoopResult(
    const easyfailover::Config& config,
    const easyfailover::DaemonLoopResult& loop_result,
    const bool dry_run) {
    auto lifecycle_snapshot = easyfailover::DaemonLifecycleResult{
        .initial_state = loop_result.initial_state,
        .final_state = loop_result.final_state,
        .started = loop_result.iterations_ran > 0,
        .iteration_ran = loop_result.iterations_ran > 0,
        .stopped = loop_result.final_state == easyfailover::DaemonLifecycleState::Stopped,
        .validation_errors = loop_result.validation_errors,
        .vip_operations = loop_result.vip_operations,
        .local_status = easyfailover::LocalNodeStatus{.node_id = config.node_id,
                                                      .priority = config.priority,
                                                      .healthy = true,
                                                      .state = easyfailover::NodeState::Backup},
        .failover_decision = easyfailover::FailoverDecision{},
        .detail = loop_result.detail};
    if (!loop_result.failover_decisions.empty()) {
        const auto& latest_decision = loop_result.failover_decisions.back();
        lifecycle_snapshot.local_status = latest_decision.local_status;
        lifecycle_snapshot.failover_decision = latest_decision.decision;
        lifecycle_snapshot.local_vip_owner =
            latest_decision.local_status.state == easyfailover::NodeState::Master;
        lifecycle_snapshot.local_vip_owner_known = true;
    }

    const auto peers_observed =
        loop_result.failover_decisions.empty()
            ? 0
            : static_cast<int>(loop_result.failover_decisions.back().peer_statuses.size());
    const auto health = easyfailover::HealthCheckResult{
        .status = config.health.command.empty() ? easyfailover::HealthStatus::Healthy
                                                : easyfailover::HealthStatus::Unhealthy,
        .detail = config.health.command.empty() ? "no health command configured"
                                                : "health check not evaluated"};
    const auto local_node_state =
        loop_result.failover_decisions.empty()
            ? easyfailover::NodeState::Backup
            : loop_result.failover_decisions.back().local_status.state;

    auto events = std::vector<easyfailover::LocalApiEvent>{};
    events.reserve(loop_result.vip_operations.size() + 1);
    events.push_back(easyfailover::buildLocalApiLifecycleEvent(
        1, lifecycle_snapshot,
        easyfailover::RuntimeLogContext{.node_id = config.node_id, .dry_run = dry_run}));
    for (std::size_t index = 0; index < loop_result.vip_operations.size(); ++index) {
        events.push_back(easyfailover::buildLocalApiVipOperationEvent(
            static_cast<std::uint64_t>(index + 2), loop_result.vip_operations.at(index), index));
    }

    return easyfailover::LocalApiHttpSnapshot{
        .status = easyfailover::buildLocalApiStatusResponse(
            config, lifecycle_snapshot, health, local_node_state, dry_run, peers_observed),
        .config = easyfailover::buildLocalApiConfigResponse(config),
        .events = easyfailover::buildLocalApiEventsResponse(std::move(events))};
}

easyfailover::DaemonLoopResult initialLoopResult() {
    return easyfailover::DaemonLoopResult{
        .initial_state = easyfailover::DaemonLifecycleState::Stopped,
        .final_state = easyfailover::DaemonLifecycleState::Stopped,
        .stop_reason = easyfailover::DaemonLoopStopReason::MaxIterations,
        .iterations_ran = 0,
        .validation_errors = {},
        .vip_operations = {},
        .health_schedules = {},
        .heartbeat_send_schedules = {},
        .heartbeat_sends = {},
        .heartbeat_receive_states = {},
        .failover_decisions = {},
        .detail = "daemon starting"};
}

class ApiThreadGuard {
  public:
    ApiThreadGuard(std::thread& thread, easyfailover::ShutdownSignalState& shutdown_state)
        : thread_{thread}, shutdown_state_{shutdown_state} {}

    ApiThreadGuard(const ApiThreadGuard&) = delete;
    ApiThreadGuard& operator=(const ApiThreadGuard&) = delete;

    ~ApiThreadGuard() {
        stop();
    }

    void stop() {
        if (thread_.joinable()) {
            shutdown_state_.requestShutdown();
            thread_.join();
        }
    }

  private:
    std::thread& thread_;
    easyfailover::ShutdownSignalState& shutdown_state_;
};

struct ApiStartupState {
    bool reported = false;
    easyfailover::LocalApiHttpServeResult result;
    std::exception_ptr exception;
};

} // namespace

int main(int argc, char** argv) {
    std::string config_path = "configs/easy-failover.toml";
    bool validate_config = false;
    bool dry_run = false;
    bool show_version = false;
    bool run_forever = false;
    bool release_vip = false;
    std::size_t max_iterations = 1;

    CLI::App app{"easy-failover virtual IP failover agent"};
    argv = app.ensure_utf8(argv);

    app.add_option("--config", config_path, "Path to TOML config file")
        ->capture_default_str()
        ->check(CLI::ExistingFile);
    app.add_flag("--validate-config", validate_config, "Validate config and exit");
    app.add_flag("--dry-run", dry_run, "Log intended actions without changing system state");
    app.add_flag("--run-forever", run_forever, "Run daemon loop until SIGINT or SIGTERM");
    app.add_flag("--release-vip", release_vip,
                 "Release the configured VIP from this host if owned, then exit (used on uninstall)");
    app.add_option("--max-iterations", max_iterations,
                  "Bound daemon loop iterations for smoke tests and development")
        ->capture_default_str();
    app.add_flag("--version", show_version, "Print version and exit");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }

    if (show_version) {
        std::cout << kVersion << '\n';
        return EXIT_SUCCESS;
    }

    try {
        initializeLogging();
        const auto config = easyfailover::loadConfigFromFile(config_path);

        // Explicit operator-invoked VIP teardown (used by package prerm on uninstall). This is a
        // manual action, not the autonomous loop, so it runs the real removal regardless of the
        // mutation-safety gate, but only after confirming this host actually owns the VIP. It is
        // best-effort and never fails the process, so it cannot block a package removal. It runs
        // before config validation so an unrelated validation error cannot strand the VIP.
        if (release_vip) {
            if (config.vip.address.empty() || config.vip.interface.empty()) {
                spdlog::warn("release-vip: vip.address/interface not configured; nothing to release");
                return EXIT_SUCCESS;
            }
            easyfailover::LinuxVipOwnershipProbe ownership_probe;
            const auto ownership =
                ownership_probe.probeLocalVip(config.vip.address, config.vip.interface);
            if (!ownership.success) {
                spdlog::warn("release-vip: ownership probe for {} on {} failed: {}",
                             config.vip.address, config.vip.interface,
                             ownership.error.empty() ? "unknown error" : ownership.error);
                return EXIT_SUCCESS;
            }
            if (!ownership.local_owner) {
                spdlog::info("release-vip: {} not present on {}; nothing to release",
                             config.vip.address, config.vip.interface);
                return EXIT_SUCCESS;
            }
            easyfailover::LinuxVipManager vip_manager{easyfailover::LinuxVipManagerOptions{
                .allow_network_mutation = true, .dry_run = dry_run}};
            const auto remove_result =
                vip_manager.removeVip(config.vip.address, config.vip.interface);
            if (remove_result.success) {
                spdlog::info("release-vip: released {} from {}{}", config.vip.address,
                             config.vip.interface, dry_run ? " (dry-run)" : "");
            } else {
                spdlog::warn("release-vip: failed to release {} from {}: {}", config.vip.address,
                             config.vip.interface,
                             remove_result.error.empty() ? "unknown error" : remove_result.error);
            }
            return EXIT_SUCCESS;
        }

        const auto validation_errors = config.validate();
        if (validate_config) {
            if (!validation_errors.empty()) {
                logValidationErrors(validation_errors);
                return EXIT_FAILURE;
            }

            spdlog::info("config '{}' is valid", config_path);
            return EXIT_SUCCESS;
        }

        if (!validation_errors.empty()) {
            logValidationErrors(validation_errors);
            return EXIT_FAILURE;
        }

        const auto api_startup = easyfailover::evaluateLocalApiStartup(config.api);
        if (api_startup.state == easyfailover::LocalApiStartupState::Rejected) {
            spdlog::error("local API startup rejected bind={} detail=\"{}\"", api_startup.bind,
                          api_startup.detail);
            return EXIT_FAILURE;
        }

        // Build the write context once at startup. In write mode the token is loaded from the
        // configured file (already validated as readable/non-empty by startup gating) and kept in
        // daemon memory only; it is never logged or serialized into any response.
        auto api_write_context = easyfailover::LocalApiWriteContext{};
        if (api_startup.state == easyfailover::LocalApiStartupState::Ready && !config.api.read_only) {
            const auto loaded_token = easyfailover::loadApiAuthToken(config.api.auth_token_file);
            if (!loaded_token.has_value() || loaded_token->empty()) {
                spdlog::error("local API write mode could not load auth token file");
                return EXIT_FAILURE;
            }
            api_write_context.write_enabled = true;
            api_write_context.config_path = config_path;
            api_write_context.auth_token = *loaded_token;
        }
        const auto api_emit_audit =
            [](const easyfailover::LocalApiHttpRequest& request,
               const easyfailover::LocalApiConfigApplyResult& result) {
                spdlog::info("{}",
                             easyfailover::formatLocalApiWriteAttemptEvent(request, result));
            };
        if (api_startup.state == easyfailover::LocalApiStartupState::Ready) {
            spdlog::info("local API startup state={} bind={} detail=\"{}\"",
                         easyfailover::toString(api_startup.state), api_startup.bind,
                         api_startup.detail);
        }

        const auto signal_handlers = easyfailover::installShutdownSignalHandlers();
        if (!signal_handlers.success) {
            spdlog::warn("one or more shutdown signal handlers could not be installed");
        }

        easyfailover::LinuxVipManager vip_manager{easyfailover::LinuxVipManagerOptions{
            .allow_network_mutation = config.mutation_safety.allow_network_mutation,
            .dry_run = dry_run}};
        easyfailover::LinuxVipOwnershipProbe ownership_probe;
        easyfailover::UdpHeartbeatTransport heartbeat_transport{config.heartbeat.bind};
        auto shutdown_state = easyfailover::ShutdownSignalState{};
        easyfailover::pollShutdownSignals(shutdown_state);
        auto api_snapshot_mutex = std::mutex{};
        auto api_snapshot = buildHttpSnapshotFromLoopResult(config, initialLoopResult(), dry_run);
        auto api_serve_result_mutex = std::mutex{};
        auto api_serve_result = std::optional<easyfailover::LocalApiHttpServeResult>{};
        auto api_thread = std::thread{};
        auto api_thread_guard = ApiThreadGuard{api_thread, shutdown_state};
        auto api_startup_mutex = std::mutex{};
        auto api_startup_condition = std::condition_variable{};
        auto api_thread_startup = ApiStartupState{};
        const auto update_api_snapshot = [&](const easyfailover::DaemonLoopResult& current_result) {
            if (api_startup.state != easyfailover::LocalApiStartupState::Ready) {
                return;
            }
            auto next_snapshot = buildHttpSnapshotFromLoopResult(config, current_result, dry_run);
            auto lock = std::lock_guard<std::mutex>{api_snapshot_mutex};
            api_snapshot = std::move(next_snapshot);
        };
        if (api_startup.state == easyfailover::LocalApiStartupState::Ready) {
            spdlog::info("local API listening bind={} detail=\"{}\"", api_startup.bind,
                         api_startup.detail);
            api_thread = std::thread{[&] {
                try {
                    const auto serve_result = easyfailover::serveLocalApiHttp(
                        api_startup.bind,
                        [&] {
                            auto lock = std::lock_guard<std::mutex>{api_snapshot_mutex};
                            return api_snapshot;
                        },
                        &shutdown_state,
                        [&](const easyfailover::LocalApiHttpServeResult& startup_result) {
                            auto lock = std::lock_guard<std::mutex>{api_startup_mutex};
                            api_thread_startup.result = startup_result;
                            api_thread_startup.reported = true;
                            api_startup_condition.notify_one();
                        },
                        api_write_context,
                        api_emit_audit);
                    {
                        auto lock = std::lock_guard<std::mutex>{api_serve_result_mutex};
                        api_serve_result = serve_result;
                    }
                    if (!serve_result.error.empty()) {
                        shutdown_state.requestShutdown();
                    }
                } catch (...) {
                    {
                        auto lock = std::lock_guard<std::mutex>{api_startup_mutex};
                        api_thread_startup.exception = std::current_exception();
                        if (!api_thread_startup.reported) {
                            api_thread_startup.reported = true;
                            api_startup_condition.notify_one();
                        }
                    }
                    shutdown_state.requestShutdown();
                }
            }};
            auto api_startup_result = easyfailover::LocalApiHttpServeResult{};
            auto api_startup_exception = std::exception_ptr{};
            {
                auto lock = std::unique_lock<std::mutex>{api_startup_mutex};
                api_startup_condition.wait(lock, [&] { return api_thread_startup.reported; });
                api_startup_result = api_thread_startup.result;
                api_startup_exception = api_thread_startup.exception;
            }
            if (api_startup_exception != nullptr) {
                std::rethrow_exception(api_startup_exception);
            }
            if (!api_startup_result.started || !api_startup_result.error.empty()) {
                spdlog::error("local API listener failed bind={} error=\"{}\"",
                              api_startup.bind, api_startup_result.error);
                return EXIT_FAILURE;
            }
        }
        const auto loop_result = easyfailover::runDaemonRuntimeLoop(
            easyfailover::DaemonLoopRequest{
                .config = config,
                .options = easyfailover::DaemonLoopOptions{
                    .runtime_options = easyfailover::DaemonRuntimeOptions{.dry_run = dry_run},
                    .max_iterations = max_iterations,
                    .run_until_shutdown = run_forever,
                    .inter_iteration_delay_ms = config.heartbeat.interval_ms,
                    .max_recorded_observations =
                        run_forever ? static_cast<std::size_t>(100) : static_cast<std::size_t>(0)},
                .initial_state = easyfailover::DaemonLifecycleState::Stopped,
                .shutdown_state = &shutdown_state,
                .config_prevalidated = true},
            vip_manager, ownership_probe, heartbeat_transport, update_api_snapshot);
        update_api_snapshot(loop_result);
        spdlog::info("{}", easyfailover::formatRuntimeLoopEvent(
                              loop_result,
                              easyfailover::RuntimeLogContext{.node_id = config.node_id,
                                                              .dry_run = dry_run}));
        for (std::size_t index = 0; index < loop_result.vip_operations.size(); ++index) {
            spdlog::info("{}",
                         easyfailover::formatRuntimeVipOperationEvent(
                             loop_result.vip_operations.at(index), index));
        }
        spdlog::info("heartbeat bind={} interval_ms={} timeout_ms={}", config.heartbeat.bind,
                     config.heartbeat.interval_ms, config.heartbeat.timeout_ms);
        spdlog::info("health command_set={} interval_ms={} timeout_ms={}",
                     !config.health.command.empty(), config.health.interval_ms,
                     config.health.timeout_ms);
        spdlog::info("configured peers={}", config.peers.size());

        if (api_thread.joinable()) {
            api_thread_guard.stop();
            {
                auto lock = std::lock_guard<std::mutex>{api_startup_mutex};
                if (api_thread_startup.exception != nullptr) {
                    std::rethrow_exception(api_thread_startup.exception);
                }
            }
            auto lock = std::lock_guard<std::mutex>{api_serve_result_mutex};
            if (api_serve_result.has_value() && !api_serve_result->error.empty()) {
                spdlog::error("local API listener failed bind={} error=\"{}\"",
                              api_startup.bind, api_serve_result->error);
                return EXIT_FAILURE;
            }
        }
        if (!loop_result.validation_errors.empty()) {
            logValidationErrors(loop_result.validation_errors);
            return EXIT_FAILURE;
        }
        if (loop_result.final_state == easyfailover::DaemonLifecycleState::Faulted) {
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        if (spdlog::default_logger() != nullptr) {
            spdlog::error("{}", error.what());
        } else {
            std::cerr << error.what() << '\n';
        }
        return EXIT_FAILURE;
    }
}
