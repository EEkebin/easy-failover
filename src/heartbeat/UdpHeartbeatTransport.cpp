#include "heartbeat/UdpHeartbeatTransport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace easyfailover {

namespace {

[[nodiscard]] std::string errnoMessage(const std::string_view prefix) {
    return std::string{prefix} + ": " + std::strerror(errno);
}

[[nodiscard]] std::string endpointAddress(const sockaddr_in& address) {
    auto host = std::array<char, INET_ADDRSTRLEN>{};
    if (inet_ntop(AF_INET, &address.sin_addr, host.data(), host.size()) == nullptr) {
        return {};
    }

    return std::string{host.data()} + ":" + std::to_string(ntohs(address.sin_port));
}

[[nodiscard]] sockaddr_in socketAddressFromEndpoint(const UdpHeartbeatEndpoint& endpoint) {
    auto address = sockaddr_in{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    if (inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error{"invalid heartbeat IPv4 address '" + endpoint.host + "'"};
    }

    return address;
}

void closeSocket(int& socket_fd) {
    if (socket_fd >= 0) {
        (void)::close(socket_fd);
        socket_fd = -1;
    }
}

} // namespace

UdpHeartbeatEndpointParseResult parseUdpHeartbeatEndpoint(const std::string_view address) {
    const auto separator = address.find(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= address.size() ||
        address.find(':', separator + 1) != std::string_view::npos) {
        return UdpHeartbeatEndpointParseResult{
            .endpoint = std::nullopt,
            .error = "heartbeat address must be an IPv4 host:port pair"};
    }

    const auto host = address.substr(0, separator);
    const auto port_text = address.substr(separator + 1);
    auto parsed_port = 0;
    const auto* port_begin = port_text.data();
    const auto* port_end = port_text.data() + port_text.size();
    const auto parse_result = std::from_chars(port_begin, port_end, parsed_port);
    if (parse_result.ec != std::errc{} || parse_result.ptr != port_end || parsed_port < 0 ||
        parsed_port > 65535) {
        return UdpHeartbeatEndpointParseResult{.endpoint = std::nullopt,
                                               .error = "heartbeat port must be 0-65535"};
    }

    auto ipv4 = in_addr{};
    const auto host_string = std::string{host};
    if (inet_pton(AF_INET, host_string.c_str(), &ipv4) != 1) {
        return UdpHeartbeatEndpointParseResult{.endpoint = std::nullopt,
                                               .error = "heartbeat host must be an IPv4 address"};
    }

    const auto normalized_address = host_string + ":" + std::to_string(parsed_port);
    return UdpHeartbeatEndpointParseResult{
        .endpoint = UdpHeartbeatEndpoint{.host = host_string,
                                         .port = static_cast<std::uint16_t>(parsed_port),
                                         .normalized_address = normalized_address},
        .error = ""};
}

UdpHeartbeatTransport::UdpHeartbeatTransport(std::string bind_address) {
    const auto parsed = parseUdpHeartbeatEndpoint(bind_address);
    if (!parsed.endpoint.has_value()) {
        throw std::runtime_error{"invalid heartbeat bind address '" + bind_address + "': " +
                                 parsed.error};
    }

    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        throw std::runtime_error{errnoMessage("failed to open heartbeat UDP socket")};
    }

    const auto reuse_addr = 1;
    if (::setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) < 0) {
        const auto error = errnoMessage("failed to configure heartbeat UDP socket");
        closeSocket(socket_fd_);
        throw std::runtime_error{error};
    }

    auto bind_address_in = socketAddressFromEndpoint(*parsed.endpoint);
    if (::bind(socket_fd_, reinterpret_cast<const sockaddr*>(&bind_address_in),
               sizeof(bind_address_in)) < 0) {
        const auto error = errnoMessage("failed to bind heartbeat UDP socket");
        closeSocket(socket_fd_);
        throw std::runtime_error{error};
    }

    auto local_address_in = sockaddr_in{};
    auto local_address_length = socklen_t{sizeof(local_address_in)};
    if (::getsockname(socket_fd_, reinterpret_cast<sockaddr*>(&local_address_in),
                      &local_address_length) < 0) {
        const auto error = errnoMessage("failed to inspect heartbeat UDP socket");
        closeSocket(socket_fd_);
        throw std::runtime_error{error};
    }

    local_address_ = endpointAddress(local_address_in);
}

