#include "net/NetMatch.hpp"
#include "core/AssetStore.hpp"
#include "game/MissionController.hpp"
#include "net/MapFiles.hpp"
#include "game/SimCommandCodec.hpp"
#include "game/Simulation.hpp"
#include "openaow/net/BitStream.hpp"
#include "openaow/net/Crc32.hpp"
#include "openaow/net/Discovery.hpp"
#include "openaow/net/Rendezvous.hpp"
#include "openaow/net/Session.hpp"

#include <algorithm>
#include <iostream>
#include <random>
#include <cstdio>
#include <cstdlib>

namespace OpenArtOfWar
{

    struct LoopbackHolder
    {
        Net::LoopbackHub hub;
    };

    namespace
    {

        Net::GameIdentity identity()
        {
            Net::GameIdentity id;
            id.gameVersion = OAOW_VERSION;
            // The simulation code: the same version string means the same source (the build is deterministic
            // across platforms, see DetMath / -ffp-contract=off)
            const std::string v = std::string(OAOW_VERSION) + "|sim1";
            id.simBuildId = Net::crc32(v.data(), v.size());
            id.assetsHash = NetMatch::assetsHash();
            return id;
        }

    } // namespace

    NetMatch::NetMatch() = default;
    NetMatch::~NetMatch()
    {
        leave();
    }

    uint64_t NetMatch::assetsHash()
    {
        static const uint64_t hash = []
        {
            // CRC of every file the simulation reads, names included, in a fixed order; two CRCs side by side
            // make a 64-bit value
            uint32_t a = 0, b = 0x9e3779b9u;
            std::vector<std::string> files = Assets::list("assets/data", ".json");
            for(const char *ext : {".yaml", ".yml"})
            {
                for(const std::string &f : Assets::list("assets/characters", ext))
                    files.push_back(f);
            }
            std::sort(files.begin(), files.end());
            std::vector<uint8_t> bytes;
            for(const std::string &f : files)
            {
                if(!Assets::readBytes(f, bytes))
                    continue;
                a = Net::crc32(f.data(), f.size(), a);
                a = Net::crc32(bytes.data(), bytes.size(), a);
                b = Net::crc32(bytes.data(), bytes.size(), b ^ static_cast<uint32_t>(bytes.size()));
            }
            return (static_cast<uint64_t>(a) << 32) | b;
        }();
        return hash;
    }

    bool NetMatch::host(uint16_t port, const std::string &playerName, std::string &error)
    {
        leave();
        Net::HostConfig hc;
        hc.identity = identity();
        hc.roomName = playerName;
        hc.password = m_password;
        // Another game hosted on this computer holds the port: the next ones of the span (browsers probe them)
        std::unique_ptr<Net::Transport> server;
        for(uint16_t p = 0; p < Net::kDiscoveryPortSpan && !server; ++p)
        {
            server = Net::createEnetServer(static_cast<uint16_t>(port + p), Net::kMaxPlayers + 4, &error);
            if(server)
                m_gamePort = static_cast<uint16_t>(port + p);
        }
        if(!server)
            return false;
        m_gameSocket = server.get();
        m_host = std::make_unique<Net::SessionHost>(hc);
        m_host->addTransport(std::move(server));
        // The host plays too: its client talks to its server through memory
        m_loopback = std::make_unique<LoopbackHolder>();
        m_host->addTransport(m_loopback->hub.server());
        Net::ClientConfig cc;
        cc.identity = hc.identity;
        cc.playerName = playerName;
        cc.password = m_password;
        m_client = std::make_unique<Net::SessionClient>(m_loopback->hub.client(), cc);
        return true;
    }

    Net::GameIdentity NetMatch::gameIdentity()
    {
        return identity();
    }

    bool NetMatch::isOwner() const
    {
        if(!m_dedicated || !connected())
            return false;
        int lowest = Net::kMaxPlayers;
        for(const Net::RosterEntry &e : m_client->roster().players)
        {
            if(e.connected)
                lowest = std::min<int>(lowest, e.slot);
        }
        return lowest == m_client->slot();
    }

    bool NetMatch::join(const std::string &address, uint16_t port, const std::string &playerName, std::string &error, bool dedicated)
    {
        leave();
        m_dedicated = dedicated;
        m_address = address;
        m_port = port;
        m_name = playerName;
        auto transport = Net::createEnetClient(address, port, &error);
        if(!transport)
            return false;
        Net::ClientConfig cc;
        cc.identity = identity();
        cc.playerName = playerName;
        cc.password = m_password;
        m_client = std::make_unique<Net::SessionClient>(std::move(transport), cc);
        return true;
    }

