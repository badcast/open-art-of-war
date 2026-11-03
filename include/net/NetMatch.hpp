#pragma once

#include "game/Match.hpp"
#include "game/SimCommand.hpp"
#include "net/ReplayRecorder.hpp"
#include "openaow/net/Protocol.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace OpenArtOfWar
{

class Simulation;

namespace Net
{
struct GameIdentity;
class SessionHost;
class SessionClient;
class HostAnnouncer;
class HostedJoin;
class Transport;
struct LobbyChoice;
struct OwnerSetAi;
struct LobbyState;
struct Roster;
struct Chat;
} // namespace Net

// The game's side of a network match: hosts (listen server, with its own client through memory) or
// joins, turns the match setup into bytes and back, sends the local player's commands once per turn
// and lets the simulation run a tick only when its turn has arrived from the host.
class NetMatch
{
public:
    NetMatch();
    ~NetMatch();

    // Listen server on `port` (UDP); false with `error` when the port cannot be opened
    // The room password: given to a game this computer hosts, sent when joining (empty: none)
    void SetPassword(const std::string &password)
    {
        m_password = password;
    }
    bool host(uint16_t port, const std::string &playerName, std::string &error);
    // List the hosted game on the server (internet games without forwarded ports); its code shows up
    // in publicCode()
    void Announce(const std::string &master, uint16_t masterPort);
    const std::string &PublicCode() const;
    // Join a game hosted by a player through the server (by its code; NAT hole punching)
    bool joinHosted(
        const std::string &master,
        uint16_t masterPort,
        const std::string &code,
        const std::string &playerName,
        std::string &error);
    // `dedicated`: a room of openaow-server (its first player, the owner, decides for the room)
    bool join(
        const std::string &address,
        uint16_t port,
        const std::string &playerName,
        std::string &error,
        bool dedicated = false);
    void leave();

    // Services the network (call every frame, menus included)
    void Update(uint32_t nowMs);

    bool IsHost() const
    {
        return m_host != nullptr;
    }
    bool IsDedicatedRoom() const
    {
        return m_dedicated;
    }
    // The owner of a dedicated room (picks the map, starts): the connected player with the lowest slot
    bool IsOwner() const;
    // May pick the map and start: the host of a listen game or the owner of a dedicated room
    bool ControlsRoom() const
    {
        return IsHost() || IsOwner();
    }
    // What the game identifies itself with (version, simulation build, data)
    static Net::GameIdentity gameIdentity();
    bool connected() const;
    bool rejected() const;
    // Why the host or server refused (valid while rejected()): the reason and its detail (the host's version, ...)
    Net::RejectReason RejectedWhy() const;
    const std::string &RejectedDetail() const;
    // The data were fetched from the host in this session (a mismatch after that is final)
    bool Synced() const
    {
        return m_synced;
    }
    // Joining (not hosting, not back in a match): no answer from the other side within the join timeout
    bool JoinTimedOut(uint32_t nowMs) const;
    static constexpr uint32_t kJoinTimeoutMs = 15000;
    // The line of the connecting screen: where to, and for how long already (translated)
    std::string ConnectingText(uint32_t nowMs) const;
    std::string StatusText() const;
    int PlayerCount() const;
    // This computer's slot once joined
    uint8_t LocalSlot() const;

    // The room. Host: picks the map (its start positions follow from it). Everybody: own picks, chat.
    void SetLobbyMap(const std::string &mapPath);
    const Net::LobbyState *Lobby() const;
    // The room's map is here, exactly the room's version of it (else it is being downloaded)
    bool MapReady() const
    {
        return !m_lobbyMapFile.empty();
    }
    // The file of the room's map on this computer (the game's own or a downloaded one); empty: not yet
    const std::string &LobbyMapFile() const
    {
        return m_lobbyMapFile;
    }
    // The match could not start here (the map is missing or differs)
    const std::string &Failure() const
    {
        return m_failure;
    }
    const Net::Roster *roster() const;
    void SetChoice(const Net::LobbyChoice &choice);
    // The own picks as last sent: the room's echo lags behind on a slow link, and editing that stale copy
    // sent old values back (a faction changed a moment ago came back with the next change of colour)
    bool LocalChoice(Net::LobbyChoice &out) const;
    // Host / room owner: add, change or remove a computer player
    void SetAi(const Net::OwnerSetAi &change);
    // Host / room owner: the team of a player (players cannot choose their own)
    void SetTeam(uint8_t slot, uint8_t team, uint8_t handicap, uint8_t armyCap = 255); // 255: keep the player's limit
    // Host / room owner: a start position open, blocked or taken by a computer player
    void SetSeat(const Net::OwnerSetSeat &change);
    // The owner sets the match options of the room
    void SetOptions(bool fogOfWar, bool prebuilt, uint8_t credits, bool night);
    void SendChat(const std::string &text);
    std::vector<Net::Chat> TakeChat();
    // Host: everybody in the room is ready (two players at least)
    bool CanStart() const;
    // Host: the match from the room's picks; random factions and start positions are drawn here with the
    // match seed, so every peer gets the same resolved setup
    // `prebuilt`: prebuilt bases (the map's own, or HQ + power + barracks + 4 units); false = from scratch (HQ + power
    // only)
    // Starts with the room's match options (fog of war, prebuilt bases, starting credits)
    void StartFromLobby(uint32_t nowMs);

    // Host: every connected player takes part, in the order they joined; the setup goes to all of them
    void startMatch(const MatchSetup &setup, uint32_t nowMs);
    // Once, when the host's setup has arrived (local set to this computer's slot)
    bool TakeStartedMatch(MatchSetup &out);

    // In the match: the local player's commands go to the host from now on
    void Attach(Simulation &sim);
    void Detach();
    // A closed turn into the simulation: everybody's commands on the turn's first tick, in the bundle's order, and the
    // players who left for good. The network and a replay schedule turns the same way.
    static void ScheduleBundle(Simulation &sim, const Net::TurnBundle &bundle, uint32_t ticksPerTurn);
    // Tick `tick` may run: its turn is here (on the first tick of a turn its commands get scheduled)
    bool CanRunTick(uint32_t tick);
    // After the tick ran: closes the local turn (sends its commands), reports checksums
    void AfterTick(uint32_t tick, uint64_t checksum);
    // Turns received but not run yet (more than a couple: the client is behind and may catch up)
    int TurnsAhead(uint32_t tick) const;
    bool Desynced() const;
    // Pause for everybody / go on; the state as the host announced it
    void TogglePause();
    bool paused() const;
    uint8_t PausedBy() const;
    uint8_t PausesLeft() const;
    uint32_t DesyncTick() const;
    // In a match the connection dropped: trying to get back in (with the session token)
    bool Reconnecting() const
    {
        return m_reconnect.active;
    }
    // Turns received from the host so far, in ticks (how far a catching-up client has to go)
    uint32_t TicksReceived() const;
    // Seconds the client has waited for the host's next turn (0 while turns arrive)
    float StalledSeconds(uint32_t nowMs) const;
    void NoteTickRan(uint32_t nowMs)
    {
        m_lastTickMs = nowMs;
    }
    // Window for reconnecting (environment network.reconnect_window_s)
    void SetReconnectWindow(uint32_t ms)
    {
        m_reconnectWindowMs = ms;
    }
    // Network statistics overlay: the link to the host / server and, on a host, every player's ping
    struct Stats
    {
        bool valid {false};
        bool host {false}; // this computer hosts (its own link is memory: 0 ms)
        uint32_t rttMs {0};
        uint32_t jitterMs {0};
        float lossPercent {0.0f};
        float sendKBps {0.0f}; // over the last second
        float recvKBps {0.0f};
        uint32_t packetsSent {0};
        uint32_t packetsLost {0};
        int inputDelayMs {0}; // commands take effect this much later (adapts to the slowest ping)
        int turnMs {0};
        int turnsBuffered {0};       // turns received but not run yet (>2: behind, catching up)
        float stalledSeconds {0.0f}; // waiting for the next turn
        bool reconnecting {false};
        bool desynced {false};
        struct Peer
        {
            uint8_t slot {0};
            std::string name;
            bool connected {false};
            uint32_t rttMs {0};
        };
        std::vector<Peer> peers; // host only
    };
    // `tick`: the simulation's current tick (for the turn buffer); call about once per frame
    Stats stats(uint32_t nowMs, uint32_t tick);

    // The match is over (or left): the replay of it is saved in the user's directory. `outcome`: "victory", "defeat",
    // "left", ... One file per match; empty result when nothing was recorded or it was saved already.
    std::string FinishReplay(const std::string &outcome);
    // The local camera (world pixel centre, zoom): sent to the others with the turns, once in a while, when it moved.
    // Only a replay uses it, to follow a player's view.
    void SendView(float centerX, float centerY, float zoom);
    static constexpr uint32_t kViewIntervalTicks = 20;

    // Tests: the connection drops here and now (the reconnect logic takes over)
    void DropConnection();

    static std::vector<uint8_t> EncodeSetup(const MatchSetup &setup);
    static bool DecodeSetup(const std::vector<uint8_t> &bytes, MatchSetup &out);
    // What the simulation reads from the assets (character configs, balance, projectiles, research, ...): peers must
    // agree. Pictures, sounds and the UI colours (net_visual in main.json) are not part of it. Computed from the files
    // as they are now, a session overlay (see below) included.
    static uint64_t AssetsHash();
    // The same files one by one (path, hash of the bytes); the last entry stands for the manifest's simulation part
    static std::vector<Net::DataEntry> DataManifest();
    // Joining a host whose data differ: the host's files are fetched and put into a session overlay (Assets::
    // SetSessionOverlay), then the join goes on. The game reloads its data when this says so (once per change); when
    // the overlay is gone (the game left) it reloads again.
    bool TakeDataChanged()
    {
        const bool changed = m_dataChanged;
        m_dataChanged = false;
        return changed;
    }
    // "Downloading game data (3/12)": what the connecting screen shows while files come in; empty otherwise
    const std::string &SyncText() const
    {
        return m_syncText;
    }
    static constexpr uint32_t kHashIntervalTicks = 20;

private:
    std::string m_password; // see setPassword
    std::unique_ptr<Net::SessionHost> m_host;
    std::unique_ptr<Net::SessionClient> m_client;
    std::unique_ptr<Net::HostAnnouncer> m_announcer;
    std::unique_ptr<Net::HostedJoin> m_hostedJoin;
    Net::Transport *m_gameSocket {nullptr}; // the host's listening socket (owned by m_host)
    uint16_t m_gamePort {0};
    std::unique_ptr<struct LoopbackHolder> m_loopback;
    Simulation *m_sim {nullptr};
    std::vector<SimCommand> m_outgoing;
    ReplayRecorder m_recorder;
    uint32_t m_lastTick {0};
    int32_t m_lastViewX {-100000}, m_lastViewY {-100000}, m_lastViewZoom {0};
    uint32_t m_scheduledTurn {0}; // turns below this have their commands scheduled
    uint32_t m_takenStarts {0};   // StartMatch messages turned into a match so far
    void UpdateMapFile();
    // Data sync (see TakeDataChanged)
    void UpdateDataSync();
    void StartDataSync(const std::vector<Net::DataEntry> &theirs);
    void AcceptDataFile(const Net::FilePathData &file);
    void FailDataSync(const std::string &files);
    struct DataSync
    {
        bool active {false};
        std::vector<Net::DataEntry> wanted;              // the files that differ, as the host has them
        std::map<std::string, std::vector<uint8_t>> got; // asset key -> bytes
        size_t bytes {0};
    } m_sync;
    bool m_dataChanged {false};
    bool m_synced {false}; // the data were fetched once in this session: a second mismatch is final
    std::string m_syncText;
    std::string m_lobbyMapFile;
    std::string m_lobbyMapChecked; // path + hash the file above belongs to
    uint64_t m_requestedMap {0};
    uint32_t m_joinStartMs {0}; // SDL ticks of the last join / joinHosted
    std::string m_failure;
    bool m_dedicated {false};
    struct Reconnect
    {
        bool active {false};
        std::string address;
        uint16_t port {0};
        std::string name;
        uint32_t deadlineMs {0};
        uint32_t nextAttemptMs {0};
        uint32_t delayMs {500};
    } m_reconnect;
    std::string m_address;
    uint16_t m_port {0};
    std::string m_name;
    // Joined by code through the server (no address to dial back): a comeback goes the same way
    std::string m_hostedMaster;
    uint16_t m_hostedMasterPort {0};
    std::string m_hostedCode;
    std::array<uint8_t, 16> m_rejoinToken {}; // Net::SessionToken
    uint32_t m_reconnectWindowMs {60000};
    uint32_t m_lastTickMs {0};
    uint32_t m_nowMs {0};
    // Traffic rate: totals one second ago
    uint64_t m_rateSent {0}, m_rateRecv {0};
    uint32_t m_rateMs {0};
    float m_sendKBps {0.0f}, m_recvKBps {0.0f};
    bool m_hasPendingChoice {false};
    std::array<uint8_t, 5> m_pendingChoice {}; // faction, team, color, spawn, ready (Net::LobbyChoice)
    uint32_t m_pendingChoiceMs {0};
};

} // namespace OpenArtOfWar
