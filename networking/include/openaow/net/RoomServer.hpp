#pragma once

#include "openaow/net/Protocol.hpp"
#include "openaow/net/Session.hpp"
#include "openaow/net/Transport.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The dedicated server (openaow-server): a master port where players list rooms, create one or find
// one by its code, and dedicated rooms, each a SessionHost on a port of its own. Players connect to
// the server only (outgoing connections), so no router needs a forwarded port. The server runs no
// simulation: it closes the turns, relays the commands and compares the checksums.
namespace OpenArtOfWar::Net {

struct RoomServerConfig {
    std::string bindAddress;              // empty: every interface
    uint16_t port{kDefaultPort};          // master port
    uint16_t roomPortFirst{38110};
    uint16_t roomPortLast{38199};
    int maxRooms{64};
    int maxPlayersPerRoom{kMaxPlayers};
    int maxMasterClients{512};
    uint16_t turnMs{100};
    uint8_t inputDelayMin{1};
    uint8_t inputDelayMax{6};
    uint32_t idleRoomTimeoutMs{120000};   // a room with a running match that nobody is in closes after this (time to reconnect)
    uint32_t emptyRoomTimeoutMs{30000};   // a room that is only a lobby closes this long after the last player left
    uint32_t unusedRoomTimeoutMs{120000}; // ... and one nobody ever entered after this
    std::string name{"OpenAOW server"};
    std::string motd;
};

class RoomServer {
public:
    explicit RoomServer(RoomServerConfig config);
    ~RoomServer();

    bool start(std::string& error);
    void update(uint32_t nowMs);

    struct Stats {
        int rooms{0};
        int players{0};
        int matches{0};
        int masterClients{0};
    };
    Stats stats() const;
    std::vector<RoomListing> listing(const std::string& gameVersion) const;
    // Rooms and numbers as JSON (the status page)
    std::string statusJson() const;
    // What happened since the last call (rooms opened / closed), for the log
    std::vector<std::string> takeLog();

private:
    struct Room;
    // A game hosted by a player that the server only lists and introduces (NAT hole punching)
    struct HostedGame {
        std::string code;
        PeerId peer{0};
        RegisterHost info;
        uint32_t lastSeenMs{0};
    };
    struct SocketSeen {
        std::string ip;
        uint16_t port{0};
        uint32_t atMs{0};
    };
    void handleMaster(const TransportEvent& e, uint32_t nowMs);
    void handleRaw(const std::string& ip, uint16_t port, const uint8_t* data, size_t size);
    bool codeTaken(const std::string& code) const;
    void sendMaster(PeerId peer, const BitWriter& w, size_t size);
    void reject(PeerId peer, RejectReason reason);
    std::string newCode() const;
    uint16_t freePort() const;

    RoomServerConfig m_config;
    std::unique_ptr<Transport> m_master;
    std::vector<std::unique_ptr<Room>> m_rooms;
    std::vector<HostedGame> m_hosted;
    std::map<SocketToken, SocketSeen> m_sockets;   // public address of game sockets that said hello
    uint32_t m_nowMs{0};
    std::vector<TransportEvent> m_events;
    std::vector<uint8_t> m_buffer;
    std::vector<std::string> m_log;
    int m_masterClients{0};
};

} // namespace OpenArtOfWar::Net
