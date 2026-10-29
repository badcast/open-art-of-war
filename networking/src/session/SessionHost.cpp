#include "openaow/net/Session.hpp"

#include <algorithm>
#include <random>

namespace OpenArtOfWar::Net {

namespace {

constexpr size_t kBufferSize = 16 * 1024;

SessionToken newToken() {
    static std::mt19937_64 rng{std::random_device{}()};
    SessionToken t{};
    for (size_t i = 0; i < t.size(); i += 8) {
        const uint64_t v = rng();
        for (size_t k = 0; k < 8 && i + k < t.size(); ++k) t[i + k] = static_cast<uint8_t>(v >> (8 * k));
    }
    return t;
}

bool isZero(const SessionToken& t) {
    return std::all_of(t.begin(), t.end(), [](uint8_t b) { return b == 0; });
}

} // namespace

SessionHost::SessionHost(HostConfig config) : m_config(std::move(config)), m_buffer(kBufferSize) {
    m_config.maxPlayers = std::clamp(m_config.maxPlayers, 1, kMaxPlayers);
    m_inputDelay = std::max<uint8_t>(m_config.inputDelayMin, 1);
}

SessionHost::~SessionHost() = default;

void SessionHost::addTransport(std::unique_ptr<Transport> transport) {
    if (transport) m_transports.push_back(std::move(transport));
}

void SessionHost::sendTo(size_t connection, Channel channel, const uint8_t* data, size_t size) {
    const Connection& c = m_connections[connection];
    m_transports[c.transport]->send(c.peer, channel, data, size);
}

void SessionHost::broadcast(Channel channel, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < m_connections.size(); ++i) {
        if (m_connections[i].slot >= 0) sendTo(i, channel, data, size);
    }
}

void SessionHost::reject(size_t connection, RejectReason reason, const std::string& detail) {
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, Reject{reason, detail});
    sendTo(connection, Channel::Control, m_buffer.data(), w.finish());
    const Connection& c = m_connections[connection];
    m_transports[c.transport]->disconnect(c.peer);
}

void SessionHost::broadcastRoster() {
    Roster roster;
    for (uint8_t s = 0; s < kMaxPlayers; ++s) {
        if (m_slots[s].used) roster.players.push_back({s, m_slots[s].name, m_slots[s].connected || m_slots[s].ai});
    }
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, roster);
    broadcast(Channel::Control, m_buffer.data(), w.finish());
}

void SessionHost::offerFile(uint64_t hash, std::vector<uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxFileBytes) return;
    m_files.erase(std::remove_if(m_files.begin(), m_files.end(), [&](const auto& f) { return f.first == hash; }), m_files.end());
    m_files.emplace_back(hash, std::move(bytes));
    auto total = [&] {
        size_t n = 0;
        for (const auto& f : m_files) n += f.second.size();
        return n;
    };
    while (m_files.size() > kMaxStoredFiles || (m_files.size() > 1 && total() > kMaxStoredBytes)) m_files.erase(m_files.begin());
}

void SessionHost::setLobbyMap(const std::string& map, uint8_t spawnCount, uint64_t mapHash) {
    if (m_running) return;
    // A map with fewer start positions than there are players in the room is not taken
    int inRoom = 0;
    for (const SlotState& s : m_slots) inRoom += s.used ? 1 : 0;
    if (spawnCount > 0 && inRoom > spawnCount) return;
    m_lobby.map = map;
    m_lobby.mapHash = mapHash;
    m_lobby.spawnCount = std::min<uint8_t>(spawnCount, kMaxPlayers);
    // Another map: nobody is ready for it yet, start positions beyond it are gone
    for (SlotState& s : m_slots) {
        s.choice.ready = s.ai;   // computer players are always ready
        if (s.choice.spawn != kRandom && s.choice.spawn >= m_lobby.spawnCount) s.choice.spawn = kRandom;
    }
    broadcastLobby();
}

void SessionHost::applyChoice(uint8_t slot, LobbyChoice choice) {
    if (m_running || slot >= kMaxPlayers || !m_slots[slot].used) return;
    // A start position somebody else holds (or the map does not have) stays as it was
    if (choice.spawn != kRandom) {
        bool taken = choice.spawn >= m_lobby.spawnCount;
        for (uint8_t s = 0; s < kMaxPlayers; ++s) {
            if (s != slot && m_slots[s].used && m_slots[s].choice.spawn == choice.spawn) taken = true;
        }
        if (taken) choice.spawn = m_slots[slot].choice.spawn;
    }
    choice.team &= 7;
    choice.color &= 15;
    m_slots[slot].choice = choice;
    broadcastLobby();
}

