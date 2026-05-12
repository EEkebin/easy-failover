#include "platform/linux/LinuxVipManager.hpp"

#include <spdlog/spdlog.h>

namespace easyfailover {

void LinuxVipManager::addVip(const std::string& address, const std::string& interface) {
    spdlog::info("dry-run stub: would add VIP {} to interface {}", address, interface);
    // TODO(v1): Shell out to iproute2 to add the address.
    // TODO(v2): Replace shelling out with a native netlink backend.
}

void LinuxVipManager::removeVip(const std::string& address, const std::string& interface) {
    spdlog::info("dry-run stub: would remove VIP {} from interface {}", address, interface);
    // TODO(v1): Shell out to iproute2 to remove the address.
    // TODO(v2): Replace shelling out with a native netlink backend.
}

void LinuxVipManager::announceVip(const std::string& address, const std::string& interface) {
    spdlog::info("dry-run stub: would announce VIP {} on interface {}", address, interface);
    // TODO(v1): Shell out to arping for IPv4 gratuitous ARP announcements.
    // TODO(later): Add IPv6 neighbor advertisements.
}

} // namespace easyfailover
