#include "transport/EnetSupport.hpp"
#include "openaow/net/Crc32.hpp"
#include "openaow/net/Net.hpp"

#include <mutex>

namespace OpenArtOfWar::Net {

namespace {

std::mutex g_lock;
int g_users = 0;

} // namespace

bool initialize() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_users == 0 && enet_initialize() != 0) return false;
    ++g_users;
    return true;
}

void shutdown() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_users == 0) return;
    if (--g_users == 0) enet_deinitialize();
}

enet_uint32 enetChecksum(const ENetBuffer* buffers, size_t bufferCount) {
    std::uint32_t crc = 0;
    for (size_t i = 0; i < bufferCount; ++i) crc = crc32(buffers[i].data, buffers[i].dataLength, crc);
    return crc;
}

void configureHost(ENetHost* host) {
    host->checksum = enetChecksum;              // every datagram checked end to end
    enet_host_compress_with_range_coder(host);  // command traffic is small and repetitive
}

} // namespace OpenArtOfWar::Net
