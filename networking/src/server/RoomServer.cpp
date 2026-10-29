#include "openaow/net/RoomServer.hpp"

#include <algorithm>
#include <random>
#include <sstream>

namespace OpenArtOfWar::Net {

namespace {

// No 0 / O and 1 / I: codes are read out loud and typed in
constexpr char kCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    return out;
}

} // namespace

struct RoomServer::Room {
    std::string code;
    std::string name;
    std::string gameVersion;
    bool listed{true};
    uint16_t port{0};
    std::unique_ptr<SessionHost> host;
    uint32_t createdMs{0};
    uint32_t lastOccupiedMs{0};
    bool everOccupied{false};
};

RoomServer::RoomServer(RoomServerConfig config) : m_config(std::move(config)), m_buffer(16 * 1024) {}
RoomServer::~RoomServer() = default;

bool RoomServer::start(std::string& error) {
    m_master = createEnetServer(m_config.port, m_config.maxMasterClients, &error, nullptr, m_config.bindAddress);
    if (!m_master) return false;
    m_master->setRawHandler([this](const std::string& ip, uint16_t port, const uint8_t* data, size_t size) {
        handleRaw(ip, port, data, size);
    });
    return true;
}

// A game socket says hello with its token: that is its address as the internet sees it
void RoomServer::handleRaw(const std::string& ip, uint16_t port, const uint8_t* data, size_t size) {
    RawKind kind{};
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    if (!decodeRaw(data, size, kind, payload, payloadSize) || kind != RawKind::Hello || payloadSize != 16) return;
    if (m_sockets.size() > 4096) m_sockets.clear();   // a flood must not grow it without end
    SocketToken token{};
    std::copy(payload, payload + 16, token.begin());
    m_sockets[token] = {ip, port, m_nowMs};
}

bool RoomServer::codeTaken(const std::string& code) const {
    return std::any_of(m_rooms.begin(), m_rooms.end(), [&](const auto& r) { return r->code == code; }) ||
           std::any_of(m_hosted.begin(), m_hosted.end(), [&](const HostedGame& h) { return h.code == code; });
}

void RoomServer::sendMaster(PeerId peer, const BitWriter& w, size_t size) {
    (void)w;
    m_master->send(peer, Channel::Control, m_buffer.data(), size);
}

void RoomServer::reject(PeerId peer, RejectReason reason) {
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, Reject{reason, rejectReasonText(reason)});
    sendMaster(peer, w, w.finish());
}

std::string RoomServer::newCode() const {
    static std::mt19937 rng{std::random_device{}()};
    for (;;) {
        std::string code;
        for (size_t i = 0; i < kRoomCodeLength; ++i) code += kCodeAlphabet[rng() % (sizeof kCodeAlphabet - 1)];
        if (!codeTaken(code)) return code;
    }
}

uint16_t RoomServer::freePort() const {
    for (uint32_t p = m_config.roomPortFirst; p <= m_config.roomPortLast; ++p) {
        if (std::none_of(m_rooms.begin(), m_rooms.end(), [&](const auto& r) { return r->port == p; })) return static_cast<uint16_t>(p);
    }
    return 0;
}

