#include "core/Application.hpp"
#include "core/Localization.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/DebugEnv.hpp"
#include "ui/MenuController.hpp"
#include "ui/HUDController.hpp"
#include "ui/UiKit.hpp"
#include "game/GameplayController.hpp"
#include "game/Sides.hpp"
#include "game/Players.hpp"
#if defined(AOW_DEBUG)
#include "test/HeadlessTest.hpp"
#endif
#include "graphics/TextureManager.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/ShaderEffects.hpp"
#include "net/NetMatch.hpp"
#include "ui/NetworkMenu.hpp"
#include "openaow/net/Discovery.hpp"
#include "openaow/net/MasterClient.hpp"
#include "game/Match.hpp"
#include "game/Simulation.hpp"
#include "graphics/Camera.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "input/InputManager.hpp"
#include "audio/SoundEngine.hpp"
#include "audio/GameSounds.hpp"
#include "audio/SoundRequests.hpp"
#include "game/TileMap.hpp"
#include "game/Pathfinder.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/AIController.hpp"
#include "game/MissionController.hpp"
#include "core/EnvironmentConfig.hpp"
#include "game/TileCollision.hpp"
#include "game/Effects.hpp"
#include "game/Projectiles.hpp"
#include "game/WeaponBalance.hpp"
#include "game/Research.hpp"
#include "storage/SaveManager.hpp"
#include "debug/DebugOverlay.hpp"

#include <SDL3/SDL.h>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cmath>

// Application: network games - hosting, joining, the dedicated-server/room flow, the per-frame network update and the
// lobby menu glue

namespace OpenArtOfWar
{

// Developer aid (OAOW_NET_BOT=1): the local player sends all of its units at the nearest enemy
// headquarters every 5 seconds, so two headless peers have real command traffic and fights to agree on
void Application::DriveNetworkBot(std::uint32_t tick)
{
    static const bool bot = DebugEnv("OAOW_NET_BOT") != nullptr;
    if (!bot)
        return;
    const PlayerId me = Players::Local();
    const Faction myFaction = Players::FactionOf(me);
    // Economy traffic too (the commands that broke the other side's faction and unit ids): riflemen from
    // every barracks, and a building of the own faction next to the headquarters now and then
    if (tick % 100 == 40)
    {
        for (const Entity &e : m_entities->GetAllEntities())
        {
            const Building *b = e.AsBuilding();
            if (b && b->alive && b->owner == me && b->type == UnitType::Barracks && b->buildProgress >= 1.0f)
            {
                SimCommand produce;
                produce.type = CommandType::Produce;
                produce.player = me;
                produce.a = myFaction == Faction::Confederation ? 1 : 0; // the faction's rifleman
                produce.b = b->id;
                m_gameplayController->simulation().Issue(std::move(produce));
            }
        }
    }
    if (tick % 200 == 70)
    {
        const int slot = (tick / 200) % 2 == 0 ? 1 : 5; // power plant, bunker (canonical blueprint slots)
        const BuildingBlueprint *bp = EntityManager::FindBlueprint(myFaction, slot);
        const Entity *hq = nullptr;
        for (const Entity &e : m_entities->GetAllEntities())
        {
            if (e.alive && e.isBuilding && e.owner == me && e.type == UnitType::Headquarters)
                hq = &e;
        }
        if (bp && hq)
        {
            bool placed = false;
            for (int r = 4; r < 12 && !placed; ++r)
            {
                for (int dy = -r; dy <= r && !placed; ++dy)
                {
                    for (int dx = -r; dx <= r && !placed; ++dx)
                    {
                        const int x = hq->tileX + dx, y = hq->tileY + dy;
                        if (std::max(std::abs(dx), std::abs(dy)) != r
                            || !m_entities->CanPlaceBuilding(x, y, bp->width, bp->height, *m_map, me))
                            continue;
                        SimCommand place;
                        place.type = CommandType::PlaceBuilding;
                        place.player = me;
                        place.a = bp->blueprintId;
                        place.x = x;
                        place.y = y;
                        std::cout << "[NetBot] place blueprint " << bp->blueprintId << " (building "
                                  << bp->buildingIndex << ") at " << x << "," << y << std::endl;
                        m_gameplayController->simulation().Issue(std::move(place));
                        placed = true;
                    }
                }
            }
        }
    }
    if (tick % 100 != 10)
        return;
    SimCommand order;
    order.type = CommandType::OrderAt;
    order.player = me;
    order.a = static_cast<std::int32_t>(OrderKind::AttackMove);
    int targetX = -1, targetY = -1;
    for (const Entity &e : m_entities->GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die)
            continue;
        if (e.owner == me && !e.isBuilding)
            order.ids.push_back(static_cast<std::uint32_t>(e.id));
        if (targetX < 0 && e.isBuilding && e.type == UnitType::Headquarters && Players::Hostile(e.owner, me))
        {
            targetX = e.tileX * TILE_WIDTH + TILE_WIDTH / 2;
            targetY = e.tileY * TILE_HEIGHT + TILE_HEIGHT / 2;
        }
    }
    if (order.ids.empty() || targetX < 0)
        return;
    order.x = targetX;
    order.y = targetY;
    m_gameplayController->simulation().Issue(std::move(order));
}

