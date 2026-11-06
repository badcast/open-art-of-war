#include "openaow/net/Session.hpp"

#include "openaow/net/ReplayFile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <random>

namespace OpenArtOfWar::Net
{

namespace
{

constexpr std::size_t kBufferSize = 16 * 1024;

SessionToken newToken()
{
    static std::mt19937_64 rng {std::random_device {}()};
    SessionToken t {};
    for (std::size_t i = 0; i < t.size(); i += 8)
    {
        const std::uint64_t v = rng();
        for (std::size_t k = 0; k < 8 && i + k < t.size(); ++k)
            t[i + k] = static_cast<std::uint8_t>(v >> (8 * k));
    }
    return t;
}

bool isZero(const SessionToken &t)
{
    return std::all_of(t.begin(), t.end(), [](std::uint8_t b) { return b == 0; });
}

} // namespace

SessionHost::SessionHost(HostConfig config)
    : m_config(std::move(config))
    , m_buffer(kBufferSize)
{
    m_config.maxPlayers = std::clamp(m_config.maxPlayers, 1, kMaxPlayers);
    m_inputDelay = std::max<std::uint8_t>(m_config.inputDelayMin, 1);
}

SessionHost::~SessionHost()
{
    finishReplay("the room was closed");
}

void SessionHost::logLine(const std::string &line) const
{
    if (m_config.log)
        m_config.log(line);
}

void SessionHost::finishReplay(const std::string &reason)
{
    if (!m_replayOpen)
        return;
    m_replayOpen = false;
    if (m_config.replayDir.empty() || m_turnLog.empty())
        return;
    ReplayData data;
    data.setup = m_setup;
    data.turns = m_turnLog;
    data.checksums = m_replayChecksums;
    nlohmann::json players = nlohmann::json::array();
    for (std::uint8_t s = 0; s < kMaxPlayers; ++s)
    {
        if (m_seen[s].name.empty() && !m_slots[s].ai)
            continue;
        players.push_back(
            {{"slot", s},
             {"name", m_slots[s].ai ? std::string("AI") : m_seen[s].name},
             {"human", !m_slots[s].ai},
             {"ai_level", m_slots[s].aiLevel},
             {"addresses", m_seen[s].addresses}});
    }
    const std::int64_t endedAt = static_cast<std::int64_t>(std::time(nullptr));
    const std::uint32_t tpt = std::max<std::uint32_t>(1, m_config.ticksPerTurn);
    nlohmann::json meta
        = {{"format", 1},
           {"game_version", m_config.identity.gameVersion},
           {"protocol", kProtocolVersion},
           {"source", "server"},
           {"recorded_at", m_replayStartedAt},
           {"ended_at", endedAt},
           {"map", m_lobby.map},
           {"map_hash", m_lobby.mapHash},
           {"assets_hash", m_config.identity.assetsHash},
           {"turn_ms", m_config.turnMs},
           {"ticks_per_turn", tpt},
           {"turns", data.turns.size()},
           {"ticks", data.turns.size() * tpt},
           {"players", players},
           {"desynced", m_desynced},
           {"desync_tick", m_desyncTick},
           {"end_reason", reason},
           {"checksum_interval", 20}};
    data.meta = meta.dump(2);
    const std::string path
        = (std::filesystem::path(m_config.replayDir) / replayFileName(m_lobby.map, m_replayStartedAt)).string();
    std::string error;
    if (writeReplay(path, data, error))
        logLine("replay saved: " + path + " (" + std::to_string(data.turns.size()) + " turns, " + reason + ")");
    else
        logLine("replay NOT saved: " + path + ": " + error);
}

void SessionHost::addTransport(std::unique_ptr<Transport> transport)
{
    if (transport)
        m_transports.push_back(std::move(transport));
}

void SessionHost::sendTo(std::size_t connection, Channel channel, const std::uint8_t *data, std::size_t size)
{
    const Connection &c = m_connections[connection];
    m_transports[c.transport]->send(c.peer, channel, data, size);
}

void SessionHost::broadcast(Channel channel, const std::uint8_t *data, std::size_t size)
{
    for (std::size_t i = 0; i < m_connections.size(); ++i)
    {
        if (m_connections[i].slot >= 0)
            sendTo(i, channel, data, size);
    }
}

void SessionHost::reject(std::size_t connection, RejectReason reason, const std::string &detail)
{
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, Reject {reason, detail});
    sendTo(connection, Channel::Control, m_buffer.data(), w.finish());
    const Connection &c = m_connections[connection];
    m_transports[c.transport]->disconnect(c.peer);
}

void SessionHost::broadcastRoster()
{
    Roster roster;
    for (std::uint8_t s = 0; s < kMaxPlayers; ++s)
    {
        if (m_slots[s].used)
            roster.players.push_back({s, m_slots[s].name, m_slots[s].connected || m_slots[s].ai});
    }
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, roster);
    broadcast(Channel::Control, m_buffer.data(), w.finish());
}

