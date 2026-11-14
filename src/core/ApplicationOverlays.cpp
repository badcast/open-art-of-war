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
#include "storage/SaveManager.hpp"
#include "debug/DebugOverlay.hpp"

#include <SDL3/SDL.h>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cmath>

// Application: loading screens and the on-top overlays (map edge fade, network banner, notices, scoreboard, fps and net
// statistics)

namespace OpenArtOfWar
{

bool Application::UpdateTacticalView()
{
    const bool inGame = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying
                        || m_state == AppState::Paused || m_state == AppState::Victory || m_state == AppState::Defeat;
    const bool tactical
        = inGame && m_hudController && m_camera && m_camera->GetZoom() < m_hudController->TacticalZoomThreshold();
    if (m_hudController)
        m_hudController->SetTactical(tactical);
    if (m_gameplayController)
        m_gameplayController->SetTactical(tactical);
    return tactical;
}

// The edge of the map melts into the dark background instead of ending in a hard line
void Application::RenderMapEdgeFade(const Camera &camera)
{
    (void)camera;
}

void Application::DrawLoadingFrame(
    std::string_view title, std::string_view status, float progress, SDL_Texture *preview, int pw, int ph)
{
    // Keep the window answering while the main loop is busy
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_QUIT)
        {
            SDL_PushEvent(&event); // the main loop handles it once it runs
            break;
        }
    }
    int w = 0, h = 0;
#if OAOW_TOUCH
    w = m_windowW;
    h = m_windowH;
#else
    SDL_GetRenderOutputSize(m_sdlRenderer, &w, &h);
#endif
    SDL_SetRenderTarget(m_sdlRenderer, nullptr);
    SDL_SetRenderClipRect(m_sdlRenderer, nullptr);
    SDL_SetRenderDrawColor(m_sdlRenderer, 8, 10, 14, 255);
    SDL_RenderClear(m_sdlRenderer);
    ui::DrawLoadingScreen(*m_renderer, *m_fontRenderer, w, h, title, status, progress, SDL_GetTicks(), preview, pw, ph);
    m_renderer->Present();
}

void Application::ShowLoading(std::string_view status, float progress)
{
    if (m_headless || !m_sdlRenderer || !m_renderer || !m_fontRenderer)
        return;
    DrawLoadingFrame("OPEN ART OF WAR: RESISTANCE ARISEN", status, progress, nullptr, 0, 0);
}

void Application::BeginMapLoading(std::string_view title, std::string_view mapPath)
{
    if (m_headless || !m_sdlRenderer || !m_renderer || !m_fontRenderer)
        return;
    m_mapLoading = true;
    m_mapLoadStartMs = SDL_GetTicks();
    m_mapLoadTitle.assign(title);
    m_mapLoadTexture = m_loadingPreview.Get(
        m_sdlRenderer,
        *m_texManager,
        "loading:" + std::string(mapPath),
        std::string(mapPath),
        {},
        m_mapLoadW,
        m_mapLoadH);
    DrawLoadingFrame(m_mapLoadTitle, _("Loading the map..."), 0.05f, m_mapLoadTexture, m_mapLoadW, m_mapLoadH);
}

void Application::EndMapLoading()
{
    if (!m_mapLoading)
        return;
    constexpr Uint32 kMinMs = 3000;
    // The work is done; the screen stays until the minimum time is over, the bar going on meanwhile
    for (Uint32 elapsed = SDL_GetTicks() - m_mapLoadStartMs; elapsed < kMinMs;
         elapsed = SDL_GetTicks() - m_mapLoadStartMs)
    {
        DrawLoadingFrame(
            m_mapLoadTitle,
            _("Loading the map..."),
            0.05f + 0.9f * static_cast<float>(elapsed) / kMinMs,
            m_mapLoadTexture,
            m_mapLoadW,
            m_mapLoadH);
        // The connection keeps being served behind the loading screen (a silent peer times out on a slow
        // link); only the transport, this runs inside updateNetwork when a match starts
        if (m_net)
            m_net->Update(static_cast<std::uint32_t>(SDL_GetTicks()));
        SDL_Delay(16);
    }
    DrawLoadingFrame(m_mapLoadTitle, _("Ready"), 1.0f, m_mapLoadTexture, m_mapLoadW, m_mapLoadH);
    m_mapLoading = false;
}

