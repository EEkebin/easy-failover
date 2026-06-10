#include "platform/MacAddress.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

namespace easyfailover {

std::string readInterfaceMac(std::string_view interface) {
    if (interface.empty()) {
        return {};
    }
    // Reject path-traversal / odd characters; interface names are short and have no '/', '.',
    // or whitespace.
    for (const char c : interface) {
        if (c == '/' || c == '.' || std::isspace(static_cast<unsigned char>(c)) != 0) {
            return {};
        }
    }

    std::ifstream file{"/sys/class/net/" + std::string{interface} + "/address"};
    if (!file) {
        return {};
    }
    std::string mac;
    std::getline(file, mac);

    // Trim trailing whitespace/newline and lowercase.
    while (!mac.empty() && (std::isspace(static_cast<unsigned char>(mac.back())) != 0)) {
        mac.pop_back();
    }
    std::transform(mac.begin(), mac.end(), mac.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // A wholly-zero MAC is not a usable identity.
    if (mac.empty() || mac == "00:00:00:00:00:00") {
        return {};
    }
    return mac;
}

} // namespace easyfailover
