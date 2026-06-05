#include "platform/linux/LinuxNetworkCommandRunner.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace easyfailover {
namespace {

constexpr int kProcessErrorExitCode = 127;
constexpr int kSignalExitCodeBase = 128;

[[nodiscard]] std::string errnoMessage(const std::string& context) {
    return context + ": " + std::strerror(errno);
}

[[nodiscard]] int exitCodeFromStatus(const int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return kSignalExitCodeBase + WTERMSIG(status);
    }

    return kProcessErrorExitCode;
}

} // namespace

NetworkCommandResult LinuxNetworkCommandRunner::run(const NetworkCommandRequest& request) {
    auto result = NetworkCommandResult{.request = request,
                                       .exit_code = 0,
                                       .executed = false,
                                       .dry_run = request.dry_run,
                                       .output = "",
                                       .error = ""};
    if (request.dry_run) {
        return result;
    }
    if (request.executable.empty()) {
        result.exit_code = kProcessErrorExitCode;
        result.error = "network command executable must not be empty";
        return result;
    }

    int output_pipe[2];
    if (pipe(output_pipe) == -1) {
        result.exit_code = kProcessErrorExitCode;
        result.error = errnoMessage("failed to create network command output pipe");
        return result;
    }

    const pid_t child_pid = fork();
    if (child_pid == -1) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        result.exit_code = kProcessErrorExitCode;
        result.error = errnoMessage("failed to fork network command");
        return result;
    }

    if (child_pid == 0) {
        close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) == -1 ||
            dup2(output_pipe[1], STDERR_FILENO) == -1) {
            _exit(kProcessErrorExitCode);
        }
        close(output_pipe[1]);

        auto argv_storage = std::vector<char*>{};
        argv_storage.reserve(request.arguments.size() + 2);
        argv_storage.push_back(const_cast<char*>(request.executable.c_str()));
        for (const auto& argument : request.arguments) {
            argv_storage.push_back(const_cast<char*>(argument.c_str()));
        }
        argv_storage.push_back(nullptr);

        execvp(request.executable.c_str(), argv_storage.data());
        _exit(kProcessErrorExitCode);
    }

    close(output_pipe[1]);
    result.executed = true;

    auto buffer = std::array<char, 4096>{};
    while (true) {
        const ssize_t bytes_read = read(output_pipe[0], buffer.data(), buffer.size());
        if (bytes_read > 0) {
            result.output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            continue;
        }
        if (bytes_read == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }

        result.error = errnoMessage("failed to read network command output");
        break;
    }
    close(output_pipe[0]);

    int status = 0;
    while (waitpid(child_pid, &status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        result.exit_code = kProcessErrorExitCode;
        result.error = errnoMessage("failed to wait for network command");
        return result;
    }

    result.exit_code = exitCodeFromStatus(status);
    return result;
}

} // namespace easyfailover
