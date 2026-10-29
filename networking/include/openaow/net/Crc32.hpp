#pragma once

#include <cstddef>
#include <cstdint>

namespace OpenArtOfWar::Net {

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), slice-by-8.
// Incremental: crc32(b, n2, crc32(a, n1)) == crc32(a + b, n1 + n2).
std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t previous = 0);

} // namespace OpenArtOfWar::Net
