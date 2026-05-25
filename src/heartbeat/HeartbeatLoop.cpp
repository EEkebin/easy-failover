#include "heartbeat/HeartbeatLoop.hpp"

#include "heartbeat/HeartbeatMessage.hpp"

namespace easyfailover {

HeartbeatLoopResult runHeartbeatLoopOnce(const LocalNodeStatus& local,
                                         const std::vector<PeerConfig>& peers,
                                         const std::int64_t receive_timeout_ms,
                                         HeartbeatTransport& transport) {
    HeartbeatLoopResult result;

    const auto payload = serializeHeartbeatMessage(HeartbeatMessage{.node_id = local.node_id,
                                                                    .priority = local.priority,
                                                                    .healthy = local.healthy,
                                                                    .state = local.state});

    result.sends.reserve(peers.size());
    for (const auto& peer : peers) {
        const auto send_result = transport.send(peer.address, payload);
        result.sends.push_back(HeartbeatSendObservation{.peer_id = peer.id,
                                                        .peer_address = send_result.peer_address,
                                                        .sent = send_result.sent,
                                                        .error = send_result.error});
    }

    const auto receive_result = transport.receive(receive_timeout_ms);
    result.receive = HeartbeatReceiveObservation{.attempted = true,
                                                .timed_out = receive_result.timed_out,
                                                .timeout_ms = receive_result.timeout_ms,
                                                .peer_address = "",
                                                .payload = "",
                                                .peer_status = std::nullopt,
                                                .error = receive_result.error};

    if (!receive_result.datagram.has_value()) {
        return result;
    }

    result.receive.peer_address = receive_result.datagram->peer_address;
    result.receive.payload = receive_result.datagram->payload;

    const auto parsed = parseHeartbeatMessage(receive_result.datagram->payload);
    if (!parsed.message.has_value()) {
        result.receive.error = parsed.error;
        return result;
    }

    result.receive.peer_status = peerStatusFromHeartbeat(*parsed.message);
    return result;
}

} // namespace easyfailover
