#include "ui/UiLayout.hpp"
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
bool LaunchMapEditor(std::string_view mapPath)
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
    std::error_code errorCode;
    for (const auto &exe : candidates)
    {
        if (!std::filesystem::is_regular_file(exe, errorCode))
            continue;
        const std::string exePath = std::filesystem::absolute(exe, errorCode).string();
        const std::string mapPathStr(mapPath);
        const char *args[] = {exePath.c_str(), mapPathStr.c_str(), nullptr};
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
    const int percent = HandicapPercentOf(index) - 100;
    return (percent > 0 ? "+" : "") + std::to_string(percent) + "%";
}

// Custom game: the table of players in the details panel (columns: level / name, faction, team, colour, remove)
// The seat table takes the whole width of the details (as in the network room); the map picture sits under it on the
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
    int customHeaderLineY {0};
    int customRowsTop {0};
    int itemH {42};
    int itemGap {4};
    bool touch {false};

    UiRect Item(int rowIndex) const
    {
        return {list.x, list.y + rowIndex * (itemH + itemGap), list.w, itemH};
    }
    UiRect ItemAt(int rowIndex, int scrollPx) const
    {
        return {list.x, list.y + rowIndex * (itemH + itemGap) - scrollPx, list.w, itemH};
    }
};

// left, the options and the map size on its right
int CustomRowH(const CampaignLayout &layout)
{
    const int headerUsed = std::max(0, layout.customRowsTop - layout.details.y);
    return std::clamp((layout.details.h - headerUsed - 150 - 30) / 8, 34, 46);
}

UiRect CustomRow(const CampaignLayout &layout, int rowIndex)
{
    const int rowHeight = CustomRowH(layout);
    return {layout.details.x + 20, layout.customRowsTop + rowIndex * rowHeight, layout.details.w - 40, rowHeight - 4};
}

UiRect CustomCell(const CampaignLayout &layout, int rowIndex, int kind)
{
    // (rowIndex is a start position of the map; kind 0 what holds it, 1 faction, 2 team, 3 colour, 7 handicap,
    // 8 army size, 9 tech level). The cells share the width of the row, so they stretch with the window
    static constexpr int kColumnShare[7] = {23, 17, 8, 8, 12, 15, 17};
    const UiRect rowRect = CustomRow(layout, rowIndex);
    const int columnIndex = kind == 1   ? 1
                            : kind == 2 ? 2
                            : kind == 3 ? 3
                            : kind == 7 ? 4
                            : kind == 8 ? 5
                            : kind == 9 ? 6
                                        : 0;
    const int gap = 4;
    const int contentX = rowRect.x + 24;
    const int totalWidth = rowRect.w - 24 - 6 * gap - 4;
    int usedPercent = 0;
    for (int j = 0; j < columnIndex; ++j)
        usedPercent += kColumnShare[j];
    return {
        contentX + totalWidth * usedPercent / 100 + columnIndex * gap,
        rowRect.y,
        totalWidth * kColumnShare[columnIndex] / 100,
        rowRect.h};
}

UiRect CustomPreviewArea(const CampaignLayout &layout)
{
    const int top = layout.customRowsTop + 8 * CustomRowH(layout) + 12;
    const int size = std::clamp(
        std::min(layout.details.Bottom() - 16 - top, static_cast<int>(layout.details.w * 0.45f)), 120, 460);
    return {layout.details.x + 20, top, size, size};
}

CampaignLayout campaignLayout(int windowWidth, int windowHeight, int missionCount)
{
    CampaignLayout layout;
    const std::string profile = uilayout::MenuProfile(uilayout::Current(), "map_select");
    layout.touch = profile == "touch";
    // ui/layout.yaml (screen map_select, shared with the network room); the numbers below stand in without it
    if (uilayout::Current().Has("map_select", profile))
    {
        const uilayout::Solved solved = uilayout::Current().Solve("map_select", profile, windowWidth, windowHeight, {});
        layout.dialog = solved.Rect("dialog");
        layout.list = solved.Rect("list");
        const int clampedCount = std::max(1, missionCount);
        layout.itemH = std::clamp(
            (layout.list.h - (clampedCount - 1) * layout.itemGap) / clampedCount,
            layout.touch ? 46 : 36,
            layout.touch ? 56 : 46);
        layout.details = solved.Rect("details");
        layout.backBtn = solved.Rect("back");
        layout.startBtn = solved.Rect("start");
        layout.sideBtn[0] = solved.Rect("side");
        layout.editorBtn = solved.Rect("editor");
        layout.difficultyBtn = solved.Rect("difficulty");
        layout.creditsBtn = solved.Rect("credits");
        layout.timeBtn = solved.Rect("time");
        layout.techBtn = solved.Rect("tech");
        layout.customHeaderLineY = std::max(layout.details.y + 58, layout.creditsBtn.Bottom() + 10);
        layout.customRowsTop = layout.customHeaderLineY + 46;
        const UiRect previewRect = CustomPreviewArea(layout);
        const int optionsX = previewRect.Right() + 20;
        const int cbH = layout.touch ? 36 : 32;
        const int cbStep = layout.touch ? 40 : 36;
        layout.fogCheckbox = {optionsX, previewRect.y, layout.details.Right() - 20 - optionsX, cbH};
        layout.baseCheckbox = {optionsX, previewRect.y + cbStep, layout.details.Right() - 20 - optionsX, cbH};
        layout.speedCheckbox = {optionsX, previewRect.y + cbStep * 2, layout.details.Right() - 20 - optionsX, cbH};
        return layout;
    }
    layout.dialog = ui::CenteredRect(windowWidth, windowHeight, windowWidth, windowHeight); // the whole screen
    int contentY = layout.dialog.y + 52 + 16;
    int footerY = layout.dialog.Bottom() - 64;
    layout.list
        = {layout.dialog.x + 20,
           contentY,
           std::clamp(static_cast<int>(layout.dialog.w * 0.27f), 300, 460),
           footerY - contentY - 8};
    int clampedCount = std::max(1, missionCount);
    layout.itemH = std::clamp((layout.list.h - (clampedCount - 1) * layout.itemGap) / clampedCount, 36, 46);
    layout.details
        = {layout.list.Right() + 16, contentY, layout.dialog.Right() - 20 - (layout.list.Right() + 16), layout.list.h};
    layout.backBtn = {layout.dialog.Right() - 20 - 140, footerY + 10, 140, 40};
    layout.startBtn = {layout.backBtn.x - 12 - 200, footerY + 10, 200, 40};
    layout.sideBtn[0] = {layout.dialog.x + 20, footerY + 10, 240, 40};
    layout.editorBtn = {layout.startBtn.x - 12 - 140, footerY + 10, 140, 40};
    layout.creditsBtn = {layout.details.Right() - 20 - 200, layout.details.y + 10, 200, 36};
    layout.timeBtn = {layout.creditsBtn.x - 12 - 160, layout.details.y + 10, 160, 36};
    layout.techBtn = {layout.timeBtn.x - 12 - 180, layout.details.y + 10, 180, 36};
    layout.customHeaderLineY = std::max(layout.details.y + 58, layout.creditsBtn.Bottom() + 10);
    layout.customRowsTop = layout.customHeaderLineY + 46;
    const UiRect previewRect = CustomPreviewArea(layout);
    const int optionsX = previewRect.Right() + 20;
    layout.fogCheckbox = {optionsX, previewRect.y, layout.details.Right() - 20 - optionsX, 32};
    layout.baseCheckbox = {optionsX, previewRect.y + 36, layout.details.Right() - 20 - optionsX, 32};
    layout.speedCheckbox = {optionsX, previewRect.y + 72, layout.details.Right() - 20 - optionsX, 32};
    layout.difficultyBtn
        = {layout.sideBtn[0].Right() + 12,
           footerY + 10,
           std::max(180, layout.editorBtn.x - 12 - (layout.sideBtn[0].Right() + 12)),
           40};
    return layout;
}