void SessionHost::offerFile(std::uint64_t hash, std::vector<std::uint8_t> bytes)
{
    if (bytes.empty() || bytes.size() > kMaxFileBytes)
        return;
    m_files.erase(
        std::remove_if(m_files.begin(), m_files.end(), [&](const auto &f) { return f.first == hash; }), m_files.end());
    m_files.emplace_back(hash, std::move(bytes));
    auto total = [&] {
        std::size_t n = 0;
        for (const auto &f : m_files)
            n += f.second.size();
        return n;
    };
    while (m_files.size() > kMaxStoredFiles || (m_files.size() > 1 && total() > kMaxStoredBytes))
        m_files.erase(m_files.begin());
}

void SessionHost::setLobbyMap(const std::string &map, std::uint8_t spawnCount, std::uint64_t mapHash)
{
    if (m_running)
        return;
    // A map with fewer start positions than there are players in the room is not taken
    int inRoom = 0;
    for (const SlotState &s : m_slots)
        inRoom += s.used ? 1 : 0;
    if (spawnCount > 0 && inRoom > spawnCount)
        return;
    m_lobby.map = map;
    m_lobby.mapHash = mapHash;
    m_lobby.spawnCount = std::min<std::uint8_t>(spawnCount, kMaxPlayers);
    // Another map: nobody is ready for it yet, blocked places are open again, everybody keeps a place of his own
    m_lobby.blockedMask = 0;
    for (SlotState &s : m_slots)
    {
        s.choice.ready = s.ai; // computer players are always ready
        if (s.used && s.choice.spawn != kRandom && s.choice.spawn >= m_lobby.spawnCount)
            s.choice.spawn = kRandom;
    }
    for (std::uint8_t i = 0; i < kMaxPlayers; ++i)
    {
        if (m_slots[i].used && m_slots[i].choice.spawn == kRandom)
            m_slots[i].choice.spawn = freeSpawn();
    }
    broadcastLobby();
}

std::uint8_t SessionHost::freeSpawn() const
{
    for (std::uint8_t n = 0; n < m_lobby.spawnCount && n < kMaxPlayers; ++n)
    {
        if (m_lobby.blockedMask & (1u << n))
            continue;
        bool held = false;
        for (const SlotState &s : m_slots)
            held = held || (s.used && s.choice.spawn == n);
        if (!held)
            return n;
    }
    return kRandom;
}

void SessionHost::setSeat(const OwnerSetSeat &change)
{
    if (m_running || change.spawn >= m_lobby.spawnCount || change.spawn >= kMaxPlayers
        || change.kind > OwnerSetSeat::Ai)
        return;
    int holder = -1;
    for (std::uint8_t s = 0; s < kMaxPlayers; ++s)
    {
        if (m_slots[s].used && m_slots[s].choice.spawn == change.spawn)
            holder = s;
    }
    if (holder >= 0 && !m_slots[holder].ai && holder == ownerSlot())
        return; // the owner cannot free his own place
    // A computer player of another level only changes its level
    if (change.kind == OwnerSetSeat::Ai && holder >= 0 && m_slots[holder].ai)
    {
        m_slots[holder].aiLevel = change.level & 3;
        broadcastRoster();
        broadcastLobby();
        return;
    }
    // A person is sent out of the room
    if (holder >= 0 && !m_slots[holder].ai && m_slots[holder].connected)
    {
        for (std::size_t i = 0; i < m_connections.size(); ++i)
        {
            if (m_connections[i].slot == holder)
            {
                reject(i, RejectReason::Kicked, "");
                m_connections[i].slot = -2;
            }
        }
    }
    if (holder >= 0)
        m_slots[holder] = SlotState {};
    if (change.kind == OwnerSetSeat::Blocked)
        m_lobby.blockedMask |= static_cast<std::uint8_t>(1u << change.spawn);
    else
        m_lobby.blockedMask &= static_cast<std::uint8_t>(~(1u << change.spawn));
    if (change.kind == OwnerSetSeat::Ai)
    {
        std::uint8_t slot = kRandom;
        for (std::uint8_t s = 0; s < m_config.maxPlayers && slot == kRandom; ++s)
        {
            if (!m_slots[s].used)
                slot = s;
        }
        if (slot != kRandom)
        {
            m_slots[slot] = SlotState {};
            m_slots[slot].used = true;
            m_slots[slot].ai = true;
            m_slots[slot].name = "AI";
            m_slots[slot].aiLevel = change.level & 3;
            m_slots[slot].choice.team = static_cast<std::uint8_t>(slot % 2);
            m_slots[slot].choice.color = freeColor(slot, static_cast<std::uint8_t>(slot + 1));
            m_slots[slot].choice.ready = true;
            m_slots[slot].choice.spawn = change.spawn;
        }
    }
    broadcastRoster();
    broadcastLobby();
}

