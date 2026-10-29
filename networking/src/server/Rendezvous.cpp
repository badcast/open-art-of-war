#include "openaow/net/Rendezvous.hpp"

#include <random>

namespace OpenArtOfWar::Net {

namespace {

SocketToken randomToken() {
    static std::mt19937_64 rng{std::random_device{}()};
    SocketToken t{};
    for (size_t i = 0; i < t.size(); i += 8) {
        const uint64_t v = rng();
        for (size_t k = 0; k < 8; ++k) t[i + k] = static_cast<uint8_t>(v >> (8 * k));
    }
    return t;
}

std::vector<uint8_t> hello(const SocketToken& token) {
    return encodeRaw(RawKind::Hello, std::vector<uint8_t>(token.begin(), token.end()));
}

constexpr uint32_t kJoinTimeoutMs = 15000;

} // namespace

HostAnnouncer::HostAnnouncer(const std::string& master, uint16_t masterPort, Transport& gameSocket, uint16_t gamePort,
                             const std::string& lanIp)
    : m_master(master, masterPort), m_socket(gameSocket), m_host(master), m_port(masterPort), m_gamePort(gamePort),
      m_lanIp(lanIp), m_token(randomToken()) {}

void HostAnnouncer::update(uint32_t nowMs, const RoomInfo& room, bool listed) {
    m_master.update(nowMs);
    // The socket's hello: a few quickly at first (UDP may lose one), then every 10 s to keep the
    // router's mapping open
    if (nowMs >= m_nextHelloMs) {
        m_socket.sendRaw(m_host, m_port, hello(m_token));
        m_nextHelloMs = nowMs + (++m_hellos < 5 ? 300 : 10000);
    }
    if (m_master.state() == MasterClient::State::Ready && nowMs >= m_nextRegisterMs) {
        RegisterHost info;
        info.gameVersion = room.gameVersion;
        info.name = room.name;
        info.map = room.map;
        info.players = room.players;
        info.maxPlayers = room.maxPlayers;
        info.listed = listed && !room.running;
        info.token = m_token;
        info.lanIp = m_lanIp;
        info.lanPort = m_gamePort;
        m_master.registerHost(info);
        m_nextRegisterMs = nowMs + 10000;
    }
    // A player is coming: open the way from this side
    PunchRequest punch;
    while (m_master.takePunchRequest(punch)) {
        for (int i = 0; i < 3; ++i) m_socket.sendRaw(punch.ip, punch.port, encodeRaw(RawKind::Punch, {}));
    }
}

HostedJoin::HostedJoin(const std::string& master, uint16_t masterPort, const std::string& code)
    : m_master(master, masterPort), m_host(master), m_port(masterPort), m_code(code), m_token(randomToken()) {
    m_socket = createEnetSocket(&m_error);
    if (!m_socket) m_state = State::Failed;
}

void HostedJoin::update(uint32_t nowMs) {
    if (m_state != State::Working) return;
    if (!m_started) {
        m_started = true;
        m_startMs = nowMs;
    }
    m_master.update(nowMs);
    if (m_master.state() == MasterClient::State::Failed) {
        m_state = State::Failed;
        m_error = m_master.error();
        return;
    }
    std::vector<TransportEvent> ignored;
    m_socket->poll(ignored);   // keeps the socket serviced (punches arrive, ENet idles)
    if (nowMs >= m_nextHelloMs) {
        m_socket->sendRaw(m_host, m_port, hello(m_token));
        m_nextHelloMs = nowMs + 300;
    }
    if (m_master.state() == MasterClient::State::Ready && nowMs >= m_nextAskMs) {
        m_master.joinHosted(m_code, m_token);
        m_nextAskMs = nowMs + 700;
    }
    RejectReason why{};
    if (m_master.takeRefusal(why) && why != RejectReason::TryAgain) {
        m_state = State::Failed;
        m_error = rejectReasonText(why);
        return;
    }
    HostedEndpoint where;
    if (m_master.takeHostedEndpoint(where)) {
        // Behind the same router as the host: its address in the local network; else the public one,
        // punched open from both sides
        const bool sameNetwork = where.yourIp == where.ip && !where.lanIp.empty();
        const std::string ip = sameNetwork ? where.lanIp : where.ip;
        const uint16_t port = sameNetwork ? where.lanPort : where.port;
        for (int i = 0; i < 3; ++i) m_socket->sendRaw(ip, port, encodeRaw(RawKind::Punch, {}));
        if (!m_socket->connectTo(ip, port)) {
            m_state = State::Failed;
            m_error = "cannot connect";
            return;
        }
        m_state = State::Ready;
        return;
    }
    if (nowMs - m_startMs > kJoinTimeoutMs) {
        m_state = State::Failed;
        m_error = "the host could not be reached through the server";
    }
}

} // namespace OpenArtOfWar::Net
