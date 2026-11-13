#include "core/Localization.hpp"
#include <SDL3/SDL.h>
#include "net/NetMatch.hpp"
#include "net/WorldSnapshot.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/DebugEnv.hpp"
#include "game/MissionController.hpp"
#include "net/MapFiles.hpp"
#include "game/SimCommandCodec.hpp"
#include "game/Simulation.hpp"
#include "openaow/net/BitStream.hpp"
#include "openaow/net/Crc32.hpp"
#include "openaow/net/Discovery.hpp"
#include "openaow/net/Rendezvous.hpp"
#include "openaow/net/Session.hpp"
#include "SimBuildIdGenerated.hpp"

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

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

Net::GameIdentity Identity()
{
    Net::GameIdentity id;
    id.gameVersion = OAOW_VERSION;
    id.simBuildId = NetMatch::SimBuildId();
    id.assetsHash = NetMatch::AssetsHash();
    return id;
}

} // namespace

std::vector<std::vector<std::uint8_t>> NetMatch::EncodeBatches(const std::vector<SimCommand> &commands)
{
    std::vector<std::vector<std::uint8_t>> out;
    std::size_t begin = 0;
    while (begin < commands.size())
    {
        std::size_t count = std::min(commands.size() - begin, SimCommandCodec::kMaxCommandsPerTurn);
        std::vector<SimCommand> part(commands.begin() + begin, commands.begin() + begin + count);
        std::vector<std::uint8_t> bytes = SimCommandCodec::Encode(part);
        while (bytes.size() > Net::kMaxPlayerCommandBytes && count > 1)
        {
            count = (count + 1) / 2;
            part.resize(count);
            bytes = SimCommandCodec::Encode(part);
        }
        if (bytes.size() <= Net::kMaxPlayerCommandBytes)
            out.push_back(std::move(bytes));
        else
            std::cerr << "[Net] A command too big for one batch was dropped" << std::endl;
        begin += count;
    }
    return out;
}

std::size_t NetMatch::CoalesceTurn(std::vector<SimCommand> &commands)
{
    // A move / attack-move without Shift releases the units from their march and clears their orders first
    // (Simulation::Apply), so one right after another for exactly the same units leaves no trace of the first
    auto replacingOrder = [](const SimCommand &c) {
        return c.type == CommandType::OrderAt && (c.b & 2) == 0 && !c.ids.empty();
    };
    auto sameUnits = [](const SimCommand &l, const SimCommand &r) {
        if (l.player != r.player || l.debug != r.debug || l.ids.size() != r.ids.size())
            return false;
        std::vector<std::uint32_t> a = l.ids, b = r.ids;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        return a == b;
    };
    std::vector<bool> drop(commands.size(), false);
    int lastView = -1;
    for (std::size_t i = 0; i < commands.size(); ++i)
    {
        const SimCommand &c = commands[i];
        if (c.type == CommandType::View)
        {
            if (lastView >= 0)
                drop[static_cast<std::size_t>(lastView)] = true;
            lastView = static_cast<int>(i);
            continue;
        }
        if (i + 1 < commands.size() && replacingOrder(c) && replacingOrder(commands[i + 1])
            && sameUnits(c, commands[i + 1]))
            drop[i] = true;
    }
    std::size_t kept = 0, dropped = 0;
    for (std::size_t i = 0; i < commands.size(); ++i)
    {
        if (drop[i])
        {
            ++dropped;
            continue;
        }
        if (kept != i)
            commands[kept] = std::move(commands[i]);
        ++kept;
    }
    commands.resize(kept);
    return dropped;
}

std::uint64_t NetMatch::SimBuildId()
{
    // The simulation code: the fingerprint of its sources (cmake/SimBuildId.cmake, computed at build time) changes
    // with any edit of AI, units, pathfinding, ..., so builds that would play differently never meet in a match
    // and replays of another build are known as such. The build is deterministic across platforms (DetMath,
    // -ffp-contract=off). The tag is only for a change outside the sources (a compiler option, say)
    const std::string v = std::string(OAOW_VERSION) + "|sim1";
    std::uint32_t crc = Net::crc32(v.data(), v.size());
    const std::uint64_t source = kSimSourceHash;
    crc = Net::crc32(&source, sizeof(source), crc);
    return (static_cast<std::uint64_t>(crc) << 32) ^ source;
}

NetMatch::NetMatch() = default;
NetMatch::~NetMatch()
{
    leave();
}

std::uint64_t NetMatch::AssetsHash()
{
    // CRC of the manifest's simulation part and of every file the simulation reads, names included, in a fixed order;
    // two CRCs side by side make a 64-bit value. Read from the assets as they are now: a session overlay counts.
    std::uint32_t a = 0, b = 0x9e3779b9u;
    const std::string manifest = LoaderResources::NetManifestText();
    a = Net::crc32(manifest.data(), manifest.size(), a);
    b = Net::crc32(manifest.data(), manifest.size(), b ^ static_cast<std::uint32_t>(manifest.size()));
    std::vector<std::string> files = LoaderResources::NetHashFiles();
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    std::vector<std::uint8_t> bytes;
    for (const std::string &f : files)
    {
        if (!Assets::ReadBytes(f, bytes))
            continue;
        a = Net::crc32(f.data(), f.size(), a);
        a = Net::crc32(bytes.data(), bytes.size(), a);
        b = Net::crc32(bytes.data(), bytes.size(), b ^ static_cast<std::uint32_t>(bytes.size()));
    }
    return (static_cast<std::uint64_t>(a) << 32) | b;
}

std::vector<Net::DataEntry> NetMatch::DataManifest()
{
    std::vector<std::string> files = LoaderResources::NetHashFiles();
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    std::vector<Net::DataEntry> out;
    std::vector<std::uint8_t> bytes;
    for (const std::string &f : files)
    {
        // A file that cannot be read is listed with hash 0: it differs from any file of the other side
        out.push_back({f, Assets::ReadBytes(f, bytes) ? MapFiles::HashOfBytes(bytes) : 0});
    }
    const std::string manifest = LoaderResources::NetManifestText();
    out.push_back({LoaderResources::kManifestPath, MapFiles::HashOfBytes({manifest.begin(), manifest.end()})});
    return out;
}

namespace
{
// A new session starts with the player's own data (a leftover overlay of an earlier host is dropped)
void DropOldOverlay(bool &changed)
{
    if (Assets::HasSessionOverlay())
    {
        Assets::ClearSessionOverlay();
        changed = true;
    }
}
} // namespace

