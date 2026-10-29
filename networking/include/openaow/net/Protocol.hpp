#pragma once

#include "openaow/net/BitStream.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Wire format of OpenAOWNetworking. One ENet packet carries one message: [MsgType : 8 bits][payload].
// Datagrams are checked by CRC-32 (ENet checksum hook) and range-coder compressed by ENet; payloads are
// bit-packed (see BitStream.hpp). The module knows nothing about the game: player commands are opaque
// bytes that the game encodes and decodes itself.
namespace OpenArtOfWar::Net {

constexpr std::uint16_t kDefaultPort = 38100;
constexpr std::uint32_t kProtocolMagic = 0x574F414Fu;   // "OAOW"
constexpr std::uint16_t kProtocolVersion = 2;

constexpr int kMaxPlayers = 8;
constexpr std::size_t kMaxNameLength = 24;
constexpr std::size_t kMaxPasswordLength = 24;
constexpr std::size_t kMaxVersionLength = 32;
constexpr std::size_t kMaxReasonLength = 120;
constexpr std::size_t kMaxChatLength = 200;
constexpr std::size_t kMaxPlayerCommandBytes = 1024;   // commands of one player for one turn
constexpr std::size_t kMaxBatchesPerTurn = 15;          // entries of one turn bundle
// A message that fits into one UDP datagram on any path (IPv4 minimum reassembly 576, typical MTU 1500
// minus IP/UDP/ENet headers); bigger ones are fragmented by ENet on the bulk channel only
constexpr std::size_t kMaxDatagramPayload = 1200;

// ENet channels: separate ordering streams, so a long chat line or a map transfer never holds up a turn
enum class Channel : std::uint8_t {
    Control = 0,    // reliable: handshake, lobby, chat
    Lockstep = 1,   // reliable: commands up, turn bundles down, world hashes
    Realtime = 2,   // unreliable sequenced: heartbeat / clock, map pings, ally cursors, telemetry
    Bulk = 3,       // reliable: map files, command log for a reconnect
    Count = 4
};

enum class MsgType : std::uint8_t {
    // Control
    Hello = 1,
    Welcome = 2,
    Reject = 3,
    Chat = 4,
    Roster = 5,        // host -> clients: who is in the room
    StartMatch = 6,    // host -> clients: the match setup (opaque, the game's own format)
    LobbyChoice = 7,   // client -> host: what the player picked in the room
    LobbyState = 8,    // host -> clients: the room (map, everybody's picks)
    FileRequest = 9,   // client -> host: a file by its hash (a map the player does not have)
    Leave = 11,        // client -> host: the player leaves for good (the slot is not kept for a reconnect)
    FileData = 10,     // host -> client: the file (compressed by the game; empty: not there);
                       // owner -> dedicated room: a file the room offers from now on
    // Lockstep
    Commands = 16,     // client -> host: the local player's commands for a turn
    TurnBundle = 17,   // host -> clients: everybody's commands of a closed turn
    WorldHash = 18,    // client -> host: checksum of the world after a tick
    Desync = 19,       // host -> clients: the checksums of a tick differ
    PauseRequest = 20, // client -> host: pause / go on
    PauseState = 21,   // host -> clients: paused or not, by whom
    // Room owner (dedicated server rooms: the first player decides what the host would)
    OwnerSetMap = 24,  // client -> room: the map and its start position count
    OwnerStart = 25,   // client -> room: the resolved match setup to start with
    OwnerSetAi = 26,   // client -> room: add, change or remove a computer player
    // Realtime
    Heartbeat = 32,
    // Master server (openaow-server's main port)
    ListRooms = 40,    // client -> master
    RoomList = 41,     // master -> client
    CreateRoom = 42,   // client -> master: a dedicated room for this game version
    FindRoom = 43,     // client -> master: by code
    RoomAddress = 44,  // master -> client: where the room is (code, port)
    ServerInfo = 45,   // master -> client on connect: name, message of the day
    RegisterHost = 46, // listen host -> master: list my game (again every few seconds)
    HostRegistered = 47, // master -> listen host: its code
    JoinHosted = 48,   // client -> master: connect me to the player-hosted game with this code
    HostedEndpoint = 49, // master -> client: where the host is (public and local network address)
    PunchRequest = 50, // master -> listen host: a player at this address is coming, punch towards it
};

enum class RejectReason : std::uint8_t {
    None = 0,
    ProtocolMismatch,
    VersionMismatch,
    AssetsMismatch,
    RoomFull,
    MatchRunning,
    Banned,
    BadPassword,
    Malformed,
    NotFound,
    ServerFull,
    TryAgain,          // the server does not know a socket address yet: ask again in a moment
};

using SessionToken = std::array<std::uint8_t, 16>;

// First message of a client. A non-zero token asks for the slot it held before a disconnect.
struct Hello {
    std::uint32_t magic{kProtocolMagic};
    std::uint16_t protocol{kProtocolVersion};
    std::string gameVersion;        // OAOW_VERSION
    std::uint64_t simBuildId{0};    // the simulation code must be the very same build
    std::uint64_t assetsHash{0};    // the game data the simulation reads
    std::string playerName;
    SessionToken token{};
    std::string password;           // of a room that has one
};

struct Welcome {
    std::uint8_t slot{0};
    SessionToken token{};           // keep it to come back after a disconnect
    std::uint16_t turnMs{100};
    std::uint8_t ticksPerTurn{2};
};

struct Reject {
    RejectReason reason{RejectReason::None};
    std::string detail;
};

struct Chat {
    std::uint8_t fromSlot{0};       // set by the host from the connection, never trusted from a client
    bool teamOnly{false};
    std::string text;
};

struct Commands {
    std::uint32_t turn{0};
    std::vector<std::uint8_t> bytes;
};

struct PlayerTurn {
    std::uint8_t slot{0};
    std::vector<std::uint8_t> bytes;
};

// Turn `turn` with every player's commands (a player may have more than one batch, applied in this
// order); turns without any cost 4 bytes
struct TurnBundle {
    std::uint32_t turn{0};
    std::uint8_t inputDelay{0};     // turns between issuing and executing, from this turn on (0 = unchanged)
    std::vector<PlayerTurn> players;
};

struct RosterEntry {
    uint8_t slot{0};
    std::string name;
    bool connected{true};
};

struct Roster {
    std::vector<RosterEntry> players;
};

constexpr size_t kMaxMatchSetupBytes = 4096;

struct StartMatch {
    uint32_t firstTurnDelayMs{3000};   // countdown before turn 0
    std::vector<uint8_t> setup;
};

// Picks of a player in the room. Faction and start position may be left to chance (kRandom): the host
// resolves them with the match seed when it starts the match.
constexpr uint8_t kRandom = 0x0F;
constexpr std::size_t kMaxMapPathLength = 128;

struct LobbyChoice {
    uint8_t faction{kRandom};   // 0 / 1 (the game's factions) or kRandom
    uint8_t team{0};            // 0..7
    uint8_t color{0};           // 0..15
    uint8_t spawn{kRandom};     // start position 0..7 or kRandom
    bool ready{false};
};

struct LobbySlot {
    uint8_t slot{0};
    LobbyChoice choice;
    bool ai{false};             // a computer player (no connection; always ready)
    uint8_t aiLevel{1};         // 0..3 (the game's difficulties)
};

// The host / room owner adds (slot kRandom: the first free one), changes or removes a computer player
struct OwnerSetAi {
    uint8_t slot{kRandom};
    bool remove{false};
    uint8_t level{1};
    LobbyChoice choice;
};

struct LobbyState {
    std::string map;            // the game's map path
    uint64_t mapHash{0};        // of the map file: everybody must have exactly this one
    uint8_t spawnCount{2};      // start positions of the map
    std::vector<LobbySlot> slots;
};

struct OwnerSetMap {
    std::string map;
    uint64_t mapHash{0};
    uint8_t spawnCount{2};
};

constexpr std::size_t kMaxFileBytes = 2 * 1024 * 1024;

struct FileRequest {
    uint64_t hash{0};
};

struct FileData {
    uint64_t hash{0};
    std::vector<uint8_t> bytes;
};

struct OwnerStart {
    uint32_t firstTurnDelayMs{3000};
    std::vector<uint8_t> setup;
};

constexpr std::size_t kRoomCodeLength = 6;
constexpr std::size_t kMaxListedRooms = 64;

struct RoomListing {
    std::string code;
    std::string name;
    std::string map;
    uint8_t players{0};
    uint8_t maxPlayers{0};
    bool running{false};
    uint16_t port{0};
    bool hosted{false};        // hosted by a player (join with JoinHosted), not a room of the server
    bool locked{false};        // the room asks for a password
};

using SocketToken = std::array<uint8_t, 16>;

struct RegisterHost {
    std::string gameVersion;
    std::string name;
    std::string map;
    uint8_t players{0};
    uint8_t maxPlayers{0};
    bool listed{true};
    SocketToken token{};       // the token its game socket says hello with
    std::string lanIp;
    uint16_t lanPort{0};
};

struct HostRegistered {
    std::string code;
};

struct JoinHosted {
    std::string code;
    SocketToken token{};       // of the joining player's game socket
};

struct HostedEndpoint {
    std::string ip;
    uint16_t port{0};
    std::string lanIp;
    uint16_t lanPort{0};
    std::string yourIp;        // the joining player's own public address (the same as ip: one network)
};

struct PunchRequest {
    std::string ip;
    uint16_t port{0};
};

struct ListRooms {
    std::string gameVersion;   // only rooms of this version
};

struct RoomList {
    std::vector<RoomListing> rooms;
};

struct CreateRoom {
    std::string gameVersion;
    uint64_t simBuildId{0};
    uint64_t assetsHash{0};
    std::string name;
    bool listed{true};         // in the public list (else only by code)
    std::string password;      // empty: open to everybody
};

struct FindRoom {
    std::string code;
};

struct RoomAddress {
    std::string code;
    uint16_t port{0};
};

struct ServerInfo {
    std::string name;
    std::string motd;
};

struct PauseRequest {
    bool pause{true};
};

struct PauseState {
    bool paused{false};
    uint8_t bySlot{0};
    uint8_t pausesLeft{0};   // of that player
};

struct Desync {
    uint32_t tick{0};
};

struct WorldHash {
    std::uint32_t tick{0};
    std::uint64_t hash{0};
};

struct Leave {};

struct Heartbeat {
    std::uint32_t sendTimeMs{0};    // echoed back for the round trip time
    std::uint32_t echoTimeMs{0};
    std::uint32_t simTick{0};       // where the sender's simulation is
};

// Encoders write the message type first; decoders expect it already read (see peekType)
void encode(BitWriter& w, const Hello& m);
void encode(BitWriter& w, const Welcome& m);
void encode(BitWriter& w, const Reject& m);
void encode(BitWriter& w, const Chat& m);
void encode(BitWriter& w, const Commands& m);
void encode(BitWriter& w, const TurnBundle& m);
void encode(BitWriter& w, const WorldHash& m);
void encode(BitWriter& w, const Heartbeat& m);
void encode(BitWriter& w, const Leave& m);
void encode(BitWriter& w, const Roster& m);
void encode(BitWriter& w, const StartMatch& m);
void encode(BitWriter& w, const Desync& m);
void encode(BitWriter& w, const PauseRequest& m);
void encode(BitWriter& w, const PauseState& m);
void encode(BitWriter& w, const LobbyChoice& m);
void encode(BitWriter& w, const LobbyState& m);
void encode(BitWriter& w, const OwnerSetMap& m);
void encode(BitWriter& w, const FileRequest& m);
void encode(BitWriter& w, const FileData& m);
void encode(BitWriter& w, const OwnerStart& m);
void encode(BitWriter& w, const OwnerSetAi& m);
void encode(BitWriter& w, const ListRooms& m);
void encode(BitWriter& w, const RoomList& m);
void encode(BitWriter& w, const CreateRoom& m);
void encode(BitWriter& w, const FindRoom& m);
void encode(BitWriter& w, const RoomAddress& m);
void encode(BitWriter& w, const ServerInfo& m);
void encode(BitWriter& w, const RegisterHost& m);
void encode(BitWriter& w, const HostRegistered& m);
void encode(BitWriter& w, const JoinHosted& m);
void encode(BitWriter& w, const HostedEndpoint& m);
void encode(BitWriter& w, const PunchRequest& m);

// Each returns false (and leaves `m` unspecified) for a truncated, oversized or otherwise invalid payload
bool decode(BitReader& r, Hello& m);
bool decode(BitReader& r, Welcome& m);
bool decode(BitReader& r, Reject& m);
bool decode(BitReader& r, Chat& m);
bool decode(BitReader& r, Commands& m);
bool decode(BitReader& r, TurnBundle& m);
bool decode(BitReader& r, WorldHash& m);
bool decode(BitReader& r, Heartbeat& m);
bool decode(BitReader& r, Roster& m);
bool decode(BitReader& r, StartMatch& m);
bool decode(BitReader& r, Desync& m);
bool decode(BitReader& r, PauseRequest& m);
bool decode(BitReader& r, PauseState& m);
bool decode(BitReader& r, LobbyChoice& m);
bool decode(BitReader& r, LobbyState& m);
bool decode(BitReader& r, OwnerSetMap& m);
bool decode(BitReader& r, FileRequest& m);
bool decode(BitReader& r, FileData& m);
bool decode(BitReader& r, OwnerStart& m);
bool decode(BitReader& r, OwnerSetAi& m);
bool decode(BitReader& r, ListRooms& m);
bool decode(BitReader& r, RoomList& m);
bool decode(BitReader& r, CreateRoom& m);
bool decode(BitReader& r, FindRoom& m);
bool decode(BitReader& r, RoomAddress& m);
bool decode(BitReader& r, ServerInfo& m);
bool decode(BitReader& r, RegisterHost& m);
bool decode(BitReader& r, HostRegistered& m);
bool decode(BitReader& r, JoinHosted& m);
bool decode(BitReader& r, HostedEndpoint& m);
bool decode(BitReader& r, PunchRequest& m);

inline MsgType readType(BitReader& r) { return static_cast<MsgType>(r.readU8()); }

const char* rejectReasonText(RejectReason reason);

} // namespace OpenArtOfWar::Net
