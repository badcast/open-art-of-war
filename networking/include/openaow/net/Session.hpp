#pragma once

#include "openaow/net/Protocol.hpp"
#include "openaow/net/Transport.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Host-relayed lockstep. Players only send their commands; the host gathers them into turns on its own
// clock and sends every turn to everybody, so all peers run the same commands on the same tick. A late
// command goes into the next open turn instead of holding everybody up. Peers report world checksums;
// the host compares them and announces a desync. The module knows nothing about the game: commands and
// the match setup are bytes the game encodes itself.
namespace OpenArtOfWar::Net {

// What both sides must agree on to play together
struct GameIdentity {
    std::string gameVersion;
    uint64_t simBuildId{0};
    uint64_t assetsHash{0};
};

struct HostConfig {
    GameIdentity identity;
    std::string roomName;   // shown to local network browsers
    // A room of the dedicated server: nobody plays on the host, the room's owner (the first player in it)
    // picks the map and starts the match with OwnerSetMap / OwnerStart
    bool ownerControls{false};
    uint16_t turnMs{100};
    uint8_t ticksPerTurn{2};
    uint8_t inputDelayMin{1};
    uint8_t inputDelayMax{6};
    int maxPlayers{kMaxPlayers};
    uint8_t pausesPerPlayer{3};
    uint32_t maxPauseMs{120000};   // a pause ends by itself after this
    uint32_t reconnectWindowMs{60000};   // a player who dropped out of a running match is given up after this
    std::string password;          // joining asks for it (empty: open)
};

class SessionHost {
public:
    explicit SessionHost(HostConfig config);
    ~SessionHost();

    // Players can come in through several transports (a listen server: ENet + its own loopback client)
    void addTransport(std::unique_ptr<Transport> transport);

    // Everything that arrived; closes the turns that are due. `nowMs`: any steadily rising clock.
    void update(uint32_t nowMs);

    // The room: the map (and how many start positions it has) and everybody's picks
    void setLobbyMap(const std::string& map, uint8_t spawnCount, uint64_t mapHash = 0);
    // A file the room hands out on request (the game's compressed map); a dedicated room gets them from
    // its owner. At most kMaxStoredFiles / kMaxStoredBytes are kept, the oldest go first.
    void offerFile(uint64_t hash, std::vector<uint8_t> bytes);
    static constexpr size_t kMaxStoredFiles = 8;
    static constexpr size_t kMaxStoredBytes = 4 * 1024 * 1024;
    const LobbyState& lobby() const { return m_lobby; }
    // Computer players: add (slot kRandom: the first free one), change or remove (see OwnerSetAi)
    void setAiSlot(const OwnerSetAi& change);
    // Everybody connected has said ready (and there are at least `minPlayers`, computer players included)
    bool everyoneReady(int minPlayers) const;

    // Sends the match setup to everybody; turn 0 closes `firstTurnDelayMs` later
    void startMatch(std::vector<uint8_t> setup, uint32_t nowMs, uint32_t firstTurnDelayMs = 3000);
    bool matchRunning() const { return m_running; }
    bool paused() const { return m_paused; }

    struct Player {
        uint8_t slot{0};
        std::string name;
        bool connected{false};
        uint32_t rttMs{0};
    };
    std::vector<Player> players() const;
    int playerCount() const;
    int connectedCount() const;
    // The slot that may decide for the room (lowest connected), -1 when nobody is there
    int ownerSlot() const;
    RoomInfo roomInfo() const;
    uint8_t inputDelay() const { return m_inputDelay; }
    // A tick whose checksums differed (desyncTick valid when desynced())
    bool desynced() const { return m_desynced; }
    uint32_t desyncTick() const { return m_desyncTick; }
    uint32_t nextTurn() const { return m_nextTurn; }

private:
    struct Connection {
        size_t transport{0};
        PeerId peer{0};
        int slot{-1};           // -1: not through the handshake yet
    };
    struct SlotState {
        bool used{false};
        bool connected{false};
        std::string name;
        SessionToken token{};
        size_t connection{0};
        std::map<uint32_t, std::vector<std::vector<uint8_t>>> commands;   // turn -> batches of encoded commands
        LobbyChoice choice;
        uint8_t pausesLeft{0};
        bool ai{false};
        uint8_t aiLevel{1};
        uint32_t goneMs{0};   // when the connection was lost (a running match keeps the slot for a while)
    };

    void handle(size_t connection, const TransportEvent& e, uint32_t nowMs);
    void handleHello(size_t connection, const Hello& hello);
    void sendTo(size_t connection, Channel channel, const uint8_t* data, size_t size);
    void broadcast(Channel channel, const uint8_t* data, size_t size);
    void broadcastRoster();
    void broadcastLobby();
    void applyChoice(uint8_t slot, LobbyChoice choice);
    void setPaused(bool paused, uint8_t bySlot, uint32_t nowMs);
    bool m_paused{false};
    uint32_t m_pauseStartMs{0};
    uint8_t m_pausedBy{0};
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> m_files;   // offered files, oldest first
    void closeTurn(uint32_t turn);
    void adaptInputDelay();
    void reject(size_t connection, RejectReason reason, const std::string& detail);

