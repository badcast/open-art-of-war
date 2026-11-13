#include "core/Application.hpp"
#include "core/LoaderResources.hpp"
#include "game/Sides.hpp"
#include "game/Missions.hpp"
#include "game/SimCommandCodec.hpp"
#include "net/MapFiles.hpp"
#include "net/ReplayRecorder.hpp"
#include "audio/SoundEngine.hpp"
#include "audio/SoundRequests.hpp"
#include "core/AssetStore.hpp"
#include "core/DebugEnv.hpp"
#include "core/Localization.hpp"
#include "game/AIController.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/GameplayController.hpp"
#include "game/MissionController.hpp"
#include "game/Pathfinder.hpp"
#include "game/Players.hpp"
#include "game/Research.hpp"
#include "game/Simulation.hpp"
#include "game/TileMap.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/ShaderEffects.hpp"
#include "input/InputManager.hpp"
#include "net/NetMatch.hpp"
#include "replay/ReplayPlayer.hpp"
#include "ui/HUDController.hpp"
#include "ui/MenuController.hpp"
#include "ui/ReplayMenu.hpp"
#include "ui/UiKit.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>

// Application: watching a replay (.awr). The recorded turns are scheduled into the simulation exactly as the network
// scheduled them; commands the viewer issues are swallowed, so the world follows the recording alone.

namespace OpenArtOfWar
{

namespace
{

namespace col = ui::color;

constexpr float kSpeeds[] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f};
constexpr int kSpeedCount = 7;
constexpr int kBarHeight = 64; // a thin row for the sound check box on top, the controls under it
constexpr int kTopRow = 26;

struct BarLayout
{
    ui::UiRect bar, voice, play, slower, speed, faster, restart, progress, view, follow, stats, research, exit;
};

// `replay`: a recording (with the progress bar and the restart button); otherwise a live match being watched
BarLayout MakeBar(int viewW, bool replay)
{
    BarLayout l;
    const int h = kBarHeight - kTopRow - 8;
    const int y = kTopRow + 2;
    l.bar = {0, 0, viewW, kBarHeight};
    l.voice = {8, 3, 230, kTopRow - 5};
    int x = 8;
    auto place = [&](ui::UiRect &r, int w) {
        r = {x, y, w, h};
        x += w + 6;
    };
    place(l.play, 76);
    place(l.slower, 30);
    place(l.speed, 52);
    place(l.faster, 30);
    if (replay)
        place(l.restart, 30);
    int right = viewW - 8;
    auto placeRight = [&](ui::UiRect &r, int w) {
        right -= w;
        r = {right, y, w, h};
        right -= 6;
    };
    placeRight(l.exit, 70);
    placeRight(l.follow, 120);
    placeRight(l.view, 190);
    // The thin top row: the sound check box at the left, the statistics and the research at the right
    l.stats = {viewW - 8 - 120, 3, 120, kTopRow - 5};
    l.research = {l.stats.x - 6 - 140, 3, 140, kTopRow - 5};
    l.progress = {x + 6, y + 6, std::max(60, right - x - 6), h - 12};
    return l;
}

std::string Clock(std::uint32_t ticks)
{
    const std::uint32_t s = ticks / 20;
    char buf[24];
    std::snprintf(buf, sizeof buf, "%u:%02u", s / 60, s % 60);
    return buf;
}

std::string TitleOf(std::string_view path)
{
    return std::filesystem::path(path).stem().string();
}

} // namespace

bool Application::OpenReplay(std::string_view path)
{
    if (!m_entities || !m_map || m_headless)
        return false;
    if (m_net)
        LeaveNetworkGame();
    if (!m_replayMenu)
        m_replayMenu = std::make_unique<ReplayMenu>();
    auto player = std::make_unique<ReplayPlayer>();
    std::string error;
    if (!player->Load(path, error))
    {
        std::cerr << "[Replay] " << path << ": " << error << std::endl;
        m_replayMenu->Refresh();
        m_replayMenu->SetNotice(error);
        m_state = AppState::MenuReplay;
        return false;
    }
    m_replay = std::move(player);
    m_replayUi = ReplayUi {};
    if (!StartReplay(m_replay->info().localSlot))
        return false;
    std::cout << "[Replay] Playing " << path << " (" << m_replay->info().turns << " turns)" << std::endl;
    return true;
}

