#pragma once

#include "openaow/net/Protocol.hpp"
#include "openaow/net/Session.hpp"
#include "openaow/net/Transport.hpp"

#include <memory>
#include <string>
#include <vector>

namespace OpenArtOfWar::Net {

// Talks to the master port of an openaow-server: the public room list, a new room, a room by its code.
// Asynchronous: issue a request, call update() every frame, read the result when it is there.
class MasterClient {
public:
    enum class State { Connecting, Ready, Failed };

    MasterClient(const std::string& host, uint16_t port);

    void update(uint32_t nowMs);
    State state() const { return m_state; }
    const std::string& error() const { return m_error; }
    const ServerInfo& serverInfo() const { return m_info; }

    // Requests (sent once connected)
    void requestList(const std::string& gameVersion);
    void createRoom(const GameIdentity& identity, const std::string& name, bool listed, const std::string& password = {});
    void findRoom(const std::string& code);

    // A game hosted by a player: list it (send again every few seconds) / ask for a hosted game's address
    void registerHost(const RegisterHost& info);
    void joinHosted(const std::string& code, const SocketToken& token);

    // Results
    bool hasList() const { return m_hasList; }
    // The code the server gave the hosted game ("" until then)
    const std::string& hostedCode() const { return m_hostedCode; }
    bool takePunchRequest(PunchRequest& out);
    bool takeHostedEndpoint(HostedEndpoint& out);
    const std::vector<RoomListing>& rooms() const { return m_rooms; }
    // A room to connect to (after createRoom / findRoom); takeRoom clears it
    bool takeRoom(RoomAddress& out);
    // The last request was refused (no such room, server full)
    bool takeRefusal(RejectReason& out);

private:
    void flush();

    std::unique_ptr<Transport> m_transport;
    State m_state{State::Connecting};
    std::string m_error;
    ServerInfo m_info;
    std::vector<std::vector<uint8_t>> m_queue;   // requests waiting for the connection
    bool m_hasList{false};
    std::vector<RoomListing> m_rooms;
    bool m_hasRoom{false};
    RoomAddress m_room;
    bool m_refused{false};
    std::string m_hostedCode;
    std::vector<PunchRequest> m_punches;
    bool m_hasEndpoint{false};
    HostedEndpoint m_endpoint;
    RejectReason m_refusal{RejectReason::None};
    uint32_t m_startMs{0};
    bool m_started{false};
    std::vector<TransportEvent> m_events;
};

} // namespace OpenArtOfWar::Net