void SessionHost::applyChoice(std::uint8_t slot, LobbyChoice choice, bool ownerEdit)
{
    if (m_running || slot >= kMaxPlayers || !m_slots[slot].used)
        return;
    // A start position the map does not have, a held or a blocked one: the player stays where he is (places are
    // taken from an open position, the room owner frees others with OwnerSetSeat)
    {
        bool ok = choice.spawn != kRandom && choice.spawn < m_lobby.spawnCount
                  && !(m_lobby.blockedMask & (1u << choice.spawn));
        for (std::uint8_t s = 0; ok && s < kMaxPlayers; ++s)
            ok = !(s != slot && m_slots[s].used && m_slots[s].choice.spawn == choice.spawn);
        if (!ok)
            choice.spawn = m_slots[slot].choice.spawn;
    }
    choice.team &= 7;
    choice.color &= 15;
    if (!ownerEdit || choice.handicap > 5)
        choice.handicap = m_slots[slot].choice.handicap;
    if (!ownerEdit || choice.armyCap > 7)
        choice.armyCap = m_slots[slot].choice.armyCap;
    // A colour somebody else has already picked: the player keeps the own one (two armies of one colour could not
    // be told apart)
    if (freeColor(slot, choice.color) != choice.color)
        choice.color = freeColor(slot, m_slots[slot].choice.color);
    m_slots[slot].choice = choice;
    broadcastLobby();
}

std::uint8_t SessionHost::freeColor(std::uint8_t slot, std::uint8_t preferred) const
{
    constexpr std::uint8_t kColors = 8;
    if (preferred < 1 || preferred > kColors)
        return preferred;
    auto taken = [&](std::uint8_t color) {
        for (std::uint8_t s = 0; s < kMaxPlayers; ++s)
        {
            if (s != slot && m_slots[s].used && m_slots[s].choice.color == color)
                return true;
        }
        return false;
    };
    if (!taken(preferred))
        return preferred;
    for (std::uint8_t c = 1; c <= kColors; ++c)
    {
        if (!taken(c))
            return c;
    }
    return preferred;
}

void SessionHost::setTeam(std::uint8_t slot, std::uint8_t team, std::uint8_t handicap, std::uint8_t armyCap)
{
    if (m_running || slot >= kMaxPlayers || !m_slots[slot].used)
        return;
    m_slots[slot].choice.team = team & 7;
    if (handicap <= 5)
        m_slots[slot].choice.handicap = handicap;
    if (armyCap <= 7)
        m_slots[slot].choice.armyCap = armyCap;
    broadcastLobby();
}

void SessionHost::setOptions(bool fogOfWar, bool prebuilt, std::uint8_t credits, bool night)
{
    if (m_running || credits > 4)
        return;
    m_lobby.fogOfWar = fogOfWar;
    m_lobby.prebuilt = prebuilt;
    m_lobby.credits = credits;
    m_lobby.night = night;
    broadcastLobby();
}

void SessionHost::setAiSlot(const OwnerSetAi &change)
{
    if (m_running)
        return;
    std::uint8_t slot = change.slot;
    if (slot == kRandom)
    {
        // A new computer player: the first free slot the map has a start position for
        int used = 0;
        for (const SlotState &s : m_slots)
            used += s.used ? 1 : 0;
        if (used >= std::min<int>(m_config.maxPlayers, m_lobby.spawnCount ? m_lobby.spawnCount : m_config.maxPlayers))
            return;
        for (std::uint8_t s = 0; s < m_config.maxPlayers && slot == kRandom; ++s)
        {
            if (!m_slots[s].used)
                slot = s;
        }
        if (slot == kRandom)
            return;
        m_slots[slot] = SlotState {};
        m_slots[slot].used = true;
        m_slots[slot].ai = true;
        m_slots[slot].name = "AI";
        m_slots[slot].choice.spawn = freeSpawn();
        if (m_slots[slot].choice.spawn == kRandom && m_lobby.spawnCount > 0)
        {
            m_slots[slot] = SlotState {}; // no open start position left
            return;
        }
    }
    if (slot >= kMaxPlayers || !m_slots[slot].used || !m_slots[slot].ai)
        return;
    if (change.remove)
    {
        m_slots[slot] = SlotState {};
    }
    else
    {
        m_slots[slot].aiLevel = change.level & 3;
        // A computer player never waits: always ready; a start position it may not take is left to chance
        LobbyChoice choice = change.choice;
        choice.ready = true;
        if (change.slot == kRandom)
        {
            choice.team = static_cast<std::uint8_t>(slot % 2);
            choice.color = freeColor(slot, static_cast<std::uint8_t>(slot + 1));
            m_slots[slot].choice.color = choice.color;
        }
        m_slots[slot].choice.ready = true;
        applyChoice(slot, choice, true);
        return; // applyChoice broadcasts
    }
    broadcastRoster();
    broadcastLobby();
}