// The custom game list shows as many fixed-height rows as fit and scrolls
// A start marker stays inside its picture
void SpotMarkerPos(
    const UiRect &previewRect, int mapWidth, int mapHeight, int tileX, int tileY, int &screenX, int &screenY)
{
    const float scale = std::min(
        (previewRect.w - 4) / static_cast<float>(mapWidth), (previewRect.h - 4) / static_cast<float>(mapHeight));
    const float originX = previewRect.CenterX() - mapWidth * scale / 2;
    const float originY = previewRect.CenterY() - mapHeight * scale / 2;
    screenX
        = std::clamp(static_cast<int>(originX + (tileX + 0.5f) * scale), previewRect.x + 10, previewRect.Right() - 10);
    screenY
        = std::clamp(static_cast<int>(originY + (tileY + 0.5f) * scale), previewRect.y + 10, previewRect.Bottom() - 10);
}

int MapFilterBarH(const CampaignLayout &layout)
{
    return layout.touch ? 96 : 80;
}

UiRect MapSearchRect(const CampaignLayout &layout)
{
    return {layout.list.x, layout.list.y - MapFilterBarH(layout), layout.list.w, layout.touch ? 42 : 36};
}

UiRect MapSearchClearRect(const CampaignLayout &layout)
{
    const UiRect s = MapSearchRect(layout);
    return {s.Right() - s.h, s.y + 2, s.h - 2, s.h - 4};
}

UiRect MapFilterBtn(const CampaignLayout &layout, int filterIndex)
{
    const int buttonWidth = (layout.list.w - 2 * 6) / 3;
    const int barH = MapFilterBarH(layout);
    const int yOff = layout.touch ? 48 : 40;
    const int btnH = layout.touch ? 42 : 34;
    return {layout.list.x + filterIndex * (buttonWidth + 6), layout.list.y - barH + yOff, buttonWidth, btnH};
}

int CustomListRows(CampaignLayout &layout)
{
    // (idempotent: the bar takes the top of the list for a custom game)
    const int barH = MapFilterBarH(layout);
    layout.list.y += barH;
    layout.list.h -= barH;
    layout.itemH = layout.touch ? 48 : 38;
    return std::max(1, (layout.list.h + layout.itemGap) / (layout.itemH + layout.itemGap));
}

// The picture of a map in the bottom part of the details panel, scaled to fit and centered
void DrawMapPreviewIn(Renderer &renderer, SDL_Texture *texture, int textureWidth, int textureHeight, const UiRect &area)
{
    ui::DrawPanel(renderer, area, col::PanelSunken, col::BorderStrong);
    if (!texture || textureWidth <= 0 || textureHeight <= 0)
        return;
    const float scale
        = std::min((area.w - 4) / static_cast<float>(textureWidth), (area.h - 4) / static_cast<float>(textureHeight));
    const int drawWidth = std::max(1, static_cast<int>(textureWidth * scale));
    const int drawHeight = std::max(1, static_cast<int>(textureHeight * scale));
    SDL_FRect destRect {
        static_cast<float>(area.CenterX() - drawWidth / 2),
        static_cast<float>(area.CenterY() - drawHeight / 2),
        static_cast<float>(drawWidth),
        static_cast<float>(drawHeight)};
    SDL_RenderTexture(renderer.GetSdlRenderer(), texture, nullptr, &destRect);
}

void DrawMapPreview(Renderer &renderer, SDL_Texture *texture, int textureWidth, int textureHeight, const UiRect &panel)
{
    const int margin = 20;
    const int maxHeight = std::min(panel.h / 2 - 12, 220);
    DrawMapPreviewIn(
        renderer,
        texture,
        textureWidth,
        textureHeight,
        UiRect {panel.x + margin, panel.Bottom() - margin - maxHeight, panel.w - 2 * margin, maxHeight});
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
    m_skirmishScroll.Reset();
}

