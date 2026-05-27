#pragma once

#include "config/Config.hpp"

#include <string>
#include <string_view>

namespace easyfailover {

enum class LocalApiStartupState {
    Disabled,
    Ready,
    Rejected,
};

struct LocalApiStartupResult {
    LocalApiStartupState state = LocalApiStartupState::Disabled;
    std::string bind;
    std::string detail;
};

[[nodiscard]] constexpr std::string_view toString(const LocalApiStartupState state) {
    switch (state) {
    case LocalApiStartupState::Disabled:
        return "disabled";
    case LocalApiStartupState::Ready:
        return "ready";
    case LocalApiStartupState::Rejected:
        return "rejected";
    }

    return "unknown";
}

[[nodiscard]] LocalApiStartupResult evaluateLocalApiStartup(const ApiConfig& config);

} // namespace easyfailover