void SessionHost::broadcastLobby()
{
    m_lobby.slots.clear();
    for (std::uint8_t s = 0; s < kMaxPlayers; ++s)
    {
        if (m_slots[s].used)
            m_lobby.slots.push_back({s, m_slots[s].choice, m_slots[s].ai, m_slots[s].aiLevel});
    }
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, m_lobby);
    broadcast(Channel::Control, m_buffer.data(), w.finish());
}

bool SessionHost::everyoneReady(int minPlayers) const
{
    int n = 0;
    for (const SlotState &s : m_slots)
    {
        if (s.used && s.ai)
        {
            ++n;
            continue;
        }
        if (!s.used || !s.connected)
            continue;
        if (!s.choice.ready)
            return false;
        ++n;
    }
    return n >= minPlayers;
}

void SessionHost::update(std::uint32_t nowMs)
{
    for (std::size_t t = 0; t < m_transports.size(); ++t)
    {
        m_events.clear();
        m_transports[t]->poll(m_events);
        for (const TransportEvent &e : m_events)
        {
            // The connection this event belongs to (a new one for a new peer)
            std::size_t index = m_connections.size();
            for (std::size_t i = 0; i < m_connections.size(); ++i)
            {
                if (m_connections[i].transport == t && m_connections[i].peer == e.peer)
                {
                    index = i;
                    break;
                }
            }
            if (index == m_connections.size())
            {
                if (e.type != TransportEvent::Type::Connected)
                    continue;
                m_connections.push_back({t, e.peer, -1});
                logLine("connection from " + m_transports[t]->peerAddress(e.peer));
            }
            handle(index, e, nowMs);
        }
    }
    // A client that does not finish fetching the host's data files in time is let go
    for (Connection &c : m_connections)
    {
        if (c.syncing && c.slot == -1 && nowMs - c.syncStartMs > kMaxSyncMs)
        {
            m_transports[c.transport]->disconnect(c.peer);
            c.slot = -2;
        }
    }
    // Drop the connections that are gone (their slots stay for a reconnect)
    m_connections.erase(
        std::remove_if(m_connections.begin(), m_connections.end(), [](const Connection &c) { return c.slot == -2; }),
        m_connections.end());
    for (std::uint8_t s = 0; s < kMaxPlayers; ++s)
    {
        SlotState &slot = m_slots[s];
        if (!slot.used || !slot.connected)
            continue;
        slot.connection = m_connections.size();
        for (std::size_t i = 0; i < m_connections.size(); ++i)
        {
            if (m_connections[i].slot == s)
                slot.connection = i;
        }
    }

    // A running match: players who have been gone longer than the reconnect window are given up; with nobody left the
    // match is over and the room is an empty lobby again (no zombie battle nobody plays)
    if (m_running)
    {
        bool changed = false;
        int humans = 0;
        for (SlotState &slot : m_slots)
        {
            if (!slot.used || slot.ai)
                continue;
            if (!slot.connected && nowMs - slot.goneMs > m_config.reconnectWindowMs)
            {
                slot = SlotState {};
                m_leftPending.push_back(static_cast<std::uint8_t>(&slot - m_slots.data()));
                changed = true;
            }
            else
            {
                ++humans;
            }
        }
        if (humans == 0)
        {
            finishReplay("all players left");
            for (SlotState &slot : m_slots)
                slot = SlotState {};
            m_running = false;
            m_paused = false;
            m_nextTurn = 0;
            m_turnLog.clear();
            m_leftPending.clear();
            changed = true;
        }
        if (changed)
        {
            broadcastRoster();
            broadcastLobby();
        }
    }

    // What local network browsers see of the room
    const RoomInfo room = roomInfo();
    for (auto &t : m_transports)
        t->setRoomInfo(room);

    // A pause ends by itself after a while
    if (m_paused && nowMs - m_pauseStartMs > m_config.maxPauseMs)
        setPaused(false, m_pausedBy, nowMs);
    // Close every turn that is due by the host's clock
    while (m_running && !m_paused
           && static_cast<std::int64_t>(nowMs) - static_cast<std::int64_t>(m_startMs)
                  >= static_cast<std::int64_t>(m_nextTurn) * m_config.turnMs)
    {
        adaptInputDelay();
        closeTurn(m_nextTurn++);
    }
}