UdpHeartbeatTransport::~UdpHeartbeatTransport() {
    closeSocket(socket_fd_);
}

UdpHeartbeatTransport::UdpHeartbeatTransport(UdpHeartbeatTransport&& other) noexcept
    : socket_fd_{other.socket_fd_}, local_address_{std::move(other.local_address_)} {
    other.socket_fd_ = -1;
}

UdpHeartbeatTransport&
UdpHeartbeatTransport::operator=(UdpHeartbeatTransport&& other) noexcept {
    if (this != &other) {
        closeSocket(socket_fd_);
        socket_fd_ = other.socket_fd_;
        local_address_ = std::move(other.local_address_);
        other.socket_fd_ = -1;
    }

    return *this;
}

HeartbeatSendResult UdpHeartbeatTransport::send(const std::string& peer_address,
                                                const std::string& payload) {
    const auto parsed = parseUdpHeartbeatEndpoint(peer_address);
    if (!parsed.endpoint.has_value()) {
        return HeartbeatSendResult{.sent = false,
                                   .peer_address = peer_address,
                                   .payload = payload,
                                   .error = parsed.error};
    }

    const auto address = socketAddressFromEndpoint(*parsed.endpoint);
    const auto sent = ::sendto(socket_fd_, payload.data(), payload.size(), 0,
                               reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (sent < 0) {
        return HeartbeatSendResult{.sent = false,
                                   .peer_address = peer_address,
                                   .payload = payload,
                                   .error = errnoMessage("failed to send heartbeat UDP datagram")};
    }

    if (static_cast<std::size_t>(sent) != payload.size()) {
        return HeartbeatSendResult{.sent = false,
                                   .peer_address = peer_address,
                                   .payload = payload,
                                   .error = "short heartbeat UDP datagram send"};
    }

    return HeartbeatSendResult{.sent = true,
                               .peer_address = peer_address,
                               .payload = payload,
                               .error = ""};
}

HeartbeatReceiveResult UdpHeartbeatTransport::receive(const std::int64_t timeout_ms) {
    auto poll_timeout_ms = timeout_ms;
    if (poll_timeout_ms < 0) {
        poll_timeout_ms = 0;
    }
    if (poll_timeout_ms > INT_MAX) {
        poll_timeout_ms = INT_MAX;
    }

    auto descriptor = pollfd{.fd = socket_fd_, .events = POLLIN, .revents = 0};
    const auto poll_result = ::poll(&descriptor, 1, static_cast<int>(poll_timeout_ms));
    if (poll_result < 0) {
        return HeartbeatReceiveResult{.datagram = std::nullopt,
                                      .timed_out = false,
                                      .timeout_ms = timeout_ms,
                                      .error = errnoMessage("failed to poll heartbeat UDP socket")};
    }
    if (poll_result == 0) {
        return HeartbeatReceiveResult{.datagram = std::nullopt,
                                      .timed_out = true,
                                      .timeout_ms = timeout_ms,
                                      .error = ""};
    }

    auto sender_address = sockaddr_in{};
    auto sender_address_length = socklen_t{sizeof(sender_address)};
    auto buffer = std::array<char, 8192>{};
    const auto received = ::recvfrom(socket_fd_, buffer.data(), buffer.size(), 0,
                                     reinterpret_cast<sockaddr*>(&sender_address),
                                     &sender_address_length);
    if (received < 0) {
        return HeartbeatReceiveResult{.datagram = std::nullopt,
                                      .timed_out = false,
                                      .timeout_ms = timeout_ms,
                                      .error =
                                          errnoMessage("failed to receive heartbeat UDP datagram")};
    }

    return HeartbeatReceiveResult{
        .datagram = HeartbeatDatagram{.peer_address = endpointAddress(sender_address),
                                      .payload = std::string{buffer.data(),
                                                             static_cast<std::size_t>(received)}},
        .timed_out = false,
        .timeout_ms = timeout_ms,
        .error = ""};
}

const std::string& UdpHeartbeatTransport::localAddress() const noexcept {
    return local_address_;
}

} // namespace easyfailover
