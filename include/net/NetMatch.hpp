#pragma once

#include "game/Match.hpp"
#include "game/SimCommand.hpp"

#include <cstdint>
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
        void setPassword(const std::string &password)
        {
            m_password = password;
        }
        bool host(uint16_t port, const std::string &playerName, std::string &error);
        // List the hosted game on the server (internet games without forwarded ports); its code shows up
        // in publicCode()
        void announce(const std::string &master, uint16_t masterPort);
        const std::string &publicCode() const;
        // Join a game hosted by a player through the server (by its code; NAT hole punching)
        bool joinHosted(const std::string &master, uint16_t masterPort, const std::string &code, const std::string &playerName, std::string &error);
        // `dedicated`: a room of openaow-server (its first player, the owner, decides for the room)
        bool join(const std::string &address, uint16_t port, const std::string &playerName, std::string &error, bool dedicated = false);
        void leave();

        // Services the network (call every frame, menus included)
        void update(uint32_t nowMs);

        bool isHost() const
        {
            return m_host != nullptr;
        }
        bool isDedicatedRoom() const
        {
            return m_dedicated;
        }
        // The owner of a dedicated room (picks the map, starts): the connected player with the lowest slot
        bool isOwner() const;
        // May pick the map and start: the host of a listen game or the owner of a dedicated room
        bool controlsRoom() const
        {
            return isHost() || isOwner();
        }
        // What the game identifies itself with (version, simulation build, data)
        static Net::GameIdentity gameIdentity();
        bool connected() const;
        bool rejected() const;
        std::string statusText() const;
        int playerCount() const;
        // This computer's slot once joined
        uint8_t localSlot() const;

        // The room. Host: picks the map (its start positions follow from it). Everybody: own picks, chat.
        void setLobbyMap(const std::string &mapPath);
        const Net::LobbyState *lobby() const;
        // The room's map is here, exactly the room's version of it (else it is being downloaded)
        bool mapReady() const
        {
            return !m_lobbyMapFile.empty();
        }
        // The file of the room's map on this computer (the game's own or a downloaded one); empty: not yet
        const std::string &lobbyMapFile() const
        {
            return m_lobbyMapFile;
        }
        // The match could not start here (the map is missing or differs)
        const std::string &failure() const
        {
            return m_failure;
        }
        const Net::Roster *roster() const;
        void setChoice(const Net::LobbyChoice &choice);
        // Host / room owner: add, change or remove a computer player
        void setAi(const Net::OwnerSetAi &change);
        void sendChat(const std::string &text);
        std::vector<Net::Chat> takeChat();
        // Host: everybody in the room is ready (two players at least)
        bool canStart() const;
        // Host: the match from the room's picks; random factions and start positions are drawn here with the
        // match seed, so every peer gets the same resolved setup
        // `prebuilt`: play with the buildings and units the map has (else everybody starts from the usual base)
        void startFromLobby(uint32_t nowMs, int startingCredits, bool prebuilt = false);

        // Host: every connected player takes part, in the order they joined; the setup goes to all of them
        void startMatch(const MatchSetup &setup, uint32_t nowMs);
        // Once, when the host's setup has arrived (local set to this computer's slot)
        bool takeStartedMatch(MatchSetup &out);

        // In the match: the local player's commands go to the host from now on
        void attach(Simulation &sim);
        void detach();
        // Tick `tick` may run: its turn is here (on the first tick of a turn its commands get scheduled)
        bool canRunTick(uint32_t tick);
        // After the tick ran: closes the local turn (sends its commands), reports checksums
        void afterTick(uint32_t tick, uint64_t checksum);
        // Turns received but not run yet (more than a couple: the client is behind and may catch up)
        int turnsAhead(uint32_t tick) const;
        bool desynced() const;
        // Pause for everybody / go on; the state as the host announced it
        void togglePause();
        bool paused() const;
        uint8_t pausedBy() const;
        uint8_t pausesLeft() const;
        uint32_t desyncTick() const;
        // In a match the connection dropped: trying to get back in (with the session token)
        bool reconnecting() const
        {
            return m_reconnect.active;
        }
        // Turns received from the host so far, in ticks (how far a catching-up client has to go)
        uint32_t ticksReceived() const;
        // Seconds the client has waited for the host's next turn (0 while turns arrive)
        float stalledSeconds(uint32_t nowMs) const;
        void noteTickRan(uint32_t nowMs)
        {
            m_lastTickMs = nowMs;
        }
        // Window for reconnecting (environment network.reconnect_window_s)
        void setReconnectWindow(uint32_t ms)
        {
            m_reconnectWindowMs = ms;
        }
        // Tests: the connection drops here and now (the reconnect logic takes over)
        void dropConnection();

        static std::vector<uint8_t> encodeSetup(const MatchSetup &setup);
        static bool decodeSetup(const std::vector<uint8_t> &bytes, MatchSetup &out);
        // What the simulation reads from the assets (game data, character configs): peers must agree
        static uint64_t assetsHash();
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
        uint32_t m_scheduledTurn {0}; // turns below this have their commands scheduled
        uint32_t m_takenStarts {0};   // StartMatch messages turned into a match so far
        void updateMapFile();
        std::string m_lobbyMapFile;
        std::string m_lobbyMapChecked; // path + hash the file above belongs to
        uint64_t m_requestedMap {0};
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
        uint32_t m_reconnectWindowMs {60000};
        uint32_t m_lastTickMs {0};
    };

} // namespace OpenArtOfWar
