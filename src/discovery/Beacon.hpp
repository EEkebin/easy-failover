#pragma once

#include "core/FailoverDecision.hpp"
#include "core/NodeState.hpp"

#include <cstdint>
#include <optional>
#include <string>

// Discovery beacon: the periodic broadcast a node emits so every other node on the LAN learns of
// it without any static peer config. Each beacon is authenticated with HMAC-SHA256 over a shared
// cluster secret, so a node only joins (and can only contend for the VIP) if it proves membership.
// This header is pure (no sockets): serialize/sign on send, verify/parse on receive.

namespace easyfailover {

constexpr int kBeaconVersion = 1;

struct Beacon {
    std::string cluster;            // pool name; must match the receiver's configured cluster
    std::string node_id;            // human-friendly id (hostname or operator-set)
    std::string mac;                // stable identity (MAC of the VIP interface), lowercased
    int priority = 0;               // higher wins the election
    std::string address;            // advertised heartbeat/contact address, host:port
    bool healthy = false;           // health-check result
    NodeState state = NodeState::Backup;
    std::uint64_t seq = 0;          // monotonically increasing per sender
};

// Serialize a beacon to its canonical signed wire form (field block + trailing `sig=` line).
// The HMAC covers the exact field block, so verification is unambiguous.
[[nodiscard]] std::string signBeacon(const Beacon& beacon, std::string_view secret);

struct BeaconVerifyResult {
    std::optional<Beacon> beacon; // present only when the signature and cluster both check out
    std::string error;            // short, non-sensitive reason when absent
};

// Verify a received wire payload against the shared secret and the expected cluster name, then
// parse it. Fails closed: any signature mismatch, malformed payload, version skew, or
// cluster-name mismatch yields an empty beacon with a reason. The HMAC compare is constant-time.
[[nodiscard]] BeaconVerifyResult verifySignedBeacon(std::string_view wire, std::string_view secret,
                                                    std::string_view expected_cluster);

// Project a verified beacon into the decision layer's PeerStatus (heartbeat_seen = true, since a
// fresh beacon is liveness). Identity is the beacon's node_id.
[[nodiscard]] PeerStatus peerStatusFromBeacon(const Beacon& beacon);

} // namespace easyfailover
