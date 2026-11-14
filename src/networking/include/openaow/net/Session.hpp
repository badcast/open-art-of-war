#pragma once

#include "openaow/net/Protocol.hpp"
#include "openaow/net/Transport.hpp"

#include <deque>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Host-relayed lockstep. Players only send their commands; the host gathers them into turns on its own
// clock and sends every turn to everybody, so all peers run the same commands on the same tick. A late
// command goes into the next open turn instead of holding everybody up. Peers report world checksums;
// the host compares them and announces a desync. The module knows nothing about the game: commands and
// the match setup are bytes the game encodes itself.
namespace OpenArtOfWar::Net
{

// What both sides must agree on to play together
struct GameIdentity
{
    std::string gameVersion;
    std::uint64_t simBuildId {0};
    std::uint64_t assetsHash {0};
};

struct HostConfig
{
    GameIdentity identity;
    std::string roomName; // shown to local network browsers
    // A room of the dedicated server: nobody plays on the host, the room's owner (the first player in it)
    // picks the map and starts the match with OwnerSetMap / OwnerStart
    bool ownerControls {false};
    std::uint16_t turnMs {100};
    std::uint8_t ticksPerTurn {2};
    std::uint8_t inputDelayMin {1};
    std::uint8_t inputDelayMax {6};
    int maxPlayers {kMaxPlayers};
    std::uint8_t pausesPerPlayer {3};
    std::uint32_t maxPauseMs {120000};       // a pause ends by itself after this
    std::uint32_t reconnectWindowMs {60000}; // a player who dropped out of a running match is given up after this
    std::string password;                    // joining asks for it (empty: open)
    // The data files of the simulation this host plays with (path, hash). A client whose data differ and that can
    // sync (Hello::canSync) is sent this list and may ask for the files; empty: such a client is turned away.
    std::vector<DataEntry> dataManifest;
    // Answers a file request: the file of `path` compressed (at most kMaxFileBytes) and the hash of its bytes; false
    // when the host does not serve it. Only paths of dataManifest are asked.
    std::function<bool(const std::string &path, std::vector<std::uint8_t> &packed, std::uint64_t &hash)> dataFile;
    // A dedicated server's room keeps the replay of every match here (empty: no recording) and tells what happens
    // (who connects from which address, the replay saved) to `log`
    std::string replayDir;
    std::function<void(const std::string &line)> log;
};

class SessionHost
{
public:
    explicit SessionHost(HostConfig config);
    ~SessionHost(); // saves the replay of a match still running

    // Players can come in through several transports (a listen server: ENet + its own loopback client)
    void addTransport(std::unique_ptr<Transport> transport);

    // Everything that arrived; closes the turns that are due. `nowMs`: any steadily rising clock.
    void update(std::uint32_t nowMs);

    // The room: the map (and how many start positions it has) and everybody's picks
    void setLobbyMap(std::string_view map, std::uint8_t spawnCount, std::uint64_t mapHash = 0);
    // A file the room hands out on request (the game's compressed map); a dedicated room gets them from
    // its owner. At most kMaxStoredFiles / kMaxStoredBytes are kept, the oldest go first.
    void offerFile(std::uint64_t hash, std::vector<std::uint8_t> bytes);
    static constexpr std::size_t kMaxStoredFiles = 8;
    static constexpr std::size_t kMaxStoredBytes = 4 * 1024 * 1024;
    const LobbyState &lobby() const
    {
        return m_lobby;
    }
    // Computer players: add (slot kRandom: the first free one), change or remove (see OwnerSetAi)
    void setAiSlot(const OwnerSetAi &change);
    // The team of a player in the room (the host / room owner decides teams, see OwnerSetTeam)
    void setTeam(
        std::uint8_t slot,
        std::uint8_t team,
        std::uint8_t handicap,
        std::uint8_t armyCap = 7,
        std::uint8_t techLevel = kTechKeep);
    // What a start position holds (see OwnerSetSeat): the host / room owner decides
    void setSeat(const OwnerSetSeat &change);
    // Match options of the room (the host / room owner decides, everybody sees them in the lobby)
    void setOptions(bool fogOfWar, bool prebuilt, std::uint8_t credits, bool night, std::uint8_t techLevel = 0);
    // Everybody connected has said ready (and there are at least `minPlayers`, computer players included)
    bool everyoneReady(int minPlayers) const;