bool NetMatch::host(std::uint16_t port, const std::string &playerName, std::string &error)
{
    leave();
    DropOldOverlay(m_dataChanged);
    Net::HostConfig hc;
    hc.identity = Identity();
    hc.roomName = playerName;
    hc.password = m_password;
    // A player whose game data differ can fetch these files from this host (see DataSync)
    hc.dataManifest = DataManifest();
    hc.dataFile = [](const std::string &path, std::vector<std::uint8_t> &packed, std::uint64_t &hash) {
        const std::vector<std::string> allowed = LoaderResources::NetHashFiles();
        if (std::find(allowed.begin(), allowed.end(), path) == allowed.end())
            return false;
        packed = MapFiles::Packed(path);
        hash = MapFiles::HashOf(path);
        return !packed.empty() && hash != 0;
    };
    // Another game hosted on this computer holds the port: the next ones of the span (browsers probe them)
    std::unique_ptr<Net::Transport> server;
    for (std::uint16_t p = 0; p < Net::kDiscoveryPortSpan && !server; ++p)
    {
        server = Net::createEnetServer(static_cast<std::uint16_t>(port + p), Net::kMaxPlayers + 4, &error);
        if (server)
            m_gamePort = static_cast<std::uint16_t>(port + p);
    }
    if (!server)
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
    return Identity();
}

bool NetMatch::IsOwner() const
{
    if (!m_dedicated || !connected())
        return false;
    int lowest = Net::kMaxPlayers;
    for (const Net::RosterEntry &e : m_client->roster().players)
    {
        if (e.connected)
            lowest = std::min<int>(lowest, e.slot);
    }
    return lowest == m_client->slot();
}

bool NetMatch::join(
    const std::string &address, std::uint16_t port, const std::string &playerName, std::string &error, bool dedicated)
{
    leave();
    DropOldOverlay(m_dataChanged);
    m_dedicated = dedicated;
    m_address = address;
    m_port = port;
    m_name = playerName;
    m_joinStartMs = SDL_GetTicks();
    auto transport = Net::createEnetClient(address, port, &error);
    if (!transport)
        return false;
    Net::ClientConfig cc;
    cc.identity = Identity();
    cc.playerName = playerName;
    cc.password = m_password;
    cc.canSync = !m_synced;
    m_client = std::make_unique<Net::SessionClient>(std::move(transport), cc);
    return true;
}

void NetMatch::Announce(const std::string &master, std::uint16_t masterPort)
{
    if (!m_host || !m_gameSocket || m_announcer)
        return;
    m_announcer
        = std::make_unique<Net::HostAnnouncer>(master, masterPort, *m_gameSocket, m_gamePort, Net::localLanIp());
}

const std::string &NetMatch::PublicCode() const
{
    static const std::string kNone;
    return m_announcer ? m_announcer->code() : kNone;
}

bool NetMatch::joinHosted(
    const std::string &master,
    std::uint16_t masterPort,
    const std::string &code,
    const std::string &playerName,
    std::string &error)
{
    leave();
    DropOldOverlay(m_dataChanged);
    m_joinStartMs = SDL_GetTicks();
    m_hostedJoin = std::make_unique<Net::HostedJoin>(master, masterPort, code);
    if (m_hostedJoin->state() == Net::HostedJoin::State::Failed)
    {
        error = m_hostedJoin->error();
        m_hostedJoin.reset();
        return false;
    }
    m_name = playerName;
    m_hostedMaster = master;
    m_hostedMasterPort = masterPort;
    m_hostedCode = code;
    m_rejoinToken = {};
    return true;
}

void NetMatch::leave()
{
    FinishReplay("left");
    // Leaving for good: the host frees the slot at once instead of waiting for a comeback
    if (m_client)
        m_client->sendLeave();
    Detach();
    m_announcer.reset();
    m_hostedJoin.reset();
    m_gameSocket = nullptr;
    m_client.reset();
    m_host.reset();
    m_loopback.reset();
    m_takenStarts = 0;
    m_dedicated = false;
    m_sync = DataSync {};
    m_synced = false;
    m_syncText.clear();
    m_scheduledTurn = 0;
    m_reconnect = Reconnect {};
    m_hostedMaster.clear();
    m_hostedMasterPort = 0;
    m_hostedCode.clear();
    m_rejoinToken = {};
}