bool MenuController::UpdateCampaignSelect(const FrameContext &context, int mouseX, int mouseY, bool leftClicked)
{
    InputManager &input = context.input;
    const int windowWidth = context.windowW;
    const int windowHeight = context.windowH;
    MissionController &mission = context.mission;
    TileMap &map = context.map;
    EntityManager &entities = context.entities;
    Economy &economy = context.economy;
    Pathfinder &pathfinder = context.pathfinder;
    AppState &state = context.state;
    if (!m_campaignSelectOpen)
        return false;
    m_mouseX = mouseX;
    m_mouseY = mouseY;
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

    const int itemCount = m_customGame ? static_cast<int>(m_skirmishMaps.size())
                                       : std::min(
                                             MissionController::kCampaignMissionCount,
                                             static_cast<int>(MissionController::GetAvailableMissions().size()));
    if (itemCount == 0 && !(m_customGame && !m_skirmishAll.empty()))
    {
        m_campaignSelectOpen = false;
        return true;
    }
    int &selected = m_customGame ? m_skirmishSelected : m_campaignMissionSelected;
    CampaignLayout layout = campaignLayout(windowWidth, windowHeight, itemCount);
    const int visibleRows = m_customGame ? CustomListRows(layout) : itemCount;

    auto startSelected = [&]() {
        if (m_customGame && itemCount == 0)
            return;
        if (m_customGame)
        {
            EnsureCustomSlots();
            bool hasFoes = false;
            const std::size_t firstCheckSlot = m_customSpectator ? 1 : 0;
            if (m_customSlots.size() <= firstCheckSlot + (m_customSpectator ? 1u : 0u))
            {
                m_editorNotice = _("A watched match needs at least two computer players.");
                return;
            }
            for (std::size_t i = firstCheckSlot; i < m_customSlots.size(); ++i)
                hasFoes = hasFoes || m_customSlots[i].team != m_customSlots[firstCheckSlot].team;
            if (!hasFoes)
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
                const auto &missionEntry = Missions::Campaign(PlayerFaction())[m_campaignMissionSelected];
                m_beginLoading(_(missionEntry.info.name), LoaderResources::MissionMapPath(missionEntry.data.mapFile));
            }
        }
        // A custom game is a match of this computer alone: every player at a start position of the map, the person at
        // the first one
        MatchSetup match;
        if (m_customGame)
        {
            std::random_device randomDevice;
            match.mapPath = m_skirmishMaps[m_skirmishSelected].path;
            match.seed = randomDevice();
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
            const int firstSlotIndex = m_customSpectator ? 1 : 0;
            for (int slotIndex = firstSlotIndex;
                 slotIndex < static_cast<int>(m_customSlots.size()) && slotIndex - firstSlotIndex < kMaxPlayers;
                 ++slotIndex)
            {
                const int playerIndex = slotIndex - firstSlotIndex;
                const CustomSlot &customSlot = m_customSlots[slotIndex];
                PlayerSlot &slot = match.slots[playerIndex];
                slot.active = true;
                slot.human = slotIndex == 0;
                if (slotIndex == 0)
                {
                    if (m_sideRandom)
                        SetPlayerFaction(
                            (randomDevice() & 1u) ? Faction::Confederation : Faction::Resistance); // drawn now
                    slot.faction = PlayerFaction();
                }
                else if (customSlot.faction == 0)
                    slot.faction = (randomDevice() & 1u) ? Faction::Confederation : Faction::Resistance;
                else
                    slot.faction = customSlot.faction == 2 ? Faction::Confederation : Faction::Resistance;
                slot.team = static_cast<std::uint8_t>(customSlot.team);
                slot.color = static_cast<std::uint8_t>(customSlot.color);
                slot.handicap = static_cast<std::uint8_t>(std::clamp(customSlot.handicap, 0, kHandicapCount - 1));
                slot.armyCap = static_cast<std::uint8_t>(std::clamp(customSlot.armyCap, 0, kArmyCapCount - 1));
                slot.techLevel = static_cast<std::int8_t>(customSlot.techLevel);
                slot.aiLevel = static_cast<std::uint8_t>(std::clamp(customSlot.level, 0, 3));
                match.startPosition[playerIndex] = static_cast<std::int8_t>(customSlot.position);
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

    // Consume wheel and press once per frame before any dropdown or list scroll uses them
    const float wheel = input.ConsumeWheelF();
    int pressX = 0, pressY = 0;
    const bool pressed = input.ConsumeLeftPress(pressX, pressY) && input.IsLeftMouseDown();
    const bool down = input.IsLeftMouseDown();

    // Custom game: search box and filter buttons over the list of maps
    if (m_customGame)
    {
        if (leftClicked)
        {
            if (!m_mapSearch.empty() && MapSearchClearRect(layout).Contains(mouseX, mouseY))
            {
                m_mapSearch.clear();
                RebuildMapList();
                m_mapSearchFocus = true;
                return true;
            }
            m_mapSearchFocus = MapSearchRect(layout).Contains(mouseX, mouseY);
            if (m_mapSearchFocus)
                return true;
        }
        if (m_mapSearchFocus)
        {
            const std::string before = m_mapSearch;
            for (int backspaceCount = input.TakeBackspaces(); backspaceCount > 0; --backspaceCount)
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
    const bool hasLayoutChoice
        = m_customGame && m_skirmishSelected >= 0 && m_skirmishSelected < static_cast<int>(m_skirmishMaps.size());
    if (hasLayoutChoice)
        EnsureCustomSlots();
    auto comboField = [&](int comboId) {
        if (comboId >= kComboSlot)
            return CustomCell(layout, (comboId - kComboSlot) / 10, (comboId - kComboSlot) % 10);
        if (comboId >= kComboMapPlayers && comboId <= kComboMapSort)
            return MapFilterBtn(layout, comboId - kComboMapPlayers);
        return comboId == kComboSide      ? layout.sideBtn[0]
               : comboId == kComboCredits ? layout.creditsBtn
               : comboId == kComboTime    ? layout.timeBtn
               : comboId == kComboTech    ? layout.techBtn
                                          : layout.difficultyBtn;
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
        const int comboId = m_combo.open;
        int pickedIndex = -1;
        if (ui::ComboUpdate(
                m_combo,
                comboField(comboId),
                static_cast<int>(CampaignComboOptions(comboId).size()),
                windowHeight,
                mouseX,
                mouseY,
                leftClicked,
                wheel,
                pickedIndex,
                down,
                pressed,
                pressX,
                pressY))
        {
            if (pickedIndex >= 0)
                CampaignComboApply(comboId, pickedIndex);
            return true;
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            m_combo.open = -1;
            return true;
        }
    }

    const int stepH = layout.itemH + layout.itemGap;
    const int contentH = m_customGame ? (itemCount * stepH - (itemCount > 0 ? layout.itemGap : 0)) : layout.list.h;
    bool navMoved = false;
    if (itemCount > 0 && input.IsKeyJustPressed(SDL_SCANCODE_UP))
    {
        selected = (selected - 1 + itemCount) % itemCount;
        navMoved = true;
    }
    if (itemCount > 0 && input.IsKeyJustPressed(SDL_SCANCODE_DOWN))
    {
        selected = (selected + 1) % itemCount;
        navMoved = true;
    }
    if (m_customGame && itemCount > 0)
    {
        if (input.IsKeyJustPressed(SDL_SCANCODE_PAGEUP))
        {
            selected = std::max(0, selected - visibleRows);
            navMoved = true;
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_PAGEDOWN))
        {
            selected = std::min(itemCount - 1, selected + visibleRows);
            navMoved = true;
        }
        if (navMoved)
            m_skirmishScroll.EnsureVisible(selected, stepH, layout.list.h, itemCount);

        const UiRect scrollBarRect {layout.list.Right() + 4, layout.list.y, 6, layout.list.h};
        ui::UpdateSmoothList(
            m_skirmishScroll,
            layout.list,
            scrollBarRect,
            contentH,
            stepH * 2,
            mouseX,
            mouseY,
            down,
            pressed,
            pressX,
            pressY,
            wheel,
            leftClicked);
        if (m_skirmishScroll.barDrag)
            return true;
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        m_campaignSelectOpen = false;
        return true;
    }
    if (!m_mapSearchFocus && (input.IsKeyJustPressed(SDL_SCANCODE_LEFT) || input.IsKeyJustPressed(SDL_SCANCODE_RIGHT)))
    {
        SetPlayerFaction(OpposingFaction(PlayerFaction()));
    }

    if (input.IsKeyJustPressed(SDL_SCANCODE_TAB))
        cycleDifficulty();
    if (leftClicked)
    {
        auto openCombo = [&](int comboId) {
            m_combo.open = comboId;
            m_combo.scroll = 0;
            return true;
        };
        if (m_customGame)
            for (int comboId = kComboMapPlayers; comboId <= kComboMapSort; ++comboId)
                if (MapFilterBtn(layout, comboId - kComboMapPlayers).Contains(mouseX, mouseY))
                    return openCombo(comboId);
        if (layout.sideBtn[0].Contains(mouseX, mouseY))
            return openCombo(kComboSide);
        if (layout.difficultyBtn.Contains(mouseX, mouseY))
            return openCombo(kComboDifficulty);
        if (hasLayoutChoice && layout.baseCheckbox.Contains(mouseX, mouseY))
        {
            m_skirmishPrebuilt = !m_skirmishPrebuilt;
            SoundRequests::Play("ui_confirm");
            return true;
        }
        if (hasLayoutChoice && layout.creditsBtn.Contains(mouseX, mouseY))
            return openCombo(kComboCredits);
        if (hasLayoutChoice && layout.timeBtn.Contains(mouseX, mouseY))
            return openCombo(kComboTime);
        if (hasLayoutChoice && layout.techBtn.Contains(mouseX, mouseY))
            return openCombo(kComboTech);
        if (hasLayoutChoice && layout.fogCheckbox.Contains(mouseX, mouseY))
        {
            m_skirmishFogOfWar = !m_skirmishFogOfWar;
            SoundRequests::Play("ui_confirm");
            return true;
        }
        if (hasLayoutChoice && layout.speedCheckbox.Contains(mouseX, mouseY))
        {
            m_skirmishAllowSpeed = !m_skirmishAllowSpeed;
            SoundRequests::Play("ui_confirm");
            return true;
        }
        if (hasLayoutChoice)
        {
            const SkirmishMapInfo &mapInfo = m_skirmishMaps[m_skirmishSelected];
            const int spotCount = std::min(mapInfo.players, kMaxPlayers);
            for (int spotIndex = 0; spotIndex < spotCount; ++spotIndex)
            {
                if (CustomCell(layout, spotIndex, 0).Contains(mouseX, mouseY))
                    return openCombo(kComboSlot + spotIndex * 10);
                const int slotIndex = CustomSlotAt(spotIndex);
                if (slotIndex < 0)
                    continue;
                for (int kind : {1, 2, 3, 7, 8, 9})
                    if (CustomCell(layout, spotIndex, kind).Contains(mouseX, mouseY))
                        return openCombo(kComboSlot + spotIndex * 10 + kind);
            }
            // An open start position on the map: the person takes it
            {
                const UiRect previewRect = CustomPreviewArea(layout);
                if (m_spotsPath == mapInfo.path && mapInfo.width > 0 && mapInfo.height > 0
                    && previewRect.Contains(mouseX, mouseY))
                {
                    for (int spotIndex = 0; spotIndex < static_cast<int>(m_spots.size()) && spotIndex < spotCount;
                         ++spotIndex)
                    {
                        int spotX = 0, spotY = 0;
                        SpotMarkerPos(
                            previewRect,
                            mapInfo.width,
                            mapInfo.height,
                            m_spots[spotIndex].first,
                            m_spots[spotIndex].second,
                            spotX,
                            spotY);
                        if (std::abs(mouseX - spotX) > 10 || std::abs(mouseY - spotY) > 10)
                            continue;
                        if (CustomSlotAt(spotIndex) < 0 && !((m_customBlocked >> spotIndex) & 1u))
                        {
                            SetCustomSeat(spotIndex, 1);
                            SoundRequests::Play("ui_confirm");
                        }
                        return true;
                    }
                }
            }
        }
        // Side the player commands in this mission
        if (layout.editorBtn.Contains(mouseX, mouseY) && (!m_customGame || itemCount > 0))
        {
            const std::string mapPath
                = m_customGame ? m_skirmishMaps[m_skirmishSelected].path
                               : LoaderResources::MissionMapPath(
                                     Missions::Campaign(PlayerFaction())[m_campaignMissionSelected].data.mapFile);
            m_editorNotice
                = LaunchMapEditor(mapPath) ? "" : _("Map editor (openaow-mapeditor) not found next to the game");
            return true;
        }
        if (layout.list.Contains(mouseX, mouseY))
        {
            const int scrollOff = m_customGame ? m_skirmishScroll.PixelOffset() : 0;
            const int relY = mouseY - layout.list.y + scrollOff;
            const int row = relY / stepH;
            if (relY >= 0 && (relY % stepH) < layout.itemH && row >= 0 && row < itemCount)
            {
                // First click selects, clicking the selected mission or map starts it
                if (selected == row)
                    startSelected();
                else
                    selected = row;
                return true;
            }
        }
        if (layout.startBtn.Contains(mouseX, mouseY))
        {
            startSelected();
            return true;
        }
        if (layout.backBtn.Contains(mouseX, mouseY))
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
    Renderer &renderer,
    TextureManager &textureManager,
    BitmapFontRenderer &fontRenderer,
    int windowWidth,
    int windowHeight)
{
    const auto &missions = MissionController::GetAvailableMissions();
    const int missionCount = std::min(MissionController::kCampaignMissionCount, static_cast<int>(missions.size()));
    const int itemCount = m_customGame ? static_cast<int>(m_skirmishMaps.size()) : missionCount;
    CampaignLayout layout = campaignLayout(windowWidth, windowHeight, itemCount);
    const int visibleRows = m_customGame ? CustomListRows(layout) : itemCount;
    const int stepH = layout.itemH + layout.itemGap;
    const int contentH = m_customGame ? (itemCount * stepH - (itemCount > 0 ? layout.itemGap : 0)) : layout.list.h;
    const int scrollOff = m_customGame ? m_skirmishScroll.PixelOffset() : 0;
    const int selected = m_customGame ? m_skirmishSelected : m_campaignMissionSelected;
    ui::DrawDialog(
        renderer,
        fontRenderer,
        windowWidth,
        windowHeight,
        layout.dialog,
        m_customGame ? _("SELECT MAP") : _("SELECT CAMPAIGN MISSION"));
    if (m_customGame)
    {
        const UiRect searchBoxRect = MapSearchRect(layout);
        ui::DrawPanel(renderer, searchBoxRect, col::PanelSunken, m_mapSearchFocus ? col::Accent : col::BorderStrong);
        const bool blink = (SDL_GetTicks() / 500) % 2 == 0;
        const std::string displayText = m_mapSearch.empty() && !m_mapSearchFocus
                                            ? std::string(_("Search map..."))
                                            : m_mapSearch + (m_mapSearchFocus && blink ? "|" : "");
        const int searchRightPad = m_mapSearch.empty() ? 20 : 44;
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, displayText, ui::FONT_BODY, searchBoxRect.w - searchRightPad),
            searchBoxRect.x + 12,
            searchBoxRect.CenterY(),
            ui::FONT_BODY,
            m_mapSearch.empty() && !m_mapSearchFocus ? col::TextMuted : col::Text,
            false,
            Anchor::LEFT | Anchor::VCENTER);
        if (!m_mapSearch.empty())
        {
            const UiRect clr = MapSearchClearRect(layout);
            const bool clrHot = clr.Contains(m_mouseX, m_mouseY);
            fontRenderer.DrawString(
                "X",
                clr.CenterX(),
                clr.CenterY(),
                ui::FONT_BODY_BOLD,
                clrHot ? col::Accent : col::TextDim,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }
        for (int comboId = kComboMapPlayers; comboId <= kComboMapSort; ++comboId)
        {
            const UiRect filterBtnRect = MapFilterBtn(layout, comboId - kComboMapPlayers);
            ui::DrawComboField(
                renderer,
                fontRenderer,
                filterBtnRect,
                CampaignComboOptions(comboId)[CampaignComboCurrent(comboId)],
                m_combo.open == comboId,
                filterBtnRect.Contains(m_mouseX, m_mouseY));
        }
        if (itemCount == 0)
            fontRenderer.DrawString(
                _("No maps match."),
                layout.list.CenterX(),
                layout.list.y + 30,
                ui::FONT_BODY,
                col::TextMuted,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
    }

    const int nameFont = layout.touch ? ui::FONT_H2 : ui::FONT_BODY_BOLD;
    renderer.SetClipRect({layout.list.x, layout.list.y, layout.list.w, layout.list.h});
    for (int itemIndex = 0; itemIndex < itemCount; ++itemIndex)
    {
        UiRect itemRect = layout.ItemAt(itemIndex, scrollOff);
        const int clipTop = std::max(itemRect.y, layout.list.y);
        const int clipBot = std::min(itemRect.Bottom(), layout.list.Bottom());
        if (clipBot <= clipTop)
            continue;
        bool isSelected = (itemIndex == selected);
        bool hovered = layout.list.Contains(m_mouseX, m_mouseY) && itemRect.Contains(m_mouseX, m_mouseY);
        if (hovered)
            SoundRequests::MarkHover();

        const UiRect bgRect {itemRect.x, clipTop, itemRect.w, clipBot - clipTop};
        ui::DrawButtonChrome(
            renderer,
            bgRect,
            isSelected ? ui::Mix(col::Hover, ui::color::AccentDeep, 0.45f)
                       : (hovered ? ui::Mix(col::Hover, ui::color::AccentDeep, 0.22f)
                                  : ui::Mix(col::PanelRaised, ui::color::AccentDeep, 0.10f)),
            isSelected ? col::Panel : col::PanelSunken,
            isSelected ? col::Accent : (hovered ? col::BorderStrong : ui::Mix(col::Border, col::AccentDeep, 0.25f)),
            isSelected ? col::Accent : (hovered ? col::BorderStrong : SDL_Color {0, 0, 0, 0}),
            isSelected ? col::Accent : SDL_Color {0, 0, 0, 0});

        if (itemRect.CenterY() - 8 < layout.list.y || itemRect.CenterY() + 8 > layout.list.Bottom())
            continue;

        std::string numberText = std::to_string(itemIndex + 1);
        fontRenderer.DrawString(
            numberText,
            itemRect.x + 24,
            itemRect.CenterY(),
            ui::FONT_BODY_BOLD,
            isSelected ? col::Accent : col::TextMuted,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
        std::string name;
        int rightReserve = 16;
        if (m_customGame)
        {
            name = m_skirmishMaps[itemIndex].DisplayName();
            const std::string pBadge = std::to_string(m_skirmishMaps[itemIndex].players) + "P";
            fontRenderer.DrawString(
                pBadge,
                itemRect.Right() - 12,
                itemRect.CenterY(),
                ui::FONT_SMALL_BOLD,
                isSelected ? col::Accent : col::TextDim,
                false,
                Anchor::RIGHT | Anchor::VCENTER);
            rightReserve = 44;
        }
        else
        {
            // Mission names are "Mission N: Title" - show just the title next to the number
            name = _(missions[itemIndex].name);
            std::size_t colon = name.find(": ");
            if (colon != std::string::npos)
                name = name.substr(colon + 2);
        }
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, name, nameFont, itemRect.w - 46 - rightReserve),
            itemRect.x + 46,
            itemRect.CenterY(),
            nameFont,
            isSelected ? col::Text : col::TextDim,
            false,
            Anchor::LEFT | Anchor::VCENTER);
    }
    renderer.ResetClipRect();

    if (m_customGame && itemCount > visibleRows)
    {
        const UiRect scrollBarRect {layout.list.Right() + 4, layout.list.y, 6, layout.list.h};
        ui::DrawScrollBar(
            renderer,
            scrollBarRect,
            m_skirmishScroll.offset,
            static_cast<float>(contentH),
            static_cast<float>(layout.list.h),
            m_skirmishScroll.barDrag || ui::OnScrollTrack(scrollBarRect, m_mouseX, m_mouseY));
    }

    if (m_customGame && selected >= 0 && selected < itemCount)
    {
        const SkirmishMapInfo &mapInfo = m_skirmishMaps[selected];
        const UiRect &detailsRect = layout.details;
        ui::DrawPanel(renderer, detailsRect, col::PanelSunken, col::Border);

        int contentX = detailsRect.x + 20;
        int contentWidth = detailsRect.w - 40;
        const int titleWidth = std::max(80, layout.techBtn.x - contentX - 16);
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, mapInfo.DisplayName(), ui::FONT_H2, titleWidth),
            contentX,
            layout.techBtn.CenterY(),
            ui::FONT_H2,
            col::Accent,
            true,
            Anchor::LEFT | Anchor::VCENTER);
        ui::DrawComboField(
            renderer,
            fontRenderer,
            layout.creditsBtn,
            ui::CreditsLabel(m_skirmishCredits),
            m_combo.open == kComboCredits,
            layout.creditsBtn.Contains(m_mouseX, m_mouseY));
        ui::DrawComboField(
            renderer,
            fontRenderer,
            layout.timeBtn,
            ui::TimeOfDayChoices()[m_skirmishNight ? 1 : 0],
            m_combo.open == kComboTime,
            layout.timeBtn.Contains(m_mouseX, m_mouseY));
        ui::DrawComboField(
            renderer,
            fontRenderer,
            layout.techBtn,
            ui::TechLevelLabel(m_skirmishTechLevel, false),
            m_combo.open == kComboTech,
            layout.techBtn.Contains(m_mouseX, m_mouseY));
        renderer.FillRectScreen(contentX, layout.customHeaderLineY, contentWidth, 1, col::Border);
        ui::DrawCaption(
            fontRenderer,
            renderer,
            contentX,
            layout.customHeaderLineY + 6,
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
            const UiRect cell = CustomCell(layout, 0, kind);
            fontRenderer.DrawString(
                ui::FitText(fontRenderer, text, ui::FONT_SMALL, cell.w),
                cell.x + 2,
                layout.customHeaderLineY + 26,
                ui::FONT_SMALL,
                col::TextMuted,
                false);
        }
        const int spotCount = std::min(mapInfo.players, kMaxPlayers);
        for (int spotIndex = 0; spotIndex < spotCount; ++spotIndex)
        {
            const int slotIndex = CustomSlotAt(spotIndex);
            const bool blocked = ((m_customBlocked >> spotIndex) & 1u) != 0;
            const UiRect rowRect = CustomRow(layout, spotIndex);
            ui::DrawPanel(
                renderer,
                rowRect,
                slotIndex == 0 ? col::Hover : (blocked ? col::PanelSunken : col::PanelRaised),
                slotIndex == 0 ? col::Accent : col::Border);
            fontRenderer.DrawString(
                std::to_string(spotIndex + 1),
                rowRect.x + 12,
                rowRect.CenterY(),
                ui::FONT_BODY_BOLD,
                slotIndex >= 0 ? ArmyColor(m_customSlots[slotIndex].color) : col::TextMuted,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
            auto combo = [&](int kind, std::string_view text, SDL_Color textColor = col::Text) {
                const UiRect cell = CustomCell(layout, spotIndex, kind);
                ui::DrawComboField(
                    renderer,
                    fontRenderer,
                    cell,
                    text,
                    m_combo.open == kComboSlot + spotIndex * 10 + kind,
                    cell.Contains(m_mouseX, m_mouseY),
                    true,
                    textColor);
            };
            if (slotIndex < 0)
            {
                combo(0, blocked ? _("Blocked") : _("Open"), blocked ? col::Danger : col::TextDim);
                continue;
            }
            const CustomSlot &customSlot = m_customSlots[slotIndex];
            if (slotIndex == 0)
                combo(0, _("YOU"), col::Text);
            else
                combo(
                    0,
                    std::string(_("AI")) + " - "
                        + DifficultyName(static_cast<AIDifficulty>(std::clamp(customSlot.level, 0, 3))),
                    col::Info);
            combo(
                1,
                slotIndex == 0
                    ? (m_sideRandom
                           ? _("Random")
                           : (PlayerFaction() == Faction::Confederation ? _("Confederation") : _("Resistance")))
                    : (customSlot.faction == 0 ? _("Random")
                                               : (customSlot.faction == 1 ? _("Resistance") : _("Confederation"))));
            combo(2, std::to_string(customSlot.team + 1));
            combo(7, customSlot.handicap == 0 ? std::string("-") : HandicapText(customSlot.handicap));
            combo(8, ui::ArmyCapChoices()[std::clamp(customSlot.armyCap, 0, kArmyCapCount - 1)]);
            combo(
                9, ui::TechLevelLabel(customSlot.techLevel, true), customSlot.techLevel < 0 ? col::TextDim : col::Text);
            combo(3, "");
            const UiRect colorCellRect = CustomCell(layout, spotIndex, 3);
            renderer.FillRectScreen(
                colorCellRect.x + 6,
                colorCellRect.y + 6,
                colorCellRect.w - 28,
                colorCellRect.h - 12,
                ArmyColor(customSlot.color));
            renderer.DrawRectScreen(
                colorCellRect.x + 6,
                colorCellRect.y + 6,
                colorCellRect.w - 28,
                colorCellRect.h - 12,
                col::BorderStrong);
        }
        const UiRect previewRect = CustomPreviewArea(layout);
        int previewWidth = 0, previewHeight = 0;
        SDL_Texture *previewTexture = m_mapPreview.Get(
            renderer.GetSdlRenderer(), textureManager, mapInfo.path, mapInfo.path, {}, previewWidth, previewHeight);
        DrawMapPreviewIn(renderer, previewTexture, previewWidth, previewHeight, previewRect);
        // The start positions with their numbers, in the colour of the player who takes each
        if (m_spotsPath != mapInfo.path)
        {
            m_spotsPath = mapInfo.path;
            m_spots = MissionController::StartPositions(mapInfo.path);
        }
        if (mapInfo.width > 0 && mapInfo.height > 0)
        {
            for (int spotIndex = 0; spotIndex < static_cast<int>(m_spots.size()); ++spotIndex)
            {
                SDL_Color spotColor = col::TextDim;
                for (const CustomSlot &customSlot : m_customSlots)
                    if (customSlot.position == spotIndex)
                        spotColor = ArmyColor(customSlot.color);
                int spotX = 0, spotY = 0;
                SpotMarkerPos(
                    previewRect,
                    mapInfo.width,
                    mapInfo.height,
                    m_spots[spotIndex].first,
                    m_spots[spotIndex].second,
                    spotX,
                    spotY);
                const bool blockedSpot = ((m_customBlocked >> spotIndex) & 1u) != 0;
                renderer.FillRectScreen(
                    spotX - 8, spotY - 8, 16, 16, blockedSpot ? SDL_Color {40, 20, 20, 255} : spotColor);
                renderer.DrawRectScreen(spotX - 8, spotY - 8, 16, 16, blockedSpot ? col::Danger : col::Text);
                if (blockedSpot)
                {
                    renderer.DrawLineScreen(spotX - 7, spotY - 7, spotX + 7, spotY + 7, col::Danger);
                    renderer.DrawLineScreen(spotX - 7, spotY + 7, spotX + 7, spotY - 7, col::Danger);
                }
                fontRenderer.DrawString(
                    std::to_string(spotIndex + 1),
                    spotX,
                    spotY,
                    ui::FONT_SMALL_BOLD,
                    col::AccentText,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
            }
        }
        const std::string sizeText = std::to_string(mapInfo.width) + " x " + std::to_string(mapInfo.height);
        DrawTextSection(
            renderer,
            fontRenderer,
            layout.fogCheckbox.x,
            layout.speedCheckbox.Bottom() + 10,
            layout.fogCheckbox.w,
            _("MAP SIZE:"),
            sizeText,
            col::Ok,
            ui::FONT_BODY);
        ui::DrawCheckbox(
            renderer,
            fontRenderer,
            layout.baseCheckbox,
            _("START WITH A BASE"),
            m_skirmishPrebuilt,
            layout.baseCheckbox.Contains(m_mouseX, m_mouseY));
        ui::DrawCheckbox(
            renderer,
            fontRenderer,
            layout.fogCheckbox,
            _("FOG OF WAR"),
            m_skirmishFogOfWar,
            layout.fogCheckbox.Contains(m_mouseX, m_mouseY));
        ui::DrawCheckbox(
            renderer,
            fontRenderer,
            layout.speedCheckbox,
            _("ALLOW SPEED CONTROL"),
            m_skirmishAllowSpeed,
            layout.speedCheckbox.Contains(m_mouseX, m_mouseY));
    }
    else if (!m_customGame && selected >= 0 && selected < itemCount)
    {
        const auto &missionInfo = missions[selected];
        const UiRect &detailsRect = layout.details;
        ui::DrawPanel(renderer, detailsRect, col::PanelSunken, col::Border);

        int contentX = detailsRect.x + 20;
        int contentWidth = detailsRect.w - 40;
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, _(missionInfo.name), ui::FONT_H2, contentWidth),
            contentX,
            detailsRect.y + 18,
            ui::FONT_H2,
            col::Accent,
            true);
        fontRenderer.DrawString(
            _(missionInfo.mapName), contentX, detailsRect.y + 50, ui::FONT_SMALL, col::TextMuted, false);
        renderer.FillRectScreen(contentX, detailsRect.y + 74, contentWidth, 1, col::Border);

        int sectionY = DrawTextSection(
            renderer,
            fontRenderer,
            contentX,
            detailsRect.y + 90,
            contentWidth,
            _("OBJECTIVES:"),
            _(missionInfo.objective),
            col::Ok,
            ui::FONT_BODY);
        DrawTextSection(
            renderer,
            fontRenderer,
            contentX,
            sectionY,
            contentWidth,
            _("BRIEFING:"),
            _(missionInfo.briefing),
            col::TextDim,
            ui::FONT_SMALL);

        // The mission's map with its preplaced bases: Resistance red, Confederation blue
        const MissionData &missionData = Missions::Campaign(PlayerFaction())[selected].data;
        std::vector<PreviewMarker> markers;
        for (const MissionEntity &missionEntity : missionData.entities)
        {
            if (missionEntity.type >= 15)
                continue;
            markers.push_back(
                {missionEntity.x,
                 missionEntity.y,
                 missionEntity.type >= 8 ? SDL_Color {230, 70, 60, 255} : SDL_Color {70, 150, 240, 255}});
        }
        int previewWidth = 0, previewHeight = 0;
        // Each side has its own campaign: the key keeps their pictures apart
        const std::string previewKey = std::string("mission:") + (PlayerFaction() == Faction::Confederation ? "c" : "r")
                                       + std::to_string(selected);
        SDL_Texture *previewTexture = m_mapPreview.Get(
            renderer.GetSdlRenderer(),
            textureManager,
            previewKey,
            LoaderResources::MissionMapPath(missionData.mapFile),
            markers,
            previewWidth,
            previewHeight);
        DrawMapPreview(renderer, previewTexture, previewWidth, previewHeight, detailsRect);
    }

    // Side and difficulty: drop-down lists
    {
        const SDL_Color sideColor
            = PlayerFaction() == Faction::Confederation ? SDL_Color {64, 150, 226, 255} : SDL_Color {214, 72, 56, 255};
        ui::DrawComboField(
            renderer,
            fontRenderer,
            layout.sideBtn[0],
            CampaignComboOptions(kComboSide)[CampaignComboCurrent(kComboSide)],
            m_combo.open == kComboSide,
            layout.sideBtn[0].Contains(m_mouseX, m_mouseY),
            true,
            sideColor);
        ui::DrawComboField(
            renderer,
            fontRenderer,
            layout.difficultyBtn,
            std::string(_("DIFFICULTY")) + ": " + DifficultyName(m_difficulty),
            m_combo.open == kComboDifficulty,
            layout.difficultyBtn.Contains(m_mouseX, m_mouseY));
    }
    ui::DrawButton(
        renderer,
        fontRenderer,
        layout.editorBtn,
        _("MAP EDITOR"),
        ui::ButtonStyle::Secondary,
        layout.editorBtn.Contains(m_mouseX, m_mouseY));
    if (!m_editorNotice.empty())
    {
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, m_editorNotice, ui::FONT_SMALL, layout.dialog.w - 280),
            layout.dialog.Right() - 20,
            layout.dialog.y + 26,
            ui::FONT_SMALL,
            col::Danger,
            false,
            Anchor::RIGHT | Anchor::VCENTER);
    }
    ui::DrawButton(
        renderer,
        fontRenderer,
        layout.startBtn,
        m_customGame ? _("START GAME") : _("START MISSION"),
        ui::ButtonStyle::Primary,
        layout.startBtn.Contains(m_mouseX, m_mouseY));
    ui::DrawButton(
        renderer,
        fontRenderer,
        layout.backBtn,
        _("BACK"),
        ui::ButtonStyle::Secondary,
        layout.backBtn.Contains(m_mouseX, m_mouseY));
    if (m_combo.open >= kComboSide)
    { // the open list, over everything
        const int comboId = m_combo.open;
        const UiRect fieldRect
            = comboId >= kComboSlot
                  ? CustomCell(layout, (comboId - kComboSlot) / 10, (comboId - kComboSlot) % 10)
                  : (comboId >= kComboMapPlayers && comboId <= kComboMapSort
                         ? MapFilterBtn(layout, comboId - kComboMapPlayers)
                     : comboId == kComboSide    ? layout.sideBtn[0]
                     : comboId == kComboCredits ? layout.creditsBtn
                     : comboId == kComboTime    ? layout.timeBtn
                     : comboId == kComboTech    ? layout.techBtn
                                                : layout.difficultyBtn);
        std::vector<std::string> items = CampaignComboOptions(comboId);
        std::vector<bool> taken;
        std::vector<SDL_Color> swatches;
        const bool hasColorSwatches = comboId >= kComboSlot && (comboId - kComboSlot) % 10 == 3;
        if (hasColorSwatches)
        {
            taken = CustomTakenColors(CustomSlotAt((comboId - kComboSlot) / 10));
            const int current = CampaignComboCurrent(comboId);
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
            fieldRect,
            items,
            CampaignComboCurrent(comboId),
            m_combo,
            windowHeight,
            m_mouseX,
            m_mouseY,
            hasColorSwatches ? &taken : nullptr,
            hasColorSwatches ? &swatches : nullptr);
    }
}