void SessionHost::setAiSlot(const OwnerSetAi& change) {
    if (m_running) return;
    uint8_t slot = change.slot;
    if (slot == kRandom) {
        // A new computer player: the first free slot the map has a start position for
        int used = 0;
        for (const SlotState& s : m_slots) used += s.used ? 1 : 0;
        if (used >= std::min<int>(m_config.maxPlayers, m_lobby.spawnCount ? m_lobby.spawnCount : m_config.maxPlayers)) return;
        for (uint8_t s = 0; s < m_config.maxPlayers && slot == kRandom; ++s) {
            if (!m_slots[s].used) slot = s;
        }
        if (slot == kRandom) return;
        m_slots[slot] = SlotState{};
        m_slots[slot].used = true;
        m_slots[slot].ai = true;
        m_slots[slot].name = "AI";
    }
    if (slot >= kMaxPlayers || !m_slots[slot].used || !m_slots[slot].ai) return;
    if (change.remove) {
        m_slots[slot] = SlotState{};
    } else {
        m_slots[slot].aiLevel = change.level & 3;
        // A computer player never waits: always ready; a start position it may not take is left to chance
        LobbyChoice choice = change.choice;
        choice.ready = true;
        if (change.slot == kRandom) {
            choice.team = static_cast<uint8_t>(slot % 2);
            choice.color = static_cast<uint8_t>(slot + 1);
        }
        m_slots[slot].choice.ready = true;
        applyChoice(slot, choice);
        return;   // applyChoice broadcasts
    }
    broadcastRoster();
    broadcastLobby();
}

void SessionHost::broadcastLobby() {
    m_lobby.slots.clear();
    for (uint8_t s = 0; s < kMaxPlayers; ++s) {
        if (m_slots[s].used) m_lobby.slots.push_back({s, m_slots[s].choice, m_slots[s].ai, m_slots[s].aiLevel});
    }
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, m_lobby);
    broadcast(Channel::Control, m_buffer.data(), w.finish());
}

bool SessionHost::everyoneReady(int minPlayers) const {
    int n = 0;
    for (const SlotState& s : m_slots) {
        if (s.used && s.ai) {
            ++n;
            continue;
        }
        if (!s.used || !s.connected) continue;
        if (!s.choice.ready) return false;
        ++n;
    }
    return n >= minPlayers;
}

void SessionHost::update(uint32_t nowMs) {
    for (size_t t = 0; t < m_transports.size(); ++t) {
        m_events.clear();
        m_transports[t]->poll(m_events);
        for (const TransportEvent& e : m_events) {
            // The connection this event belongs to (a new one for a new peer)
            size_t index = m_connections.size();
            for (size_t i = 0; i < m_connections.size(); ++i) {
                if (m_connections[i].transport == t && m_connections[i].peer == e.peer) {
                    index = i;
                    break;
                }
            }
            if (index == m_connections.size()) {
                if (e.type != TransportEvent::Type::Connected) continue;
                m_connections.push_back({t, e.peer, -1});
            }
            handle(index, e, nowMs);
        }
    }
    // Drop the connections that are gone (their slots stay for a reconnect)
    m_connections.erase(std::remove_if(m_connections.begin(), m_connections.end(),
                                       [](const Connection& c) { return c.slot == -2; }),
                        m_connections.end());
    for (uint8_t s = 0; s < kMaxPlayers; ++s) {
        SlotState& slot = m_slots[s];
        if (!slot.used || !slot.connected) continue;
        slot.connection = m_connections.size();
        for (size_t i = 0; i < m_connections.size(); ++i) {
            if (m_connections[i].slot == s) slot.connection = i;
        }
    }

    // A running match: players who have been gone longer than the reconnect window are given up; with nobody left the
    // match is over and the room is an empty lobby again (no zombie battle nobody plays)
    if (m_running) {
        bool changed = false;
        int humans = 0;
        for (SlotState& slot : m_slots) {
            if (!slot.used || slot.ai) continue;
            if (!slot.connected && nowMs - slot.goneMs > m_config.reconnectWindowMs) {
                slot = SlotState{};
                changed = true;
            } else {
                ++humans;
            }
        }
        if (humans == 0) {
            for (SlotState& slot : m_slots) slot = SlotState{};
            m_running = false;
            m_paused = false;
            m_nextTurn = 0;
            m_turnLog.clear();
            changed = true;
        }
        if (changed) {
            broadcastRoster();
            broadcastLobby();
        }
    }

    // What local network browsers see of the room
    const RoomInfo room = roomInfo();
    for (auto& t : m_transports) t->setRoomInfo(room);

    // A pause ends by itself after a while
    if (m_paused && nowMs - m_pauseStartMs > m_config.maxPauseMs) setPaused(false, m_pausedBy, nowMs);
    // Close every turn that is due by the host's clock
    while (m_running && !m_paused && static_cast<int64_t>(nowMs) - static_cast<int64_t>(m_startMs) >=
                            static_cast<int64_t>(m_nextTurn) * m_config.turnMs) {
        adaptInputDelay();
        closeTurn(m_nextTurn++);
    }
}

