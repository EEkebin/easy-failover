#pragma once

#include "platform/NetworkCommandRunner.hpp"
#include "platform/VipManager.hpp"

namespace easyfailover {

struct LinuxVipManagerOptions {
    bool allow_network_mutation = false;
    bool dry_run = true;
};

class LinuxVipManager final : public VipManager {
  public:
    LinuxVipManager();
    explicit LinuxVipManager(NetworkCommandRunner& command_runner,
                             LinuxVipManagerOptions options = {});

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

    DryRunNetworkCommandRunner default_command_runner_;
    NetworkCommandRunner* command_runner_;
    LinuxVipManagerOptions options_;
};

} // namespace easyfailover
