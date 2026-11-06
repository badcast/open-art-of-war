#include "core/Sha256.hpp"

#include <cstring>

namespace OpenArtOfWar
{

namespace
{
constexpr std::uint32_t kRound[64]
    = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
       0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
       0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
       0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
       0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
       0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
       0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
       0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t Rotr(std::uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}
} // namespace

Sha256::Sha256()
    : m_state {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}
{}

void Sha256::Block(const std::uint8_t *p)
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) | (std::uint32_t(p[i * 4 + 2]) << 8)
               | std::uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i)
    {
        const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
    for (int i = 0; i < 64; ++i)
    {
        const std::uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const std::uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::Update(const void *data, std::size_t size)
{
    const auto *p = static_cast<const std::uint8_t *>(data);
    m_bits += static_cast<std::uint64_t>(size) * 8;
    if (m_used > 0)
    {
        const std::size_t take = size < 64 - m_used ? size : 64 - m_used;
        std::memcpy(m_buffer + m_used, p, take);
        m_used += take;
        p += take;
        size -= take;
        if (m_used < 64)
            return;
        Block(m_buffer);
        m_used = 0;
    }
    for (; size >= 64; p += 64, size -= 64)
        Block(p);
    std::memcpy(m_buffer, p, size);
    m_used = size;
}

std::array<std::uint8_t, 32> Sha256::Final()
{
    const std::uint64_t bits = m_bits;
    const std::uint8_t pad = 0x80;
    Update(&pad, 1);
    const std::uint8_t zero = 0;
    while (m_used != 56)
        Update(&zero, 1);
    std::uint8_t length[8];
    for (int i = 0; i < 8; ++i)
        length[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    Update(length, 8);
    std::array<std::uint8_t, 32> out {};
    for (int i = 0; i < 8; ++i)
    {
        out[i * 4] = static_cast<std::uint8_t>(m_state[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(m_state[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(m_state[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(m_state[i]);
    }
    return out;
}

std::string Sha256::Hex(const void *data, std::size_t size)
{
    Sha256 sha;
    sha.Update(data, size);
    const std::array<std::uint8_t, 32> digest = sha.Final();
    static const char *digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (std::uint8_t b : digest)
    {
        hex.push_back(digits[b >> 4]);
        hex.push_back(digits[b & 15]);
    }
    return hex;
}

} // namespace OpenArtOfWar
