#include "openaow/net/Session.hpp"

#include <algorithm>

namespace OpenArtOfWar::Net
{

namespace
{
constexpr std::size_t kBufferSize = 16 * 1024;
} // namespace

SessionClient::SessionClient(std::unique_ptr<Transport> transport, ClientConfig config)
    : m_transport(std::move(transport))
    , m_config(std::move(config))
    , m_buffer(kBufferSize)
{
    if (!m_transport)
        m_state = State::Disconnected;
}

void SessionClient::send(Channel channel, const std::uint8_t *data, std::size_t size)
{
    if (m_transport && m_state != State::Disconnected)
        m_transport->send(0, channel, data, size);
}

void SessionClient::update(std::uint32_t nowMs)
{
    (void) nowMs;
    if (!m_transport)
        return;
    m_events.clear();
    m_transport->poll(m_events);
    for (const TransportEvent &e : m_events)
        handle(e);
}

void SessionClient::handle(const TransportEvent &e)
{
    if (e.type == TransportEvent::Type::Connected)
    {
        Hello hello;
        hello.gameVersion = m_config.identity.gameVersion;
        hello.simBuildId = m_config.identity.simBuildId;
        hello.assetsHash = m_config.identity.assetsHash;
        hello.playerName = m_config.playerName;
        hello.token = m_config.token;
        hello.password = m_config.password;
        hello.canSync = m_config.canSync;
        BitWriter w(m_buffer.data(), m_buffer.size());
        encode(w, hello);
        send(Channel::Control, m_buffer.data(), w.finish());
        return;
    }
    if (e.type == TransportEvent::Type::Disconnected)
    {
        if (m_state != State::Rejected)
            m_state = State::Disconnected;
        return;
    }
    BitReader r(e.data.data(), e.data.size());
    switch (readType(r))
    {
        case MsgType::Welcome:
            if (decode(r, m_welcome))
            {
                m_state = State::Joined;
                m_config.token = m_welcome.token;
            }
            break;
        case MsgType::DataManifest:
        {
            DataManifest manifest;
            if (decode(r, manifest))
            {
                m_manifest = std::move(manifest.files);
                m_manifestWaiting = true;
            }
            break;
        }
        case MsgType::FilePathData:
        {
            FilePathData data;
            if (decode(r, data))
                m_dataFiles.push_back(std::move(data));
            break;
        }
        case MsgType::Reject:
            if (decode(r, m_reject))
                m_state = State::Rejected;
            break;
        case MsgType::Roster:
        {
            Roster roster;
            if (decode(r, roster))
                m_roster = std::move(roster);
            break;
        }
        case MsgType::LobbyState:
        {
            LobbyState lobby;
            if (decode(r, lobby))
                m_lobby = std::move(lobby);
            break;
        }
        case MsgType::StartMatch:
        {
            StartMatch m;
            if (!decode(r, m))
                break;
            // A match from the start (also after a reconnect: every turn so far follows)
            m_started = true;
            // Authority state of an older match goes; one of this match that overtook the start stays. The tick
            // filter starts again from what is left (a new match counts its ticks from zero)
            m_authorityStates.erase(
                std::remove_if(
                    m_authorityStates.begin(),
                    m_authorityStates.end(),
                    [&](const AuthorityState &a) { return a.matchId != m.matchId; }),
                m_authorityStates.end());
            m_hasAuthorityStateTick = false;
            m_authorityStateTick = 0;
            m_matchId = m.matchId;
            m_authorityAcks.clear();
            m_authoritySlot = m.authoritySlot;
            ++m_starts;
            m_setup = std::move(m.setup);
            m_turns.clear();
            m_turnsReceived = 0;
            m_desynced = false;
            m_pause = PauseState {};
            break;
        }
        case MsgType::AuthorityState:
        {
            AuthorityState state;
            if (e.data.size() <= kMaxAuthorityStateBytes + 32 && decode(r, state) && state.recipient == slot()
                && (!m_started || state.matchId == m_matchId)
                && (!m_hasAuthorityStateTick || state.tick > m_authorityStateTick))
            {
                if (m_authorityStates.size() >= kMaxQueuedAuthorityStates)
                    m_authorityStates.pop_front(); // a client that never takes them: the oldest go first
                m_authorityStates.push_back(std::move(state));
            }
            break;
        }
        case MsgType::AuthorityAck:
        {
            AuthorityAck ack;
            if (m_started && slot() == m_authoritySlot && decode(r, ack) && ack.matchId == m_matchId
                && ack.sender != slot())
                m_authorityAcks.push_back(ack);
            break;
        }
        case MsgType::TurnBundle:
        {
            TurnBundle bundle;
            if (!decode(r, bundle))
                break;
            if (bundle.inputDelay != 0)
                m_inputDelay = bundle.inputDelay;
            m_turnsReceived = std::max(m_turnsReceived, bundle.turn + 1);
            m_turns[bundle.turn] = std::move(bundle);
            break;
        }
        case MsgType::PauseState:
        {
            PauseState m;
            if (decode(r, m))
                m_pause = m;
            break;
        }
        case MsgType::Desync:
        {
            Desync m;
            if (decode(r, m))
            {
                m_desynced = true;
                m_desyncTick = m.tick;
            }
            break;
        }
        case MsgType::Chat:
        {
            Chat m;
            if (decode(r, m))
                m_chat.push_back(std::move(m));
            break;
        }
        case MsgType::FileData:
        {
            FileData m;
            if (decode(r, m))
                m_files.push_back(std::move(m));
            break;
        }
        default:
            break;
    }
}

bool SessionClient::sendAuthorityState(const AuthorityState &state)
{
    if (m_state != State::Joined || !m_started || state.recipient >= kMaxPlayers || state.recipient == slot()
        || state.bytes.size() > kMaxAuthorityStateBytes)
        return false;
    const auto recipient = std::find_if(m_roster.players.begin(), m_roster.players.end(), [&](const RosterEntry &p) {
        return p.slot == state.recipient && p.connected;
    });
    if (recipient == m_roster.players.end())
        return false; // this is a local roster check, not a delivery acknowledgement
    std::vector<std::uint8_t> packet(state.bytes.size() + 32);
    BitWriter w(packet.data(), packet.size());
    AuthorityState out = state;
    out.matchId = m_matchId;
    encode(w, out);
    const std::size_t size = w.finish();
    if (w.overflowed())
        return false;
    send(Channel::Bulk, packet.data(), size);
    return true;
}

bool SessionClient::takeAuthorityState(AuthorityState &state)
{
    while (!m_authorityStates.empty())
    {
        state = std::move(m_authorityStates.front());
        m_authorityStates.pop_front();
        if (!m_hasAuthorityStateTick || state.tick > m_authorityStateTick)
            return true;
    }
    return false;
}

bool SessionClient::sendAuthorityAck(std::uint32_t tick)
{
    if (m_state != State::Joined || !m_started || slot() == m_authoritySlot)
        return false;
    // The caller acknowledges only after decoding and applying the snapshot.
    if (!m_hasAuthorityStateTick || tick > m_authorityStateTick)
    {
        m_authorityStateTick = tick;
        m_hasAuthorityStateTick = true;
    }
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, AuthorityAck {slot(), m_matchId, tick});
    send(Channel::Control, m_buffer.data(), w.finish());
    return !w.overflowed();
}

