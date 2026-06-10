#include "discovery/PeerTable.hpp"

namespace easyfailover {

std::string PeerTable::identityOf(const Beacon& beacon) {
    return !beacon.mac.empty() ? beacon.mac : beacon.node_id;
}

void PeerTable::observe(const Beacon& beacon, std::int64_t now_ms) {
    const std::string key = identityOf(beacon);
    if (key.empty()) {
        return;
    }
    entries_[key] = Entry{beacon, now_ms};
}

std::vector<PeerStatus> PeerTable::activePeers(std::int64_t now_ms, std::int64_t timeout_ms,
                                               std::string_view self_identity) const {
    std::vector<PeerStatus> peers;
    peers.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        if (!self_identity.empty() && key == self_identity) {
            continue;
        }
        if (now_ms - entry.last_seen_ms > timeout_ms) {
            continue;
        }
        peers.push_back(peerStatusFromBeacon(entry.beacon));
    }
    return peers;
}

void PeerTable::prune(std::int64_t now_ms, std::int64_t timeout_ms) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now_ms - it->second.last_seen_ms > timeout_ms) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace easyfailover