// Sets the recorded match up from its first tick, watching `watchSlot`
bool Application::StartReplay(int watchSlot)
{
    std::string error;
    MatchSetup setup;
    if (!m_replay || !m_replay->PrepareMatch(watchSlot, setup, error))
    {
        if (!m_replayMenu)
            m_replayMenu = std::make_unique<ReplayMenu>();
        m_replayMenu->Refresh();
        m_replayMenu->SetNotice(error);
        CloseReplay();
        m_state = AppState::MenuReplay;
        return false;
    }
    const ReplayPlayer::Info &rinfo = m_replay->info();
    const bool campaign = rinfo.mode == "campaign";
    if (campaign)
    {
        // A campaign mission: its side and the computer's difficulty, then the mission sets the world up
        SetPlayerFaction(
            rinfo.playerFaction == static_cast<int>(Faction::Confederation) ? Faction::Confederation
                                                                            : Faction::Resistance);
        m_aiController->SetDifficulty(static_cast<AIDifficulty>(std::clamp(rinfo.difficulty, 0, 3)));
    }
    ReloadGameData(); // the match plays with the data it was recorded with
    BeginMapLoading(TitleOf(setup.mapPath), setup.mapPath);
    const bool loaded = campaign ? m_mission->LoadMission(rinfo.mission, *m_map, *m_entities, *m_economy, *m_pathfinder)
                                 : m_mission->LoadMatch(setup, *m_map, *m_entities, *m_economy, *m_pathfinder);
    if (!loaded)
    {
        EndMapLoading();
        if (!m_replayMenu)
            m_replayMenu = std::make_unique<ReplayMenu>();
        m_replayMenu->Refresh();
        m_replayMenu->SetNotice(_("The map of this replay could not be set up"));
        CloseReplay();
        m_state = AppState::MenuReplay;
        return false;
    }
    m_replaySetup = setup;
    m_notices.clear();
    m_gameplayController->ResetForMission(*m_mission, *m_hudController, *m_entities, *m_map);
    // Nothing the viewer does reaches the world
    m_gameplayController->simulation().SetCommandSink([](SimCommand) {});
    m_gameplayController->SetSimulationHold(false);
    m_replay->Rewind();
    m_replayUi.accumulator = 0.0;
    m_replayUi.finished = false;
    m_replayUi.paused = false;
    m_replayUi.cameraSet = false;
    m_replayUi.watch = setup.local;
    if (m_sound)
        m_sound->SetInterfaceMuted(!m_replayUi.interfaceSound); // the interface is silent unless the check box is on
    // Tests (OAOW_REPLAY_SPEED=<0..4>, debug builds): the playback speed from the start
    if (const char *speed = DebugEnv("OAOW_REPLAY_SPEED"))
        m_replayUi.speed = std::clamp(std::atoi(speed), 0, kSpeedCount - 1);
    m_hudController->SetShowAll(false);
    m_hudController->SetObserver(true);
    m_state = AppState::SkirmishPlaying;
    m_camera->SetZoom(m_settings.zoomDefault);
    m_camera->CenterOn(m_mission->GetPlayerHqX() * TILE_WIDTH, m_mission->GetPlayerHqY() * TILE_HEIGHT);
    EndMapLoading();
    return true;
}

void Application::CloseReplay()
{
    const bool had = m_replay != nullptr;
    if (m_gameplayController)
    {
        m_gameplayController->SetSimulationHold(false);
        m_gameplayController->simulation().SetCommandSink(nullptr);
    }
    if (m_hudController)
    {
        m_hudController->SetShowAll(false);
        m_hudController->SetObserver(false);
    }
    if (m_sound)
        m_sound->SetInterfaceMuted(false);
    m_replay.reset(); // takes the recorded data away
    m_replayUi = ReplayUi {};
    if (had && m_aiController)
        m_aiController->SetDifficulty(
            static_cast<AIDifficulty>(m_settings.difficulty)); // (a campaign replay set its own)
    if (had)
        ReloadGameData(); // the player's own data again
}

// Whose view is shown: a player's (his fog, economy and colours) or -1: everything, as an observer
void Application::ReplayWatch(int slot)
{
    // A campaign mission is seen with its player's eyes or as an observer: the computer side has no view of its own
    if (m_replay && m_replay->info().mode == "campaign" && slot >= 0)
        slot = m_replaySetup.local;
    m_replayUi.watch = slot;
    m_hudController->SetShowAll(slot < 0);
    if (slot < 0)
        return;
    if (m_replay && m_replay->info().mode == "campaign")
        return; // (the players of the mission stay as it set them up)
    Players::setup(m_replaySetup.slots, static_cast<PlayerId>(slot));
    m_replaySetup.local = static_cast<PlayerId>(slot);
    m_map->SetViewerTeam(Players::TeamOf(static_cast<PlayerId>(slot)));
    // What this player has seen is not known: it is seen again from now on. Only the drawn fog: wiping what the teams
    // see (ResetAllFog) made the next tick's commands find no visible enemy (an attack became a move: out of sync)
    m_map->ResetViewerFog();
    // (the local economy stays the simulation's: handing it to the watched player updated that economy twice and
    // the local player's never - the world went out of sync)
    m_entities->RebuildBlueprints();
    m_gameplayController->ClearSelection();
}

std::vector<Application::ObserverPlayer> Application::ObserverPlayers() const
{
    std::vector<ObserverPlayer> out;
    if (m_replay)
    {
        for (const auto &p : m_replay->info().players)
            out.push_back({p.slot, p.name, p.color, p.human});
        return out;
    }
    static const char *const kLevels[4] = {"Easy", "Medium", "Hard", "Hardcore"};
    for (int slot = 0; slot < kMaxPlayers; ++slot)
    {
        const PlayerSlot &ps = m_replaySetup.slots[slot];
        if (!ps.active)
            continue;
        const std::string name = ps.human ? std::string(_("Player")) + " " + std::to_string(slot + 1)
                                          : std::string(_("AI")) + " " + std::to_string(slot + 1) + " ("
                                                + _(kLevels[ps.aiLevel & 3]) + ")";
        out.push_back({slot, name, ps.color, ps.human});
    }
    return out;
}