bool SessionClient::takeAuthorityAck(AuthorityAck &ack)
{
    if (m_authorityAcks.empty())
        return false;
    ack = m_authorityAcks.front();
    m_authorityAcks.pop_front();
    return true;
}

void SessionClient::sendCommands(std::uint32_t turn, const std::vector<std::uint8_t> &bytes)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, Commands {turn, bytes});
    const std::size_t size = w.finish();
    if (!w.overflowed())
        send(Channel::Lockstep, m_buffer.data(), size);
}

const TurnBundle *SessionClient::turn(std::uint32_t turn) const
{
    auto it = m_turns.find(turn);
    return it == m_turns.end() ? nullptr : &it->second;
}

void SessionClient::dropTurnsBefore(std::uint32_t turn)
{
    m_turns.erase(m_turns.begin(), m_turns.lower_bound(turn));
}

void SessionClient::requestPause(bool pause)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, PauseRequest {pause});
    send(Channel::Lockstep, m_buffer.data(), w.finish());
}

void SessionClient::sendWorldHash(std::uint32_t tick, std::uint64_t hash)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, WorldHash {tick, hash});
    send(Channel::Lockstep, m_buffer.data(), w.finish());
}

void SessionClient::sendLobbyChoice(const LobbyChoice &choice)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, choice);
    send(Channel::Control, m_buffer.data(), w.finish());
}

