#include "platform/linux/LinuxHealthCommandRunner.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
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
    // Create a pipe so the child can report execl errno back to the parent.
    // FD_CLOEXEC on the write end ensures it is closed automatically on a
    // successful exec, giving the parent an EOF that signals exec success.
    int exec_error_pipe[2];
    if (pipe(exec_error_pipe) == -1) {
        return CommandResult{.exit_code = kProcessErrorExitCode,
                             .timed_out = false,
                             .error = errnoMessage("failed to create exec-error pipe")};
    }
    if (fcntl(exec_error_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
        close(exec_error_pipe[0]);
        close(exec_error_pipe[1]);
        return CommandResult{.exit_code = kProcessErrorExitCode,
                             .timed_out = false,
                             .error = errnoMessage("failed to configure exec-error pipe")};
    }

    const pid_t child_pid = fork();
    if (child_pid == -1) {
        close(exec_error_pipe[0]);
        close(exec_error_pipe[1]);
        return CommandResult{.exit_code = kProcessErrorExitCode,
                             .timed_out = false,
                             .error = errnoMessage("failed to fork health command")};
    }

    if (child_pid == 0) {
        close(exec_error_pipe[0]);
        static_cast<void>(setpgid(0, 0));
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        // execl failed: write errno back to parent, retrying on EINTR/partial writes.
        const int exec_errno = errno;
        const char* buf = reinterpret_cast<const char*>(&exec_errno);
        size_t remaining = sizeof(exec_errno);
        while (remaining > 0) {
            const ssize_t written = write(exec_error_pipe[1], buf, remaining);
            if (written > 0) {
                buf += written;
                remaining -= static_cast<size_t>(written);
            } else if (written == -1 && errno == EINTR) {
                continue;
            } else {
                break; // unrecoverable write error; parent will still exit nonzero
            }
        }
        _exit(kProcessErrorExitCode);
    }

    // Parent: close write end and check whether exec succeeded.
    close(exec_error_pipe[1]);
    int child_exec_errno = 0;
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(sizeof(child_exec_errno))) {
        const ssize_t n = read(exec_error_pipe[0],
                               reinterpret_cast<char*>(&child_exec_errno) + total,
                               sizeof(child_exec_errno) - static_cast<size_t>(total));
        if (n > 0) {
            total += n;
        } else if (n == 0) {
            break; // EOF: exec succeeded
        } else if (errno != EINTR) {
            break; // unexpected read error
        }
    }
    close(exec_error_pipe[0]);

    if (total == static_cast<ssize_t>(sizeof(child_exec_errno))) {
        // exec failed: reap the child and surface the errno.
        while (waitpid(child_pid, nullptr, 0) == -1 && errno == EINTR) {
        }
        return CommandResult{
            .exit_code = kProcessErrorExitCode,
            .timed_out = false,
            .error = "failed to exec health command: " + std::string(std::strerror(child_exec_errno))};
    }

    if (total > 0) {
        // Partial read before EOF: exec-error protocol failure.
        while (waitpid(child_pid, nullptr, 0) == -1 && errno == EINTR) {
        }
        return CommandResult{.exit_code = kProcessErrorExitCode,
                             .timed_out = false,
                             .error = "exec-error protocol failure: partial errno received"};
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