void SessionHost::handle(size_t connection, const TransportEvent& e, uint32_t nowMs) {
    Connection& c = m_connections[connection];
    if (e.type == TransportEvent::Type::Connected) return;   // waiting for its Hello
    if (e.type == TransportEvent::Type::Disconnected) {
        if (c.slot >= 0) {
            m_slots[c.slot].connected = false;
            m_slots[c.slot].goneMs = nowMs;
            // In the room the slot is free again; in a match it waits for the player to come back (for a while)
            if (!m_running) m_slots[c.slot] = SlotState{};
            c.slot = -2;
            broadcastRoster();
            broadcastLobby();
        } else {
            c.slot = -2;
        }
        return;
    }
    BitReader r(e.data.data(), e.data.size());
    const MsgType type = readType(r);
    if (c.slot < 0) {
        Hello hello;
        if (type != MsgType::Hello || !decode(r, hello)) {
            reject(connection, RejectReason::Malformed, "expected a hello");
            return;
        }
        handleHello(connection, hello);
        return;
    }
    const uint8_t slot = static_cast<uint8_t>(c.slot);
    switch (type) {
        case MsgType::Commands: {
            Commands m;
            if (!decode(r, m) || !m_running) return;
            // Too late for its turn: the next open one (nobody waits for a slow peer)
            const uint32_t turn = std::max(m.turn, m_nextTurn);
            if (turn > m_nextTurn + 64) return;   // nonsense far in the future
            // A late batch can land on a turn that already has one: the bundle carries one entry per batch
            if (!m.bytes.empty()) m_slots[slot].commands[turn].push_back(std::move(m.bytes));
            break;
        }
        case MsgType::WorldHash: {
            WorldHash m;
            if (!decode(r, m) || m_desynced) return;
            auto& byTick = m_hashes[m.tick];
            byTick[slot] = m.hash;
            int expected = 0;
            for (const SlotState& s : m_slots) expected += (s.used && s.connected) ? 1 : 0;
            if (static_cast<int>(byTick.size()) >= expected) {
                bool same = true;
                for (const auto& [s, h] : byTick) same = same && h == byTick.begin()->second;
                if (!same) {
                    m_desynced = true;
                    m_desyncTick = m.tick;
                    BitWriter w(m_buffer.data(), m_buffer.size());
                    encode(w, Desync{m.tick});
                    broadcast(Channel::Lockstep, m_buffer.data(), w.finish());
                }
                m_hashes.erase(m_hashes.begin(), m_hashes.upper_bound(m.tick));
            }
            break;
        }
        case MsgType::OwnerSetMap: {
            OwnerSetMap m;
            if (m_config.ownerControls && slot == ownerSlot() && decode(r, m)) setLobbyMap(m.map, m.spawnCount, m.mapHash);
            break;
        }
        case MsgType::OwnerSetAi: {
            OwnerSetAi m;
            if (m_config.ownerControls && slot == ownerSlot() && decode(r, m)) setAiSlot(m);
            break;
        }
        case MsgType::OwnerStart: {
            OwnerStart m;
            if (m_config.ownerControls && slot == ownerSlot() && decode(r, m) && everyoneReady(2)) {
                startMatch(std::move(m.setup), nowMs, m.firstTurnDelayMs);
            }
            break;
        }
        case MsgType::FileRequest: {
            FileRequest m;
            if (!decode(r, m)) return;
            FileData answer;
            answer.hash = m.hash;
            for (const auto& f : m_files) {
                if (f.first == m.hash) answer.bytes = f.second;
            }
            std::vector<uint8_t> big(answer.bytes.size() + 64);
            BitWriter w(big.data(), big.size());
            encode(w, answer);
            // The bulk channel: a big file never holds up turns or the room
            sendTo(connection, Channel::Bulk, big.data(), w.finish());
            break;
        }
        case MsgType::FileData: {
            FileData m;
            if (m_config.ownerControls && slot == ownerSlot() && decode(r, m)) offerFile(m.hash, std::move(m.bytes));
            break;
        }
        case MsgType::PauseRequest: {
            PauseRequest m;
            if (!decode(r, m) || !m_running) return;
            if (m.pause && !m_paused && m_slots[slot].pausesLeft > 0) {
                --m_slots[slot].pausesLeft;
                setPaused(true, slot, nowMs);
            } else if (!m.pause && m_paused) {
                setPaused(false, slot, nowMs);
            }
            break;
        }
        case MsgType::LobbyChoice: {
            LobbyChoice m;
            if (decode(r, m)) applyChoice(slot, m);
            break;
        }
        case MsgType::Leave: {
            // The player goes for good: the slot is free at once, in the room and in a match alike
            m_slots[slot] = SlotState{};
            c.slot = -2;
            broadcastRoster();
            broadcastLobby();
            break;
        }
        case MsgType::Chat: {
            Chat m;
            if (!decode(r, m)) return;
            m.fromSlot = slot;   // never trusted from the sender
            BitWriter w(m_buffer.data(), m_buffer.size());
            encode(w, m);
            broadcast(Channel::Control, m_buffer.data(), w.finish());
            break;
        }
        default:
            break;
    }
}