// The middle of a player's army (its base when it has no troops): where "follow" looks for a computer player
bool Application::ArmyCentreOf(int slot, float &x, float &y) const
{
    double sx = 0.0, sy = 0.0;
    int n = 0;
    float hqX = -1.0f, hqY = -1.0f;
    for (const Entity &e : m_entities->GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die || e.owner != slot)
            continue;
        if (e.isBuilding)
        {
            if (hqX < 0.0f && e.type == UnitType::Headquarters)
            {
                hqX = e.GetUnitCenterPixelX();
                hqY = e.GetUnitCenterPixelY();
            }
            continue;
        }
        sx += e.GetUnitCenterPixelX();
        sy += e.GetUnitCenterPixelY();
        ++n;
    }
    if (n > 0)
    {
        x = static_cast<float>(sx / n);
        y = static_cast<float>(sy / n);
        return true;
    }
    if (hqX >= 0.0f)
    {
        x = hqX;
        y = hqY;
        return true;
    }
    return false;
}

void Application::LeaveObserver()
{
    if (m_replay)
    {
        CloseReplay();
        m_state = AppState::MenuReplay;
        if (m_replayMenu)
            m_replayMenu->Refresh();
        return;
    }
    m_state = AppState::MainMenu; // the watched match ends (the spectator mode switches itself off)
}

void Application::UpdateReplayInput(double frameTime)
{
    Simulation &sim = m_gameplayController->simulation();
    const bool replay = m_replay != nullptr;
    const std::uint32_t total = replay ? m_replay->info().ticks : 0u;
    m_replayUi.finished = replay && sim.Tick() >= total;

    // Keys
    if (m_input->IsKeyJustPressed(SDL_SCANCODE_ESCAPE) && (replay || m_replayUi.stats))
    {
        if (m_replayUi.stats)
            m_replayUi.stats = false; // the statistics close first
        else
            LeaveObserver();
        return;
    }
    bool togglePause
        = m_input->IsKeyJustPressed(SDL_SCANCODE_SPACE)
          || (!replay && (m_input->IsKeyJustPressed(SDL_SCANCODE_P) || m_input->IsKeyJustPressed(SDL_SCANCODE_PAUSE)));
    bool restart = replay && m_input->IsKeyJustPressed(SDL_SCANCODE_HOME);
    const int previousSpeed = m_replayUi.speed;
    const bool allowSpeed = IsSpeedControlAllowed();
    if (!allowSpeed)
        m_replayUi.speed = 1;
    int speedStep = 0;
    if (allowSpeed
        && (m_input->IsKeyJustPressed(SDL_SCANCODE_EQUALS) || m_input->IsKeyJustPressed(SDL_SCANCODE_KP_PLUS)))
        speedStep = 1;
    if (allowSpeed
        && (m_input->IsKeyJustPressed(SDL_SCANCODE_MINUS) || m_input->IsKeyJustPressed(SDL_SCANCODE_KP_MINUS)))
        speedStep = -1;
    // (Tab shows the scoreboard in a live match: there the view changes with V)
    bool nextView = m_input->IsKeyJustPressed(replay ? SDL_SCANCODE_TAB : SDL_SCANCODE_V);
    bool toggleFollow = m_input->IsKeyJustPressed(SDL_SCANCODE_F);
    bool toggleStats = m_input->IsKeyJustPressed(SDL_SCANCODE_S) && !m_input->isKeyPressed(SDL_SCANCODE_LCTRL);
    bool openResearch = false;

    // The bar (a click on it is taken before the game sees it)
    const BarLayout bar = MakeBar(m_camera->GetViewportWidth(), replay);
    const int mx = m_input->GetMouseScreenX(), my = m_input->GetMouseScreenY();
    // The open list of the view combo box takes the input first
    const std::vector<ObserverPlayer> viewPlayers = ObserverPlayers();
    int pressX = 0, pressY = 0;
    const bool down = m_input->IsLeftMouseDown();
    const bool pressed = m_input->ConsumeLeftPress(pressX, pressY) && down;
    if (m_replayUi.combo.open >= 0)
    {
        int picked = -1;
        float lx, ly;
        const bool clicked = m_input->WasLeftClickIssued(lx, ly);
        ui::ComboUpdate(
            m_replayUi.combo,
            bar.view,
            static_cast<int>(viewPlayers.size()) + 1,
            m_windowH,
            mx,
            my,
            clicked,
            m_input->ConsumeWheelF(),
            picked,
            down,
            pressed,
            pressX,
            pressY);
        if (picked >= 0)
            ReplayWatch(picked == 0 ? -1 : viewPlayers[picked - 1].slot);
    }
    else if (bar.bar.Contains(mx, my))
    {
        float lx, ly;
        if (m_input->WasLeftClickIssued(lx, ly))
        {
            if (bar.voice.Contains(mx, my))
            {
                m_replayUi.interfaceSound = !m_replayUi.interfaceSound;
                if (m_sound)
                    m_sound->SetInterfaceMuted(!m_replayUi.interfaceSound);
            }
            else if (bar.play.Contains(mx, my))
                togglePause = true;
            else if (allowSpeed && bar.slower.Contains(mx, my))
                speedStep = -1;
            else if (allowSpeed && bar.speed.Contains(mx, my))
                m_replayUi.speed = 1;
            else if (allowSpeed && bar.faster.Contains(mx, my))
                speedStep = 1;
            else if (replay && bar.restart.Contains(mx, my))
                restart = true;
            else if (bar.view.Contains(mx, my))
            {
                m_replayUi.combo.open = 0;
                m_replayUi.combo.scroll = 0;
            }
            else if (bar.follow.Contains(mx, my))
                toggleFollow = true;
            else if (bar.stats.Contains(mx, my))
                toggleStats = true;
            else if (bar.research.Contains(mx, my))
                openResearch = true;
            else if (bar.exit.Contains(mx, my))
            {
                LeaveObserver();
                return;
            }
        }
    }

    if (replay && (restart || (togglePause && m_replayUi.finished)))
    {
        StartReplay(m_replayUi.watch >= 0 ? m_replayUi.watch : m_replaySetup.local);
        return;
    }
    if (togglePause)
        m_replayUi.paused = !m_replayUi.paused;
    if (speedStep != 0)
        m_replayUi.speed = std::clamp(m_replayUi.speed + speedStep, 0, kSpeedCount - 1);
    // Discard whole ticks owed at the old speed, retaining the interpolation phase.
    // Otherwise a slow speed can keep running fast while a large backlog drains.
    if (m_replayUi.speed != previousSpeed)
        m_replayUi.accumulator = std::fmod(m_replayUi.accumulator, SIMULATION_TIMESTEP);
    if (toggleStats)
        m_replayUi.stats = !m_replayUi.stats;
    if (openResearch)
    {
        // The research of the watched player (the first one when everything is shown)
        if (m_replayUi.watch < 0 && !viewPlayers.empty())
            ReplayWatch(viewPlayers.front().slot);
        if (m_hudController->IsResearchWindowOpen())
            m_hudController->CloseResearchWindow();
        else
            m_hudController->OpenResearchWindow();
    }
    if (nextView)
    {
        // Observer -> player 1 -> player 2 ... -> observer
        int next = -1;
        if (m_replayUi.watch < 0)
            next = viewPlayers.empty() ? -1 : viewPlayers.front().slot;
        else
        {
            for (std::size_t i = 0; i < viewPlayers.size(); ++i)
                if (viewPlayers[i].slot == m_replayUi.watch)
                    next = i + 1 < viewPlayers.size() ? viewPlayers[i + 1].slot : -1;
        }
        ReplayWatch(next);
    }
    if (toggleFollow)
    {
        m_replayUi.follow = !m_replayUi.follow;
        m_replayUi.cameraSet = false;
    }

    // Following the camera of the watched player (the first human when the observer follows); a computer player
    // has no camera: the middle of its army
    if (m_replayUi.follow)
    {
        int slot = m_replayUi.watch;
        if (slot < 0)
        {
            for (const auto &p : viewPlayers)
                if (p.human)
                {
                    slot = p.slot;
                    break;
                }
            if (slot < 0 && !viewPlayers.empty())
                slot = viewPlayers.front().slot;
        }
        // The viewer took the camera himself: following ends
        if (m_replayUi.cameraSet
            && (std::fabs(m_camera->GetX() - m_replayUi.lastCamX) > 3.0f
                || std::fabs(m_camera->GetY() - m_replayUi.lastCamY) > 3.0f
                || std::fabs(m_camera->GetZoom() - m_replayUi.lastCamZoom) > 0.001f))
        {
            m_replayUi.follow = false;
        }
        float cx = 0.0f, cy = 0.0f, zoom = m_camera->GetZoom();
        bool have = m_replay && slot >= 0 && m_replay->CameraOf(slot, sim.Tick(), cx, cy, zoom);
        if (!have && slot >= 0)
            have = ArmyCentreOf(slot, cx, cy);
        if (m_replayUi.follow && have)
        {
            const float zoomNow = m_camera->GetZoom();
            const float k = static_cast<float>(std::min(1.0, frameTime * (m_replay ? 5.0 : 2.0)));
            const float newZoom = std::clamp(zoomNow + (zoom - zoomNow) * k, 0.5f, 4.0f);
            const float curX = m_camera->GetX() + m_camera->GetViewportWidth() / zoomNow * 0.5f;
            const float curY = m_camera->GetY() + m_camera->GetViewportHeight() / zoomNow * 0.5f;
            if (std::fabs(newZoom - zoomNow) > 0.0005f)
                m_camera->SetZoom(newZoom);
            m_camera->CenterOn(curX + (cx - curX) * k, curY + (cy - curY) * k);
            m_camera->ClampToBounds(m_map->GetWidth(), m_map->GetHeight());
            m_replayUi.lastCamX = m_camera->GetX();
            m_replayUi.lastCamY = m_camera->GetY();
            m_replayUi.lastCamZoom = m_camera->GetZoom();
            m_replayUi.cameraSet = true;
        }
    }
}

