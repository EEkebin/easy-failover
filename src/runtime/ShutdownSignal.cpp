#include "runtime/ShutdownSignal.hpp"

#include <csignal>

namespace easyfailover {

namespace {

constexpr std::sig_atomic_t kNoSignal = 0;
constexpr std::sig_atomic_t kInterruptSignal = 1;
constexpr std::sig_atomic_t kTerminateSignal = 2;
constexpr std::sig_atomic_t kUnknownSignal = 3;

volatile std::sig_atomic_t g_pending_shutdown_signal = kNoSignal;

void handleShutdownSignal(const int signal) {
    if (signal == SIGINT) {
        g_pending_shutdown_signal = kInterruptSignal;
        return;
    }

    if (signal == SIGTERM) {
        g_pending_shutdown_signal = kTerminateSignal;
        return;
    }

    g_pending_shutdown_signal = kUnknownSignal;
}

[[nodiscard]] ShutdownSignal signalFromPendingValue(const std::sig_atomic_t signal) {
    switch (signal) {
    case kInterruptSignal:
        return ShutdownSignal::Interrupt;
    case kTerminateSignal:
        return ShutdownSignal::Terminate;
    case kNoSignal:
        return ShutdownSignal::None;
    default:
        return ShutdownSignal::Unknown;
    }
}

} // namespace

void ShutdownSignalState::requestShutdown(const ShutdownSignal signal) {
    shutdown_requested_ = true;
    signal_ = signal;
}

bool ShutdownSignalState::shutdownRequested() const {
    return shutdown_requested_;
}

ShutdownSignal ShutdownSignalState::signal() const {
    return signal_;
}

std::string_view ShutdownSignalState::reason() const {
    if (!shutdown_requested_) {
        return "shutdown not requested";
    }

    switch (signal_) {
    case ShutdownSignal::Interrupt:
        return "shutdown requested by interrupt signal";
    case ShutdownSignal::Terminate:
        return "shutdown requested by terminate signal";
    case ShutdownSignal::None:
    case ShutdownSignal::Unknown:
        return "shutdown requested";
    }

    return "shutdown requested";
}

SignalHandlerInstallResult installShutdownSignalHandlers() {
    auto result = SignalHandlerInstallResult{};

    if (std::signal(SIGINT, handleShutdownSignal) == SIG_ERR) {
        result.success = false;
    } else {
        result.interrupt_handler_installed = true;
    }

    if (std::signal(SIGTERM, handleShutdownSignal) == SIG_ERR) {
        result.success = false;
    } else {
        result.terminate_handler_installed = true;
    }

    return result;
}

void pollShutdownSignals(ShutdownSignalState& state) {
    const auto pending_signal = signalFromPendingValue(g_pending_shutdown_signal);
    if (pending_signal != ShutdownSignal::None) {
        state.requestShutdown(pending_signal);
    }
}

#if defined(EASY_FAILOVER_BUILD_TESTING)
void resetPendingShutdownSignalForTest() {
    g_pending_shutdown_signal = kNoSignal;
}
#endif

} // namespace easyfailover
