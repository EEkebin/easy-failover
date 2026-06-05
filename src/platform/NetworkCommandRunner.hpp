#pragma once

#include <string>
#include <vector>

namespace easyfailover {

struct NetworkCommandRequest {
    std::string executable;
    std::vector<std::string> arguments;
    bool dry_run = true;
};

struct NetworkCommandResult {
    NetworkCommandRequest request;
    int exit_code = 0;
    bool executed = false;
    bool dry_run = true;
    std::string output;
    std::string error;
};

class NetworkCommandRunner {
  public:
    virtual ~NetworkCommandRunner() = default;

    [[nodiscard]] virtual NetworkCommandResult run(const NetworkCommandRequest& request) = 0;
};

class DryRunNetworkCommandRunner final : public NetworkCommandRunner {
  public:
    [[nodiscard]] NetworkCommandResult run(const NetworkCommandRequest& request) override;
};

} // namespace easyfailover