void NetMatch::Update(std::uint32_t nowMs)
{
    m_nowMs = nowMs;
    // The room has echoed the own picks: its copy is current again
    if (m_hasPendingChoice && m_client)
    {
        if (m_nowMs - m_pendingChoiceMs >= 1000)
            m_hasPendingChoice = false;
        for (const Net::LobbySlot &s : m_client->lobby().slots)
        {
            if (s.slot == LocalSlot() && s.choice.faction == m_pendingChoice[0] && s.choice.team == m_pendingChoice[1]
                && s.choice.color == m_pendingChoice[2] && s.choice.spawn == m_pendingChoice[3]
                && s.choice.ready == (m_pendingChoice[4] != 0))
                m_hasPendingChoice = false;
        }
    }
    // Through the server to a player-hosted game: when the way is open, the session takes the socket
    if (m_hostedJoin)
    {
        m_hostedJoin->update(nowMs);
        if (m_hostedJoin->state() == Net::HostedJoin::State::Ready)
        {
            Net::ClientConfig cc;
            cc.identity = Identity();
            cc.playerName = m_name;
            cc.password = m_password;
            cc.canSync = !m_synced;
            cc.token = m_rejoinToken; // set when this is a comeback after a lost connection
            m_client = std::make_unique<Net::SessionClient>(m_hostedJoin->takeTransport(), cc);
            m_hostedJoin.reset();
        }
        else if (m_hostedJoin->state() == Net::HostedJoin::State::Failed)
        {
            // A comeback attempt that failed is tried again (below) until the reconnect window ends
            if (!m_reconnect.active)
                m_failure = "server: " + m_hostedJoin->error();
            m_hostedJoin.reset();
        }
    }
    if (m_announcer && m_host)
        m_announcer->update(nowMs, m_host->roomInfo(), true);
    if (m_client)
        m_client->update(nowMs);
    if (m_host)
        m_host->update(nowMs);
    if (m_client)
        m_client->update(nowMs);
    UpdateDataSync();

    // Lost in the middle of a match: back in with the session token, backing off between the tries
    if (!m_host && m_client && m_takenStarts > 0 && m_client->state() == Net::SessionClient::State::Disconnected)
    {
        if (!m_reconnect.active)
        {
            m_reconnect = Reconnect {true, m_address, m_port, m_name, nowMs + m_reconnectWindowMs, nowMs + 500, 500};
            std::cerr << "[Net] Connection lost, reconnecting" << std::endl;
        }
        if (nowMs >= m_reconnect.deadlineMs)
            return; // given up: the game shows it
        if (nowMs >= m_reconnect.nextAttemptMs && m_reconnect.address.empty() && !m_hostedCode.empty())
        {
            // Joined by code: punch through the server again, the session token takes the slot back
            if (!m_hostedJoin)
            {
                m_rejoinToken = m_client->token();
                m_hostedJoin = std::make_unique<Net::HostedJoin>(m_hostedMaster, m_hostedMasterPort, m_hostedCode);
            }
            m_reconnect.delayMs = std::min<std::uint32_t>(m_reconnect.delayMs * 2, 4000);
            m_reconnect.nextAttemptMs = nowMs + m_reconnect.delayMs;
        }
        else if (nowMs >= m_reconnect.nextAttemptMs)
        {
            std::string error;
            auto transport = Net::createEnetClient(m_reconnect.address, m_reconnect.port, &error);
            if (transport)
            {
                Net::ClientConfig cc;
                cc.identity = Identity();
                cc.playerName = m_reconnect.name;
                cc.password = m_password;
                cc.canSync = !m_synced;
                cc.token = m_client->token();
                m_client = std::make_unique<Net::SessionClient>(std::move(transport), cc);
            }
            m_reconnect.delayMs = std::min<std::uint32_t>(m_reconnect.delayMs * 2, 4000);
            m_reconnect.nextAttemptMs = nowMs + m_reconnect.delayMs;
        }
    }
    UpdateMapFile();
    if (m_reconnect.active && m_client && m_client->state() == Net::SessionClient::State::Joined)
    {
        m_reconnect.active = false;
        std::cerr << "[Net] Reconnected" << std::endl;
    }
}

// ---- Joining a host with other simulation data: fetch its files, play with them for this session ----

void NetMatch::FailDataSync(const std::string &files)
{
    m_failure = std::string(_("Your game data differ from the host's and could not be synced: ")) + files;
    std::cerr << "[Net] Data sync failed: " << files << std::endl;
    m_sync = DataSync {};
    m_syncText.clear();
    if (m_client)
        m_client->disconnect();
}

void NetMatch::StartDataSync(const std::vector<Net::DataEntry> &theirs)
{
    std::map<std::string, std::uint64_t> own;
    for (const Net::DataEntry &e : DataManifest())
        own[e.path] = e.hash;
    const std::vector<std::string> allowed = LoaderResources::NetHashFiles();
    m_sync = DataSync {};
    std::string unfixable;
    auto note = [&](const std::string &path) {
        unfixable += (unfixable.empty() ? "" : ", ") + path.substr(path.find_last_of('/') + 1);
    };
    for (const Net::DataEntry &e : theirs)
    {
        const auto it = own.find(e.path);
        if (it != own.end() && it->second == e.hash)
            continue;
        // Only files this game itself lists as simulation data are taken, nothing else a host may name
        const bool listed = std::find(allowed.begin(), allowed.end(), e.path) != allowed.end();
        if (!listed || e.hash == 0)
            note(e.path);
        else
            m_sync.wanted.push_back(e);
    }
    for (const auto &entry : own)
    {
        const bool known
            = std::any_of(theirs.begin(), theirs.end(), [&](const Net::DataEntry &e) { return e.path == entry.first; });
        if (!known)
            note(entry.first);
    }
    if (!unfixable.empty() || m_sync.wanted.empty())
    {
        FailDataSync(unfixable.empty() ? std::string(_("(the game's file lists differ)")) : unfixable);
        return;
    }
    m_sync.active = true;
    for (const Net::DataEntry &e : m_sync.wanted)
        m_client->requestDataFile(e.path);
    m_syncText = std::string(_("Downloading game data")) + " (0/" + std::to_string(m_sync.wanted.size()) + ")";
    std::cout << "[Net] Game data differ from the host's: fetching " << m_sync.wanted.size() << " file(s)" << std::endl;
}

void NetMatch::AcceptDataFile(const Net::FilePathData &file)
{
    const auto want = std::find_if(
        m_sync.wanted.begin(), m_sync.wanted.end(), [&](const Net::DataEntry &e) { return e.path == file.path; });
    // Anything not asked for, or twice, is ignored
    const std::string prefix = "assets/";
    if (want == m_sync.wanted.end() || file.path.compare(0, prefix.size(), prefix) != 0)
        return;
    const std::string key = file.path.substr(prefix.size());
    if (m_sync.got.count(key))
        return;
    std::vector<std::uint8_t> raw;
    // The hash it is announced with must be the one of the host's list, and the bytes must have it
    if (file.hash != want->hash || !MapFiles::Unpack(file.bytes, file.hash, raw, Net::kMaxFileBytes * 8))
    {
        FailDataSync(file.path.substr(file.path.find_last_of('/') + 1));
        return;
    }
    m_sync.bytes += raw.size();
    // It must read as what its name says: a hostile host gets no further than the parser
    bool readable = m_sync.bytes <= 4u * 1024 * 1024;
    if (readable && file.path.size() > 5 && file.path.compare(file.path.size() - 5, 5, ".json") == 0)
        readable = !nlohmann::json::parse(raw.begin(), raw.end(), nullptr, false).is_discarded();
    else if (readable && file.path.size() > 5 && file.path.compare(file.path.size() - 5, 5, ".yaml") == 0)
    {
        try
        {
            YAML::Load(std::string(raw.begin(), raw.end()));
        }
        catch (...)
        {
            readable = false;
        }
    }
    if (!readable)
    {
        FailDataSync(file.path.substr(file.path.find_last_of('/') + 1));
        return;
    }
    m_sync.got[key] = std::move(raw);
    m_syncText = std::string(_("Downloading game data")) + " (" + std::to_string(m_sync.got.size()) + "/"
                 + std::to_string(m_sync.wanted.size()) + ")";
}

