#pragma once

#include <string>
#include <vector>

// Transport for discovery beacons: broadcast our signed beacon to the LAN and drain whatever
// beacons have arrived. Kept abstract so the runtime loop can be driven by a fake in tests and by
// a real UDP broadcast socket in production. All methods are best-effort and never throw — a
// transient network error must not fault the failover loop.

namespace easyfailover {

class DiscoveryTransport {
  public:
    virtual ~DiscoveryTransport() = default;

    // Best-effort broadcast of one beacon payload to the configured port.
    virtual void broadcast(const std::string& payload) = 0;

    // Non-blocking: return every beacon payload received since the last call (possibly empty).
    [[nodiscard]] virtual std::vector<std::string> drain() = 0;
};

} // namespace easyfailover
