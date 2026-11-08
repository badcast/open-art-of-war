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

// ---- Settings screen --------------------------------------------------------------------------------------------
constexpr int kSettingsTabs = 4;

// The dialog has the same size on every tab; a tab with more rows than fit scrolls (wheel, scroll bar, arrows)
struct SettingsLayout
{
    static constexpr int kRowH = 56;
    static constexpr int kGap = 6;
    UiRect dialog;
    UiRect tabs[kSettingsTabs];
    UiRect list;     // where the rows are
    UiRect track;    // the scroll bar along its right edge
    int visible {1}; // rows that fit
    UiRect defaultsBtn;
    UiRect backBtn;

    // Row `i` of the tab with row `top` first in view; empty (w = 0) when it is scrolled out of view
    UiRect Row(int i, int top) const
    {
        const int slot = i - top;
        if (slot < 0 || slot >= visible)
            return {};
        return {list.x, list.y + slot * (kRowH + kGap), list.w, kRowH};
    }
};

SettingsLayout settingsLayout(int windowW, int windowH)
{
    SettingsLayout l;
    l.dialog = ui::CenteredRect(windowW, windowH, 780, std::min(640, std::max(420, windowH - 40)));
    const int tabW = (l.dialog.w - 40 - (kSettingsTabs - 1) * 6) / kSettingsTabs;
    for (int i = 0; i < kSettingsTabs; ++i)
        l.tabs[i] = {l.dialog.x + 20 + i * (tabW + 6), l.dialog.y + 60, tabW, 36};
    l.backBtn = {l.dialog.Right() - 20 - 160, l.dialog.Bottom() - 20 - 40, 160, 40};
    l.defaultsBtn = {l.backBtn.x - 12 - 200, l.backBtn.y, 200, 40};
    const int top = l.dialog.y + 60 + 36 + 14;
    const int space = l.backBtn.y - 12 - top;
    l.visible = std::max(1, (space + SettingsLayout::kGap) / (SettingsLayout::kRowH + SettingsLayout::kGap));
    // (the rows leave room for the scroll bar on the right)
    l.list
        = {l.dialog.x + 20,
           top,
           l.dialog.w - 40 - 14,
           l.visible * (SettingsLayout::kRowH + SettingsLayout::kGap) - SettingsLayout::kGap};
    l.track = {l.list.Right() + 8, l.list.y, 4, l.list.h};
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
        r.value = n > 0 ? options[static_cast<std::size_t>(current)] : std::move(fallbackValue);
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
        for (std::size_t i = 0; i < resolutions.size(); ++i)
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
                w = resolutions[static_cast<std::size_t>(i)].w;
                h = resolutions[static_cast<std::size_t>(i)].h;
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
            for (std::size_t i = 0; i < usable.size(); ++i)
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
                    select(usable[static_cast<std::size_t>(i)].name);
                },
                usable.size() > 1,
                value);
        }
        // Shader effects: only the SDL GPU render API draws them
        const bool fx = renderApi.effectsAvailable && renderApi.effectsAvailable();
        const std::string fxHint = _("Needs the SDL GPU render API");
        // (night is the time of day of the match, chosen with the map: not a player's own setting)
        static const char *const kGrades[4] = {"Off", "Natural", "Vivid", "Cinematic"};
        static const char *const kGradeHints[4]
            = {"The picture as drawn",
               "A little more contrast and colour",
               "Strong colours",
               "Muted colours, warm light, dark corners"};
        const int g = std::clamp(s.colorGrade, 0, 3);
        std::vector<std::string> gradeNames;
        for (const char *name : kGrades)
            gradeNames.push_back(_(name));
        choiceList(
            _("Color grading"),
            fx ? std::string(_(kGradeHints[g])) : fxHint,
            gradeNames,
            g,
            [&s, notify](int i) {
                s.colorGrade = std::clamp(i, 0, 3);
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
            _("General volume"),
            _("Volume of all the sound: music, surroundings, effects, interface and voices"),
            static_cast<float>(s.generalVolume),
            0.0f,
            128.0f,
            8.0f,
            percent,
            [&s, notify](float v) {
                s.generalVolume = static_cast<int>(v);
                notify(SettingsArea::Audio);
            }));
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
            _("Ambient volume"),
            _("Volume of the surroundings in a match: the forest by day or by night"),
            static_cast<float>(s.ambientVolume),
            0.0f,
            128.0f,
            8.0f,
            percent,
            [&s, notify](float v) {
                s.ambientVolume = static_cast<int>(v);
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
        s.generalVolume = d.generalVolume;
        s.musicVolume = d.musicVolume;
        s.fxVolume = d.fxVolume;
        s.voiceVolume = d.voiceVolume;
        s.ambientVolume = d.ambientVolume;
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

} // namespace

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
        m_apiConfirm.modalResult = -1;
        m_apiConfirm.title = std::string(_("SWITCH RENDER API?"));
        m_apiConfirm.text
            = m_inGameSettingsOpen
                  ? std::string(_("The render API changes at the next start of the game: "))
                        + RenderBackend::Label(name)
                  : std::string(_("The window is recreated with this render API: ")) + RenderBackend::Label(name)
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
        m_settingsTop = 0;
    }
    std::vector<SettingsRow> current = rows();
    const int count = static_cast<int>(current.size());
    const SettingsLayout l = settingsLayout(windowW, windowH);
    const int maxTop = std::max(0, count - l.visible);
    m_settingsSelected = std::clamp(m_settingsSelected, 0, std::max(0, count - 1));
    bool moved = false;
    if (input.IsKeyJustPressed(SDL_SCANCODE_UP) && count > 0)
    {
        m_settingsSelected = (m_settingsSelected - 1 + count) % count;
        moved = true;
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_DOWN) && count > 0)
    {
        m_settingsSelected = (m_settingsSelected + 1) % count;
        moved = true;
    }
    // The row picked with the arrows scrolls into view
    if (moved && m_settingsSelected < m_settingsTop)
        m_settingsTop = m_settingsSelected;
    if (moved && m_settingsSelected >= m_settingsTop + l.visible)
        m_settingsTop = m_settingsSelected - l.visible + 1;
    m_settingsTop = std::clamp(m_settingsTop, 0, maxTop);

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
        if (m_combo.open >= count || current[m_combo.open].options.empty() || !current[m_combo.open].enabled
            || l.Row(m_combo.open, m_settingsTop).w == 0)
        {
            m_combo.open = -1;
        }
        else
        {
            int picked = -1;
            const int openRow = m_combo.open;
            if (ui::ComboUpdate(
                    m_combo,
                    SettingsControlRect(l.Row(openRow, m_settingsTop)),
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
    // No list open: the wheel scrolls the rows; the scroll bar can be dragged
    if (m_combo.open < 0 && wheel != 0 && maxTop > 0)
        m_settingsTop = std::clamp(m_settingsTop - wheel, 0, maxTop);
    if (maxTop > 0)
    {
        int px = 0, py = 0;
        if (input.ConsumeLeftPress(px, py) && input.IsLeftMouseDown() && OnScrollBar(l.track, px, py))
            m_scrollDrag = 4;
        if (m_scrollDrag == 4)
        {
            if (input.IsLeftMouseDown())
                m_settingsTop = ScrollTopFromMouse(l.track, my, count, l.visible);
            else
                m_scrollDrag = 0;
            return;
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
                m_settingsTop = 0;
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
        for (int i = 0; i < count; ++i)
        {
            const UiRect row = l.Row(i, m_settingsTop);
            if (!row.Contains(mx, my))
                continue;
            m_settingsSelected = i;
            const UiRect c = SettingsControlRect(row);
            if (current[i].slider)
            {
                const UiRect track = SliderTrackRect(row);
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
    if (m_sliderDrag >= 0 && input.IsLeftMouseDown() && m_sliderDrag < count && current[m_sliderDrag].slider
        && l.Row(m_sliderDrag, m_settingsTop).w > 0)
    {
        const UiRect track = SliderTrackRect(l.Row(m_sliderDrag, m_settingsTop));
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
    const SettingsLayout l = settingsLayout(windowW, windowH);
    const int count = static_cast<int>(rows.size());
    const int top = std::clamp(m_settingsTop, 0, std::max(0, count - l.visible));
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

    for (int i = top; i < count && i < top + l.visible; ++i)
    {
        const SettingsRow &row = rows[i];
        const UiRect r = l.Row(i, top);
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
    // A tab longer than the dialog: its scroll bar
    if (count > l.visible)
    {
        ui::DrawScrollBar(
            renderer,
            l.track,
            static_cast<float>(top),
            static_cast<float>(count),
            static_cast<float>(l.visible),
            OnScrollBar(l.track, m_mouseX, m_mouseY) || m_scrollDrag == 4);
    }
    // The open list over everything
    if (m_combo.open >= 0 && m_combo.open < count && l.Row(m_combo.open, top).w > 0
        && !rows[m_combo.open].options.empty())
    {
        ui::DrawComboList(
            renderer,
            fontRenderer,
            SettingsControlRect(l.Row(m_combo.open, top)),
            rows[m_combo.open].options,
            rows[m_combo.open].current,
            m_combo,
            windowH,
            m_mouseX,
            m_mouseY);
    }
    ui::DrawConfirm(renderer, fontRenderer, windowW, windowH, m_apiConfirm, m_mouseX, m_mouseY);
}

} // namespace OpenArtOfWar
