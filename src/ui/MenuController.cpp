#include "ui/UiLayout.hpp"
#include "ui/MenuController.hpp"
#include "MenuControllerInternal.hpp"
#include "core/JsonFile.hpp"
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

// ---------------------------------------------------------------------------
// Shared geometry: the same rectangles drive rendering and mouse hit-testing
// ---------------------------------------------------------------------------

constexpr int kMainMenuItems = 10;

const char *const kMainMenuLabels[kMainMenuItems]
    = {"CAMPAIGN", "SKIRMISH", "NETWORK GAME", "OPEN REPLAY", "TEST MAP", "MODS", "SETTINGS", "HELP", "ABOUT", "EXIT"};

int MainMenuColumnX(int windowW)
{
    return std::clamp(windowW / 14, 32, 120);
}

int MainMenuTitleY(int windowH)
{
    return std::max(36, windowH / 9);
}

UiRect MainMenuItemRect(int i, int windowW, int windowH)
{
    // ui/layout.yaml (screen main_menu); the numbers below stand in when the file has no such screen
    const uilayout::Layout &file = uilayout::Current();
    if (file.Has("main_menu", "pc"))
        return file.Solve("main_menu", uilayout::MenuProfile(file, "main_menu"), windowW, windowH, {})
            .Rect("item" + std::to_string(i));
    const int itemH = 44;
    const int gap = 6;
    int startY = MainMenuTitleY(windowH) + 128;
    int maxStart = windowH - 56 - kMainMenuItems * (itemH + gap);
    startY = std::max(MainMenuTitleY(windowH) + 96, std::min(startY, maxStart));
    return {MainMenuColumnX(windowW), startY + i * (itemH + gap), 340, itemH};
}

UiRect InfoDialogRect(int windowW, int windowH)
{
    return ui::CenteredRect(windowW, windowH, 700, 500);
}

[[maybe_unused]] std::string DifficultyDesc(AIDifficulty d)
{
    switch (d)
    {
        case AIDifficulty::Easy:
            return _("-50% AI Resources");
        case AIDifficulty::Medium:
            return _("-20% AI Resources");
        case AIDifficulty::Hard:
            return _("Fair AI, No Handicap");
        case AIDifficulty::Hardcore:
            return _("+30% Resources, +10% Speed");
    }
    return {};
}

[[maybe_unused]] std::string OnOff(bool on)
{
    return on ? _("ON") : _("OFF");
}

} // namespace

void MenuController::ActivateMenuItem(
    int index,
    AppState &state,
    bool &running,
    [[maybe_unused]] SoundEngine &sound,
    MissionController &mission,
    TileMap &map,
    EntityManager &entities,
    Economy &economy,
    Pathfinder &pathfinder,
    Camera &camera)
{
    switch (index)
    {
        case 0: // Campaign: the single-player missions only
            SetCampaignSelectOpen(true);
            m_customGame = false;
            m_campaignMissionSelected = 0;
            break;
        case 1: // Custom game: any map of assets/maps, side picked in the dialog
            m_skirmishAll = MissionController::ListSkirmishMaps();
            m_mapSearch.clear();
            m_mapSearchFocus = false;
            m_mapPlayersFilter = m_mapSizeFilter = m_mapSort = 0;
            m_skirmishSelected = -1;
            RebuildMapList();
            SetCampaignSelectOpen(!m_skirmishAll.empty());
            m_customGame = true;
            m_skirmishAllowSpeed = false;
            m_customSlots.clear();
            m_customBlocked = 0;
            m_customOpen = 0;
            m_customSpectator = false;
            m_skirmishSelected = 0;
            m_skirmishTop = 0;
            break;
        case 2: // Network game: local network / internet, then the game room
            state = AppState::MenuNetwork;
            break;
        case 3: // Open replay: the recorded network games
            state = AppState::MenuReplay;
            break;
        case 4: // Test map
            mission.LoadTestMap(map, entities, economy, pathfinder);
            state = AppState::CampaignPlaying;
            camera.CenterOn((mission.GetPlayerHqX() + 12) * TILE_WIDTH, mission.GetPlayerHqY() * TILE_HEIGHT);
            break;
        case 5: // Mods: the data packages in use
            state = AppState::MenuMods;
            break;
        case 6:
            state = AppState::MenuSettings;
            break;
        case 7:
            state = AppState::MenuHelp;
            break;
        case 8:
            state = AppState::MenuAbout;
            break;
        case 9:
            running = false;
            break;
        default:
            break;
    }
}

