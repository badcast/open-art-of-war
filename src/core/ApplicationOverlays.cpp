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

// The edge of the map melts into the dark background instead of ending in a hard line
void Application::RenderMapEdgeFade(const Camera &camera)
{
    if (!m_map)
        return;
    float x0, y0, x1, y1;
    camera.WorldToScreen(0.0f, 0.0f, x0, y0);
    camera.WorldToScreen(
        static_cast<float>(m_map->GetWidth() * TILE_WIDTH),
        static_cast<float>(m_map->GetHeight() * TILE_HEIGHT),
        x1,
        y1);
    const int viewW = camera.GetViewportWidth(), viewH = camera.GetViewportHeight();
    const int depth = std::max(4, static_cast<int>(1.6f * TILE_HEIGHT * camera.GetZoom()));
    const int left = static_cast<int>(std::floor(x0)), top = static_cast<int>(std::floor(y0));
    const int right = static_cast<int>(std::ceil(x1)), bottom = static_cast<int>(std::ceil(y1));
    auto shade = [&](int i) {
        const float t = 1.0f - static_cast<float>(i) / depth;
        return SDL_Color {0, 0, 0, static_cast<Uint8>(215.0f * t * t)};
    };
    const int yFrom = std::max(0, top), yTo = std::min(viewH - 1, bottom - 1);
    const int xFrom = std::max(0, left), xTo = std::min(viewW - 1, right - 1);
    for (int i = 0; i < depth; ++i)
    {
        const SDL_Color c = shade(i);
        const int lx = left + i, rx = right - 1 - i, ty = top + i, by = bottom - 1 - i;
        if (lx >= 0 && lx < viewW)
            m_renderer->DrawLineScreen(lx, yFrom, lx, yTo, c);
        if (rx >= 0 && rx < viewW)
            m_renderer->DrawLineScreen(rx, yFrom, rx, yTo, c);
        if (ty >= 0 && ty < viewH)
            m_renderer->DrawLineScreen(xFrom, ty, xTo, ty, c);
        if (by >= 0 && by < viewH)
            m_renderer->DrawLineScreen(xFrom, by, xTo, by, c);
    }
}

void Application::DrawLoadingFrame(
    const std::string &title, const std::string &status, float progress, SDL_Texture *preview, int pw, int ph)
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
    SDL_GetRenderOutputSize(m_sdlRenderer, &w, &h);
    SDL_SetRenderTarget(m_sdlRenderer, nullptr);
    SDL_SetRenderClipRect(m_sdlRenderer, nullptr);
    SDL_SetRenderDrawColor(m_sdlRenderer, 8, 10, 14, 255);
    SDL_RenderClear(m_sdlRenderer);
    ui::DrawLoadingScreen(*m_renderer, *m_fontRenderer, w, h, title, status, progress, SDL_GetTicks(), preview, pw, ph);
    SDL_RenderPresent(m_sdlRenderer);
}

void Application::ShowLoading(const std::string &status, float progress)
{
    if (m_headless || !m_sdlRenderer || !m_renderer || !m_fontRenderer)
        return;
    DrawLoadingFrame("OPEN ART OF WAR", status, progress, nullptr, 0, 0);
}

