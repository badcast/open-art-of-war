#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace OpenArtOfWar
{

// SHA-256 (FIPS 180-4): feed data with Update, read the digest once with Final
class Sha256
{
public:
    Sha256();
    void Update(const void *data, std::size_t size);
    std::array<std::uint8_t, 32> Final();

    // Lower-case hex digest of a whole buffer
    static std::string Hex(const void *data, std::size_t size);

private:
    void Block(const std::uint8_t *p);

    std::uint32_t m_state[8];
    std::uint8_t m_buffer[64];
    std::size_t m_used {0};
    std::uint64_t m_bits {0};
};

} // namespace OpenArtOfWar
