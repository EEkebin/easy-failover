#pragma once

#include "platform/NetworkCommandRunner.hpp"
#include "platform/VipOwnershipProbe.hpp"
#include "platform/linux/LinuxNetworkCommandRunner.hpp"

namespace easyfailover {

class LinuxVipOwnershipProbe final : public VipOwnershipProbe {
  public:
    LinuxVipOwnershipProbe();
    explicit LinuxVipOwnershipProbe(NetworkCommandRunner& command_runner);

    [[nodiscard]] VipOwnershipProbeResult probeLocalVip(
        const std::string& address,
        const std::string& interface) override;

  private:
    LinuxNetworkCommandRunner default_command_runner_;
    NetworkCommandRunner* command_runner_;
};

} // namespace easyfailover