std::vector<std::string> MenuController::CampaignComboOptions(int comboId) const
{
    if (comboId >= kComboSlot)
    {
        const int row = (comboId - kComboSlot) / 10, kind = (comboId - kComboSlot) % 10;
        std::vector<std::string> options;
        if (kind == 0)
        {
            for (int code : CustomSeatCodes(row))
                options.push_back(
                    code == 1   ? std::string(CustomSlotAt(row) == 0 ? _("YOU") : _("Occupy"))
                    : code == 2 ? std::string(_("Open"))
                    : code == 7 ? std::string(_("Blocked"))
                                : std::string(_("AI")) + " - " + DifficultyName(static_cast<AIDifficulty>(code - 3)));
        }
        else if (kind == 1)
        {
            options = {_("Random"), _("Resistance"), _("Confederation")};
        }
        else if (kind == 2)
        {
            for (int i = 1; i <= kMaxPlayers; ++i)
                options.push_back(std::string(_("Team")) + " " + std::to_string(i));
        }
        else if (kind == 3)
        {
            for (int i = 1; i <= Settings::kArmyColors; ++i)
                options.push_back(_(ArmyColorName(i)));
        }
        else if (kind == 7)
        {
            options = {_("None"), "-25%", "-50%", "+25%", "+50%", "+100%"};
        }
        else if (kind == 9)
        {
            options = ui::TechLevelChoices(true);
        }
        else
        {
            options = ui::ArmyCapChoices();
        }
        return options;
    }
    if (comboId == kComboMapPlayers)
    {
        std::vector<std::string> options;
        for (int playerCount : MapPlayerCounts())
            options.push_back(
                std::string(_("Players")) + ": "
                + (playerCount == 0 ? std::string(_("Any")) : std::to_string(playerCount)));
        return options;
    }
    if (comboId == kComboMapSize)
    {
        const char *sizes[] = {_("Any"), _("Small"), _("Medium"), _("Large")};
        std::vector<std::string> options;
        for (const char *sizeLabel : sizes)
            options.push_back(std::string(_("Size")) + ": " + sizeLabel);
        return options;
    }
    if (comboId == kComboMapSort)
    {
        const char *sorts[] = {_("Name"), _("Players"), _("Size")};
        std::vector<std::string> options;
        for (const char *sortLabel : sorts)
            options.push_back(std::string(_("Sort")) + ": " + sortLabel);
        return options;
    }
    if (comboId == kComboSide)
        return {_("Resistance"), _("Confederation"), _("Random")};
    if (comboId == kComboDifficulty)
    {
        return {
            DifficultyName(AIDifficulty::Easy),
            DifficultyName(AIDifficulty::Medium),
            DifficultyName(AIDifficulty::Hard),
            DifficultyName(AIDifficulty::Hardcore)};
    }
    if (comboId == kComboCredits)
        return ui::CreditsChoices();
    if (comboId == kComboTime)
        return ui::TimeOfDayChoices();
    if (comboId == kComboTech)
        return ui::TechLevelChoices(false);
    return ui::LayoutChoices();
}