namespace
{
// What a refusal says, in the player's language; `detail` is the host's note (its version for another game version)
std::string RejectText(Net::RejectReason reason, const std::string &detail, bool afterSync)
{
    switch (reason)
    {
        case Net::RejectReason::ProtocolMismatch:
            return _("The other side uses another network protocol: both need the same version of the game.");
        case Net::RejectReason::VersionMismatch:
            return std::string(_("Different game version"))
                   + (detail.empty() ? std::string()
                                     : std::string(": ") + _("the host plays") + " " + detail + ", " + _("you have")
                                           + " " + OAOW_VERSION);
        case Net::RejectReason::AssetsMismatch:
            return afterSync ? std::string(_("Different game data: they still differ after the sync."))
                             : std::string(
                                   _("Different game data: the host cannot send its files (ask it to update, or "
                                     "join a game of the same version)."));
        case Net::RejectReason::RoomFull:
            return _("The room is full");
        case Net::RejectReason::MatchRunning:
            return _("The match has already started");
        case Net::RejectReason::Banned:
            return _("Not allowed to join");
        case Net::RejectReason::Kicked:
            return _("The room owner freed your place");
        case Net::RejectReason::BadPassword:
            return _("Wrong password");
        case Net::RejectReason::NotFound:
            return _("No such room");
        case Net::RejectReason::ServerFull:
            return _("The server has no free room");
        case Net::RejectReason::TryAgain:
            return _("Not ready yet");
        default:
            return Net::rejectReasonText(reason);
    }
}
} // namespace

// The game plays with the data files it has now (the host's, while a network session has them): everything the
// simulation read at start is read again
void Application::ReloadGameData()
{
    if (!m_entities || !m_map)
        return;
    m_entities->LoadCharacterConfigs();
    ReloadResearchCatalogs();
    Projectiles::reload();
    WeaponBalance::Reload();
    EffectsConfig::reload();
    TileCollision::Reload();
    if (Assets::HasSessionOverlay())
    {
        EnvironmentConfig host;
        host.load(); // the host's construction rules; the player's own settings (m_env) stay as they are
        m_map->LoadEnvironment(host);
    }
    else
    {
        m_map->LoadEnvironment(m_env);
    }
}

void Application::LeaveNetworkGame()
{
    if (m_net)
        m_net->leave();
    m_net.reset();
    m_netMatchStarted = false;
    m_netMatchLoaded = false;
    m_netPlayers = {};
    m_notices.clear();
    m_netMapPath.clear();
    m_netAuto = false;
    m_networkMenu->Reset();
}

