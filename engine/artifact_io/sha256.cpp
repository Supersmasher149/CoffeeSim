#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "espressolab/artifact_io.hpp"

// Section 10.3: SHA-256 is a reproducibility signal, not a security boundary.
// Implemented here so a clean clone needs no crypto dependency.
namespace espressolab::artifact_io {
namespace {

constexpr std::array<std::uint32_t, 64> kK{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void transform(std::array<std::uint32_t, 8>& h, const unsigned char* block) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
        w[static_cast<std::size_t>(i)] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                                         (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                                         (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                                         static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::array<std::uint32_t, 8> v = h;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
        const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        const std::uint32_t temp1 = v[7] + S1 + ch + kK[i] + w[i];
        const std::uint32_t S0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
        const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        const std::uint32_t temp2 = S0 + maj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + temp1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = temp1 + temp2;
    }
    for (std::size_t i = 0; i < 8; ++i) h[i] += v[i];
}

}  // namespace

std::string sha256_hex(const std::string& bytes) {
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    // `bytes` can be close to 1 GiB (a CFD3D field/result payload). Transform
    // full blocks straight out of it rather than copying the whole thing into
    // a padded buffer first: the message tail (< 64 bytes) plus the 0x80
    // terminator, zero padding, and the 8-byte bit length always fit in two
    // 64-byte blocks, built here without ever duplicating the input.
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t size = bytes.size();
    const std::uint64_t bit_length = static_cast<std::uint64_t>(size) * 8;

    std::size_t offset = 0;
    for (; offset + 64 <= size; offset += 64) {
        transform(h, data + offset);
    }

    std::array<unsigned char, 128> tail{};
    const std::size_t tail_len = size - offset;
    std::memcpy(tail.data(), data + offset, tail_len);
    std::size_t padded_len = tail_len;
    tail[padded_len++] = 0x80;
    while (padded_len % 64 != 56) tail[padded_len++] = 0;
    for (int i = 7; i >= 0; --i) {
        tail[padded_len++] = static_cast<unsigned char>((bit_length >> (i * 8)) & 0xff);
    }

    for (std::size_t block_offset = 0; block_offset < padded_len; block_offset += 64) {
        transform(h, tail.data() + block_offset);
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint32_t word : h) {
        for (int i = 7; i >= 0; --i) {
            out.push_back(kHex[(word >> (i * 4)) & 0xf]);
        }
    }
    return out;
}

}  // namespace espressolab::artifact_io