    // Sends the match setup to everybody; turn 0 closes `firstTurnDelayMs` later
    void startMatch(std::vector<std::uint8_t> setup, std::uint32_t nowMs, std::uint32_t firstTurnDelayMs = 3000);
    bool matchRunning() const
    {
        return m_running;
    }
    bool paused() const
    {
        return m_paused;
    }

    struct Player
    {
        std::uint8_t slot {0};
        std::string name;
        bool connected {false};
        std::uint32_t rttMs {0};
    };
    std::vector<Player> players() const;
    int playerCount() const;
    int connectedCount() const;
    // The slot that may decide for the room (lowest connected), -1 when nobody is there
    int ownerSlot() const;
    RoomInfo roomInfo() const;
    std::uint8_t inputDelay() const
    {
        return m_inputDelay;
    }
    // A tick whose checksums differed (desyncTick valid when desynced())
    bool desynced() const
    {
        return m_desynced;
    }
    std::uint32_t desyncTick() const
    {
        return m_desyncTick;
    }
    std::uint32_t nextTurn() const
    {
        return m_nextTurn;
    }

private:
    struct Connection
    {
        std::size_t transport {0};
        PeerId peer {0};
        int slot {-1};        // -1: not through the handshake yet
        bool syncing {false}; // its data differ: it may fetch the manifest's files before saying hello again
        std::uint32_t syncStartMs {0};
    };
    struct SlotState
    {
        bool used {false};
        bool connected {false};
        std::string name;
        SessionToken token {};
        std::size_t connection {0};
        std::map<std::uint32_t, std::vector<std::vector<std::uint8_t>>> commands; // turn -> batches of encoded commands
        LobbyChoice choice;
        std::uint8_t pausesLeft {0};
        bool ai {false};
        std::uint8_t aiLevel {1};
        std::uint32_t goneMs {0};   // when the connection was lost (a running match keeps the slot for a while)
        bool hashed {false};        // sent a world hash in this match
        std::uint32_t hashTick {0}; // the newest tick it sent a hash for
        bool authorityAcked {false};
        std::uint32_t authorityAckTick {0};
    };

    // A team colour (1..8) nobody else in the room has: `preferred` when it is free, else the first free one.
    // The classic look (0) and anything else is left as it is.
    std::uint8_t freeColor(std::uint8_t slot, std::uint8_t preferred) const;
    // The first start position of the map nobody holds and nobody blocked (kRandom: none)
    std::uint8_t freeSpawn() const;
    void handle(std::size_t connection, const TransportEvent &e, std::uint32_t nowMs);
    void handleHello(std::size_t connection, const Hello &hello, std::uint32_t nowMs);
    void sendTo(std::size_t connection, Channel channel, const std::uint8_t *data, std::size_t size);
    void broadcast(Channel channel, const std::uint8_t *data, std::size_t size);
    void broadcastRoster();
    void broadcastLobby();
    // `ownerEdit`: the room owner sets it (computer players); a player cannot change his own handicap
    void applyChoice(std::uint8_t slot, LobbyChoice choice, bool ownerEdit = false);
    void setPaused(bool paused, std::uint8_t bySlot, std::uint32_t nowMs);
    bool m_paused {false};
    std::uint32_t m_pauseStartMs {0};
    std::uint8_t m_pausedBy {0};
    std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> m_files; // offered files, oldest first
    void closeTurn(std::uint32_t turn);
    // Compares the world hashes of every tick all players that still send hashes have reported
    void checkHashes();
    void adaptInputDelay();
    void reject(std::size_t connection, RejectReason reason, std::string_view detail);