void SessionHost::handleHello(size_t connection, const Hello& hello) {
    if (hello.protocol != kProtocolVersion) {
        reject(connection, RejectReason::ProtocolMismatch, "");
        return;
    }
    if (hello.gameVersion != m_config.identity.gameVersion || hello.simBuildId != m_config.identity.simBuildId) {
        reject(connection, RejectReason::VersionMismatch, m_config.identity.gameVersion);
        return;
    }
    if (hello.assetsHash != m_config.identity.assetsHash) {
        reject(connection, RejectReason::AssetsMismatch, "");
        return;
    }
    // Coming back after a drop: the same slot
    int slot = -1;
    if (!isZero(hello.token)) {
        for (int s = 0; s < kMaxPlayers; ++s) {
            if (m_slots[s].used && !m_slots[s].connected && m_slots[s].token == hello.token) slot = s;
        }
    }
    const bool resumed = slot >= 0;
    if (!resumed) {
        if (!m_config.password.empty() && hello.password != m_config.password) {
            reject(connection, RejectReason::BadPassword, "");
            return;
        }
        if (m_running) {
            reject(connection, RejectReason::MatchRunning, "");
            return;
        }
        // The room holds as many players as the map has start positions
        int taken = 0;
        for (const SlotState& other : m_slots) taken += other.used ? 1 : 0;
        const int capacity = (!m_lobby.map.empty() && m_lobby.spawnCount) ? std::min<int>(m_config.maxPlayers, m_lobby.spawnCount)
                                                                         : m_config.maxPlayers; // no map yet: no limit
        for (int s = 0; s < m_config.maxPlayers && slot < 0 && taken < capacity; ++s) {
            if (!m_slots[s].used) slot = s;
        }
        if (slot < 0) {
            reject(connection, RejectReason::RoomFull, "");
            return;
        }
        m_slots[slot] = SlotState{};
        m_slots[slot].used = true;
        m_slots[slot].token = newToken();
        // Defaults: random faction and start, alternating teams, an own colour
        m_slots[slot].choice.team = static_cast<uint8_t>(slot % 2);
        m_slots[slot].choice.color = static_cast<uint8_t>(slot + 1);
    }
    SlotState& s = m_slots[slot];
    s.connected = true;
    s.name = hello.playerName.empty() ? "Player " + std::to_string(slot + 1) : hello.playerName;
    m_connections[connection].slot = slot;

    Welcome welcome;
    welcome.slot = static_cast<uint8_t>(slot);
    welcome.token = s.token;
    welcome.turnMs = m_config.turnMs;
    welcome.ticksPerTurn = m_config.ticksPerTurn;
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, welcome);
    sendTo(connection, Channel::Control, m_buffer.data(), w.finish());
    broadcastRoster();
    broadcastLobby();

    if (resumed && m_running) {
        // Back into a running match: the setup and every turn so far, the game plays them through
        BitWriter sw(m_buffer.data(), m_buffer.size());
        encode(sw, StartMatch{0, m_setup});
        sendTo(connection, Channel::Lockstep, m_buffer.data(), sw.finish());
        for (const std::vector<uint8_t>& bundle : m_turnLog) sendTo(connection, Channel::Lockstep, bundle.data(), bundle.size());
    }
}