void MenuController::UpdateMainMenu(const FrameContext &ctx, bool &running)
{
    InputManager &input = ctx.input;
    const int windowW = ctx.windowW;
    const int windowH = ctx.windowH;
    AppState &state = ctx.state;
    SoundEngine &sound = ctx.sound;
    MissionController &mission = ctx.mission;
    TileMap &map = ctx.map;
    EntityManager &entities = ctx.entities;
    Economy &economy = ctx.economy;
    Pathfinder &pathfinder = ctx.pathfinder;
    Camera &camera = ctx.camera;
    int mx = input.GetMouseScreenX();
    int my = input.GetMouseScreenY();
    m_mouseX = mx;
    m_mouseY = my;
    float lx, ly;
    bool clicked = input.WasLeftClickIssued(lx, ly);

    if (m_campaignSelectOpen)
    {
        UpdateCampaignSelect(ctx, mx, my, clicked);
        return;
    }

    if (input.IsKeyJustPressed(SDL_SCANCODE_UP))
    {
        m_selectedMenuItem = (m_selectedMenuItem - 1 + kMainMenuItems) % kMainMenuItems;
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_DOWN))
    {
        m_selectedMenuItem = (m_selectedMenuItem + 1) % kMainMenuItems;
    }

    int px = 0, py = 0;
    const bool pressed = input.ConsumeLeftPress(px, py);
    if (UpdateMusicPlayer(
            clicked, pressed, px, py, input.IsLeftMouseDown(), mx, my, windowH, true, sound, m_scrollDrag))
        return;

    for (int i = 0; i < kMainMenuItems; ++i)
    {
        if (MainMenuItemRect(i, windowW, windowH).Contains(mx, my))
        {
            m_selectedMenuItem = i;
            if (clicked)
            {
                ActivateMenuItem(i, state, running, sound, mission, map, entities, economy, pathfinder, camera);
                return;
            }
        }
    }

    if (input.IsConfirmJustPressed())
    {
        ActivateMenuItem(
            m_selectedMenuItem, state, running, sound, mission, map, entities, economy, pathfinder, camera);
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        running = false;
    }
}

void MenuController::UpdateInfoScreen(InputManager &input, AppState &state)
{
    m_mouseX = input.GetMouseScreenX();
    m_mouseY = input.GetMouseScreenY();
    float lx, ly;
    bool clicked = input.WasLeftClickIssued(lx, ly);
    int px = 0, py = 0;
    if (input.ConsumeLeftPress(px, py) && input.IsLeftMouseDown() && OnScrollBar(m_infoTrack, px, py))
        m_scrollDrag = 2;
    if (m_scrollDrag == 2)
    {
        if (input.IsLeftMouseDown())
            m_infoScroll = ScrollTopFromMouse(m_infoTrack, m_mouseY, m_infoTotal, m_infoVisible);
        else
            m_scrollDrag = 0;
        return;
    }
    m_infoScroll = std::max(0, m_infoScroll - input.ConsumeWheel() * 3);
    if (input.IsKeyJustPressed(SDL_SCANCODE_UP))
        m_infoScroll = std::max(0, m_infoScroll - 1);
    if (input.IsKeyJustPressed(SDL_SCANCODE_DOWN))
        ++m_infoScroll;
    if (input.IsKeyJustPressed(SDL_SCANCODE_PAGEUP))
        m_infoScroll = std::max(0, m_infoScroll - 10);
    if (input.IsKeyJustPressed(SDL_SCANCODE_PAGEDOWN))
        m_infoScroll += 10;
    if (clicked || input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE) || input.IsConfirmJustPressed())
    {
        state = AppState::MainMenu;
        m_infoScroll = 0;
    }
}

// ---------------------------------------------------------------------------
// Renderers
// ---------------------------------------------------------------------------

void MenuController::RenderSplashLogo(
    Renderer &renderer, Camera &camera, TextureManager &texManager, int windowW, int windowH)
{
    (void) camera;
    renderer.FillRectScreen(0, 0, windowW, windowH, {0, 0, 0, 255});

    SDL_Texture *logoTex = texManager.GetUiTexture("logo");
    if (logoTex)
    {
        int lw = logoTex->w / 2;
        int lh = logoTex->h / 2;
        int lx = (windowW - lw) / 2;
        int ly = (windowH - lh) / 2 - 25;
        renderer.DrawTextureScreen(logoTex, lx, ly, lw, lh);
    }
}

