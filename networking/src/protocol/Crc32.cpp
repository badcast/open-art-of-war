#include "openaow/net/Crc32.hpp"

#include <cstring>

namespace OpenArtOfWar::Net {

namespace {

struct Tables {
    std::uint32_t t[8][256];

    Tables() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
            t[0][i] = c;
        }
        for (std::uint32_t i = 0; i < 256; ++i) {
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
        }
    }
};

const Tables& tables() {
    static const Tables kTables;
    return kTables;
}

} // namespace

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t previous) {
    const auto& t = tables().t;
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t c = ~previous;
    // Eight bytes per step; the words are read byte by byte, so the result does not depend on endianness
    while (size >= 8) {
        const std::uint32_t lo = c ^ (static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
                                      static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24);
        c = t[7][lo & 0xFFu] ^ t[6][(lo >> 8) & 0xFFu] ^ t[5][(lo >> 16) & 0xFFu] ^ t[4][lo >> 24] ^
            t[3][p[4]] ^ t[2][p[5]] ^ t[1][p[6]] ^ t[0][p[7]];
        p += 8;
        size -= 8;
    }
    while (size--) c = t[0][(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return ~c;
}

} // namespace OpenArtOfWar::Net