int MenuController::CampaignComboCurrent(int comboId) const
{
    if (comboId >= kComboSlot)
    {
        const int row = (comboId - kComboSlot) / 10, kind = (comboId - kComboSlot) % 10;
        const int slotIndex = CustomSlotAt(row);
        if (kind == 0)
        {
            const std::vector<int> codes = CustomSeatCodes(row);
            const int currentCode = slotIndex == 0
                                        ? 1
                                        : (slotIndex > 0 ? 3 + std::clamp(m_customSlots[slotIndex].level, 0, 3)
                                                         : (((m_customBlocked >> row) & 1u) ? 7 : 2));
            for (std::size_t i = 0; i < codes.size(); ++i)
                if (codes[i] == currentCode)
                    return static_cast<int>(i);
            return 0;
        }
        if (slotIndex < 0)
            return 0;
        const CustomSlot &customSlot = m_customSlots[slotIndex];
        if (kind == 1)
            return slotIndex == 0 ? (m_sideRandom ? 0 : (PlayerFaction() == Faction::Confederation ? 2 : 1))
                                  : customSlot.faction;
        if (kind == 2)
            return customSlot.team;
        if (kind == 3)
            return customSlot.color - 1;
        if (kind == 7)
            return std::clamp(customSlot.handicap, 0, kHandicapCount - 1);
        if (kind == 9)
            return std::max(0, customSlot.techLevel + 1);
        return std::clamp(customSlot.armyCap, 0, kArmyCapCount - 1);
    }
    if (comboId == kComboMapPlayers)
    {
        const std::vector<int> counts = MapPlayerCounts();
        const auto foundIt = std::find(counts.begin(), counts.end(), m_mapPlayersFilter);
        return foundIt == counts.end() ? 0 : static_cast<int>(foundIt - counts.begin());
    }
    if (comboId == kComboMapSize)
        return std::clamp(m_mapSizeFilter, 0, 3);
    if (comboId == kComboMapSort)
        return std::clamp(m_mapSort, 0, 2);
    if (comboId == kComboSide)
        return m_sideRandom ? 2 : (PlayerFaction() == Faction::Confederation ? 1 : 0);
    if (comboId == kComboDifficulty)
        return std::clamp(static_cast<int>(m_difficulty), 0, 3);
    if (comboId == kComboTime)
        return m_skirmishNight ? 1 : 0;
    if (comboId == kComboTech)
        return std::max(0, m_skirmishTechLevel + 1);
    return std::clamp(m_skirmishCredits, 0, 4);
}