void SessionHost::handle(std::size_t connection, const TransportEvent &e, std::uint32_t nowMs)
{
    Connection &c = m_connections[connection];
    if (e.type == TransportEvent::Type::Connected)
        return; // waiting for its Hello
    if (e.type == TransportEvent::Type::Disconnected)
    {
        if (c.slot >= 0)
        {
            logLine(
                "player disconnected: slot " + std::to_string(c.slot) + " '" + m_slots[c.slot].name + "' from "
                + m_transports[c.transport]->peerAddress(c.peer));
            m_slots[c.slot].connected = false;
            m_slots[c.slot].goneMs = nowMs;
            // In the room the slot is free again; in a match it waits for the player to come back (for a while)
            if (!m_running)
                m_slots[c.slot] = SlotState {};
            c.slot = -2;
            broadcastRoster();
            broadcastLobby();
        }
        else
        {
            c.slot = -2;
        }
        return;
    }
    BitReader r(e.data.data(), e.data.size());
    const MsgType type = readType(r);
    if (c.slot < 0)
    {
        // A client with other data fetches the host's files before it says hello again
        if (type == MsgType::FilePathRequest && c.syncing)
        {
            FilePathRequest m;
            if (!decode(r, m))
                return;
            FilePathData answer;
            answer.path = m.path;
            bool listed = false;
            for (const DataEntry &entry : m_config.dataManifest)
                listed = listed || entry.path == m.path;
            if (listed && m_config.dataFile)
            {
                std::vector<std::uint8_t> packed;
                std::uint64_t hash = 0;
                if (m_config.dataFile(m.path, packed, hash) && packed.size() <= kMaxFileBytes)
                {
                    answer.hash = hash;
                    answer.bytes = std::move(packed);
                }
            }
            std::vector<std::uint8_t> big(answer.bytes.size() + 256);
            BitWriter w(big.data(), big.size());
            encode(w, answer);
            sendTo(connection, Channel::Bulk, big.data(), w.finish());
            return;
        }
        Hello hello;
        if (type != MsgType::Hello || !decode(r, hello))
        {
            reject(connection, RejectReason::Malformed, "expected a hello");
            return;
        }
        handleHello(connection, hello, nowMs);
        return;
    }
    const std::uint8_t slot = static_cast<std::uint8_t>(c.slot);
    switch (type)
    {
        case MsgType::Commands:
        {
            Commands m;
            if (!decode(r, m) || !m_running)
                return;
            // Too late for its turn: the next open one (nobody waits for a slow peer)
            const std::uint32_t turn = std::max(m.turn, m_nextTurn);
            if (turn > m_nextTurn + 64)
                return; // nonsense far in the future
            // A late batch can land on a turn that already has one: the bundle carries one entry per batch
            if (!m.bytes.empty())
                m_slots[slot].commands[turn].push_back(std::move(m.bytes));
            break;
        }
        case MsgType::WorldHash:
        {
            WorldHash m;
            if (!decode(r, m) || m_desynced)
                return;
            auto &byTick = m_hashes[m.tick];
            byTick[slot] = m.hash;
            int expected = 0;
            for (const SlotState &s : m_slots)
                expected += (s.used && s.connected) ? 1 : 0;
            if (static_cast<int>(byTick.size()) >= expected)
            {
                bool same = true;
                for (const auto &[s, h] : byTick)
                    same = same && h == byTick.begin()->second;
                if (!same)
                {
                    m_desynced = true;
                    m_desyncTick = m.tick;
                    BitWriter w(m_buffer.data(), m_buffer.size());
                    encode(w, Desync {m.tick});
                    broadcast(Channel::Lockstep, m_buffer.data(), w.finish());
                    logLine("desync at tick " + std::to_string(m.tick));
                }
                else if (m_replayOpen)
                {
                    m_replayChecksums.emplace_back(m.tick, byTick.begin()->second);
                }
                m_hashes.erase(m_hashes.begin(), m_hashes.upper_bound(m.tick));
            }
            break;
        }
        case MsgType::OwnerSetMap:
        {
            OwnerSetMap m;
            if (m_config.ownerControls && slot == ownerSlot() && decode(r, m))
                setLobbyMap(m.map, m.spawnCount, m.mapHash);
            break;
        }
        case MsgType::OwnerSetAi:
        {
            OwnerSetAi m;
            if (m_config.ownerControls && slot == ownerSlot() && decode(r, m))
                setAiSlot(m);
            break;
        }
        case MsgType::OwnerSetTeam:
        {
            OwnerSetTeam m;
            if (slot == ownerSlot() && decode(r, m))
                setTeam(m.slot, m.team, m.handicap, m.armyCap);
            break;
        }
        case MsgType::OwnerSetSeat:
        {
            OwnerSetSeat m;
            if (slot == ownerSlot() && decode(r, m))
                setSeat(m);
            break;
        }
        case MsgType::OwnerSetOptions:
        {
            OwnerSetOptions m;
            if (slot == ownerSlot() && decode(r, m))
                setOptions(m.fogOfWar, m.prebuilt, m.credits, m.night);
            break;
        }
        case MsgType::OwnerStart:
        {
            OwnerStart m;
            if (m_config.ownerControls && slot == ownerSlot() && decode(r, m) && everyoneReady(2))
            {
                startMatch(std::move(m.setup), nowMs, m.firstTurnDelayMs);
            }
            break;
        }
        case MsgType::FileRequest:
        {
            FileRequest m;
            if (!decode(r, m))
                return;
            FileData answer;
            answer.hash = m.hash;
            for (const auto &f : m_files)
            {
                if (f.first == m.hash)
                    answer.bytes = f.second;
            }
            std::vector<std::uint8_t> big(answer.bytes.size() + 64);
            BitWriter w(big.data(), big.size());
            encode(w, answer);
            // The bulk channel: a big file never holds up turns or the room
            sendTo(connection, Channel::Bulk, big.data(), w.finish());
            break;
        }
        case MsgType::FileData:
        {
            FileData m;
            if (m_config.ownerControls && slot == ownerSlot() && decode(r, m))
                offerFile(m.hash, std::move(m.bytes));
            break;
        }
        case MsgType::PauseRequest:
        {
            PauseRequest m;
            if (!decode(r, m) || !m_running)
                return;
            if (m.pause && !m_paused && m_slots[slot].pausesLeft > 0)
            {
                --m_slots[slot].pausesLeft;
                setPaused(true, slot, nowMs);
            }
            else if (!m.pause && m_paused)
            {
                setPaused(false, slot, nowMs);
            }
            break;
        }
        case MsgType::LobbyChoice:
        {
            LobbyChoice m;
            if (!decode(r, m))
                break;
            // Teams are the host's / room owner's decision: a player's own pick keeps the team it has
            if (static_cast<int>(slot) != ownerSlot())
                m.team = m_slots[slot].choice.team;
            applyChoice(slot, m);
            break;
        }
        case MsgType::Leave:
        {
            // The player goes for good: the slot is free at once, in the room and in a match alike; in a match
            // everybody removes the player's forces with the next turn
            m_slots[slot] = SlotState {};
            if (m_running)
                m_leftPending.push_back(slot);
            c.slot = -2;
            broadcastRoster();
            broadcastLobby();
            break;
        }
        case MsgType::Chat:
        {
            Chat m;
            if (!decode(r, m))
                return;
            m.fromSlot = slot; // never trusted from the sender
            BitWriter w(m_buffer.data(), m_buffer.size());
            encode(w, m);
            broadcast(Channel::Control, m_buffer.data(), w.finish());
            break;
        }
        default:
            break;
    }
}