// A custom match watched by its viewer: like a replay, the world runs at the chosen speed and may stand still
void Application::UpdateSpectator(double frameTime)
{
    UpdateReplayInput(frameTime);
    if (!m_spectating || m_state != AppState::SkirmishPlaying)
        return;
    constexpr double kStep = SIMULATION_TIMESTEP;
    if (!m_replayUi.paused)
        m_replayUi.accumulator += frameTime * kSpeeds[m_replayUi.speed];
    else
        m_replayUi.accumulator = 0.0;
    m_replayUi.accumulator = std::min(m_replayUi.accumulator, kStep * 240.0);
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(28);
    int ran = 0;
    while (m_replayUi.accumulator >= kStep && m_state == AppState::SkirmishPlaying)
    {
        Update(static_cast<float>(kStep));
        m_replayUi.accumulator -= kStep;
        if (++ran >= 400 || clock::now() > deadline)
            break;
    }
    if (ran == 0 && m_state == AppState::SkirmishPlaying)
    {
        // No tick this frame: the interface still lives, the world stands
        m_gameplayController->SetSimulationHold(true);
        Update(static_cast<float>(frameTime));
        m_gameplayController->SetSimulationHold(false);
    }
}

void Application::UpdateReplay(double frameTime)
{
    if (!m_replay)
        return;
    UpdateReplayInput(frameTime);
    if (!m_replay || m_state != AppState::SkirmishPlaying)
        return;
    Simulation &sim = m_gameplayController->simulation();
    const std::uint32_t total = m_replay->info().ticks;
    constexpr double kStep = SIMULATION_TIMESTEP;
    if (!m_replayUi.paused && sim.Tick() < total)
        m_replayUi.accumulator += frameTime * kSpeeds[m_replayUi.speed];
    else
        m_replayUi.accumulator = 0.0;
    m_replayUi.accumulator = std::min(m_replayUi.accumulator, kStep * 240.0);

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(28);
    int ran = 0;
    while (m_replayUi.accumulator >= kStep && sim.Tick() < total && m_state == AppState::SkirmishPlaying)
    {
        const std::uint32_t tick = sim.Tick();
        m_gameplayController->SetSimulationHold(false);
        m_replay->ScheduleDue(sim, tick);
        Update(static_cast<float>(kStep));
        // The world is only watched: a victory or defeat of the viewed player ends nothing
        if (m_state == AppState::Victory || m_state == AppState::Defeat)
            m_state = AppState::SkirmishPlaying;
        if (sim.Tick() != tick && m_replay->NeedsChecksum(tick))
        {
            SimContext ctx {*m_entities, *m_map, *m_pathfinder, *m_economy, *m_aiController};
            m_replay->Verify(tick, sim.Checksum(ctx));
        }
        m_replayUi.accumulator -= kStep;
        ++ran;
        if (ran >= 400 || clock::now() > deadline)
            break;
    }
    if (ran == 0)
    {
        // No tick this frame: the interface still lives, the world stands
        m_gameplayController->SetSimulationHold(true);
        Update(static_cast<float>(frameTime));
        if (m_state == AppState::Victory || m_state == AppState::Defeat)
            m_state = AppState::SkirmishPlaying;
        m_gameplayController->SetSimulationHold(false);
    }
    if (m_state != AppState::SkirmishPlaying)
    {
        CloseReplay();
        return;
    }
    const bool finished = sim.Tick() >= total;
    if (finished && !m_replayUi.finished)
    {
        std::cout << "[Replay] Finished at tick " << sim.Tick()
                  << (m_replay->MismatchTick() >= 0 ? ", OUT OF SYNC" : ", in sync") << std::endl;
        if (DebugEnv("OAOW_REPLAY_EXIT"))
            m_running = false;
    }
    m_replayUi.finished = finished;
}

