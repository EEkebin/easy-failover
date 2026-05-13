#pragma once

#include "config/Config.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace easyfailover {

enum class HealthStatus { Healthy, Unhealthy };

[[nodiscard]] constexpr std::string_view toString(const HealthStatus status) {
    switch (status) {
    case HealthStatus::Healthy:
        return "healthy";
    case HealthStatus::Unhealthy:
        return "unhealthy";
    }

    return "unknown";
}

struct HealthCheckResult {
    HealthStatus status = HealthStatus::Unhealthy;
    std::string detail;
};

struct CommandResult {
    int exit_code = 0;
    bool timed_out = false;
    std::string error;
};

class HealthCommandRunner {
  public:
    virtual ~HealthCommandRunner() = default;

    [[nodiscard]] virtual CommandResult run(const std::string& command,
                                            std::int64_t timeout_ms) = 0;
};

[[nodiscard]] HealthCheckResult evaluateHealthCheck(const HealthConfig& config,
                                                    HealthCommandRunner& runner);

} // namespace easyfailover
