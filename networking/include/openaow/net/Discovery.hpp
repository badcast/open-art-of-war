#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Finding games on the local network. A browser broadcasts small probes on the game ports; every host
// answers from its game socket (no extra port) with what its room is about. Probes and answers start
// with their own magic, so they never mix with ENet's traffic on the same socket.
namespace OpenArtOfWar::Net {

constexpr uint16_t kDiscoveryPortSpan = 4;   // hosts on one computer take 38100, 38101, ...

// What a host tells about its room
struct RoomInfo {
    std::string gameVersion;
    std::string name;
    std::string map;
    uint8_t players{0};
    uint8_t maxPlayers{0};
    bool running{false};
    bool locked{false};   // asks for a password
};

struct FoundRoom {
    std::string address;   // dotted IPv4
    uint16_t port{0};
    RoomInfo info;
    uint32_t pingMs{0};
    uint32_t lastSeenMs{0};
};

// Encoded probe / answer (also used by the host's socket hook)
std::vector<uint8_t> encodeProbe(uint32_t nonce);
bool decodeProbe(const uint8_t* data, size_t size, uint32_t& nonce);
std::vector<uint8_t> encodeAnswer(uint32_t nonce, uint16_t port, const RoomInfo& info);
bool decodeAnswer(const uint8_t* data, size_t size, uint32_t& nonce, uint16_t& port, RoomInfo& info);

// This computer's address in its local network (the first IPv4 interface that is up and not
// loopback); empty when there is none
std::string localLanIp();

class LanBrowser {
public:
    // `basePort`: the first game port (38100); probes go to it and the next kDiscoveryPortSpan - 1
    explicit LanBrowser(uint16_t basePort);
    ~LanBrowser();
    LanBrowser(const LanBrowser&) = delete;
    LanBrowser& operator=(const LanBrowser&) = delete;

    bool ok() const;
    // Probes once a second, collects answers; rooms not heard of for 3 s drop out
    void update(uint32_t nowMs);
    const std::vector<FoundRoom>& rooms() const { return m_rooms; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::vector<FoundRoom> m_rooms;
};

} // namespace OpenArtOfWar::Net