void Application::RenderReplayBar()
{
    if ((!m_replay && !m_spectating) || !m_camera)
        return;
    const bool replay = m_replay != nullptr;
    const int viewW = m_camera->GetViewportWidth();
    const BarLayout l = MakeBar(viewW, replay);
    const int mx = m_input->GetMouseScreenX(), my = m_input->GetMouseScreenY();
    Simulation &sim = m_gameplayController->simulation();
    const std::uint32_t total = replay ? std::max<std::uint32_t>(1, m_replay->info().ticks) : 1u;
    const std::uint32_t tick = replay ? std::min(sim.Tick(), total) : sim.Tick();
    const std::vector<ObserverPlayer> players = ObserverPlayers();

    if (m_replayUi.stats)
        RenderObserverStats(players);

    ui::FillVerticalGradient(*m_renderer, l.bar, ui::WithAlpha(col::Panel, 245), ui::WithAlpha(col::PanelSunken, 230));
    m_renderer->FillRectScreen(0, kBarHeight - 1, viewW, 1, col::Border);
    auto hot = [&](const ui::UiRect &r) {
        return r.Contains(mx, my);
    };
    ui::DrawButton(
        *m_renderer,
        *m_fontRenderer,
        l.play,
        m_replayUi.finished ? _("AGAIN") : (m_replayUi.paused ? _("PLAY") : _("PAUSE")),
        ui::ButtonStyle::Primary,
        hot(l.play));
    const bool allowSpeed = IsSpeedControlAllowed();
    ui::DrawButton(
        *m_renderer,
        *m_fontRenderer,
        l.slower,
        "-",
        ui::ButtonStyle::Secondary,
        hot(l.slower),
        !allowSpeed || m_replayUi.speed <= 0);
    char speed[16];
    std::snprintf(speed, sizeof speed, "x%g", kSpeeds[m_replayUi.speed]);
    m_fontRenderer->DrawString(
        speed,
        l.speed.CenterX(),
        l.speed.CenterY(),
        ui::FONT_BODY_BOLD,
        allowSpeed ? col::Text : col::TextMuted,
        false,
        Anchor::HCENTER | Anchor::VCENTER);
    ui::DrawButton(
        *m_renderer,
        *m_fontRenderer,
        l.faster,
        "+",
        ui::ButtonStyle::Secondary,
        hot(l.faster),
        !allowSpeed || m_replayUi.speed >= kSpeedCount - 1);
    if (replay)
        ui::DrawButton(*m_renderer, *m_fontRenderer, l.restart, "|<", ui::ButtonStyle::Secondary, hot(l.restart));

    // Progress and time (a live match: its clock only)
    if (replay)
    {
        m_renderer->FillRectScreen(l.progress.x, l.progress.y, l.progress.w, l.progress.h, col::PanelSunken);
        ui::DrawProgressBar(*m_renderer, l.progress, static_cast<float>(tick) / static_cast<float>(total), col::Accent);
    }
    m_fontRenderer->DrawString(
        replay ? Clock(tick) + " / " + Clock(total) : std::string(_("Watching")) + "   " + Clock(tick),
        l.progress.CenterX(),
        l.progress.CenterY(),
        ui::FONT_SMALL_BOLD,
        col::Text,
        true,
        Anchor::HCENTER | Anchor::VCENTER);

    // Whose view
    std::string viewName = _("Observer (all)");
    for (const auto &p : players)
        if (p.slot == m_replayUi.watch)
            viewName = p.name;
    ui::DrawComboField(
        *m_renderer,
        *m_fontRenderer,
        l.view,
        ui::FitText(*m_fontRenderer, viewName, ui::FONT_SMALL_BOLD, l.view.w - 40),
        m_replayUi.combo.open >= 0,
        hot(l.view));
    ui::DrawCheckbox(
        *m_renderer, *m_fontRenderer, l.voice, _("Voice the interface"), m_replayUi.interfaceSound, hot(l.voice));
    ui::DrawButton(
        *m_renderer,
        *m_fontRenderer,
        l.follow,
        m_replayUi.follow ? _("FOLLOW: ON") : _("FOLLOW: OFF"),
        m_replayUi.follow ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
        hot(l.follow));
    ui::DrawButton(
        *m_renderer,
        *m_fontRenderer,
        l.stats,
        _("STATS"),
        m_replayUi.stats ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
        hot(l.stats));
    ui::DrawButton(
        *m_renderer,
        *m_fontRenderer,
        l.research,
        _("RESEARCH"),
        m_hudController->IsResearchWindowOpen() ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
        hot(l.research));
    ui::DrawButton(*m_renderer, *m_fontRenderer, l.exit, _("EXIT"), ui::ButtonStyle::Danger, hot(l.exit));
    if (l.bar.Contains(mx, my))
        SoundRequests::MarkHover();

    // The recording does not match this game's world
    if (replay && m_replay->MismatchTick() >= 0)
    {
        m_fontRenderer->DrawString(
            std::string(_("This replay does not play back the same here (differs at tick")) + " "
                + std::to_string(m_replay->MismatchTick()) + ")",
            viewW / 2,
            kBarHeight + 10,
            ui::FONT_BODY_BOLD,
            col::Danger,
            true,
            Anchor::HCENTER | Anchor::TOP);
    }
    else if (replay && !m_replay->info().checksumsComparable)
    {
        m_fontRenderer->DrawString(
            _("Recorded by another game build: it may play out differently."),
            viewW / 2,
            kBarHeight + 10,
            ui::FONT_BODY_BOLD,
            col::Accent,
            true,
            Anchor::HCENTER | Anchor::TOP);
    }
    if (m_replayUi.finished)
    {
        m_fontRenderer->DrawString(
            _("REPLAY FINISHED"),
            viewW / 2,
            m_windowH / 2 - 20,
            ui::FONT_H1,
            col::Accent,
            true,
            Anchor::HCENTER | Anchor::VCENTER);
    }

    // The open view list, over everything
    if (m_replayUi.combo.open >= 0)
    {
        std::vector<std::string> items {_("Observer (all)")};
        std::vector<SDL_Color> swatches {col::TextDim};
        int current = 0, index = 1;
        for (const auto &p : players)
        {
            items.push_back(p.name);
            swatches.push_back(ArmyColor(p.color));
            if (p.slot == m_replayUi.watch)
                current = index;
            ++index;
        }
        ui::DrawComboList(
            *m_renderer,
            *m_fontRenderer,
            l.view,
            items,
            current,
            m_replayUi.combo,
            m_windowH,
            mx,
            my,
            nullptr,
            &swatches);
    }
}