    void NetMatch::announce(const std::string &master, uint16_t masterPort)
    {
        if(!m_host || !m_gameSocket || m_announcer)
            return;
        m_announcer = std::make_unique<Net::HostAnnouncer>(master, masterPort, *m_gameSocket, m_gamePort, Net::localLanIp());
    }

    const std::string &NetMatch::publicCode() const
    {
        static const std::string kNone;
        return m_announcer ? m_announcer->code() : kNone;
    }

    bool NetMatch::joinHosted(const std::string &master, uint16_t masterPort, const std::string &code, const std::string &playerName, std::string &error)
    {
        leave();
        m_hostedJoin = std::make_unique<Net::HostedJoin>(master, masterPort, code);
        if(m_hostedJoin->state() == Net::HostedJoin::State::Failed)
        {
            error = m_hostedJoin->error();
            m_hostedJoin.reset();
            return false;
        }
        m_name = playerName;
        return true;
    }

    void NetMatch::leave()
    {
        // Leaving for good: the host frees the slot at once instead of waiting for a comeback
        if(m_client)
            m_client->sendLeave();
        detach();
        m_announcer.reset();
        m_hostedJoin.reset();
        m_gameSocket = nullptr;
        m_client.reset();
        m_host.reset();
        m_loopback.reset();
        m_takenStarts = 0;
        m_dedicated = false;
        m_scheduledTurn = 0;
        m_reconnect = Reconnect {};
    }

    void NetMatch::update(uint32_t nowMs)
    {
        // Through the server to a player-hosted game: when the way is open, the session takes the socket
        if(m_hostedJoin)
        {
            m_hostedJoin->update(nowMs);
            if(m_hostedJoin->state() == Net::HostedJoin::State::Ready)
            {
                Net::ClientConfig cc;
                cc.identity = identity();
                cc.playerName = m_name;
                cc.password = m_password;
                m_client = std::make_unique<Net::SessionClient>(m_hostedJoin->takeTransport(), cc);
                m_hostedJoin.reset();
            }
            else if(m_hostedJoin->state() == Net::HostedJoin::State::Failed)
            {
                m_failure = "server: " + m_hostedJoin->error();
                m_hostedJoin.reset();
            }
        }
        if(m_announcer && m_host)
            m_announcer->update(nowMs, m_host->roomInfo(), true);
        if(m_client)
            m_client->update(nowMs);
        if(m_host)
            m_host->update(nowMs);
        if(m_client)
            m_client->update(nowMs);

        // Lost in the middle of a match: back in with the session token, backing off between the tries
        if(!m_host && m_client && m_takenStarts > 0 && m_client->state() == Net::SessionClient::State::Disconnected)
        {
            if(!m_reconnect.active)
            {
                m_reconnect = Reconnect {true, m_address, m_port, m_name, nowMs + m_reconnectWindowMs, nowMs + 500, 500};
                std::cerr << "[Net] Connection lost, reconnecting" << std::endl;
            }
            if(nowMs >= m_reconnect.deadlineMs)
                return; // given up: the game shows it
            if(nowMs >= m_reconnect.nextAttemptMs)
            {
                std::string error;
                auto transport = Net::createEnetClient(m_reconnect.address, m_reconnect.port, &error);
                if(transport)
                {
                    Net::ClientConfig cc;
                    cc.identity = identity();
                    cc.playerName = m_reconnect.name;
                    cc.password = m_password;
                    cc.token = m_client->token();
                    m_client = std::make_unique<Net::SessionClient>(std::move(transport), cc);
                }
                m_reconnect.delayMs = std::min<uint32_t>(m_reconnect.delayMs * 2, 4000);
                m_reconnect.nextAttemptMs = nowMs + m_reconnect.delayMs;
            }
        }
        updateMapFile();
        if(m_reconnect.active && m_client && m_client->state() == Net::SessionClient::State::Joined)
        {
            m_reconnect.active = false;
            std::cerr << "[Net] Reconnected" << std::endl;
        }
    }

    void NetMatch::togglePause()
    {
        if(m_client)
            m_client->requestPause(!m_client->pauseState().paused);
    }
    bool NetMatch::paused() const
    {
        return m_client && m_client->pauseState().paused;
    }
    uint8_t NetMatch::pausedBy() const
    {
        return m_client ? m_client->pauseState().bySlot : 0;
    }
    uint8_t NetMatch::pausesLeft() const
    {
        return m_client ? m_client->pauseState().pausesLeft : 0;
    }