void NetMatch::UpdateDataSync()
{
    if (!m_client || m_host)
        return;
    std::vector<Net::DataEntry> theirs;
    if (m_client->takeDataManifest(theirs))
    {
        if (m_synced)
            FailDataSync(_("(the data still differ after the sync)"));
        else
            StartDataSync(theirs);
    }
    Net::FilePathData file;
    while (m_sync.active && m_client->takeDataFile(file))
        AcceptDataFile(file);
    if (m_sync.active && m_sync.got.size() == m_sync.wanted.size())
    {
        // All here: play with the host's files from now on, and say hello again with the data we have now
        Assets::SetSessionOverlay(std::move(m_sync.got));
        m_sync = DataSync {};
        m_synced = true;
        m_dataChanged = true;
        m_syncText.clear();
        m_client->sayHelloAgain(AssetsHash());
        std::cout << "[Net] Playing with the host's game data" << std::endl;
    }
}

void NetMatch::TogglePause()
{
    if (m_client)
        m_client->requestPause(!m_client->pauseState().paused);
}
bool NetMatch::paused() const
{
    return m_client && m_client->pauseState().paused;
}
std::uint8_t NetMatch::PausedBy() const
{
    return m_client ? m_client->pauseState().bySlot : 0;
}
std::uint8_t NetMatch::PausesLeft() const
{
    return m_client ? m_client->pauseState().pausesLeft : 0;
}

void NetMatch::DropConnection()
{
    if (m_client && !m_host)
        m_client->disconnect();
}

// The room's map on this computer: the own file when it is the room's version, else a downloaded copy,
// else ask the room for it
void NetMatch::UpdateMapFile()
{
    if (!m_client)
        return;
    std::uint64_t hash = 0;
    std::vector<std::uint8_t> bytes;
    while (m_client->takeFile(hash, bytes))
    {
        const Net::LobbyState &lobby = m_client->lobby();
        if (hash == lobby.mapHash && !MapFiles::Store(lobby.map, hash, bytes).empty())
            m_lobbyMapChecked.clear();
    }
    const Net::LobbyState &lobby = m_client->lobby();
    const std::string key = lobby.map + "#" + std::to_string(lobby.mapHash);
    if (key == m_lobbyMapChecked)
        return;
    if (lobby.map.empty())
    {
        m_lobbyMapFile.clear();
        return;
    }
    m_lobbyMapFile = MapFiles::Resolve(lobby.map, lobby.mapHash);
    if (!m_lobbyMapFile.empty())
    {
        m_lobbyMapChecked = key;
    }
    else if (m_requestedMap != lobby.mapHash)
    {
        m_requestedMap = lobby.mapHash;
        m_client->requestFile(lobby.mapHash);
        std::cout << "[Net] Downloading the map " << lobby.map << std::endl;
    }
}

std::uint32_t NetMatch::DesyncTick() const
{
    return m_client ? m_client->desyncTick() : 0;
}

std::uint32_t NetMatch::TicksReceived() const
{
    return m_client ? m_client->turnsReceived() * std::max<std::uint32_t>(1, m_client->ticksPerTurn()) : 0;
}

