#include "openaow/net/Session.hpp"

#include <algorithm>

namespace OpenArtOfWar::Net {

namespace {
constexpr size_t kBufferSize = 16 * 1024;
} // namespace

SessionClient::SessionClient(std::unique_ptr<Transport> transport, ClientConfig config)
    : m_transport(std::move(transport)), m_config(std::move(config)), m_buffer(kBufferSize) {
    if (!m_transport) m_state = State::Disconnected;
}

void SessionClient::send(Channel channel, const uint8_t* data, size_t size) {
    if (m_transport && m_state != State::Disconnected) m_transport->send(0, channel, data, size);
}

void SessionClient::update(uint32_t nowMs) {
    (void)nowMs;
    if (!m_transport) return;
    m_events.clear();
    m_transport->poll(m_events);
    for (const TransportEvent& e : m_events) handle(e);
}

void SessionClient::handle(const TransportEvent& e) {
    if (e.type == TransportEvent::Type::Connected) {
        Hello hello;
        hello.gameVersion = m_config.identity.gameVersion;
        hello.simBuildId = m_config.identity.simBuildId;
        hello.assetsHash = m_config.identity.assetsHash;
        hello.playerName = m_config.playerName;
        hello.token = m_config.token;
        hello.password = m_config.password;
        BitWriter w(m_buffer.data(), m_buffer.size());
        encode(w, hello);
        send(Channel::Control, m_buffer.data(), w.finish());
        return;
    }
    if (e.type == TransportEvent::Type::Disconnected) {
        if (m_state != State::Rejected) m_state = State::Disconnected;
        return;
    }
    BitReader r(e.data.data(), e.data.size());
    switch (readType(r)) {
        case MsgType::Welcome:
            if (decode(r, m_welcome)) {
                m_state = State::Joined;
                m_config.token = m_welcome.token;
            }
            break;
        case MsgType::Reject:
            if (decode(r, m_reject)) m_state = State::Rejected;
            break;
        case MsgType::Roster: {
            Roster roster;
            if (decode(r, roster)) m_roster = std::move(roster);
            break;
        }
        case MsgType::LobbyState: {
            LobbyState lobby;
            if (decode(r, lobby)) m_lobby = std::move(lobby);
            break;
        }
        case MsgType::StartMatch: {
            StartMatch m;
            if (!decode(r, m)) break;
            // A match from the start (also after a reconnect: every turn so far follows)
            m_started = true;
            ++m_starts;
            m_setup = std::move(m.setup);
            m_turns.clear();
            m_turnsReceived = 0;
            m_desynced = false;
            m_pause = PauseState{};
            break;
        }
        case MsgType::TurnBundle: {
            TurnBundle bundle;
            if (!decode(r, bundle)) break;
            if (bundle.inputDelay != 0) m_inputDelay = bundle.inputDelay;
            m_turnsReceived = std::max(m_turnsReceived, bundle.turn + 1);
            m_turns[bundle.turn] = std::move(bundle);
            break;
        }
        case MsgType::PauseState: {
            PauseState m;
            if (decode(r, m)) m_pause = m;
            break;
        }
        case MsgType::Desync: {
            Desync m;
            if (decode(r, m)) {
                m_desynced = true;
                m_desyncTick = m.tick;
            }
            break;
        }
        case MsgType::Chat: {
            Chat m;
            if (decode(r, m)) m_chat.push_back(std::move(m));
            break;
        }
        case MsgType::FileData: {
            FileData m;
            if (decode(r, m)) m_files.push_back(std::move(m));
            break;
        }
        default:
            break;
    }
}

void SessionClient::sendCommands(uint32_t turn, const std::vector<uint8_t>& bytes) {
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, Commands{turn, bytes});
    const size_t size = w.finish();
    if (!w.overflowed()) send(Channel::Lockstep, m_buffer.data(), size);
}

const TurnBundle* SessionClient::turn(uint32_t turn) const {
    auto it = m_turns.find(turn);
    return it == m_turns.end() ? nullptr : &it->second;
}

void SessionClient::dropTurnsBefore(uint32_t turn) {
    m_turns.erase(m_turns.begin(), m_turns.lower_bound(turn));
}

void SessionClient::requestPause(bool pause) {
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, PauseRequest{pause});
    send(Channel::Lockstep, m_buffer.data(), w.finish());
}

void SessionClient::sendWorldHash(uint32_t tick, uint64_t hash) {
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, WorldHash{tick, hash});
    send(Channel::Lockstep, m_buffer.data(), w.finish());
}

void SessionClient::sendLobbyChoice(const LobbyChoice& choice) {
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, choice);
    send(Channel::Control, m_buffer.data(), w.finish());
}

void SessionClient::requestFile(uint64_t hash) {
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, FileRequest{hash});
    send(Channel::Control, m_buffer.data(), w.finish());
}

void SessionClient::uploadFile(uint64_t hash, const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> big(bytes.size() + 64);
    BitWriter w(big.data(), big.size());
    encode(w, FileData{hash, bytes});
    const size_t size = w.finish();
    if (!w.overflowed()) send(Channel::Bulk, big.data(), size);
}

bool SessionClient::takeFile(uint64_t& hash, std::vector<uint8_t>& bytes) {
    if (m_files.empty()) return false;
    hash = m_files.front().hash;
    bytes = std::move(m_files.front().bytes);
    m_files.erase(m_files.begin());
    return true;
}

void SessionClient::sendOwnerSetMap(const std::string& map, uint8_t spawnCount, uint64_t mapHash) {
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, OwnerSetMap{map, mapHash, spawnCount});
    send(Channel::Control, m_buffer.data(), w.finish());
}

void SessionClient::sendOwnerStart(const std::vector<uint8_t>& setup, uint32_t firstTurnDelayMs) {
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, OwnerStart{firstTurnDelayMs, setup});
    // On the lockstep channel, like the StartMatch it turns into
    send(Channel::Lockstep, m_buffer.data(), w.finish());
}

void SessionClient::sendOwnerSetAi(const OwnerSetAi& change) {
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, change);
    send(Channel::Control, m_buffer.data(), w.finish());
}

void SessionClient::sendChat(const std::string& text, bool teamOnly) {
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, Chat{0, teamOnly, text});
    send(Channel::Control, m_buffer.data(), w.finish());
}

std::vector<Chat> SessionClient::takeChat() {
    std::vector<Chat> out;
    out.swap(m_chat);
    return out;
}

void SessionClient::sendLeave() {
    if (!m_transport || m_state != State::Joined) return;
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, Leave{});
    send(Channel::Control, m_buffer.data(), w.finish());
    // Out of the door before the connection is torn down
    m_events.clear();
    m_transport->poll(m_events);
}

void SessionClient::disconnect() {
    if (m_transport) m_transport->disconnect(0);
    m_state = State::Disconnected;
}

uint32_t SessionClient::roundTripMs() const { return m_transport ? m_transport->roundTripMs(0) : 0; }

} // namespace OpenArtOfWar::Net