// The dedicated server of the internet tab: its room list, and joining a room it created or found
void Application::UpdateServer()
{
    if (m_net || (!m_networkMenu->InternetTab() && m_cliServerAction == 0 && !m_serverJoinPending))
        return;
    const nlohmann::json &net = m_env.Section("network");
    const std::string host
        = !m_serverOverride.empty() ? m_serverOverride : net.value("master_host", std::string("openaow.imister.tech"));
    const std::uint16_t port
        = m_serverOverridePort ? m_serverOverridePort : static_cast<std::uint16_t>(net.value("master_port", 38100));
    const std::uint32_t now = SDL_GetTicks();
    if (!m_master)
    {
        m_master = std::make_unique<Net::MasterClient>(host, port);
        m_serverListMs = 0;
    }
    m_master->update(now);
    if (m_master->state() == Net::MasterClient::State::Failed)
    {
        m_serverStatus = host + ": " + m_master->error();
        m_serverRooms.clear();
        if (m_serverJoinPending)
            m_networkMenu->SetNotice(std::string(_("Server: ")) + m_master->error());
        m_serverJoinPending = false;
        // Try the server again every few seconds while the internet tab is open
        if (m_serverFailedMs == 0)
            m_serverFailedMs = now;
        if (now - m_serverFailedMs >= kServerRetryMs)
        {
            m_serverFailedMs = 0;
            m_master.reset();
        }
        return;
    }
    m_serverFailedMs = 0;
    if (m_master->state() == Net::MasterClient::State::Connecting)
    {
        m_serverStatus = host + ": " + _("connecting...");
        return;
    }
    // The command line's request, once the server answers
    if (m_cliServerAction == 1)
        m_master->createRoom(NetMatch::gameIdentity(), m_settings.playerName, true);
    if (m_cliServerAction == 2)
        m_master->findRoom(m_cliServerCode);
    if (m_cliServerAction != 0)
    {
        m_cliServerAction = 0;
        m_serverJoinPending = true;
    }
    m_serverStatus = m_master->serverInfo().name.empty() ? host : m_master->serverInfo().name;
    if (!m_master->serverInfo().motd.empty())
        m_serverStatus += " - " + m_master->serverInfo().motd;
    // The list every 3 seconds, only while the internet tab of the network screen is open (no need to press
    // Refresh to see rooms come and go)
    if (m_serverListMs == 0 || now - m_serverListMs >= kServerRetryMs)
    {
        m_serverListMs = now;
        m_master->requestList(OAOW_VERSION);
    }
    if (m_master->hasList())
    {
        m_serverRooms.clear();
        for (const Net::RoomListing &r : m_master->rooms())
        {
            if (r.running)
                continue;
            const std::string map = r.map.substr(r.map.find_last_of('/') + 1);
            m_serverRooms.push_back(
                {host, r.port, r.name, map.substr(0, map.rfind('.')), r.players, r.maxPlayers, 0, r.code, r.locked});
        }
    }
    Net::RejectReason refusal {};
    if (m_master->takeRefusal(refusal))
    {
        m_networkMenu->SetNotice(std::string(_("Server: ")) + RejectText(refusal, "", false));
        m_serverJoinPending = false;
    }
    Net::RoomAddress room;
    if (m_serverJoinPending && m_master->takeRoom(room))
    {
        m_serverJoinPending = false;
        m_net = std::make_unique<NetMatch>();
        std::string error;
        m_net->SetPassword(m_networkMenu->password());
        // Port 0: a game hosted by a player, reached through the server's introduction
        const bool ok = room.port == 0 ? m_net->joinHosted(host, port, room.code, m_settings.playerName, error)
                                       : m_net->join(host, room.port, m_settings.playerName, error, true);
        if (!ok)
        {
            m_networkMenu->SetNotice(std::string(_("Cannot connect: ")) + error);
            m_net.reset();
        }
        else
        {
            std::cout << "[Net] Room " << room.code << " on " << host << ":" << room.port << std::endl;
        }
    }
}

