#pragma once

#include "platform/NetworkCommandRunner.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace easyfailover {

enum class VipOperationType {
    Add,
    Remove,
    Announce,
};

[[nodiscard]] constexpr std::string_view toString(const VipOperationType type) {
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

struct VipOperationRequest {
    VipOperationType type = VipOperationType::Add;
    std::string address;
    std::string interface;
    bool dry_run = true;
};

struct VipOperationResult {
    VipOperationRequest request;
    bool success = false;
    bool dry_run = true;
    std::vector<NetworkCommandResult> commands;
    std::string error;
};

class VipManager {
  public:
    virtual ~VipManager() = default;

    [[nodiscard]] virtual VipOperationResult addVip(const std::string& address,
                                                    const std::string& interface) = 0;
    [[nodiscard]] virtual VipOperationResult removeVip(const std::string& address,
                                                       const std::string& interface) = 0;
    [[nodiscard]] virtual VipOperationResult announceVip(const std::string& address,
                                                         const std::string& interface) = 0;
};

} // namespace easyfailover
