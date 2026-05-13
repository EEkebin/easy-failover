#pragma once

#include "health/HealthCheck.hpp"

#include <cstdint>
#include <string>

namespace easyfailover {

class LinuxHealthCommandRunner final : public HealthCommandRunner {
  public:
    [[nodiscard]] CommandResult run(const std::string& command,
                                    std::int64_t timeout_ms) override;
};

} // namespace easyfailover