void Application::RenderNetworkBanner()
{
    if (!InNetworkMatch()
        || !(m_state == AppState::SkirmishPlaying || m_state == AppState::Victory || m_state == AppState::Defeat))
        return;
    const std::uint32_t tick = m_gameplayController->simulation().Tick();
    std::string text;
    SDL_Color color = ui::color::Accent;
    if (m_net->Desynced())
    {
        text = std::string(_("OUT OF SYNC at tick ")) + std::to_string(m_net->DesyncTick())
               + _(": the players' games differ, the result is not reliable");
        if (!m_desyncDumpPath.empty())
            text += std::string(" (") + _("world state saved: ")
                    + std::filesystem::path(m_desyncDumpPath).filename().string() + ")";
        color = ui::color::Danger;
    }
    else if (m_net->paused())
    {
        std::string who = "?";
        if (const Net::Roster *roster = m_net->roster())
        {
            for (const Net::RosterEntry &e : roster->players)
                if (e.slot == m_net->PausedBy())
                    who = e.name;
        }
        text = std::string(_("PAUSED by ")) + who + " - " + _("P to go on");
    }
    else if (m_net->Reconnecting())
    {
        text = _("Connection lost: reconnecting...");
        color = ui::color::Danger;
    }
    else if (m_net->TurnsAhead(tick) > 10)
    {
        const std::uint32_t total = std::max<std::uint32_t>(1, m_net->TicksReceived());
        text = std::string(_("Catching up with the match: "))
               + std::to_string(std::min<std::uint32_t>(100, tick * 100 / total)) + "%";
    }
    else if (m_net->StalledSeconds(SDL_GetTicks()) > 1.5f)
    {
        text = _("Waiting for the host...");
    }
    if (text.empty())
        return;
    const int w = m_fontRenderer->GetTextWidth(text, ui::FONT_BODY_BOLD) + 40;
    const int x = (m_camera->GetViewportWidth() - w) / 2, y = 44;
    m_renderer->FillRectScreen(x, y, w, 34, ui::color::Panel);
    m_renderer->DrawRectScreen(x, y, w, 34, color);
    m_fontRenderer->DrawString(
        text, x + w / 2, y + 17, ui::FONT_BODY_BOLD, color, false, Anchor::HCENTER | Anchor::VCENTER);
}

void Application::TrackNetPlayers(std::uint32_t nowMs)
{
    const Net::Roster *roster = m_net ? m_net->roster() : nullptr;
    // While this computer is the one cut off, the roster is stale: nothing to report about the others
    if (!roster || !m_net->connected())
        return;
    std::array<const Net::RosterEntry *, 8> now {};
    for (const Net::RosterEntry &e : roster->players)
    {
        if (e.slot < now.size())
            now[e.slot] = &e;
    }
    char buf[200];
    for (std::size_t slot = 0; slot < m_netPlayers.size(); ++slot)
    {
        NetPlayer &p = m_netPlayers[slot];
        const Net::RosterEntry *e = now[slot];
        const bool local = slot == m_net->LocalSlot();
        if (e)
        {
            if (!e->name.empty())
                p.name = e->name;
            if (p.present && !local)
            {
                if (p.connected && !e->connected)
                {
                    std::snprintf(buf, sizeof buf, _("Player %s has disconnected"), p.name.c_str());
                    PushNotice(buf, nowMs);
                    std::cout << "[Net] Player " << p.name << " (slot " << slot << ") disconnected" << std::endl;
                }
                else if (!p.connected && e->connected)
                {
                    std::snprintf(buf, sizeof buf, _("Player %s is back"), p.name.c_str());
                    PushNotice(buf, nowMs);
                }
            }
            p.present = true;
            p.connected = e->connected;
        }
        else if (p.present)
        {
            // Out of the roster: left the match, or gone longer than the reconnect window. Its base and
            // units are destroyed by the turn that announces it (see CommandType::PlayerLeft).
            if (!local)
            {
                std::snprintf(
                    buf,
                    sizeof buf,
                    p.connected ? _("Player %s has disconnected") : _("Player %s did not come back"),
                    p.name.c_str());
                PushNotice(buf, nowMs);
                std::snprintf(buf, sizeof buf, _("The base of %s is destroyed"), p.name.c_str());
                PushNotice(buf, nowMs);
            }
            p.present = false;
            p.connected = false;
            p.left = true;
            std::cout << "[Net] Player " << p.name << " (slot " << slot << ") left the match" << std::endl;
        }
    }
}

