#pragma once

#include "platform/VipManager.hpp"

namespace easyfailover {

class LinuxVipManager final : public VipManager {
  public:
    void addVip(const std::string& address, const std::string& interface) override;
    void removeVip(const std::string& address, const std::string& interface) override;
    void announceVip(const std::string& address, const std::string& interface) override;
};

} // namespace easyfailover
