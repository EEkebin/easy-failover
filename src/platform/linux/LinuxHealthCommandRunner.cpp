#include "platform/linux/LinuxHealthCommandRunner.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace easyfailover {
namespace {

constexpr int kProcessErrorExitCode = 127;
constexpr int kSignalExitCodeBase = 128;
constexpr int kTimeoutExitCode = 124;
constexpr auto kPollInterval = std::chrono::milliseconds{10};
constexpr auto kTerminateGracePeriod = std::chrono::milliseconds{100};

[[nodiscard]] std::string errnoMessage(const std::string& context) {
    return context + ": " + std::strerror(errno);
}

[[nodiscard]] CommandResult resultFromStatus(const int status) {
    if (WIFEXITED(status)) {
        return CommandResult{.exit_code = WEXITSTATUS(status), .timed_out = false, .error = ""};
    }

    if (WIFSIGNALED(status)) {
        return CommandResult{.exit_code = kSignalExitCodeBase + WTERMSIG(status),
                             .timed_out = false,
                             .error = "health command terminated by signal"};
    }

    return CommandResult{.exit_code = kProcessErrorExitCode,
                         .timed_out = false,
                         .error = "health command ended with unknown status"};
}

void terminateProcessGroup(const pid_t child_pid) {
    static_cast<void>(kill(-child_pid, SIGTERM));

    const auto deadline = std::chrono::steady_clock::now() + kTerminateGracePeriod;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t wait_result = waitpid(child_pid, nullptr, WNOHANG);
        if (wait_result == child_pid || (wait_result == -1 && errno == ECHILD)) {
            return;
        }

        if (wait_result == -1 && errno != EINTR) {
            break;
        }

        std::this_thread::sleep_for(kPollInterval);
    }

    static_cast<void>(kill(-child_pid, SIGKILL));
    while (waitpid(child_pid, nullptr, 0) == -1 && errno == EINTR) {
    }
}

} // namespace

CommandResult LinuxHealthCommandRunner::run(const std::string& command,
                                            const std::int64_t timeout_ms) {
    const pid_t child_pid = fork();
    if (child_pid == -1) {
        return CommandResult{.exit_code = kProcessErrorExitCode,
                             .timed_out = false,
                             .error = errnoMessage("failed to fork health command")};
    }

    if (child_pid == 0) {
        static_cast<void>(setpgid(0, 0));
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(kProcessErrorExitCode);
    }

    static_cast<void>(setpgid(child_pid, child_pid));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
    while (true) {
        int status = 0;
        const pid_t wait_result = waitpid(child_pid, &status, WNOHANG);
        if (wait_result == child_pid) {
            return resultFromStatus(status);
        }

        if (wait_result == -1) {
            if (errno == EINTR) {
                continue;
            }

            return CommandResult{.exit_code = kProcessErrorExitCode,
                                 .timed_out = false,
                                 .error = errnoMessage("failed to wait for health command")};
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            terminateProcessGroup(child_pid);
            return CommandResult{.exit_code = kTimeoutExitCode,
                                 .timed_out = true,
                                 .error = "health command timed out"};
        }

        std::this_thread::sleep_for(kPollInterval);
    }
}

} // namespace easyfailover