    void NetMatch::dropConnection()
    {
        if(m_client && !m_host)
            m_client->disconnect();
    }

    // The room's map on this computer: the own file when it is the room's version, else a downloaded copy,
    // else ask the room for it
    void NetMatch::updateMapFile()
    {
        if(!m_client)
            return;
        uint64_t hash = 0;
        std::vector<uint8_t> bytes;
        while(m_client->takeFile(hash, bytes))
        {
            const Net::LobbyState &lobby = m_client->lobby();
            if(hash == lobby.mapHash && !MapFiles::store(lobby.map, hash, bytes).empty())
                m_lobbyMapChecked.clear();
        }
        const Net::LobbyState &lobby = m_client->lobby();
        const std::string key = lobby.map + "#" + std::to_string(lobby.mapHash);
        if(key == m_lobbyMapChecked)
            return;
        if(lobby.map.empty())
        {
            m_lobbyMapFile.clear();
            return;
        }
        m_lobbyMapFile = MapFiles::resolve(lobby.map, lobby.mapHash);
        if(!m_lobbyMapFile.empty())
        {
            m_lobbyMapChecked = key;
        }
        else if(m_requestedMap != lobby.mapHash)
        {
            m_requestedMap = lobby.mapHash;
            m_client->requestFile(lobby.mapHash);
            std::cout << "[Net] Downloading the map " << lobby.map << std::endl;
        }
    }

    uint32_t NetMatch::desyncTick() const
    {
        return m_client ? m_client->desyncTick() : 0;
    }

    uint32_t NetMatch::ticksReceived() const
    {
        return m_client ? m_client->turnsReceived() * std::max<uint32_t>(1, m_client->ticksPerTurn()) : 0;
    }

    float NetMatch::stalledSeconds(uint32_t nowMs) const
    {
        if(!m_client || m_lastTickMs == 0 || nowMs < m_lastTickMs)
            return 0.0f;
        return (nowMs - m_lastTickMs) / 1000.0f;
    }

    bool NetMatch::connected() const
    {
        return m_client && m_client->state() == Net::SessionClient::State::Joined;
    }
    bool NetMatch::rejected() const
    {
        return m_client && m_client->state() == Net::SessionClient::State::Rejected;
    }
    uint8_t NetMatch::localSlot() const
    {
        return m_client ? m_client->slot() : 0;
    }
    bool NetMatch::desynced() const
    {
        return m_client && m_client->desynced();
    }

    int NetMatch::playerCount() const
    {
        if(m_host)
            return m_host->playerCount();
        return m_client ? static_cast<int>(m_client->roster().players.size()) : 0;
    }

    std::string NetMatch::statusText() const
    {
        if(m_hostedJoin)
            return "through the server";
        if(!m_client)
            return "offline";
        switch(m_client->state())
        {
            case Net::SessionClient::State::Connecting:
                return "connecting";
            case Net::SessionClient::State::Joined:
                return "joined";
            case Net::SessionClient::State::Rejected:
                return Net::rejectReasonText(m_client->rejectReason());
            case Net::SessionClient::State::Disconnected:
                return "disconnected";
        }
        return "";
    }

    void NetMatch::setLobbyMap(const std::string &mapPath)
    {
        if(!controlsRoom())
            return;
        const uint8_t spots = static_cast<uint8_t>(std::min<size_t>(MissionController::startPositions(mapPath).size(), kMaxPlayers));
        const uint64_t hash = MapFiles::hashOf(mapPath);
        // The room hands the map out to whoever does not have this very file (~20 KB compressed)
        const std::vector<uint8_t> packed = MapFiles::packed(mapPath);
        if(m_host)
        {
            m_host->offerFile(hash, packed);
            m_host->setLobbyMap(mapPath, spots, hash);
        }
        else
        {
            m_client->uploadFile(hash, packed);
            m_client->sendOwnerSetMap(mapPath, spots, hash);
        }
    }

    const Net::LobbyState *NetMatch::lobby() const
    {
        return m_client ? &m_client->lobby() : nullptr;
    }
    const Net::Roster *NetMatch::roster() const
    {
        return m_client ? &m_client->roster() : nullptr;
    }
    void NetMatch::setChoice(const Net::LobbyChoice &choice)
    {
        if(m_client)
            m_client->sendLobbyChoice(choice);
    }

