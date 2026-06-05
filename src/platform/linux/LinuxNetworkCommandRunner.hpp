#pragma once

#include "platform/NetworkCommandRunner.hpp"

namespace easyfailover {

class LinuxNetworkCommandRunner final : public NetworkCommandRunner {
  public:
    [[nodiscard]] NetworkCommandResult run(const NetworkCommandRequest& request) override;
};

} // namespace easyfailover