    HostConfig m_config;
    std::vector<std::unique_ptr<Transport>> m_transports;
    std::vector<Connection> m_connections;
    std::array<SlotState, kMaxPlayers> m_slots {};
    bool m_running {false};
    std::uint32_t m_matchId {0};
    int m_authoritySlot {-1}; // fixed at match start, never transferred implicitly
    std::uint32_t m_startMs {0};
    std::uint32_t m_nextTurn {0};
    std::uint8_t m_inputDelay {2};
    std::uint8_t m_announcedDelay {0};
    std::map<std::uint32_t, std::map<std::uint8_t, std::uint64_t>> m_hashes; // tick -> slot -> checksum
    LobbyState m_lobby;
    std::vector<std::uint8_t> m_setup;                // of the running match (for players coming back)
    std::vector<std::vector<std::uint8_t>> m_turnLog; // every closed turn, encoded (ditto)
    std::vector<std::uint8_t> m_leftPending;          // slots given up in the match, announced with the next turn
    bool m_desynced {false};
    std::uint32_t m_desyncTick {0};
    // Replay of the running match (see HostConfig::replayDir)
    struct SeenPlayer
    {
        std::string name;
        std::vector<std::string> addresses; // every address the slot connected from, in order
    };
    std::array<SeenPlayer, kMaxPlayers> m_seen {};
    std::vector<std::pair<std::uint32_t, std::uint64_t>> m_replayChecksums; // ticks every connected player agrees on
    std::int64_t m_replayStartedAt {0};
    bool m_replayOpen {false};
    void logLine(std::string_view line) const;
    void finishReplay(std::string_view reason);
    std::vector<TransportEvent> m_events;
    std::vector<std::uint8_t> m_buffer;
    static constexpr std::uint32_t kMaxSyncMs = 30000;
    static constexpr int kMaxSyncing = 4;
};

struct ClientConfig
{
    GameIdentity identity;
    std::string playerName;
    SessionToken token {}; // non-zero: take back the slot of an earlier connection
    std::string password;  // of the room, when it has one
    bool canSync {false};  // may fetch the host's data files when they differ from the own (see DataManifest)
};

class SessionClient
{
public:
    enum class State
    {
        Connecting,
        Joined,
        Rejected,
        Disconnected
    };

    SessionClient(std::unique_ptr<Transport> transport, ClientConfig config);

    void update(std::uint32_t nowMs);

    // Opaque per-player state; only the match authority is allowed to publish it.
    bool sendAuthorityState(const AuthorityState &state);
    bool takeAuthorityState(AuthorityState &state);
    // Confirm a successfully decoded and applied snapshot; older states are then ignored.
    bool sendAuthorityAck(std::uint32_t tick);
    bool takeAuthorityAck(AuthorityAck &ack);

    State state() const
    {
        return m_state;
    }
    RejectReason rejectReason() const
    {
        return m_reject.reason;
    }
    const std::string &rejectDetail() const
    {
        return m_reject.detail;
    }
    std::uint8_t slot() const
    {
        return m_welcome.slot;
    }
    const SessionToken &token() const
    {
        return m_welcome.token;
    }
    std::uint16_t turnMs() const
    {
        return m_welcome.turnMs;
    }
    std::uint8_t ticksPerTurn() const
    {
        return m_welcome.ticksPerTurn;
    }
    const Roster &roster() const
    {
        return m_roster;
    }
    const LobbyState &lobby() const
    {
        return m_lobby;
    }
    void sendLobbyChoice(const LobbyChoice &choice);
    // Dedicated server rooms, the owner only
    void sendOwnerSetMap(std::string_view map, std::uint8_t spawnCount, std::uint64_t mapHash = 0);
    // Files: ask the room for one / give the dedicated room one to hand out (owner)
    void requestFile(std::uint64_t hash);
    void uploadFile(std::uint64_t hash, const std::vector<std::uint8_t> &bytes);
    // A file that arrived (empty bytes: the room does not have it); false when none is waiting
    bool takeFile(std::uint64_t &hash, std::vector<std::uint8_t> &bytes);
    // Data sync (joining a host whose simulation data differ): the host's list, once it arrived
    bool takeDataManifest(std::vector<DataEntry> &files);
    void requestDataFile(std::string_view path);
    // A data file that arrived (empty bytes: the host does not serve it); false when none is waiting
    bool takeDataFile(FilePathData &out);
    // Says hello again with the data hash the client has now (after the fetched files were installed)
    void sayHelloAgain(std::uint64_t assetsHash);
    void sendOwnerStart(const std::vector<std::uint8_t> &setup, std::uint32_t firstTurnDelayMs = 3000);
    void sendOwnerSetAi(const OwnerSetAi &change);
    void sendOwnerSetTeam(const OwnerSetTeam &change);
    void sendOwnerSetOptions(const OwnerSetOptions &change);
    void sendOwnerSetSeat(const OwnerSetSeat &change);