void Application::UpdateNetworkMenu()
{
    // SDL delivers typed text only while text input is on
    const bool wantText = m_networkMenu->WantsText();
    if (wantText != m_textInput)
    {
        if (wantText)
            SDL_StartTextInput(m_window);
        else
            SDL_StopTextInput(m_window);
        m_textInput = wantText;
    }
    const std::uint16_t port = static_cast<std::uint16_t>(m_env.Section("network").value("udp_port", 38100));
    // Games on the local network while browsing (games of another version are left out)
    if (!m_net)
    {
        if (!m_lanBrowser)
            m_lanBrowser = std::make_unique<Net::LanBrowser>(port);
        m_lanBrowser->update(SDL_GetTicks());
        m_lanGames.clear();
        for (const Net::FoundRoom &r : m_lanBrowser->rooms())
        {
            if (r.info.gameVersion != OAOW_VERSION || r.info.running)
                continue;
            const std::string map = r.info.map.substr(r.info.map.find_last_of('/') + 1);
            m_lanGames.push_back(
                {r.address,
                 r.port,
                 r.info.name,
                 map.substr(0, map.rfind('.')),
                 r.info.players,
                 r.info.maxPlayers,
                 r.pingMs,
                 std::string(),
                 r.info.locked});
        }
    }
    else
    {
        m_lanBrowser.reset();
    }
    UpdateServer();
    switch (m_networkMenu->Update(*m_input, m_windowW, m_windowH, m_net.get(), m_settings, m_lanGames, m_serverRooms))
    {
        case NetworkAction::Back:
            SaveSettings();
            m_lanBrowser.reset();
            m_master.reset();
            m_state = AppState::MainMenu;
            break;
        case NetworkAction::Host:
        {
            SaveSettings();
            m_net = std::make_unique<NetMatch>();
            std::string error;
            m_net->SetPassword(m_networkMenu->password());
            if (!m_net->host(port, m_settings.playerName, error))
            {
                m_networkMenu->SetNotice(std::string(_("Cannot create the game: ")) + error);
                m_net.reset();
            }
            else if (m_networkMenu->InternetTab())
            {
                // From the internet tab: also listed on the server, reachable by its code without
                // forwarded ports (NAT hole punching)
                const nlohmann::json &netEnv = m_env.Section("network");
                m_net->Announce(
                    m_serverOverride.empty() ? netEnv.value("master_host", std::string("openaow.imister.tech"))
                                             : m_serverOverride,
                    m_serverOverridePort ? m_serverOverridePort
                                         : static_cast<std::uint16_t>(netEnv.value("master_port", 38100)));
            }
            break;
        }
        case NetworkAction::Join:
        {
            SaveSettings();
            m_net = std::make_unique<NetMatch>();
            std::string error;
            m_net->SetPassword(m_networkMenu->password());
            if (!m_net->join(m_networkMenu->JoinAddress(), m_networkMenu->JoinPort(), m_settings.playerName, error))
            {
                m_networkMenu->SetNotice(std::string(_("Cannot connect: ")) + error);
                m_net.reset();
            }
            break;
        }
        case NetworkAction::Leave:
            LeaveNetworkGame();
            break;
        case NetworkAction::ServerRefresh:
            m_serverListMs = 0; // updateServer asks again
            m_master.reset();   // and reconnects if the last attempt failed
            break;
        case NetworkAction::ServerCreate:
            SaveSettings();
            UpdateServer();
            if (m_master)
            {
                m_master->createRoom(NetMatch::gameIdentity(), m_settings.playerName, true, m_networkMenu->password());
                m_serverJoinPending = true;
            }
            break;
        case NetworkAction::ServerJoin:
            SaveSettings();
            UpdateServer();
            if (m_master)
            {
                m_master->findRoom(m_networkMenu->JoinCode());
                m_serverJoinPending = true;
            }
            break;
        case NetworkAction::Start:
            if (m_net)
            {
                m_networkMenu->PushOptions(*m_net);
                m_net->StartFromLobby(SDL_GetTicks());
                m_netMatchStarted = true;
            }
            break;
        case NetworkAction::None:
            break;
    }
    // The connection ended while in the menu
    if (m_net && !m_net->connected() && m_net->StatusText() == "disconnected")
    {
        m_networkMenu->SetNotice(_("The connection was lost"));
        LeaveNetworkGame();
    }
}

// What a network match wants the player to know: catching up, waiting, reconnecting, out of sync
namespace
{
// The name of a map for its loading screen
std::string MapTitleOf(const std::string &path)
{
    for (const SkirmishMapInfo &m : MissionController::ListSkirmishMaps())
    {
        if (m.path == path)
            return m.DisplayName();
    }
    return std::filesystem::path(path).stem().string();
}
} // namespace

bool Application::InNetworkMatch() const
{
    return m_net && m_netMatchStarted && m_mission && m_mission->IsMatch();
}

