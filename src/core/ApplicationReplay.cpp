#include "core/Application.hpp"
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

constexpr float kSpeeds[] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
constexpr int kSpeedCount = 5;
constexpr int kBarHeight = 64; // a thin row for the sound check box on top, the controls under it
constexpr int kTopRow = 26;

struct BarLayout
{
    ui::UiRect bar, voice, play, slower, speed, faster, restart, progress, view, follow, exit;
};

BarLayout MakeBar(int viewW)
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
    place(l.restart, 30);
    int right = viewW - 8;
    auto placeRight = [&](ui::UiRect &r, int w) {
        right -= w;
        r = {right, y, w, h};
        right -= 6;
    };
    placeRight(l.exit, 70);
    placeRight(l.follow, 120);
    placeRight(l.view, 210);
    l.progress = {x + 6, y + 6, std::max(60, right - x - 6), h - 12};
    return l;
}

std::string Clock(uint32_t ticks)
{
    const uint32_t s = ticks / 20;
    char buf[24];
    std::snprintf(buf, sizeof buf, "%u:%02u", s / 60, s % 60);
    return buf;
}

std::string TitleOf(const std::string &path)
{
    return std::filesystem::path(path).stem().string();
}

} // namespace

bool Application::OpenReplay(const std::string &path)
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
    ReloadGameData(); // the match plays with the data it was recorded with
    BeginMapLoading(TitleOf(setup.mapPath), setup.mapPath);
    if (!m_mission->LoadMatch(setup, *m_map, *m_entities, *m_economy, *m_pathfinder))
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
    if (had)
        ReloadGameData(); // the player's own data again
}

// Whose view is shown: a player's (his fog, economy and colours) or -1: everything, as an observer
void Application::ReplayWatch(int slot)
{
    m_replayUi.watch = slot;
    m_hudController->SetShowAll(slot < 0);
    if (slot < 0)
        return;
    Players::setup(m_replaySetup.slots, static_cast<PlayerId>(slot));
    m_replaySetup.local = static_cast<PlayerId>(slot);
    m_map->SetViewerTeam(Players::TeamOf(static_cast<PlayerId>(slot)));
    m_map->ResetAllFog(FogState::Hidden); // what this player has seen is not known: it is seen again from now on
    m_economy->SetPlayer(static_cast<PlayerId>(slot));
    m_entities->RebuildBlueprints();
    m_gameplayController->ClearSelection();
}

