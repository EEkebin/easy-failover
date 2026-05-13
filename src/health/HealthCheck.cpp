#include "health/HealthCheck.hpp"

namespace easyfailover {

HealthCheckResult evaluateHealthCheck(const HealthConfig& config, HealthCommandRunner& runner) {
    if (config.command.empty()) {
        return HealthCheckResult{.status = HealthStatus::Healthy,
                                 .detail = "no health command configured"};
    }

    const auto result = runner.run(config.command, config.timeout_ms);

    if (result.timed_out) {
        return HealthCheckResult{.status = HealthStatus::Unhealthy,
                                 .detail = "health command timed out"};
    }

    if (!result.error.empty()) {
        return HealthCheckResult{.status = HealthStatus::Unhealthy, .detail = result.error};
    }

    if (result.exit_code == 0) {
        return HealthCheckResult{.status = HealthStatus::Healthy,
                                 .detail = "health command exited successfully"};
    }

    return HealthCheckResult{.status = HealthStatus::Unhealthy,
                             .detail = "health command exited with nonzero status"};
}

} // namespace easyfailover