void Application::HostNetworkGame(
    std::uint16_t port, int players, const std::string &mapPath, const std::string &name, bool fromScratch)
{
    m_net = std::make_unique<NetMatch>();
    std::string error;
    if (!m_net->host(port, name, error))
    {
        std::cerr << "[Net] Cannot host on port " << port << ": " << error << std::endl;
        m_net.reset();
        return;
    }
    if (m_networkMenu)
        m_networkMenu->SetPrebuilt(!fromScratch);
    m_netWantedPlayers = std::clamp(players, 1, kMaxPlayers);
    m_netMapPath = mapPath;
    m_netAuto = true;
    m_state = AppState::MenuNetwork; // the room shows who has joined
    std::cout << "[Net] Hosting on UDP " << port << ", waiting for " << m_netWantedPlayers << " players" << std::endl;
}

void Application::AnnounceNetworkGame(const std::string &server, std::uint16_t port)
{
    if (m_net)
        m_net->Announce(server, port);
}

void Application::JoinNetworkGame(const std::string &address, std::uint16_t port, const std::string &name)
{
    m_net = std::make_unique<NetMatch>();
    std::string error;
    if (!m_net->join(address, port, name, error))
    {
        std::cerr << "[Net] Cannot join " << address << ":" << port << ": " << error << std::endl;
        m_net.reset();
        return;
    }
    std::cout << "[Net] Joining " << address << ":" << port << std::endl;
    m_netAuto = true;
    m_state = AppState::MenuNetwork;
}

void Application::ServerNetworkGame(
    const std::string &server,
    std::uint16_t port,
    const std::string &code,
    int players,
    const std::string &mapPath,
    const std::string &name,
    bool fromScratch)
{
    if (m_networkMenu && code.empty())
        m_networkMenu->SetPrebuilt(!fromScratch);
    m_serverOverride = server;
    m_serverOverridePort = port;
    m_cliServerAction = code.empty() ? 1 : 2;
    m_cliServerCode = code;
    m_netWantedPlayers = std::clamp(players, 1, kMaxPlayers);
    m_netMapPath = mapPath;
    m_netAuto = true;
    m_settings.playerName = name;
    m_state = AppState::MenuNetwork;
    std::cout << "[Net] Server " << server << ":" << port << (code.empty() ? ", creating a room" : ", room " + code)
              << std::endl;
}

