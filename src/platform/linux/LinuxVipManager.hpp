#pragma once

#include "platform/NetworkCommandRunner.hpp"
#include "platform/VipManager.hpp"
#include "platform/linux/LinuxNetworkCommandRunner.hpp"

namespace easyfailover {

struct LinuxVipManagerOptions {
    bool allow_network_mutation = false;
    bool dry_run = true;
};

class LinuxVipManager final : public VipManager {
  public:
    LinuxVipManager();
    explicit LinuxVipManager(LinuxVipManagerOptions options);
    explicit LinuxVipManager(NetworkCommandRunner& command_runner,
                             LinuxVipManagerOptions options = {});
    LinuxVipManager(const LinuxVipManager&) = delete;
    LinuxVipManager& operator=(const LinuxVipManager&) = delete;
    LinuxVipManager(LinuxVipManager&&) = delete;
    LinuxVipManager& operator=(LinuxVipManager&&) = delete;

    [[nodiscard]] VipOperationResult addVip(const std::string& address,
                                            const std::string& interface) override;
    [[nodiscard]] VipOperationResult removeVip(const std::string& address,
                                               const std::string& interface) override;
    [[nodiscard]] VipOperationResult announceVip(const std::string& address,
                                                 const std::string& interface) override;

  private:
    [[nodiscard]] VipOperationResult runOperation(VipOperationType type,
                                                  const std::string& address,
                                                  const std::string& interface);

    LinuxNetworkCommandRunner default_command_runner_;
    NetworkCommandRunner* command_runner_;
    LinuxVipManagerOptions options_;
};

} // namespace easyfailover
