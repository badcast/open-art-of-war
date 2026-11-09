#include "ui/MenuController.hpp"
#include "MenuControllerInternal.hpp"
#include "ui/MapFilter.hpp"
#include "ui/MatchOptions.hpp"
#include "audio/SoundRequests.hpp"
#include <nlohmann/json.hpp>
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "graphics/ShaderEffects.hpp"
#include "game/Sides.hpp"
#include "ui/UiKit.hpp"
#include "core/Localization.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/TextureManager.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "input/InputManager.hpp"
#include "audio/SoundEngine.hpp"
#include "game/MissionController.hpp"
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/Pathfinder.hpp"
#include "game/AIController.hpp"
#include "storage/SaveManager.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>
#include <sstream>
#include <filesystem>
#include <vector>

namespace OpenArtOfWar
{

namespace
{

// Starts openaow-mapeditor (next to the game, or in the working directory) with `mapPath`; false when it is missing
bool LaunchMapEditor(const std::string &mapPath)
{
#ifdef _WIN32
    const char *exeName = "openaow-mapeditor.exe";
#else
    const char *exeName = "openaow-mapeditor";
#endif
    std::vector<std::filesystem::path> candidates;
    if (const char *base = SDL_GetBasePath())
        candidates.push_back(std::filesystem::path(base) / exeName);
    candidates.push_back(std::filesystem::path(".") / exeName);
    candidates.push_back(std::filesystem::path("build") / exeName);
    std::error_code ec;
    for (const auto &exe : candidates)
    {
        if (!std::filesystem::is_regular_file(exe, ec))
            continue;
        const std::string exePath = std::filesystem::absolute(exe, ec).string();
        const char *args[] = {exePath.c_str(), mapPath.c_str(), nullptr};
        SDL_Process *process = SDL_CreateProcess(args, false);
        if (!process)
            continue;
        SDL_DestroyProcess(process); // the editor lives on its own
        return true;
    }
    return false;
}

// "-25%" ... "+100%" for a handicap index (the start credits form)
std::string HandicapText(int index)
{
    const int pct = HandicapPercentOf(index) - 100;
    return (pct > 0 ? "+" : "") + std::to_string(pct) + "%";
}

// Custom game: the table of players in the details panel (columns: level / name, faction, team, colour, remove)
// The seat table takes the whole width of the details (as in the network room); the map picture sits under it on the
// left, the options and the map size on its right
int CustomRowH(const UiRect &d)
{
    return std::clamp((d.h - 118 - 150 - 30) / 8, 30, 40);
}

UiRect CustomRow(const UiRect &d, int i)
{
    const int rowH = CustomRowH(d);
    return {d.x + 20, d.y + 118 + i * rowH, d.w - 40, rowH - 4};
}

UiRect CustomCell(const UiRect &d, int i, int kind)
{
    // (i is a start position of the map; kind 0 what holds it, 1 faction, 2 team, 3 colour, 7 handicap, 8 army size,
    // 9 tech level). The cells share the width of the row, so they stretch with the window
    static constexpr int kShare[7] = {23, 17, 8, 8, 12, 15, 17};
    const UiRect r = CustomRow(d, i);
    const int k = kind == 1 ? 1 : kind == 2 ? 2 : kind == 3 ? 3 : kind == 7 ? 4 : kind == 8 ? 5 : kind == 9 ? 6 : 0;
    const int gap = 4, x0 = r.x + 24, total = r.w - 24 - 6 * gap - 4;
    int used = 0;
    for (int j = 0; j < k; ++j)
        used += kShare[j];
    return {x0 + total * used / 100 + k * gap, r.y, total * kShare[k] / 100, r.h};
}

UiRect CustomPreviewArea(const UiRect &d)
{
    const int top = d.y + 118 + 8 * CustomRowH(d) + 12;
    const int size = std::clamp(std::min(d.Bottom() - 16 - top, static_cast<int>(d.w * 0.45f)), 120, 460);
    return {d.x + 20, top, size, size};
}

// Campaign mission select
struct CampaignLayout
{
    UiRect dialog;
    UiRect list;
    UiRect details;
    UiRect startBtn;
    UiRect backBtn;
    UiRect sideBtn[1];    // combo: the side the player commands (Resistance / Confederation)
    UiRect difficultyBtn; // combo: AI difficulty, right of the side combo
    UiRect editorBtn;     // opens the map editor with the selected map, left of Start
    UiRect baseCheckbox;  // custom game: start with a base (check box under the picture)
    UiRect fogCheckbox;   // custom game: fog of war checkbox
    UiRect speedCheckbox; // custom game: allow speed control checkbox
    UiRect creditsBtn;    // custom game: starting credits (top right of the details)
    UiRect timeBtn;       // custom game: time of day (left of the credits)
    UiRect techBtn;       // custom game: tech level of the match (left of the time of day)
    int itemH {40};
    int itemGap {4};

