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

constexpr int kPauseItems = 6;

// Pause menu
struct PauseLayout
{
    UiRect dialog;
    UiRect items[kPauseItems];
};

PauseLayout pauseLayout(int windowW, int windowH)
{
    PauseLayout l;
    // ui/layout.yaml (screen pause); the numbers below stand in when the file has no such screen
    if (uilayout::Current().Has("pause", "pc"))
    {
        const uilayout::Solved s = uilayout::Current().Solve("pause", "pc", windowW, windowH, {});
        l.dialog = s.Rect("dialog");
        for (int i = 0; i < kPauseItems; ++i)
            l.items[i] = s.Rect("item" + std::to_string(i));
        return l;
    }
    const int itemH = 46;
    const int gap = 8;
    int h = 52 + 20 + kPauseItems * itemH + (kPauseItems - 1) * gap + 24;
    l.dialog = ui::CenteredRect(windowW, windowH, 380, h);
    for (int i = 0; i < kPauseItems; ++i)
    {
        l.items[i] = {l.dialog.x + 20, l.dialog.y + 72 + i * (itemH + gap), l.dialog.w - 40, itemH};
    }
    return l;
}

UiRect BriefingRect(int windowW, int windowH)
{
    return ui::CenteredRect(windowW, windowH, 820, 480);
}

UiRect ObjectivesRect(int windowW, int windowH)
{
    return ui::CenteredRect(windowW, windowH, 680, 480);
}

// The mission dialogue window: as large as the text needs, within the screen
UiRect MissionDialogRect(int windowW, int windowH)
{
    const int w = std::min(780, windowW - 40);
    const int h = std::min(560, windowH - 40);
    return ui::CenteredRect(windowW, windowH, w, h);
}

} // namespace

bool MenuController::UpdateInGameMenu(const FrameContext &ctx, int mx, int my, bool leftClicked)
{
    InputManager &input = ctx.input;
    const int windowW = ctx.windowW;
    const int windowH = ctx.windowH;
    SoundEngine &sound = ctx.sound;
    MissionController &mission = ctx.mission;
    TileMap &map = ctx.map;
    EntityManager &entities = ctx.entities;
    Economy &economy = ctx.economy;
    Pathfinder &pathfinder = ctx.pathfinder;
    Camera &camera = ctx.camera;
    AppState &state = ctx.state;
    if (!m_inGameMenuOpen)
        return false;
    m_mouseX = mx;
    m_mouseY = my;

    if (m_inGameSettingsOpen)
    {
        UpdateSettingsScreen(input, mx, my, leftClicked, windowW, windowH, state);
        return true;
    }
    // A question (leave the game, restart) is open: nothing else reacts until it is answered
    int confirmAnswer = -1;
    if (m_confirm.open)
    {
        confirmAnswer = ui::ConfirmUpdate(
            m_confirm,
            windowW,
            windowH,
            mx,
            my,
            leftClicked,
            input.IsConfirmJustPressed(),
            input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE));
        if (confirmAnswer < 0)
            return true;
        m_confirm.open = false;
    }
    if (confirmAnswer < 0 && input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        m_inGameMenuOpen = false;
        return true;
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_UP))
    {
        m_inGameMenuSelected = (m_inGameMenuSelected - 1 + kPauseItems) % kPauseItems;
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_DOWN))
    {
        m_inGameMenuSelected = (m_inGameMenuSelected + 1) % kPauseItems;
    }

    auto activate = [&](int item, bool confirmed = false) {
        // Leaving or restarting a game loses its progress: ask first
        if ((item == 4 || item == 5) && !confirmed)
        {
            if (item == 4 && mission.IsOnlineMatch())
                return;
            m_confirm.open = true;
            m_confirm.modalResult = -1;
            SoundRequests::Play("ui_show_dialog");
            m_confirm.action = item;
            if (item == 4)
            {
                m_confirm.title = _("RESTART THE GAME?");
                m_confirm.text = _("The mission starts over: the progress made so far is lost.");
            }
            else if (mission.IsOnlineMatch())
            {
                m_confirm.title = _("LEAVE THE MATCH?");
                m_confirm.text
                    = _("You leave the match and return to the main menu. The other players go on without you.");
            }
            else
            {
                m_confirm.title = _("LEAVE THE GAME?");
                m_confirm.text = _("You return to the main menu: the progress of this game is lost.");
            }
            return;
        }
        switch (item)
        {
            case 0: // Continue
                m_inGameMenuOpen = false;
                break;
            case 1: // Objectives
                SetObjectivesDialogOpen(true);
                m_inGameMenuOpen = false;
                break;
            case 2: // Settings, same screen as in the main menu
                m_inGameSettingsOpen = true;
                SoundRequests::Play("ui_show_dialog");
                m_settingsSelected = 0;
                m_settingsTop = 0;
                break;
            case 3: // Sound & Music
                sound.ToggleMute();
                if (m_settings && m_onSettingsChanged)
                {
                    m_settings->muted = sound.IsMuted();
                    m_onSettingsChanged(SettingsArea::Audio);
                }
                break;
            case 4: // Restart mission
                if (!mission.IsOnlineMatch())
                {
                    mission.Restart(map, entities, economy, pathfinder);
                    camera.CenterOn(mission.GetPlayerHqX() * TILE_WIDTH, mission.GetPlayerHqY() * TILE_HEIGHT);
                }
                m_inGameMenuOpen = false;
                break;
            case 5: // Main menu
                state = AppState::MainMenu;
                m_inGameMenuOpen = false;
                m_inGameSettingsOpen = false;
                m_confirm = {};
                m_combo = {};
                SetObjectivesDialogOpen(false);
                m_missionDialogOpen = false;
                break;
        }
    };

    if (confirmAnswer >= 0)
    {
        if (confirmAnswer == 1)
            activate(m_confirm.action, true);
        return true;
    }
    if (UpdateMusicPlayer(
            leftClicked, false, 0, 0, input.IsLeftMouseDown(), mx, my, windowH, false, sound, m_scrollDrag))
        return true;
    PauseLayout l = pauseLayout(windowW, windowH);
    for (int i = 0; i < kPauseItems; ++i)
    {
        if (l.items[i].Contains(mx, my))
        {
            m_inGameMenuSelected = i;
            if (leftClicked)
            {
                activate(i);
                return true;
            }
        }
    }
    if (input.IsConfirmJustPressed())
    {
        activate(m_inGameMenuSelected);
    }
    return true;
}

