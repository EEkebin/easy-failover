#include "discovery/Beacon.hpp"

#include "discovery/Hmac.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <unordered_map>

namespace easyfailover {

namespace {

constexpr std::string_view kSigDelim = "\nsig=";

NodeState stateFromString(std::string_view value) {
    if (value == "master") {
        return NodeState::Master;
    }
    if (value == "candidate") {
        return NodeState::Candidate;
    }
    if (value == "fault") {
        return NodeState::Fault;
    }
    return NodeState::Backup;
}

// Build the canonical field block (everything the HMAC covers). Fixed key order so signing and
// verification are byte-identical. String fields here are node/system controlled and newline-free.
std::string serializeFields(const Beacon& beacon) {
    std::string out;
    out += "v=" + std::to_string(kBeaconVersion) + "\n";
    out += "cluster=" + beacon.cluster + "\n";
    out += "node=" + beacon.node_id + "\n";
    out += "mac=" + beacon.mac + "\n";
    out += "prio=" + std::to_string(beacon.priority) + "\n";
    out += "addr=" + beacon.address + "\n";
    out += std::string("healthy=") + (beacon.healthy ? "1" : "0") + "\n";
    out += "state=" + std::string(toString(beacon.state)) + "\n";
    out += "seq=" + std::to_string(beacon.seq) + "\n";
    return out;
}

// Parse a field block (already HMAC-verified, so trusted) into a Beacon.
Beacon parseFields(std::string_view block) {
    std::unordered_map<std::string, std::string> fields;
    std::size_t pos = 0;
    while (pos < block.size()) {
        const std::size_t nl = block.find('\n', pos);
        const std::string_view line =
            block.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        const std::size_t eq = line.find('=');
        if (eq != std::string_view::npos) {
            fields.emplace(std::string(line.substr(0, eq)), std::string(line.substr(eq + 1)));
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }

    Beacon beacon;
    const auto get = [&](const char* key) -> std::string {
        const auto it = fields.find(key);
        return it == fields.end() ? std::string{} : it->second;
    };
    beacon.cluster = get("cluster");
    beacon.node_id = get("node");
    beacon.mac = get("mac");
    beacon.address = get("addr");
    beacon.healthy = get("healthy") == "1";
    beacon.state = stateFromString(get("state"));

    const std::string prio = get("prio");
    std::from_chars(prio.data(), prio.data() + prio.size(), beacon.priority);
    const std::string seq = get("seq");
    std::from_chars(seq.data(), seq.data() + seq.size(), beacon.seq);
    return beacon;
}

bool versionMatches(std::string_view block) {
    const std::string prefix = "v=" + std::to_string(kBeaconVersion) + "\n";
    return block.rfind(prefix, 0) == 0; // block starts with the expected version line
}

} // namespace

std::string signBeacon(const Beacon& beacon, std::string_view secret) {
    const std::string block = serializeFields(beacon);
    const std::string sig = hmacSha256Hex(secret, block);
    return block + "sig=" + sig + "\n";
}

BeaconVerifyResult verifySignedBeacon(std::string_view wire, std::string_view secret,
                                      std::string_view expected_cluster) {
    const std::size_t delim = wire.rfind(kSigDelim);
    if (delim == std::string_view::npos) {
        return {std::nullopt, "missing signature"};
    }
    // The signed region includes the '\n' that terminates the last field line.
    const std::string_view block = wire.substr(0, delim + 1);
    std::string_view sigLine = wire.substr(delim + 1); // "sig=<hex>\n"
    sigLine.remove_prefix(4);                          // drop "sig="
    if (!sigLine.empty() && sigLine.back() == '\n') {
        sigLine.remove_suffix(1);
    }

    const std::string expected = hmacSha256Hex(secret, block);
    if (!constantTimeEquals(expected, sigLine)) {
        return {std::nullopt, "signature mismatch"};
    }
    if (!versionMatches(block)) {
        return {std::nullopt, "unsupported beacon version"};
    }

    Beacon beacon = parseFields(block);
    if (beacon.cluster != expected_cluster) {
        return {std::nullopt, "cluster mismatch"};
    }
    if (beacon.mac.empty() && beacon.node_id.empty()) {
        return {std::nullopt, "beacon has no identity"};
    }
    return {beacon, ""};
}

PeerStatus peerStatusFromBeacon(const Beacon& beacon) {
    PeerStatus status;
    status.node_id = beacon.node_id;
    status.priority = beacon.priority;
    status.healthy = beacon.healthy;
    status.heartbeat_seen = true;
    status.state = beacon.state;
    return status;
}

} // namespace easyfailover