    UiRect Item(int i) const
    {
        return {list.x, list.y + i * (itemH + itemGap), list.w, itemH};
    }
};

CampaignLayout campaignLayout(int windowW, int windowH, int missionCount)
{
    CampaignLayout l;
    l.dialog = ui::CenteredRect(windowW, windowH, windowW, windowH); // the whole screen
    int contentY = l.dialog.y + 52 + 16;
    int footerY = l.dialog.Bottom() - 64;
    l.list = {
        l.dialog.x + 20, contentY, std::clamp(static_cast<int>(l.dialog.w * 0.27f), 300, 460), footerY - contentY - 8};
    int n = std::max(1, missionCount);
    l.itemH = std::clamp((l.list.h - (n - 1) * l.itemGap) / n, 26, 44);
    l.details = {l.list.Right() + 16, contentY, l.dialog.Right() - 20 - (l.list.Right() + 16), l.list.h};
    l.backBtn = {l.dialog.Right() - 20 - 140, footerY + 10, 140, 40};
    l.startBtn = {l.backBtn.x - 12 - 200, footerY + 10, 200, 40};
    l.sideBtn[0] = {l.dialog.x + 20, footerY + 10, 240, 40};
    l.editorBtn = {l.startBtn.x - 12 - 140, footerY + 10, 140, 40};
    l.creditsBtn = {l.details.Right() - 20 - 200, l.details.y + 12, 200, 32};
    l.timeBtn = {l.creditsBtn.x - 12 - 160, l.details.y + 12, 160, 32};
    l.techBtn = {l.timeBtn.x - 12 - 180, l.details.y + 12, 180, 32};
    const UiRect pv = CustomPreviewArea(l.details);
    const int optX = pv.Right() + 20;
    l.fogCheckbox = {optX, pv.y, l.details.Right() - 20 - optX, 28};
    l.baseCheckbox = {optX, pv.y + 32, l.details.Right() - 20 - optX, 28};
    l.speedCheckbox = {optX, pv.y + 64, l.details.Right() - 20 - optX, 28};
    l.difficultyBtn = {
        l.sideBtn[0].Right() + 12, footerY + 10, std::max(180, l.editorBtn.x - 12 - (l.sideBtn[0].Right() + 12)), 40};
    return l;
}

// The custom game list shows as many fixed-height rows as fit and scrolls
// A start marker stays inside its picture
void SpotMarkerPos(const UiRect &pv, int mapW, int mapH, int tileX, int tileY, int &sx, int &sy)
{
    const float sc = std::min((pv.w - 4) / static_cast<float>(mapW), (pv.h - 4) / static_cast<float>(mapH));
    const float ox = pv.CenterX() - mapW * sc / 2, oy = pv.CenterY() - mapH * sc / 2;
    sx = std::clamp(static_cast<int>(ox + (tileX + 0.5f) * sc), pv.x + 10, pv.Right() - 10);
    sy = std::clamp(static_cast<int>(oy + (tileY + 0.5f) * sc), pv.y + 10, pv.Bottom() - 10);
}

constexpr int kMapFilterBarH = 72; // search box and the three filter buttons over the list of maps

UiRect MapSearchRect(const CampaignLayout &l)
{
    return {l.list.x, l.list.y - kMapFilterBarH, l.list.w, 30};
}

UiRect MapFilterBtn(const CampaignLayout &l, int i)
{
    const int w = (l.list.w - 2 * 6) / 3;
    return {l.list.x + i * (w + 6), l.list.y - kMapFilterBarH + 36, w, 28};
}

int CustomListRows(CampaignLayout &l)
{
    // (idempotent: the bar takes the top of the list for a custom game)
    l.list.y += kMapFilterBarH;
    l.list.h -= kMapFilterBarH;
    l.itemH = 30;
    return std::max(1, (l.list.h + l.itemGap) / (l.itemH + l.itemGap));
}

// The picture of a map in the bottom part of the details panel, scaled to fit and centered
void DrawMapPreviewIn(Renderer &renderer, SDL_Texture *tex, int texW, int texH, const UiRect &area)
{
    renderer.FillRectScreen(area.x, area.y, area.w, area.h, col::PanelSunken);
    renderer.DrawRectScreen(area.x, area.y, area.w, area.h, col::Border);
    if (!tex || texW <= 0 || texH <= 0)
        return;
    const float scale = std::min((area.w - 4) / static_cast<float>(texW), (area.h - 4) / static_cast<float>(texH));
    const int w = std::max(1, static_cast<int>(texW * scale)), h = std::max(1, static_cast<int>(texH * scale));
    SDL_FRect dst {
        static_cast<float>(area.CenterX() - w / 2),
        static_cast<float>(area.CenterY() - h / 2),
        static_cast<float>(w),
        static_cast<float>(h)};
    SDL_RenderTexture(renderer.GetSdlRenderer(), tex, nullptr, &dst);
}

void DrawMapPreview(Renderer &renderer, SDL_Texture *tex, int texW, int texH, const UiRect &panel)
{
    const int margin = 20;
    const int maxH = std::min(panel.h / 2 - 12, 220);
    DrawMapPreviewIn(
        renderer,
        tex,
        texW,
        texH,
        UiRect {panel.x + margin, panel.Bottom() - margin - maxH, panel.w - 2 * margin, maxH});
}

} // namespace

std::vector<int> MenuController::MapPlayerCounts() const
{
    return ui::MapFilter::PlayerCounts(m_skirmishAll);
}

void MenuController::RebuildMapList()
{
    const std::string keep = (m_skirmishSelected >= 0 && m_skirmishSelected < static_cast<int>(m_skirmishMaps.size()))
                                 ? m_skirmishMaps[m_skirmishSelected].path
                                 : std::string();
    ui::MapFilter filter;
    filter.search = m_mapSearch;
    filter.players = m_mapPlayersFilter;
    filter.size = m_mapSizeFilter;
    filter.sort = m_mapSort;
    m_skirmishMaps = filter.Apply(m_skirmishAll);
    m_skirmishSelected = 0;
    for (std::size_t i = 0; i < m_skirmishMaps.size(); ++i)
        if (m_skirmishMaps[i].path == keep)
            m_skirmishSelected = static_cast<int>(i);
    m_skirmishTop = 0;
}

bool MenuController::UpdateCampaignSelect(const FrameContext &ctx, int mx, int my, bool leftClicked)
{
    InputManager &input = ctx.input;
    const int windowW = ctx.windowW;
    const int windowH = ctx.windowH;
    MissionController &mission = ctx.mission;
    TileMap &map = ctx.map;
    EntityManager &entities = ctx.entities;
    Economy &economy = ctx.economy;
    Pathfinder &pathfinder = ctx.pathfinder;
    AppState &state = ctx.state;
    if (!m_campaignSelectOpen)
        return false;
    m_mouseX = mx;
    m_mouseY = my;
    if (m_settings)
        m_difficulty = static_cast<AIDifficulty>(m_settings->difficulty);

    auto cycleDifficulty = [&]() {
        if (!m_settings)
            return;
        m_settings->difficulty = (m_settings->difficulty + 1) % 4;
        m_difficulty = static_cast<AIDifficulty>(m_settings->difficulty);
        if (m_onSettingsChanged)
            m_onSettingsChanged(SettingsArea::Game);
    };

    const int mCount = m_customGame ? static_cast<int>(m_skirmishMaps.size())
                                    : std::min(
                                          MissionController::kCampaignMissionCount,
                                          static_cast<int>(MissionController::GetAvailableMissions().size()));
    if (mCount == 0 && !(m_customGame && !m_skirmishAll.empty()))
    {
        m_campaignSelectOpen = false;
        return true;
    }
    int &selected = m_customGame ? m_skirmishSelected : m_campaignMissionSelected;
    CampaignLayout l = campaignLayout(windowW, windowH, mCount);
    const int rows = m_customGame ? CustomListRows(l) : mCount;

    auto startSelected = [&]() {
        if (m_customGame && mCount == 0)
            return;
        if (m_customGame)
        {
            EnsureCustomSlots();
            bool foes = false;
            const std::size_t first = m_customSpectator ? 1 : 0;
            if (m_customSlots.size() <= first + (m_customSpectator ? 1u : 0u))
            {
                m_editorNotice = _("A watched match needs at least two computer players.");
                return;
            }
            for (std::size_t i = first; i < m_customSlots.size(); ++i)
                foes = foes || m_customSlots[i].team != m_customSlots[first].team;
            if (!foes)
            {
                m_editorNotice = _("Everybody is in one team: there is nobody to fight.");
                return;
            }
            m_editorNotice.clear();
        }
        // The loading screen with the picture of the map, kept up for at least three seconds
        if (m_beginLoading)
        {
            if (m_customGame)
            {
                m_beginLoading(
                    m_skirmishMaps[m_skirmishSelected].DisplayName(), m_skirmishMaps[m_skirmishSelected].path);
            }
            else
            {
                const auto &data = Missions::Campaign(PlayerFaction())[m_campaignMissionSelected];
                m_beginLoading(_(data.info.name), LoaderResources::MissionMapPath(data.data.mapFile));
            }
        }
        // A custom game is a match of this computer alone: every player at a start position of the map, the person at
        // the first one
        MatchSetup match;
        if (m_customGame)
        {
            std::random_device rd;
            match.mapPath = m_skirmishMaps[m_skirmishSelected].path;
            match.seed = rd();
            match.localOnly = true;
            match.prebuilt = m_skirmishPrebuilt;
            match.fogOfWar = m_skirmishFogOfWar;
            match.allowSpeed = m_skirmishAllowSpeed;
            match.startingCredits = kStartingCredits[std::clamp(m_skirmishCredits, 0, 4)];
            match.night = m_skirmishNight;
            match.techLevel = static_cast<std::int8_t>(m_skirmishTechLevel);
            // The person watches: he is no player, the computer players take the ids from 0 (a watcher's view belongs
            // to the first of them)
            match.local = 0;
            const int firstSlot = m_customSpectator ? 1 : 0;
            for (int i = firstSlot; i < static_cast<int>(m_customSlots.size()) && i - firstSlot < kMaxPlayers; ++i)
            {
                const int p = i - firstSlot;
                const CustomSlot &cs = m_customSlots[i];
                PlayerSlot &slot = match.slots[p];
                slot.active = true;
                slot.human = i == 0;
                if (i == 0)
                {
                    if (m_sideRandom)
                        SetPlayerFaction((rd() & 1u) ? Faction::Confederation : Faction::Resistance); // drawn now
                    slot.faction = PlayerFaction();
                }
                else if (cs.faction == 0)
                    slot.faction = (rd() & 1u) ? Faction::Confederation : Faction::Resistance;
                else
                    slot.faction = cs.faction == 2 ? Faction::Confederation : Faction::Resistance;
                slot.team = static_cast<std::uint8_t>(cs.team);
                slot.color = static_cast<std::uint8_t>(cs.color);
                slot.handicap = static_cast<std::uint8_t>(std::clamp(cs.handicap, 0, kHandicapCount - 1));
                slot.armyCap = static_cast<std::uint8_t>(std::clamp(cs.armyCap, 0, kArmyCapCount - 1));
                slot.techLevel = static_cast<std::int8_t>(cs.techLevel);
                slot.aiLevel = static_cast<std::uint8_t>(std::clamp(cs.level, 0, 3));
                match.startPosition[p] = static_cast<std::int8_t>(cs.position);
            }
        }
        const bool started = m_customGame
                                 ? mission.LoadMatch(match, map, entities, economy, pathfinder)
                                 : mission.LoadMission(m_campaignMissionSelected, map, entities, economy, pathfinder);
        if (m_endLoading)
            m_endLoading();
        if (!started)
            return;
        m_spectatorMatch = m_customGame && m_customSpectator;
        state = AppState::Briefing;
        m_campaignSelectOpen = false;
    };

    // Custom game: search box and filter buttons over the list of maps
    if (m_customGame)
    {
        if (leftClicked)
        {
            m_mapSearchFocus = MapSearchRect(l).Contains(mx, my);
            if (m_mapSearchFocus)
                return true;
        }
        if (m_mapSearchFocus)
        {
            const std::string before = m_mapSearch;
            for (int n = input.TakeBackspaces(); n > 0; --n)
                PopLastUtf8(m_mapSearch);
            m_mapSearch += input.TakeTypedText();
            while (m_mapSearch.size() > 40)
                PopLastUtf8(m_mapSearch);
            if (m_mapSearch != before)
                RebuildMapList();
            if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE) || input.IsKeyJustPressed(SDL_SCANCODE_RETURN)
                || input.IsKeyJustPressed(SDL_SCANCODE_KP_ENTER))
            {
                m_mapSearchFocus = false;
                return true;
            }
        }
    }
    if (mCount > 0 && input.IsKeyJustPressed(SDL_SCANCODE_UP))
    {
        selected = (selected - 1 + mCount) % mCount;
    }
    if (mCount > 0 && input.IsKeyJustPressed(SDL_SCANCODE_DOWN))
    {
        selected = (selected + 1) % mCount;
    }
    if (m_customGame && mCount > 0)
    {
        if (input.IsKeyJustPressed(SDL_SCANCODE_PAGEUP))
            selected = std::max(0, selected - rows);
        if (input.IsKeyJustPressed(SDL_SCANCODE_PAGEDOWN))
            selected = std::min(mCount - 1, selected + rows);
        // The wheel scrolls the list, the selection stays where it is
        m_skirmishTop = std::clamp(m_skirmishTop - input.ConsumeWheel(), 0, std::max(0, mCount - rows));
        if (input.IsKeyJustPressed(SDL_SCANCODE_UP) || input.IsKeyJustPressed(SDL_SCANCODE_DOWN)
            || input.IsKeyJustPressed(SDL_SCANCODE_PAGEUP) || input.IsKeyJustPressed(SDL_SCANCODE_PAGEDOWN))
        {
            if (selected < m_skirmishTop)
                m_skirmishTop = selected;
            if (selected >= m_skirmishTop + rows)
                m_skirmishTop = selected - rows + 1;
        }
    }
    if (m_customGame && mCount > rows)
    {
        const UiRect bar {l.list.Right() + 4, l.list.y, 4, l.list.h};
        int px = 0, py = 0;
        if (input.ConsumeLeftPress(px, py) && input.IsLeftMouseDown() && OnScrollBar(bar, px, py))
            m_scrollDrag = 1;
        if (m_scrollDrag == 1)
        {
            if (input.IsLeftMouseDown())
                m_skirmishTop = ScrollTopFromMouse(bar, my, mCount, rows);
            else
                m_scrollDrag = 0;
            return true;
        }
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        if (m_combo.open >= kComboSide)
        { // closes the open list first
            m_combo.open = -1;
            return true;
        }
        m_campaignSelectOpen = false;
        return true;
    }
    if (!m_mapSearchFocus && (input.IsKeyJustPressed(SDL_SCANCODE_LEFT) || input.IsKeyJustPressed(SDL_SCANCODE_RIGHT)))
    {
        SetPlayerFaction(OpposingFaction(PlayerFaction()));
    }

    if (input.IsKeyJustPressed(SDL_SCANCODE_TAB))
        cycleDifficulty();
    const bool hasLayoutChoice
        = m_customGame && m_skirmishSelected >= 0 && m_skirmishSelected < static_cast<int>(m_skirmishMaps.size());
    // The drop-down lists of the footer (side, difficulty), of the map (prebuilt bases / from scratch) and of the
    // players
    if (hasLayoutChoice)
        EnsureCustomSlots();
    auto comboField = [&](int id) {
        if (id >= kComboSlot)
            return CustomCell(l.details, (id - kComboSlot) / 10, (id - kComboSlot) % 10);
        if (id >= kComboMapPlayers && id <= kComboMapSort)
            return MapFilterBtn(l, id - kComboMapPlayers);
        return id == kComboSide      ? l.sideBtn[0]
               : id == kComboCredits ? l.creditsBtn
               : id == kComboTime    ? l.timeBtn
               : id == kComboTech    ? l.techBtn
                                     : l.difficultyBtn;
    };
    if (m_combo.open >= 0 && m_combo.open < kComboSide)
        m_combo.open = -1; // one of the settings screen's
    if ((m_combo.open == kComboCredits || m_combo.open == kComboTime || m_combo.open == kComboTech
         || m_combo.open >= kComboSlot)
        && !hasLayoutChoice)
        m_combo.open = -1;
    if (m_combo.open >= kComboSlot && hasLayoutChoice
        && (m_combo.open - kComboSlot) / 10 >= std::min(m_skirmishMaps[m_skirmishSelected].players, kMaxPlayers))
        m_combo.open = -1;
    if (m_combo.open >= 0)
    {
        const int id = m_combo.open;
        int picked = -1;
        if (ui::ComboUpdate(
                m_combo,
                comboField(id),
                static_cast<int>(CampaignComboOptions(id).size()),
                windowH,
                mx,
                my,
                leftClicked,
                input.ConsumeWheel(),
                picked))
        {
            if (picked >= 0)
                CampaignComboApply(id, picked);
            return true;
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            m_combo.open = -1;
            return true;
        }
    }
    if (leftClicked)
    {
        auto openCombo = [&](int id) {
            m_combo.open = id;
            m_combo.scroll = 0;
            return true;
        };
        if (m_customGame)
            for (int k = kComboMapPlayers; k <= kComboMapSort; ++k)
                if (MapFilterBtn(l, k - kComboMapPlayers).Contains(mx, my))
                    return openCombo(k);
        if (l.sideBtn[0].Contains(mx, my))
            return openCombo(kComboSide);
        if (l.difficultyBtn.Contains(mx, my))
            return openCombo(kComboDifficulty);
        if (hasLayoutChoice && l.baseCheckbox.Contains(mx, my))
        {
            m_skirmishPrebuilt = !m_skirmishPrebuilt;
            SoundRequests::Play("ui_confirm");
            return true;
        }
        if (hasLayoutChoice && l.creditsBtn.Contains(mx, my))
            return openCombo(kComboCredits);
        if (hasLayoutChoice && l.timeBtn.Contains(mx, my))
            return openCombo(kComboTime);
        if (hasLayoutChoice && l.techBtn.Contains(mx, my))
            return openCombo(kComboTech);
        if (hasLayoutChoice && l.fogCheckbox.Contains(mx, my))
        {
            m_skirmishFogOfWar = !m_skirmishFogOfWar;
            SoundRequests::Play("ui_confirm");
            return true;
        }
        if (hasLayoutChoice && l.speedCheckbox.Contains(mx, my))
        {
            m_skirmishAllowSpeed = !m_skirmishAllowSpeed;
            SoundRequests::Play("ui_confirm");
            return true;
        }
        if (hasLayoutChoice)
        {
            const UiRect &d = l.details;
            const SkirmishMapInfo &mi = m_skirmishMaps[m_skirmishSelected];
            const int places = std::min(mi.players, kMaxPlayers);
            for (int i = 0; i < places; ++i)
            {
                if (CustomCell(d, i, 0).Contains(mx, my))
                    return openCombo(kComboSlot + i * 10);
                const int si = CustomSlotAt(i);
                if (si < 0)
                    continue;
                for (int kind : {1, 2, 3, 7, 8, 9})
                    if (CustomCell(d, i, kind).Contains(mx, my))
                        return openCombo(kComboSlot + i * 10 + kind);
            }
            // An open start position on the map: the person takes it
            {
                const UiRect pv = CustomPreviewArea(d);
                if (m_spotsPath == mi.path && mi.width > 0 && mi.height > 0 && pv.Contains(mx, my))
                {
                    for (int n = 0; n < static_cast<int>(m_spots.size()) && n < places; ++n)
                    {
                        int sx = 0, sy = 0;
                        SpotMarkerPos(pv, mi.width, mi.height, m_spots[n].first, m_spots[n].second, sx, sy);
                        if (std::abs(mx - sx) > 10 || std::abs(my - sy) > 10)
                            continue;
                        if (CustomSlotAt(n) < 0 && !((m_customBlocked >> n) & 1u))
                        {
                            SetCustomSeat(n, 1);
                            SoundRequests::Play("ui_confirm");
                        }
                        return true;
                    }
                }
            }
        }
        // Side the player commands in this mission
        if (l.editorBtn.Contains(mx, my) && (!m_customGame || mCount > 0))
        {
            const std::string mapPath
                = m_customGame ? m_skirmishMaps[m_skirmishSelected].path
                               : LoaderResources::MissionMapPath(
                                     Missions::Campaign(PlayerFaction())[m_campaignMissionSelected].data.mapFile);
            m_editorNotice
                = LaunchMapEditor(mapPath) ? "" : _("Map editor (openaow-mapeditor) not found next to the game");
            return true;
        }
        const int first = m_customGame ? m_skirmishTop : 0;
        for (int row = 0; row < rows && first + row < mCount; ++row)
        {
            if (l.Item(row).Contains(mx, my))
            {
                // First click selects, clicking the selected mission or map starts it
                if (selected == first + row)
                {
                    startSelected();
                }
                else
                {
                    selected = first + row;
                }
                return true;
            }
        }
        if (l.startBtn.Contains(mx, my))
        {
            startSelected();
            return true;
        }
        if (l.backBtn.Contains(mx, my))
        {
            m_campaignSelectOpen = false;
            return true;
        }
    }

    if (input.IsConfirmJustPressed())
    {
        startSelected();
    }
    return true;
}