void Application::UpdateNetwork(std::uint32_t nowMs)
{
    if (!m_net)
    {
        // Out of the network game: the player's own data are back
        if (Assets::HasSessionOverlay())
        {
            Assets::ClearSessionOverlay();
            ReloadGameData();
        }
        return;
    }
    m_net->Update(nowMs);
    if (m_net->TakeDataChanged())
        ReloadGameData();
    static std::string announcedCode;
    if (m_net->PublicCode() != announcedCode)
    {
        announcedCode = m_net->PublicCode();
        if (!announcedCode.empty())
            std::cout << "[Net] Listed on the server as " << announcedCode << std::endl;
    }
    // A game from the command line: everybody says ready by itself, the host / room owner sets the map and
    // starts as soon as the wanted number of players is there (factions at random, teams by slot)
    if (m_netAuto && m_net->connected() && !m_netMatchStarted && nowMs - m_netAutoMs > 300)
    {
        m_netAutoMs = nowMs;
        const Net::LobbyState *lobby = m_net->Lobby();
        if (m_net->ControlsRoom() && !m_netMapPath.empty() && lobby && lobby->map != m_netMapPath)
            m_net->SetLobbyMap(m_netMapPath);
        if (lobby)
        {
            for (const Net::LobbySlot &s : lobby->slots)
            {
                Net::LobbyChoice mine = s.choice;
                // OAOW_NET_FACTION_BY_SLOT=1 (self-test): even slots Resistance, odd ones Confederation, so
                // a match always has both factions
                static const bool factionBySlot = DebugEnv("OAOW_NET_FACTION_BY_SLOT") != nullptr;
                if (s.slot == m_net->LocalSlot() && m_net->LocalChoice(mine) && !mine.ready && m_net->MapReady())
                {
                    if (factionBySlot)
                        mine.faction = static_cast<std::uint8_t>(
                            m_net->LocalSlot() % 2 == 0 ? Faction::Resistance : Faction::Confederation);
                    // OAOW_NET_FACTION=0|1: this faction for everybody (players of one faction)
                    if (const char *forced = DebugEnv("OAOW_NET_FACTION");
                        forced && (forced[0] == '0' || forced[0] == '1'))
                        mine.faction = static_cast<std::uint8_t>(forced[0] - '0');
                    mine.ready = true;
                    m_net->SetChoice(mine);
                }
            }
        }
        if (m_net->ControlsRoom() && m_net->CanStart() && m_net->PlayerCount() >= m_netWantedPlayers)
        {
            m_networkMenu->PushOptions(*m_net);
            m_net->StartFromLobby(nowMs);
            std::cout << "[Net] Starting the match on " << m_netMapPath << " for " << m_net->PlayerCount() << " players"
                      << std::endl;
            m_netMatchStarted = true;
        }
    }
    MatchSetup setup;
    if (m_net->TakeStartedMatch(setup))
    {
        BeginMapLoading(MapTitleOf(setup.mapPath), setup.mapPath);
        if (!m_mission->LoadMatch(setup, *m_map, *m_entities, *m_economy, *m_pathfinder))
        {
            EndMapLoading();
            std::cerr << "[Net] The match map could not be set up" << std::endl;
            m_net.reset();
            return;
        }
        m_netMatchStarted = true;
        m_netMatchLoaded = true;
        m_netPlayers = {};
        m_notices.clear();
        // A fresh simulation now, before the first turn's commands are scheduled into it
        m_gameplayController->ResetForMission(*m_mission, *m_hudController, *m_entities, *m_map);
        m_net->Attach(m_gameplayController->simulation());
        m_net->SetReconnectWindow(
            static_cast<std::uint32_t>(m_env.Section("network").value("reconnect_window_s", 60)) * 1000u);
        m_state = AppState::SkirmishPlaying;
        m_camera->SetZoom(m_settings.zoomDefault);
        m_camera->CenterOn(m_mission->GetPlayerHqX() * TILE_WIDTH, m_mission->GetPlayerHqY() * TILE_HEIGHT);
        EndMapLoading(); // not before three seconds of the loading screen are over
        std::cout << "[Net] Match started, local slot " << int(setup.local) << std::endl;
        // Who plays what, as this computer got it (the self-test compares the peers' lines)
        for (int i = 0; i < kMaxPlayers; ++i)
        {
            if (setup.slots[i].active)
                std::cout << "[Net] Slot " << i << ": faction " << static_cast<int>(setup.slots[i].faction) << " team "
                          << int(setup.slots[i].team) << " color " << int(setup.slots[i].color) << std::endl;
        }
    }
    if (!m_net->Failure().empty())
    {
        std::cerr << "[Net] " << m_net->Failure() << std::endl;
        m_networkMenu->SetNotice(m_net->Failure());
        m_state = AppState::MenuNetwork;
        LeaveNetworkGame();
        return;
    }
    if (m_net->JoinTimedOut(nowMs))
    {
        std::cerr << "[Net] No answer from the game" << std::endl;
        m_networkMenu->SetNotice(_("No answer: check the address, and that the host forwards UDP port 38100"));
        LeaveNetworkGame();
        return;
    }
    if (m_net->rejected())
    {
        std::cerr << "[Net] Rejected: " << m_net->StatusText() << std::endl;
        m_networkMenu->SetNotice(
            std::string(_("Not admitted: "))
            + RejectText(m_net->RejectedWhy(), m_net->RejectedDetail(), m_net->Synced()));
        LeaveNetworkGame();
        return;
    }
    // Back in the menus after a match: the session is over
    if (m_netMatchLoaded && (m_state == AppState::MainMenu || m_state == AppState::MenuNetwork))
    {
        LeaveNetworkGame();
        return;
    }
    if (m_netMatchLoaded)
        TrackNetPlayers(nowMs);
    if (m_net->Desynced())
    {
        static bool reported = false;
        if (!reported)
            std::cerr << "[Net] DESYNC: the worlds of the players differ" << std::endl;
        reported = true;
    }
}

} // namespace OpenArtOfWar
