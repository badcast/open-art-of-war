#include "ui/UiLayout.hpp"
#include "ui/MenuController.hpp"
#include "MenuControllerInternal.hpp"
#include "TouchMenuGeometry.hpp"
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
    if (uilayout::MenuProfile(file, "main_menu") == "touch")
        return menu_detail::TouchMainMenuItemRect(i, windowW, windowH);
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
            m_skirmishScroll.Reset();
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
            clicked, pressed, px, py, input.IsLeftMouseDown(), mx, my, windowW, windowH, true, sound, m_scrollDrag))
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
    const int wheel = input.ConsumeWheel();
    int px = 0, py = 0;
    const bool down = input.IsLeftMouseDown();
    const bool pressed = input.ConsumeLeftPress(px, py) && down;

    constexpr int kInfoLineH = 30;
    const int contentH = m_infoTotal * kInfoLineH;
    ui::UpdateSmoothList(
        m_infoScroll,
        m_infoListRect,
        m_infoTrack,
        contentH,
        kInfoLineH * 3,
        m_mouseX,
        m_mouseY,
        down,
        pressed,
        px,
        py,
        wheel,
        clicked);
    if (m_infoScroll.barDrag)
        return;

    const float maxScroll = std::max(0.0f, static_cast<float>(contentH - m_infoListRect.h));
    auto nudgeInfo = [&](float deltaPx) {
        m_infoScroll.velocity = 0.0f;
        m_infoScroll.target = std::clamp(m_infoScroll.target + deltaPx, 0.0f, maxScroll);
    };
    if (input.IsKeyJustPressed(SDL_SCANCODE_UP))
        nudgeInfo(-static_cast<float>(kInfoLineH));
    if (input.IsKeyJustPressed(SDL_SCANCODE_DOWN))
        nudgeInfo(static_cast<float>(kInfoLineH));
    if (input.IsKeyJustPressed(SDL_SCANCODE_PAGEUP))
        nudgeInfo(-static_cast<float>(kInfoLineH * 10));
    if (input.IsKeyJustPressed(SDL_SCANCODE_PAGEDOWN))
        nudgeInfo(static_cast<float>(kInfoLineH * 10));
    if (clicked || input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE) || input.IsConfirmJustPressed())
    {
        state = AppState::MainMenu;
        m_infoScroll.Reset();
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
    int windowH,
    bool drawBackdrop)
{
    if (drawBackdrop)
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
            ui::FillHorizontalGradient(
                renderer, {tx, 0, fadeW, windowH}, col::Backdrop, ui::WithAlpha(col::Backdrop, 0));
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
    }
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
    if (!drawBackdrop)
    {
        int backdropWidth = std::max(
            x + fontRenderer.GetTextWidth("OPEN ART OF WAR", ui::FONT_HERO),
            x + 2 + fontRenderer.GetTextWidth(_("FREEDOM FOR LIBERTY"), ui::FONT_BODY_BOLD));
        for (int i = 0; i < kMainMenuItems; ++i)
        {
            const UiRect item = MainMenuItemRect(i, windowW, windowH);
            backdropWidth = std::max(
                backdropWidth,
                item.x + 25 + fontRenderer.GetTextWidth(_(kMainMenuLabels[i]), touchMenu ? ui::FONT_H1 : ui::FONT_H2));
        }
        renderer.FillRectScreen(0, 0, std::min(windowW, backdropWidth + 24), windowH, {0, 0, 0, 150});
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

        if (!touchMenu)
        {
            const SDL_Color itemAccent = isExit ? col::Danger : col::Accent;
            const SDL_Color plateLeft
                = selected ? ui::Mix(col::Hover, isExit ? col::DangerDeep : ui::color::AccentDeep, 0.56f)
                           : ui::WithAlpha(ui::Mix(col::PanelRaised, ui::color::Info, 0.10f), 195);
            const SDL_Color plateRight = selected ? ui::WithAlpha(col::Panel, 24) : ui::WithAlpha(col::Panel, 12);
            const SDL_Color barCol = selected ? itemAccent : SDL_Color {0, 0, 0, 0};
            ui::DrawButtonChrome(renderer, r, plateLeft, plateRight, {0, 0, 0, 0}, barCol, {0, 0, 0, 0}, 255, true);

            const SDL_Color rimStart = selected ? ui::WithAlpha(itemAccent, 185) : ui::WithAlpha(col::BorderStrong, 95);
            ui::FillHorizontalGradient(renderer, {r.x + 1, r.y, r.w - 2, 1}, rimStart, ui::WithAlpha(rimStart, 0));
            ui::FillHorizontalGradient(
                renderer,
                {r.x + 1, r.Bottom() - 1, r.w - 2, 1},
                ui::WithAlpha(rimStart, static_cast<Uint8>(rimStart.a / 2)),
                ui::WithAlpha(rimStart, 0));
        }

        SDL_Color textCol = selected ? (isExit ? SDL_Color {255, 204, 194, 255} : col::White) : col::TextDim;
        if (touchMenu)
        {
            // Touch: the same text lines as on the desktop (no button plate), only bigger and always lit
            textCol = isExit ? SDL_Color {255, 190, 180, 255} : col::Text;
        }
        fontRenderer.DrawString(
            label,
            r.x + 20 + (selected && !touchMenu ? 6 : 0),
            r.CenterY(),
            touchMenu ? ui::FONT_H1 : ui::FONT_H2,
            textCol,
            selected,
            Anchor::LEFT | Anchor::VCENTER);
        if (i == kMainMenuItems - 2)
        {
            // Separator between the game modes / info items and Exit
            ui::FillHorizontalGradient(
                renderer, {r.x + 12, r.Bottom() + 2, 220, 1}, col::BorderStrong, ui::WithAlpha(col::Border, 0));
        }
    }

    // Footer
    constexpr int thikn = 6;
    const std::string ver = std::string(_("Version")) + " " + OAOW_VERSION;
    std::tuple<int, int> t = fontRenderer.GetTextPointFromAnchor(
        ver, windowW - 24, windowH - 30, ui::FONT_SMALL, Anchor::RIGHT | Anchor::TOP);
    const UiRect verRect {
        std::get<0>(t) - thikn - 4,
        std::get<1>(t) - thikn,
        fontRenderer.GetTextWidth(ver, ui::FONT_SMALL) + thikn * 2 + 8,
        fontRenderer.GetTextHeight(ui::FONT_SMALL) + thikn * 2};
    ui::DrawPanel(renderer, verRect, ui::WithAlpha(col::Panel, 230), col::BorderStrong);
    renderer.FillRectScreen(verRect.x + 1, verRect.y + 3, 2, std::max(1, verRect.h - 6), col::Accent);
    fontRenderer.DrawString(
        ver, windowW - 24, windowH - 30, ui::FONT_SMALL, col::White, true, Anchor::RIGHT | Anchor::TOP);

    RenderMusicPlayer(renderer, fontRenderer, sound, windowW, windowH, true, m_mouseX, m_mouseY);

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
    std::string_view title,
    const std::vector<std::string> &lines)
{
    UiRect d = InfoDialogRect(windowW, windowH);
    UiRect content = ui::DrawDialog(renderer, fontRenderer, windowW, windowH, d, title);

    // Long texts scroll: the footer button takes the bottom of the dialog
    constexpr int kLineH = 30;
    const int listH = std::max(kLineH, DialogFooterButton(d).y - 10 - content.y);
    const int visible = std::max(1, listH / kLineH);
    const int total = static_cast<int>(lines.size());
    const int contentH = total * kLineH;
    const float maxScroll = std::max(0.0f, static_cast<float>(contentH - listH));
    m_infoListRect = {content.x, content.y, content.w, listH};
    m_infoTotal = total;
    m_infoVisible = visible;
    m_infoScroll.offset = std::clamp(m_infoScroll.offset, 0.0f, maxScroll);
    m_infoScroll.target = std::clamp(m_infoScroll.target, 0.0f, maxScroll);
    const int scrollPx = m_infoScroll.PixelOffset();
    renderer.SetClipRect({m_infoListRect.x, m_infoListRect.y, m_infoListRect.w, m_infoListRect.h});
    const int firstLine = std::max(0, scrollPx / kLineH);
    for (int i = firstLine; i < total; ++i)
    {
        const int y = content.y + i * kLineH - scrollPx;
        if (y >= m_infoListRect.Bottom())
            break;
        if (!lines[i].empty())
        {
            fontRenderer.DrawString(
                ui::FitText(fontRenderer, lines[i], ui::FONT_BODY, content.w - 14),
                content.x,
                y,
                ui::FONT_BODY,
                col::Text,
                false);
        }
    }
    renderer.ResetClipRect();
    m_infoTrack = {};
    if (maxScroll > 0.0f)
    {
        m_infoTrack = {content.Right() - 5, content.y, 5, listH};
        ui::DrawScrollBar(
            renderer,
            m_infoTrack,
            m_infoScroll.offset,
            static_cast<float>(contentH),
            static_cast<float>(listH),
            m_infoScroll.barDrag || ui::OnScrollTrack(m_infoTrack, m_mouseX, m_mouseY));
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
        ui::DrawButtonChrome(
            renderer,
            chip,
            ui::Mix(col::Hover, ui::color::AccentDeep, 0.25f),
            col::PanelSunken,
            col::BorderStrong,
            {0, 0, 0, 0},
            ui::Mix(col::Accent, col::BorderStrong, 0.35f));
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

std::vector<std::string> MenuController::AboutLines() const
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
    std::string buildInfo;
    if (Assets::ReadText("assets/data/buildinfo.yaml", buildInfo))
    {
        const auto info = JsonFile::ReadYamlFromString(buildInfo);
        if (info.contains("libraries") && info["libraries"].is_array())
        {
            lines.push_back("LIBRARIES");
            lines.push_back("");
            for (const auto &library : info["libraries"])
            {
                if (!library.is_object() || !library.contains("name") || !library["name"].is_string()
                    || !library.contains("description") || !library["description"].is_string())
                    continue;
                lines.push_back(library["name"].get<std::string>() + " - " + library["description"].get<std::string>());
                lines.push_back("");
            }
        }
    }
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
    return lines;
}

void MenuController::RenderMenuAbout(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH)
{
    RenderInfoDialog(renderer, fontRenderer, windowW, windowH, _("ABOUT GAME"), AboutLines());
}

} // namespace OpenArtOfWar
