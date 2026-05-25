#include "config/Config.hpp"
#include "core/FailoverNode.hpp"
#include "platform/linux/LinuxVipManager.hpp"

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
        if (!validation_errors.empty()) {
            logValidationErrors(validation_errors);
            return EXIT_FAILURE;
        }

        if (validate_config) {
            spdlog::info("config '{}' is valid", config_path);
            return EXIT_SUCCESS;
        }

        easyfailover::FailoverNode node{config.node_id, config.vip.address, config.priority};
        node.printStatus();

        spdlog::info("heartbeat bind={} interval_ms={} timeout_ms={}", config.heartbeat.bind,
                     config.heartbeat.interval_ms, config.heartbeat.timeout_ms);
        spdlog::info("health command='{}' interval_ms={} timeout_ms={}", config.health.command,
                     config.health.interval_ms, config.health.timeout_ms);
        spdlog::info("configured peers={}", config.peers.size());

        easyfailover::LinuxVipManager vip_manager;
        if (dry_run) {
            spdlog::info("dry-run mode enabled; no network state will be changed");
            static_cast<void>(vip_manager.addVip(config.vip.address, config.vip.interface));
            static_cast<void>(vip_manager.announceVip(config.vip.address, config.vip.interface));
        } else {
            spdlog::info("real VIP movement is not implemented yet; no network state changed");
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
