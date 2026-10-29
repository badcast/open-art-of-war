#include "openaow/net/Discovery.hpp"
#include "openaow/net/BitStream.hpp"
#include "openaow/net/Net.hpp"
#include "transport/EnetSupport.hpp"

#include <algorithm>
#include <cstring>
#include <map>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace OpenArtOfWar::Net {

namespace {

constexpr char kProbeMagic[8] = {'O', 'A', 'O', 'W', 'D', 'I', 'S', 'C'};
constexpr char kAnswerMagic[8] = {'O', 'A', 'O', 'W', 'R', 'O', 'O', 'M'};
constexpr uint8_t kDiscoveryVersion = 2;
constexpr size_t kMaxLength = 64;

bool hasMagic(const uint8_t* data, size_t size, const char (&magic)[8]) {
    return size >= 8 && std::memcmp(data, magic, 8) == 0;
}

std::string dotted(enet_uint32 host) {
    ENetAddress a{};
    a.host = host;
    char text[32] = {};
    enet_address_get_host_ip(&a, text, sizeof text);
    return text;
}

} // namespace

std::vector<uint8_t> encodeProbe(uint32_t nonce) {
    std::vector<uint8_t> buf(16);
    std::memcpy(buf.data(), kProbeMagic, 8);
    BitWriter w(buf.data() + 8, buf.size() - 8);
    w.writeU8(kDiscoveryVersion);
    w.writeU32(nonce);
    buf.resize(8 + w.finish());
    return buf;
}

bool decodeProbe(const uint8_t* data, size_t size, uint32_t& nonce) {
    if (!hasMagic(data, size, kProbeMagic)) return false;
    BitReader r(data + 8, size - 8);
    if (r.readU8() != kDiscoveryVersion) return false;
    nonce = r.readU32();
    return !r.failed();
}

std::vector<uint8_t> encodeAnswer(uint32_t nonce, uint16_t port, const RoomInfo& info) {
    std::vector<uint8_t> buf(512);
    std::memcpy(buf.data(), kAnswerMagic, 8);
    BitWriter w(buf.data() + 8, buf.size() - 8);
    w.writeU8(kDiscoveryVersion);
    w.writeU32(nonce);
    w.writeU16(port);
    w.writeString(info.gameVersion, kMaxLength);
    w.writeString(info.name, kMaxLength);
    w.writeString(info.map, kMaxLength * 2);
    w.writeU8(info.players);
    w.writeU8(info.maxPlayers);
    w.writeBool(info.running);
    w.writeBool(info.locked);
    buf.resize(8 + w.finish());
    return buf;
}

bool decodeAnswer(const uint8_t* data, size_t size, uint32_t& nonce, uint16_t& port, RoomInfo& info) {
    if (!hasMagic(data, size, kAnswerMagic)) return false;
    BitReader r(data + 8, size - 8);
    if (r.readU8() != kDiscoveryVersion) return false;
    nonce = r.readU32();
    port = r.readU16();
    info.gameVersion = r.readString(kMaxLength);
    info.name = r.readString(kMaxLength);
    info.map = r.readString(kMaxLength * 2);
    info.players = r.readU8();
    info.maxPlayers = r.readU8();
    info.running = r.readBool();
    info.locked = r.readBool();
    return !r.failed();
}

std::string localLanIp() {
#if !defined(_WIN32)
    std::string found;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* i = list; i && found.empty(); i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
            found = dotted(reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
        }
        freeifaddrs(list);
    }
    return found;
#else
    Session session;
    char name[256] = {};
    if (gethostname(name, sizeof name) != 0) return {};
    ENetAddress a{};
    if (enet_address_set_host(&a, name) != 0) return {};
    return dotted(a.host);
#endif
}

struct LanBrowser::Impl {
    Session session;
    ENetSocket socket{ENET_SOCKET_NULL};
    uint16_t basePort{0};
    uint32_t nonce{0};
    uint32_t lastProbeMs{0};
    bool probedOnce{false};
    std::map<uint32_t, uint32_t> sentAt;   // nonce -> time