void MenuController::RenderCampaignMissionSelect(
    Renderer &renderer, TextureManager &texManager, BitmapFontRenderer &fontRenderer, int windowW, int windowH)
{
    const auto &missions = MissionController::GetAvailableMissions();
    const int missionCount = std::min(MissionController::kCampaignMissionCount, static_cast<int>(missions.size()));
    const int count = m_customGame ? static_cast<int>(m_skirmishMaps.size()) : missionCount;
    CampaignLayout l = campaignLayout(windowW, windowH, count);
    const int rows = m_customGame ? CustomListRows(l) : count;
    const int first = m_customGame ? std::clamp(m_skirmishTop, 0, std::max(0, count - rows)) : 0;
    const int selected = m_customGame ? m_skirmishSelected : m_campaignMissionSelected;
    ui::DrawDialog(
        renderer,
        fontRenderer,
        windowW,
        windowH,
        l.dialog,
        m_customGame ? _("SELECT MAP") : _("SELECT CAMPAIGN MISSION"));
    if (m_customGame)
    {
        const UiRect sb = MapSearchRect(l);
        ui::DrawPanel(renderer, sb, col::PanelSunken, m_mapSearchFocus ? col::Accent : col::Border);
        const bool blink = (SDL_GetTicks() / 500) % 2 == 0;
        const std::string shown = m_mapSearch.empty() && !m_mapSearchFocus
                                      ? std::string(_("Search map..."))
                                      : m_mapSearch + (m_mapSearchFocus && blink ? "|" : "");
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, shown, ui::FONT_BODY, sb.w - 20),
            sb.x + 10,
            sb.CenterY(),
            ui::FONT_BODY,
            m_mapSearch.empty() && !m_mapSearchFocus ? col::TextMuted : col::Text,
            false,
            Anchor::LEFT | Anchor::VCENTER);
        for (int k = kComboMapPlayers; k <= kComboMapSort; ++k)
        {
            const UiRect b = MapFilterBtn(l, k - kComboMapPlayers);
            ui::DrawComboField(
                renderer,
                fontRenderer,
                b,
                CampaignComboOptions(k)[CampaignComboCurrent(k)],
                m_combo.open == k,
                b.Contains(m_mouseX, m_mouseY));
        }
        if (count == 0)
            fontRenderer.DrawString(
                _("No maps match."),
                l.list.CenterX(),
                l.list.y + 30,
                ui::FONT_BODY,
                col::TextMuted,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
    }

    for (int row = 0; row < rows && first + row < count; ++row)
    {
        const int i = first + row;
        UiRect r = l.Item(row);
        bool isSelected = (i == selected);
        bool hovered = r.Contains(m_mouseX, m_mouseY);
        if (hovered)
            SoundRequests::MarkHover();

        renderer.FillRectScreen(
            r.x, r.y, r.w, r.h, isSelected ? col::Hover : (hovered ? col::PanelRaised : col::PanelSunken));
        renderer.DrawRectScreen(r.x, r.y, r.w, r.h, isSelected ? col::BorderStrong : col::Border);
        if (isSelected)
            renderer.FillRectScreen(r.x, r.y, 3, r.h, col::Accent);

        std::string num = std::to_string(i + 1);
        fontRenderer.DrawString(
            num,
            r.x + 26,
            r.CenterY(),
            ui::FONT_BODY_BOLD,
            isSelected ? col::Accent : col::TextMuted,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
        std::string name;
        if (m_customGame)
        {
            name = m_skirmishMaps[i].DisplayName();
        }
        else
        {
            // Mission names are "Mission N: Title" - show just the title next to the number
            name = _(missions[i].name);
            std::size_t colon = name.find(": ");
            if (colon != std::string::npos)
                name = name.substr(colon + 2);
        }
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, name, ui::FONT_BODY, r.w - 56),
            r.x + 48,
            r.CenterY(),
            ui::FONT_BODY,
            isSelected ? col::Text : col::TextDim,
            false,
            Anchor::LEFT | Anchor::VCENTER);
    }

    if (m_customGame && count > rows)
    {
        // Scroll bar beside the list
        const int trackX = l.list.Right() + 4;
        renderer.FillRectScreen(trackX, l.list.y, 4, l.list.h, col::PanelSunken);
        const int thumbH = std::max(20, l.list.h * rows / count);
        const int thumbY = l.list.y + (l.list.h - thumbH) * first / std::max(1, count - rows);
        renderer.FillRectScreen(trackX, thumbY, 4, thumbH, col::BorderStrong);
    }

    if (m_customGame && selected >= 0 && selected < count)
    {
        const SkirmishMapInfo &info = m_skirmishMaps[selected];
        const UiRect &d = l.details;
        ui::DrawPanel(renderer, d, col::PanelSunken, col::Border);

        int x = d.x + 20;
        int w = d.w - 40;
        const int titleW = w - 220 - 172 - 192 - 118;
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, info.DisplayName(), ui::FONT_H2, titleW),
            x,
            d.y + 18,
            ui::FONT_H2,
            col::Accent,
            true);
        ui::DrawComboField(
            renderer,
            fontRenderer,
            l.creditsBtn,
            ui::CreditsLabel(m_skirmishCredits),
            m_combo.open == kComboCredits,
            l.creditsBtn.Contains(m_mouseX, m_mouseY));
        ui::DrawComboField(
            renderer,
            fontRenderer,
            l.timeBtn,
            ui::TimeOfDayChoices()[m_skirmishNight ? 1 : 0],
            m_combo.open == kComboTime,
            l.timeBtn.Contains(m_mouseX, m_mouseY));
        ui::DrawComboField(
            renderer,
            fontRenderer,
            l.techBtn,
            ui::TechLevelLabel(m_skirmishTechLevel, false),
            m_combo.open == kComboTech,
            l.techBtn.Contains(m_mouseX, m_mouseY));
        renderer.FillRectScreen(x, d.y + 74, w, 1, col::Border);
        ui::DrawCaption(
            fontRenderer,
            renderer,
            x,
            d.y + 80,
            std::string(_("PLAYERS")) + " (" + std::to_string(m_customSlots.size() - (m_customSpectator ? 1u : 0u))
                + ")");
        // Column captions, the same order as in the network room
        for (const auto &[kind, text] : std::initializer_list<std::pair<int, const char *>> {
                 {0, _("Place")},
                 {1, _("Faction")},
                 {2, _("Team")},
                 {3, _("Colour")},
                 {7, _("Handicap")},
                 {8, _("Army")},
                 {9, _("Tech")}})
        {
            const UiRect cell = CustomCell(d, 0, kind);
            fontRenderer.DrawString(
                ui::FitText(fontRenderer, text, ui::FONT_SMALL, cell.w),
                cell.x + 2,
                d.y + 100,
                ui::FONT_SMALL,
                col::TextMuted,
                false);
        }
        const int places = std::min(info.players, kMaxPlayers);
        for (int i = 0; i < places; ++i)
        {
            const int si = CustomSlotAt(i);
            const bool blocked = ((m_customBlocked >> i) & 1u) != 0;
            const UiRect row = CustomRow(d, i);
            ui::DrawPanel(
                renderer,
                row,
                si == 0 ? col::Hover : (blocked ? col::PanelSunken : col::PanelRaised),
                si == 0 ? col::Accent : col::Border);
            fontRenderer.DrawString(
                std::to_string(i + 1),
                row.x + 12,
                row.CenterY(),
                ui::FONT_BODY_BOLD,
                si >= 0 ? ArmyColor(m_customSlots[si].color) : col::TextMuted,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
            auto combo = [&](int kind, const std::string &text, SDL_Color c = col::Text) {
                const UiRect cell = CustomCell(d, i, kind);
                ui::DrawComboField(
                    renderer,
                    fontRenderer,
                    cell,
                    ui::FitText(fontRenderer, text, ui::FONT_SMALL, cell.w - 34),
                    m_combo.open == kComboSlot + i * 10 + kind,
                    cell.Contains(m_mouseX, m_mouseY),
                    true,
                    c);
            };
            if (si < 0)
            {
                combo(0, blocked ? _("Blocked") : _("Open"), blocked ? col::Danger : col::TextDim);
                continue;
            }
            const CustomSlot &cs = m_customSlots[si];
            if (si == 0)
                combo(0, _("YOU"), col::Text);
            else
                combo(
                    0,
                    std::string(_("AI")) + " - "
                        + DifficultyName(static_cast<AIDifficulty>(std::clamp(cs.level, 0, 3))),
                    col::Info);
            combo(
                1,
                si == 0 ? (m_sideRandom
                               ? _("Random")
                               : (PlayerFaction() == Faction::Confederation ? _("Confederation") : _("Resistance")))
                        : (cs.faction == 0 ? _("Random") : (cs.faction == 1 ? _("Resistance") : _("Confederation"))));
            combo(2, std::to_string(cs.team + 1));
            combo(7, cs.handicap == 0 ? std::string("-") : HandicapText(cs.handicap));
            combo(8, ui::ArmyCapChoices()[std::clamp(cs.armyCap, 0, kArmyCapCount - 1)]);
            combo(9, ui::TechLevelLabel(cs.techLevel, true), cs.techLevel < 0 ? col::TextDim : col::Text);
            combo(3, "");
            const UiRect cc = CustomCell(d, i, 3);
            renderer.FillRectScreen(cc.x + 6, cc.y + 6, cc.w - 28, cc.h - 12, ArmyColor(cs.color));
            renderer.DrawRectScreen(cc.x + 6, cc.y + 6, cc.w - 28, cc.h - 12, col::BorderStrong);
        }
        const UiRect pv = CustomPreviewArea(d);
        int pw = 0, ph = 0;
        SDL_Texture *tex = m_mapPreview.Get(renderer.GetSdlRenderer(), texManager, info.path, info.path, {}, pw, ph);
        DrawMapPreviewIn(renderer, tex, pw, ph, pv);
        // The start positions with their numbers, in the colour of the player who takes each
        if (m_spotsPath != info.path)
        {
            m_spotsPath = info.path;
            m_spots = MissionController::StartPositions(info.path);
        }
        if (info.width > 0 && info.height > 0)
        {
            for (int n = 0; n < static_cast<int>(m_spots.size()); ++n)
            {
                SDL_Color c = col::TextDim;
                for (const CustomSlot &cs : m_customSlots)
                    if (cs.position == n)
                        c = ArmyColor(cs.color);
                int sx = 0, sy = 0;
                SpotMarkerPos(pv, info.width, info.height, m_spots[n].first, m_spots[n].second, sx, sy);
                const bool blockedSpot = ((m_customBlocked >> n) & 1u) != 0;
                renderer.FillRectScreen(sx - 8, sy - 8, 16, 16, blockedSpot ? SDL_Color {40, 20, 20, 255} : c);
                renderer.DrawRectScreen(sx - 8, sy - 8, 16, 16, blockedSpot ? col::Danger : col::Text);
                if (blockedSpot)
                {
                    renderer.DrawLineScreen(sx - 7, sy - 7, sx + 7, sy + 7, col::Danger);
                    renderer.DrawLineScreen(sx - 7, sy + 7, sx + 7, sy - 7, col::Danger);
                }
                fontRenderer.DrawString(
                    std::to_string(n + 1),
                    sx,
                    sy,
                    ui::FONT_SMALL_BOLD,
                    col::AccentText,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
            }
        }
        const std::string size = std::to_string(info.width) + " x " + std::to_string(info.height);
        DrawTextSection(
            renderer,
            fontRenderer,
            l.fogCheckbox.x,
            l.speedCheckbox.Bottom() + 10,
            l.fogCheckbox.w,
            _("MAP SIZE:"),
            size,
            col::Ok,
            ui::FONT_BODY);
        ui::DrawCheckbox(
            renderer,
            fontRenderer,
            l.baseCheckbox,
            _("START WITH A BASE"),
            m_skirmishPrebuilt,
            l.baseCheckbox.Contains(m_mouseX, m_mouseY));
        ui::DrawCheckbox(
            renderer,
            fontRenderer,
            l.fogCheckbox,
            _("FOG OF WAR"),
            m_skirmishFogOfWar,
            l.fogCheckbox.Contains(m_mouseX, m_mouseY));
        ui::DrawCheckbox(
            renderer,
            fontRenderer,
            l.speedCheckbox,
            _("ALLOW SPEED CONTROL"),
            m_skirmishAllowSpeed,
            l.speedCheckbox.Contains(m_mouseX, m_mouseY));
    }
    else if (!m_customGame && selected >= 0 && selected < count)
    {
        const auto &minfo = missions[selected];
        const UiRect &d = l.details;
        ui::DrawPanel(renderer, d, col::PanelSunken, col::Border);

        int x = d.x + 20;
        int w = d.w - 40;
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, _(minfo.name), ui::FONT_H2, w), x, d.y + 18, ui::FONT_H2, col::Accent, true);
        fontRenderer.DrawString(_(minfo.mapName), x, d.y + 50, ui::FONT_SMALL, col::TextMuted, false);
        renderer.FillRectScreen(x, d.y + 74, w, 1, col::Border);

        int y = DrawTextSection(
            renderer, fontRenderer, x, d.y + 90, w, _("OBJECTIVES:"), _(minfo.objective), col::Ok, ui::FONT_BODY);
        DrawTextSection(
            renderer, fontRenderer, x, y, w, _("BRIEFING:"), _(minfo.briefing), col::TextDim, ui::FONT_SMALL);

        // The mission's map with its preplaced bases: Resistance red, Confederation blue
        const MissionData &data = Missions::Campaign(PlayerFaction())[selected].data;
        std::vector<PreviewMarker> markers;
        for (const MissionEntity &e : data.entities)
        {
            if (e.type >= 15)
                continue;
            markers.push_back({e.x, e.y, e.type >= 8 ? SDL_Color {230, 70, 60, 255} : SDL_Color {70, 150, 240, 255}});
        }
        int pw = 0, ph = 0;
        // Each side has its own campaign: the key keeps their pictures apart
        const std::string previewKey = std::string("mission:") + (PlayerFaction() == Faction::Confederation ? "c" : "r")
                                       + std::to_string(selected);
        SDL_Texture *tex = m_mapPreview.Get(
            renderer.GetSdlRenderer(),
            texManager,
            previewKey,
            LoaderResources::MissionMapPath(data.mapFile),
            markers,
            pw,
            ph);
        DrawMapPreview(renderer, tex, pw, ph, d);
    }

    // Side and difficulty: drop-down lists
    {
        const SDL_Color sideColor
            = PlayerFaction() == Faction::Confederation ? SDL_Color {64, 150, 226, 255} : SDL_Color {214, 72, 56, 255};
        ui::DrawComboField(
            renderer,
            fontRenderer,
            l.sideBtn[0],
            CampaignComboOptions(kComboSide)[CampaignComboCurrent(kComboSide)],
            m_combo.open == kComboSide,
            l.sideBtn[0].Contains(m_mouseX, m_mouseY),
            true,
            sideColor);
        ui::DrawComboField(
            renderer,
            fontRenderer,
            l.difficultyBtn,
            std::string(_("DIFFICULTY")) + ": " + DifficultyName(m_difficulty),
            m_combo.open == kComboDifficulty,
            l.difficultyBtn.Contains(m_mouseX, m_mouseY));
    }
    ui::DrawButton(
        renderer,
        fontRenderer,
        l.editorBtn,
        _("MAP EDITOR"),
        ui::ButtonStyle::Secondary,
        l.editorBtn.Contains(m_mouseX, m_mouseY));
    if (!m_editorNotice.empty())
    {
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, m_editorNotice, ui::FONT_SMALL, l.dialog.w - 280),
            l.dialog.Right() - 20,
            l.dialog.y + 26,
            ui::FONT_SMALL,
            col::Danger,
            false,
            Anchor::RIGHT | Anchor::VCENTER);
    }
    ui::DrawButton(
        renderer,
        fontRenderer,
        l.startBtn,
        m_customGame ? _("START GAME") : _("START MISSION"),
        ui::ButtonStyle::Primary,
        l.startBtn.Contains(m_mouseX, m_mouseY));
    ui::DrawButton(
        renderer,
        fontRenderer,
        l.backBtn,
        _("BACK"),
        ui::ButtonStyle::Secondary,
        l.backBtn.Contains(m_mouseX, m_mouseY));
    if (m_combo.open >= kComboSide)
    { // the open list, over everything
        const int id = m_combo.open;
        const UiRect field
            = id >= kComboSlot ? CustomCell(l.details, (id - kComboSlot) / 10, (id - kComboSlot) % 10)
                               : (id >= kComboMapPlayers && id <= kComboMapSort ? MapFilterBtn(l, id - kComboMapPlayers)
                                  : id == kComboSide                            ? l.sideBtn[0]
                                  : id == kComboCredits                         ? l.creditsBtn
                                  : id == kComboTime                            ? l.timeBtn
                                  : id == kComboTech                            ? l.techBtn
                                                                                : l.difficultyBtn);
        std::vector<std::string> items = CampaignComboOptions(id);
        std::vector<bool> taken;
        std::vector<SDL_Color> swatches;
        const bool colors = id >= kComboSlot && (id - kComboSlot) % 10 == 3;
        if (colors)
        {
            taken = CustomTakenColors(CustomSlotAt((id - kComboSlot) / 10));
            const int current = CampaignComboCurrent(id);
            for (int i = 1; i <= Settings::kArmyColors; ++i)
                swatches.push_back(ArmyColor(i));
            for (std::size_t i = 0; i < items.size() && i < taken.size(); ++i)
            {
                if (taken[i] && static_cast<int>(i) != current)
                    items[i] += std::string(" (") + _("taken") + ")";
            }
        }
        ui::DrawComboList(
            renderer,
            fontRenderer,
            field,
            items,
            CampaignComboCurrent(id),
            m_combo,
            windowH,
            m_mouseX,
            m_mouseY,
            colors ? &taken : nullptr,
            colors ? &swatches : nullptr);
    }
}