    void NetMatch::setAi(const Net::OwnerSetAi &change)
    {
        if(m_host)
            m_host->setAiSlot(change);
        else if(isOwner())
            m_client->sendOwnerSetAi(change);
    }
    void NetMatch::sendChat(const std::string &text)
    {
        if(m_client && !text.empty())
            m_client->sendChat(text, false);
    }
    std::vector<Net::Chat> NetMatch::takeChat()
    {
        return m_client ? m_client->takeChat() : std::vector<Net::Chat> {};
    }
    bool NetMatch::canStart() const
    {
        if(m_host)
            return !m_host->lobby().map.empty() && m_host->everyoneReady(2);
        if(!isOwner())
            return false;
        // The room's view of the owner: everybody listed in the room is ready
        const Net::LobbyState &lobby = m_client->lobby();
        if(lobby.map.empty() || lobby.slots.size() < 2)
            return false;
        return std::all_of(lobby.slots.begin(), lobby.slots.end(), [](const Net::LobbySlot &s) { return s.choice.ready; });
    }

    void NetMatch::startFromLobby(uint32_t nowMs, int startingCredits, bool prebuilt)
    {
        if(!controlsRoom())
            return;
        const Net::LobbyState &lobby = m_host ? m_host->lobby() : m_client->lobby();
        MatchSetup setup;
        setup.mapPath = lobby.map;
        setup.mapHash = lobby.mapHash;
        setup.seed = static_cast<uint32_t>(std::random_device {}()) ^ nowMs;
        setup.startingCredits = startingCredits;
        setup.prebuilt = prebuilt;
        std::mt19937 rng(setup.seed);
        // Start positions picked by the players first, the random ones get the free spots left
        std::vector<int> free;
        for(int i = 0; i < lobby.spawnCount; ++i)
        {
            bool taken = false;
            for(const Net::LobbySlot &s : lobby.slots)
                taken = taken || s.choice.spawn == i;
            if(!taken)
                free.push_back(i);
        }
        std::shuffle(free.begin(), free.end(), rng);
        for(const Net::LobbySlot &s : lobby.slots)
        {
            PlayerSlot &slot = setup.slots[s.slot];
            slot.active = true;
            slot.human = !s.ai;
            slot.aiLevel = s.aiLevel;
            slot.faction = s.choice.faction == Net::kRandom ? static_cast<Faction>(rng() % 2) : static_cast<Faction>(s.choice.faction);
            slot.team = s.choice.team;
            slot.color = s.choice.color;
            int spawn = s.choice.spawn;
            if(spawn == Net::kRandom)
            {
                if(free.empty())
                    return; // more players than start positions
                spawn = free.back();
                free.pop_back();
            }
            setup.startPosition[s.slot] = static_cast<int8_t>(spawn);
        }
        if(m_host)
            startMatch(setup, nowMs);
        else
            m_client->sendOwnerStart(encodeSetup(setup));
    }

    void NetMatch::startMatch(const MatchSetup &setup, uint32_t nowMs)
    {
        if(m_host)
            m_host->startMatch(encodeSetup(setup), nowMs);
    }

    bool NetMatch::takeStartedMatch(MatchSetup &out)
    {
        // A new StartMatch: the match itself, or the same one again after a reconnect (played through from
        // its first turn)
        if(!m_client || !m_client->matchStarted() || m_client->matchStarts() == m_takenStarts)
            return false;
        if(!decodeSetup(m_client->matchSetup(), out))
            return false;
        out.local = m_client->slot();
        // Exactly the room's map: the own file or the downloaded copy
        const std::string file = MapFiles::resolve(out.mapPath, out.mapHash);
        if(file.empty())
        {
            m_failure = "the map " + out.mapPath + " of this match is missing or differs";
            m_takenStarts = m_client->matchStarts();
            return false;
        }
        out.mapPath = file;
        m_takenStarts = m_client->matchStarts();
        m_scheduledTurn = 0;
        m_outgoing.clear();
        return true;
    }

    void NetMatch::attach(Simulation &sim)
    {
        m_sim = &sim;
        m_outgoing.clear();
        sim.setCommandSink([this](SimCommand c) { m_outgoing.push_back(std::move(c)); });
    }

    void NetMatch::detach()
    {
        if(m_sim)
            m_sim->setCommandSink(nullptr);
        m_sim = nullptr;
    }