void MenuController::RenderSplashTitle(
    Renderer &renderer, Camera &camera, TextureManager &texManager, int windowW, int windowH)
{
    (void) camera;
    renderer.FillRectScreen(0, 0, windowW, windowH, {10, 12, 18, 255});

    SDL_Texture *titleTex = texManager.GetUiTexture("title");
    if (titleTex)
    {
        int th = std::min(windowH - 120, 600);
        int tw = th * 240 / 323; // the original art's proportions, until the texture tells its own
        float texW = 0.0f, texH = 0.0f;
        if (SDL_GetTextureSize(titleTex, &texW, &texH) && texW > 0.0f && texH > 0.0f)
            tw = static_cast<int>(th * texW / texH); // whatever size the key art has, it is not stretched
        int tx = (windowW - tw) / 2;
        int ty = (windowH - th) / 2;
        renderer.DrawTextureScreen(titleTex, tx, ty, tw, th);
    }
}

void MenuController::RenderMainMenu(
    Renderer &renderer,
    TextureManager &texManager,
    BitmapFontRenderer &fontRenderer,
    const SoundEngine &sound,
    int windowW,
    int windowH)
{
    renderer.FillRectScreen(0, 0, windowW, windowH, col::Backdrop);

    // Key art anchored to the right edge, blended into the dark backdrop
    SDL_Texture *titleTex = texManager.GetUiTexture("title");
    if (titleTex)
    {
        int th = windowH;
        int tw = th * 240 / 323; // the original art's proportions, until the texture tells its own
        float texW = 0.0f, texH = 0.0f;
        if (SDL_GetTextureSize(titleTex, &texW, &texH) && texW > 0.0f && texH > 0.0f)
            tw = static_cast<int>(th * texW / texH); // whatever size the key art has, it is not stretched
        int tx = windowW - tw;
        renderer.DrawTextureScreen(titleTex, tx, 0, tw, th);
        renderer.FillRectScreen(tx, 0, tw, th, {8, 10, 14, 70});
        int fadeW = std::min(tw, 320);
        ui::FillHorizontalGradient(renderer, {tx, 0, fadeW, windowH}, col::Backdrop, ui::WithAlpha(col::Backdrop, 0));
        if (tx > 0)
            renderer.FillRectScreen(0, 0, tx, windowH, col::Backdrop);
        m_backdropFx.render(renderer, texManager, tx, windowW, windowH, SDL_GetTicks());
        // The blasts melt into the key art instead of ending at a hard edge
        if (tx > 0)
            ui::FillHorizontalGradient(
                renderer,
                {std::max(0, tx - 200), 0, std::min(tx, 200), windowH},
                ui::WithAlpha(col::Backdrop, 0),
                col::Backdrop);
    }
    else
    {
        m_backdropFx.render(renderer, texManager, windowW * 55 / 100, windowW, windowH, SDL_GetTicks());
    }
    ui::FillVerticalGradient(
        renderer, {0, windowH - 160, windowW, 160}, ui::WithAlpha(col::Backdrop, 0), col::Backdrop);

    // Title block
    int x = MainMenuColumnX(windowW);
    int titleY = MainMenuTitleY(windowH);
    const uilayout::Layout &layoutFile = uilayout::Current();
    const std::string profile = uilayout::MenuProfile(layoutFile, "main_menu");
    const bool touchMenu = profile == "touch";
    if (layoutFile.Has("main_menu", profile))
    {
        const UiRect t = layoutFile.Solve("main_menu", profile, windowW, windowH, {}).Rect("title");
        x = t.x;
        titleY = t.y;
    }
    fontRenderer.DrawString("OPEN ART OF WAR", x, titleY, ui::FONT_HERO, col::Accent, true);
    fontRenderer.DrawString(_("FREEDOM FOR LIBERTY"), x + 2, titleY + 58, ui::FONT_BODY_BOLD, col::TextDim, false);
    renderer.FillRectScreen(x + 2, titleY + 86, 72, 3, col::Accent);

    // Menu items
    for (int i = 0; i < kMainMenuItems; ++i)
    {
        UiRect r = MainMenuItemRect(i, windowW, windowH);
        if (r.Contains(m_mouseX, m_mouseY))
            SoundRequests::MarkHover();
        bool selected = (i == m_selectedMenuItem);
        bool isExit = (i == kMainMenuItems - 1);
        std::string label = _(kMainMenuLabels[i]);

        // if (selected)
        // {
        //     ui::FillHorizontalGradient(renderer, r, ui::WithAlpha(col::Hover, 250), ui::WithAlpha(col::Hover, 0));
        //     renderer.FillRectScreen(r.x, r.y, 4, r.h, isExit ? col::Danger : col::Accent);
        // }
        SDL_Color textCol = selected ? (isExit ? SDL_Color {255, 190, 180, 255} : col::Text) : col::TextDim;
        if (touchMenu)
        {
            // Touch: the same text lines as on the desktop (no button plate), only bigger and always lit
            textCol = isExit ? SDL_Color {255, 190, 180, 255} : col::Text;
        }
        fontRenderer.DrawString(
            label,
            r.x + 20 + (selected && !touchMenu ? 5 : 0),
            r.CenterY(),
            touchMenu ? ui::FONT_H1 : ui::FONT_H2,
            textCol,
            selected,
            Anchor::LEFT | Anchor::VCENTER);
        if (i == kMainMenuItems - 2)
        {
            // Separator between the game modes / info items and Exit
            renderer.FillRectScreen(r.x + 20, r.Bottom() + 2, 120, 1, col::Border);
        }
    }

    // Footer
    fontRenderer.DrawString(
        std::string(_("Version")) + " " + OAOW_VERSION,
        windowW - 24,
        windowH - 30,
        ui::FONT_SMALL,
        col::TextMuted,
        false,
        Anchor::RIGHT | Anchor::TOP);

    RenderMusicPlayer(renderer, fontRenderer, sound, windowH, true, m_mouseX, m_mouseY);

    if (m_campaignSelectOpen)
    {
        RenderCampaignMissionSelect(renderer, texManager, fontRenderer, windowW, windowH);
    }
}

