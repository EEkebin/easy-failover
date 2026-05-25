#include "platform/NetworkCommandRunner.hpp"

namespace easyfailover {

NetworkCommandResult DryRunNetworkCommandRunner::run(const NetworkCommandRequest& request) {
    return NetworkCommandResult{.request = request,
                                .exit_code = 0,
                                .executed = false,
                                .dry_run = true,
                                .error = ""};
}

} // namespace easyfailover
