#pragma once

// Internal: ENet is visible only inside the networking library
#include <enet/enet.h>

namespace OpenArtOfWar::Net {

// ENet checksum hook: CRC-32 over the datagram (both ends must use the same one)
enet_uint32 enetChecksum(const ENetBuffer* buffers, size_t bufferCount);

// What every host of the game sets up: CRC-32 checksums, range-coder compression
void configureHost(ENetHost* host);

} // namespace OpenArtOfWar::Net