bool MenuController::UpdateObjectivesDialog(InputManager &input, bool leftClicked)
{
    if (!m_objectivesDialogOpen)
        return false;

    if (leftClicked || input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE) || input.IsConfirmJustPressed())
    {
        m_objectivesDialogOpen = false;
    }
    return true;
}

bool MenuController::UpdateMissionDialog(
    InputManager &input, int mx, int my, bool leftClicked, int windowW, int windowH)
{
    if (!m_missionDialogOpen)
        return false;
    m_mouseX = mx;
    m_mouseY = my;
    const UiRect btn = DialogFooterButton(MissionDialogRect(windowW, windowH), 200);
    if ((leftClicked && btn.Contains(mx, my)) || input.IsConfirmJustPressed()
        || input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        m_missionDialogOpen = false;
        SoundRequests::Play("ui_confirm");
    }
    return true;
}

void MenuController::RenderBriefing(
    Renderer &renderer, BitmapFontRenderer &fontRenderer, const MissionController &mission, int windowW, int windowH)
{
    renderer.FillRectScreen(0, 0, windowW, windowH, col::Backdrop);
    UiRect d = BriefingRect(windowW, windowH);
    std::string title = mission.GetMissionName().empty() ? std::string(_("MISSION BRIEFING"))
                                                         : std::string(_(mission.GetMissionName()));
    UiRect content = ui::DrawDialog(renderer, fontRenderer, windowW, windowH, d, title);

    int y = DrawTextSection(
        renderer,
        fontRenderer,
        content.x,
        content.y,
        content.w,
        _("OBJECTIVES:"),
        _(mission.GetObjectiveText()),
        col::Ok,
        ui::FONT_BODY);
    DrawTextSection(
        renderer,
        fontRenderer,
        content.x,
        y,
        content.w,
        _("BRIEFING:"),
        _(mission.GetBriefingText()),
        col::Text,
        ui::FONT_BODY);

    UiRect start = DialogFooterButton(d, 220);
    fontRenderer.DrawString(
        _("Click or press Enter to begin"),
        d.x + 20,
        start.CenterY(),
        ui::FONT_SMALL,
        col::TextMuted,
        false,
        Anchor::LEFT | Anchor::VCENTER);
    ui::DrawButton(
        renderer,
        fontRenderer,
        start,
        _("START MISSION"),
        ui::ButtonStyle::Primary,
        start.Contains(m_mouseX, m_mouseY));
}

