#include "runtime/RuntimeLog.hpp"

#include <array>
#include <cstddef>
#include <sstream>

namespace easyfailover {

namespace {

[[nodiscard]] const char* boolValue(const bool value) {
    return value ? "true" : "false";
}

[[nodiscard]] std::string quotedValue(const std::string_view value) {
    constexpr auto kHexDigits = std::array<char, 16>{'0', '1', '2', '3', '4', '5', '6', '7',
                                                     '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    auto escaped = std::string{};
    escaped.reserve(value.size());

    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\\':
        case '"':
            escaped.push_back('\\');
            escaped.push_back(character);
            break;
        default:
            if (byte < 0x20 || byte == 0x7F) {
                escaped += "\\x";
                escaped.push_back(kHexDigits.at(byte >> 4U));
                escaped.push_back(kHexDigits.at(byte & 0x0FU));
            } else {
                escaped.push_back(character);
            }
            break;
        }
    }

    return "\"" + escaped + "\"";
}

} // namespace

std::string formatRuntimeLifecycleEvent(const DaemonLifecycleResult& result,
                                        const RuntimeLogContext& context) {
    auto output = std::ostringstream{};
    output << "event=daemon_lifecycle_result"
           << " node_id=" << quotedValue(context.node_id)
           << " initial_state=" << toString(result.initial_state)
           << " final_state=" << toString(result.final_state)
           << " started=" << boolValue(result.started)
           << " iteration_ran=" << boolValue(result.iteration_ran)
           << " stopped=" << boolValue(result.stopped)
           << " dry_run=" << boolValue(context.dry_run)
           << " validation_errors=" << result.validation_errors.size()
           << " vip_operations=" << result.vip_operations.size()
           << " detail=" << quotedValue(result.detail);
    return output.str();
}

std::string formatRuntimeVipOperationEvent(const VipOperationResult& result,
                                           const std::size_t index) {
    auto output = std::ostringstream{};
    output << "event=vip_operation"
           << " index=" << index
           << " operation=" << toString(result.request.type)
           << " address=" << quotedValue(result.request.address)
           << " interface=" << quotedValue(result.request.interface)
           << " request_dry_run=" << boolValue(result.request.dry_run)
           << " result_dry_run=" << boolValue(result.dry_run)
           << " success=" << boolValue(result.success)
           << " commands=" << result.commands.size()
           << " error=" << quotedValue(result.error);
    return output.str();
}

} // namespace easyfailover
