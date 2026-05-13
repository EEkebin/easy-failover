#pragma once

#include <optional>
#include <string>

namespace easyfailover {

[[nodiscard]] std::optional<std::string> getSystemHostname();

} // namespace easyfailover