void Application::BeginMapLoading(const std::string &title, const std::string &mapPath)
{
    if (m_headless || !m_sdlRenderer || !m_renderer || !m_fontRenderer)
        return;
    m_mapLoading = true;
    m_mapLoadStartMs = SDL_GetTicks();
    m_mapLoadTitle = title;
    m_mapLoadTexture
        = m_loadingPreview.Get(m_sdlRenderer, *m_texManager, "loading:" + mapPath, mapPath, {}, m_mapLoadW, m_mapLoadH);
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
            m_net->Update(static_cast<uint32_t>(SDL_GetTicks()));
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
    const uint32_t tick = m_gameplayController->simulation().Tick();
    std::string text;
    SDL_Color color = ui::color::Accent;
    if (m_net->Desynced())
    {
        text = std::string(_("OUT OF SYNC at tick ")) + std::to_string(m_net->DesyncTick())
               + _(": the players' games differ, the result is not reliable");
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
        const uint32_t total = std::max<uint32_t>(1, m_net->TicksReceived());
        text = std::string(_("Catching up with the match: "))
               + std::to_string(std::min<uint32_t>(100, tick * 100 / total)) + "%";
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

void Application::TrackNetPlayers(uint32_t nowMs)
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
    for (size_t slot = 0; slot < m_netPlayers.size(); ++slot)
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

void Application::PushNotice(const std::string &text, uint32_t nowMs)
{
    constexpr uint32_t kShowMs = 7000;
    constexpr size_t kMaxNotices = 5;
    m_notices.push_back({text, nowMs + kShowMs});
    if (m_notices.size() > kMaxNotices)
        m_notices.erase(m_notices.begin(), m_notices.end() - kMaxNotices);
}

void Application::RenderNotices()
{
    const uint32_t nowMs = SDL_GetTicks();
    m_notices.erase(
        std::remove_if(m_notices.begin(), m_notices.end(), [&](const Notice &n) { return nowMs >= n.untilMs; }),
        m_notices.end());
    if (m_notices.empty()
        || !(
            m_state == AppState::SkirmishPlaying || m_state == AppState::CampaignPlaying || m_state == AppState::Paused
            || m_state == AppState::Victory || m_state == AppState::Defeat))
        return;
    const int viewW = m_camera->GetViewportWidth();
    int y = 86; // under the network banner
    for (const Notice &n : m_notices)
    {
        // The last second fades out
        const uint32_t left = n.untilMs - nowMs;
        const float alpha = std::min(1.0f, left / 1000.0f);
        auto fade = [&](SDL_Color c) {
            return ui::WithAlpha(c, static_cast<uint8_t>(c.a * alpha));
        };
        const int w = m_fontRenderer->GetTextWidth(n.text, ui::FONT_BODY_BOLD) + 32;
        const int x = (viewW - w) / 2;
        m_renderer->FillRectScreen(x, y, w, 28, fade(ui::color::Panel));
        m_renderer->FillRectScreen(x, y, 3, 28, fade(ui::color::Danger));
        m_fontRenderer->DrawString(
            n.text,
            x + w / 2,
            y + 14,
            ui::FONT_BODY_BOLD,
            fade(ui::color::Text),
            false,
            Anchor::HCENTER | Anchor::VCENTER);
        y += 32;
    }
}

void Application::RenderScoreboard()
{
    if (!m_input || !m_input->isKeyPressed(SDL_SCANCODE_TAB) || !m_entities)
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
        std::string name;
        std::string faction;
        SDL_Color color;
        std::string colorName;
        int points;
        int kills;
        int losses;
        bool gone;
    };
    std::vector<Row> rows;
    for (PlayerId id = 0; id < kMaxPlayers; ++id)
    {
        if (!Players::IsActive(id))
            continue;
        const PlayerSlot &slot = Players::Slot(id);
        Row r;
        r.id = id;
        r.gone = false;
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
    // Best first; the same points: in slot order
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.points > b.points; });

    const int viewW = m_camera->GetViewportWidth();
    const int rowH = 30, headH = 64;
    // Columns: colour swatch + name | faction | colour | efficiency | kills | losses
    const int colName = 16, colFaction = 250, colColor = 410, colPoints = 520, colKills = 620, colLosses = 700;
    const int w = std::min(viewW - 32, 780);
    const int h = headH + static_cast<int>(rows.size()) * rowH + 34;
    const int x = (viewW - w) / 2, y = std::max(60, (m_windowH - h) / 3);
    m_renderer->FillRectScreen(x, y, w, h, col::Panel);
    m_renderer->DrawRectScreen(x, y, w, h, col::BorderStrong);
    m_renderer->FillRectScreen(x, y, w, 2, col::Accent);
    m_fontRenderer->DrawString(_("MATCH STATISTICS"), x + colName, y + 12, ui::FONT_H2, col::Accent, false);
    const int hy = y + 44;
    m_fontRenderer->DrawString(_("Player"), x + colName + 22, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    m_fontRenderer->DrawString(_("Faction"), x + colFaction, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    m_fontRenderer->DrawString(_("Color"), x + colColor, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    m_fontRenderer->DrawString(_("Efficiency"), x + colPoints, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    m_fontRenderer->DrawString(_("Kills"), x + colKills, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    m_fontRenderer->DrawString(_("Losses"), x + colLosses, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
    int ry = y + headH;
    for (const Row &r : rows)
    {
        const bool me = r.id == Players::Local();
        if (me)
            m_renderer->FillRectScreen(x + 4, ry, w - 8, rowH - 2, col::Hover);
        const int cy = ry + rowH / 2 - 1;
        const SDL_Color text = r.gone ? col::TextMuted : col::Text;
        m_renderer->FillRectScreen(x + colName, cy - 7, 14, 14, r.color);
        m_renderer->DrawRectScreen(x + colName, cy - 7, 14, 14, col::Border);
        std::string name = r.name;
        if (r.gone)
            name += std::string(" (") + _("left") + ")";
        m_fontRenderer->DrawString(
            name,
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
            r.gone ? col::TextMuted : col::Accent,
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
        ry + 10,
        ui::FONT_SMALL,
        col::TextMuted,
        false);
}

void Application::RenderFpsCounter()
{
    if (!m_settings.showFps)
        return;
    m_fontRenderer->DrawString(
        "FPS " + std::to_string(static_cast<int>(m_fps + 0.5f)), 10, 8, ui::FONT_SMALL_BOLD, ui::color::Accent, true);
}

void Application::RenderNetStats()
{
    if (!m_settings.showNetStats || !m_net)
        return;
    const uint32_t tick = m_netMatchLoaded ? m_gameplayController->simulation().Tick() : 0;
    const NetMatch::Stats st = m_net->stats(static_cast<uint32_t>(SDL_GetTicks()), tick);
    if (!st.valid)
        return;
    namespace col = ui::color;
    auto pingColor = [](uint32_t ms) {
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
    for (size_t i = 0; i < lines.size(); ++i)
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

void Application::UpdateGameChat(uint32_t nowMs)
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
            m_gameChat.lines.begin(),
            m_gameChat.lines.end(),
            [&](const Notice &n) { return nowMs >= n.untilMs; }),
        m_gameChat.lines.end());

    const bool enter = m_input->IsKeyJustPressed(SDL_SCANCODE_RETURN) || m_input->IsKeyJustPressed(SDL_SCANCODE_KP_ENTER);
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
        if (enter && m_state != AppState::Paused && !m_menuController->IsAnyMenuOpen())
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
    const uint32_t nowMs = SDL_GetTicks();
    const int viewW = m_camera->GetViewportWidth();
    const int boxW = std::min(560, viewW - 24);
    int y = m_windowH - 62 - static_cast<int>(m_gameChat.lines.size()) * 22;
    for (const Notice &n : m_gameChat.lines)
    {
        const uint32_t left = n.untilMs > nowMs ? n.untilMs - nowMs : 0;
        const float alpha = m_gameChat.open ? 1.0f : std::min(1.0f, left / 1000.0f);
        const auto fade = [&](SDL_Color c) { return ui::WithAlpha(c, static_cast<uint8_t>(c.a * alpha)); };
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
