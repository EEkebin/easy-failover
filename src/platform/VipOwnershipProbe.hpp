#pragma once

#include "platform/NetworkCommandRunner.hpp"

#include <string>
#include <vector>

namespace easyfailover {

struct VipOwnershipProbeRequest {
    std::string address;
    std::string interface;
};

struct VipOwnershipProbeResult {
    VipOwnershipProbeRequest request;
    bool success = false;
    bool local_owner = false;
    std::vector<NetworkCommandResult> commands;
    std::string error;
};

class VipOwnershipProbe {
  public:
    virtual ~VipOwnershipProbe() = default;

    [[nodiscard]] virtual VipOwnershipProbeResult probeLocalVip(
        const std::string& address,
        const std::string& interface) = 0;
};

} // namespace easyfailover