namespace
{
constexpr std::uint32_t kNoticeShowMs = 7000;
} // namespace

void Application::PushNotice(std::string_view text, std::uint32_t nowMs)
{
    constexpr std::uint32_t kShowMs = kNoticeShowMs;
    constexpr std::size_t kMaxNotices = 5;
    m_notices.push_back({std::string(text), nowMs + kShowMs});
    if (m_notices.size() > kMaxNotices)
        m_notices.erase(m_notices.begin(), m_notices.end() - kMaxNotices);
}

void Application::RenderNotices()
{
    const std::uint32_t nowMs = SDL_GetTicks();
    m_notices.erase(
        std::remove_if(m_notices.begin(), m_notices.end(), [&](const Notice &n) { return nowMs >= n.untilMs; }),
        m_notices.end());
    if (m_notices.empty()
        || !(
            m_state == AppState::SkirmishPlaying || m_state == AppState::CampaignPlaying || m_state == AppState::Paused
            || m_state == AppState::Victory || m_state == AppState::Defeat))
        return;
    // Toasts at the left of the map, one under another: each slides in from the left edge, stays and slides out again
    const int viewW = m_camera->GetViewportWidth();
    const bool observing = (m_replay || m_spectating) && m_hudController;
    int y = observing ? m_hudController->ObserverBarHeight() + 10 : 86; // under the network banner / the bar
    const int height = m_touchUi ? 44 : 32;
    for (const Notice &n : m_notices)
    {
        const float elapsed = static_cast<float>(kNoticeShowMs - (n.untilMs - nowMs)) / 1000.0f;
        ui::DrawToast(
            *m_renderer,
            *m_fontRenderer,
            12,
            y,
            height,
            std::max(160, viewW / 2),
            n.text,
            ui::ToastSlide(elapsed, static_cast<float>(kNoticeShowMs) / 1000.0f),
            ui::color::Danger);
        y += height + 6;
    }
}

