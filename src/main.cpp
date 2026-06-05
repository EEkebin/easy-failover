#include "api/LocalApi.hpp"
#include "config/Config.hpp"
#include "heartbeat/HeartbeatTransport.hpp"
#include "platform/linux/LinuxVipOwnershipProbe.hpp"
#include "platform/linux/LinuxVipManager.hpp"
#include "runtime/DaemonRuntime.hpp"
#include "runtime/RuntimeLog.hpp"
#include "runtime/ShutdownSignal.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

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

} // namespace

int main(int argc, char** argv) {
    std::string config_path = "configs/easy-failover.toml";
    bool validate_config = false;
    bool dry_run = false;
    bool show_version = false;

    CLI::App app{"easy-failover virtual IP failover agent"};
    argv = app.ensure_utf8(argv);

    app.add_option("--config", config_path, "Path to TOML config file")
        ->capture_default_str()
        ->check(CLI::ExistingFile);
    app.add_flag("--validate-config", validate_config, "Validate config and exit");
    app.add_flag("--dry-run", dry_run, "Log intended actions without changing system state");
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
        easyfailover::DisabledHeartbeatTransport heartbeat_transport;
        auto shutdown_state = easyfailover::ShutdownSignalState{};
        easyfailover::pollShutdownSignals(shutdown_state);
        const auto loop_result = easyfailover::runDaemonRuntimeLoop(
            easyfailover::DaemonLoopRequest{
                .config = config,
                .options = easyfailover::DaemonLoopOptions{
                    .runtime_options = easyfailover::DaemonRuntimeOptions{.dry_run = dry_run},
                    .max_iterations = 1},
                .initial_state = easyfailover::DaemonLifecycleState::Stopped,
                .shutdown_state = &shutdown_state,
                .config_prevalidated = true},
            vip_manager, ownership_probe, heartbeat_transport);
        spdlog::info("{}", easyfailover::formatRuntimeLoopEvent(
                              loop_result,
                              easyfailover::RuntimeLogContext{.node_id = config.node_id,
                                                              .dry_run = dry_run}));
        for (std::size_t index = 0; index < loop_result.vip_operations.size(); ++index) {
            spdlog::info("{}",
                         easyfailover::formatRuntimeVipOperationEvent(
                             loop_result.vip_operations.at(index), index));
        }
        if (!loop_result.validation_errors.empty()) {
            logValidationErrors(loop_result.validation_errors);
            return EXIT_FAILURE;
        }
        if (loop_result.final_state == easyfailover::DaemonLifecycleState::Faulted) {
            return EXIT_FAILURE;
        }

        spdlog::info("heartbeat bind={} interval_ms={} timeout_ms={}", config.heartbeat.bind,
                     config.heartbeat.interval_ms, config.heartbeat.timeout_ms);
        spdlog::info("health command='{}' interval_ms={} timeout_ms={}", config.health.command,
                     config.health.interval_ms, config.health.timeout_ms);
        spdlog::info("configured peers={}", config.peers.size());

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
