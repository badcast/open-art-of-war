#pragma once

#include "openaow/net/MasterClient.hpp"
#include "openaow/net/Transport.hpp"

#include <memory>
#include <string>

// Games hosted by players, reachable without forwarded ports: the server (openaow-server's master port)
// lists them and introduces host and player, both punch a hole through their NAT from their game
// sockets, then the player connects as usual. Works for the common home routers (address-preserving
// NAT); behind a strict (symmetric) NAT a room on the server is the way.
namespace OpenArtOfWar::Net {

// The host's side: lists the game on the server, keeps its socket's public address known (and the
// router's mapping open) and punches towards the players the server announces
class HostAnnouncer {
public:
    // `gameSocket`: the host's listening transport (it must outlive the announcer)
    HostAnnouncer(const std::string& master, uint16_t masterPort, Transport& gameSocket, uint16_t gamePort,
                  const std::string& lanIp);
    void update(uint32_t nowMs, const RoomInfo& room, bool listed);
    const std::string& code() const { return m_master.hostedCode(); }
    bool failed() const { return m_master.state() == MasterClient::State::Failed; }
    const std::string& error() const { return m_master.error(); }

private:
    MasterClient m_master;
    Transport& m_socket;
    std::string m_host;
    uint16_t m_port;
    uint16_t m_gamePort;
    std::string m_lanIp;
    SocketToken m_token{};
    uint32_t m_nextHelloMs{0};
    uint32_t m_nextRegisterMs{0};
    int m_hellos{0};
};

// The player's side: finds the hosted game by its code, punches, connects; then the transport goes to a
// SessionClient
class HostedJoin {
public:
    enum class State { Working, Ready, Failed };

    HostedJoin(const std::string& master, uint16_t masterPort, const std::string& code);
    void update(uint32_t nowMs);
    State state() const { return m_state; }
    const std::string& error() const { return m_error; }
    // Once Ready: the game socket, connecting to the host
    std::unique_ptr<Transport> takeTransport() { return std::move(m_socket); }

private:
    MasterClient m_master;
    std::unique_ptr<Transport> m_socket;
    std::string m_host;
    uint16_t m_port;
    std::string m_code;
    SocketToken m_token{};
    State m_state{State::Working};
    std::string m_error;
    uint32_t m_startMs{0};
    uint32_t m_nextHelloMs{0};
    uint32_t m_nextAskMs{0};
    bool m_started{false};
};

} // namespace OpenArtOfWar::Net