void Application::UpdateReplayInput(double frameTime)
{
    Simulation &sim = m_gameplayController->simulation();
    const uint32_t total = m_replay->info().ticks;
    m_replayUi.finished = sim.Tick() >= total;

    // Keys
    if (m_input->IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        CloseReplay();
        m_state = AppState::MenuReplay;
        if (m_replayMenu)
            m_replayMenu->Refresh();
        return;
    }
    bool togglePause = m_input->IsKeyJustPressed(SDL_SCANCODE_SPACE);
    bool restart = m_input->IsKeyJustPressed(SDL_SCANCODE_HOME);
    int speedStep = 0;
    if (m_input->IsKeyJustPressed(SDL_SCANCODE_EQUALS) || m_input->IsKeyJustPressed(SDL_SCANCODE_KP_PLUS))
        speedStep = 1;
    if (m_input->IsKeyJustPressed(SDL_SCANCODE_MINUS) || m_input->IsKeyJustPressed(SDL_SCANCODE_KP_MINUS))
        speedStep = -1;
    bool nextView = m_input->IsKeyJustPressed(SDL_SCANCODE_TAB);
    bool toggleFollow = m_input->IsKeyJustPressed(SDL_SCANCODE_F);

    // The bar (a click on it is taken before the game sees it)
    const BarLayout bar = MakeBar(m_camera->GetViewportWidth());
    const int mx = m_input->GetMouseScreenX(), my = m_input->GetMouseScreenY();
    // The open list of the view combo box takes the input first
    const auto &viewPlayers = m_replay->info().players;
    if (m_replayUi.comboOpen >= 0)
    {
        ui::ComboState cs;
        cs.open = 0;
        cs.scroll = m_replayUi.comboScroll;
        int picked = -1;
        float lx, ly;
        const bool clicked = m_input->WasLeftClickIssued(lx, ly);
        ui::ComboUpdate(cs, bar.view, static_cast<int>(viewPlayers.size()) + 1, m_windowH, mx, my, clicked, m_input->ConsumeWheel(), picked);
        m_replayUi.comboScroll = cs.scroll;
        if (cs.open < 0)
            m_replayUi.comboOpen = -1;
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
            else if (bar.slower.Contains(mx, my))
                speedStep = -1;
            else if (bar.faster.Contains(mx, my))
                speedStep = 1;
            else if (bar.restart.Contains(mx, my))
                restart = true;
            else if (bar.view.Contains(mx, my))
            {
                m_replayUi.comboOpen = 0;
                m_replayUi.comboScroll = 0;
            }
            else if (bar.follow.Contains(mx, my))
                toggleFollow = true;
            else if (bar.exit.Contains(mx, my))
            {
                CloseReplay();
                m_state = AppState::MenuReplay;
                if (m_replayMenu)
                    m_replayMenu->Refresh();
                return;
            }
        }
    }

    if (restart || (togglePause && m_replayUi.finished))
    {
        StartReplay(m_replayUi.watch >= 0 ? m_replayUi.watch : m_replaySetup.local);
        return;
    }
    if (togglePause)
        m_replayUi.paused = !m_replayUi.paused;
    if (speedStep != 0)
        m_replayUi.speed = std::clamp(m_replayUi.speed + speedStep, 0, kSpeedCount - 1);
    if (nextView)
    {
        // Observer -> player 1 -> player 2 ... -> observer
        const auto &players = m_replay->info().players;
        int next = -1;
        if (m_replayUi.watch < 0)
            next = players.empty() ? -1 : players.front().slot;
        else
        {
            for (size_t i = 0; i < players.size(); ++i)
                if (players[i].slot == m_replayUi.watch)
                    next = i + 1 < players.size() ? players[i + 1].slot : -1;
        }
        ReplayWatch(next);
    }
    if (toggleFollow)
    {
        m_replayUi.follow = !m_replayUi.follow;
        m_replayUi.cameraSet = false;
    }

    // Following the camera of the watched player (the first human when the observer follows)
    if (m_replayUi.follow)
    {
        int slot = m_replayUi.watch;
        if (slot < 0)
        {
            for (const auto &p : m_replay->info().players)
                if (p.human)
                {
                    slot = p.slot;
                    break;
                }
        }
        // The viewer took the camera himself: following ends
        if (m_replayUi.cameraSet
            && (std::fabs(m_camera->GetX() - m_replayUi.lastCamX) > 3.0f
                || std::fabs(m_camera->GetY() - m_replayUi.lastCamY) > 3.0f
                || std::fabs(m_camera->GetZoom() - m_replayUi.lastCamZoom) > 0.001f))
        {
            m_replayUi.follow = false;
        }
        float cx, cy, zoom;
        if (m_replayUi.follow && slot >= 0 && m_replay->CameraOf(slot, sim.Tick(), cx, cy, zoom))
        {
            const float zoomNow = m_camera->GetZoom();
            const float k = static_cast<float>(std::min(1.0, frameTime * 5.0));
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

void Application::UpdateReplay(double frameTime)
{
    if (!m_replay)
        return;
    UpdateReplayInput(frameTime);
    if (!m_replay || m_state != AppState::SkirmishPlaying)
        return;
    Simulation &sim = m_gameplayController->simulation();
    const uint32_t total = m_replay->info().ticks;
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
        const uint32_t tick = sim.Tick();
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
    const bool finished = sim.Tick() >= total;
    if (finished && !m_replayUi.finished)
    {
        std::cout << "[Replay] Finished at tick " << sim.Tick() << (m_replay->MismatchTick() >= 0 ? ", OUT OF SYNC" : ", in sync")
                  << std::endl;
        if (DebugEnv("OAOW_REPLAY_EXIT"))
            m_running = false;
    }
    m_replayUi.finished = finished;
}

void Application::RenderReplayBar()
{
    if (!m_replay || !m_camera)
        return;
    const int viewW = m_camera->GetViewportWidth();
    const BarLayout l = MakeBar(viewW);
    const int mx = m_input->GetMouseScreenX(), my = m_input->GetMouseScreenY();
    Simulation &sim = m_gameplayController->simulation();
    const uint32_t total = std::max<uint32_t>(1, m_replay->info().ticks);
    const uint32_t tick = std::min(sim.Tick(), total);

    ui::FillVerticalGradient(*m_renderer, l.bar, ui::WithAlpha(col::Panel, 245), ui::WithAlpha(col::PanelSunken, 230));
    m_renderer->FillRectScreen(0, kBarHeight - 1, viewW, 1, col::Border);
    auto hot = [&](const ui::UiRect &r) { return r.Contains(mx, my); };
    ui::DrawButton(
        *m_renderer,
        *m_fontRenderer,
        l.play,
        m_replayUi.finished ? _("AGAIN") : (m_replayUi.paused ? _("PLAY") : _("PAUSE")),
        ui::ButtonStyle::Primary,
        hot(l.play));
    ui::DrawButton(*m_renderer, *m_fontRenderer, l.slower, "-", ui::ButtonStyle::Secondary, hot(l.slower));
    char speed[16];
    std::snprintf(speed, sizeof speed, "x%g", kSpeeds[m_replayUi.speed]);
    m_fontRenderer->DrawString(
        speed, l.speed.CenterX(), l.speed.CenterY(), ui::FONT_BODY_BOLD, col::Text, false, Anchor::HCENTER | Anchor::VCENTER);
    ui::DrawButton(*m_renderer, *m_fontRenderer, l.faster, "+", ui::ButtonStyle::Secondary, hot(l.faster));
    ui::DrawButton(*m_renderer, *m_fontRenderer, l.restart, "|<", ui::ButtonStyle::Secondary, hot(l.restart));

    // Progress and time
    m_renderer->FillRectScreen(l.progress.x, l.progress.y, l.progress.w, l.progress.h, col::PanelSunken);
    ui::DrawProgressBar(*m_renderer, l.progress, static_cast<float>(tick) / static_cast<float>(total), col::Accent);
    m_fontRenderer->DrawString(
        Clock(tick) + " / " + Clock(total),
        l.progress.CenterX(),
        l.progress.CenterY(),
        ui::FONT_SMALL_BOLD,
        col::Text,
        true,
        Anchor::HCENTER | Anchor::VCENTER);

    // Whose view
    std::string viewName = _("Observer (all)");
    for (const auto &p : m_replay->info().players)
        if (p.slot == m_replayUi.watch)
            viewName = p.name;
    ui::DrawComboField(
        *m_renderer,
        *m_fontRenderer,
        l.view,
        ui::FitText(*m_fontRenderer, viewName, ui::FONT_SMALL_BOLD, l.view.w - 40),
        m_replayUi.comboOpen >= 0,
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
    ui::DrawButton(*m_renderer, *m_fontRenderer, l.exit, _("EXIT"), ui::ButtonStyle::Danger, hot(l.exit));
    if (l.bar.Contains(mx, my))
        SoundRequests::MarkHover();

    // The recording does not match this game's world
    if (m_replay->MismatchTick() >= 0)
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
    if (m_replayUi.comboOpen >= 0)
    {
        std::vector<std::string> items {_("Observer (all)")};
        std::vector<SDL_Color> swatches {col::TextDim};
        int current = 0, index = 1;
        for (const auto &p : m_replay->info().players)
        {
            items.push_back(p.name);
            swatches.push_back(ArmyColor(p.color));
            if (p.slot == m_replayUi.watch)
                current = index;
            ++index;
        }
        ui::ComboState cs;
        cs.open = 0;
        cs.scroll = m_replayUi.comboScroll;
        ui::DrawComboList(*m_renderer, *m_fontRenderer, l.view, items, current, cs, m_windowH, mx, my, nullptr, &swatches);
    }
}

} // namespace OpenArtOfWar