// Every player of the watched match at a glance: treasury, army, kills and losses, and the research under way
void Application::RenderObserverStats(const std::vector<ObserverPlayer> &players)
{
    if (players.empty())
        return;
    Simulation &sim = m_gameplayController->simulation();
    SimContext ctx {*m_entities, *m_map, *m_pathfinder, *m_economy, *m_aiController};
    auto sortedPlayers = players;
    const auto defeated = [&](const ObserverPlayer &p) {
        return m_entities->IsEliminated(static_cast<PlayerId>(p.slot));
    };
    std::stable_partition(
        sortedPlayers.begin(), sortedPlayers.end(), [&](const ObserverPlayer &p) { return !defeated(p); });
    const bool hasDefeated = std::any_of(players.begin(), players.end(), defeated);
    const int rowH = 28, headH = 64;
    const int colName = 14, colCredits = 220, colArmy = 300, colKills = 370, colLosses = 470, colResearch = 560;
    const int w = std::min(m_camera->GetViewportWidth() - 24, 900);
    const int h = headH + static_cast<int>(players.size()) * rowH + 10 + (hasDefeated ? rowH : 0);
    const int x = 12, y = kBarHeight + 34;
    m_renderer->FillRectScreen(x, y, w, h, ui::WithAlpha(col::Panel, 236));
    m_renderer->DrawRectScreen(x, y, w, h, col::BorderStrong);
    m_renderer->FillRectScreen(x, y, w, 2, col::Accent);
    m_fontRenderer->DrawString(_("STATISTICS"), x + colName, y + 10, ui::FONT_BODY_BOLD, col::Accent, false);
    const int hy = y + 38;
    for (const auto &[cx, text] : std::initializer_list<std::pair<int, const char *>> {
             {colName + 22, _("Player")},
             {colCredits, _("Credits")},
             {colArmy, _("Army")},
             {colKills, _("Kills")},
             {colLosses, _("Losses")},
             {colResearch, _("Research")}})
        m_fontRenderer->DrawString(
            ui::FitText(*m_fontRenderer, text, ui::FONT_SMALL_BOLD, 96),
            x + cx,
            hy,
            ui::FONT_SMALL_BOLD,
            col::TextDim,
            false);
    int ry = y + headH;
    bool defeatedHeading = false;
    for (const ObserverPlayer &p : sortedPlayers)
    {
        const bool isDefeated = defeated(p);
        if (isDefeated && !defeatedHeading)
        {
            m_fontRenderer->DrawString(_("Defeated"), x + colName, ry + 6, ui::FONT_SMALL_BOLD, col::TextMuted, false);
            ry += rowH;
            defeatedHeading = true;
        }
        const PlayerId id = static_cast<PlayerId>(p.slot);
        const int cy = ry + rowH / 2 - 1;
        if (p.slot == m_replayUi.watch)
            m_renderer->FillRectScreen(x + 4, ry, w - 8, rowH - 2, col::Hover);
        m_renderer->FillRectScreen(x + colName, cy - 7, 14, 14, isDefeated ? col::TextMuted : ArmyColor(p.color));
        m_renderer->DrawRectScreen(x + colName, cy - 7, 14, 14, col::Border);
        auto text = [&](int cx, std::string_view t, SDL_Color c, int width) {
            m_fontRenderer->DrawString(
                ui::FitText(*m_fontRenderer, t, ui::FONT_BODY, width),
                x + cx,
                cy,
                ui::FONT_BODY,
                isDefeated ? col::TextMuted : c,
                false,
                Anchor::LEFT | Anchor::VCENTER);
        };
        text(colName + 22, p.name, col::Text, colCredits - colName - 30);
        const AIController *ai = m_gameplayController->MatchAi(id);
        const int credits = ai ? ai->GetEnemyCredits() : sim.EconomyOf(id, ctx).GetCredits();
        int army = 0;
        for (const Entity &e : m_entities->GetAllEntities())
            army += e.alive && e.state != ActionState::Die && !e.isBuilding && e.owner == id;
        const EntityManager::PlayerStats &st = m_entities->GetPlayerStats(id);
        text(colCredits, std::to_string(credits), col::Accent, 80);
        text(colArmy, std::to_string(army), col::Text, 70);
        text(colKills, std::to_string(st.unitsKilled + st.buildingsDestroyed), col::Text, 60);
        text(colLosses, std::to_string(st.unitsLost + st.buildingsLost), col::Text, 70);
        // The research: the technology under way with its progress, and how many are done
        const ResearchProgress r = m_gameplayController->ResearchOf(id);
        int done = 0;
        for (int i = 0; i < kResearchCount; ++i)
            done += IsResearched(id, i) ? 1 : 0;
        const int barW = w - colResearch - 16;
        std::string research = std::to_string(done) + "/" + std::to_string(kResearchCount);
        if (!isDefeated && r.active >= 0 && r.active < kResearchCount)
        {
            const float ratio = std::clamp(r.timer / std::max(0.01f, r.duration), 0.0f, 1.0f);
            ui::DrawProgressBar(*m_renderer, {x + colResearch, cy + 8, barW, 4}, ratio, col::Info);
            research += "  " + std::string(_(researchDef(Players::FactionOf(id), r.active).name)) + "  "
                        + std::to_string(static_cast<int>(ratio * 100.0f)) + "%";
        }
        text(colResearch, research, r.active >= 0 ? col::Info : col::TextDim, barW);
        ry += rowH;
    }
}