void SessionHost::handleHello(std::size_t connection, const Hello &hello, std::uint32_t nowMs)
{
    if (hello.protocol != kProtocolVersion)
    {
        reject(connection, RejectReason::ProtocolMismatch, "");
        return;
    }
    if (hello.gameVersion != m_config.identity.gameVersion || hello.simBuildId != m_config.identity.simBuildId)
    {
        reject(connection, RejectReason::VersionMismatch, m_config.identity.gameVersion);
        return;
    }
    if (hello.assetsHash != m_config.identity.assetsHash)
    {
        // A client that can sync gets the host's list and keeps the connection to fetch the files
        int syncing = 0;
        for (const Connection &other : m_connections)
            syncing += other.syncing ? 1 : 0;
        if (hello.canSync && !m_config.dataManifest.empty() && m_config.dataFile
            && (m_connections[connection].syncing || syncing < kMaxSyncing))
        {
            Connection &c = m_connections[connection];
            if (!c.syncing)
                c.syncStartMs = nowMs;
            c.syncing = true;
            DataManifest manifest {m_config.dataManifest};
            std::vector<std::uint8_t> big(m_config.dataManifest.size() * (kMaxDataPathLength + 16) + 64);
            BitWriter w(big.data(), big.size());
            encode(w, manifest);
            sendTo(connection, Channel::Control, big.data(), w.finish());
            return;
        }
        reject(connection, RejectReason::AssetsMismatch, "");
        return;
    }
    m_connections[connection].syncing = false;
    // Coming back after a drop: the same slot
    int slot = -1;
    if (!isZero(hello.token))
    {
        for (int s = 0; s < kMaxPlayers; ++s)
        {
            if (m_slots[s].used && !m_slots[s].ai && m_slots[s].token == hello.token)
                slot = s;
        }
        // The player's side already gave the old link up while this side has not noticed yet (a lossy link times
        // out at different moments at both ends): the secret token proves it is the same player, the new
        // connection takes the slot over and the stale one is dropped
        if (slot >= 0 && m_slots[slot].connected)
        {
            for (std::size_t i = 0; i < m_connections.size(); ++i)
            {
                if (i != connection && m_connections[i].slot == slot)
                {
                    m_connections[i].slot = -2;
                    m_transports[m_connections[i].transport]->disconnect(m_connections[i].peer);
                }
            }
            m_slots[slot].connected = false;
        }
    }
    const bool resumed = slot >= 0;
    if (!resumed)
    {
        if (!m_config.password.empty() && hello.password != m_config.password)
        {
            reject(connection, RejectReason::BadPassword, "");
            return;
        }
        if (m_running)
        {
            reject(connection, RejectReason::MatchRunning, "");
            return;
        }
        // The room holds as many players as the map has start positions
        int taken = 0;
        for (const SlotState &other : m_slots)
            taken += other.used ? 1 : 0;
        const int capacity = (!m_lobby.map.empty() && m_lobby.spawnCount)
                                 ? std::min<int>(m_config.maxPlayers, m_lobby.spawnCount)
                                 : m_config.maxPlayers; // no map yet: no limit
        if (!m_lobby.map.empty() && m_lobby.spawnCount && freeSpawn() == kRandom)
        {
            reject(connection, RejectReason::RoomFull, ""); // every start position is taken or blocked
            return;
        }
        for (int s = 0; s < m_config.maxPlayers && slot < 0 && taken < capacity; ++s)
        {
            if (!m_slots[s].used)
                slot = s;
        }
        if (slot < 0)
        {
            reject(connection, RejectReason::RoomFull, "");
            return;
        }
        m_slots[slot] = SlotState {};
        m_slots[slot].used = true;
        m_slots[slot].token = newToken();
        // Defaults: random faction and start, alternating teams, an own colour
        m_slots[slot].choice.team = static_cast<std::uint8_t>(slot % 2);
        m_slots[slot].choice.color = freeColor(static_cast<std::uint8_t>(slot), static_cast<std::uint8_t>(slot + 1));
        m_slots[slot].choice.spawn = freeSpawn(); // the first open start position
    }
    SlotState &s = m_slots[slot];
    s.connected = true;
    s.name = hello.playerName.empty() ? "Player " + std::to_string(slot + 1) : hello.playerName;
    m_connections[connection].slot = slot;
    {
        // Who is who: the address of every player, kept for the log and the replay of the match
        SeenPlayer &seen = m_seen[slot];
        if (!resumed)
            seen = SeenPlayer {};
        seen.name = s.name;
        const std::string address
            = m_transports[m_connections[connection].transport]->peerAddress(m_connections[connection].peer);
        if (std::find(seen.addresses.begin(), seen.addresses.end(), address) == seen.addresses.end())
            seen.addresses.push_back(address);
        logLine(
            std::string(resumed ? "player back: slot " : "player joined: slot ") + std::to_string(slot) + " '" + s.name
            + "' from " + address);
    }

    Welcome welcome;
    welcome.slot = static_cast<std::uint8_t>(slot);
    welcome.token = s.token;
    welcome.turnMs = m_config.turnMs;
    welcome.ticksPerTurn = m_config.ticksPerTurn;
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, welcome);
    sendTo(connection, Channel::Control, m_buffer.data(), w.finish());
    broadcastRoster();
    broadcastLobby();

    if (resumed && m_running)
    {
        // Back into a running match: the setup and every turn so far, the game plays them through
        BitWriter sw(m_buffer.data(), m_buffer.size());
        encode(sw, StartMatch {0, m_setup});
        sendTo(connection, Channel::Lockstep, m_buffer.data(), sw.finish());
        for (const std::vector<std::uint8_t> &bundle : m_turnLog)
            sendTo(connection, Channel::Lockstep, bundle.data(), bundle.size());
    }
}

