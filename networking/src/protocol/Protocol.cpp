#include "openaow/net/Protocol.hpp"

#include <algorithm>

namespace OpenArtOfWar::Net {

namespace {

void writeType(BitWriter& w, MsgType t) { w.writeU8(static_cast<std::uint8_t>(t)); }

void writeToken(BitWriter& w, const SessionToken& t) { w.writeBytes(t.data(), t.size()); }

bool readToken(BitReader& r, SessionToken& t) {
    const std::uint8_t* p = r.readBytes(t.size());
    if (!p) return false;
    for (std::size_t i = 0; i < t.size(); ++i) t[i] = p[i];
    return true;
}

void writeBlob(BitWriter& w, const std::vector<std::uint8_t>& bytes) {
    w.writeVarUint(bytes.size());
    w.writeBytes(bytes.data(), bytes.size());
}

bool readBlob(BitReader& r, std::vector<std::uint8_t>& out, std::size_t maxSize) {
    const std::uint64_t n = r.readVarUint();
    if (n > maxSize) {
        r.fail();
        return false;
    }
    const std::uint8_t* p = r.readBytes(static_cast<std::size_t>(n));
    if (!p) return false;
    out.assign(p, p + n);
    return true;
}

// The whole payload consumed and nothing went wrong
bool done(const BitReader& r) { return !r.failed() && r.atEnd(); }

} // namespace

void encode(BitWriter& w, const Hello& m) {
    writeType(w, MsgType::Hello);
    w.writeU32(m.magic);
    w.writeU16(m.protocol);
    w.writeString(m.gameVersion, kMaxVersionLength);
    w.writeU64(m.simBuildId);
    w.writeU64(m.assetsHash);
    w.writeString(m.playerName, kMaxNameLength);
    writeToken(w, m.token);
    w.writeString(m.password, kMaxPasswordLength);
}

bool decode(BitReader& r, Hello& m) {
    m.magic = r.readU32();
    m.protocol = r.readU16();
    // A peer of another protocol may lay out the rest differently: stop here and let the caller reject it
    if (r.failed() || m.magic != kProtocolMagic) return false;
    if (m.protocol != kProtocolVersion) return true;
    m.gameVersion = r.readString(kMaxVersionLength);
    m.simBuildId = r.readU64();
    m.assetsHash = r.readU64();
    m.playerName = r.readString(kMaxNameLength);
    readToken(r, m.token);
    m.password = r.readString(kMaxPasswordLength);
    return done(r);
}

void encode(BitWriter& w, const Welcome& m) {
    writeType(w, MsgType::Welcome);
    w.writeRanged(m.slot, 0, kMaxPlayers - 1);
    w.alignToByte();
    writeToken(w, m.token);
    w.writeVarUint(m.turnMs);
    w.writeU8(m.ticksPerTurn);
}

bool decode(BitReader& r, Welcome& m) {
    m.slot = static_cast<std::uint8_t>(r.readRanged(0, kMaxPlayers - 1));
    r.alignToByte();
    readToken(r, m.token);
    const std::uint64_t turnMs = r.readVarUint();
    m.ticksPerTurn = r.readU8();
    if (turnMs < 10 || turnMs > 1000 || m.ticksPerTurn == 0) r.fail();
    m.turnMs = static_cast<std::uint16_t>(turnMs);
    return done(r);
}

void encode(BitWriter& w, const Reject& m) {
    writeType(w, MsgType::Reject);
    w.writeU8(static_cast<std::uint8_t>(m.reason));
    w.writeString(m.detail, kMaxReasonLength);
}

bool decode(BitReader& r, Reject& m) {
    const std::uint8_t reason = r.readU8();
    if (reason > static_cast<std::uint8_t>(RejectReason::TryAgain)) r.fail();
    m.reason = static_cast<RejectReason>(reason);
    m.detail = r.readString(kMaxReasonLength);
    return done(r);
}

void encode(BitWriter& w, const Chat& m) {
    writeType(w, MsgType::Chat);
    w.writeRanged(m.fromSlot, 0, kMaxPlayers - 1);
    w.writeBool(m.teamOnly);
    w.writeString(m.text, kMaxChatLength);
}

bool decode(BitReader& r, Chat& m) {
    m.fromSlot = static_cast<std::uint8_t>(r.readRanged(0, kMaxPlayers - 1));
    m.teamOnly = r.readBool();
    m.text = r.readString(kMaxChatLength);
    return done(r);
}

void encode(BitWriter& w, const Commands& m) {
    writeType(w, MsgType::Commands);
    w.writeVarUint(m.turn);
    writeBlob(w, m.bytes);
}

bool decode(BitReader& r, Commands& m) {
    const std::uint64_t turn = r.readVarUint();
    if (turn > 0xFFFFFFFFull) r.fail();
    m.turn = static_cast<std::uint32_t>(turn);
    readBlob(r, m.bytes, kMaxPlayerCommandBytes);
    return done(r);
}

void encode(BitWriter& w, const TurnBundle& m) {
    writeType(w, MsgType::TurnBundle);
    w.writeVarUint(m.turn);
    // Flag + count + slots share bytes: an empty turn is type, turn and one byte
    w.writeBool(m.inputDelay != 0);
    w.writeRanged(static_cast<std::int32_t>(std::min<std::size_t>(m.players.size(), kMaxBatchesPerTurn)), 0, kMaxBatchesPerTurn);
    for (std::size_t i = 0; i < m.players.size() && i < kMaxBatchesPerTurn; ++i) w.writeRanged(m.players[i].slot, 0, kMaxPlayers - 1);
    w.alignToByte();
    if (m.inputDelay != 0) w.writeU8(m.inputDelay);
    for (std::size_t i = 0; i < m.players.size() && i < kMaxBatchesPerTurn; ++i) writeBlob(w, m.players[i].bytes);
}

bool decode(BitReader& r, TurnBundle& m) {
    const std::uint64_t turn = r.readVarUint();
    if (turn > 0xFFFFFFFFull) r.fail();
    m.turn = static_cast<std::uint32_t>(turn);
    const bool hasDelay = r.readBool();
    const int count = r.readRanged(0, kMaxBatchesPerTurn);
    m.players.resize(r.failed() ? 0 : static_cast<std::size_t>(count));
    for (PlayerTurn& p : m.players) p.slot = static_cast<std::uint8_t>(r.readRanged(0, kMaxPlayers - 1));
    r.alignToByte();
    m.inputDelay = hasDelay ? r.readU8() : 0;
    if (hasDelay && m.inputDelay == 0) r.fail();
    for (PlayerTurn& p : m.players) {
        if (!readBlob(r, p.bytes, kMaxPlayerCommandBytes)) break;
    }
    return done(r);
}

void encode(BitWriter& w, const WorldHash& m) {
    writeType(w, MsgType::WorldHash);
    w.writeVarUint(m.tick);
    w.writeU64(m.hash);
}

bool decode(BitReader& r, WorldHash& m) {
    const std::uint64_t tick = r.readVarUint();
    if (tick > 0xFFFFFFFFull) r.fail();
    m.tick = static_cast<std::uint32_t>(tick);
    m.hash = r.readU64();
    return done(r);
}

void encode(BitWriter& w, const Leave&) {
    writeType(w, MsgType::Leave);
}

void encode(BitWriter& w, const Heartbeat& m) {
    writeType(w, MsgType::Heartbeat);
    w.writeU32(m.sendTimeMs);
    w.writeU32(m.echoTimeMs);
    w.writeVarUint(m.simTick);
}

bool decode(BitReader& r, Heartbeat& m) {
    m.sendTimeMs = r.readU32();
    m.echoTimeMs = r.readU32();
    const std::uint64_t tick = r.readVarUint();
    if (tick > 0xFFFFFFFFull) r.fail();
    m.simTick = static_cast<std::uint32_t>(tick);
    return done(r);
}

void encode(BitWriter& w, const Roster& m) {
    writeType(w, MsgType::Roster);
    w.writeRanged(static_cast<std::int32_t>(m.players.size()), 0, kMaxPlayers);
    for (const RosterEntry& e : m.players) {
        w.writeRanged(e.slot, 0, kMaxPlayers - 1);
        w.writeBool(e.connected);
        w.writeString(e.name, kMaxNameLength);
    }
}

bool decode(BitReader& r, Roster& m) {
    const int count = r.readRanged(0, kMaxPlayers);
    m.players.resize(r.failed() ? 0 : static_cast<std::size_t>(count));
    for (RosterEntry& e : m.players) {
        e.slot = static_cast<std::uint8_t>(r.readRanged(0, kMaxPlayers - 1));
        e.connected = r.readBool();
        e.name = r.readString(kMaxNameLength);
        if (r.failed()) break;
    }
    return done(r);
}

void encode(BitWriter& w, const StartMatch& m) {
    writeType(w, MsgType::StartMatch);
    w.writeVarUint(m.firstTurnDelayMs);
    writeBlob(w, m.setup);
}

bool decode(BitReader& r, StartMatch& m) {
    const std::uint64_t delay = r.readVarUint();
    if (delay > 60000) r.fail();
    m.firstTurnDelayMs = static_cast<std::uint32_t>(delay);
    readBlob(r, m.setup, kMaxMatchSetupBytes);
    return done(r);
}

void encode(BitWriter& w, const PauseRequest& m) {
    writeType(w, MsgType::PauseRequest);
    w.writeBool(m.pause);
}

bool decode(BitReader& r, PauseRequest& m) {
    m.pause = r.readBool();
    return done(r);
}

void encode(BitWriter& w, const PauseState& m) {
    writeType(w, MsgType::PauseState);
    w.writeBool(m.paused);
    w.writeRanged(m.bySlot, 0, kMaxPlayers - 1);
    w.writeBits(m.pausesLeft, 4);
}

bool decode(BitReader& r, PauseState& m) {
    m.paused = r.readBool();
    m.bySlot = static_cast<uint8_t>(r.readRanged(0, kMaxPlayers - 1));
    m.pausesLeft = static_cast<uint8_t>(r.readBits(4));
    return done(r);
}

void encode(BitWriter& w, const Desync& m) {
    writeType(w, MsgType::Desync);
    w.writeVarUint(m.tick);
}

bool decode(BitReader& r, Desync& m) {
    const std::uint64_t tick = r.readVarUint();
    if (tick > 0xFFFFFFFFull) r.fail();
    m.tick = static_cast<std::uint32_t>(tick);
    return done(r);
}

namespace {

void writeChoice(BitWriter& w, const LobbyChoice& c) {
    w.writeBits(c.faction, 4);
    w.writeBits(c.team, 3);
    w.writeBits(c.color, 4);
    w.writeBits(c.spawn, 4);
    w.writeBool(c.ready);
}

void readChoice(BitReader& r, LobbyChoice& c) {
    c.faction = static_cast<uint8_t>(r.readBits(4));
    c.team = static_cast<uint8_t>(r.readBits(3));
    c.color = static_cast<uint8_t>(r.readBits(4));
    c.spawn = static_cast<uint8_t>(r.readBits(4));
    c.ready = r.readBool();
    if (c.faction > 1 && c.faction != kRandom) r.fail();
    if (c.spawn >= kMaxPlayers && c.spawn != kRandom) r.fail();
}

} // namespace

void encode(BitWriter& w, const LobbyChoice& m) {
    writeType(w, MsgType::LobbyChoice);
    writeChoice(w, m);
}

bool decode(BitReader& r, LobbyChoice& m) {
    readChoice(r, m);
    return done(r);
}

void encode(BitWriter& w, const LobbyState& m) {
    writeType(w, MsgType::LobbyState);
    w.writeString(m.map, kMaxMapPathLength);
    w.writeU64(m.mapHash);
    w.writeRanged(m.spawnCount, 0, kMaxPlayers);
    w.writeRanged(static_cast<std::int32_t>(m.slots.size()), 0, kMaxPlayers);
    for (const LobbySlot& s : m.slots) {
        w.writeRanged(s.slot, 0, kMaxPlayers - 1);
        writeChoice(w, s.choice);
        w.writeBool(s.ai);
        if (s.ai) w.writeBits(s.aiLevel, 2);
    }
}

bool decode(BitReader& r, LobbyState& m) {
    m.map = r.readString(kMaxMapPathLength);
    m.mapHash = r.readU64();
    m.spawnCount = static_cast<uint8_t>(r.readRanged(0, kMaxPlayers));
    const int count = r.readRanged(0, kMaxPlayers);
    m.slots.resize(r.failed() ? 0 : static_cast<std::size_t>(count));
    for (LobbySlot& s : m.slots) {
        s.slot = static_cast<uint8_t>(r.readRanged(0, kMaxPlayers - 1));
        readChoice(r, s.choice);
        s.ai = r.readBool();
        s.aiLevel = s.ai ? static_cast<uint8_t>(r.readBits(2)) : 1;
        if (r.failed()) break;
    }
    return done(r);
}

void encode(BitWriter& w, const OwnerSetMap& m) {
    writeType(w, MsgType::OwnerSetMap);
    w.writeString(m.map, kMaxMapPathLength);
    w.writeU64(m.mapHash);
    w.writeRanged(m.spawnCount, 0, kMaxPlayers);
}

bool decode(BitReader& r, OwnerSetMap& m) {
    m.map = r.readString(kMaxMapPathLength);
    m.mapHash = r.readU64();
    m.spawnCount = static_cast<uint8_t>(r.readRanged(0, kMaxPlayers));
    return done(r);
}

void encode(BitWriter& w, const FileRequest& m) {
    writeType(w, MsgType::FileRequest);
    w.writeU64(m.hash);
}

bool decode(BitReader& r, FileRequest& m) {
    m.hash = r.readU64();
    return done(r);
}

void encode(BitWriter& w, const FileData& m) {
    writeType(w, MsgType::FileData);
    w.writeU64(m.hash);
    writeBlob(w, m.bytes);
}

bool decode(BitReader& r, FileData& m) {
    m.hash = r.readU64();
    readBlob(r, m.bytes, kMaxFileBytes);
    return done(r);
}

void encode(BitWriter& w, const OwnerSetAi& m) {
    writeType(w, MsgType::OwnerSetAi);
    w.writeBits(m.slot, 4);
    w.writeBool(m.remove);
    w.writeBits(m.level, 2);
    writeChoice(w, m.choice);
}

bool decode(BitReader& r, OwnerSetAi& m) {
    m.slot = static_cast<uint8_t>(r.readBits(4));
    if (m.slot >= kMaxPlayers && m.slot != kRandom) r.fail();
    m.remove = r.readBool();
    m.level = static_cast<uint8_t>(r.readBits(2));
    readChoice(r, m.choice);
    return done(r);
}

void encode(BitWriter& w, const OwnerStart& m) {
    writeType(w, MsgType::OwnerStart);
    w.writeVarUint(m.firstTurnDelayMs);
    writeBlob(w, m.setup);
}

bool decode(BitReader& r, OwnerStart& m) {
    const std::uint64_t delay = r.readVarUint();
    if (delay > 60000) r.fail();
    m.firstTurnDelayMs = static_cast<std::uint32_t>(delay);
    readBlob(r, m.setup, kMaxMatchSetupBytes);
    return done(r);
}

void encode(BitWriter& w, const ListRooms& m) {
    writeType(w, MsgType::ListRooms);
    w.writeString(m.gameVersion, kMaxVersionLength);
}

bool decode(BitReader& r, ListRooms& m) {
    m.gameVersion = r.readString(kMaxVersionLength);
    return done(r);
}

void encode(BitWriter& w, const RoomList& m) {
    writeType(w, MsgType::RoomList);
    const std::size_t n = std::min(m.rooms.size(), kMaxListedRooms);
    w.writeVarUint(n);
    for (std::size_t i = 0; i < n; ++i) {
        const RoomListing& e = m.rooms[i];
        w.writeString(e.code, kRoomCodeLength);
        w.writeString(e.name, kMaxNameLength);
        w.writeString(e.map, kMaxMapPathLength);
        w.writeRanged(e.players, 0, kMaxPlayers);
        w.writeRanged(e.maxPlayers, 0, kMaxPlayers);
        w.writeBool(e.running);
        w.writeU16(e.port);
        w.writeBool(e.hosted);
        w.writeBool(e.locked);
    }
}

bool decode(BitReader& r, RoomList& m) {
    const std::uint64_t n = r.readVarUint();
    if (n > kMaxListedRooms) {
        r.fail();
        return false;
    }
    m.rooms.resize(static_cast<std::size_t>(n));
    for (RoomListing& e : m.rooms) {
        e.code = r.readString(kRoomCodeLength);
        e.name = r.readString(kMaxNameLength);
        e.map = r.readString(kMaxMapPathLength);
        e.players = static_cast<uint8_t>(r.readRanged(0, kMaxPlayers));
        e.maxPlayers = static_cast<uint8_t>(r.readRanged(0, kMaxPlayers));
        e.running = r.readBool();
        e.port = r.readU16();
        e.hosted = r.readBool();
        e.locked = r.readBool();
        if (r.failed()) break;
    }
    return done(r);
}

void encode(BitWriter& w, const CreateRoom& m) {
    writeType(w, MsgType::CreateRoom);
    w.writeString(m.gameVersion, kMaxVersionLength);
    w.writeU64(m.simBuildId);
    w.writeU64(m.assetsHash);
    w.writeString(m.name, kMaxNameLength);
    w.writeBool(m.listed);
    w.writeString(m.password, kMaxPasswordLength);
}

bool decode(BitReader& r, CreateRoom& m) {
    m.gameVersion = r.readString(kMaxVersionLength);
    m.simBuildId = r.readU64();
    m.assetsHash = r.readU64();
    m.name = r.readString(kMaxNameLength);
    m.listed = r.readBool();
    m.password = r.readString(kMaxPasswordLength);
    return done(r);
}

void encode(BitWriter& w, const FindRoom& m) {
    writeType(w, MsgType::FindRoom);
    w.writeString(m.code, kRoomCodeLength);
}

bool decode(BitReader& r, FindRoom& m) {
    m.code = r.readString(kRoomCodeLength);
    return done(r);
}

void encode(BitWriter& w, const RoomAddress& m) {
    writeType(w, MsgType::RoomAddress);
    w.writeString(m.code, kRoomCodeLength);
    w.writeU16(m.port);
}

bool decode(BitReader& r, RoomAddress& m) {
    m.code = r.readString(kRoomCodeLength);
    m.port = r.readU16();
    return done(r);
}

namespace {
constexpr std::size_t kMaxIpLength = 46;
} // namespace

void encode(BitWriter& w, const RegisterHost& m) {
    writeType(w, MsgType::RegisterHost);
    w.writeString(m.gameVersion, kMaxVersionLength);
    w.writeString(m.name, kMaxNameLength);
    w.writeString(m.map, kMaxMapPathLength);
    w.writeRanged(m.players, 0, kMaxPlayers);
    w.writeRanged(m.maxPlayers, 0, kMaxPlayers);
    w.writeBool(m.listed);
    w.alignToByte();
    writeToken(w, m.token);
    w.writeString(m.lanIp, kMaxIpLength);
    w.writeU16(m.lanPort);
}

bool decode(BitReader& r, RegisterHost& m) {
    m.gameVersion = r.readString(kMaxVersionLength);
    m.name = r.readString(kMaxNameLength);
    m.map = r.readString(kMaxMapPathLength);
    m.players = static_cast<uint8_t>(r.readRanged(0, kMaxPlayers));
    m.maxPlayers = static_cast<uint8_t>(r.readRanged(0, kMaxPlayers));
    m.listed = r.readBool();
    r.alignToByte();
    readToken(r, m.token);
    m.lanIp = r.readString(kMaxIpLength);
    m.lanPort = r.readU16();
    return done(r);
}

void encode(BitWriter& w, const HostRegistered& m) {
    writeType(w, MsgType::HostRegistered);
    w.writeString(m.code, kRoomCodeLength);
}

bool decode(BitReader& r, HostRegistered& m) {
    m.code = r.readString(kRoomCodeLength);
    return done(r);
}

void encode(BitWriter& w, const JoinHosted& m) {
    writeType(w, MsgType::JoinHosted);
    w.writeString(m.code, kRoomCodeLength);
    writeToken(w, m.token);
}

bool decode(BitReader& r, JoinHosted& m) {
    m.code = r.readString(kRoomCodeLength);
    readToken(r, m.token);
    return done(r);
}

void encode(BitWriter& w, const HostedEndpoint& m) {
    writeType(w, MsgType::HostedEndpoint);
    w.writeString(m.ip, kMaxIpLength);
    w.writeU16(m.port);
    w.writeString(m.lanIp, kMaxIpLength);
    w.writeU16(m.lanPort);
    w.writeString(m.yourIp, kMaxIpLength);
}

bool decode(BitReader& r, HostedEndpoint& m) {
    m.ip = r.readString(kMaxIpLength);
    m.port = r.readU16();
    m.lanIp = r.readString(kMaxIpLength);
    m.lanPort = r.readU16();
    m.yourIp = r.readString(kMaxIpLength);
    return done(r);
}

void encode(BitWriter& w, const PunchRequest& m) {
    writeType(w, MsgType::PunchRequest);
    w.writeString(m.ip, kMaxIpLength);
    w.writeU16(m.port);
}

bool decode(BitReader& r, PunchRequest& m) {
    m.ip = r.readString(kMaxIpLength);
    m.port = r.readU16();
    return done(r);
}

void encode(BitWriter& w, const ServerInfo& m) {
    writeType(w, MsgType::ServerInfo);
    w.writeString(m.name, kMaxNameLength * 2);
    w.writeString(m.motd, kMaxReasonLength * 2);
}

bool decode(BitReader& r, ServerInfo& m) {
    m.name = r.readString(kMaxNameLength * 2);
    m.motd = r.readString(kMaxReasonLength * 2);
    return done(r);
}

const char* rejectReasonText(RejectReason reason) {
    switch (reason) {
        case RejectReason::None: return "";
        case RejectReason::ProtocolMismatch: return "Different network protocol";
        case RejectReason::VersionMismatch: return "Different game version";
        case RejectReason::AssetsMismatch: return "Different game data";
        case RejectReason::RoomFull: return "The room is full";
        case RejectReason::MatchRunning: return "The match has already started";
        case RejectReason::Banned: return "Not allowed to join";
        case RejectReason::BadPassword: return "Wrong password";
        case RejectReason::Malformed: return "Malformed message";
        case RejectReason::NotFound: return "No such room";
        case RejectReason::ServerFull: return "The server has no free room";
        case RejectReason::TryAgain: return "Not ready yet";
    }
    return "";
}

} // namespace OpenArtOfWar::Net
