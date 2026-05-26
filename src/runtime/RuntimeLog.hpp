#pragma once

#include "runtime/DaemonRuntime.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace easyfailover {

struct RuntimeLogContext {
    std::string_view node_id;
    bool dry_run = true;
};

[[nodiscard]] constexpr std::string_view toString(const VipOperationType type) {
    switch (type) {
    case VipOperationType::Add:
        return "add";
    case VipOperationType::Remove:
        return "remove";
    case VipOperationType::Announce:
        return "announce";
    }

    return "unknown";
}

[[nodiscard]] std::string formatRuntimeLifecycleEvent(
    const DaemonLifecycleResult& result,
    const RuntimeLogContext& context);

[[nodiscard]] std::string formatRuntimeVipOperationEvent(
    const VipOperationResult& result,
    std::size_t index);

} // namespace easyfailover