    HostConfig m_config;
    std::vector<std::unique_ptr<Transport>> m_transports;
    std::vector<Connection> m_connections;
    std::array<SlotState, kMaxPlayers> m_slots{};
    bool m_running{false};
    uint32_t m_startMs{0};
    uint32_t m_nextTurn{0};
    uint8_t m_inputDelay{2};
    uint8_t m_announcedDelay{0};
    std::map<uint32_t, std::map<uint8_t, uint64_t>> m_hashes;   // tick -> slot -> checksum
    LobbyState m_lobby;
    std::vector<uint8_t> m_setup;                     // of the running match (for players coming back)
    std::vector<std::vector<uint8_t>> m_turnLog;      // every closed turn, encoded (ditto)
    bool m_desynced{false};
    uint32_t m_desyncTick{0};
    std::vector<TransportEvent> m_events;
    std::vector<uint8_t> m_buffer;
};

struct ClientConfig {
    GameIdentity identity;
    std::string playerName;
    SessionToken token{};   // non-zero: take back the slot of an earlier connection
    std::string password;   // of the room, when it has one
};

class SessionClient {
public:
    enum class State { Connecting, Joined, Rejected, Disconnected };

    SessionClient(std::unique_ptr<Transport> transport, ClientConfig config);

    void update(uint32_t nowMs);

    State state() const { return m_state; }
    RejectReason rejectReason() const { return m_reject.reason; }
    const std::string& rejectDetail() const { return m_reject.detail; }
    uint8_t slot() const { return m_welcome.slot; }
    const SessionToken& token() const { return m_welcome.token; }
    uint16_t turnMs() const { return m_welcome.turnMs; }
    uint8_t ticksPerTurn() const { return m_welcome.ticksPerTurn; }
    const Roster& roster() const { return m_roster; }
    const LobbyState& lobby() const { return m_lobby; }
    void sendLobbyChoice(const LobbyChoice& choice);
    // Dedicated server rooms, the owner only
    void sendOwnerSetMap(const std::string& map, uint8_t spawnCount, uint64_t mapHash = 0);
    // Files: ask the room for one / give the dedicated room one to hand out (owner)
    void requestFile(uint64_t hash);
    void uploadFile(uint64_t hash, const std::vector<uint8_t>& bytes);
    // A file that arrived (empty bytes: the room does not have it); false when none is waiting
    bool takeFile(uint64_t& hash, std::vector<uint8_t>& bytes);
    void sendOwnerStart(const std::vector<uint8_t>& setup, uint32_t firstTurnDelayMs = 3000);
    void sendOwnerSetAi(const OwnerSetAi& change);

    // The host started the match: its setup (the game decodes it) and when turn 0 is due
    bool matchStarted() const { return m_started; }
    // Grows with every StartMatch (a reconnect brings the match again, from its start)
    uint32_t matchStarts() const { return m_starts; }
    const std::vector<uint8_t>& matchSetup() const { return m_setup; }

    // Commands of the local player for `turn` (one message per turn; empty turns need not be sent)
    void sendCommands(uint32_t turn, const std::vector<uint8_t>& bytes);
    // The turn as the host closed it, nullptr when it has not arrived yet
    const TurnBundle* turn(uint32_t turn) const;
    // Turns before `turn` are done with
    void dropTurnsBefore(uint32_t turn);
    // The newest turn received + 1 (how far the client may run)
    uint32_t turnsReceived() const { return m_turnsReceived; }
    // Turns between issuing a command and running it (set by the host)
    uint8_t inputDelay() const { return m_inputDelay; }

    void sendWorldHash(uint32_t tick, uint64_t hash);

    // Pause for everybody (a limited number of times) / go on (anybody)
    void requestPause(bool pause);
    const PauseState& pauseState() const { return m_pause; }
    bool desynced() const { return m_desynced; }
    uint32_t desyncTick() const { return m_desyncTick; }

    void sendChat(const std::string& text, bool teamOnly);
    std::vector<Chat> takeChat();

    uint32_t roundTripMs() const;
    // Leaves the host (the connection ends as if it had dropped)
    // Tells the host that the player leaves for good: his slot is freed at once (a drop is kept for a reconnect)
    void sendLeave();
    void disconnect();

private:
    void handle(const TransportEvent& e);
    void send(Channel channel, const uint8_t* data, size_t size);

    std::unique_ptr<Transport> m_transport;
    ClientConfig m_config;
    State m_state{State::Connecting};
    Welcome m_welcome;
    Reject m_reject;
    Roster m_roster;
    LobbyState m_lobby;
    bool m_started{false};
    uint32_t m_starts{0};
    std::vector<uint8_t> m_setup;
    std::map<uint32_t, TurnBundle> m_turns;
    uint32_t m_turnsReceived{0};
    uint8_t m_inputDelay{2};
    bool m_desynced{false};
    uint32_t m_desyncTick{0};
    PauseState m_pause;
    std::vector<Chat> m_chat;
    std::vector<FileData> m_files;
    std::vector<TransportEvent> m_events;
    std::vector<uint8_t> m_buffer;
};

} // namespace OpenArtOfWar::Net