void RoomServer::handleMaster(const TransportEvent& e, uint32_t nowMs) {
    if (e.type == TransportEvent::Type::Connected) {
        ++m_masterClients;
        BitWriter w(m_buffer.data(), m_buffer.size());
        encode(w, ServerInfo{m_config.name, m_config.motd});
        sendMaster(e.peer, w, w.finish());
        return;
    }
    if (e.type == TransportEvent::Type::Disconnected) {
        m_masterClients = std::max(0, m_masterClients - 1);
        // A player-hosted game is listed only while its host keeps the connection
        m_hosted.erase(std::remove_if(m_hosted.begin(), m_hosted.end(), [&](const HostedGame& h) { return h.peer == e.peer; }),
                       m_hosted.end());
        return;
    }
    BitReader r(e.data.data(), e.data.size());
    switch (readType(r)) {
        case MsgType::ListRooms: {
            ListRooms m;
            if (!decode(r, m)) return;
            BitWriter w(m_buffer.data(), m_buffer.size());
            encode(w, RoomList{listing(m.gameVersion)});
            sendMaster(e.peer, w, w.finish());
            break;
        }
        case MsgType::CreateRoom: {
            CreateRoom m;
            if (!decode(r, m)) return;
            const uint16_t port = static_cast<int>(m_rooms.size()) < m_config.maxRooms ? freePort() : 0;
            if (port == 0) {
                reject(e.peer, RejectReason::ServerFull);
                return;
            }
            HostConfig hc;
            hc.identity = GameIdentity{m.gameVersion, m.simBuildId, m.assetsHash};
            hc.roomName = m.name.empty() ? "Room" : m.name;
            hc.ownerControls = true;
            hc.password = m.password;
            hc.turnMs = m_config.turnMs;
            hc.inputDelayMin = m_config.inputDelayMin;
            hc.inputDelayMax = m_config.inputDelayMax;
            hc.maxPlayers = m_config.maxPlayersPerRoom;
            std::string error;
            auto transport = createEnetServer(port, m_config.maxPlayersPerRoom + 4, &error, nullptr, m_config.bindAddress);
            if (!transport) {
                m_log.push_back("cannot open room port " + std::to_string(port) + ": " + error);
                reject(e.peer, RejectReason::ServerFull);
                return;
            }
            auto room = std::make_unique<Room>();
            room->code = newCode();
            room->name = hc.roomName;
            room->gameVersion = m.gameVersion;
            room->listed = m.listed;
            room->port = port;
            room->host = std::make_unique<SessionHost>(hc);
            room->host->addTransport(std::move(transport));
            room->createdMs = nowMs;
            room->lastOccupiedMs = nowMs;
            m_log.push_back("room " + room->code + " '" + room->name + "' opened on port " + std::to_string(port) +
                            " (version " + m.gameVersion + ")");
            BitWriter w(m_buffer.data(), m_buffer.size());
            encode(w, RoomAddress{room->code, port});
            sendMaster(e.peer, w, w.finish());
            m_rooms.push_back(std::move(room));
            break;
        }
        case MsgType::RegisterHost: {
            RegisterHost m;
            if (!decode(r, m)) return;
            auto it = std::find_if(m_hosted.begin(), m_hosted.end(), [&](const HostedGame& h) { return h.peer == e.peer; });
            if (it == m_hosted.end()) {
                if (static_cast<int>(m_hosted.size()) >= m_config.maxRooms * 4) {
                    reject(e.peer, RejectReason::ServerFull);
                    return;
                }
                m_hosted.push_back({newCode(), e.peer, m, nowMs});
                it = m_hosted.end() - 1;
                m_log.push_back("player-hosted game " + it->code + " '" + m.name + "' listed");
            }
            it->info = m;
            it->lastSeenMs = nowMs;
            BitWriter w(m_buffer.data(), m_buffer.size());
            encode(w, HostRegistered{it->code});
            sendMaster(e.peer, w, w.finish());
            break;
        }
        case MsgType::JoinHosted: {
            JoinHosted m;
            if (!decode(r, m)) return;
            std::string code = m.code;
            std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            auto host = std::find_if(m_hosted.begin(), m_hosted.end(), [&](const HostedGame& h) { return h.code == code; });
            if (host == m_hosted.end()) {
                reject(e.peer, RejectReason::NotFound);
                return;
            }
            auto hostSocket = m_sockets.find(host->info.token);
            auto clientSocket = m_sockets.find(m.token);
            if (hostSocket == m_sockets.end() || clientSocket == m_sockets.end()) {
                reject(e.peer, RejectReason::TryAgain);   // the sockets' hellos are still on their way
                return;
            }
            // Introduce them: the host punches towards the player, the player gets the host's addresses
            BitWriter w(m_buffer.data(), m_buffer.size());
            encode(w, PunchRequest{clientSocket->second.ip, clientSocket->second.port});
            sendMaster(host->peer, w, w.finish());
            BitWriter w2(m_buffer.data(), m_buffer.size());
            encode(w2, HostedEndpoint{hostSocket->second.ip, hostSocket->second.port, host->info.lanIp, host->info.lanPort,
                                      clientSocket->second.ip});
            sendMaster(e.peer, w2, w2.finish());
            break;
        }
        case MsgType::FindRoom: {
            FindRoom m;
            if (!decode(r, m)) return;
            std::string code = m.code;
            std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            auto it = std::find_if(m_rooms.begin(), m_rooms.end(), [&](const auto& room) { return room->code == code; });
            if (it == m_rooms.end()) {
                // A game hosted by a player: port 0 tells the client to go through JoinHosted
                if (std::any_of(m_hosted.begin(), m_hosted.end(), [&](const HostedGame& h) { return h.code == code; })) {
                    BitWriter w(m_buffer.data(), m_buffer.size());
                    encode(w, RoomAddress{code, 0});
                    sendMaster(e.peer, w, w.finish());
                    return;
                }
                reject(e.peer, RejectReason::NotFound);
                return;
            }
            BitWriter w(m_buffer.data(), m_buffer.size());
            encode(w, RoomAddress{(*it)->code, (*it)->port});
            sendMaster(e.peer, w, w.finish());
            break;
        }
        default:
            break;
    }
}

