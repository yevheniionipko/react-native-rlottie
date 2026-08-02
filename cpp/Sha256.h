// Chunk 5.3 — a minimal, self-contained SHA-256 (FIPS 180-4).
//
// Used only to build the deterministic model-cache key natively from the raw
// JSON bytes about to be parsed (CacheKey.h). Not a general crypto utility:
// no streaming API, not constant-time, no HMAC — none of that is needed for a
// cache key derivation, and adding it would be unjustified complexity here.
// Vendoring a few dozen lines avoids pulling in a third-party crypto library
// for one hash.
//
// Header-only (inline): both android/src/main/cpp/CMakeLists.txt and
// tests/cpp/CMakeLists.txt enumerate cpp/*.cpp explicitly, and adding a new
// .cpp to the Android list is out of scope here (owned by another agent) —
// see the same note in InputLimits.h.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace rnrlottie {
namespace detail {

inline std::uint32_t sha256Rotr(std::uint32_t x, std::uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline const std::array<std::uint32_t, 64>& sha256K() {
    static const std::array<std::uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    return k;
}

}  // namespace detail

// Lowercase 64-hex-character digest of `data`. Reference (non-SIMD,
// non-unrolled) implementation — this runs once per load, not per frame.
inline std::string sha256Hex(const std::string& data) {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    // Pad: 0x80, zeros, then the 64-bit big-endian bit length, to a multiple
    // of 64 bytes.
    std::string msg = data;
    const std::uint64_t bitLen = static_cast<std::uint64_t>(data.size()) * 8u;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) {
        msg.push_back(static_cast<char>(0x00));
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xff));
    }

    const auto& k = detail::sha256K();
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + chunk + i * 4);
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) |
                   (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = detail::sha256Rotr(w[i - 15], 7) ^
                                     detail::sha256Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = detail::sha256Rotr(w[i - 2], 17) ^
                                     detail::sha256Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 =
                detail::sha256Rotr(e, 6) ^ detail::sha256Rotr(e, 11) ^ detail::sha256Rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 =
                detail::sha256Rotr(a, 2) ^ detail::sha256Rotr(a, 13) ^ detail::sha256Rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;

            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint32_t word : h) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            const unsigned char byte = static_cast<unsigned char>((word >> shift) & 0xff);
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0f]);
        }
    }
    return out;
}

}  // namespace rnrlottie
