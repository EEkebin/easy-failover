#include "discovery/Hmac.hpp"

#include <array>
#include <cstring>

namespace easyfailover {

namespace {

// --- SHA-256 (FIPS 180-4) --------------------------------------------------------------------

constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, std::uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

struct Sha256Ctx {
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    void transform(const std::uint8_t* p) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(p[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
};

std::array<std::uint8_t, 32> sha256Raw(const std::uint8_t* data, std::size_t len) {
    Sha256Ctx ctx;
    std::size_t full = len / 64;
    for (std::size_t i = 0; i < full; ++i) {
        ctx.transform(data + i * 64);
    }

    // Final block(s) with padding.
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rem = len - full * 64;
    std::memcpy(tail.data(), data + full * 64, rem);
    tail[rem] = 0x80;
    const std::size_t total = (rem < 56) ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i) {
        tail[total - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    ctx.transform(tail.data());
    if (total == 128) {
        ctx.transform(tail.data() + 64);
    }

    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(ctx.h[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(ctx.h[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(ctx.h[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(ctx.h[i]);
    }
    return out;
}

std::string toHex(const std::array<std::uint8_t, 32>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

} // namespace

std::array<std::uint8_t, 32> sha256(std::string_view data) {
    return sha256Raw(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::string hmacSha256Hex(std::string_view key, std::string_view message) {
    constexpr std::size_t kBlock = 64;
    std::array<std::uint8_t, kBlock> k0{};

    if (key.size() > kBlock) {
        const auto hashed = sha256(key);
        std::memcpy(k0.data(), hashed.data(), hashed.size());
    } else {
        std::memcpy(k0.data(), key.data(), key.size());
    }

    std::string inner;
    inner.reserve(kBlock + message.size());
    for (std::size_t i = 0; i < kBlock; ++i) {
        inner.push_back(static_cast<char>(k0[i] ^ 0x36));
    }
    inner.append(message);
    const auto innerHash = sha256(inner);

    std::string outer;
    outer.reserve(kBlock + innerHash.size());
    for (std::size_t i = 0; i < kBlock; ++i) {
        outer.push_back(static_cast<char>(k0[i] ^ 0x5c));
    }
    outer.append(reinterpret_cast<const char*>(innerHash.data()), innerHash.size());

    return toHex(sha256(outer));
}

bool constantTimeEquals(std::string_view a, std::string_view b) {
    // Fold the length difference into the accumulator so unequal lengths never short-circuit.
    std::uint32_t diff = static_cast<std::uint32_t>(a.size() ^ b.size());
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        diff |= static_cast<std::uint8_t>(a[i]) ^ static_cast<std::uint8_t>(b[i]);
    }
    return diff == 0;
}

} // namespace easyfailover
