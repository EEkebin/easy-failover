#include "platform/Hostname.hpp"

#include <array>
#include <unistd.h>

namespace easyfailover {

std::optional<std::string> getSystemHostname() {
    std::array<char, 256> buffer{};
    if (gethostname(buffer.data(), buffer.size()) != 0) {
        return std::nullopt;
    }

    buffer.back() = '\0';
    std::string hostname{buffer.data()};
    if (hostname.empty()) {
        return std::nullopt;
    }

    return hostname;
}

} // namespace easyfailover