void MenuController::RenderInfoDialog(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    int windowW,
    int windowH,
    const std::string &title,
    const std::vector<std::string> &lines)
{
    UiRect d = InfoDialogRect(windowW, windowH);
    UiRect content = ui::DrawDialog(renderer, fontRenderer, windowW, windowH, d, title);

    // Long texts scroll: the footer button takes the bottom of the dialog
    const int visible = std::max(1, (DialogFooterButton(d).y - 10 - content.y) / 30);
    const int total = static_cast<int>(lines.size());
    m_infoScroll = std::clamp(m_infoScroll, 0, std::max(0, total - visible));
    int y = content.y;
    for (int i = m_infoScroll; i < total && i < m_infoScroll + visible; ++i)
    {
        if (!lines[i].empty())
        {
            fontRenderer.DrawString(
                ui::FitText(fontRenderer, lines[i], ui::FONT_BODY, content.w - 12),
                content.x,
                y,
                ui::FONT_BODY,
                col::Text,
                false);
        }
        y += 30;
    }
    m_infoTrack = {};
    m_infoTotal = total;
    m_infoVisible = visible;
    if (total > visible)
    {
        m_infoTrack = {content.Right() - 4, content.y, 4, visible * 30};
        const int trackH = visible * 30;
        const int thumbH = std::max(24, trackH * visible / total);
        const int thumbY = content.y + (trackH - thumbH) * m_infoScroll / (total - visible);
        renderer.FillRectScreen(content.Right() - 4, content.y, 4, trackH, col::PanelSunken);
        renderer.FillRectScreen(content.Right() - 4, thumbY, 4, thumbH, col::BorderStrong);
    }

    UiRect back = DialogFooterButton(d);
    ui::DrawButton(
        renderer, fontRenderer, back, _("BACK"), ui::ButtonStyle::Secondary, back.Contains(m_mouseX, m_mouseY));
}