    // The host started the match: its setup (the game decodes it) and when turn 0 is due
    bool matchStarted() const
    {
        return m_started;
    }
    // Grows with every StartMatch (a reconnect brings the match again, from its start)
    std::uint32_t matchStarts() const
    {
        return m_starts;
    }
    const std::vector<std::uint8_t> &matchSetup() const
    {
        return m_setup;
    }

    // Commands of the local player for `turn` (one message per turn; empty turns need not be sent)
    void sendCommands(std::uint32_t turn, const std::vector<std::uint8_t> &bytes);
    // The turn as the host closed it, nullptr when it has not arrived yet
    const TurnBundle *turn(std::uint32_t turn) const;
    // Turns before `turn` are done with
    void dropTurnsBefore(std::uint32_t turn);
    // The newest turn received + 1 (how far the client may run)
    std::uint32_t turnsReceived() const
    {
        return m_turnsReceived;
    }
    // Turns between issuing a command and running it (set by the host)
    std::uint8_t inputDelay() const
    {
        return m_inputDelay;
    }

    // The player whose game simulates the world in this match (valid once matchStarted())
    std::uint8_t authoritySlot() const
    {
        return m_authoritySlot;
    }

    void sendWorldHash(std::uint32_t tick, std::uint64_t hash);

    // Pause for everybody (a limited number of times) / go on (anybody)
    void requestPause(bool pause);
    const PauseState &pauseState() const
    {
        return m_pause;
    }
    bool desynced() const
    {
        return m_desynced;
    }
    std::uint32_t desyncTick() const
    {
        return m_desyncTick;
    }

    void sendChat(std::string_view text, bool teamOnly);
    std::vector<Chat> takeChat();

    std::uint32_t roundTripMs() const;
    // Round trip, jitter, loss and traffic of the link to the host / server
    LinkStats linkStats() const;
    // Leaves the host (the connection ends as if it had dropped)
    // Tells the host that the player leaves for good: his slot is freed at once (a drop is kept for a reconnect)
    void sendLeave();
    void disconnect();

private:
    void handle(const TransportEvent &e);
    void send(Channel channel, const std::uint8_t *data, std::size_t size);

    std::unique_ptr<Transport> m_transport;
    ClientConfig m_config;
    State m_state {State::Connecting};
    Welcome m_welcome;
    Reject m_reject;
    Roster m_roster;
    LobbyState m_lobby;
    bool m_started {false};
    std::uint32_t m_starts {0};
    std::vector<std::uint8_t> m_setup;
    std::map<std::uint32_t, TurnBundle> m_turns;
    std::uint32_t m_turnsReceived {0};
    std::uint8_t m_inputDelay {2};
    bool m_desynced {false};
    std::uint32_t m_desyncTick {0};
    PauseState m_pause;
    std::vector<Chat> m_chat;
    std::deque<AuthorityState>
        m_authorityStates; // in arrival order, every one is delivered (one-time events ride on them)
    std::deque<AuthorityAck> m_authorityAcks;
    std::array<bool, kMaxPlayers> m_authorityAcked {};
    std::array<std::uint32_t, kMaxPlayers> m_authorityAckTicks {};
    std::uint32_t m_matchId {0};
    std::uint8_t m_authoritySlot {0};
    std::uint32_t m_authorityStateTick {0};
    bool m_hasAuthorityStateTick {false};
    std::vector<FileData> m_files;
    bool m_manifestWaiting {false};
    std::vector<DataEntry> m_manifest;
    std::vector<FilePathData> m_dataFiles;
    std::vector<TransportEvent> m_events;
    std::vector<std::uint8_t> m_buffer;
};

} // namespace OpenArtOfWar::Net