    bool NetMatch::canRunTick(uint32_t tick)
    {
        if(!m_client || !m_sim)
            return true;
        const uint32_t tpt = std::max<uint32_t>(1, m_client->ticksPerTurn());
        const uint32_t turn = tick / tpt;
        if(turn < m_scheduledTurn)
            return true;
        const Net::TurnBundle *bundle = m_client->turn(turn);
        if(!bundle)
            return false;
        // Everybody's commands of this turn on its first tick, in the bundle's order
        for(const Net::PlayerTurn &p : bundle->players)
        {
            std::vector<SimCommand> commands;
            if(!SimCommandCodec::decode(p.bytes.data(), p.bytes.size(), p.slot, commands))
                continue;
            for(SimCommand &c : commands)
            {
                c.debug = false; // no debug powers in a network game
                m_sim->schedule(turn * tpt, std::move(c));
            }
        }
        m_scheduledTurn = turn + 1;
        m_client->dropTurnsBefore(turn);
        return true;
    }

    void NetMatch::afterTick(uint32_t tick, uint64_t checksum)
    {
        if(!m_client || !m_sim)
            return;
        const uint32_t tpt = std::max<uint32_t>(1, m_client->ticksPerTurn());
        // `tick` ran; the last tick of a turn ends it: what the player did meanwhile goes out
        if((tick + 1) % tpt == 0 && !m_outgoing.empty())
        {
            const uint32_t turn = tick / tpt + m_client->inputDelay();
            m_client->sendCommands(turn, SimCommandCodec::encode(m_outgoing));
            m_outgoing.clear();
        }
        if(tick % kHashIntervalTicks == 0)
        {
            m_client->sendWorldHash(tick, checksum);
            // OAOW_NET_TRACE=1: the checksums on stdout, to compare two peers by eye or script
            static const bool trace = std::getenv("OAOW_NET_TRACE") != nullptr;
            if(trace && tick % (kHashIntervalTicks * 10) == 0)
            {
                std::printf("[Net] tick %u checksum %016llx\n", tick, static_cast<unsigned long long>(checksum));
                std::fflush(stdout);
            }
        }
    }

    int NetMatch::turnsAhead(uint32_t tick) const
    {
        if(!m_client)
            return 0;
        const uint32_t tpt = std::max<uint32_t>(1, m_client->ticksPerTurn());
        return static_cast<int>(m_client->turnsReceived()) - static_cast<int>(tick / tpt) - 1;
    }

    std::vector<uint8_t> NetMatch::encodeSetup(const MatchSetup &setup)
    {
        std::vector<uint8_t> buf(Net::kMaxMatchSetupBytes);
        Net::BitWriter w(buf.data(), buf.size());
        w.writeString(setup.mapPath, 256);
        w.writeU64(setup.mapHash);
        w.writeU32(setup.seed);
        w.writeVarInt(setup.startingCredits);
        w.writeBool(setup.fogOfWar);
        w.writeBool(setup.prebuilt);
        for(int i = 0; i < kMaxPlayers; ++i)
        {
            const PlayerSlot &s = setup.slots[i];
            w.writeBool(s.active);
            if(!s.active)
                continue;
            w.writeBits(static_cast<uint32_t>(s.faction), 2);
            w.writeBits(s.team, 3);
            w.writeBits(s.color, 4);
            w.writeBool(s.human);
            if(!s.human)
                w.writeBits(s.aiLevel & 3u, 2);
            w.writeBits(static_cast<uint32_t>(setup.startPosition[i]) & 7u, 3);
        }
        buf.resize(w.finish());
        return buf;
    }

    bool NetMatch::decodeSetup(const std::vector<uint8_t> &bytes, MatchSetup &out)
    {
        Net::BitReader r(bytes.data(), bytes.size());
        out = MatchSetup {};
        out.mapPath = r.readString(256);
        out.mapHash = r.readU64();
        out.seed = r.readU32();
        out.startingCredits = static_cast<int>(r.readVarInt());
        out.fogOfWar = r.readBool();
        out.prebuilt = r.readBool();
        for(int i = 0; i < kMaxPlayers; ++i)
        {
            PlayerSlot &s = out.slots[i];
            s.active = r.readBool();
            if(!s.active)
                continue;
            const uint32_t faction = r.readBits(2);
            if(faction > static_cast<uint32_t>(Faction::Confederation))
                return false;
            s.faction = static_cast<Faction>(faction);
            s.team = static_cast<uint8_t>(r.readBits(3));
            s.color = static_cast<uint8_t>(r.readBits(4));
            s.human = r.readBool();
            if(!s.human)
                s.aiLevel = static_cast<uint8_t>(r.readBits(2));
            out.startPosition[i] = static_cast<int8_t>(r.readBits(3));
        }
        // Only maps of the game's own folder (the path comes from another computer)
        if(out.mapPath.rfind("assets/maps/", 0) != 0 || out.mapPath.find("..") != std::string::npos)
            return false;
        return !r.failed();
    }

} // namespace OpenArtOfWar