std::vector<std::string> MenuController::CampaignComboOptions(int id) const
{
    if (id >= kComboSlot)
    {
        const int row = (id - kComboSlot) / 10, kind = (id - kComboSlot) % 10;
        std::vector<std::string> out;
        if (kind == 0)
        {
            for (int code : CustomSeatCodes(row))
                out.push_back(
                    code == 1   ? std::string(CustomSlotAt(row) == 0 ? _("YOU") : _("Occupy"))
                    : code == 2 ? std::string(_("Open"))
                    : code == 7 ? std::string(_("Blocked"))
                                : std::string(_("AI")) + " - " + DifficultyName(static_cast<AIDifficulty>(code - 3)));
        }
        else if (kind == 1)
        {
            out = {_("Random"), _("Resistance"), _("Confederation")};
        }
        else if (kind == 2)
        {
            for (int i = 1; i <= kMaxPlayers; ++i)
                out.push_back(std::string(_("Team")) + " " + std::to_string(i));
        }
        else if (kind == 3)
        {
            for (int i = 1; i <= Settings::kArmyColors; ++i)
                out.push_back(_(ArmyColorName(i)));
        }
        else if (kind == 7)
        {
            out = {_("None"), "-25%", "-50%", "+25%", "+50%", "+100%"};
        }
        else if (kind == 9)
        {
            out = ui::TechLevelChoices(true);
        }
        else
        {
            out = ui::ArmyCapChoices();
        }
        return out;
    }
    if (id == kComboMapPlayers)
    {
        std::vector<std::string> out;
        for (int n : MapPlayerCounts())
            out.push_back(std::string(_("Players")) + ": " + (n == 0 ? std::string(_("Any")) : std::to_string(n)));
        return out;
    }
    if (id == kComboMapSize)
    {
        const char *sizes[] = {_("Any"), _("Small"), _("Medium"), _("Large")};
        std::vector<std::string> out;
        for (const char *z : sizes)
            out.push_back(std::string(_("Size")) + ": " + z);
        return out;
    }
    if (id == kComboMapSort)
    {
        const char *sorts[] = {_("Name"), _("Players"), _("Size")};
        std::vector<std::string> out;
        for (const char *z : sorts)
            out.push_back(std::string(_("Sort")) + ": " + z);
        return out;
    }
    if (id == kComboSide)
        return {_("RESISTANCE"), _("CONFEDERACY"), _("RANDOM")};
    if (id == kComboDifficulty)
    {
        return {
            DifficultyName(AIDifficulty::Easy),
            DifficultyName(AIDifficulty::Medium),
            DifficultyName(AIDifficulty::Hard),
            DifficultyName(AIDifficulty::Hardcore)};
    }
    if (id == kComboCredits)
        return ui::CreditsChoices();
    if (id == kComboTime)
        return ui::TimeOfDayChoices();
    if (id == kComboTech)
        return ui::TechLevelChoices(false);
    return ui::LayoutChoices();
}

