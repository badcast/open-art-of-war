#include "openaow/net/MasterClient.hpp"

#include <chrono>

namespace OpenArtOfWar::Net
{

namespace
{
constexpr std::uint32_t kConnectTimeoutMs = 6000;
} // namespace

MasterClient::MasterClient(std::string_view host, std::uint16_t port)
{
    m_creating = std::async(std::launch::async, [hostStr = std::string(host), port]() {
        Created c;
        c.transport = createEnetClient(hostStr, port, &c.error);
        return c;
    });
}

void MasterClient::flush()
{
    if (m_state != State::Ready)
        return;
    for (const auto &bytes : m_queue)
        m_transport->send(0, Channel::Control, bytes.data(), bytes.size());
    m_queue.clear();
}

template <typename T>
static std::vector<std::uint8_t> encodeMessage(const T &m)
{
    std::vector<std::uint8_t> buf(1024);
    BitWriter w(buf.data(), buf.size());
    encode(w, m);
    buf.resize(w.finish());
    return buf;
}

void MasterClient::requestList(std::string_view gameVersion)
{
    m_queue.push_back(encodeMessage(ListRooms {std::string(gameVersion)}));
    flush();
}

void MasterClient::createRoom(
    const GameIdentity &identity, std::string_view name, bool listed, std::string_view password)
{
    m_queue.push_back(encodeMessage(
        CreateRoom {
            identity.gameVersion,
            identity.simBuildId,
            identity.assetsHash,
            std::string(name),
            listed,
            std::string(password)}));
    flush();
}

void MasterClient::registerHost(const RegisterHost &info)
{
    m_queue.push_back(encodeMessage(info));
    flush();
}

void MasterClient::joinHosted(std::string_view code, const SocketToken &token)
{
    m_queue.push_back(encodeMessage(JoinHosted {std::string(code), token}));
    flush();
}

bool MasterClient::takePunchRequest(PunchRequest &out)
{
    if (m_punches.empty())
        return false;
    out = m_punches.front();
    m_punches.erase(m_punches.begin());
    return true;
}

bool MasterClient::takeHostedEndpoint(HostedEndpoint &out)
{
    if (!m_hasEndpoint)
        return false;
    out = m_endpoint;
    m_hasEndpoint = false;
    return true;
}

void MasterClient::findRoom(std::string_view code)
{
    m_queue.push_back(encodeMessage(FindRoom {std::string(code)}));
    flush();
}

void MasterClient::update(std::uint32_t nowMs)
{
    if (m_creating.valid() && m_creating.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        Created c = m_creating.get();
        m_transport = std::move(c.transport);
        if (!m_transport)
        {
            m_state = State::Failed;
            m_error = c.error.empty() ? "cannot reach the server" : c.error;
        }
    }
    if (!m_transport || m_state == State::Failed)
        return;
    if (!m_started)
    {
        m_started = true;
        m_startMs = nowMs;
    }
    m_events.clear();
    m_transport->poll(m_events);
    for (const TransportEvent &e : m_events)
    {
        if (e.type == TransportEvent::Type::Connected)
        {
            m_state = State::Ready;
            flush();
            continue;
        }
        if (e.type == TransportEvent::Type::Disconnected)
        {
            m_state = State::Failed;
            m_error = "the server closed the connection";
            continue;
        }
        BitReader r(e.data.data(), e.data.size());
        switch (readType(r))
        {
            case MsgType::ServerInfo:
                decode(r, m_info);
                break;
            case MsgType::RoomList:
            {
                RoomList list;
                if (decode(r, list))
                {
                    m_rooms = std::move(list.rooms);
                    m_hasList = true;
                }
                break;
            }
            case MsgType::RoomAddress:
                if (decode(r, m_room))
                    m_hasRoom = true;
                break;
            case MsgType::HostRegistered:
            {
                HostRegistered m;
                if (decode(r, m))
                    m_hostedCode = m.code;
                break;
            }
            case MsgType::PunchRequest:
            {
                PunchRequest m;
                if (decode(r, m))
                    m_punches.push_back(m);
                break;
            }
            case MsgType::HostedEndpoint:
                if (decode(r, m_endpoint))
                    m_hasEndpoint = true;
                break;
            case MsgType::Reject:
            {
                Reject m;
                if (decode(r, m))
                {
                    m_refused = true;
                    m_refusal = m.reason;
                }
                break;
            }
            default:
                break;
        }
    }
    if (m_state == State::Connecting && nowMs - m_startMs > kConnectTimeoutMs)
    {
        m_state = State::Failed;
        m_error = "the server does not answer";
    }
}

bool MasterClient::takeRoom(RoomAddress &out)
{
    if (!m_hasRoom)
        return false;
    out = m_room;
    m_hasRoom = false;
    return true;
}

bool MasterClient::takeRefusal(RejectReason &out)
{
    if (!m_refused)
        return false;
    out = m_refusal;
    m_refused = false;
    return true;
}

} // namespace OpenArtOfWar::Net