void Application::RenderScoreboard()
{
    if (!m_input || (!m_input->isKeyPressed(SDL_SCANCODE_TAB) && !m_scoreboardOpen) || !m_entities)
        return;
    if (!(m_state == AppState::SkirmishPlaying || m_state == AppState::CampaignPlaying || m_state == AppState::Victory
          || m_state == AppState::Defeat))
        return;
    if (m_menuController->IsInGameMenuOpen())
        return;
    namespace col = ui::color;
    struct Row
    {
        PlayerId id;
        int team {0};
        std::string name;
        std::string faction;
        SDL_Color color;
        std::string colorName;
        int points;
        int kills;
        int losses;
        bool gone;
        bool defeated;
    };
    std::vector<Row> rows;
    for (PlayerId id = 0; id < kMaxPlayers; ++id)
    {
        if (!Players::IsActive(id))
            continue;
        const PlayerSlot &slot = Players::Slot(id);
        Row r;
        r.id = id;
        r.team = slot.team;
        r.gone = false;
        r.defeated = m_entities->IsEliminated(id);
        if (!slot.human)
        {
            static const char *const kLevels[4] = {"Easy", "Medium", "Hard", "Hardcore"};
            r.name = std::string(_("AI")) + " (" + _(kLevels[slot.aiLevel & 3]) + ")";
        }
        else if (InNetworkMatch() && !m_netPlayers[id].name.empty())
        {
            r.name = m_netPlayers[id].name;
            r.gone = m_netPlayers[id].left;
        }
        else if (id == Players::Local())
        {
            r.name = m_settings.playerName;
        }
        else
        {
            r.name = std::string(_("Player")) + " " + std::to_string(id + 1);
        }
        r.faction = slot.faction == Faction::Confederation ? _("Confederation") : _("Resistance");
        r.color = slot.color == 0 ? col::TextDim : ArmyColor(slot.color);
        r.colorName = _(ArmyColorName(slot.color));
        const EntityManager::PlayerStats &st = m_entities->GetPlayerStats(id);
        r.points = m_entities->GetEfficiencyPoints(id);
        r.kills = st.unitsKilled + st.buildingsDestroyed;
        r.losses = st.unitsLost + st.buildingsLost;
        rows.push_back(r);
    }
    if (rows.empty())
        return;
    const int localTeam = Players::Slot(Players::Local()).team;
    // Group by team (local player's team first), then alive before defeated, then best points
    std::stable_sort(rows.begin(), rows.end(), [localTeam](const Row &a, const Row &b) {
        const bool aLocal = (a.team == localTeam);
        const bool bLocal = (b.team == localTeam);
        if (aLocal != bLocal)
            return aLocal;
        if (a.team != b.team)
            return a.team < b.team;
        if (a.defeated != b.defeated)
            return !a.defeated;
        return a.points > b.points;
    });
    int teamCount = 0;
    {
        int prevTeam = -1;
        for (const Row &r : rows)
        {
            if (r.team != prevTeam)
            {
                ++teamCount;
                prevTeam = r.team;
            }
        }
    }

    const int viewW = m_camera->GetViewportWidth();
    const int rowH = 30, teamHeaderH = 26, headH = 84;
    // Columns: colour swatch + name | faction | colour | efficiency | kills | losses
    const int colName = 16, colFaction = 250, colColor = 410, colPoints = 520, colKills = 620, colLosses = 700;
    const int w = std::min(viewW - 32, 780);
    const int h = std::min(
        m_windowH - 40,
        headH + static_cast<int>(rows.size()) * rowH + teamCount * teamHeaderH + 34);
    const int x = (viewW - w) / 2, y = std::max(20, (m_windowH - h) / 3);
    m_renderer->FillRectScreen(x, y, w, h, col::Panel);
    m_renderer->DrawRectScreen(x, y, w, h, col::BorderStrong);
    m_renderer->FillRectScreen(x, y, w, 2, col::Accent);
    m_fontRenderer->DrawString(_("MATCH STATISTICS"), x + colName, y + 12, ui::FONT_H2, col::Accent, false);
    // The map and how long the match has gone on
    {
        std::string about;
        if (m_mission && !m_mission->GetMapTitle().empty())
            about = std::string(_("Map")) + ": " + m_mission->GetMapTitle() + "    ";
        const std::uint32_t seconds
            = m_gameplayController
                  ? static_cast<std::uint32_t>(m_gameplayController->simulation().Tick() / SIMULATION_TICK_RATE)
                  : 0u;
        char clock[32];
        if (seconds >= 3600)
            std::snprintf(clock, sizeof clock, "%u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
        else
            std::snprintf(clock, sizeof clock, "%u:%02u", seconds / 60, seconds % 60);
        about += std::string(_("Time")) + ": " + clock;
        m_fontRenderer->DrawString(
            ui::FitText(*m_fontRenderer, about, ui::FONT_BODY, w - 2 * colName),
            x + colName,
            y + 40,
            ui::FONT_BODY,
            col::TextDim,
            false);
    }
    const int hy = y + 64;
    m_fontRenderer->DrawString(_("Player"), x + colName + 22, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    m_fontRenderer->DrawString(_("Faction"), x + colFaction, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    m_fontRenderer->DrawString(_("Color"), x + colColor, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    m_fontRenderer->DrawString(_("Efficiency"), x + colPoints, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    m_fontRenderer->DrawString(_("Kills"), x + colKills, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    m_fontRenderer->DrawString(_("Losses"), x + colLosses, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    int ry = y + headH;
    int currentTeam = -1;
    for (const Row &r : rows)
    {
        if (r.team != currentTeam)
        {
            if (ry + teamHeaderH > y + h - 24)
                break;
            currentTeam = r.team;
            int teamPoints = 0;
            for (const Row &tr : rows)
            {
                if (tr.team == currentTeam)
                    teamPoints += tr.points;
            }
            const bool isAllyTeam = (currentTeam == localTeam);
            const SDL_Color teamColor = isAllyTeam ? col::Ok : col::Danger;
            m_renderer->FillRectScreen(x + 8, ry + 2, w - 16, teamHeaderH - 4, ui::WithAlpha(teamColor, 28));
            m_renderer->FillRectScreen(x + 8, ry + 2, 3, teamHeaderH - 4, teamColor);
            m_renderer->DrawRectScreen(x + 8, ry + 2, w - 16, teamHeaderH - 4, ui::WithAlpha(teamColor, 110));
            std::string teamLabel = std::string(_("Team")) + " " + std::to_string(currentTeam + 1);
            if (isAllyTeam)
                teamLabel += std::string(" (") + _("Allies") + ")";
            const int tcy = ry + teamHeaderH / 2;
            m_fontRenderer->DrawString(
                teamLabel, x + colName + 6, tcy, ui::FONT_SMALL_BOLD, teamColor, false, Anchor::LEFT | Anchor::VCENTER);
            m_fontRenderer->DrawString(
                std::to_string(teamPoints),
                x + colPoints,
                tcy,
                ui::FONT_SMALL_BOLD,
                teamColor,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            ry += teamHeaderH;
        }
        if (ry + rowH > y + h - 24)
            break;
        const bool me = r.id == Players::Local();
        if (me)
            m_renderer->FillRectScreen(x + 8, ry, w - 16, rowH - 2, ui::WithAlpha(col::Accent, 32));
        const int cy = ry + rowH / 2 - 1;
        const SDL_Color text = (r.gone || r.defeated) ? col::TextMuted : col::Text;
        m_renderer->FillRectScreen(x + colName, cy - 7, 14, 14, r.defeated ? col::TextMuted : r.color);
        m_renderer->DrawRectScreen(x + colName, cy - 7, 14, 14, col::Border);
        std::string name = r.name;
        if (r.gone)
            name += std::string(" (") + _("left") + ")";
        if (r.defeated)
            name += std::string(" (") + _("Defeated") + ")";
        m_fontRenderer->DrawString(
            ui::FitText(*m_fontRenderer, name, me ? ui::FONT_BODY_BOLD : ui::FONT_BODY, colFaction - colName - 30),
            x + colName + 22,
            cy,
            me ? ui::FONT_BODY_BOLD : ui::FONT_BODY,
            text,
            false,
            Anchor::LEFT | Anchor::VCENTER);
        m_fontRenderer->DrawString(
            r.faction, x + colFaction, cy, ui::FONT_BODY, text, false, Anchor::LEFT | Anchor::VCENTER);
        m_fontRenderer->DrawString(
            r.colorName, x + colColor, cy, ui::FONT_BODY, text, false, Anchor::LEFT | Anchor::VCENTER);
        m_fontRenderer->DrawString(
            std::to_string(r.points),
            x + colPoints,
            cy,
            ui::FONT_BODY_BOLD,
            (r.gone || r.defeated) ? col::TextMuted : col::Accent,
            false,
            Anchor::LEFT | Anchor::VCENTER);
        m_fontRenderer->DrawString(
            std::to_string(r.kills), x + colKills, cy, ui::FONT_BODY, text, false, Anchor::LEFT | Anchor::VCENTER);
        m_fontRenderer->DrawString(
            std::to_string(r.losses), x + colLosses, cy, ui::FONT_BODY, text, false, Anchor::LEFT | Anchor::VCENTER);
        ry += rowH;
    }
    m_fontRenderer->DrawString(
        _("Efficiency: a point for every 10 credits of enemy forces destroyed"),
        x + colName,
        y + h - 22,
        ui::FONT_SMALL,
        col::TextMuted,
        false);
}

void Application::RenderFpsCounter()
{
    if (!m_settings.showFps)
        return;
    m_fontRenderer->DrawString(
        "FPS " + std::to_string(static_cast<int>(m_fps + 0.5f)), 1, 1, ui::FONT_SMALL_BOLD, ui::color::Accent, true);
}

void Application::RenderNetStats()
{
    if (!m_settings.showNetStats || !m_net)
        return;
    const std::uint32_t tick = m_netMatchLoaded ? m_gameplayController->simulation().Tick() : 0;
    const NetMatch::Stats st = m_net->stats(static_cast<std::uint32_t>(SDL_GetTicks()), tick);
    if (!st.valid)
        return;
    namespace col = ui::color;
    auto pingColor = [](std::uint32_t ms) {
        return ms < 80 ? col::Ok : (ms < 160 ? col::Accent : col::Danger);
    };
    struct Line
    {
        std::string text;
        SDL_Color color;
    };
    std::vector<Line> lines;
    char buf[160];
    lines.push_back({_("NETWORK"), col::Accent});
    if (st.host)
    {
        lines.push_back({_("Hosting this game"), col::TextDim});
    }
    else
    {
        std::snprintf(
            buf, sizeof buf, "%s %u %s   %s %u %s", _("Ping"), st.rttMs, _("ms"), _("Jitter"), st.jitterMs, _("ms"));
        lines.push_back({buf, pingColor(st.rttMs + st.jitterMs)});
        std::snprintf(buf, sizeof buf, "%s %.1f%%", _("Packet loss"), st.lossPercent);
        lines.push_back({buf, st.lossPercent < 1.0f ? col::Text : (st.lossPercent < 5.0f ? col::Accent : col::Danger)});
        // (a host's own client talks to its server through memory: no traffic of its own to show)
        std::snprintf(
            buf, sizeof buf, "%s  %.1f / %.1f %s", _("Traffic out / in"), st.sendKBps, st.recvKBps, _("KB/s"));
        lines.push_back({buf, col::Text});
    }
    if (m_netMatchLoaded)
    {
        std::snprintf(
            buf,
            sizeof buf,
            "%s %d %s  (%s %d %s)",
            _("Input delay"),
            st.inputDelayMs,
            _("ms"),
            _("turn"),
            st.turnMs,
            _("ms"));
        lines.push_back({buf, col::Text});
        std::snprintf(buf, sizeof buf, "%s %d", _("Turns buffered"), st.turnsBuffered);
        lines.push_back({buf, st.turnsBuffered > 2 ? col::Accent : col::Text});
        if (st.authorityMode)
        {
            if (st.snapshotBytes >= 1024)
            {
                std::snprintf(
                    buf,
                    sizeof buf,
                    "%s %.1f %s  %.1f/%s  %s %u %s",
                    _("Snapshot"),
                    static_cast<float>(st.snapshotBytes) / 1024.0f,
                    _("KB"),
                    st.snapshotRate,
                    _("s"),
                    _("age"),
                    st.snapshotAgeMs,
                    _("ms"));
            }
            else
            {
                std::snprintf(
                    buf,
                    sizeof buf,
                    "%s %zu %s  %.1f/%s  %s %u %s",
                    _("Snapshot"),
                    st.snapshotBytes,
                    _("B"),
                    st.snapshotRate,
                    _("s"),
                    _("age"),
                    st.snapshotAgeMs,
                    _("ms"));
            }
            lines.push_back({buf, st.snapshotAgeMs > 400 ? col::Accent : col::Text});
        }
        if (st.stalledSeconds > 0.3f)
        {
            std::snprintf(buf, sizeof buf, "%s %.1f %s", _("Waiting for the next turn"), st.stalledSeconds, _("s"));
            lines.push_back({buf, col::Danger});
        }
    }
    for (const NetMatch::Stats::Peer &p : st.peers)
    {
        if (p.rttMs == 0 && p.connected)
            continue; // the host's own client (memory link)
        std::snprintf(
            buf,
            sizeof buf,
            "%d. %s  %s",
            p.slot + 1,
            p.name.c_str(),
            p.connected ? (std::to_string(p.rttMs) + " " + _("ms")).c_str() : _("disconnected"));
        lines.push_back({buf, p.connected ? pingColor(p.rttMs) : col::Danger});
    }
    if (st.reconnecting)
        lines.push_back({_("Reconnecting..."), col::Danger});
    if (st.desynced)
        lines.push_back({_("DESYNC"), col::Danger});

    int w = 0;
    for (const Line &l : lines)
        w = std::max(w, m_fontRenderer->GetTextWidth(l.text, ui::FONT_SMALL));
    const int lineH = 16, x = 8, y = m_settings.showFps ? 28 : 8;
    const int h = static_cast<int>(lines.size()) * lineH + 8;
    m_renderer->FillRectScreen(x, y, w + 16, h, {8, 10, 14, 200});
    m_renderer->DrawRectScreen(x, y, w + 16, h, col::Border);
    for (std::size_t i = 0; i < lines.size(); ++i)
        m_fontRenderer->DrawString(
            lines[i].text,
            x + 8,
            y + 4 + static_cast<int>(i) * lineH,
            i == 0 ? ui::FONT_SMALL_BOLD : ui::FONT_SMALL,
            lines[i].color,
            false);
}

} // namespace OpenArtOfWar

namespace OpenArtOfWar
{

namespace
{
void PopUtf8Char(std::string &s)
{
    while (!s.empty())
    {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80)
            break;
    }
}
} // namespace

void Application::UpdateGameChat(std::uint32_t nowMs)
{
    const bool inMatch = m_net && !m_replay && m_mission && m_mission->IsOnlineMatch()
                         && (m_state == AppState::SkirmishPlaying || m_state == AppState::CampaignPlaying
                             || m_state == AppState::Paused);
    if (!inMatch)
    {
        if (m_gameChat.open)
        {
            m_gameChat.open = false;
            m_gameChat.input.clear();
            if (m_menuController)
                m_menuController->SetChatOpen(false);
            if (m_textInput)
            {
                SDL_StopTextInput(m_window);
                m_textInput = false;
            }
        }
        m_gameChat.lines.clear();
        return;
    }
    // Received messages
    for (Net::Chat &c : m_net->TakeChat())
    {
        std::string who = "?";
        if (const Net::Roster *roster = m_net->roster())
        {
            for (const Net::RosterEntry &e : roster->players)
                if (e.slot == c.fromSlot)
                    who = e.name;
        }
        m_gameChat.lines.push_back({who + ": " + c.text, nowMs + 15000});
        if (m_gameChat.lines.size() > 8)
            m_gameChat.lines.erase(m_gameChat.lines.begin());
        SoundRequests::Play("ui_click");
    }
    m_gameChat.lines.erase(
        std::remove_if(
            m_gameChat.lines.begin(), m_gameChat.lines.end(), [&](const Notice &n) { return nowMs >= n.untilMs; }),
        m_gameChat.lines.end());

    const bool enter
        = m_input->IsKeyJustPressed(SDL_SCANCODE_RETURN) || m_input->IsKeyJustPressed(SDL_SCANCODE_KP_ENTER);
    auto close = [&]() {
        m_gameChat.open = false;
        m_gameChat.input.clear();
        m_menuController->SetChatOpen(false);
        SDL_StopTextInput(m_window);
        m_textInput = false;
    };
    if (!m_gameChat.open)
    {
        // (not over a pause menu, a dialog or the in-game settings: Enter belongs to them there)
        if (enter && m_state != AppState::Paused && !m_menuController->IsAnyMenuOpen()
            && !m_hudController->IsResearchWindowOpen())
        {
            m_gameChat.open = true;
            m_gameChat.input.clear();
            m_menuController->SetChatOpen(true);
            m_input->TakeTypedText();
            m_input->TakeBackspaces();
            SDL_StartTextInput(m_window);
            m_textInput = true;
        }
        return;
    }
    if (m_input->IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        close();
        return;
    }
    for (int i = m_input->TakeBackspaces(); i > 0; --i)
        PopUtf8Char(m_gameChat.input);
    m_gameChat.input += m_input->TakeTypedText();
    while (m_gameChat.input.size() > Net::kMaxChatLength)
        PopUtf8Char(m_gameChat.input);
    if (enter)
    {
        if (!m_gameChat.input.empty())
            m_net->SendChat(m_gameChat.input);
        close();
    }
}

void Application::RenderGameChat()
{
    if (m_gameChat.lines.empty() && !m_gameChat.open)
        return;
    const std::uint32_t nowMs = SDL_GetTicks();
    const int viewW = m_camera->GetViewportWidth();
    const int boxW = std::min(560, viewW - 24);
    int y = m_windowH - 62 - static_cast<int>(m_gameChat.lines.size()) * 22;
    for (const Notice &n : m_gameChat.lines)
    {
        const std::uint32_t left = n.untilMs > nowMs ? n.untilMs - nowMs : 0;
        const float alpha = m_gameChat.open ? 1.0f : std::min(1.0f, left / 1000.0f);
        const auto fade = [&](SDL_Color c) {
            return ui::WithAlpha(c, static_cast<std::uint8_t>(c.a * alpha));
        };
        m_renderer->FillRectScreen(12, y, boxW, 20, fade(ui::WithAlpha(ui::color::Panel, 200)));
        m_fontRenderer->DrawString(
            ui::FitText(*m_fontRenderer, n.text, ui::FONT_SMALL_BOLD, boxW - 12),
            18,
            y + 10,
            ui::FONT_SMALL_BOLD,
            fade(ui::color::Text),
            true,
            Anchor::LEFT | Anchor::VCENTER);
        y += 22;
    }
    if (m_gameChat.open)
    {
        const ui::UiRect field {12, m_windowH - 50, boxW, 32};
        ui::DrawPanel(*m_renderer, field, ui::color::PanelSunken, ui::color::Accent);
        std::string shown = std::string(_("Say")) + ": " + m_gameChat.input;
        if (((nowMs / 500) & 1u) == 0)
            shown += "|";
        m_fontRenderer->DrawString(
            ui::FitText(*m_fontRenderer, shown, ui::FONT_BODY, field.w - 16),
            field.x + 8,
            field.CenterY(),
            ui::FONT_BODY,
            ui::color::Text,
            false,
            Anchor::LEFT | Anchor::VCENTER);
    }
}

} // namespace OpenArtOfWar
