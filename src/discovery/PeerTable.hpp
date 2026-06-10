#pragma once

#include "core/FailoverDecision.hpp"
#include "discovery/Beacon.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Live roster of nodes discovered via beacons. Pure and time-injected (no clock of its own): the
// runtime calls observe() for each verified beacon and activePeers() each tick. Entries expire
// when no fresh beacon has arrived within the timeout, so a node that goes silent drops out of the
// election automatically. Identity is the beacon MAC (falling back to node_id).

namespace easyfailover {

class PeerTable {
  public:
    // Record a verified beacon seen at now_ms. Keyed by identity; the latest observation wins.
    void observe(const Beacon& beacon, std::int64_t now_ms);

    // Peers seen within timeout_ms of now_ms, as decision-layer PeerStatus. The entry whose
    // identity equals self_identity (this node's own MAC/id) is excluded so a node never counts
    // itself as a peer.
    [[nodiscard]] std::vector<PeerStatus> activePeers(std::int64_t now_ms, std::int64_t timeout_ms,
                                                      std::string_view self_identity = {}) const;

    // Drop entries older than timeout_ms (housekeeping; activePeers already filters).
    void prune(std::int64_t now_ms, std::int64_t timeout_ms);

    [[nodiscard]] std::size_t size() const { return entries_.size(); }

    // The identity key a beacon resolves to (MAC, else node_id).
    [[nodiscard]] static std::string identityOf(const Beacon& beacon);

  private:
    struct Entry {
        Beacon beacon;
        std::int64_t last_seen_ms = 0;
    };
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace easyfailover
