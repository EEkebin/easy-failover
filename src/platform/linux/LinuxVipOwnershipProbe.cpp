#include "platform/linux/LinuxVipOwnershipProbe.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace easyfailover {
namespace {

[[nodiscard]] std::string addressWithoutPrefix(const std::string& address) {
    const auto prefix_pos = address.find('/');
    if (prefix_pos == std::string::npos) {
        return address;
    }

    return address.substr(0, prefix_pos);
}

[[nodiscard]] bool tokenMatchesAddress(const std::string_view token,
                                       const std::string_view configured_address,
                                       const std::string_view host_address) {
    return token == configured_address || token == host_address;
}

[[nodiscard]] bool outputContainsVip(const std::string& output,
                                     const std::string& configured_address) {
    const auto host_address = addressWithoutPrefix(configured_address);
    auto position = std::size_t{0};
    while (position < output.size()) {
        while (position < output.size() &&
               std::isspace(static_cast<unsigned char>(output.at(position))) != 0) {
            ++position;
        }

        const auto token_start = position;
        while (position < output.size() &&
               std::isspace(static_cast<unsigned char>(output.at(position))) == 0) {
            ++position;
        }
        const auto token = std::string_view{output}.substr(token_start, position - token_start);
        if (token == "inet") {
            while (position < output.size() &&
                   std::isspace(static_cast<unsigned char>(output.at(position))) != 0) {
                ++position;
            }
            const auto address_start = position;
            while (position < output.size() &&
                   std::isspace(static_cast<unsigned char>(output.at(position))) == 0) {
                ++position;
            }
            const auto candidate =
                std::string_view{output}.substr(address_start, position - address_start);
            if (tokenMatchesAddress(candidate, configured_address, host_address)) {
                return true;
            }
        }
    }

    return false;
}

} // namespace

LinuxVipOwnershipProbe::LinuxVipOwnershipProbe()
    : command_runner_{&default_command_runner_} {
}

LinuxVipOwnershipProbe::LinuxVipOwnershipProbe(NetworkCommandRunner& command_runner)
    : command_runner_{&command_runner} {
}

VipOwnershipProbeResult LinuxVipOwnershipProbe::probeLocalVip(const std::string& address,
                                                              const std::string& interface) {
    auto result = VipOwnershipProbeResult{.request = {.address = address, .interface = interface},
                                          .success = false,
                                          .local_owner = false,
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

    auto command = NetworkCommandRequest{.executable = "ip",
                                         .arguments = {"addr", "show", "dev", interface},
                                         .dry_run = false};
    auto command_result = command_runner_->run(command);
    result.commands.push_back(command_result);

    if (!command_result.error.empty() || command_result.exit_code != 0) {
        result.error = command_result.error.empty() ? "VIP ownership probe command failed"
                                                    : command_result.error;
        return result;
    }

    result.local_owner = outputContainsVip(command_result.output, address);
    result.success = true;
    return result;
}

} // namespace easyfailover