bool SessionClient::takeDataManifest(std::vector<DataEntry> &files)
{
    if (!m_manifestWaiting)
        return false;
    files = std::move(m_manifest);
    m_manifest.clear();
    m_manifestWaiting = false;
    return true;
}

void SessionClient::requestDataFile(const std::string &path)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, FilePathRequest {path});
    send(Channel::Control, m_buffer.data(), w.finish());
}

bool SessionClient::takeDataFile(FilePathData &out)
{
    if (m_dataFiles.empty())
        return false;
    out = std::move(m_dataFiles.front());
    m_dataFiles.erase(m_dataFiles.begin());
    return true;
}

void SessionClient::sayHelloAgain(std::uint64_t assetsHash)
{
    m_config.identity.assetsHash = assetsHash;
    Hello hello;
    hello.gameVersion = m_config.identity.gameVersion;
    hello.simBuildId = m_config.identity.simBuildId;
    hello.assetsHash = assetsHash;
    hello.playerName = m_config.playerName;
    hello.token = m_config.token;
    hello.password = m_config.password;
    hello.canSync = false; // one round: a second mismatch is final
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, hello);
    send(Channel::Control, m_buffer.data(), w.finish());
}

void SessionClient::requestFile(std::uint64_t hash)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, FileRequest {hash});
    send(Channel::Control, m_buffer.data(), w.finish());
}

void SessionClient::uploadFile(std::uint64_t hash, const std::vector<std::uint8_t> &bytes)
{
    std::vector<std::uint8_t> big(bytes.size() + 64);
    BitWriter w(big.data(), big.size());
    encode(w, FileData {hash, bytes});
    const std::size_t size = w.finish();
    if (!w.overflowed())
        send(Channel::Bulk, big.data(), size);
}

bool SessionClient::takeFile(std::uint64_t &hash, std::vector<std::uint8_t> &bytes)
{
    if (m_files.empty())
        return false;
    hash = m_files.front().hash;
    bytes = std::move(m_files.front().bytes);
    m_files.erase(m_files.begin());
    return true;
}

void SessionClient::sendOwnerSetMap(const std::string &map, std::uint8_t spawnCount, std::uint64_t mapHash)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, OwnerSetMap {map, mapHash, spawnCount});
    send(Channel::Control, m_buffer.data(), w.finish());
}

void SessionClient::sendOwnerStart(const std::vector<std::uint8_t> &setup, std::uint32_t firstTurnDelayMs)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, OwnerStart {firstTurnDelayMs, setup});
    // On the lockstep channel, like the StartMatch it turns into
    send(Channel::Lockstep, m_buffer.data(), w.finish());
}

void SessionClient::sendOwnerSetAi(const OwnerSetAi &change)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, change);
    send(Channel::Control, m_buffer.data(), w.finish());
}

void SessionClient::sendOwnerSetTeam(const OwnerSetTeam &change)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, change);
    send(Channel::Control, m_buffer.data(), w.finish());
}

void SessionClient::sendOwnerSetSeat(const OwnerSetSeat &change)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, change);
    send(Channel::Control, m_buffer.data(), w.finish());
}

void SessionClient::sendOwnerSetOptions(const OwnerSetOptions &change)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, change);
    send(Channel::Control, m_buffer.data(), w.finish());
}

void SessionClient::sendChat(const std::string &text, bool teamOnly)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, Chat {0, teamOnly, text});
    send(Channel::Control, m_buffer.data(), w.finish());
}

std::vector<Chat> SessionClient::takeChat()
{
    std::vector<Chat> out;
    out.swap(m_chat);
    return out;
}

void SessionClient::sendLeave()
{
    if (!m_transport || m_state != State::Joined)
        return;
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, Leave {});
    send(Channel::Control, m_buffer.data(), w.finish());
    // Out of the door before the connection is torn down
    m_events.clear();
    m_transport->poll(m_events);
}

void SessionClient::disconnect()
{
    if (m_transport)
        m_transport->disconnect(0);
    m_state = State::Disconnected;
}

std::uint32_t SessionClient::roundTripMs() const
{
    return m_transport ? m_transport->roundTripMs(0) : 0;
}
LinkStats SessionClient::linkStats() const
{
    return m_transport ? m_transport->linkStats(0) : LinkStats {};
}

} // namespace OpenArtOfWar::Net