void MenuController::RenderInGamePauseMenu(
    Renderer &renderer, BitmapFontRenderer &fontRenderer, const SoundEngine &sound, int windowW, int windowH)
{
    PauseLayout l = pauseLayout(windowW, windowH);
    ui::DrawDialog(renderer, fontRenderer, windowW, windowH, l.dialog, _("GAME PAUSED"));

    std::string soundLabel = std::string(_("Sound & Music")) + ": " + (sound.IsMuted() ? _("OFF") : _("ON"));
    std::string items[kPauseItems]
        = {_("CONTINUE"), _("OBJECTIVES"), _("SETTINGS"), soundLabel, _("RESTART MISSION"), _("MAIN MENU")};

    for (int i = 0; i < kPauseItems; ++i)
    {
        bool hot = (i == m_inGameMenuSelected) || l.items[i].Contains(m_mouseX, m_mouseY);
        if (l.items[i].Contains(m_mouseX, m_mouseY))
            SoundRequests::MarkHover();
        ui::ButtonStyle style = (i == 0)
                                    ? ui::ButtonStyle::Primary
                                    : (i == kPauseItems - 1 ? ui::ButtonStyle::Danger : ui::ButtonStyle::Secondary);
        ui::DrawButton(renderer, fontRenderer, l.items[i], items[i], style, hot);
    }
    RenderMusicPlayer(renderer, fontRenderer, sound, windowH, false, m_mouseX, m_mouseY);
    ui::DrawConfirm(renderer, fontRenderer, windowW, windowH, m_confirm, m_mouseX, m_mouseY);
}

void MenuController::RenderMissionDialog(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH)
{
    if (!m_missionDialogOpen)
        return;
    const UiRect d = MissionDialogRect(windowW, windowH);
    const UiRect content = ui::DrawDialog(renderer, fontRenderer, windowW, windowH, d, m_missionDialog.title);
    const UiRect btn = DialogFooterButton(d, 200);
    const int bottom = btn.y - 12;
    // Speaker in the accent colour, the line below it; body text gets smaller when the dialogue is long
    int scale = ui::FONT_BODY;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        int h = 0;
        for (const DialogLine &line : m_missionDialog.lines)
        {
            // Estimate of the wrapped height (word wrap leaves some room at the line ends)
            const int width = fontRenderer.GetTextWidth(line.text, scale);
            const int rows = 1 + width / std::max(1, content.w * 9 / 10);
            h += 22 + rows * fontRenderer.GetTextHeight(scale) + 14;
        }
        if (content.y + h <= bottom)
            break;
        scale = ui::FONT_SMALL;
    }
    int y = content.y;
    for (const DialogLine &line : m_missionDialog.lines)
    {
        if (y >= bottom)
            break;
        if (!line.speaker.empty())
            fontRenderer.DrawString(line.speaker, content.x, y, ui::FONT_SMALL_BOLD, col::Accent, false);
        y += 22;
        y += fontRenderer.DrawTextWrapped(line.text, content.x, y, content.w, scale, col::Text, false) + 14;
    }
    ui::DrawButton(
        renderer, fontRenderer, btn, _("ACKNOWLEDGED"), ui::ButtonStyle::Primary, btn.Contains(m_mouseX, m_mouseY));
}

void MenuController::RenderObjectivesDialog(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const MissionController &mission,
    AIDifficulty difficulty,
    int windowW,
    int windowH)
{
    UiRect d = ObjectivesRect(windowW, windowH);
    std::string title = _(mission.GetMissionName()) + _(" - OBJECTIVES");
    UiRect content = ui::DrawDialog(renderer, fontRenderer, windowW, windowH, d, title);

    // Campaign: the difficulty the computer plays at (a match and a custom game show theirs in their own setup)
    const int missionId = mission.GetCurrentMissionId();
    const bool campaign = !mission.IsMatch() && !mission.IsSkirmish() && missionId >= 0
                          && missionId < MissionController::kCampaignMissionCount;
    if (campaign)
    {
        ui::DrawCaption(fontRenderer, renderer, content.x, content.y, _("DIFFICULTY"));
        fontRenderer.DrawString(
            DifficultyName(difficulty),
            content.x + 9 + fontRenderer.GetTextWidth(_("DIFFICULTY"), ui::FONT_SMALL_BOLD) + 12,
            content.y - 2,
            ui::FONT_BODY_BOLD,
            col::Accent,
            false);
        content.y += 30;
    }

    int y = DrawTextSection(
        renderer,
        fontRenderer,
        content.x,
        content.y,
        content.w,
        _("PRIMARY OBJECTIVES:"),
        _(mission.GetObjectiveText()),
        col::Ok,
        ui::FONT_BODY);
    DrawTextSection(
        renderer,
        fontRenderer,
        content.x,
        y,
        content.w,
        _("OPERATION BRIEFING:"),
        _(mission.GetBriefingText()),
        col::TextDim,
        ui::FONT_SMALL);

    UiRect btn = DialogFooterButton(d);
    ui::DrawButton(
        renderer, fontRenderer, btn, _("CONTINUE"), ui::ButtonStyle::Primary, btn.Contains(m_mouseX, m_mouseY));
}

} // namespace OpenArtOfWar
