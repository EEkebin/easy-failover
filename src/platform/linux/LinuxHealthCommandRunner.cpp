#include "platform/linux/LinuxHealthCommandRunner.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <climits>
#include <fcntl.h>
#include <poll.h>
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

void terminateProcessGroup(const pid_t child_pid, const bool use_pgid) {
    const pid_t target = use_pgid ? -child_pid : child_pid;
    static_cast<void>(kill(target, SIGTERM));

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

    static_cast<void>(kill(target, SIGKILL));
    while (waitpid(child_pid, nullptr, 0) == -1 && errno == EINTR) {
    }
}

} // namespace

CommandResult LinuxHealthCommandRunner::run(const std::string& command,
                                            const std::int64_t timeout_ms) {
    // Start the timeout clock at function entry so health.timeout_ms bounds the
    // entire run() call, including exec startup and the pipe handshake below.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};

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
        if (setpgid(0, 0) == -1) {
            // setpgid failed: write negative errno as sentinel to distinguish
            // from execl failure (which writes a positive errno), then exit.
            // Both child and parent share the same host ABI (fork/exec IPC),
            // so the int representation is identical on both ends of the pipe.
            const int neg_errno = -errno;
            const char* buf = reinterpret_cast<const char*>(&neg_errno);
            size_t remaining = sizeof(neg_errno);
            while (remaining > 0) {
                const ssize_t written = write(exec_error_pipe[1], buf, remaining);
                if (written > 0) {
                    buf += written;
                    remaining -= static_cast<size_t>(written);
                } else if (written == -1 && errno == EINTR) {
                    continue;
                } else {
                    break;
                }
            }
            _exit(kProcessErrorExitCode);
        }
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

    // Parent: close write end and wait for exec result within the deadline.
    close(exec_error_pipe[1]);

    // Poll the exec-error pipe with the remaining timeout budget so that the
    // exec handshake doesn't silently exceed health.timeout_ms.
    {
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                close(exec_error_pipe[0]);
                terminateProcessGroup(child_pid, true);
                return CommandResult{.exit_code = kTimeoutExitCode,
                                     .timed_out = true,
                                     .error = "health command timed out"};
            }
            const auto remaining_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const int poll_ms =
                remaining_ms > INT_MAX ? INT_MAX : static_cast<int>(remaining_ms);
            struct pollfd pfd{};
            pfd.fd    = exec_error_pipe[0];
            pfd.events = POLLIN;
            const int ret = poll(&pfd, 1, poll_ms);
            if (ret > 0) {
                break; // data (or EOF) available; proceed to the read loop
            }
            if (ret == 0) {
                // Timed out waiting for exec result.
                close(exec_error_pipe[0]);
                terminateProcessGroup(child_pid, true);
                return CommandResult{.exit_code = kTimeoutExitCode,
                                     .timed_out = true,
                                     .error = "health command timed out"};
            }
            if (errno != EINTR) {
                close(exec_error_pipe[0]);
                terminateProcessGroup(child_pid, true);
                return CommandResult{.exit_code = kProcessErrorExitCode,
                                     .timed_out = false,
                                     .error = errnoMessage("failed to poll exec-error pipe")};
            }
            // EINTR: retry with an updated remaining time.
        }
    }

    int child_exec_errno = 0;
    ssize_t total = 0;
    bool pipe_read_failed = false;
    int pipe_read_errno = 0;
    while (total < static_cast<ssize_t>(sizeof(child_exec_errno))) {
        const ssize_t n = read(exec_error_pipe[0],
                               reinterpret_cast<char*>(&child_exec_errno) + total,
                               sizeof(child_exec_errno) - static_cast<size_t>(total));
        if (n > 0) {
            total += n;
        } else if (n == 0) {
            break; // EOF: exec succeeded (FD_CLOEXEC closed write end on successful exec)
        } else if (errno == EINTR) {
            continue; // interrupted; retry
        } else {
            pipe_read_errno = errno;
            pipe_read_failed = true;
            break; // unexpected read error
        }
    }
    close(exec_error_pipe[0]);

    if (total == static_cast<ssize_t>(sizeof(child_exec_errno))) {
        // Reap the child before returning.
        while (waitpid(child_pid, nullptr, 0) == -1 && errno == EINTR) {
        }
        if (child_exec_errno < 0) {
            // Negative sentinel: setpgid failed in child.
            // (This branch always returns; it never falls through to the partial-read check below.)
            return CommandResult{
                .exit_code = kProcessErrorExitCode,
                .timed_out = false,
                .error = "failed to set process group for health command: " +
                         std::string(std::strerror(-child_exec_errno))};
        }
        // Positive errno: execl failed.
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

    if (pipe_read_failed) {
        // read() failed with a non-EINTR error before any bytes were received;
        // we cannot determine whether exec succeeded or failed.
        while (waitpid(child_pid, nullptr, 0) == -1 && errno == EINTR) {
        }
        return CommandResult{.exit_code = kProcessErrorExitCode,
                             .timed_out = false,
                             .error = "failed to read exec-error pipe: " +
                                      std::string(std::strerror(pipe_read_errno))};
    }

    // Parent-side setpgid is a race-condition fix: if the child hasn't called
    // setpgid(0,0) yet by the time we reach here, this ensures it is in its own
    // group before we might need to kill it.  EACCES means the child already
    // exec'd (its own setpgid succeeded first); ESRCH means it already exited.
    // Any other error means we cannot reliably kill the process group, so fall
    // back to killing just the child PID on timeout.
    bool use_pgid = true;
    if (setpgid(child_pid, child_pid) == -1 && errno != EACCES && errno != ESRCH) {
        use_pgid = false;
    }

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
            terminateProcessGroup(child_pid, use_pgid);
            return CommandResult{.exit_code = kTimeoutExitCode,
                                 .timed_out = true,
                                 .error = "health command timed out"};
        }

        std::this_thread::sleep_for(kPollInterval);
    }
}

} // namespace easyfailover
