#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Self-contained SHA-256 and HMAC-SHA256. Vendored so the daemon keeps zero crypto-library
// runtime dependencies. Used to authenticate discovery beacons (see Beacon.hpp): a node only
// trusts a beacon whose HMAC verifies against the shared cluster secret.

namespace easyfailover {

// Raw SHA-256 digest of `data`.
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::string_view data);

// HMAC-SHA256(key, message), returned as a 64-char lowercase hex string.
[[nodiscard]] std::string hmacSha256Hex(std::string_view key, std::string_view message);

// Constant-time comparison of two equal-length-or-not strings. Returns true only when both the
// lengths and every byte match; the running time does not reveal where a mismatch occurred.
[[nodiscard]] bool constantTimeEquals(std::string_view a, std::string_view b);

} // namespace easyfailover
