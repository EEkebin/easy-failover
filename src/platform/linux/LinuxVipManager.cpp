#include "platform/linux/LinuxVipManager.hpp"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace easyfailover {

namespace {

[[nodiscard]] std::string operationName(const VipOperationType type) {
    switch (type) {
    case VipOperationType::Add:
        return "add";
    case VipOperationType::Remove:
        return "remove";
    case VipOperationType::Announce:
        return "announce";
    }

    return "unknown";
}

[[nodiscard]] std::string addressWithoutPrefix(const std::string& address) {
    const auto prefix_pos = address.find('/');
    if (prefix_pos == std::string::npos) {
        return address;
    }

    return address.substr(0, prefix_pos);
}

[[nodiscard]] std::vector<NetworkCommandRequest> buildLinuxVipCommands(
    const VipOperationType type,
    const std::string& address,
    const std::string& interface,
    const bool dry_run) {
    switch (type) {
    case VipOperationType::Add:
        return {NetworkCommandRequest{.executable = "ip",
                                      .arguments = {"addr", "add", address, "dev", interface},
                                      .dry_run = dry_run}};
    case VipOperationType::Remove:
        return {NetworkCommandRequest{.executable = "ip",
                                      .arguments = {"addr", "del", address, "dev", interface},
                                      .dry_run = dry_run}};
    case VipOperationType::Announce:
        return {NetworkCommandRequest{
            .executable = "arping",
            .arguments = {"-A", "-c", "3", "-I", interface, addressWithoutPrefix(address)},
            .dry_run = dry_run}};
    }

    return {};
}

} // namespace

LinuxVipManager::LinuxVipManager()
    : command_runner_{&default_command_runner_}, options_{} {
}

LinuxVipManager::LinuxVipManager(NetworkCommandRunner& command_runner,
                                 const LinuxVipManagerOptions options)
    : command_runner_{&command_runner}, options_{options} {
}

VipOperationResult LinuxVipManager::addVip(const std::string& address,
                                           const std::string& interface) {
    return runOperation(VipOperationType::Add, address, interface);
}

VipOperationResult LinuxVipManager::removeVip(const std::string& address,
                                              const std::string& interface) {
    return runOperation(VipOperationType::Remove, address, interface);
}

VipOperationResult LinuxVipManager::announceVip(const std::string& address,
                                                const std::string& interface) {
    return runOperation(VipOperationType::Announce, address, interface);
}

VipOperationResult LinuxVipManager::runOperation(const VipOperationType type,
                                                 const std::string& address,
                                                 const std::string& interface) {
    const auto effective_dry_run = options_.dry_run || !options_.allow_network_mutation;
    auto result = VipOperationResult{.request = VipOperationRequest{.type = type,
                                                                    .address = address,
                                                                    .interface = interface,
                                                                    .dry_run = effective_dry_run},
                                     .success = false,
                                     .dry_run = effective_dry_run,
                                     .commands = {},
                                     .error = ""};

    if (address.empty()) {
        result.error = "vip address must not be empty";
        return result;
    }

    if (interface.empty()) {
        result.error = "vip interface must not be empty";
        return result;
    }

    const auto commands = buildLinuxVipCommands(type, address, interface, effective_dry_run);
    for (const auto& command : commands) {
        result.commands.push_back(command_runner_->run(command));
    }

    result.success = true;
    for (const auto& command_result : result.commands) {
        if (!command_result.error.empty() || command_result.exit_code != 0) {
            result.success = false;
            result.error = command_result.error.empty() ? "network command failed"
                                                        : command_result.error;
            break;
        }
    }

    if (result.success) {
        spdlog::info("{} VIP {} on interface {}{}", operationName(type), address, interface,
                     effective_dry_run ? " (dry-run)" : "");
    }

    return result;
}

} // namespace easyfailover