float NetMatch::StalledSeconds(std::uint32_t nowMs) const
{
    if (!m_client || m_lastTickMs == 0 || nowMs < m_lastTickMs)
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

Net::RejectReason NetMatch::RejectedWhy() const
{
    return m_client ? m_client->rejectReason() : Net::RejectReason::None;
}
const std::string &NetMatch::RejectedDetail() const
{
    static const std::string kNone;
    return m_client ? m_client->rejectDetail() : kNone;
}

bool NetMatch::JoinTimedOut(std::uint32_t nowMs) const
{
    if (m_host || m_reconnect.active || m_takenStarts > 0 || connected() || m_joinStartMs == 0)
        return false;
    const bool waiting = m_hostedJoin || (m_client && m_client->state() == Net::SessionClient::State::Connecting);
    return waiting && nowMs - m_joinStartMs > kJoinTimeoutMs;
}

std::string NetMatch::ConnectingText(std::uint32_t nowMs) const
{
    const std::string target = m_hostedJoin || m_address.empty() ? std::string(_("the game through the server"))
                                                                 : m_address + ":" + std::to_string(m_port);
    const std::uint32_t seconds = m_joinStartMs ? (nowMs - m_joinStartMs) / 1000 : 0;
    if (!m_syncText.empty())
        return m_syncText;
    return std::string(_("Connecting to")) + " " + target + "...  " + std::to_string(seconds) + " / "
           + std::to_string(kJoinTimeoutMs / 1000) + " " + _("s");
}
std::uint8_t NetMatch::LocalSlot() const
{
    return m_client ? m_client->slot() : 0;
}
bool NetMatch::Desynced() const
{
    return m_client && m_client->desynced();
}

int NetMatch::PlayerCount() const
{
    if (m_host)
        return m_host->playerCount();
    return m_client ? static_cast<int>(m_client->roster().players.size()) : 0;
}

std::string NetMatch::StatusText() const
{
    if (m_hostedJoin)
        return "through the server";
    if (!m_client)
        return "offline";
    switch (m_client->state())
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

void NetMatch::SetLobbyMap(const std::string &mapPath)
{
    if (!ControlsRoom())
        return;
    const std::uint8_t spots = static_cast<std::uint8_t>(
        std::min<std::size_t>(MissionController::StartPositions(mapPath).size(), kMaxPlayers));
    const std::uint64_t hash = MapFiles::HashOf(mapPath);
    // The room hands the map out to whoever does not have this very file (~20 KB compressed)
    const std::vector<std::uint8_t> packed = MapFiles::Packed(mapPath);
    if (m_host)
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

const Net::LobbyState *NetMatch::Lobby() const
{
    return m_client ? &m_client->lobby() : nullptr;
}
const Net::Roster *NetMatch::roster() const
{
    return m_client ? &m_client->roster() : nullptr;
}
void NetMatch::SetChoice(const Net::LobbyChoice &choice)
{
    if (!m_client)
        return;
    Net::LobbyChoice c = choice;
    // Only the classic look (0) and the eight team colours exist (9..15 drew white)
    if (c.color > 8)
        c.color = static_cast<std::uint8_t>(1 + LocalSlot() % 8);
    m_client->sendLobbyChoice(c);
    m_pendingChoice = {c.faction, c.team, c.color, c.spawn, static_cast<std::uint8_t>(c.ready ? 1 : 0)};
    m_hasPendingChoice = true;
    m_pendingChoiceMs = m_nowMs;
}

bool NetMatch::LocalChoice(Net::LobbyChoice &out) const
{
    if (!m_client)
        return false;
    bool found = false;
    for (const Net::LobbySlot &s : m_client->lobby().slots)
    {
        if (s.slot == LocalSlot())
        {
            out = s.choice;
            found = true;
        }
    }
    if (!found)
        return false;
    // Until the room echoes what was sent (or 1 s went by: the room may have refused or changed it)
    if (m_hasPendingChoice && m_nowMs - m_pendingChoiceMs < 1000)
    {
        out.faction = m_pendingChoice[0];
        out.team = m_pendingChoice[1];
        out.color = m_pendingChoice[2];
        out.spawn = m_pendingChoice[3];
        out.ready = m_pendingChoice[4] != 0;
    }
    return true;
}

void NetMatch::SetAi(const Net::OwnerSetAi &change)
{
    if (m_host)
        m_host->setAiSlot(change);
    else if (IsOwner())
        m_client->sendOwnerSetAi(change);
}
void NetMatch::SetTeam(
    std::uint8_t slot, std::uint8_t team, std::uint8_t handicap, std::uint8_t armyCap, std::uint8_t techLevel)
{
    if (armyCap > 7)
    {
        armyCap = 7;
        if (const Net::LobbyState *lobby = Lobby())
            for (const Net::LobbySlot &s : lobby->slots)
                if (s.slot == slot)
                    armyCap = s.choice.armyCap;
    }
    if (m_hasPendingChoice && slot == LocalSlot())
        m_pendingChoice[1] = team;
    if (m_host)
        m_host->setTeam(slot, team, handicap, armyCap, techLevel);
    else if (IsOwner())
        m_client->sendOwnerSetTeam(Net::OwnerSetTeam {slot, team, handicap, armyCap, techLevel});
}
void NetMatch::SetSeat(const Net::OwnerSetSeat &change)
{
    if (m_host)
        m_host->setSeat(change);
    else if (IsOwner())
        m_client->sendOwnerSetSeat(change);
}
void NetMatch::SetOptions(bool fogOfWar, bool prebuilt, std::uint8_t credits, bool night, std::uint8_t techLevel)
{
    if (m_host)
        m_host->setOptions(fogOfWar, prebuilt, credits, night, techLevel);
    else if (IsOwner())
        m_client->sendOwnerSetOptions(Net::OwnerSetOptions {fogOfWar, prebuilt, credits, night, techLevel});
}
void NetMatch::SendChat(const std::string &text)
{
    if (m_client && !text.empty())
        m_client->sendChat(text, false);
}
std::vector<Net::Chat> NetMatch::TakeChat()
{
    return m_client ? m_client->takeChat() : std::vector<Net::Chat> {};
}
bool NetMatch::CanStart() const
{
    if (m_host)
        return !m_host->lobby().map.empty() && m_host->everyoneReady(2);
    if (!IsOwner())
        return false;
    // The room's view of the owner: everybody listed in the room is ready
    const Net::LobbyState &lobby = m_client->lobby();
    if (lobby.map.empty() || lobby.slots.size() < 2)
        return false;
    return std::all_of(lobby.slots.begin(), lobby.slots.end(), [](const Net::LobbySlot &s) { return s.choice.ready; });
}

void NetMatch::StartFromLobby(std::uint32_t nowMs)
{
    if (!ControlsRoom())
        return;
    const Net::LobbyState &lobby = m_host ? m_host->lobby() : m_client->lobby();
    MatchSetup setup;
    setup.mapPath = lobby.map;
    setup.mapHash = lobby.mapHash;
    setup.seed = static_cast<std::uint32_t>(std::random_device {}()) ^ nowMs;
    setup.startingCredits = kStartingCredits[lobby.credits <= 4 ? lobby.credits : kDefaultCreditsIndex];
    setup.prebuilt = lobby.prebuilt;
    setup.fogOfWar = lobby.fogOfWar;
    setup.night = lobby.night;
    setup.authorityMode = m_authorityMode; // the owner decides for everybody
    setup.techLevel = static_cast<std::int8_t>(lobby.techLevel) - 1; // (0 in the room: the map's / the default)
    std::mt19937 rng(setup.seed);
    // Start positions picked by the players first, the random ones get the free spots left
    std::vector<int> free;
    for (int i = 0; i < lobby.spawnCount; ++i)
    {
        bool taken = false;
        for (const Net::LobbySlot &s : lobby.slots)
            taken = taken || s.choice.spawn == i;
        if (!taken)
            free.push_back(i);
    }
    std::shuffle(free.begin(), free.end(), rng);
    for (const Net::LobbySlot &s : lobby.slots)
    {
        PlayerSlot &slot = setup.slots[s.slot];
        slot.active = true;
        slot.human = !s.ai;
        slot.aiLevel = s.aiLevel;
        slot.faction = s.choice.faction == Net::kRandom ? static_cast<Faction>(rng() % 2)
                                                        : static_cast<Faction>(s.choice.faction);
        slot.team = s.choice.team;
        slot.handicap = s.choice.handicap <= 5 ? s.choice.handicap : 0;
        slot.armyCap = s.choice.armyCap < kArmyCapCount ? s.choice.armyCap : kArmyCapCount - 1;
        slot.techLevel = static_cast<std::int8_t>(s.choice.techLevel) - 1; // (0 in the room: the match's)
        slot.color = s.choice.color <= 8 ? s.choice.color : static_cast<std::uint8_t>(1 + s.slot % 8);
        int spawn = s.choice.spawn;
        if (spawn == Net::kRandom)
        {
            if (free.empty())
                return; // more players than start positions
            spawn = free.back();
            free.pop_back();
        }
        setup.startPosition[s.slot] = static_cast<std::int8_t>(spawn);
    }
    if (m_host)
        startMatch(setup, nowMs);
    else
        m_client->sendOwnerStart(EncodeSetup(setup));
}

void NetMatch::startMatch(const MatchSetup &setup, std::uint32_t nowMs)
{
    if (m_host)
        m_host->startMatch(EncodeSetup(setup), nowMs);
}

bool NetMatch::TakeStartedMatch(MatchSetup &out)
{
    // A new StartMatch: the match itself, or the same one again after a reconnect (played through from
    // its first turn)
    if (!m_client || !m_client->matchStarted() || m_client->matchStarts() == m_takenStarts)
        return false;
    if (!DecodeSetup(m_client->matchSetup(), out))
        return false;
    out.local = m_client->slot();
    // Exactly the room's map: the own file or the downloaded copy
    const std::string file = MapFiles::Resolve(out.mapPath, out.mapHash);
    if (file.empty())
    {
        m_failure = "the map " + out.mapPath + " of this match is missing or differs";
        m_takenStarts = m_client->matchStarts();
        return false;
    }
    // The replay of this match (the setup as the room sent it, the map still under the room's name)
    {
        ReplayRecorder::Info info;
        info.setupBytes = m_client->matchSetup();
        info.setup = out;
        info.names.assign(kMaxPlayers, std::string());
        for (const Net::RosterEntry &e : m_client->roster().players)
        {
            if (e.slot < kMaxPlayers)
                info.names[e.slot] = e.name;
        }
        info.ticksPerTurn = std::max<std::uint32_t>(1, m_client->ticksPerTurn());
        info.turnMs = m_client->turnMs();
        info.assetsHash = AssetsHash();
        m_recorder.Begin(std::move(info));
    }
    out.mapPath = file;
    m_authorityMode = out.authorityMode;
    m_takenStarts = m_client->matchStarts();
    m_scheduledTurn = 0;
    m_outgoing.clear();
    return true;
}

bool NetMatch::IsAuthority() const
{
    return m_client && m_client->matchStarted() && m_client->authoritySlot() == m_client->slot();
}

void NetMatch::PublishSnapshot(const WorldSnapshot &snapshot, const EntityManager &entities, const TileMap &map)
{
    if (!m_client || !IsAuthority())
        return;
    Net::AuthorityAck ack;
    while (m_client->takeAuthorityAck(ack))
    {
        if (ack.sender >= kMaxPlayers)
            continue;
        auto &peer = m_snapshotPeers[ack.sender];
        const auto found = std::find_if(peer.history.begin(), peer.history.end(), [&](const WorldSnapshot &s) { return s.tick == ack.tick; });
        if (found != peer.history.end() && (!peer.hasAck || ack.tick > peer.ack))
        {
            peer.ack = ack.tick;
            peer.hasAck = true;
        }
    }
    for (const Net::RosterEntry &p : m_client->roster().players)
    {
        if (p.slot >= kMaxPlayers || p.slot == m_client->slot())
            continue;
        auto &peer = m_snapshotPeers[p.slot];
        if (!p.connected)
        {
            peer = SnapshotPeer {};
            continue;
        }
        WorldSnapshot current = snapshot;
        current.FilterForPlayer(entities, map, p.slot);
        current.deaths.erase(std::remove_if(current.deaths.begin(), current.deaths.end(), [&](const DeathSnap &d) {
            if (peer.history.empty())
                return true;
            const auto &previous = peer.history.back().entities;
            return std::none_of(previous.begin(), previous.end(), [&](const EntitySnap &e) { return e.id == d.id; });
        }), current.deaths.end());
        for (const auto &entity : current.entities)
            peer.knownEntities.insert(entity.id);
        current.hiddenAlive.erase(std::remove_if(current.hiddenAlive.begin(), current.hiddenAlive.end(), [&](std::uint32_t id) {
            return peer.knownEntities.count(id) == 0;
        }), current.hiddenAlive.end());
        const auto base = std::find_if(peer.history.begin(), peer.history.end(), [&](const WorldSnapshot &s) { return peer.hasAck && s.tick == peer.ack; });
        WorldSnapshot outgoing = (!peer.hasAck || snapshot.tick < peer.ack || snapshot.tick - peer.ack > 40 || base == peer.history.end() || snapshot.tick % 100 == 0)
            ? current : current.DeltaFrom(*base);
        Net::AuthorityState state;
        state.tick = snapshot.tick;
        state.recipient = p.slot;
        state.bytes = outgoing.Encode();
        if (state.bytes.empty() || !m_client->sendAuthorityState(state))
            continue;
        peer.bytes += state.bytes.size();
        const std::uint32_t now = SDL_GetTicks();
        if (!peer.traceStart)
            peer.traceStart = now;
        if (DebugEnv("OAOW_NET_TRACE") && now - peer.traceStart >= 1000)
        {
            std::printf("[Net] recipient %u snapshot bytes %llu in %u ms\n", p.slot, static_cast<unsigned long long>(peer.bytes), now - peer.traceStart);
            peer.bytes = 0;
            peer.traceStart = now;
        }
        current.projectiles.clear(); current.cues.clear(); current.blasts.clear(); current.explosions.clear(); current.deaths.clear(); current.groundMarks.clear();
        current.hasWalls = false; current.walls.clear();
        if (DebugEnv("OAOW_NET_TRACE") && current.tick % 20 == 0)
        {
            // What this recipient is entitled to see, hashed: the replica prints the hash of what it holds (`repl`)
            std::uint64_t h = 1469598103934665603ull;
            WorldSnapshot plain = current;
            plain.hiddenAlive.clear();
            const std::vector<std::uint8_t> bytes = plain.Encode();
            for (std::uint8_t b : bytes)
                h = (h ^ b) * 1099511628211ull;
            std::printf("[Snap] auth tick %u entities %zu bytes %zu hash %016llx\n", current.tick, plain.entities.size(), bytes.size(),
                static_cast<unsigned long long>(h));
            std::fflush(stdout);
        }
        peer.history.push_back(std::move(current));
        if (peer.history.size() > 64)
            peer.history.pop_front();
    }
}

bool NetMatch::TakeSnapshot(WorldSnapshot &out)
{
    Net::AuthorityState state;
    while (m_client && m_client->takeAuthorityState(state))
    {
        if (!WorldSnapshot::Decode(state.bytes.data(), state.bytes.size(), out) || out.tick != state.tick)
            continue;
        if (!out.full)
        {
            const auto base = std::find_if(m_replicaSnapshots.begin(), m_replicaSnapshots.end(), [&](const WorldSnapshot &s) { return s.tick == out.baseTick; });
            if (base == m_replicaSnapshots.end() || !out.ExpandFrom(*base))
                continue;
        }
        return true;
    }
    return false;
}

void NetMatch::AckSnapshot(const WorldSnapshot &snapshot)
{
    if (!m_client)
        return;
    WorldSnapshot saved = snapshot;
    saved.projectiles.clear(); saved.cues.clear(); saved.blasts.clear(); saved.explosions.clear(); saved.deaths.clear(); saved.groundMarks.clear();
    saved.hasWalls = false; saved.walls.clear();
    m_replicaSnapshots.push_back(std::move(saved));
    if (m_replicaSnapshots.size() > 64)
        m_replicaSnapshots.pop_front();
    m_client->sendAuthorityAck(snapshot.tick);
}

void NetMatch::PumpReplica(std::uint32_t nowMs)
{
    (void)nowMs;
    if (!IsReplica())
        return;
    // Nobody simulates the turns here: only the authority applies them
    while (const Net::TurnBundle *bundle = m_client->turn(m_scheduledTurn))
    {
        m_recorder.OnBundle(*bundle);
        m_client->dropTurnsBefore(m_scheduledTurn);
        ++m_scheduledTurn;
    }
    if (m_outgoing.empty())
        return;
    // The host's newest turn is the clock here: what the player did goes out for a turn a little ahead of it
    const std::uint32_t turn = m_client->turnsReceived() + m_client->inputDelay();
    CoalesceTurn(m_outgoing);
    for (const std::vector<std::uint8_t> &bytes : EncodeBatches(m_outgoing))
        m_client->sendCommands(turn, bytes);
    m_replicaSentTurn = turn + 1;
    m_outgoing.clear();
}

void NetMatch::Attach(Simulation &sim)
{
    m_snapshotPeers = {};
    m_replicaSnapshots.clear();
    m_sim = &sim;
    sim.SetExecutionMode(
        !m_authorityMode ? Simulation::ExecutionMode::Standalone
                         : (IsAuthority() ? Simulation::ExecutionMode::Authority : Simulation::ExecutionMode::Replica));
    m_outgoing.clear();
    sim.SetCommandSink([this](SimCommand c) { m_outgoing.push_back(std::move(c)); });
}

void NetMatch::Detach()
{
    if (m_sim)
    {
        m_sim->SetCommandSink(nullptr);
        m_sim->SetExecutionMode(Simulation::ExecutionMode::Standalone);
    }
    m_sim = nullptr;
}

bool NetMatch::CanRunTick(std::uint32_t tick)
{
    if (!m_client || !m_sim)
        return true;
    if (!m_sim->AdvancesWorld())
        return false; // a replica never ticks on its own
    const std::uint32_t tpt = std::max<std::uint32_t>(1, m_client->ticksPerTurn());
    const std::uint32_t turn = tick / tpt;
    if (turn < m_scheduledTurn)
        return true;
    const Net::TurnBundle *bundle = m_client->turn(turn);
    if (!bundle)
        return false;
    m_recorder.OnBundle(*bundle);
    ScheduleBundle(*m_sim, *bundle, tpt);
    m_scheduledTurn = turn + 1;
    m_client->dropTurnsBefore(turn);
    return true;
}

void NetMatch::ScheduleBundle(Simulation &sim, const Net::TurnBundle &bundle, std::uint32_t ticksPerTurn)
{
    if (!sim.AdvancesWorld())
        return;
    const std::uint32_t first = bundle.turn * ticksPerTurn;
    for (const Net::PlayerTurn &p : bundle.players)
    {
        std::vector<SimCommand> commands;
        if (!SimCommandCodec::Decode(p.bytes.data(), p.bytes.size(), p.slot, commands))
            continue;
        for (SimCommand &c : commands)
        {
            if (c.type == CommandType::PlayerLeft)
                continue;    // only the host's word counts for that (below)
            c.debug = false; // no debug powers in a network game
            sim.Schedule(first, std::move(c));
        }
    }
    // Players who left the match for good: their forces go on this tick, on every peer alike
    for (std::uint8_t slot : bundle.left)
    {
        SimCommand c;
        c.type = CommandType::PlayerLeft;
        c.player = slot;
        sim.Schedule(first, std::move(c));
    }
}

void NetMatch::AfterTick(std::uint32_t tick, std::uint64_t checksum)
{
    if (!m_client || !m_sim || !m_sim->AdvancesWorld())
        return;
    m_lastTick = tick;
    if (tick % kHashIntervalTicks == 0)
        m_recorder.OnChecksum(tick, checksum);
    const std::uint32_t tpt = std::max<std::uint32_t>(1, m_client->ticksPerTurn());
    // `tick` ran; the last tick of a turn ends it: what the player did meanwhile goes out
    if ((tick + 1) % tpt == 0 && !m_outgoing.empty())
    {
        const std::uint32_t turn = tick / tpt + m_client->inputDelay();
        CoalesceTurn(m_outgoing);
        // In batches the room accepts (a bigger one was refused by the host and the orders were lost); the host
        // keeps every batch
        for (const std::vector<std::uint8_t> &bytes : EncodeBatches(m_outgoing))
            m_client->sendCommands(turn, bytes);
        m_outgoing.clear();
    }
    if (tick % kHashIntervalTicks == 0)
    {
        m_client->sendWorldHash(tick, checksum);
        // OAOW_NET_TRACE=1: the checksums on stdout, to compare two peers by eye or script
        static const bool trace = DebugEnv("OAOW_NET_TRACE") != nullptr;
        if (trace && tick % (kHashIntervalTicks * 10) == 0)
        {
            std::printf("[Net] tick %u checksum %016llx\n", tick, static_cast<unsigned long long>(checksum));
            std::fflush(stdout);
        }
    }
}

NetMatch::Stats NetMatch::stats(std::uint32_t nowMs, std::uint32_t tick)
{
    Stats s;
    if (!m_client)
        return s;
    s.valid = true;
    s.host = m_host != nullptr;
    const Net::LinkStats link = m_client->linkStats();
    s.rttMs = link.rttMs;
    s.jitterMs = link.jitterMs;
    s.lossPercent = link.packetLoss * 100.0f;
    s.packetsSent = link.packetsSent;
    s.packetsLost = link.packetsLost;
    // Traffic per second, measured over (at least) one second
    if (m_rateMs == 0 || link.bytesSent < m_rateSent || link.bytesReceived < m_rateRecv)
    {
        m_rateMs = nowMs;
        m_rateSent = link.bytesSent;
        m_rateRecv = link.bytesReceived;
    }
    else if (nowMs - m_rateMs >= 1000)
    {
        const float secs = (nowMs - m_rateMs) / 1000.0f;
        m_sendKBps = (link.bytesSent - m_rateSent) / 1024.0f / secs;
        m_recvKBps = (link.bytesReceived - m_rateRecv) / 1024.0f / secs;
        m_rateMs = nowMs;
        m_rateSent = link.bytesSent;
        m_rateRecv = link.bytesReceived;
    }
    s.sendKBps = m_sendKBps;
    s.recvKBps = m_recvKBps;
    s.turnMs = m_client->turnMs();
    s.inputDelayMs = static_cast<int>(m_client->inputDelay()) * s.turnMs;
    s.turnsBuffered = m_sim ? std::max(0, TurnsAhead(tick)) : 0;
    s.stalledSeconds = m_sim ? StalledSeconds(nowMs) : 0.0f;
    s.reconnecting = m_reconnect.active;
    s.desynced = Desynced();
    if (m_host)
    {
        for (const Net::SessionHost::Player &p : m_host->players())
            s.peers.push_back({p.slot, p.name, p.connected, p.rttMs});
    }
    return s;
}

std::string NetMatch::FinishReplay(const std::string &outcome)
{
    return m_recorder.Finish(outcome, m_lastTick);
}

void NetMatch::SendView(float centerX, float centerY, float zoom)
{
    if (!m_client || !m_sim)
        return;
    const std::int32_t x = static_cast<std::int32_t>(centerX), y = static_cast<std::int32_t>(centerY);
    const std::int32_t z = static_cast<std::int32_t>(zoom * 100.0f + 0.5f);
    // Only a camera that moved (a tile or so) or zoomed is worth the bytes
    if (std::abs(x - m_lastViewX) < TILE_WIDTH && std::abs(y - m_lastViewY) < TILE_HEIGHT && z == m_lastViewZoom)
        return;
    m_lastViewX = x;
    m_lastViewY = y;
    m_lastViewZoom = z;
    SimCommand c;
    c.type = CommandType::View;
    c.player = m_client->slot();
    c.x = x;
    c.y = y;
    c.a = z;
    m_outgoing.push_back(std::move(c));
}

int NetMatch::TurnsAhead(std::uint32_t tick) const
{
    if (!m_client || IsReplica()) // a replica plays no turns: nothing to catch up with
        return 0;
    const std::uint32_t tpt = std::max<std::uint32_t>(1, m_client->ticksPerTurn());
    return static_cast<int>(m_client->turnsReceived()) - static_cast<int>(tick / tpt) - 1;
}

std::vector<std::uint8_t> NetMatch::EncodeSetup(const MatchSetup &setup)
{
    std::vector<std::uint8_t> buf(Net::kMaxMatchSetupBytes);
    Net::BitWriter w(buf.data(), buf.size());
    w.writeString(setup.mapPath, 256);
    w.writeU64(setup.mapHash);
    w.writeU32(setup.seed);
    w.writeVarInt(setup.startingCredits);
    w.writeBool(setup.fogOfWar);
    w.writeBool(setup.prebuilt);
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        const PlayerSlot &s = setup.slots[i];
        w.writeBool(s.active);
        if (!s.active)
            continue;
        w.writeBits(static_cast<std::uint32_t>(s.faction), 2);
        w.writeBits(s.team, 3);
        w.writeBits(s.color, 4);
        w.writeBool(s.human);
        if (!s.human)
            w.writeBits(s.aiLevel & 3u, 2);
        w.writeBits(static_cast<std::uint32_t>(setup.startPosition[i]) & 7u, 3);
        w.writeBits(s.handicap, 3);
        w.writeBits(s.armyCap, 3);
    }
    w.writeBool(setup.night); // a setup written before it has none (day)
    // Tech levels (data/capabilities.yaml), after a mark: a setup written before them has none (the default)
    w.writeBool(true);
    w.writeBits(static_cast<std::uint32_t>(setup.techLevel + 1) & 15u, 4);
    for (int i = 0; i < kMaxPlayers; ++i)
        if (setup.slots[i].active)
            w.writeBits(static_cast<std::uint32_t>(setup.slots[i].techLevel + 1) & 15u, 4);
    w.writeBool(setup.authorityMode); // after the tech levels: an older setup has none (lockstep)
    buf.resize(w.finish());
    return buf;
}

bool NetMatch::DecodeSetup(const std::vector<std::uint8_t> &bytes, MatchSetup &out)
{
    Net::BitReader r(bytes.data(), bytes.size());
    out = MatchSetup {};
    out.mapPath = r.readString(256);
    out.mapHash = r.readU64();
    out.seed = r.readU32();
    out.startingCredits = static_cast<int>(r.readVarInt());
    out.fogOfWar = r.readBool();
    out.prebuilt = r.readBool();
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        PlayerSlot &s = out.slots[i];
        s.active = r.readBool();
        if (!s.active)
            continue;
        const std::uint32_t faction = r.readBits(2);
        if (faction > static_cast<std::uint32_t>(Faction::Confederation))
            return false;
        s.faction = static_cast<Faction>(faction);
        s.team = static_cast<std::uint8_t>(r.readBits(3));
        s.color = static_cast<std::uint8_t>(r.readBits(4));
        if (s.color > 8)
            s.color = static_cast<std::uint8_t>(1 + i % 8); // the same fallback on every computer
        s.human = r.readBool();
        if (!s.human)
            s.aiLevel = static_cast<std::uint8_t>(r.readBits(2));
        out.startPosition[i] = static_cast<std::int8_t>(r.readBits(3));
        s.handicap = static_cast<std::uint8_t>(r.readBits(3));
        if (s.handicap >= kHandicapCount)
            return false;
        s.armyCap = static_cast<std::uint8_t>(r.readBits(3));
    }
    // The time of day came later: an older setup (a replay) ends here with zero padding, a day
    out.night = !r.atEnd() && r.readBool();
    // So did the tech levels: none (zero padding) in an older one
    if (!r.atEnd() && r.readBool())
    {
        out.techLevel = static_cast<std::int8_t>(static_cast<int>(r.readBits(4)) - 1);
        for (int i = 0; i < kMaxPlayers; ++i)
            if (out.slots[i].active)
                out.slots[i].techLevel = static_cast<std::int8_t>(static_cast<int>(r.readBits(4)) - 1);
    }
    out.authorityMode = !r.atEnd() && r.readBool();
    // Only maps of the game's own folder (the path comes from another computer)
    if (out.mapPath.rfind(LoaderResources::MapsDir() + "/", 0) != 0 || out.mapPath.find("..") != std::string::npos)
        return false;
    return !r.failed();
}

} // namespace OpenArtOfWar