void MenuController::RenderMenuHelp(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH)
{
    UiRect d = InfoDialogRect(windowW, windowH);
    UiRect content = ui::DrawDialog(renderer, fontRenderer, windowW, windowH, d, _("HELP & CONTROLS"));

    struct Binding
    {
        const char *keys;
        bool localizedKeys; // mouse buttons are named per language, keyboard keys are not
        const char *action;
    };
    static const Binding kBindings[] = {
        {"LMB", true, "Select unit / drag to box-select"},
        {"RMB", true, "Move, or attack the target under the cursor"},
        {"W A S D / Arrows", true, "Scroll the camera"},
        {"MMB", true, "Drag the map"},
        {"Wheel", true, "Zoom (over the map) / scroll lists (over the sidebar)"},
        {"Q", false, "Attack-move: units engage every enemy on the way"},
        {"G", false, "Siege mode for Sniper and Rhino"},
        {"Esc", false, "Cancel / game menu"},
    };

    int keyColW = 170;
    int y = content.y;
    for (const auto &b : kBindings)
    {
        std::string keys = b.localizedKeys ? std::string(_(b.keys)) : std::string(b.keys);
        int kw = fontRenderer.GetTextWidth(keys, ui::FONT_SMALL_BOLD) + 16;
        UiRect chip {content.x, y, kw, 26};
        renderer.FillRectScreen(chip.x, chip.y, chip.w, chip.h, col::PanelRaised);
        renderer.DrawRectScreen(chip.x, chip.y, chip.w, chip.h, col::BorderStrong);
        renderer.FillRectScreen(chip.x + 1, chip.Bottom() - 3, chip.w - 2, 2, col::PanelSunken);
        fontRenderer.DrawString(
            keys,
            chip.CenterX(),
            chip.CenterY() - 1,
            ui::FONT_SMALL_BOLD,
            col::Accent,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, _(b.action), ui::FONT_BODY, content.w - keyColW),
            content.x + keyColW,
            chip.CenterY(),
            ui::FONT_BODY,
            col::Text,
            false,
            Anchor::LEFT | Anchor::VCENTER);
        y += 38;
    }

    UiRect back = DialogFooterButton(d);
    ui::DrawButton(
        renderer, fontRenderer, back, _("BACK"), ui::ButtonStyle::Secondary, back.Contains(m_mouseX, m_mouseY));
}

// Who made the music (assets/data/audio_tracks.yaml): every author once, with the link or licence given there
std::vector<std::string> MusicCredits()
{
    static std::vector<std::string> credits = [] {
        std::vector<std::string> out;
        std::string text;
        std::string tracklistPath = LoaderResources::Tracklist();
        if (!Assets::ReadText(tracklistPath, text))
        {
            if (tracklistPath.find(".yaml") != std::string::npos)
            {
                std::string alt = tracklistPath;
                alt.replace(alt.find(".yaml"), 5, ".json");
                Assets::ReadText(alt, text);
            }
        }
        if (text.empty())
            return out;
        const nlohmann::json doc = JsonFile::ReadFromString(text);
        if (!doc.is_object())
            return out;
        for (const char *list : {"mainmenu", "tracks"})
        {
            if (!doc.contains(list))
                continue;
            const auto &section = doc[list];
            auto processItem = [&](const nlohmann::json &track) {
                if (!track.is_object() || !track.contains("author"))
                    return;
                std::string line = "  " + track.value("author", std::string());
                if (track.contains("link"))
                    line += " - " + track.value("link", std::string());
                else if (track.contains("license"))
                    line += " (" + track.value("license", std::string()) + ")";
                if (std::find(out.begin(), out.end(), line) == out.end())
                    out.push_back(line);
            };
            if (section.is_array())
            {
                for (const auto &track : section)
                    processItem(track);
            }
            else if (section.is_object())
            {
                processItem(section);
            }
        }
        return out;
    }();
    return credits;
}

void MenuController::RenderMenuAbout(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH)
{
    std::vector<std::string> lines = {
        "Open Art Of War: Freedom for Liberty",
        std::string(_("Version")) + ": " + OAOW_VERSION,
        std::string(_("Author")) + ": badcast",
        "https://github.com/badcast/open-art-of-war",
        "",
        _("Central Peru is occupied by the army of the Confederation."),
        _("The Resistance stands ready to strike for freedom!"),
        "",
    };
    // The licence texts shipped with the music; the tracklist authors only when the file is missing
    std::string licence;
    if (Assets::ReadText(LoaderResources::AudioLicense(), licence))
    {
        std::istringstream in(licence);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
        while (!lines.empty() && lines.back().empty())
            lines.pop_back();
    }
    else
    {
        for (const std::string &credit : MusicCredits())
            lines.push_back(credit);
    }
    RenderInfoDialog(renderer, fontRenderer, windowW, windowH, _("ABOUT GAME"), lines);
}

} // namespace OpenArtOfWar