int MenuController::CampaignComboCurrent(int id) const
{
    if (id >= kComboSlot)
    {
        const int row = (id - kComboSlot) / 10, kind = (id - kComboSlot) % 10;
        const int si = CustomSlotAt(row);
        if (kind == 0)
        {
            const std::vector<int> codes = CustomSeatCodes(row);
            const int now = si == 0 ? 1
                                    : (si > 0 ? 3 + std::clamp(m_customSlots[si].level, 0, 3)
                                              : (((m_customBlocked >> row) & 1u) ? 7 : 2));
            for (std::size_t i = 0; i < codes.size(); ++i)
                if (codes[i] == now)
                    return static_cast<int>(i);
            return 0;
        }
        if (si < 0)
            return 0;
        const CustomSlot &cs = m_customSlots[si];
        if (kind == 1)
            return si == 0 ? (m_sideRandom ? 0 : (PlayerFaction() == Faction::Confederation ? 2 : 1)) : cs.faction;
        if (kind == 2)
            return cs.team;
        if (kind == 3)
            return cs.color - 1;
        if (kind == 7)
            return std::clamp(cs.handicap, 0, kHandicapCount - 1);
        if (kind == 9)
            return std::max(0, cs.techLevel + 1);
        return std::clamp(cs.armyCap, 0, kArmyCapCount - 1);
    }
    if (id == kComboMapPlayers)
    {
        const std::vector<int> counts = MapPlayerCounts();
        const auto it = std::find(counts.begin(), counts.end(), m_mapPlayersFilter);
        return it == counts.end() ? 0 : static_cast<int>(it - counts.begin());
    }
    if (id == kComboMapSize)
        return std::clamp(m_mapSizeFilter, 0, 3);
    if (id == kComboMapSort)
        return std::clamp(m_mapSort, 0, 2);
    if (id == kComboSide)
        return m_sideRandom ? 2 : (PlayerFaction() == Faction::Confederation ? 1 : 0);
    if (id == kComboDifficulty)
        return std::clamp(static_cast<int>(m_difficulty), 0, 3);
    if (id == kComboTime)
        return m_skirmishNight ? 1 : 0;
    if (id == kComboTech)
        return std::max(0, m_skirmishTechLevel + 1);
    return std::clamp(m_skirmishCredits, 0, 4);
}

