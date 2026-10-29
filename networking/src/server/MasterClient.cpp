#include "openaow/net/MasterClient.hpp"

namespace OpenArtOfWar::Net {

namespace {
constexpr uint32_t kConnectTimeoutMs = 6000;
} // namespace

MasterClient::MasterClient(const std::string& host, uint16_t port) {
    m_transport = createEnetClient(host, port, &m_error);
    if (!m_transport) m_state = State::Failed;
}

void MasterClient::flush() {
    if (m_state != State::Ready) return;
    for (const auto& bytes : m_queue) m_transport->send(0, Channel::Control, bytes.data(), bytes.size());
    m_queue.clear();
}

template <typename T>
static std::vector<uint8_t> encodeMessage(const T& m) {
    std::vector<uint8_t> buf(1024);
    BitWriter w(buf.data(), buf.size());
    encode(w, m);
    buf.resize(w.finish());
    return buf;
}

void MasterClient::requestList(const std::string& gameVersion) {
    m_queue.push_back(encodeMessage(ListRooms{gameVersion}));
    flush();
}

void MasterClient::createRoom(const GameIdentity& identity, const std::string& name, bool listed, const std::string& password) {
    m_queue.push_back(encodeMessage(CreateRoom{identity.gameVersion, identity.simBuildId, identity.assetsHash, name, listed, password}));
    flush();
}

void MasterClient::registerHost(const RegisterHost& info) {
    m_queue.push_back(encodeMessage(info));
    flush();
}

void MasterClient::joinHosted(const std::string& code, const SocketToken& token) {
    m_queue.push_back(encodeMessage(JoinHosted{code, token}));
    flush();
}

bool MasterClient::takePunchRequest(PunchRequest& out) {
    if (m_punches.empty()) return false;
    out = m_punches.front();
    m_punches.erase(m_punches.begin());
    return true;
}

bool MasterClient::takeHostedEndpoint(HostedEndpoint& out) {
    if (!m_hasEndpoint) return false;
    out = m_endpoint;
    m_hasEndpoint = false;
    return true;
}

void MasterClient::findRoom(const std::string& code) {
    m_queue.push_back(encodeMessage(FindRoom{code}));
    flush();
}

void MasterClient::update(uint32_t nowMs) {
    if (!m_transport || m_state == State::Failed) return;
    if (!m_started) {
        m_started = true;
        m_startMs = nowMs;
    }
    m_events.clear();
    m_transport->poll(m_events);
    for (const TransportEvent& e : m_events) {
        if (e.type == TransportEvent::Type::Connected) {
            m_state = State::Ready;
            flush();
            continue;
        }
        if (e.type == TransportEvent::Type::Disconnected) {
            m_state = State::Failed;
            m_error = "the server closed the connection";
            continue;
        }
        BitReader r(e.data.data(), e.data.size());
        switch (readType(r)) {
            case MsgType::ServerInfo: decode(r, m_info); break;
            case MsgType::RoomList: {
                RoomList list;
                if (decode(r, list)) {
                    m_rooms = std::move(list.rooms);
                    m_hasList = true;
                }
                break;
            }
            case MsgType::RoomAddress:
                if (decode(r, m_room)) m_hasRoom = true;
                break;
            case MsgType::HostRegistered: {
                HostRegistered m;
                if (decode(r, m)) m_hostedCode = m.code;
                break;
            }
            case MsgType::PunchRequest: {
                PunchRequest m;
                if (decode(r, m)) m_punches.push_back(m);
                break;
            }
            case MsgType::HostedEndpoint:
                if (decode(r, m_endpoint)) m_hasEndpoint = true;
                break;
            case MsgType::Reject: {
                Reject m;
                if (decode(r, m)) {
                    m_refused = true;
                    m_refusal = m.reason;
                }
                break;
            }
            default:
                break;
        }
    }
    if (m_state == State::Connecting && nowMs - m_startMs > kConnectTimeoutMs) {
        m_state = State::Failed;
        m_error = "the server does not answer";
    }
}

bool MasterClient::takeRoom(RoomAddress& out) {
    if (!m_hasRoom) return false;
    out = m_room;
    m_hasRoom = false;
    return true;
}

bool MasterClient::takeRefusal(RejectReason& out) {
    if (!m_refused) return false;
    out = m_refusal;
    m_refused = false;
    return true;
}

} // namespace OpenArtOfWar::Net
