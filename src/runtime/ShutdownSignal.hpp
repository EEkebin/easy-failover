#pragma once

#include <atomic>
#include <string_view>

namespace easyfailover {

enum class ShutdownSignal {
    None,
    Interrupt,
    Terminate,
    Unknown,
};

struct SignalHandlerInstallResult {
    bool success = true;
    bool interrupt_handler_installed = false;
    bool terminate_handler_installed = false;
};

class ShutdownSignalState {
  public:
    void requestShutdown(ShutdownSignal signal = ShutdownSignal::Unknown);
    [[nodiscard]] bool shutdownRequested() const;
    [[nodiscard]] ShutdownSignal signal() const;
    [[nodiscard]] std::string_view reason() const;

  private:
    std::atomic<bool> shutdown_requested_ = false;
    std::atomic<ShutdownSignal> signal_ = ShutdownSignal::None;
};

[[nodiscard]] constexpr std::string_view toString(const ShutdownSignal signal) {
    switch (signal) {
    case ShutdownSignal::None:
        return "none";
    case ShutdownSignal::Interrupt:
        return "interrupt";
    case ShutdownSignal::Terminate:
        return "terminate";
    case ShutdownSignal::Unknown:
        return "unknown";
    }

    return "unknown";
}

[[nodiscard]] SignalHandlerInstallResult installShutdownSignalHandlers();
void pollShutdownSignals(ShutdownSignalState& state);

#if defined(EASY_FAILOVER_BUILD_TESTING)
void resetPendingShutdownSignalForTest();
#endif

} // namespace easyfailover
