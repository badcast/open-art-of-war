#include "ui/MenuController.hpp"
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
#include <cstdio>
#include <random>
#include <sstream>
#include <filesystem>
#include <vector>

namespace OpenArtOfWar
{

using ui::UiRect;
namespace col = ui::color;

namespace
{

// ---------------------------------------------------------------------------
// Shared geometry: the same rectangles drive rendering and mouse hit-testing
// ---------------------------------------------------------------------------

constexpr int kMainMenuItems = 9;
constexpr int kPauseItems = 6;

const char *const kMainMenuLabels[kMainMenuItems]
    = {"CAMPAIGN", "CUSTOM GAME", "NETWORK GAME", "OPEN REPLAY", "TEST MAP", "SETTINGS", "HELP", "ABOUT", "EXIT"};

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
    const int itemH = 44;
    const int gap = 6;
    int startY = MainMenuTitleY(windowH) + 128;
    int maxStart = windowH - 56 - kMainMenuItems * (itemH + gap);
    startY = std::max(MainMenuTitleY(windowH) + 96, std::min(startY, maxStart));
    return {MainMenuColumnX(windowW), startY + i * (itemH + gap), 340, itemH};
}

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
UiRect CustomRow(const UiRect &d, int i)
{
    return {d.x + 20, d.y + 104 + i * 34, d.w - 40 - 222, 30};
}
UiRect CustomCell(const UiRect &d, int i, int kind)
{
    const UiRect r = CustomRow(d, i);
    switch (kind)
    {
        case 1:
            return {r.x + 110, r.y, 108, r.h};
        case 2:
            return {r.x + 224, r.y, 50, r.h};
        case 3:
            return {r.x + 280, r.y, 44, r.h};
        case 6:
            return {r.x + 330, r.y, 44, r.h};
        case 7:
            return {r.x + 380, r.y, 52, r.h};
        case 5:
            return {r.Right() - 28, r.y + 3, 24, r.h - 6};
        default:
            return {r.x, r.y, 104, r.h};
    }
}
UiRect CustomAddBtn(const UiRect &d, int rows)
{
    const UiRect r = CustomRow(d, rows);
    return {r.x, r.y + 2, 190, 28};
}
UiRect CustomPreviewArea(const UiRect &d)
{
    return {d.Right() - 20 - 210, d.y + 88, 210, 210};
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
    UiRect layoutBtn;     // custom game: prebuilt bases or from scratch (top right of the details)
    UiRect fogCheckbox;   // custom game: fog of war checkbox
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
    l.dialog = ui::CenteredRect(windowW, windowH, 1080, 640);
    int contentY = l.dialog.y + 52 + 16;
    int footerY = l.dialog.Bottom() - 64;
    l.list = {l.dialog.x + 20, contentY, 300, footerY - contentY - 8};
    int n = std::max(1, missionCount);
    l.itemH = std::clamp((l.list.h - (n - 1) * l.itemGap) / n, 26, 44);
    l.details = {l.list.Right() + 16, contentY, l.dialog.Right() - 20 - (l.list.Right() + 16), l.list.h};
    l.backBtn = {l.dialog.Right() - 20 - 140, footerY + 10, 140, 40};
    l.startBtn = {l.backBtn.x - 12 - 200, footerY + 10, 200, 40};
    l.sideBtn[0] = {l.dialog.x + 20, footerY + 10, 240, 40};
    l.editorBtn = {l.startBtn.x - 12 - 140, footerY + 10, 140, 40};
    l.layoutBtn = {l.details.Right() - 20 - 210, l.details.y + 12, 210, 32};
    const UiRect pv = CustomPreviewArea(l.details);
    l.fogCheckbox = {pv.x, pv.Bottom() + 66, pv.w, 28};
    l.difficultyBtn = {
        l.sideBtn[0].Right() + 12, footerY + 10, std::max(180, l.editorBtn.x - 12 - (l.sideBtn[0].Right() + 12)), 40};
    return l;
}

// The custom game list shows as many fixed-height rows as fit and scrolls
int CustomListRows(CampaignLayout &l)
{
    l.itemH = 30;
    return std::max(1, (l.list.h + l.itemGap) / (l.itemH + l.itemGap));
}

// Pause menu
struct PauseLayout
{
    UiRect dialog;
    UiRect items[kPauseItems];
};

PauseLayout pauseLayout(int windowW, int windowH)
{
    PauseLayout l;
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

// Music player in the bottom left corner: previous, track name, next; the main menu adds a seek bar with the time above
struct PlayerLayout
{
    UiRect panel, prev, next, name, seek;
};

PlayerLayout playerLayout(int windowH)
{
    PlayerLayout l;
    l.panel = {24, windowH - 24 - 40, 340, 40};
    l.prev = {l.panel.x, l.panel.y, 40, 40};
    l.next = {l.panel.Right() - 40, l.panel.y, 40, 40};
    l.name = {l.prev.Right() + 8, l.panel.y, l.panel.w - 2 * 40 - 16, 40};
    l.seek = {l.panel.x, l.panel.y - 30, l.panel.w, 22};
    return l;
}

std::string FormatTime(double seconds)
{
    const int s = std::max(0, static_cast<int>(seconds));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

// First visible line for a thumb dragged to mouse height `my` along the scroll bar `track`
int ScrollTopFromMouse(const UiRect &track, int my, int total, int visible)
{
    if (total <= visible)
        return 0;
    const int thumbH = std::max(20, track.h * visible / total);
    const float ratio
        = std::clamp(static_cast<float>(my - track.y - thumbH / 2) / std::max(1, track.h - thumbH), 0.0f, 1.0f);
    return static_cast<int>(ratio * (total - visible) + 0.5f);
}

// Scroll bars are 4px wide: grab them from a wider strip
bool OnScrollBar(const UiRect &track, int mx, int my)
{
    return track.w > 0 && mx >= track.x - 8 && mx <= track.Right() + 8 && my >= track.y && my <= track.Bottom();
}

// Returns true when the mouse took the click (or holds the seek bar)
bool UpdateMusicPlayer(
    bool clicked,
    bool pressed,
    int px,
    int py,
    bool down,
    int mx,
    int my,
    int windowH,
    bool withSeek,
    SoundEngine &sound,
    int &drag)
{
    const PlayerLayout l = playerLayout(windowH);
    if (drag == 3)
    {
        if (!down)
        {
            drag = 0;
            return clicked;
        } // the release click ends the drag, it presses nothing
        // Seeking re-reads the stream: follow the mouse a few times a second, not every frame
        static Uint64 lastSeek = 0;
        const Uint64 now = SDL_GetTicks();
        if (now - lastSeek >= 120)
        {
            lastSeek = now;
            sound.SeekTrack(static_cast<double>(mx - l.seek.x) / l.seek.w);
        }
        return true;
    }
    if (withSeek && pressed && down && l.seek.Contains(px, py))
    {
        sound.SeekTrack(static_cast<double>(px - l.seek.x) / l.seek.w);
        drag = 3;
        return true;
    }
    if (!clicked)
        return false;
    if (l.prev.Contains(mx, my))
    {
        sound.PrevTrack();
        return true;
    }
    if (l.next.Contains(mx, my))
    {
        sound.NextTrack();
        return true;
    }
    if (withSeek && l.seek.Contains(mx, my))
        return true;
    return l.panel.Contains(mx, my);
}

void RenderMusicPlayer(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const SoundEngine &sound,
    int windowH,
    bool withSeek,
    int mx,
    int my)
{
    const PlayerLayout l = playerLayout(windowH);
    renderer.FillRectScreen(l.name.x, l.name.y, l.name.w, l.name.h, col::PanelSunken);
    renderer.DrawRectScreen(l.name.x, l.name.y, l.name.w, l.name.h, col::Border);
    fontRenderer.DrawString(
        ui::FitText(fontRenderer, sound.GetCurrentTrackName(), ui::FONT_SMALL, l.name.w - 16),
        l.name.CenterX(),
        l.name.CenterY(),
        ui::FONT_SMALL,
        col::TextDim,
        false,
        Anchor::HCENTER | Anchor::VCENTER);
    ui::DrawButton(renderer, fontRenderer, l.prev, "<", ui::ButtonStyle::Secondary, l.prev.Contains(mx, my));
    ui::DrawButton(renderer, fontRenderer, l.next, ">", ui::ButtonStyle::Secondary, l.next.Contains(mx, my));
    if (!withSeek)
        return;

    const double length = sound.GetTrackLength();
    const double ratio = length > 0.0 ? std::clamp(sound.GetTrackPosition() / length, 0.0, 1.0) : 0.0;
    const std::string elapsed = FormatTime(sound.GetTrackPosition());
    const std::string total = FormatTime(length);
    const int labelW = 44;
    const int barX = l.seek.x + labelW, barW = l.seek.w - 2 * labelW;
    const int barY = l.seek.CenterY() - 2;
    fontRenderer.DrawString(
        elapsed, l.seek.x, l.seek.CenterY(), ui::FONT_SMALL, col::TextDim, false, Anchor::LEFT | Anchor::VCENTER);
    fontRenderer.DrawString(
        total, l.seek.Right(), l.seek.CenterY(), ui::FONT_SMALL, col::TextDim, false, Anchor::RIGHT | Anchor::VCENTER);
    renderer.FillRectScreen(barX, barY, barW, 4, col::PanelSunken);
    renderer.FillRectScreen(barX, barY, static_cast<int>(barW * ratio), 4, col::Accent);
    renderer.FillRectScreen(barX + static_cast<int>(barW * ratio) - 3, l.seek.CenterY() - 7, 6, 14, col::Text);
}

UiRect InfoDialogRect(int windowW, int windowH)
{
    return ui::CenteredRect(windowW, windowH, 700, 500);
}

UiRect DialogFooterButton(const UiRect &dialog, int w = 180)
{
    return {dialog.Right() - 20 - w, dialog.Bottom() - 20 - 40, w, 40};
}

UiRect BriefingRect(int windowW, int windowH)
{
    return ui::CenteredRect(windowW, windowH, 820, 480);
}

UiRect ObjectivesRect(int windowW, int windowH)
{
    return ui::CenteredRect(windowW, windowH, 680, 480);
}

std::string DifficultyName(AIDifficulty d)
{
    switch (d)
    {
        case AIDifficulty::Easy:
            return _("Easy");
        case AIDifficulty::Medium:
            return _("Medium");
        case AIDifficulty::Hard:
            return _("Hard");
        case AIDifficulty::Hardcore:
            return _("Hardcore");
    }
    return {};
}

std::string DifficultyDesc(AIDifficulty d)
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

// ---- Settings screen --------------------------------------------------------------------------------------------
constexpr int kSettingsTabs = 4;

struct SettingsLayout
{
    UiRect dialog;
    UiRect tabs[kSettingsTabs];
    static constexpr int kMaxRows = 10;
    UiRect rows[kMaxRows];
    int rowCount {6};
    UiRect defaultsBtn;
    UiRect backBtn;
};

// `rows`: how many the shown tab has; the dialog grows and the rows get lower so they all fit
SettingsLayout settingsLayout(int windowW, int windowH, int rows = 6)
{
    SettingsLayout l;
    l.rowCount = std::clamp(rows, 1, SettingsLayout::kMaxRows);
    l.dialog = ui::CenteredRect(windowW, windowH, 780, l.rowCount > 6 ? 700 : 560);
    const int tabW = (l.dialog.w - 40 - (kSettingsTabs - 1) * 6) / kSettingsTabs;
    for (int i = 0; i < kSettingsTabs; ++i)
        l.tabs[i] = {l.dialog.x + 20 + i * (tabW + 6), l.dialog.y + 60, tabW, 36};
    l.backBtn = {l.dialog.Right() - 20 - 160, l.dialog.Bottom() - 20 - 40, 160, 40};
    l.defaultsBtn = {l.backBtn.x - 12 - 200, l.backBtn.y, 200, 40};
    const int top = l.dialog.y + 60 + 36 + 14;
    const int gap = 6;
    const int space = l.backBtn.y - 12 - top;
    const int rowH = std::clamp((space - gap * (l.rowCount - 1)) / l.rowCount, 40, 56);
    for (int i = 0; i < l.rowCount; ++i)
        l.rows[i] = {l.dialog.x + 20, top + i * (rowH + gap), l.dialog.w - 40, rowH};
    return l;
}

// The control of a row: a value pill with arrows, or a slider with its value
UiRect SettingsControlRect(const UiRect &row)
{
    return {row.Right() - 16 - 320, row.CenterY() - 16, 320, 32};
}

UiRect SliderTrackRect(const UiRect &row)
{
    const UiRect c = SettingsControlRect(row);
    return {c.x, c.CenterY() - 4, c.w - 78, 8};
}

struct SettingsRow
{
    std::string label;
    std::string hint;
    std::string value;
    SDL_Color valueColor {col::Accent};
    bool slider {false};
    float ratio {0.0f}; // slider position 0..1
    bool enabled {true};
    std::function<void(int)> step;       // -1 / +1: previous / next value, one notch for a slider
    std::vector<std::string> options;    // a row with choices is a combo box (a drop-down list)
    int current {0};                     // index of the chosen option
    std::function<void(int)> select;     // takes the index of the option picked in the list
    std::function<void(float)> setRatio; // slider: set from the position 0..1
};

std::string OnOff(bool on)
{
    return on ? _("ON") : _("OFF");
}

std::string Number(float v, int decimals)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

// A slider over [lo, hi] in steps of `notch`; `commit` stores the new value
SettingsRow SliderRow(
    std::string label,
    std::string hint,
    float value,
    float lo,
    float hi,
    float notch,
    std::function<std::string(float)> show,
    std::function<void(float)> commit)
{
    SettingsRow row;
    row.label = std::move(label);
    row.hint = std::move(hint);
    row.slider = true;
    row.ratio = (value - lo) / (hi - lo);
    row.value = show(value);
    row.step = [=](int dir) {
        commit(std::clamp(value + dir * notch, lo, hi));
    };
    row.setRatio = [=](float r) {
        const float raw = lo + std::clamp(r, 0.0f, 1.0f) * (hi - lo);
        commit(std::clamp(std::round(raw / notch) * notch, lo, hi));
    };
    return row;
}

std::vector<SettingsRow> BuildSettingsRows(
    int tab,
    Settings &s,
    const std::vector<Resolution> &resolutions,
    const std::function<void(SettingsArea)> &notify,
    const RenderApiBinding &renderApi,
    bool inGame)
{
    std::vector<SettingsRow> rows;
    // A row with a list of choices (shown as a combo box); the arrow keys still step through them
    auto choiceList = [&](std::string label,
                          std::string hint,
                          std::vector<std::string> options,
                          int current,
                          std::function<void(int)> select,
                          bool enabled = true,
                          std::string fallbackValue = {}) {
        SettingsRow r;
        r.label = std::move(label);
        r.hint = std::move(hint);
        const int n = static_cast<int>(options.size());
        current = n > 0 ? std::clamp(current, 0, n - 1) : 0;
        r.value = n > 0 ? options[static_cast<size_t>(current)] : std::move(fallbackValue);
        r.options = std::move(options);
        r.current = current;
        r.select = select;
        r.step = [select, n, current](int dir) {
            if (n > 0)
                select((current + dir + n) % n);
        };
        r.enabled = enabled;
        rows.push_back(std::move(r));
    };
    // On / off as a list of two
    auto toggle
        = [&](std::string label, std::string hint, bool value, std::function<void(bool)> set, bool enabled = true) {
              choiceList(
                  std::move(label),
                  std::move(hint),
                  {_("ON"), _("OFF")},
                  value ? 0 : 1,
                  [set](int i) { set(i == 0); },
                  enabled);
          };
    auto choice = [&](std::string label,
                      std::string hint,
                      std::string value,
                      std::function<void(int)> step,
                      bool enabled = true) {
        SettingsRow r;
        r.label = std::move(label);
        r.hint = std::move(hint);
        r.value = std::move(value);
        r.step = std::move(step);
        r.enabled = enabled;
        rows.push_back(std::move(r));
    };

    if (tab == 0)
    { // VIDEO
        static const char *const kModes[3] = {"Windowed", "Borderless", "Fullscreen"};
        choiceList(
            _("Window mode"),
            _("Window, borderless fullscreen or exclusive fullscreen"),
            {_(kModes[0]), _(kModes[1]), _(kModes[2])},
            static_cast<int>(s.windowMode),
            [&s, notify](int i) {
                s.windowMode = static_cast<WindowMode>(std::clamp(i, 0, 2));
                notify(SettingsArea::Video);
            });

        const bool exclusive = s.windowMode == WindowMode::Exclusive;
        const bool borderless = s.windowMode == WindowMode::Borderless;
        int &w = exclusive ? s.fullscreenW : s.windowW;
        int &h = exclusive ? s.fullscreenH : s.windowH;
        int index = 0;
        long best = -1;
        for (size_t i = 0; i < resolutions.size(); ++i)
        { // the listed size nearest to the current one
            const long d = static_cast<long>(resolutions[i].w - w) * (resolutions[i].w - w)
                           + static_cast<long>(resolutions[i].h - h) * (resolutions[i].h - h);
            if (best < 0 || d < best)
            {
                best = d;
                index = static_cast<int>(i);
            }
        }
        const std::string value
            = borderless ? std::string(_("Desktop")) : std::to_string(w) + " x " + std::to_string(h);
        std::vector<std::string> sizes;
        for (const Resolution &r : resolutions)
            sizes.push_back(std::to_string(r.w) + " x " + std::to_string(r.h));
        choiceList(
            _("Resolution"),
            exclusive ? _("Display mode of the monitor") : _("Size of the game window"),
            sizes,
            index,
            [&w, &h, resolutions, notify](int i) {
                if (i < 0 || i >= static_cast<int>(resolutions.size()))
                    return;
                w = resolutions[static_cast<size_t>(i)].w;
                h = resolutions[static_cast<size_t>(i)].h;
                notify(SettingsArea::Video);
            },
            !borderless,
            value);

        toggle(_("VSync"), _("Wait for the monitor refresh, no tearing"), s.vsync, [&s, notify](bool on) {
            s.vsync = on;
            notify(SettingsArea::Video);
        });

        static const int kLimits[5] = {0, 30, 60, 120, 144};
        int li = 0;
        for (int i = 0; i < 5; ++i)
            if (kLimits[i] == s.fpsLimit)
                li = i;
        choiceList(
            _("FPS limit"),
            _("Cap on frames per second"),
            {_("Unlimited"), "30", "60", "120", "144"},
            li,
            [&s, notify](int i) {
                s.fpsLimit = kLimits[std::clamp(i, 0, 4)];
                notify(SettingsArea::Video);
            });

        toggle(_("Show FPS"), _("Frame counter in the corner of the screen"), s.showFps, [&s, notify](bool on) {
            s.showFps = on;
            notify(SettingsArea::Video);
        });

        toggle(
            _("Network statistics"),
            _("Network game: ping, jitter, packet loss and traffic in the corner"),
            s.showNetStats,
            [&s, notify](bool on) {
                s.showNetStats = on;
                notify(SettingsArea::Video);
            });
    }
    else if (tab == 1)
    { // GRAPHICS: quality preset and render API
        static const char *const kQuality[3] = {"Low", "Medium", "High"};
        static const char *const kQualityHint[3]
            = {"Sprites at 1/3 of the resolution", "Sprites at 1/2 of the resolution", "Original resolution"};
        const int q = std::clamp(s.graphicsQuality, 0, 2);
        choiceList(
            _("Graphics quality"),
            _(kQualityHint[q]),
            {_(kQuality[0]), _(kQuality[1]), _(kQuality[2])},
            q,
            [&s, notify](int i) {
                s.graphicsQuality = std::clamp(i, 0, 2);
                notify(SettingsArea::Graphics);
            });
        toggle(
            _("Smoothing"),
            _("Smooth scaling of the sprites instead of hard pixels"),
            s.smoothing,
            [&s, notify](bool on) {
                s.smoothing = on;
                notify(SettingsArea::Graphics);
            });

        if (renderApi.options && renderApi.configured && renderApi.active)
        {
            // Render API: the ones known to fail on this system are left out, untried ones marked with "?"
            std::vector<RenderApiOption> usable;
            for (const RenderApiOption &o : renderApi.options())
            {
                if (o.status != RenderApiOption::Status::Fails)
                    usable.push_back(o);
            }
            const std::string configured = renderApi.configured();
            const std::string active = renderApi.active();
            int ci = 0;
            std::vector<std::string> apiLabels;
            for (size_t i = 0; i < usable.size(); ++i)
            {
                apiLabels.push_back(
                    RenderBackend::Label(usable[i].name)
                    + (usable[i].status == RenderApiOption::Status::Unknown ? " ?" : ""));
                if (usable[i].name == configured)
                    ci = static_cast<int>(i);
            }
            const std::string value = RenderBackend::Label(configured);
            const bool pending = configured != active;
            auto select = renderApi.select;
            choiceList(
                _("Render API"),
                pending ? _("Takes effect after the window is recreated") : _("Graphics interface that draws the game"),
                apiLabels,
                ci,
                [usable, select](int i) {
                    if (!select || i < 0 || i >= static_cast<int>(usable.size()))
                        return;
                    select(usable[static_cast<size_t>(i)].name);
                },
                usable.size() > 1,
                value);
        }
        // Shader effects: only the SDL GPU render API draws them
        const bool fx = renderApi.effectsAvailable && renderApi.effectsAvailable();
        const std::string fxHint = _("Needs the SDL GPU render API");
        static const char *const kGrades[6] = {"Off", "Natural", "Vivid", "Cinematic", "Dusk", "Night"};
        static const char *const kGradeHints[6]
            = {"The picture as drawn",
               "A little more contrast and colour",
               "Strong colours",
               "Muted colours, warm light, dark corners",
               "Evening light",
               "Night: explosions light up the dark"};
        const int g = std::clamp(s.colorGrade, 0, 5);
        std::vector<std::string> gradeNames;
        for (const char *name : kGrades)
            gradeNames.push_back(_(name));
        choiceList(
            _("Color grading"),
            fx ? std::string(_(kGradeHints[g])) : fxHint,
            gradeNames,
            g,
            [&s, notify](int i) {
                s.colorGrade = std::clamp(i, 0, 5);
                notify(SettingsArea::Graphics);
            },
            fx);
        toggle(
            _("Glow"),
            fx ? std::string(_("Explosions, shots and projectiles glow")) : fxHint,
            s.glow,
            [&s, notify](bool on) {
                s.glow = on;
                notify(SettingsArea::Graphics);
            },
            fx);
        choiceList(
            _("Pixel filter"),
            fx ? std::string(_("Crisp pixels with smooth edges when zoomed")) : fxHint,
            {_("Standard"), _("Sharp")},
            s.sharpPixels ? 1 : 0,
            [&s, notify](int i) {
                s.sharpPixels = i == 1;
                notify(SettingsArea::Graphics);
            },
            fx);
    }
    else if (tab == 2)
    { // AUDIO
        toggle(_("Sound & Music"), _("Music and sound effects"), !s.muted, [&s, notify](bool on) {
            s.muted = !on;
            notify(SettingsArea::Audio);
        });
        rows.back().valueColor = s.muted ? col::Danger : col::Ok;
        auto percent = [](float v) {
            return std::to_string(static_cast<int>(v * 100.0f / 128.0f + 0.5f)) + "%";
        };
        rows.push_back(SliderRow(
            _("Music volume"),
            _("Volume of the soundtrack"),
            static_cast<float>(s.musicVolume),
            0.0f,
            128.0f,
            8.0f,
            percent,
            [&s, notify](float v) {
                s.musicVolume = static_cast<int>(v);
                notify(SettingsArea::Audio);
            }));
        rows.push_back(SliderRow(
            _("Effects volume"),
            _("Volume of the sound effects"),
            static_cast<float>(s.fxVolume),
            0.0f,
            128.0f,
            8.0f,
            percent,
            [&s, notify](float v) {
                s.fxVolume = static_cast<int>(v);
                notify(SettingsArea::Audio);
            }));
        rows.push_back(SliderRow(
            _("Interface volume"),
            _("Volume of the interface sounds: clicks, dialogs and alerts"),
            static_cast<float>(s.uiVolume),
            0.0f,
            128.0f,
            8.0f,
            percent,
            [&s, notify](float v) {
                s.uiVolume = static_cast<int>(v);
                notify(SettingsArea::Audio);
            }));
        rows.push_back(SliderRow(
            _("Voice volume"),
            _("Volume of the unit replies and the announcer"),
            static_cast<float>(s.voiceVolume),
            0.0f,
            128.0f,
            8.0f,
            percent,
            [&s, notify](float v) {
                s.voiceVolume = static_cast<int>(v);
                notify(SettingsArea::Audio);
            }));
        toggle(_("Sound reverb"), _("Echo of explosions and heavy guns"), s.reverb, [&s, notify](bool on) {
            s.reverb = on;
            notify(SettingsArea::Audio);
        });
    }
    else
    { // GAME
        choiceList(
            _("Language"),
            _("Interface language"),
            {"English", "Русский"},
            s.language == "ru" ? 1 : 0,
            [&s, notify](int i) {
                s.language = i == 1 ? "ru" : "en";
                notify(SettingsArea::Game);
            });
        rows.push_back(SliderRow(
            _("Default zoom"),
            _("Map zoom when a mission starts"),
            s.zoomDefault,
            0.5f,
            4.0f,
            0.25f,
            [](float v) { return Number(v, 2) + "x"; },
            [&s, notify](float v) {
                s.zoomDefault = v;
                notify(SettingsArea::Graphics);
            }));
        rows.push_back(SliderRow(
            _("Minimum zoom"),
            _("How far the mouse wheel zooms out"),
            s.zoomMin,
            0.5f,
            1.0f,
            0.05f,
            [](float v) { return Number(v, 2) + "x"; },
            [&s, notify](float v) {
                s.zoomMin = v;
                notify(SettingsArea::Graphics);
            }));
        rows.push_back(SliderRow(
            _("Maximum zoom"),
            _("How far the mouse wheel zooms in"),
            s.zoomMax,
            2.0f,
            4.0f,
            0.25f,
            [](float v) { return Number(v, 2) + "x"; },
            [&s, notify](float v) {
                s.zoomMax = v;
                notify(SettingsArea::Graphics);
            }));
        rows.push_back(SliderRow(
            _("Edge scroll margin"),
            _("Screen border thickness in pixels for scrolling with the mouse"),
            static_cast<float>(s.edgePanMargin),
            static_cast<float>(Settings::kMinEdgePanMargin),
            static_cast<float>(Settings::kMaxEdgePanMargin),
            1.0f,
            [](float v) { return std::to_string(static_cast<int>(v)) + " px"; },
            [&s, notify](float v) {
                s.edgePanMargin = static_cast<int>(v);
                notify(SettingsArea::Game);
            }));
    }
    return rows;
}

// Settings of one tab back to their defaults
void ResetSettingsTab(int tab, Settings &s)
{
    const Settings d;
    if (tab == 0)
    {
        s.windowMode = d.windowMode;
        s.windowW = d.windowW;
        s.windowH = d.windowH;
        s.fullscreenW = d.fullscreenW;
        s.fullscreenH = d.fullscreenH;
        s.vsync = d.vsync;
        s.fpsLimit = d.fpsLimit;
        s.showFps = d.showFps;
        s.showNetStats = d.showNetStats;
    }
    else if (tab == 1)
    {
        s.graphicsQuality = d.graphicsQuality;
        s.smoothing = d.smoothing;
        s.colorGrade = d.colorGrade;
        s.glow = d.glow;
        s.sharpPixels = d.sharpPixels;
    }
    else if (tab == 2)
    {
        s.musicVolume = d.musicVolume;
        s.fxVolume = d.fxVolume;
        s.voiceVolume = d.voiceVolume;
        s.uiVolume = d.uiVolume;
        s.reverb = d.reverb;
        s.muted = d.muted;
    }
    else
    {
        s.language = d.language;
        s.zoomDefault = d.zoomDefault;
        s.zoomMin = d.zoomMin;
        s.zoomMax = d.zoomMax;
        s.edgePanMargin = d.edgePanMargin;
    }
}

SettingsArea SettingsTabArea(int tab)
{
    static const SettingsArea kAreas[kSettingsTabs]
        = {SettingsArea::Video, SettingsArea::Graphics, SettingsArea::Audio, SettingsArea::Game};
    return kAreas[std::clamp(tab, 0, kSettingsTabs - 1)];
}

// Section heading + body text block used by briefing / objectives / campaign details
int DrawTextSection(
    Renderer &r,
    BitmapFontRenderer &font,
    int x,
    int y,
    int w,
    const std::string &caption,
    const std::string &body,
    SDL_Color bodyColor,
    int bodyScale)
{
    ui::DrawCaption(font, r, x, y, caption);
    y += 24;
    y += font.DrawTextWrapped(body, x, y, w, bodyScale, bodyColor, false);
    return y + 18;
}

} // namespace

// ---------------------------------------------------------------------------
// Update handlers
// ---------------------------------------------------------------------------

bool MenuController::UpdateInGameMenu(
    InputManager &input,
    int mx,
    int my,
    bool leftClicked,
    int windowW,
    int windowH,
    SoundEngine &sound,
    MissionController &mission,
    TileMap &map,
    EntityManager &entities,
    Economy &economy,
    Pathfinder &pathfinder,
    Camera &camera,
    AppState &state)
{
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
            m_confirm.open = true;
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
                mission.Restart(LoaderResources::kDefaultRoot, map, entities, economy, pathfinder);
                camera.CenterOn(mission.GetPlayerHqX() * TILE_WIDTH, mission.GetPlayerHqY() * TILE_HEIGHT);
                m_inGameMenuOpen = false;
                break;
            case 5: // Main menu
                state = AppState::MainMenu;
                m_inGameMenuOpen = false;
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

namespace
{
// The mission dialogue window: as large as the text needs, within the screen
UiRect MissionDialogRect(int windowW, int windowH)
{
    const int w = std::min(780, windowW - 40);
    const int h = std::min(560, windowH - 40);
    return ui::CenteredRect(windowW, windowH, w, h);
}
} // namespace

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

bool MenuController::UpdateCampaignSelect(
    InputManager &input,
    int mx,
    int my,
    bool leftClicked,
    int windowW,
    int windowH,
    MissionController &mission,
    TileMap &map,
    EntityManager &entities,
    Economy &economy,
    Pathfinder &pathfinder,
    Camera &camera,
    AppState &state,
    AIController &ai)
{
    (void) camera;
    if (!m_campaignSelectOpen)
        return false;
    m_mouseX = mx;
    m_mouseY = my;
    (void) ai;
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
    if (mCount == 0)
    {
        m_campaignSelectOpen = false;
        return true;
    }
    int &selected = m_customGame ? m_skirmishSelected : m_campaignMissionSelected;
    CampaignLayout l = campaignLayout(windowW, windowH, mCount);
    const int rows = m_customGame ? CustomListRows(l) : mCount;

    auto startSelected = [&]() {
        if (m_customGame)
        {
            EnsureCustomSlots();
            bool foes = false;
            for (const CustomSlot &cs : m_customSlots)
                foes = foes || cs.team != m_customSlots[0].team;
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
                m_beginLoading(_(data.info.name), LoaderResources::MapPath(data.data.mapFile));
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
            match.local = 0;
            for (int p = 0; p < static_cast<int>(m_customSlots.size()) && p < kMaxPlayers; ++p)
            {
                const CustomSlot &cs = m_customSlots[p];
                PlayerSlot &slot = match.slots[p];
                slot.active = true;
                slot.human = p == 0;
                if (p == 0)
                    slot.faction = PlayerFaction();
                else if (cs.faction == 0)
                    slot.faction = (rd() & 1u) ? Faction::Confederation : Faction::Resistance;
                else
                    slot.faction = cs.faction == 2 ? Faction::Confederation : Faction::Resistance;
                slot.team = static_cast<uint8_t>(cs.team);
                slot.color = static_cast<uint8_t>(cs.color);
                slot.handicap = static_cast<uint8_t>(std::clamp(cs.handicap, 0, kHandicapCount - 1));
                slot.aiLevel = static_cast<uint8_t>(std::clamp(cs.level, 0, 3));
                match.startPosition[p] = static_cast<int8_t>(cs.position);
            }
        }
        const bool started
            = m_customGame
                  ? mission.LoadMatch(match, map, entities, economy, pathfinder)
                  : mission.LoadMission(
                        m_campaignMissionSelected, LoaderResources::kDefaultRoot, map, entities, economy, pathfinder);
        if (m_endLoading)
            m_endLoading();
        if (!started)
            return;
        state = AppState::Briefing;
        m_campaignSelectOpen = false;
    };

    if (input.IsKeyJustPressed(SDL_SCANCODE_UP))
    {
        selected = (selected - 1 + mCount) % mCount;
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_DOWN))
    {
        selected = (selected + 1) % mCount;
    }
    if (m_customGame)
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
    if (input.IsKeyJustPressed(SDL_SCANCODE_LEFT) || input.IsKeyJustPressed(SDL_SCANCODE_RIGHT))
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
        return id == kComboSide ? l.sideBtn[0] : (id == kComboDifficulty ? l.difficultyBtn : l.layoutBtn);
    };
    if (m_combo.open >= 0 && m_combo.open < kComboSide)
        m_combo.open = -1; // one of the settings screen's
    if ((m_combo.open == kComboLayout || m_combo.open >= kComboSlot) && !hasLayoutChoice)
        m_combo.open = -1;
    if (m_combo.open >= kComboSlot && (m_combo.open - kComboSlot) / 10 >= static_cast<int>(m_customSlots.size()))
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
        if (l.sideBtn[0].Contains(mx, my))
            return openCombo(kComboSide);
        if (l.difficultyBtn.Contains(mx, my))
            return openCombo(kComboDifficulty);
        if (hasLayoutChoice && l.layoutBtn.Contains(mx, my))
            return openCombo(kComboLayout);
        if (hasLayoutChoice && l.fogCheckbox.Contains(mx, my))
        {
            m_skirmishFogOfWar = !m_skirmishFogOfWar;
            SoundRequests::Play("ui_confirm");
            return true;
        }
        if (hasLayoutChoice)
        {
            const UiRect &d = l.details;
            for (int i = 0; i < static_cast<int>(m_customSlots.size()); ++i)
            {
                for (int kind : {1, 2, 3, 4, 6, 7})
                {
                    if ((i > 0 || kind != 4) && CustomCell(d, i, kind).Contains(mx, my))
                        return openCombo(kComboSlot + i * 10 + kind);
                }
                if (i > 0 && CustomCell(d, i, 5).Contains(mx, my))
                {
                    m_customSlots.erase(m_customSlots.begin() + i);
                    return true;
                }
            }
            if (static_cast<int>(m_customSlots.size()) < m_skirmishMaps[m_skirmishSelected].players
                && CustomAddBtn(d, static_cast<int>(m_customSlots.size())).Contains(mx, my))
            {
                CustomSlot add;
                for (int pos = 0; pos < kMaxPlayers; ++pos)
                { // the first start position nobody holds
                    bool used = false;
                    for (const CustomSlot &cs : m_customSlots)
                        used = used || cs.position == pos;
                    if (!used)
                    {
                        add.position = pos;
                        break;
                    }
                }
                add.level = std::clamp(static_cast<int>(m_difficulty), 0, 3);
                const std::vector<bool> takenColors = CustomTakenColors(-1);
                for (int c = 0; c < static_cast<int>(takenColors.size()); ++c)
                {
                    if (!takenColors[c])
                    {
                        add.color = c + 1;
                        break;
                    }
                }
                for (int t = 0; t < kMaxPlayers; ++t)
                { // the first team nobody is in yet
                    bool used = false;
                    for (const CustomSlot &cs : m_customSlots)
                        used = used || cs.team == t;
                    if (!used)
                    {
                        add.team = t;
                        break;
                    }
                }
                m_customSlots.push_back(add);
                return true;
            }
        }
        // Side the player commands in this mission
        if (l.editorBtn.Contains(mx, my))
        {
            const std::string mapPath
                = m_customGame ? m_skirmishMaps[m_skirmishSelected].path
                               : LoaderResources::MapPath(
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
            m_skirmishMaps = MissionController::ListSkirmishMaps();
            SetCampaignSelectOpen(!m_skirmishMaps.empty());
            m_customGame = true;
            m_customSlots.clear();
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
            mission.LoadTestMap(LoaderResources::kDefaultRoot, map, entities, economy, pathfinder);
            state = AppState::CampaignPlaying;
            camera.CenterOn((mission.GetPlayerHqX() + 12) * TILE_WIDTH, mission.GetPlayerHqY() * TILE_HEIGHT);
            break;
        case 5:
            state = AppState::MenuSettings;
            break;
        case 6:
            state = AppState::MenuHelp;
            break;
        case 7:
            state = AppState::MenuAbout;
            break;
        case 8:
            running = false;
            break;
        default:
            break;
    }
}

void MenuController::UpdateMainMenu(
    InputManager &input,
    int windowW,
    int windowH,
    AppState &state,
    bool &running,
    SoundEngine &sound,
    MissionController &mission,
    TileMap &map,
    EntityManager &entities,
    Economy &economy,
    Pathfinder &pathfinder,
    Camera &camera,
    AIController &ai)
{
    int mx = input.GetMouseScreenX();
    int my = input.GetMouseScreenY();
    m_mouseX = mx;
    m_mouseY = my;
    float lx, ly;
    bool clicked = input.WasLeftClickIssued(lx, ly);

    if (m_campaignSelectOpen)
    {
        UpdateCampaignSelect(
            input, mx, my, clicked, windowW, windowH, mission, map, entities, economy, pathfinder, camera, state, ai);
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

void MenuController::BindSettings(
    Settings &settings,
    std::function<void(SettingsArea)> onChanged,
    std::function<const std::vector<Resolution> &()> resolutions)
{
    m_settings = &settings;
    m_onSettingsChanged = std::move(onChanged);
    m_resolutions = std::move(resolutions);
}

void MenuController::UpdateSettings(InputManager &input, int windowW, int windowH, AppState &state)
{
    float lx, ly;
    const bool clicked = input.WasLeftClickIssued(lx, ly);
    UpdateSettingsScreen(input, input.GetMouseScreenX(), input.GetMouseScreenY(), clicked, windowW, windowH, state);
}

// Settings page of the main menu, or of the pause menu (m_inGameSettingsOpen): Back returns to where it came from
void MenuController::UpdateSettingsScreen(
    InputManager &input, int mx, int my, bool clicked, int windowW, int windowH, AppState &state)
{
    m_mouseX = mx;
    m_mouseY = my;
    auto close = [&]() {
        m_combo.open = -1;
        if (m_inGameSettingsOpen)
            m_inGameSettingsOpen = false;
        else
            state = AppState::MainMenu;
    };
    if (!m_settings || !m_onSettingsChanged || !m_resolutions)
    {
        close();
        return;
    }

    static const std::vector<Resolution> kNoResolutions;
    // Picking another render API does not switch at once: a dialog asks first (the window is recreated)
    RenderApiBinding api = m_renderApi;
    api.select = [this](const std::string &name) {
        if (m_renderApi.configured && name == m_renderApi.configured())
            return;
        m_apiPending = name;
        m_apiConfirm.open = true;
        m_apiConfirm.title = std::string(_("SWITCH RENDER API?"));
        m_apiConfirm.text
            = m_inGameSettingsOpen
                  ? std::string(_("The render API changes at the next start of the game: "))
                        + RenderBackend::Label(name)
                  : std::string(_("The window is recreated with this render API: "))
                        + RenderBackend::Label(name)
                        + std::string(_(". You get 15 seconds to keep it, otherwise the previous one returns."));
    };
    auto rows = [&]() {
        return BuildSettingsRows(
            m_settingsTab,
            *m_settings,
            m_resolutions ? m_resolutions() : kNoResolutions,
            m_onSettingsChanged,
            api,
            m_inGameSettingsOpen);
    };

    // The question is open: nothing else reacts until it is answered
    if (m_apiConfirm.open)
    {
        const int answer = ui::ConfirmUpdate(
            m_apiConfirm,
            windowW,
            windowH,
            mx,
            my,
            clicked,
            input.IsConfirmJustPressed(),
            input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE));
        if (answer >= 0)
        {
            m_apiConfirm.open = false;
            if (answer == 1 && m_renderApi.select)
            {
                m_renderApi.select(m_apiPending); // remembered for the next start
                if (!m_inGameSettingsOpen && m_renderApi.applyNow)
                    m_renderApi.applyNow();
            }
            m_apiPending.clear();
        }
        return;
    }

    if (input.IsKeyJustPressed(SDL_SCANCODE_TAB))
    {
        m_settingsTab = (m_settingsTab + 1) % kSettingsTabs;
        m_settingsSelected = 0;
    }
    std::vector<SettingsRow> current = rows();
    const int count = static_cast<int>(current.size());
    SettingsLayout l = settingsLayout(windowW, windowH, count);
    m_settingsSelected = std::clamp(m_settingsSelected, 0, std::max(0, count - 1));
    if (input.IsKeyJustPressed(SDL_SCANCODE_UP) && count > 0)
        m_settingsSelected = (m_settingsSelected - 1 + count) % count;
    if (input.IsKeyJustPressed(SDL_SCANCODE_DOWN) && count > 0)
        m_settingsSelected = (m_settingsSelected + 1) % count;

    auto stepRow = [&](int row, int dir) {
        if (row >= 0 && row < count && current[row].enabled && current[row].step)
            current[row].step(dir);
    };
    // An open list takes the mouse first: a pick sets the value, a click anywhere else only closes it
    const int wheel = input.ConsumeWheel();
    if (m_combo.open >= kComboSide)
        m_combo.open = -1; // one of the custom game dialog's
    if (m_combo.open >= 0)
    {
        if (m_combo.open >= count || current[m_combo.open].options.empty() || !current[m_combo.open].enabled)
        {
            m_combo.open = -1;
        }
        else
        {
            int picked = -1;
            const int openRow = m_combo.open;
            if (ui::ComboUpdate(
                    m_combo,
                    SettingsControlRect(l.rows[openRow]),
                    static_cast<int>(current[openRow].options.size()),
                    windowH,
                    mx,
                    my,
                    clicked,
                    wheel,
                    picked))
            {
                if (picked >= 0 && current[openRow].select)
                    current[openRow].select(picked);
                return;
            }
            if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
            { // Esc closes the list, not the screen
                m_combo.open = -1;
                m_combo.scroll = 0;
                return;
            }
        }
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_LEFT))
        stepRow(m_settingsSelected, -1);
    if (input.IsKeyJustPressed(SDL_SCANCODE_RIGHT) || input.IsConfirmJustPressed())
        stepRow(m_settingsSelected, +1);

    if (clicked)
    {
        for (int i = 0; i < kSettingsTabs; ++i)
        {
            if (l.tabs[i].Contains(mx, my) && i != m_settingsTab)
            {
                m_combo.open = -1;
                m_settingsTab = i;
                m_settingsSelected = 0;
                return;
            }
        }
        if (l.backBtn.Contains(mx, my))
        {
            close();
            return;
        }
        if (l.defaultsBtn.Contains(mx, my))
        {
            ResetSettingsTab(m_settingsTab, *m_settings);
            m_onSettingsChanged(SettingsTabArea(m_settingsTab));
            return;
        }
        for (int i = 0; i < count && i < l.rowCount; ++i)
        {
            if (!l.rows[i].Contains(mx, my))
                continue;
            m_settingsSelected = i;
            const UiRect c = SettingsControlRect(l.rows[i]);
            if (current[i].slider)
            {
                const UiRect track = SliderTrackRect(l.rows[i]);
                if (mx >= track.x - 6 && mx <= track.Right() + 6)
                {
                    m_sliderDrag = i;
                    current[i].setRatio(static_cast<float>(mx - track.x) / track.w);
                }
            }
            else if (!current[i].options.empty())
            {
                // A combo box: the field opens its list
                if (current[i].enabled && c.Contains(mx, my))
                {
                    m_combo.open = i;
                    m_combo.scroll = std::clamp(
                        current[i].current - ui::kComboMaxVisible / 2,
                        0,
                        std::max(0, static_cast<int>(current[i].options.size()) - ui::kComboMaxVisible));
                }
            }
            else
            {
                // Left part of the pill: previous value, anywhere else: next
                stepRow(i, mx < c.x + c.w / 3 ? -1 : +1);
            }
            break;
        }
    }
    if (m_sliderDrag >= 0 && input.IsLeftMouseDown() && m_sliderDrag < count && current[m_sliderDrag].slider)
    {
        const UiRect track = SliderTrackRect(l.rows[m_sliderDrag]);
        current[m_sliderDrag].setRatio(static_cast<float>(mx - track.x) / track.w);
    }
    else if (!input.IsLeftMouseDown())
    {
        m_sliderDrag = -1;
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        close();
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

        if (selected)
        {
            ui::FillHorizontalGradient(renderer, r, ui::WithAlpha(col::Hover, 250), ui::WithAlpha(col::Hover, 0));
            renderer.FillRectScreen(r.x, r.y, 4, r.h, isExit ? col::Danger : col::Accent);
        }
        SDL_Color textCol = selected ? (isExit ? SDL_Color {255, 190, 180, 255} : col::Text) : col::TextDim;
        fontRenderer.DrawString(
            label,
            r.x + 20 + (selected ? 5 : 0),
            r.CenterY(),
            ui::FONT_H2,
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

// Who made the music (assets/data/audio_tracks.json): every author once, with the link or licence given there
std::vector<std::string> MusicCredits()
{
    static std::vector<std::string> credits = [] {
        std::vector<std::string> out;
        std::string text;
        if (!Assets::ReadText(LoaderResources::Tracklist(), text))
            return out;
        const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
        if (!doc.is_object())
            return out;
        for (const char *list : {"mainmenu", "tracks"})
        {
            if (!doc.contains(list) || !doc[list].is_array())
                continue;
            for (const auto &track : doc[list])
            {
                if (!track.is_object() || !track.contains("author"))
                    continue;
                std::string line = "  " + track.value("author", std::string());
                if (track.contains("link"))
                    line += " - " + track.value("link", std::string());
                else if (track.contains("license"))
                    line += " (" + track.value("license", std::string()) + ")";
                if (std::find(out.begin(), out.end(), line) == out.end())
                    out.push_back(line);
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

void MenuController::RenderMenuSettings(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH)
{
    if (!m_settings || !m_resolutions)
    {
        ui::DrawDialog(
            renderer, fontRenderer, windowW, windowH, settingsLayout(windowW, windowH).dialog, _("SETTINGS"));
        return;
    }
    static const std::function<void(SettingsArea)> kNoNotify = [](SettingsArea) {
    };
    Settings scratch = *m_settings; // only read: the rows' value texts, the callbacks are not called here
    std::vector<SettingsRow> rows
        = BuildSettingsRows(m_settingsTab, scratch, m_resolutions(), kNoNotify, m_renderApi, m_inGameSettingsOpen);
    SettingsLayout l = settingsLayout(windowW, windowH, static_cast<int>(rows.size()));
    ui::DrawDialog(renderer, fontRenderer, windowW, windowH, l.dialog, _("SETTINGS"));

    static const char *const kTabNames[kSettingsTabs] = {"VIDEO", "GRAPHICS", "AUDIO", "GAME"};
    for (int i = 0; i < kSettingsTabs; ++i)
    {
        ui::DrawButton(
            renderer,
            fontRenderer,
            l.tabs[i],
            _(kTabNames[i]),
            i == m_settingsTab ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
            l.tabs[i].Contains(m_mouseX, m_mouseY));
    }

    for (int i = 0; i < static_cast<int>(rows.size()) && i < l.rowCount; ++i)
    {
        const SettingsRow &row = rows[i];
        const UiRect &r = l.rows[i];
        const bool hot = r.Contains(m_mouseX, m_mouseY) || i == m_settingsSelected;
        ui::DrawPanel(renderer, r, hot ? col::Hover : col::PanelRaised, hot ? col::BorderStrong : col::Border);
        if (hot)
            renderer.FillRectScreen(r.x, r.y, 3, r.h, col::Accent);
        const SDL_Color text = row.enabled ? col::Text : col::TextMuted;
        // Label and hint keep their places relative to the row's height (rows get lower on long tabs)
        fontRenderer.DrawString(row.label, r.x + 18, r.y + r.h * 10 / 56, ui::FONT_BODY_BOLD, text, false);
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, row.hint, ui::FONT_SMALL, r.w - 360),
            r.x + 18,
            r.y + r.h * 33 / 56,
            ui::FONT_SMALL,
            col::TextDim,
            false);

        const UiRect c = SettingsControlRect(r);
        if (row.slider)
        {
            const UiRect track = SliderTrackRect(r);
            renderer.FillRectScreen(track.x, track.y, track.w, track.h, col::PanelSunken);
            renderer.DrawRectScreen(track.x, track.y, track.w, track.h, col::Border);
            renderer.FillRectScreen(
                track.x, track.y, static_cast<int>(track.w * std::clamp(row.ratio, 0.0f, 1.0f)), track.h, col::Accent);
            const int knobX = track.x + static_cast<int>(track.w * std::clamp(row.ratio, 0.0f, 1.0f));
            renderer.FillRectScreen(knobX - 4, track.CenterY() - 10, 8, 20, col::Text);
            fontRenderer.DrawString(
                row.value,
                c.Right(),
                c.CenterY(),
                ui::FONT_BODY_BOLD,
                col::Accent,
                false,
                Anchor::RIGHT | Anchor::VCENTER);
        }
        else if (!row.options.empty())
        {
            ui::DrawComboField(
                renderer,
                fontRenderer,
                c,
                row.value,
                m_combo.open == i,
                c.Contains(m_mouseX, m_mouseY),
                row.enabled,
                row.valueColor);
        }
        else
        {
            renderer.FillRectScreen(c.x, c.y, c.w, c.h, col::PanelSunken);
            renderer.DrawRectScreen(
                c.x, c.y, c.w, c.h, ui::WithAlpha(row.enabled ? row.valueColor : col::TextMuted, 160));
            fontRenderer.DrawString(
                ui::FitText(fontRenderer, row.value, ui::FONT_BODY_BOLD, c.w - 64),
                c.CenterX(),
                c.CenterY(),
                ui::FONT_BODY_BOLD,
                row.enabled ? row.valueColor : col::TextMuted,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
            if (row.enabled)
            {
                fontRenderer.DrawString(
                    "\xE2\x80\xB9",
                    c.x + 16,
                    c.CenterY() - 1,
                    ui::FONT_BODY_BOLD,
                    col::TextDim,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
                fontRenderer.DrawString(
                    "\xE2\x80\xBA",
                    c.Right() - 16,
                    c.CenterY() - 1,
                    ui::FONT_BODY_BOLD,
                    col::TextDim,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
            }
        }
    }

    fontRenderer.DrawString(
        _("Click: pick from a list. Arrows: step. Tab: next page"),
        l.dialog.x + 20,
        l.backBtn.CenterY(),
        ui::FONT_SMALL,
        col::TextMuted,
        false,
        Anchor::LEFT | Anchor::VCENTER);
    ui::DrawButton(
        renderer,
        fontRenderer,
        l.defaultsBtn,
        _("DEFAULTS"),
        ui::ButtonStyle::Secondary,
        l.defaultsBtn.Contains(m_mouseX, m_mouseY));
    ui::DrawButton(
        renderer,
        fontRenderer,
        l.backBtn,
        _("BACK"),
        ui::ButtonStyle::Secondary,
        l.backBtn.Contains(m_mouseX, m_mouseY));
    // The open list over everything
    if (m_combo.open >= 0 && m_combo.open < static_cast<int>(rows.size()) && m_combo.open < l.rowCount
        && !rows[m_combo.open].options.empty())
    {
        ui::DrawComboList(
            renderer,
            fontRenderer,
            SettingsControlRect(l.rows[m_combo.open]),
            rows[m_combo.open].options,
            rows[m_combo.open].current,
            m_combo,
            windowH,
            m_mouseX,
            m_mouseY);
    }
    ui::DrawConfirm(renderer, fontRenderer, windowW, windowH, m_apiConfirm, m_mouseX, m_mouseY);
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

namespace
{

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
            size_t colon = name.find(": ");
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
        const int titleW = w - 220;
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
            l.layoutBtn,
            m_skirmishPrebuilt ? _("PREBUILT BASES") : _("FROM SCRATCH"),
            m_combo.open == kComboLayout,
            l.layoutBtn.Contains(m_mouseX, m_mouseY));
        renderer.FillRectScreen(x, d.y + 74, w, 1, col::Border);
        ui::DrawCaption(
            fontRenderer,
            renderer,
            x,
            d.y + 82,
            std::string(_("PLAYERS")) + " (" + std::to_string(m_customSlots.size()) + "/" + std::to_string(info.players)
                + ")");
        // Column captions, the same order as in the network room
        for (const auto &[kind, text] : std::initializer_list<std::pair<int, const char *>> {
                 {1, _("Faction")}, {2, _("Team")}, {3, _("Colour")}, {6, _("Pos.")}, {7, _("Handicap")}})
        {
            const UiRect cell = CustomCell(d, 0, kind);
            fontRenderer.DrawString(text, cell.x, d.y + 90, ui::FONT_SMALL, col::TextMuted, false);
        }
        for (int i = 0; i < static_cast<int>(m_customSlots.size()); ++i)
        {
            const CustomSlot &cs = m_customSlots[i];
            const UiRect row = CustomRow(d, i);
            ui::DrawPanel(renderer, row, i == 0 ? col::Hover : col::PanelRaised, i == 0 ? col::Accent : col::Border);
            auto combo = [&](int kind, const std::string &text, SDL_Color c = col::Text) {
                const UiRect cell = CustomCell(d, i, kind);
                ui::DrawComboField(
                    renderer,
                    fontRenderer,
                    cell,
                    text,
                    m_combo.open == kComboSlot + i * 10 + kind,
                    cell.Contains(m_mouseX, m_mouseY),
                    true,
                    c);
            };
            if (i == 0)
            {
                fontRenderer.DrawString(
                    _("YOU"),
                    row.x + 10,
                    row.CenterY(),
                    ui::FONT_BODY_BOLD,
                    col::Text,
                    false,
                    Anchor::LEFT | Anchor::VCENTER);
                combo(1, PlayerFaction() == Faction::Confederation ? _("Confederation") : _("Resistance"));
            }
            else
            {
                combo(
                    4,
                    std::string(_("AI")) + " (" + DifficultyName(static_cast<AIDifficulty>(std::clamp(cs.level, 0, 3)))
                        + ")",
                    col::Info);
                combo(1, cs.faction == 0 ? _("Random") : (cs.faction == 1 ? _("Resistance") : _("Confederation")));
                const UiRect x = CustomCell(d, i, 5);
                ui::DrawButton(renderer, fontRenderer, x, "x", ui::ButtonStyle::Danger, x.Contains(m_mouseX, m_mouseY));
            }
            combo(2, std::to_string(cs.team + 1));
            combo(6, std::to_string(cs.position + 1));
            combo(7, cs.handicap == 0 ? std::string("-") : HandicapText(cs.handicap));
            combo(3, "");
            const UiRect cc = CustomCell(d, i, 3);
            renderer.FillRectScreen(cc.x + 6, cc.y + 6, cc.w - 28, cc.h - 12, ArmyColor(cs.color));
            renderer.DrawRectScreen(cc.x + 6, cc.y + 6, cc.w - 28, cc.h - 12, col::BorderStrong);
        }
        if (static_cast<int>(m_customSlots.size()) < info.players)
        {
            const UiRect add = CustomAddBtn(d, static_cast<int>(m_customSlots.size()));
            ui::DrawButton(
                renderer,
                fontRenderer,
                add,
                std::string("+ ") + _("COMPUTER PLAYER"),
                ui::ButtonStyle::Secondary,
                add.Contains(m_mouseX, m_mouseY));
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
            const float sc = std::min((pv.w - 4) / static_cast<float>(info.width), (pv.h - 4) / static_cast<float>(info.height));
            const float ox = pv.CenterX() - info.width * sc / 2, oy = pv.CenterY() - info.height * sc / 2;
            for (int n = 0; n < static_cast<int>(m_spots.size()); ++n)
            {
                SDL_Color c = col::TextDim;
                for (const CustomSlot &cs : m_customSlots)
                    if (cs.position == n)
                        c = ArmyColor(cs.color);
                const int sx = static_cast<int>(ox + (m_spots[n].first + 0.5f) * sc);
                const int sy = static_cast<int>(oy + (m_spots[n].second + 0.5f) * sc);
                renderer.FillRectScreen(sx - 8, sy - 8, 16, 16, c);
                renderer.DrawRectScreen(sx - 8, sy - 8, 16, 16, col::Text);
                fontRenderer.DrawString(
                    std::to_string(n + 1), sx, sy, ui::FONT_SMALL_BOLD, col::AccentText, false, Anchor::HCENTER | Anchor::VCENTER);
            }
        }
        const std::string size = std::to_string(info.width) + " x " + std::to_string(info.height);
        DrawTextSection(
            renderer, fontRenderer, pv.x, pv.Bottom() + 12, pv.w, _("MAP SIZE:"), size, col::Ok, ui::FONT_BODY);
        ui::DrawCheckbox(
            renderer,
            fontRenderer,
            l.fogCheckbox,
            _("FOG OF WAR"),
            m_skirmishFogOfWar,
            l.fogCheckbox.Contains(m_mouseX, m_mouseY));
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
            renderer.GetSdlRenderer(), texManager, previewKey, LoaderResources::MapPath(data.mapFile), markers, pw, ph);
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
            = id >= kComboSlot
                  ? CustomCell(l.details, (id - kComboSlot) / 10, (id - kComboSlot) % 10)
                  : (id == kComboSide ? l.sideBtn[0] : (id == kComboDifficulty ? l.difficultyBtn : l.layoutBtn));
        std::vector<std::string> items = CampaignComboOptions(id);
        std::vector<bool> taken;
        std::vector<SDL_Color> swatches;
        const bool colors = id >= kComboSlot && (id - kComboSlot) % 10 == 3;
        if (colors)
        {
            taken = CustomTakenColors((id - kComboSlot) / 10);
            const int current = CampaignComboCurrent(id);
            for (int i = 1; i <= Settings::kArmyColors; ++i)
                swatches.push_back(ArmyColor(i));
            for (size_t i = 0; i < items.size() && i < taken.size(); ++i)
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
        if (kind == 1)
        {
            if (row > 0)
                out.push_back(_("Random"));
            out.push_back(_("Resistance"));
            out.push_back(_("Confederation"));
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
        else if (kind == 6)
        {
            const int count = m_skirmishSelected >= 0 && m_skirmishSelected < static_cast<int>(m_skirmishMaps.size())
                ? std::clamp(m_skirmishMaps[m_skirmishSelected].players, 2, kMaxPlayers)
                : kMaxPlayers;
            for (int i = 1; i <= count; ++i)
                out.push_back(std::string(_("Position")) + " " + std::to_string(i));
        }
        else
        {
            for (int i = 0; i < 4; ++i)
                out.push_back(DifficultyName(static_cast<AIDifficulty>(i)));
        }
        return out;
    }
    if (id == kComboSide)
        return {_("RESISTANCE"), _("CONFEDERACY")};
    if (id == kComboDifficulty)
    {
        return {
            DifficultyName(AIDifficulty::Easy),
            DifficultyName(AIDifficulty::Medium),
            DifficultyName(AIDifficulty::Hard),
            DifficultyName(AIDifficulty::Hardcore)};
    }
    return {_("PREBUILT BASES"), _("FROM SCRATCH")};
}

int MenuController::CampaignComboCurrent(int id) const
{
    if (id >= kComboSlot)
    {
        const int row = (id - kComboSlot) / 10, kind = (id - kComboSlot) % 10;
        if (row >= static_cast<int>(m_customSlots.size()))
            return 0;
        const CustomSlot &cs = m_customSlots[row];
        if (kind == 1)
            return row == 0 ? (PlayerFaction() == Faction::Confederation ? 1 : 0) : cs.faction;
        if (kind == 2)
            return cs.team;
        if (kind == 3)
            return cs.color - 1;
        if (kind == 6)
            return cs.position;
        if (kind == 7)
            return std::clamp(cs.handicap, 0, kHandicapCount - 1);
        return cs.level;
    }
    if (id == kComboSide)
        return PlayerFaction() == Faction::Confederation ? 1 : 0;
    if (id == kComboDifficulty)
        return std::clamp(static_cast<int>(m_difficulty), 0, 3);
    return m_skirmishPrebuilt ? 0 : 1;
}

void MenuController::CampaignComboApply(int id, int index)
{
    if (id >= kComboSlot)
    {
        const int row = (id - kComboSlot) / 10, kind = (id - kComboSlot) % 10;
        if (row >= static_cast<int>(m_customSlots.size()))
            return;
        CustomSlot &cs = m_customSlots[row];
        if (kind == 1)
        {
            if (row == 0)
                SetPlayerFaction(index == 1 ? Faction::Confederation : Faction::Resistance);
            else
                cs.faction = index;
        }
        else if (kind == 2)
        {
            cs.team = std::clamp(index, 0, kMaxPlayers - 1);
        }
        else if (kind == 3)
        {
            const std::vector<bool> taken = CustomTakenColors(row);
            if (index >= 0 && index < static_cast<int>(taken.size()) && !taken[index])
                cs.color = index + 1;
        }
        else if (kind == 7)
        {
            cs.handicap = std::clamp(index, 0, kHandicapCount - 1);
        }
        else if (kind == 6)
        {
            std::vector<int> positions;
            for (const CustomSlot &s : m_customSlots)
                positions.push_back(s.position);
            ApplyCustomPosition(positions, row, index);
            for (size_t i = 0; i < positions.size(); ++i)
                m_customSlots[i].position = positions[i];
        }
        else
        {
            cs.level = std::clamp(index, 0, 3);
        }
        return;
    }
    if (id == kComboSide)
    {
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
    else
    {
        m_skirmishPrebuilt = index == 0;
    }
}

void MenuController::ApplyCustomPosition(std::vector<int> &positions, int row, int position)
{
    if (row < 0 || row >= static_cast<int>(positions.size()) || position < 0 || position >= kMaxPlayers)
        return;
    for (size_t i = 0; i < positions.size(); ++i)
    {
        if (static_cast<int>(i) != row && positions[i] == position)
        {
            positions[i] = positions[row]; // the holder takes the place that was left
            break;
        }
    }
    positions[row] = position;
}

// Colours the other players of the custom game already wear (index 0 = colour 1)
std::vector<bool> MenuController::CustomTakenColors(int row) const
{
    std::vector<bool> taken(Settings::kArmyColors, false);
    for (int i = 0; i < static_cast<int>(m_customSlots.size()); ++i)
    {
        const int c = m_customSlots[i].color;
        if (i != row && c >= 1 && c <= Settings::kArmyColors)
            taken[c - 1] = true;
    }
    return taken;
}

// The person plus at least one computer player, never more than the map has start positions
void MenuController::EnsureCustomSlots()
{
    if (m_skirmishSelected < 0 || m_skirmishSelected >= static_cast<int>(m_skirmishMaps.size()))
        return;
    const size_t most = static_cast<size_t>(std::clamp(m_skirmishMaps[m_skirmishSelected].players, 2, kMaxPlayers));
    if (m_customSlots.empty())
    {
        m_customSlots.resize(2);
        m_customSlots[0].color = 1;
        m_customSlots[1].color = 2;
        m_customSlots[1].team = 1;
        m_customSlots[1].level = std::clamp(static_cast<int>(m_difficulty), 0, 3);
        m_customSlots[1].position = 1;
    }
    if (m_customSlots.size() > most)
        m_customSlots.resize(most);
    // Positions the map does not have, or held twice: the players take the lowest free ones
    std::vector<bool> used(kMaxPlayers, false);
    for (CustomSlot &cs : m_customSlots)
    {
        if (cs.position < 0 || cs.position >= static_cast<int>(most) || used[cs.position])
            cs.position = -1;
        else
            used[cs.position] = true;
    }
    for (CustomSlot &cs : m_customSlots)
    {
        if (cs.position >= 0)
            continue;
        for (int pos = 0; pos < static_cast<int>(most); ++pos)
        {
            if (!used[pos])
            {
                cs.position = pos;
                used[pos] = true;
                break;
            }
        }
    }
}

} // namespace OpenArtOfWar