void MenuController::CampaignComboApply(int id, int index)
{
    if (id >= kComboSlot)
    {
        const int row = (id - kComboSlot) / 10, kind = (id - kComboSlot) % 10;
        if (kind == 0)
        {
            const std::vector<int> codes = CustomSeatCodes(row);
            if (index >= 0 && index < static_cast<int>(codes.size()))
                SetCustomSeat(row, codes[index]);
            return;
        }
        const int si = CustomSlotAt(row);
        if (si < 0)
            return;
        CustomSlot &cs = m_customSlots[si];
        if (kind == 1)
        {
            if (si == 0)
            {
                m_sideRandom = index == 0;
                if (index != 0)
                    SetPlayerFaction(index == 2 ? Faction::Confederation : Faction::Resistance);
            }
            else
            {
                cs.faction = index;
            }
        }
        else if (kind == 2)
        {
            cs.team = std::clamp(index, 0, kMaxPlayers - 1);
        }
        else if (kind == 3)
        {
            const std::vector<bool> taken = CustomTakenColors(si);
            if (index >= 0 && index < static_cast<int>(taken.size()) && !taken[index])
                cs.color = index + 1;
        }
        else if (kind == 7)
        {
            cs.handicap = std::clamp(index, 0, kHandicapCount - 1);
        }
        else if (kind == 9)
        {
            cs.techLevel = std::clamp(index - 1, -1, static_cast<int>(Capabilities::Levels().size()) - 1);
        }
        else
        {
            cs.armyCap = std::clamp(index, 0, kArmyCapCount - 1);
        }
        return;
    }
    if (id >= kComboMapPlayers && id <= kComboMapSort)
    {
        if (id == kComboMapPlayers)
        {
            const std::vector<int> counts = MapPlayerCounts();
            m_mapPlayersFilter = counts[std::clamp(index, 0, static_cast<int>(counts.size()) - 1)];
        }
        else if (id == kComboMapSize)
        {
            m_mapSizeFilter = std::clamp(index, 0, 3);
        }
        else
        {
            m_mapSort = std::clamp(index, 0, 2);
        }
        RebuildMapList();
    }
    else if (id == kComboSide)
    {
        m_sideRandom = index == 2;
        if (index != 2)
            SetPlayerFaction(index == 1 ? Faction::Confederation : Faction::Resistance);
    }
    else if (id == kComboDifficulty)
    {
        if (!m_settings)
            return;
        m_settings->difficulty = std::clamp(index, 0, 3);
        m_difficulty = static_cast<AIDifficulty>(m_settings->difficulty);
        if (m_onSettingsChanged)
            m_onSettingsChanged(SettingsArea::Game);
    }
    else if (id == kComboCredits)
    {
        m_skirmishCredits = std::clamp(index, 0, 4);
    }
    else if (id == kComboTime)
    {
        m_skirmishNight = index == 1;
    }
    else if (id == kComboTech)
    {
        m_skirmishTechLevel = std::clamp(index - 1, -1, static_cast<int>(Capabilities::Levels().size()) - 1);
    }
}

} // namespace OpenArtOfWar
