#pragma once

#include <string>
#include <string_view>

namespace easyfailover {

// Read the MAC address of a network interface, lowercased "aa:bb:cc:dd:ee:ff", or empty on failure
// (missing interface, no permission, non-Linux). Used as a stable node identity for discovery.
[[nodiscard]] std::string readInterfaceMac(std::string_view interface);

} // namespace easyfailover