// Starts the recording when a match of this computer alone begins, ends it when the match is over or left
void Application::UpdateLocalReplay()
{
    const bool inMatch = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying
                         || m_state == AppState::Paused || m_state == AppState::Briefing || m_state == AppState::Victory
                         || m_state == AppState::Defeat;
    const bool recording = m_localRecorder && m_localRecorder->Active();
    if (recording && (!inMatch || m_replay || m_net || m_mission->GetLoadSerial() != m_localRecordSerial))
        FinishLocalReplay("left");
    if (!inMatch || m_replay || m_net || m_headless || m_state == AppState::Victory || m_state == AppState::Defeat)
        return;
    if (m_mission->GetLoadSerial() == m_localRecordSerial)
        return;
    // The simulation is reset for the new mission on the first update of the game: the recording starts after it
    if (m_gameplayController->MissionSerial() != m_mission->GetLoadSerial())
        return;
    m_localRecordSerial = m_mission->GetLoadSerial();
    Simulation &sim = m_gameplayController->simulation();
    if (sim.Tick() > 1)
        return; // the match ran already (it would not play back from its start)
    m_localTicks = sim.Tick();

    ReplayRecorder::Info info;
    MatchSetup setup;
    const int missionId = m_mission->GetCurrentMissionId();
    if (m_mission->IsMatch() && !m_mission->IsOnlineMatch())
    {
        setup = m_mission->matchSetup();
        info.mode = "custom";
    }
    else if (
        !m_mission->IsMatch() && !m_mission->IsSkirmish() && missionId >= 0
        && missionId < MissionController::kCampaignMissionCount)
    {
        // A campaign mission: the setup only names the map and the two sides (the mission sets the world up)
        const auto &campaign = Missions::Campaign(PlayerFaction());
        setup.mapPath = LoaderResources::MissionMapPath(campaign[missionId].data.mapFile);
        setup.local = Players::Local();
        setup.slots[setup.local] = {true, PlayerFaction(), 0, 0, true, 1};
        setup.slots[setup.local == 0 ? 1 : 0]
            = {true, AiFaction(), 1, 0, false, static_cast<std::uint8_t>(std::clamp(m_settings.difficulty, 0, 3))};
        info.mode = "campaign";
        info.mission = missionId;
        info.playerFaction = static_cast<int>(PlayerFaction());
        info.difficulty = std::clamp(m_settings.difficulty, 0, 3);
    }
    else
        return; // the test map and the like are not recorded
    if (setup.mapHash == 0)
        setup.mapHash = MapFiles::HashOf(setup.mapPath);
    setup.localOnly = true;
    info.setupBytes = NetMatch::EncodeSetup(setup);
    info.setup = setup;
    info.names.assign(kMaxPlayers, std::string());
    if (setup.local < kMaxPlayers && setup.slots[setup.local].human)
        info.names[setup.local] = m_settings.playerName;
    info.ticksPerTurn = 1;
    info.turnMs = static_cast<std::uint32_t>(SIMULATION_TIMESTEP * 1000.0);
    info.assetsHash = NetMatch::AssetsHash();
    if (!m_localRecorder)
        m_localRecorder = std::make_unique<ReplayRecorder>();
    m_localRecorder->Begin(std::move(info));
    m_localCommands.clear();
    m_localViewX = m_localViewY = -1e9f;
    m_localViewZoom = 0.0f;
    Simulation *simPtr = &sim;
    sim.SetRecordHook([this, simPtr](std::uint32_t tick, const SimCommand &c) {
        (void) simPtr;
        SimCommand copy = c;
        copy.debug = false;
        m_localCommands[tick].push_back(std::move(copy));
    });
    std::cout << "[Replay] Recording the " << (m_mission->IsMatch() ? "custom game" : "campaign mission") << std::endl;
}