void SessionHost::setPaused(bool paused, uint8_t bySlot, uint32_t nowMs) {
    if (!m_running || paused == m_paused) return;
    if (paused) {
        m_pauseStartMs = nowMs;
        m_pausedBy = bySlot;
    } else {
        // The turn clock goes on where it stopped: no burst of turns after the pause
        m_startMs += nowMs - m_pauseStartMs;
    }
    m_paused = paused;
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, PauseState{m_paused, bySlot, m_slots[bySlot].pausesLeft});
    broadcast(Channel::Lockstep, m_buffer.data(), w.finish());
}

void SessionHost::startMatch(std::vector<uint8_t> setup, uint32_t nowMs, uint32_t firstTurnDelayMs) {
    if (m_running) return;
    m_setup = std::move(setup);
    m_running = true;
    m_paused = false;
    for (SlotState& s : m_slots) s.pausesLeft = m_config.pausesPerPlayer;
    m_startMs = nowMs + firstTurnDelayMs;
    m_nextTurn = 0;
    m_turnLog.clear();
    m_announcedDelay = 0;
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, StartMatch{firstTurnDelayMs, m_setup});
    // On the lockstep channel: it must come before the first turn
    broadcast(Channel::Lockstep, m_buffer.data(), w.finish());
}

void SessionHost::adaptInputDelay() {
    // Commands must reach the host before their turn closes: half the slowest round trip plus room
    // for jitter, in whole turns
    uint32_t worst = 0;
    for (const Connection& c : m_connections) {
        if (c.slot >= 0) worst = std::max(worst, m_transports[c.transport]->roundTripMs(c.peer));
    }
    const uint32_t needMs = worst / 2 + 30;
    const uint32_t turns = (needMs + m_config.turnMs - 1) / m_config.turnMs;
    m_inputDelay = static_cast<uint8_t>(std::clamp<uint32_t>(turns, m_config.inputDelayMin, m_config.inputDelayMax));
}

void SessionHost::closeTurn(uint32_t turn) {
    TurnBundle bundle;
    bundle.turn = turn;
    if (m_inputDelay != m_announcedDelay) {
        bundle.inputDelay = m_inputDelay;
        m_announcedDelay = m_inputDelay;
    }
    for (uint8_t s = 0; s < kMaxPlayers; ++s) {
        SlotState& slot = m_slots[s];
        if (!slot.used) continue;
        auto it = slot.commands.find(turn);
        if (it != slot.commands.end()) {
            for (auto& batch : it->second) bundle.players.push_back({s, std::move(batch)});
        }
        // Nothing older may stay behind
        slot.commands.erase(slot.commands.begin(), slot.commands.upper_bound(turn));
    }
    BitWriter w(m_buffer.data(), m_buffer.size());
    encode(w, bundle);
    const size_t size = w.finish();
    if (w.overflowed()) return;
    broadcast(Channel::Lockstep, m_buffer.data(), size);
    m_turnLog.emplace_back(m_buffer.data(), m_buffer.data() + size);
}

std::vector<SessionHost::Player> SessionHost::players() const {
    std::vector<Player> out;
    for (uint8_t s = 0; s < kMaxPlayers; ++s) {
        const SlotState& slot = m_slots[s];
        if (!slot.used) continue;
        Player p{s, slot.name, slot.connected, 0};
        if (slot.connected && slot.connection < m_connections.size()) {
            const Connection& c = m_connections[slot.connection];
            p.rttMs = m_transports[c.transport]->roundTripMs(c.peer);
        }
        out.push_back(p);
    }
    return out;
}

int SessionHost::connectedCount() const {
    int n = 0;
    for (const SlotState& s : m_slots) n += (s.used && s.connected) ? 1 : 0;
    return n;
}

int SessionHost::ownerSlot() const {
    for (int s = 0; s < kMaxPlayers; ++s) {
        if (m_slots[s].used && m_slots[s].connected) return s;
    }
    return -1;
}

RoomInfo SessionHost::roomInfo() const {
    RoomInfo room;
    room.gameVersion = m_config.identity.gameVersion;
    room.name = m_config.roomName;
    room.map = m_lobby.map;
    room.players = static_cast<uint8_t>(m_running ? connectedCount() : playerCount());
    room.locked = !m_config.password.empty();
    room.maxPlayers = static_cast<uint8_t>(std::min<int>(m_config.maxPlayers, m_lobby.spawnCount ? m_lobby.spawnCount : m_config.maxPlayers));
    room.running = m_running;
    return room;
}

int SessionHost::playerCount() const {
    int n = 0;
    for (const SlotState& s : m_slots) n += s.used ? 1 : 0;
    return n;
}

} // namespace OpenArtOfWar::Net