void RoomServer::update(uint32_t nowMs) {
    if (!m_master) return;
    m_nowMs = nowMs;
    m_events.clear();
    m_master->poll(m_events);
    for (const TransportEvent& e : m_events) handleMaster(e, nowMs);
    for (auto& room : m_rooms) {
        room->host->update(nowMs);
        if (room->host->connectedCount() > 0) {
            room->lastOccupiedMs = nowMs;
            room->everOccupied = true;
        }
    }
    // Player-hosted games that stopped telling about themselves, socket hellos that are old
    m_hosted.erase(std::remove_if(m_hosted.begin(), m_hosted.end(), [&](const HostedGame& h) { return nowMs - h.lastSeenMs > 30000; }),
                   m_hosted.end());
    for (auto it = m_sockets.begin(); it != m_sockets.end();) {
        it = nowMs - it->second.atMs > 60000 ? m_sockets.erase(it) : std::next(it);
    }
    // Rooms nobody is in close after a while (sooner when nobody ever came)
    m_rooms.erase(std::remove_if(m_rooms.begin(), m_rooms.end(), [&](const auto& room) {
                      const uint32_t idle = nowMs - room->lastOccupiedMs;
                      const uint32_t limit = !room->everOccupied ? m_config.unusedRoomTimeoutMs
                                           : (room->host->matchRunning() ? m_config.idleRoomTimeoutMs : m_config.emptyRoomTimeoutMs);
                      const bool close = idle > limit;
                      if (close) m_log.push_back("room " + room->code + " closed (empty)");
                      return close;
                  }),
                  m_rooms.end());
}

std::vector<RoomListing> RoomServer::listing(const std::string& gameVersion) const {
    std::vector<RoomListing> out;
    for (const auto& room : m_rooms) {
        if (!room->listed || (!gameVersion.empty() && room->gameVersion != gameVersion)) continue;
        const RoomInfo info = room->host->roomInfo();
        out.push_back({room->code, room->name, info.map, info.players, info.maxPlayers, info.running, room->port, false, info.locked});
        if (out.size() >= kMaxListedRooms) break;
    }
    for (const HostedGame& h : m_hosted) {
        if (out.size() >= kMaxListedRooms) break;
        if (!h.info.listed || (!gameVersion.empty() && h.info.gameVersion != gameVersion)) continue;
        out.push_back({h.code, h.info.name, h.info.map, h.info.players, h.info.maxPlayers, false, 0, true});
    }
    return out;
}

RoomServer::Stats RoomServer::stats() const {
    Stats s;
    s.rooms = static_cast<int>(m_rooms.size());
    s.masterClients = m_masterClients;
    for (const auto& room : m_rooms) {
        s.players += room->host->connectedCount();
        s.matches += room->host->matchRunning() ? 1 : 0;
    }
    return s;
}

std::string RoomServer::statusJson() const {
    const Stats s = stats();
    std::ostringstream o;
    o << "{\"name\":\"" << jsonEscape(m_config.name) << "\",\"rooms\":" << s.rooms << ",\"players\":" << s.players
      << ",\"matches\":" << s.matches << ",\"master_clients\":" << s.masterClients << ",\"list\":[";
    bool first = true;
    for (const auto& room : m_rooms) {
        const RoomInfo info = room->host->roomInfo();
        o << (first ? "" : ",") << "{\"code\":\"" << room->code << "\",\"name\":\"" << jsonEscape(room->name)
          << "\",\"version\":\"" << jsonEscape(room->gameVersion) << "\",\"map\":\"" << jsonEscape(info.map)
          << "\",\"players\":" << int(info.players) << ",\"running\":" << (info.running ? "true" : "false")
          << ",\"port\":" << room->port << ",\"listed\":" << (room->listed ? "true" : "false") << "}";
        first = false;
    }
    o << "]}";
    return o.str();
}

std::vector<std::string> RoomServer::takeLog() {
    std::vector<std::string> out;
    out.swap(m_log);
    return out;
}

} // namespace OpenArtOfWar::Net