// After `tick` of the recorded match ran: the world checksum and (now and then) the player's camera
void Application::LocalTickRan(std::uint32_t tick)
{
    if (!m_localRecorder || !m_localRecorder->Active())
        return;
    m_localTicks = tick + 1;
    if (tick % NetMatch::kHashIntervalTicks == 0)
    {
        SimContext ctx {*m_entities, *m_map, *m_pathfinder, *m_economy, *m_aiController};
        m_localRecorder->OnChecksum(tick, m_gameplayController->simulation().Checksum(ctx));
    }
    if (tick % NetMatch::kViewIntervalTicks == 0 && m_camera && !m_spectating)
    {
        const float zoom = std::max(0.01f, m_camera->GetZoom());
        const float x = m_camera->GetX() + m_camera->GetViewportWidth() / zoom * 0.5f;
        const float y = m_camera->GetY() + m_camera->GetViewportHeight() / zoom * 0.5f;
        if (std::abs(x - m_localViewX) >= TILE_WIDTH || std::abs(y - m_localViewY) >= TILE_HEIGHT
            || zoom != m_localViewZoom)
        {
            m_localViewX = x;
            m_localViewY = y;
            m_localViewZoom = zoom;
            SimCommand c;
            c.type = CommandType::View;
            c.player = Players::Local();
            c.x = static_cast<int>(x);
            c.y = static_cast<int>(y);
            c.a = static_cast<std::int32_t>(zoom * 100.0f + 0.5f); // (as a network match sends it)
            m_localCommands[tick].push_back(std::move(c));
        }
    }
}

void Application::FinishLocalReplay(std::string_view outcome)
{
    if (m_gameplayController)
        m_gameplayController->simulation().SetRecordHook(nullptr);
    if (!m_localRecorder || !m_localRecorder->Active())
        return;
    // One turn per tick, each with what the player did on it
    for (std::uint32_t t = 0; t < m_localTicks; ++t)
    {
        Net::TurnBundle bundle;
        bundle.turn = t;
        auto it = m_localCommands.find(t);
        if (it != m_localCommands.end() && !it->second.empty())
        {
            std::map<PlayerId, std::vector<SimCommand>> bySlot;
            for (const SimCommand &c : it->second)
                bySlot[c.player].push_back(c);
            // In the batches a turn bundle carries, the same as a network game sends them: one block of everything
            // (more than 64 commands, or a big selection) made the whole file unreadable
            for (const auto &[slot, commands] : bySlot)
            {
                for (std::vector<std::uint8_t> &bytes : NetMatch::EncodeBatches(commands))
                    bundle.players.push_back({slot, std::move(bytes)});
            }
            if (bundle.players.size() > Net::kMaxBatchesPerTurn)
            {
                std::cerr << "[Replay] Tick " << t << ": " << bundle.players.size() - Net::kMaxBatchesPerTurn
                          << " batches of commands over the limit were left out" << std::endl;
                bundle.players.resize(Net::kMaxBatchesPerTurn);
            }
        }
        m_localRecorder->OnBundle(bundle);
    }
    m_localCommands.clear();
    m_localRecorder->Finish(outcome, m_localTicks);
}

} // namespace OpenArtOfWar