void SessionHost::setPaused(bool paused, std::uint8_t bySlot, std::uint32_t nowMs)
{
    if (!m_running || paused == m_paused)
        return;
    if (paused)
    {
        m_pauseStartMs = nowMs;
        m_pausedBy = bySlot;
    }
    else
    {
        // The turn clock goes on where it stopped: no burst of turns after the pause
        m_startMs += nowMs - m_pauseStartMs;
    }
    m_paused = paused;
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, PauseState {m_paused, bySlot, m_slots[bySlot].pausesLeft});
    broadcast(Channel::Lockstep, m_buffer.data(), w.finish());
}

void SessionHost::startMatch(std::vector<std::uint8_t> setup, std::uint32_t nowMs, std::uint32_t firstTurnDelayMs)
{
    if (m_running)
        return;
    m_setup = std::move(setup);
    m_running = true;
    m_paused = false;
    for (SlotState &s : m_slots)
        s.pausesLeft = m_config.pausesPerPlayer;
    m_startMs = nowMs + firstTurnDelayMs;
    m_nextTurn = 0;
    m_turnLog.clear();
    m_leftPending.clear();
    m_announcedDelay = 0;
    m_replayChecksums.clear();
    m_replayStartedAt = static_cast<std::int64_t>(std::time(nullptr));
    m_replayOpen = !m_config.replayDir.empty();
    logLine("match started on '" + m_lobby.map + "' with " + std::to_string(connectedCount()) + " players");
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, StartMatch {firstTurnDelayMs, m_setup});
    // On the lockstep channel: it must come before the first turn
    broadcast(Channel::Lockstep, m_buffer.data(), w.finish());
}