    // Where the probes go: the limited broadcast, every IPv4 interface's own broadcast address and
    // this computer itself (games hosted here)
    std::vector<enet_uint32> targets() const {
        std::vector<enet_uint32> out{ENET_HOST_BROADCAST, htonl(INADDR_LOOPBACK)};
#if !defined(_WIN32)
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) == 0) {
            for (ifaddrs* i = list; i; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_BROADCAST) || !i->ifa_broadaddr) continue;
                const enet_uint32 b = reinterpret_cast<const sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr;
                if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
            }
            freeifaddrs(list);
        }
#endif
        return out;
    }
};

LanBrowser::LanBrowser(uint16_t basePort) : m_impl(std::make_unique<Impl>()) {
    m_impl->basePort = basePort;
    if (!m_impl->session.ok()) return;
    m_impl->socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (m_impl->socket == ENET_SOCKET_NULL) return;
    enet_socket_set_option(m_impl->socket, ENET_SOCKOPT_NONBLOCK, 1);
    enet_socket_set_option(m_impl->socket, ENET_SOCKOPT_BROADCAST, 1);
    ENetAddress any{};
    any.host = ENET_HOST_ANY;
    any.port = 0;
    enet_socket_bind(m_impl->socket, &any);
}

LanBrowser::~LanBrowser() {
    if (m_impl && m_impl->socket != ENET_SOCKET_NULL) enet_socket_destroy(m_impl->socket);
}

bool LanBrowser::ok() const { return m_impl->socket != ENET_SOCKET_NULL; }

void LanBrowser::update(uint32_t nowMs) {
    if (!ok()) return;
    Impl& d = *m_impl;
    if (!d.probedOnce || nowMs - d.lastProbeMs >= 1000) {
        d.probedOnce = true;
        d.lastProbeMs = nowMs;
        const uint32_t nonce = ++d.nonce;
        d.sentAt[nonce] = nowMs;
        while (d.sentAt.size() > 8) d.sentAt.erase(d.sentAt.begin());
        const std::vector<uint8_t> probe = encodeProbe(nonce);
        ENetBuffer buffer;
        buffer.data = const_cast<uint8_t*>(probe.data());
        buffer.dataLength = probe.size();
        for (enet_uint32 host : d.targets()) {
            for (uint16_t p = 0; p < kDiscoveryPortSpan; ++p) {
                ENetAddress to{};
                to.host = host;
                to.port = static_cast<enet_uint16>(d.basePort + p);
                enet_socket_send(d.socket, &to, &buffer, 1);
            }
        }
    }
    uint8_t data[600];
    for (int guard = 0; guard < 64; ++guard) {
        ENetAddress from{};
        ENetBuffer buffer;
        buffer.data = data;
        buffer.dataLength = sizeof data;
        const int got = enet_socket_receive(d.socket, &from, &buffer, 1);
        if (got <= 0) break;
        uint32_t nonce = 0;
        uint16_t port = 0;
        RoomInfo info;
        if (!decodeAnswer(data, static_cast<size_t>(got), nonce, port, info)) continue;
        const std::string address = dotted(from.host);
        auto it = std::find_if(m_rooms.begin(), m_rooms.end(),
                               [&](const FoundRoom& r) { return r.port == port && (r.address == address || r.info.name == info.name); });
        if (it == m_rooms.end()) {
            m_rooms.push_back({address, port, info, 0, nowMs});
            it = m_rooms.end() - 1;
        }
        // The same game over loopback and the network: keep the network address once known
        if (address != "127.0.0.1") it->address = address;
        it->info = info;
        it->lastSeenMs = nowMs;
        auto sent = d.sentAt.find(nonce);
        if (sent != d.sentAt.end()) it->pingMs = nowMs - sent->second;
    }
    m_rooms.erase(std::remove_if(m_rooms.begin(), m_rooms.end(), [&](const FoundRoom& r) { return nowMs - r.lastSeenMs > 3000; }),
                  m_rooms.end());
}

} // namespace OpenArtOfWar::Net
