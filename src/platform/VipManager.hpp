#pragma once

#include <string>

namespace easyfailover {

class VipManager {
  public:
    virtual ~VipManager() = default;

    virtual void addVip(const std::string& address, const std::string& interface) = 0;
    virtual void removeVip(const std::string& address, const std::string& interface) = 0;
    virtual void announceVip(const std::string& address, const std::string& interface) = 0;
};

} // namespace easyfailover