void SessionHost::adaptInputDelay()
{
    // Commands must reach the host before their turn closes: half the slowest round trip plus room
    // for jitter, in whole turns
    std::uint32_t worst = 0;
    for (const Connection &c : m_connections)
    {
        if (c.slot >= 0)
            worst = std::max(worst, m_transports[c.transport]->roundTripMs(c.peer));
    }
    const std::uint32_t needMs = worst / 2 + 30;
    const std::uint32_t turns = (needMs + m_config.turnMs - 1) / m_config.turnMs;
    m_inputDelay = static_cast<std::uint8_t>(std::clamp<std::uint32_t>(turns, m_config.inputDelayMin, m_config.inputDelayMax));
}

void SessionHost::closeTurn(std::uint32_t turn)
{
    TurnBundle bundle;
    bundle.turn = turn;
    bundle.left = m_leftPending;
    if (m_inputDelay != m_announcedDelay)
    {
        bundle.inputDelay = m_inputDelay;
        m_announcedDelay = m_inputDelay;
    }
    for (std::uint8_t s = 0; s < kMaxPlayers; ++s)
    {
        SlotState &slot = m_slots[s];
        if (!slot.used)
            continue;
        auto it = slot.commands.find(turn);
        if (it != slot.commands.end())
        {
            for (auto &batch : it->second)
                bundle.players.push_back({s, std::move(batch)});
        }
        // Nothing older may stay behind
        slot.commands.erase(slot.commands.begin(), slot.commands.upper_bound(turn));
    }
    // What does not fit into this turn (more batches than a bundle holds, or more bytes than a packet) moves
    // on to the next turn instead of being lost; the turn itself always goes out, or the clients would wait
    // for it forever. Batches keep their order: the carried ones go before anything already queued.
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> carry;
    while (bundle.players.size() > kMaxBatchesPerTurn)
    {
        carry.emplace_back(bundle.players.back().slot, std::move(bundle.players.back().bytes));
        bundle.players.pop_back();
    }
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, bundle);
    std::size_t size = w.finish();
    while (w.overflowed() && !bundle.players.empty())
    {
        carry.emplace_back(bundle.players.back().slot, std::move(bundle.players.back().bytes));
        bundle.players.pop_back();
        w = BitWriter(m_buffer.data(), m_buffer.size());
        encode(w, bundle);
        size = w.finish();
    }
    // `carry` holds the batches last-first: put them back first-first in front of the next turn's
    for (auto it = carry.begin(); it != carry.end(); ++it)
    {
        auto &next = m_slots[it->first].commands[turn + 1];
        next.insert(next.begin(), std::move(it->second));
    }
    if (w.overflowed())
        return;
    m_leftPending.clear();
    broadcast(Channel::Lockstep, m_buffer.data(), size);
    m_turnLog.emplace_back(m_buffer.data(), m_buffer.data() + size);
}

std::vector<SessionHost::Player> SessionHost::players() const
{
    std::vector<Player> out;
    for (std::uint8_t s = 0; s < kMaxPlayers; ++s)
    {
        const SlotState &slot = m_slots[s];
        if (!slot.used)
            continue;
        Player p {s, slot.name, slot.connected, 0};
        if (slot.connected && slot.connection < m_connections.size())
        {
            const Connection &c = m_connections[slot.connection];
            p.rttMs = m_transports[c.transport]->roundTripMs(c.peer);
        }
        out.push_back(p);
    }
    return out;
}

int SessionHost::connectedCount() const
{
    int n = 0;
    for (const SlotState &s : m_slots)
        n += (s.used && s.connected) ? 1 : 0;
    return n;
}

int SessionHost::ownerSlot() const
{
    for (int s = 0; s < kMaxPlayers; ++s)
    {
        if (m_slots[s].used && m_slots[s].connected)
            return s;
    }
    return -1;
}

RoomInfo SessionHost::roomInfo() const
{
    RoomInfo room;
    room.gameVersion = m_config.identity.gameVersion;
    room.name = m_config.roomName;
    room.map = m_lobby.map;
    room.players = static_cast<std::uint8_t>(m_running ? connectedCount() : playerCount());
    room.locked = !m_config.password.empty();
    room.maxPlayers = static_cast<std::uint8_t>(
        std::min<int>(m_config.maxPlayers, m_lobby.spawnCount ? m_lobby.spawnCount : m_config.maxPlayers));
    room.running = m_running;
    return room;
}

int SessionHost::playerCount() const
{
    int n = 0;
    for (const SlotState &s : m_slots)
        n += s.used ? 1 : 0;
    return n;
}

} // namespace OpenArtOfWar::Net
