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

[[nodiscard]] std::string formatRuntimeLifecycleEvent(
    const DaemonLifecycleResult& result,
    const RuntimeLogContext& context);

[[nodiscard]] std::string formatRuntimeLoopEvent(const DaemonLoopResult& result,
                                                 const RuntimeLogContext& context);

[[nodiscard]] std::string formatRuntimeVipOperationEvent(
    const VipOperationResult& result,
    std::size_t zero_based_index);

} // namespace easyfailover