void MenuController::CampaignComboApply(int comboId, int index)
{
    if (comboId >= kComboSlot)
    {
        const int row = (comboId - kComboSlot) / 10, kind = (comboId - kComboSlot) % 10;
        if (kind == 0)
        {
            const std::vector<int> codes = CustomSeatCodes(row);
            if (index >= 0 && index < static_cast<int>(codes.size()))
                SetCustomSeat(row, codes[index]);
            return;
        }
        const int slotIndex = CustomSlotAt(row);
        if (slotIndex < 0)
            return;
        CustomSlot &customSlot = m_customSlots[slotIndex];
        if (kind == 1)
        {
            if (slotIndex == 0)
            {
                m_sideRandom = index == 0;
                if (index != 0)
                    SetPlayerFaction(index == 2 ? Faction::Confederation : Faction::Resistance);
            }
            else
            {
                customSlot.faction = index;
            }
        }
        else if (kind == 2)
        {
            customSlot.team = std::clamp(index, 0, kMaxPlayers - 1);
        }
        else if (kind == 3)
        {
            const std::vector<bool> taken = CustomTakenColors(slotIndex);
            if (index >= 0 && index < static_cast<int>(taken.size()) && !taken[index])
                customSlot.color = index + 1;
        }
        else if (kind == 7)
        {
            customSlot.handicap = std::clamp(index, 0, kHandicapCount - 1);
        }
        else if (kind == 9)
        {
            customSlot.techLevel = std::clamp(index - 1, -1, static_cast<int>(Capabilities::Levels().size()) - 1);
        }
        else
        {
            customSlot.armyCap = std::clamp(index, 0, kArmyCapCount - 1);
        }
        return;
    }
    if (comboId >= kComboMapPlayers && comboId <= kComboMapSort)
    {
        if (comboId == kComboMapPlayers)
        {
            const std::vector<int> counts = MapPlayerCounts();
            m_mapPlayersFilter = counts[std::clamp(index, 0, static_cast<int>(counts.size()) - 1)];
        }
        else if (comboId == kComboMapSize)
        {
            m_mapSizeFilter = std::clamp(index, 0, 3);
        }
        else
        {
            m_mapSort = std::clamp(index, 0, 2);
        }
        RebuildMapList();
    }
    else if (comboId == kComboSide)
    {
        m_sideRandom = index == 2;
        if (index != 2)
            SetPlayerFaction(index == 1 ? Faction::Confederation : Faction::Resistance);
    }
    else if (comboId == kComboDifficulty)
    {
        if (!m_settings)
            return;
        m_settings->difficulty = std::clamp(index, 0, 3);
        m_difficulty = static_cast<AIDifficulty>(m_settings->difficulty);
        if (m_onSettingsChanged)
            m_onSettingsChanged(SettingsArea::Game);
    }
    else if (comboId == kComboCredits)
    {
        m_skirmishCredits = std::clamp(index, 0, 4);
    }
    else if (comboId == kComboTime)
    {
        m_skirmishNight = index == 1;
    }
    else if (comboId == kComboTech)
    {
        m_skirmishTechLevel = std::clamp(index - 1, -1, static_cast<int>(Capabilities::Levels().size()) - 1);
    }
}

} // namespace OpenArtOfWar
