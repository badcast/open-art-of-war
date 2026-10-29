#include "ui/MenuController.hpp"
#include <nlohmann/json.hpp>
#include "core/AssetStore.hpp"
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

        constexpr int kMainMenuItems = 8;
        constexpr int kPauseItems = 6;

        const char *const kMainMenuLabels[kMainMenuItems] = {"CAMPAIGN", "CUSTOM GAME", "NETWORK GAME", "TEST MAP", "SETTINGS", "HELP", "ABOUT", "EXIT"};

        int mainMenuColumnX(int windowW)
        {
            return std::clamp(windowW / 14, 32, 120);
        }

        int mainMenuTitleY(int windowH)
        {
            return std::max(36, windowH / 9);
        }

        UiRect mainMenuItemRect(int i, int windowW, int windowH)
        {
            const int itemH = 44;
            const int gap = 6;
            int startY = mainMenuTitleY(windowH) + 128;
            int maxStart = windowH - 56 - kMainMenuItems * (itemH + gap);
            startY = std::max(mainMenuTitleY(windowH) + 96, std::min(startY, maxStart));
            return {mainMenuColumnX(windowW), startY + i * (itemH + gap), 340, itemH};
        }

        // Starts openaow-mapeditor (next to the game, or in the working directory) with `mapPath`; false when it is missing
        bool launchMapEditor(const std::string &mapPath)
        {
#ifdef _WIN32
            const char *exeName = "openaow-mapeditor.exe";
#else
            const char *exeName = "openaow-mapeditor";
#endif
            std::vector<std::filesystem::path> candidates;
            if(const char *base = SDL_GetBasePath())
                candidates.push_back(std::filesystem::path(base) / exeName);
            candidates.push_back(std::filesystem::path(".") / exeName);
            candidates.push_back(std::filesystem::path("build") / exeName);
            std::error_code ec;
            for(const auto &exe : candidates)
            {
                if(!std::filesystem::is_regular_file(exe, ec))
                    continue;
                const std::string exePath = std::filesystem::absolute(exe, ec).string();
                const char *args[] = {exePath.c_str(), mapPath.c_str(), nullptr};
                SDL_Process *process = SDL_CreateProcess(args, false);
                if(!process)
                    continue;
                SDL_DestroyProcess(process); // the editor lives on its own
                return true;
            }
            return false;
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
            UiRect layoutBtn;     // maps with buildings of their own: prebuilt bases or from scratch (top right of the details)
            int itemH {40};
            int itemGap {4};

            UiRect item(int i) const
            {
                return {list.x, list.y + i * (itemH + itemGap), list.w, itemH};
            }
        };

        CampaignLayout campaignLayout(int windowW, int windowH, int missionCount)
        {
            CampaignLayout l;
            l.dialog = ui::centeredRect(windowW, windowH, 1080, 640);
            int contentY = l.dialog.y + 52 + 16;
            int footerY = l.dialog.bottom() - 64;
            l.list = {l.dialog.x + 20, contentY, 300, footerY - contentY - 8};
            int n = std::max(1, missionCount);
            l.itemH = std::clamp((l.list.h - (n - 1) * l.itemGap) / n, 26, 44);
            l.details = {l.list.right() + 16, contentY, l.dialog.right() - 20 - (l.list.right() + 16), l.list.h};
            l.backBtn = {l.dialog.right() - 20 - 140, footerY + 10, 140, 40};
            l.startBtn = {l.backBtn.x - 12 - 200, footerY + 10, 200, 40};
            l.sideBtn[0] = {l.dialog.x + 20, footerY + 10, 240, 40};
            l.editorBtn = {l.startBtn.x - 12 - 140, footerY + 10, 140, 40};
            l.layoutBtn = {l.details.right() - 20 - 210, l.details.y + 12, 210, 32};
            l.difficultyBtn = {l.sideBtn[0].right() + 12, footerY + 10, std::max(180, l.editorBtn.x - 12 - (l.sideBtn[0].right() + 12)), 40};
            return l;
        }

        // The custom game list shows as many fixed-height rows as fit and scrolls
        int customListRows(CampaignLayout &l)
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
            l.dialog = ui::centeredRect(windowW, windowH, 380, h);
            for(int i = 0; i < kPauseItems; ++i)
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
            l.next = {l.panel.right() - 40, l.panel.y, 40, 40};
            l.name = {l.prev.right() + 8, l.panel.y, l.panel.w - 2 * 40 - 16, 40};
            l.seek = {l.panel.x, l.panel.y - 30, l.panel.w, 22};
            return l;
        }

        std::string formatTime(double seconds)
        {
            const int s = std::max(0, static_cast<int>(seconds));
            char buf[16];
            std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
            return buf;
        }

        // First visible line for a thumb dragged to mouse height `my` along the scroll bar `track`
        int scrollTopFromMouse(const UiRect &track, int my, int total, int visible)
        {
            if(total <= visible)
                return 0;
            const int thumbH = std::max(20, track.h * visible / total);
            const float ratio = std::clamp(static_cast<float>(my - track.y - thumbH / 2) / std::max(1, track.h - thumbH), 0.0f, 1.0f);
            return static_cast<int>(ratio * (total - visible) + 0.5f);
        }

        // Scroll bars are 4px wide: grab them from a wider strip
        bool onScrollBar(const UiRect &track, int mx, int my)
        {
            return track.w > 0 && mx >= track.x - 8 && mx <= track.right() + 8 && my >= track.y && my <= track.bottom();
        }

        // Returns true when the mouse took the click (or holds the seek bar)
        bool updateMusicPlayer(bool clicked, bool pressed, int px, int py, bool down, int mx, int my, int windowH, bool withSeek, SoundEngine &sound, int &drag)
        {
            const PlayerLayout l = playerLayout(windowH);
            if(drag == 3)
            {
                if(!down)
                {
                    drag = 0;
                    return clicked;
                } // the release click ends the drag, it presses nothing
                // Seeking re-reads the stream: follow the mouse a few times a second, not every frame
                static Uint64 lastSeek = 0;
                const Uint64 now = SDL_GetTicks();
                if(now - lastSeek >= 120)
                {
                    lastSeek = now;
                    sound.seekTrack(static_cast<double>(mx - l.seek.x) / l.seek.w);
                }
                return true;
            }
            if(withSeek && pressed && down && l.seek.contains(px, py))
            {
                sound.seekTrack(static_cast<double>(px - l.seek.x) / l.seek.w);
                drag = 3;
                return true;
            }
            if(!clicked)
                return false;
            if(l.prev.contains(mx, my))
            {
                sound.prevTrack();
                return true;
            }
            if(l.next.contains(mx, my))
            {
                sound.nextTrack();
                return true;
            }
            if(withSeek && l.seek.contains(mx, my))
                return true;
            return l.panel.contains(mx, my);
        }

        void renderMusicPlayer(Renderer &renderer, BitmapFontRenderer &fontRenderer, const SoundEngine &sound, int windowH, bool withSeek, int mx, int my)
        {
            const PlayerLayout l = playerLayout(windowH);
            renderer.fillRectScreen(l.name.x, l.name.y, l.name.w, l.name.h, col::PanelSunken);
            renderer.drawRectScreen(l.name.x, l.name.y, l.name.w, l.name.h, col::Border);
            fontRenderer.drawString(ui::fitText(fontRenderer, sound.getCurrentTrackName(), ui::FONT_SMALL, l.name.w - 16), l.name.centerX(), l.name.centerY(), ui::FONT_SMALL, col::TextDim, false, Anchor::HCENTER | Anchor::VCENTER);
            ui::drawButton(renderer, fontRenderer, l.prev, "<", ui::ButtonStyle::Secondary, l.prev.contains(mx, my));
            ui::drawButton(renderer, fontRenderer, l.next, ">", ui::ButtonStyle::Secondary, l.next.contains(mx, my));
            if(!withSeek)
                return;

            const double length = sound.getTrackLength();
            const double ratio = length > 0.0 ? std::clamp(sound.getTrackPosition() / length, 0.0, 1.0) : 0.0;
            const std::string elapsed = formatTime(sound.getTrackPosition());
            const std::string total = formatTime(length);
            const int labelW = 44;
            const int barX = l.seek.x + labelW, barW = l.seek.w - 2 * labelW;
            const int barY = l.seek.centerY() - 2;
            fontRenderer.drawString(elapsed, l.seek.x, l.seek.centerY(), ui::FONT_SMALL, col::TextDim, false, Anchor::LEFT | Anchor::VCENTER);
            fontRenderer.drawString(total, l.seek.right(), l.seek.centerY(), ui::FONT_SMALL, col::TextDim, false, Anchor::RIGHT | Anchor::VCENTER);
            renderer.fillRectScreen(barX, barY, barW, 4, col::PanelSunken);
            renderer.fillRectScreen(barX, barY, static_cast<int>(barW * ratio), 4, col::Accent);
            renderer.fillRectScreen(barX + static_cast<int>(barW * ratio) - 3, l.seek.centerY() - 7, 6, 14, col::Text);
        }

        UiRect infoDialogRect(int windowW, int windowH)
        {
            return ui::centeredRect(windowW, windowH, 700, 500);
        }

        UiRect dialogFooterButton(const UiRect &dialog, int w = 180)
        {
            return {dialog.right() - 20 - w, dialog.bottom() - 20 - 40, w, 40};
        }

        UiRect briefingRect(int windowW, int windowH)
        {
            return ui::centeredRect(windowW, windowH, 820, 480);
        }

        UiRect objectivesRect(int windowW, int windowH)
        {
            return ui::centeredRect(windowW, windowH, 680, 480);
        }

        std::string difficultyName(AIDifficulty d)
        {
            switch(d)
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

        std::string difficultyDesc(AIDifficulty d)
        {
            switch(d)
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
            l.dialog = ui::centeredRect(windowW, windowH, 780, l.rowCount > 6 ? 700 : 560);
            const int tabW = (l.dialog.w - 40 - (kSettingsTabs - 1) * 6) / kSettingsTabs;
            for(int i = 0; i < kSettingsTabs; ++i)
                l.tabs[i] = {l.dialog.x + 20 + i * (tabW + 6), l.dialog.y + 60, tabW, 36};
            l.backBtn = {l.dialog.right() - 20 - 160, l.dialog.bottom() - 20 - 40, 160, 40};
            l.defaultsBtn = {l.backBtn.x - 12 - 200, l.backBtn.y, 200, 40};
            const int top = l.dialog.y + 60 + 36 + 14;
            const int gap = 6;
            const int space = l.backBtn.y - 12 - top;
            const int rowH = std::clamp((space - gap * (l.rowCount - 1)) / l.rowCount, 40, 56);
            for(int i = 0; i < l.rowCount; ++i)
                l.rows[i] = {l.dialog.x + 20, top + i * (rowH + gap), l.dialog.w - 40, rowH};
            return l;
        }

        // The control of a row: a value pill with arrows, or a slider with its value
        UiRect settingsControlRect(const UiRect &row)
        {
            return {row.right() - 16 - 320, row.centerY() - 16, 320, 32};
        }

        UiRect sliderTrackRect(const UiRect &row)
        {
            const UiRect c = settingsControlRect(row);
            return {c.x, c.centerY() - 4, c.w - 78, 8};
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

        std::string onOff(bool on)
        {
            return on ? _("ON") : _("OFF");
        }

        std::string number(float v, int decimals)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
            return buf;
        }

        // A slider over [lo, hi] in steps of `notch`; `commit` stores the new value
        SettingsRow sliderRow(std::string label, std::string hint, float value, float lo, float hi, float notch, std::function<std::string(float)> show, std::function<void(float)> commit)
        {
            SettingsRow row;
            row.label = std::move(label);
            row.hint = std::move(hint);
            row.slider = true;
            row.ratio = (value - lo) / (hi - lo);
            row.value = show(value);
            row.step = [=](int dir) { commit(std::clamp(value + dir * notch, lo, hi)); };
            row.setRatio = [=](float r)
            {
                const float raw = lo + std::clamp(r, 0.0f, 1.0f) * (hi - lo);
                commit(std::clamp(std::round(raw / notch) * notch, lo, hi));
            };
            return row;
        }

        std::vector<SettingsRow> buildSettingsRows(int tab, Settings &s, const std::vector<Resolution> &resolutions, const std::function<void(SettingsArea)> &notify, const RenderApiBinding &renderApi, bool inGame)
        {
            std::vector<SettingsRow> rows;
            // A row with a list of choices (shown as a combo box); the arrow keys still step through them
            auto choiceList = [&](std::string label, std::string hint, std::vector<std::string> options, int current, std::function<void(int)> select, bool enabled = true, std::string fallbackValue = {})
            {
                SettingsRow r;
                r.label = std::move(label);
                r.hint = std::move(hint);
                const int n = static_cast<int>(options.size());
                current = n > 0 ? std::clamp(current, 0, n - 1) : 0;
                r.value = n > 0 ? options[static_cast<size_t>(current)] : std::move(fallbackValue);
                r.options = std::move(options);
                r.current = current;
                r.select = select;
                r.step = [select, n, current](int dir)
                {
                    if(n > 0)
                        select((current + dir + n) % n);
                };
                r.enabled = enabled;
                rows.push_back(std::move(r));
            };
            // On / off as a list of two
            auto toggle = [&](std::string label, std::string hint, bool value, std::function<void(bool)> set, bool enabled = true) { choiceList(std::move(label), std::move(hint), {_("ON"), _("OFF")}, value ? 0 : 1, [set](int i) { set(i == 0); }, enabled); };
            auto choice = [&](std::string label, std::string hint, std::string value, std::function<void(int)> step, bool enabled = true)
            {
                SettingsRow r;
                r.label = std::move(label);
                r.hint = std::move(hint);
                r.value = std::move(value);
                r.step = std::move(step);
                r.enabled = enabled;
                rows.push_back(std::move(r));
            };

            if(tab == 0)
            { // VIDEO
                static const char *const kModes[3] = {"Windowed", "Borderless", "Fullscreen"};
                choiceList(
                    _("Window mode"),
                    _("Window, borderless fullscreen or exclusive fullscreen"),
                    {_(kModes[0]), _(kModes[1]), _(kModes[2])},
                    static_cast<int>(s.windowMode),
                    [&s, notify](int i)
                    {
                        s.windowMode = static_cast<WindowMode>(std::clamp(i, 0, 2));
                        notify(SettingsArea::Video);
                    });

                const bool exclusive = s.windowMode == WindowMode::Exclusive;
                const bool borderless = s.windowMode == WindowMode::Borderless;
                int &w = exclusive ? s.fullscreenW : s.windowW;
                int &h = exclusive ? s.fullscreenH : s.windowH;
                int index = 0;
                long best = -1;
                for(size_t i = 0; i < resolutions.size(); ++i)
                { // the listed size nearest to the current one
                    const long d = static_cast<long>(resolutions[i].w - w) * (resolutions[i].w - w) + static_cast<long>(resolutions[i].h - h) * (resolutions[i].h - h);
                    if(best < 0 || d < best)
                    {
                        best = d;
                        index = static_cast<int>(i);
                    }
                }
                const std::string value = borderless ? std::string(_("Desktop")) : std::to_string(w) + " x " + std::to_string(h);
                std::vector<std::string> sizes;
                for(const Resolution &r : resolutions)
                    sizes.push_back(std::to_string(r.w) + " x " + std::to_string(r.h));
                choiceList(
                    _("Resolution"),
                    exclusive ? _("Display mode of the monitor") : _("Size of the game window"),
                    sizes,
                    index,
                    [&w, &h, resolutions, notify](int i)
                    {
                        if(i < 0 || i >= static_cast<int>(resolutions.size()))
                            return;
                        w = resolutions[static_cast<size_t>(i)].w;
                        h = resolutions[static_cast<size_t>(i)].h;
                        notify(SettingsArea::Video);
                    },
                    !borderless,
                    value);

                toggle(
                    _("VSync"),
                    _("Wait for the monitor refresh, no tearing"),
                    s.vsync,
                    [&s, notify](bool on)
                    {
                        s.vsync = on;
                        notify(SettingsArea::Video);
                    });

                static const int kLimits[5] = {0, 30, 60, 120, 144};
                int li = 0;
                for(int i = 0; i < 5; ++i)
                    if(kLimits[i] == s.fpsLimit)
                        li = i;
                choiceList(
                    _("FPS limit"),
                    _("Cap on frames per second"),
                    {_("Unlimited"), "30", "60", "120", "144"},
                    li,
                    [&s, notify](int i)
                    {
                        s.fpsLimit = kLimits[std::clamp(i, 0, 4)];
                        notify(SettingsArea::Video);
                    });

                toggle(
                    _("Show FPS"),
                    _("Frame counter in the corner of the screen"),
                    s.showFps,
                    [&s, notify](bool on)
                    {
                        s.showFps = on;
                        notify(SettingsArea::Video);
                    });
            }
            else if(tab == 1)
            { // GRAPHICS: quality preset and render API
                static const char *const kQuality[3] = {"Low", "Medium", "High"};
                static const char *const kQualityHint[3] = {"Sprites at 1/3 of the resolution", "Sprites at 1/2 of the resolution", "Original resolution"};
                const int q = std::clamp(s.graphicsQuality, 0, 2);
                choiceList(
                    _("Graphics quality"),
                    _(kQualityHint[q]),
                    {_(kQuality[0]), _(kQuality[1]), _(kQuality[2])},
                    q,
                    [&s, notify](int i)
                    {
                        s.graphicsQuality = std::clamp(i, 0, 2);
                        notify(SettingsArea::Graphics);
                    });
                toggle(
                    _("Smoothing"),
                    _("Smooth scaling of the sprites instead of hard pixels"),
                    s.smoothing,
                    [&s, notify](bool on)
                    {
                        s.smoothing = on;
                        notify(SettingsArea::Graphics);
                    });

                if(renderApi.options && renderApi.configured && renderApi.active)
                {
                    // Render API: the ones known to fail on this system are left out, untried ones marked with "?"
                    std::vector<RenderApiOption> usable;
                    for(const RenderApiOption &o : renderApi.options())
                    {
                        if(o.status != RenderApiOption::Status::Fails)
                            usable.push_back(o);
                    }
                    const std::string configured = renderApi.configured();
                    const std::string active = renderApi.active();
                    int ci = 0;
                    std::vector<std::string> apiLabels;
                    for(size_t i = 0; i < usable.size(); ++i)
                    {
                        apiLabels.push_back(RenderBackend::label(usable[i].name) + (usable[i].status == RenderApiOption::Status::Unknown ? " ?" : ""));
                        if(usable[i].name == configured)
                            ci = static_cast<int>(i);
                    }
                    const std::string value = RenderBackend::label(configured);
                    const bool pending = configured != active;
                    auto select = renderApi.select;
                    choiceList(
                        _("Render API"),
                        pending ? _("Takes effect after the window is recreated") : _("Graphics interface that draws the game"),
                        apiLabels,
                        ci,
                        [usable, select](int i)
                        {
                            if(!select || i < 0 || i >= static_cast<int>(usable.size()))
                                return;
                            select(usable[static_cast<size_t>(i)].name);
                        },
                        usable.size() > 1,
                        value);
                    if(pending)
                    {
                        auto applyNow = renderApi.applyNow;
                        choice(
                            _("Apply render API"),
                            inGame ? _("Applied on the next start of the game") : _("Recreates the window, then asks to keep it"),
                            std::string(_("RECREATE WINDOW")),
                            [applyNow](int)
                            {
                                if(applyNow)
                                    applyNow();
                            },
                            !inGame);
                    }
                }
                // Shader effects: only the SDL GPU render API draws them
                const bool fx = renderApi.effectsAvailable && renderApi.effectsAvailable();
                const std::string fxHint = _("Needs the SDL GPU render API");
                static const char *const kGrades[6] = {"Off", "Natural", "Vivid", "Cinematic", "Dusk", "Night"};
                static const char *const kGradeHints[6] = {"The picture as drawn", "A little more contrast and colour", "Strong colours", "Muted colours, warm light, dark corners", "Evening light", "Night: explosions light up the dark"};
                const int g = std::clamp(s.colorGrade, 0, 5);
                std::vector<std::string> gradeNames;
                for(const char *name : kGrades)
                    gradeNames.push_back(_(name));
                choiceList(
                    _("Color grading"),
                    fx ? std::string(_(kGradeHints[g])) : fxHint,
                    gradeNames,
                    g,
                    [&s, notify](int i)
                    {
                        s.colorGrade = std::clamp(i, 0, 5);
                        notify(SettingsArea::Graphics);
                    },
                    fx);
                toggle(
                    _("Glow"),
                    fx ? std::string(_("Explosions, shots and projectiles glow")) : fxHint,
                    s.glow,
                    [&s, notify](bool on)
                    {
                        s.glow = on;
                        notify(SettingsArea::Graphics);
                    },
                    fx);
                choiceList(
                    _("Pixel filter"),
                    fx ? std::string(_("Crisp pixels with smooth edges when zoomed")) : fxHint,
                    {_("Standard"), _("Sharp")},
                    s.sharpPixels ? 1 : 0,
                    [&s, notify](int i)
                    {
                        s.sharpPixels = i == 1;
                        notify(SettingsArea::Graphics);
                    },
                    fx);
            }
            else if(tab == 2)
            { // AUDIO
                toggle(
                    _("Sound & Music"),
                    _("Music and sound effects"),
                    !s.muted,
                    [&s, notify](bool on)
                    {
                        s.muted = !on;
                        notify(SettingsArea::Audio);
                    });
                rows.back().valueColor = s.muted ? col::Danger : col::Ok;
                auto percent = [](float v) { return std::to_string(static_cast<int>(v * 100.0f / 128.0f + 0.5f)) + "%"; };
                rows.push_back(sliderRow(
                    _("Music volume"),
                    _("Volume of the soundtrack"),
                    static_cast<float>(s.musicVolume),
                    0.0f,
                    128.0f,
                    8.0f,
                    percent,
                    [&s, notify](float v)
                    {
                        s.musicVolume = static_cast<int>(v);
                        notify(SettingsArea::Audio);
                    }));
                rows.push_back(sliderRow(
                    _("Effects volume"),
                    _("Volume of the sound effects"),
                    static_cast<float>(s.fxVolume),
                    0.0f,
                    128.0f,
                    8.0f,
                    percent,
                    [&s, notify](float v)
                    {
                        s.fxVolume = static_cast<int>(v);
                        notify(SettingsArea::Audio);
                    }));
            }
            else
            { // GAME
                choiceList(
                    _("Language"),
                    _("Interface language"),
                    {"English", "Русский"},
                    s.language == "ru" ? 1 : 0,
                    [&s, notify](int i)
                    {
                        s.language = i == 1 ? "ru" : "en";
                        notify(SettingsArea::Game);
                    });
                rows.push_back(sliderRow(
                    _("Default zoom"),
                    _("Map zoom when a mission starts"),
                    s.zoomDefault,
                    0.5f,
                    4.0f,
                    0.25f,
                    [](float v) { return number(v, 2) + "x"; },
                    [&s, notify](float v)
                    {
                        s.zoomDefault = v;
                        notify(SettingsArea::Graphics);
                    }));
                rows.push_back(sliderRow(
                    _("Minimum zoom"),
                    _("How far the mouse wheel zooms out"),
                    s.zoomMin,
                    0.5f,
                    1.0f,
                    0.05f,
                    [](float v) { return number(v, 2) + "x"; },
                    [&s, notify](float v)
                    {
                        s.zoomMin = v;
                        notify(SettingsArea::Graphics);
                    }));
                rows.push_back(sliderRow(
                    _("Maximum zoom"),
                    _("How far the mouse wheel zooms in"),
                    s.zoomMax,
                    2.0f,
                    4.0f,
                    0.25f,
                    [](float v) { return number(v, 2) + "x"; },
                    [&s, notify](float v)
                    {
                        s.zoomMax = v;
                        notify(SettingsArea::Graphics);
                    }));
            }
            return rows;
        }

        // Settings of one tab back to their defaults
        void resetSettingsTab(int tab, Settings &s)
        {
            const Settings d;
            if(tab == 0)
            {
                s.windowMode = d.windowMode;
                s.windowW = d.windowW;
                s.windowH = d.windowH;
                s.fullscreenW = d.fullscreenW;
                s.fullscreenH = d.fullscreenH;
                s.vsync = d.vsync;
                s.fpsLimit = d.fpsLimit;
                s.showFps = d.showFps;
            }
            else if(tab == 1)
            {
                s.graphicsQuality = d.graphicsQuality;
                s.smoothing = d.smoothing;
                s.colorGrade = d.colorGrade;
                s.glow = d.glow;
                s.sharpPixels = d.sharpPixels;
            }
            else if(tab == 2)
            {
                s.musicVolume = d.musicVolume;
                s.fxVolume = d.fxVolume;
                s.muted = d.muted;
            }
            else
            {
                s.language = d.language;
                s.zoomDefault = d.zoomDefault;
                s.zoomMin = d.zoomMin;
                s.zoomMax = d.zoomMax;
            }
        }

        SettingsArea settingsTabArea(int tab)
        {
            static const SettingsArea kAreas[kSettingsTabs] = {SettingsArea::Video, SettingsArea::Graphics, SettingsArea::Audio, SettingsArea::Game};
            return kAreas[std::clamp(tab, 0, kSettingsTabs - 1)];
        }

        // Section heading + body text block used by briefing / objectives / campaign details
        int drawTextSection(Renderer &r, BitmapFontRenderer &font, int x, int y, int w, const std::string &caption, const std::string &body, SDL_Color bodyColor, int bodyScale)
        {
            ui::drawCaption(font, r, x, y, caption);
            y += 24;
            y += font.drawTextWrapped(body, x, y, w, bodyScale, bodyColor, false);
            return y + 18;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Update handlers
    // ---------------------------------------------------------------------------

    bool MenuController::updateInGameMenu(InputManager &input, int mx, int my, bool leftClicked, int windowW, int windowH, SoundEngine &sound, MissionController &mission, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder, Camera &camera, AppState &state)
    {
        if(!m_inGameMenuOpen)
            return false;
        m_mouseX = mx;
        m_mouseY = my;

        if(m_inGameSettingsOpen)
        {
            updateSettingsScreen(input, mx, my, leftClicked, windowW, windowH, state);
            return true;
        }
        // A question (leave the game, restart) is open: nothing else reacts until it is answered
        int confirmAnswer = -1;
        if(m_confirm.open)
        {
            confirmAnswer = ui::confirmUpdate(m_confirm, windowW, windowH, mx, my, leftClicked, input.isConfirmJustPressed(), input.isKeyJustPressed(SDL_SCANCODE_ESCAPE));
            if(confirmAnswer < 0)
                return true;
            m_confirm.open = false;
        }
        if(confirmAnswer < 0 && input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            m_inGameMenuOpen = false;
            return true;
        }
        if(input.isKeyJustPressed(SDL_SCANCODE_UP))
        {
            m_inGameMenuSelected = (m_inGameMenuSelected - 1 + kPauseItems) % kPauseItems;
        }
        if(input.isKeyJustPressed(SDL_SCANCODE_DOWN))
        {
            m_inGameMenuSelected = (m_inGameMenuSelected + 1) % kPauseItems;
        }

        auto activate = [&](int item, bool confirmed = false)
        {
            // Leaving or restarting a game loses its progress: ask first
            if((item == 4 || item == 5) && !confirmed)
            {
                m_confirm.open = true;
                m_confirm.action = item;
                if(item == 4)
                {
                    m_confirm.title = _("RESTART THE GAME?");
                    m_confirm.text = _("The mission starts over: the progress made so far is lost.");
                }
                else if(mission.isMatch())
                {
                    m_confirm.title = _("LEAVE THE MATCH?");
                    m_confirm.text = _("You leave the match and return to the main menu. The other players go on without you.");
                }
                else
                {
                    m_confirm.title = _("LEAVE THE GAME?");
                    m_confirm.text = _("You return to the main menu: the progress of this game is lost.");
                }
                return;
            }
            switch(item)
            {
                case 0: // Continue
                    m_inGameMenuOpen = false;
                    break;
                case 1: // Objectives
                    m_objectivesDialogOpen = true;
                    m_inGameMenuOpen = false;
                    break;
                case 2: // Settings, same screen as in the main menu
                    m_inGameSettingsOpen = true;
                    m_settingsSelected = 0;
                    break;
                case 3: // Sound & Music
                    sound.toggleMute();
                    if(m_settings && m_onSettingsChanged)
                    {
                        m_settings->muted = sound.isMuted();
                        m_onSettingsChanged(SettingsArea::Audio);
                    }
                    break;
                case 4: // Restart mission
                    mission.restart("assets", map, entities, economy, pathfinder);
                    camera.centerOn(mission.getPlayerHqX() * TILE_WIDTH, mission.getPlayerHqY() * TILE_HEIGHT);
                    m_inGameMenuOpen = false;
                    break;
                case 5: // Main menu
                    state = AppState::MainMenu;
                    m_inGameMenuOpen = false;
                    break;
            }
        };

        if(confirmAnswer >= 0)
        {
            if(confirmAnswer == 1)
                activate(m_confirm.action, true);
            return true;
        }
        if(updateMusicPlayer(leftClicked, false, 0, 0, input.isLeftMouseDown(), mx, my, windowH, false, sound, m_scrollDrag))
            return true;
        PauseLayout l = pauseLayout(windowW, windowH);
        for(int i = 0; i < kPauseItems; ++i)
        {
            if(l.items[i].contains(mx, my))
            {
                m_inGameMenuSelected = i;
                if(leftClicked)
                {
                    activate(i);
                    return true;
                }
            }
        }
        if(input.isConfirmJustPressed())
        {
            activate(m_inGameMenuSelected);
        }
        return true;
    }

    bool MenuController::updateObjectivesDialog(InputManager &input, bool leftClicked)
    {
        if(!m_objectivesDialogOpen)
            return false;

        if(leftClicked || input.isKeyJustPressed(SDL_SCANCODE_ESCAPE) || input.isConfirmJustPressed())
        {
            m_objectivesDialogOpen = false;
        }
        return true;
    }

    bool MenuController::updateCampaignSelect(InputManager &input, int mx, int my, bool leftClicked, int windowW, int windowH, MissionController &mission, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder, Camera &camera, AppState &state, AIController &ai)
    {
        (void) camera;
        if(!m_campaignSelectOpen)
            return false;
        m_mouseX = mx;
        m_mouseY = my;
        (void) ai;
        if(m_settings)
            m_difficulty = static_cast<AIDifficulty>(m_settings->difficulty);

        auto cycleDifficulty = [&]()
        {
            if(!m_settings)
                return;
            m_settings->difficulty = (m_settings->difficulty + 1) % 4;
            m_difficulty = static_cast<AIDifficulty>(m_settings->difficulty);
            if(m_onSettingsChanged)
                m_onSettingsChanged(SettingsArea::Game);
        };

        const int mCount = m_customGame ? static_cast<int>(m_skirmishMaps.size()) : std::min(MissionController::kCampaignMissionCount, static_cast<int>(MissionController::getAvailableMissions().size()));
        if(mCount == 0)
        {
            m_campaignSelectOpen = false;
            return true;
        }
        int &selected = m_customGame ? m_skirmishSelected : m_campaignMissionSelected;
        CampaignLayout l = campaignLayout(windowW, windowH, mCount);
        const int rows = m_customGame ? customListRows(l) : mCount;

        auto startSelected = [&]()
        {
            // The loading screen with the picture of the map, kept up for at least three seconds
            if(m_beginLoading)
            {
                if(m_customGame)
                {
                    m_beginLoading(m_skirmishMaps[m_skirmishSelected].displayName(), m_skirmishMaps[m_skirmishSelected].path);
                }
                else
                {
                    const auto &data = Missions::campaign(playerFaction())[m_campaignMissionSelected];
                    m_beginLoading(_(data.info.name), "assets/maps/" + data.data.mapFile + ".json");
                }
            }
            const bool started =
                m_customGame ? mission.loadSkirmish(m_skirmishMaps[m_skirmishSelected].path, map, entities, economy, pathfinder, m_skirmishMaps[m_skirmishSelected].hasPrebuilt && m_skirmishPrebuilt) : mission.loadMission(m_campaignMissionSelected, "assets", map, entities, economy, pathfinder);
            if(m_endLoading)
                m_endLoading();
            if(!started)
                return;
            state = AppState::Briefing;
            m_campaignSelectOpen = false;
        };

        if(input.isKeyJustPressed(SDL_SCANCODE_UP))
        {
            selected = (selected - 1 + mCount) % mCount;
        }
        if(input.isKeyJustPressed(SDL_SCANCODE_DOWN))
        {
            selected = (selected + 1) % mCount;
        }
        if(m_customGame)
        {
            if(input.isKeyJustPressed(SDL_SCANCODE_PAGEUP))
                selected = std::max(0, selected - rows);
            if(input.isKeyJustPressed(SDL_SCANCODE_PAGEDOWN))
                selected = std::min(mCount - 1, selected + rows);
            // The wheel scrolls the list, the selection stays where it is
            m_skirmishTop = std::clamp(m_skirmishTop - input.consumeWheel(), 0, std::max(0, mCount - rows));
            if(input.isKeyJustPressed(SDL_SCANCODE_UP) || input.isKeyJustPressed(SDL_SCANCODE_DOWN) || input.isKeyJustPressed(SDL_SCANCODE_PAGEUP) || input.isKeyJustPressed(SDL_SCANCODE_PAGEDOWN))
            {
                if(selected < m_skirmishTop)
                    m_skirmishTop = selected;
                if(selected >= m_skirmishTop + rows)
                    m_skirmishTop = selected - rows + 1;
            }
        }
        if(m_customGame && mCount > rows)
        {
            const UiRect bar {l.list.right() + 4, l.list.y, 4, l.list.h};
            int px = 0, py = 0;
            if(input.consumeLeftPress(px, py) && input.isLeftMouseDown() && onScrollBar(bar, px, py))
                m_scrollDrag = 1;
            if(m_scrollDrag == 1)
            {
                if(input.isLeftMouseDown())
                    m_skirmishTop = scrollTopFromMouse(bar, my, mCount, rows);
                else
                    m_scrollDrag = 0;
                return true;
            }
        }
        if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            if(m_combo.open >= kComboSide)
            { // closes the open list first
                m_combo.open = -1;
                return true;
            }
            m_campaignSelectOpen = false;
            return true;
        }
        if(input.isKeyJustPressed(SDL_SCANCODE_LEFT) || input.isKeyJustPressed(SDL_SCANCODE_RIGHT))
        {
            setPlayerFaction(opposingFaction(playerFaction()));
        }

        if(input.isKeyJustPressed(SDL_SCANCODE_TAB))
            cycleDifficulty();
        const bool hasLayoutChoice = m_customGame && m_skirmishSelected >= 0 && m_skirmishSelected < static_cast<int>(m_skirmishMaps.size()) && m_skirmishMaps[m_skirmishSelected].hasPrebuilt;
        // The drop-down lists of the footer (side, difficulty) and of the map (prebuilt bases / from scratch)
        auto comboField = [&](int id) { return id == kComboSide ? l.sideBtn[0] : (id == kComboDifficulty ? l.difficultyBtn : l.layoutBtn); };
        if(m_combo.open >= 0 && m_combo.open < kComboSide)
            m_combo.open = -1; // one of the settings screen's
        if(m_combo.open == kComboLayout && !hasLayoutChoice)
            m_combo.open = -1;
        if(m_combo.open >= 0)
        {
            const int id = m_combo.open;
            int picked = -1;
            if(ui::comboUpdate(m_combo, comboField(id), static_cast<int>(campaignComboOptions(id).size()), windowH, mx, my, leftClicked, input.consumeWheel(), picked))
            {
                if(picked >= 0)
                    campaignComboApply(id, picked);
                return true;
            }
            if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
            {
                m_combo.open = -1;
                return true;
            }
        }
        if(leftClicked)
        {
            auto openCombo = [&](int id)
            {
                m_combo.open = id;
                m_combo.scroll = 0;
                return true;
            };
            if(l.sideBtn[0].contains(mx, my))
                return openCombo(kComboSide);
            if(l.difficultyBtn.contains(mx, my))
                return openCombo(kComboDifficulty);
            if(hasLayoutChoice && l.layoutBtn.contains(mx, my))
                return openCombo(kComboLayout);
            // Side the player commands in this mission
            if(l.editorBtn.contains(mx, my))
            {
                const std::string mapPath = m_customGame ? m_skirmishMaps[m_skirmishSelected].path : "assets/maps/" + Missions::campaign(playerFaction())[m_campaignMissionSelected].data.mapFile + ".json";
                m_editorNotice = launchMapEditor(mapPath) ? "" : _("Map editor (openaow-mapeditor) not found next to the game");
                return true;
            }
            const int first = m_customGame ? m_skirmishTop : 0;
            for(int row = 0; row < rows && first + row < mCount; ++row)
            {
                if(l.item(row).contains(mx, my))
                {
                    // First click selects, clicking the selected mission or map starts it
                    if(selected == first + row)
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
            if(l.startBtn.contains(mx, my))
            {
                startSelected();
                return true;
            }
            if(l.backBtn.contains(mx, my))
            {
                m_campaignSelectOpen = false;
                return true;
            }
        }

        if(input.isConfirmJustPressed())
        {
            startSelected();
        }
        return true;
    }

    void MenuController::activateMenuItem(int index, AppState &state, bool &running, [[maybe_unused]] SoundEngine &sound, MissionController &mission, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder, Camera &camera)
    {
        switch(index)
        {
            case 0: // Campaign: the single-player missions only
                m_campaignSelectOpen = true;
                m_customGame = false;
                m_campaignMissionSelected = 0;
                break;
            case 1: // Custom game: any map of assets/maps, side picked in the dialog
                m_skirmishMaps = MissionController::listSkirmishMaps();
                m_campaignSelectOpen = !m_skirmishMaps.empty();
                m_customGame = true;
                m_skirmishSelected = 0;
                m_skirmishTop = 0;
                break;
            case 2: // Network game: local network / internet, then the game room
                state = AppState::MenuNetwork;
                break;
            case 3: // Test map
                mission.loadTestMap("assets", map, entities, economy, pathfinder);
                state = AppState::CampaignPlaying;
                camera.centerOn((mission.getPlayerHqX() + 12) * TILE_WIDTH, mission.getPlayerHqY() * TILE_HEIGHT);
                break;
            case 4:
                state = AppState::MenuSettings;
                break;
            case 5:
                state = AppState::MenuHelp;
                break;
            case 6:
                state = AppState::MenuAbout;
                break;
            case 7:
                running = false;
                break;
            default:
                break;
        }
    }

    void MenuController::updateMainMenu(InputManager &input, int windowW, int windowH, AppState &state, bool &running, SoundEngine &sound, MissionController &mission, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder, Camera &camera, AIController &ai)
    {
        int mx = input.getMouseScreenX();
        int my = input.getMouseScreenY();
        m_mouseX = mx;
        m_mouseY = my;
        float lx, ly;
        bool clicked = input.wasLeftClickIssued(lx, ly);

        if(m_campaignSelectOpen)
        {
            updateCampaignSelect(input, mx, my, clicked, windowW, windowH, mission, map, entities, economy, pathfinder, camera, state, ai);
            return;
        }

        if(input.isKeyJustPressed(SDL_SCANCODE_UP))
        {
            m_selectedMenuItem = (m_selectedMenuItem - 1 + kMainMenuItems) % kMainMenuItems;
        }
        if(input.isKeyJustPressed(SDL_SCANCODE_DOWN))
        {
            m_selectedMenuItem = (m_selectedMenuItem + 1) % kMainMenuItems;
        }

        int px = 0, py = 0;
        const bool pressed = input.consumeLeftPress(px, py);
        if(updateMusicPlayer(clicked, pressed, px, py, input.isLeftMouseDown(), mx, my, windowH, true, sound, m_scrollDrag))
            return;

        for(int i = 0; i < kMainMenuItems; ++i)
        {
            if(mainMenuItemRect(i, windowW, windowH).contains(mx, my))
            {
                m_selectedMenuItem = i;
                if(clicked)
                {
                    activateMenuItem(i, state, running, sound, mission, map, entities, economy, pathfinder, camera);
                    return;
                }
            }
        }

        if(input.isConfirmJustPressed())
        {
            activateMenuItem(m_selectedMenuItem, state, running, sound, mission, map, entities, economy, pathfinder, camera);
        }
        if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            running = false;
        }
    }

    void MenuController::bindSettings(Settings &settings, std::function<void(SettingsArea)> onChanged, std::function<const std::vector<Resolution> &()> resolutions)
    {
        m_settings = &settings;
        m_onSettingsChanged = std::move(onChanged);
        m_resolutions = std::move(resolutions);
    }

    void MenuController::updateSettings(InputManager &input, int windowW, int windowH, AppState &state)
    {
        float lx, ly;
        const bool clicked = input.wasLeftClickIssued(lx, ly);
        updateSettingsScreen(input, input.getMouseScreenX(), input.getMouseScreenY(), clicked, windowW, windowH, state);
    }

    // Settings page of the main menu, or of the pause menu (m_inGameSettingsOpen): Back returns to where it came from
    void MenuController::updateSettingsScreen(InputManager &input, int mx, int my, bool clicked, int windowW, int windowH, AppState &state)
    {
        m_mouseX = mx;
        m_mouseY = my;
        auto close = [&]()
        {
            m_combo.open = -1;
            if(m_inGameSettingsOpen)
                m_inGameSettingsOpen = false;
            else
                state = AppState::MainMenu;
        };
        if(!m_settings || !m_onSettingsChanged || !m_resolutions)
        {
            close();
            return;
        }

        static const std::vector<Resolution> kNoResolutions;
        auto rows = [&]() { return buildSettingsRows(m_settingsTab, *m_settings, m_resolutions ? m_resolutions() : kNoResolutions, m_onSettingsChanged, m_renderApi, m_inGameSettingsOpen); };

        if(input.isKeyJustPressed(SDL_SCANCODE_TAB))
        {
            m_settingsTab = (m_settingsTab + 1) % kSettingsTabs;
            m_settingsSelected = 0;
        }
        std::vector<SettingsRow> current = rows();
        const int count = static_cast<int>(current.size());
        SettingsLayout l = settingsLayout(windowW, windowH, count);
        m_settingsSelected = std::clamp(m_settingsSelected, 0, std::max(0, count - 1));
        if(input.isKeyJustPressed(SDL_SCANCODE_UP) && count > 0)
            m_settingsSelected = (m_settingsSelected - 1 + count) % count;
        if(input.isKeyJustPressed(SDL_SCANCODE_DOWN) && count > 0)
            m_settingsSelected = (m_settingsSelected + 1) % count;

        auto stepRow = [&](int row, int dir)
        {
            if(row >= 0 && row < count && current[row].enabled && current[row].step)
                current[row].step(dir);
        };
        // An open list takes the mouse first: a pick sets the value, a click anywhere else only closes it
        const int wheel = input.consumeWheel();
        if(m_combo.open >= kComboSide)
            m_combo.open = -1; // one of the custom game dialog's
        if(m_combo.open >= 0)
        {
            if(m_combo.open >= count || current[m_combo.open].options.empty() || !current[m_combo.open].enabled)
            {
                m_combo.open = -1;
            }
            else
            {
                int picked = -1;
                const int openRow = m_combo.open;
                if(ui::comboUpdate(m_combo, settingsControlRect(l.rows[openRow]), static_cast<int>(current[openRow].options.size()), windowH, mx, my, clicked, wheel, picked))
                {
                    if(picked >= 0 && current[openRow].select)
                        current[openRow].select(picked);
                    return;
                }
                if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
                { // Esc closes the list, not the screen
                    m_combo.open = -1;
                    m_combo.scroll = 0;
                    return;
                }
            }
        }
        if(input.isKeyJustPressed(SDL_SCANCODE_LEFT))
            stepRow(m_settingsSelected, -1);
        if(input.isKeyJustPressed(SDL_SCANCODE_RIGHT) || input.isConfirmJustPressed())
            stepRow(m_settingsSelected, +1);

        if(clicked)
        {
            for(int i = 0; i < kSettingsTabs; ++i)
            {
                if(l.tabs[i].contains(mx, my) && i != m_settingsTab)
                {
                    m_combo.open = -1;
                    m_settingsTab = i;
                    m_settingsSelected = 0;
                    return;
                }
            }
            if(l.backBtn.contains(mx, my))
            {
                close();
                return;
            }
            if(l.defaultsBtn.contains(mx, my))
            {
                resetSettingsTab(m_settingsTab, *m_settings);
                m_onSettingsChanged(settingsTabArea(m_settingsTab));
                return;
            }
            for(int i = 0; i < count && i < l.rowCount; ++i)
            {
                if(!l.rows[i].contains(mx, my))
                    continue;
                m_settingsSelected = i;
                const UiRect c = settingsControlRect(l.rows[i]);
                if(current[i].slider)
                {
                    const UiRect track = sliderTrackRect(l.rows[i]);
                    if(mx >= track.x - 6 && mx <= track.right() + 6)
                    {
                        m_sliderDrag = i;
                        current[i].setRatio(static_cast<float>(mx - track.x) / track.w);
                    }
                }
                else if(!current[i].options.empty())
                {
                    // A combo box: the field opens its list
                    if(current[i].enabled && c.contains(mx, my))
                    {
                        m_combo.open = i;
                        m_combo.scroll = std::clamp(current[i].current - ui::kComboMaxVisible / 2, 0, std::max(0, static_cast<int>(current[i].options.size()) - ui::kComboMaxVisible));
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
        if(m_sliderDrag >= 0 && input.isLeftMouseDown() && m_sliderDrag < count && current[m_sliderDrag].slider)
        {
            const UiRect track = sliderTrackRect(l.rows[m_sliderDrag]);
            current[m_sliderDrag].setRatio(static_cast<float>(mx - track.x) / track.w);
        }
        else if(!input.isLeftMouseDown())
        {
            m_sliderDrag = -1;
        }
        if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            close();
        }
    }

    void MenuController::updateInfoScreen(InputManager &input, AppState &state)
    {
        m_mouseX = input.getMouseScreenX();
        m_mouseY = input.getMouseScreenY();
        float lx, ly;
        bool clicked = input.wasLeftClickIssued(lx, ly);
        int px = 0, py = 0;
        if(input.consumeLeftPress(px, py) && input.isLeftMouseDown() && onScrollBar(m_infoTrack, px, py))
            m_scrollDrag = 2;
        if(m_scrollDrag == 2)
        {
            if(input.isLeftMouseDown())
                m_infoScroll = scrollTopFromMouse(m_infoTrack, m_mouseY, m_infoTotal, m_infoVisible);
            else
                m_scrollDrag = 0;
            return;
        }
        m_infoScroll = std::max(0, m_infoScroll - input.consumeWheel() * 3);
        if(input.isKeyJustPressed(SDL_SCANCODE_UP))
            m_infoScroll = std::max(0, m_infoScroll - 1);
        if(input.isKeyJustPressed(SDL_SCANCODE_DOWN))
            ++m_infoScroll;
        if(input.isKeyJustPressed(SDL_SCANCODE_PAGEUP))
            m_infoScroll = std::max(0, m_infoScroll - 10);
        if(input.isKeyJustPressed(SDL_SCANCODE_PAGEDOWN))
            m_infoScroll += 10;
        if(clicked || input.isKeyJustPressed(SDL_SCANCODE_ESCAPE) || input.isConfirmJustPressed())
        {
            state = AppState::MainMenu;
            m_infoScroll = 0;
        }
    }

    // ---------------------------------------------------------------------------
    // Renderers
    // ---------------------------------------------------------------------------

    void MenuController::renderSplashLogo(Renderer &renderer, Camera &camera, TextureManager &texManager, int windowW, int windowH)
    {
        (void) camera;
        renderer.fillRectScreen(0, 0, windowW, windowH, {0, 16, 60, 255});

        SDL_Texture *logoTex = texManager.getUiTexture("logo");
        if(logoTex)
        {
            int lw = 296;
            int lh = 196;
            int lx = (windowW - lw) / 2;
            int ly = (windowH - lh) / 2 - 25;
            renderer.drawTextureScreen(logoTex, lx, ly, lw, lh);
        }
    }

    void MenuController::renderSplashTitle(Renderer &renderer, Camera &camera, TextureManager &texManager, int windowW, int windowH)
    {
        (void) camera;
        renderer.fillRectScreen(0, 0, windowW, windowH, {10, 12, 18, 255});

        SDL_Texture *titleTex = texManager.getUiTexture("title");
        if(titleTex)
        {
            int th = std::min(windowH - 120, 600);
            int tw = th * 240 / 323;
            int tx = (windowW - tw) / 2;
            int ty = (windowH - th) / 2;
            renderer.drawTextureScreen(titleTex, tx, ty, tw, th);
        }
    }

    void MenuController::renderMainMenu(Renderer &renderer, TextureManager &texManager, BitmapFontRenderer &fontRenderer, const SoundEngine &sound, int windowW, int windowH)
    {
        renderer.fillRectScreen(0, 0, windowW, windowH, col::Backdrop);

        // Key art anchored to the right edge, blended into the dark backdrop
        SDL_Texture *titleTex = texManager.getUiTexture("title");
        if(titleTex)
        {
            int th = windowH;
            int tw = th * 240 / 323;
            int tx = windowW - tw;
            renderer.drawTextureScreen(titleTex, tx, 0, tw, th);
            renderer.fillRectScreen(tx, 0, tw, th, {8, 10, 14, 70});
            int fadeW = std::min(tw, 320);
            ui::fillHorizontalGradient(renderer, {tx, 0, fadeW, windowH}, col::Backdrop, ui::withAlpha(col::Backdrop, 0));
            if(tx > 0)
                renderer.fillRectScreen(0, 0, tx, windowH, col::Backdrop);
            m_backdropFx.render(renderer, texManager, tx, windowW, windowH, SDL_GetTicks());
            // The blasts melt into the key art instead of ending at a hard edge
            if(tx > 0)
                ui::fillHorizontalGradient(renderer, {std::max(0, tx - 200), 0, std::min(tx, 200), windowH}, ui::withAlpha(col::Backdrop, 0), col::Backdrop);
        }
        else
        {
            m_backdropFx.render(renderer, texManager, windowW * 55 / 100, windowW, windowH, SDL_GetTicks());
        }
        ui::fillVerticalGradient(renderer, {0, windowH - 160, windowW, 160}, ui::withAlpha(col::Backdrop, 0), col::Backdrop);

        // Title block
        int x = mainMenuColumnX(windowW);
        int titleY = mainMenuTitleY(windowH);
        fontRenderer.drawString("OPEN ART OF WAR", x, titleY, ui::FONT_HERO, col::Accent, true);
        fontRenderer.drawString(_("FREEDOM FOR LIBERTY"), x + 2, titleY + 58, ui::FONT_BODY_BOLD, col::TextDim, false);
        renderer.fillRectScreen(x + 2, titleY + 86, 72, 3, col::Accent);

        // Menu items
        for(int i = 0; i < kMainMenuItems; ++i)
        {
            UiRect r = mainMenuItemRect(i, windowW, windowH);
            bool selected = (i == m_selectedMenuItem);
            bool isExit = (i == kMainMenuItems - 1);
            std::string label = _(kMainMenuLabels[i]);

            if(selected)
            {
                ui::fillHorizontalGradient(renderer, r, ui::withAlpha(col::Hover, 250), ui::withAlpha(col::Hover, 0));
                renderer.fillRectScreen(r.x, r.y, 4, r.h, isExit ? col::Danger : col::Accent);
            }
            SDL_Color textCol = selected ? (isExit ? SDL_Color {255, 190, 180, 255} : col::Text) : col::TextDim;
            fontRenderer.drawString(label, r.x + 20 + (selected ? 5 : 0), r.centerY(), ui::FONT_H2, textCol, selected, Anchor::LEFT | Anchor::VCENTER);
            if(i == kMainMenuItems - 2)
            {
                // Separator between the game modes / info items and Exit
                renderer.fillRectScreen(r.x + 20, r.bottom() + 2, 120, 1, col::Border);
            }
        }

        // Footer
        fontRenderer.drawString(std::string(_("Version")) + " " + OAOW_VERSION, windowW - 24, windowH - 30, ui::FONT_SMALL, col::TextMuted, false, Anchor::RIGHT | Anchor::TOP);

        renderMusicPlayer(renderer, fontRenderer, sound, windowH, true, m_mouseX, m_mouseY);

        if(m_campaignSelectOpen)
        {
            renderCampaignMissionSelect(renderer, texManager, fontRenderer, windowW, windowH);
        }
    }

    void MenuController::renderInfoDialog(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH, const std::string &title, const std::vector<std::string> &lines)
    {
        UiRect d = infoDialogRect(windowW, windowH);
        UiRect content = ui::drawDialog(renderer, fontRenderer, windowW, windowH, d, title);

        // Long texts scroll: the footer button takes the bottom of the dialog
        const int visible = std::max(1, (dialogFooterButton(d).y - 10 - content.y) / 30);
        const int total = static_cast<int>(lines.size());
        m_infoScroll = std::clamp(m_infoScroll, 0, std::max(0, total - visible));
        int y = content.y;
        for(int i = m_infoScroll; i < total && i < m_infoScroll + visible; ++i)
        {
            if(!lines[i].empty())
            {
                fontRenderer.drawString(ui::fitText(fontRenderer, lines[i], ui::FONT_BODY, content.w - 12), content.x, y, ui::FONT_BODY, col::Text, false);
            }
            y += 30;
        }
        m_infoTrack = {};
        m_infoTotal = total;
        m_infoVisible = visible;
        if(total > visible)
        {
            m_infoTrack = {content.right() - 4, content.y, 4, visible * 30};
            const int trackH = visible * 30;
            const int thumbH = std::max(24, trackH * visible / total);
            const int thumbY = content.y + (trackH - thumbH) * m_infoScroll / (total - visible);
            renderer.fillRectScreen(content.right() - 4, content.y, 4, trackH, col::PanelSunken);
            renderer.fillRectScreen(content.right() - 4, thumbY, 4, thumbH, col::BorderStrong);
        }

        UiRect back = dialogFooterButton(d);
        ui::drawButton(renderer, fontRenderer, back, _("BACK"), ui::ButtonStyle::Secondary, back.contains(m_mouseX, m_mouseY));
    }

    void MenuController::renderMenuHelp(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH)
    {
        UiRect d = infoDialogRect(windowW, windowH);
        UiRect content = ui::drawDialog(renderer, fontRenderer, windowW, windowH, d, _("HELP & CONTROLS"));

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
        for(const auto &b : kBindings)
        {
            std::string keys = b.localizedKeys ? std::string(_(b.keys)) : std::string(b.keys);
            int kw = fontRenderer.getTextWidth(keys, ui::FONT_SMALL_BOLD) + 16;
            UiRect chip {content.x, y, kw, 26};
            renderer.fillRectScreen(chip.x, chip.y, chip.w, chip.h, col::PanelRaised);
            renderer.drawRectScreen(chip.x, chip.y, chip.w, chip.h, col::BorderStrong);
            renderer.fillRectScreen(chip.x + 1, chip.bottom() - 3, chip.w - 2, 2, col::PanelSunken);
            fontRenderer.drawString(keys, chip.centerX(), chip.centerY() - 1, ui::FONT_SMALL_BOLD, col::Accent, false, Anchor::HCENTER | Anchor::VCENTER);
            fontRenderer.drawString(ui::fitText(fontRenderer, _(b.action), ui::FONT_BODY, content.w - keyColW), content.x + keyColW, chip.centerY(), ui::FONT_BODY, col::Text, false, Anchor::LEFT | Anchor::VCENTER);
            y += 38;
        }

        UiRect back = dialogFooterButton(d);
        ui::drawButton(renderer, fontRenderer, back, _("BACK"), ui::ButtonStyle::Secondary, back.contains(m_mouseX, m_mouseY));
    }

    // Who made the music (assets/data/tracklist.json): every author once, with the link or licence given there
    std::vector<std::string> musicCredits()
    {
        static std::vector<std::string> credits = []
        {
            std::vector<std::string> out;
            std::string text;
            if(!Assets::readText("assets/data/tracklist.json", text))
                return out;
            const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
            if(!doc.is_object())
                return out;
            for(const char *list : {"mainmenu", "tracks"})
            {
                if(!doc.contains(list) || !doc[list].is_array())
                    continue;
                for(const auto &track : doc[list])
                {
                    if(!track.is_object() || !track.contains("author"))
                        continue;
                    std::string line = "  " + track.value("author", std::string());
                    if(track.contains("link"))
                        line += " - " + track.value("link", std::string());
                    else if(track.contains("license"))
                        line += " (" + track.value("license", std::string()) + ")";
                    if(std::find(out.begin(), out.end(), line) == out.end())
                        out.push_back(line);
                }
            }
            return out;
        }();
        return credits;
    }

    void MenuController::renderMenuAbout(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH)
    {
        std::vector<std::string> lines = {
            "Open Art Of War: Freedom for Liberty",
            std::string(_("Version")) + ": " + OAOW_VERSION,
            std::string(_("Author")) + ": badcast",
            "https://github.com/badcast/open-artofwar",
            "",
            _("Porting: Modern C++17, SDL3, ImGui"),
            "",
            _("Central Peru is occupied by the army of the Confederation."),
            _("The Resistance stands ready to strike for freedom!"),
            "",
            _("Music:")};
        // The licence texts shipped with the music; the tracklist authors only when the file is missing
        std::string licence;
        if(Assets::readText("assets/audio/LICENSE", licence))
        {
            std::istringstream in(licence);
            std::string line;
            while(std::getline(in, line))
            {
                if(!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }
            while(!lines.empty() && lines.back().empty())
                lines.pop_back();
        }
        else
        {
            for(const std::string &credit : musicCredits())
                lines.push_back(credit);
        }
        renderInfoDialog(renderer, fontRenderer, windowW, windowH, _("ABOUT GAME"), lines);
    }

    void MenuController::renderMenuSettings(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH)
    {
        if(!m_settings || !m_resolutions)
        {
            ui::drawDialog(renderer, fontRenderer, windowW, windowH, settingsLayout(windowW, windowH).dialog, _("SETTINGS"));
            return;
        }
        static const std::function<void(SettingsArea)> kNoNotify = [](SettingsArea) {};
        Settings scratch = *m_settings; // only read: the rows' value texts, the callbacks are not called here
        std::vector<SettingsRow> rows = buildSettingsRows(m_settingsTab, scratch, m_resolutions(), kNoNotify, m_renderApi, m_inGameSettingsOpen);
        SettingsLayout l = settingsLayout(windowW, windowH, static_cast<int>(rows.size()));
        ui::drawDialog(renderer, fontRenderer, windowW, windowH, l.dialog, _("SETTINGS"));

        static const char *const kTabNames[kSettingsTabs] = {"VIDEO", "GRAPHICS", "AUDIO", "GAME"};
        for(int i = 0; i < kSettingsTabs; ++i)
        {
            ui::drawButton(renderer, fontRenderer, l.tabs[i], _(kTabNames[i]), i == m_settingsTab ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary, l.tabs[i].contains(m_mouseX, m_mouseY));
        }

        for(int i = 0; i < static_cast<int>(rows.size()) && i < l.rowCount; ++i)
        {
            const SettingsRow &row = rows[i];
            const UiRect &r = l.rows[i];
            const bool hot = r.contains(m_mouseX, m_mouseY) || i == m_settingsSelected;
            ui::drawPanel(renderer, r, hot ? col::Hover : col::PanelRaised, hot ? col::BorderStrong : col::Border);
            if(hot)
                renderer.fillRectScreen(r.x, r.y, 3, r.h, col::Accent);
            const SDL_Color text = row.enabled ? col::Text : col::TextMuted;
            // Label and hint keep their places relative to the row's height (rows get lower on long tabs)
            fontRenderer.drawString(row.label, r.x + 18, r.y + r.h * 10 / 56, ui::FONT_BODY_BOLD, text, false);
            fontRenderer.drawString(ui::fitText(fontRenderer, row.hint, ui::FONT_SMALL, r.w - 360), r.x + 18, r.y + r.h * 33 / 56, ui::FONT_SMALL, col::TextDim, false);

            const UiRect c = settingsControlRect(r);
            if(row.slider)
            {
                const UiRect track = sliderTrackRect(r);
                renderer.fillRectScreen(track.x, track.y, track.w, track.h, col::PanelSunken);
                renderer.drawRectScreen(track.x, track.y, track.w, track.h, col::Border);
                renderer.fillRectScreen(track.x, track.y, static_cast<int>(track.w * std::clamp(row.ratio, 0.0f, 1.0f)), track.h, col::Accent);
                const int knobX = track.x + static_cast<int>(track.w * std::clamp(row.ratio, 0.0f, 1.0f));
                renderer.fillRectScreen(knobX - 4, track.centerY() - 10, 8, 20, col::Text);
                fontRenderer.drawString(row.value, c.right(), c.centerY(), ui::FONT_BODY_BOLD, col::Accent, false, Anchor::RIGHT | Anchor::VCENTER);
            }
            else if(!row.options.empty())
            {
                ui::drawComboField(renderer, fontRenderer, c, row.value, m_combo.open == i, c.contains(m_mouseX, m_mouseY), row.enabled, row.valueColor);
            }
            else
            {
                renderer.fillRectScreen(c.x, c.y, c.w, c.h, col::PanelSunken);
                renderer.drawRectScreen(c.x, c.y, c.w, c.h, ui::withAlpha(row.enabled ? row.valueColor : col::TextMuted, 160));
                fontRenderer.drawString(ui::fitText(fontRenderer, row.value, ui::FONT_BODY_BOLD, c.w - 64), c.centerX(), c.centerY(), ui::FONT_BODY_BOLD, row.enabled ? row.valueColor : col::TextMuted, false, Anchor::HCENTER | Anchor::VCENTER);
                if(row.enabled)
                {
                    fontRenderer.drawString("\xE2\x80\xB9", c.x + 16, c.centerY() - 1, ui::FONT_BODY_BOLD, col::TextDim, false, Anchor::HCENTER | Anchor::VCENTER);
                    fontRenderer.drawString("\xE2\x80\xBA", c.right() - 16, c.centerY() - 1, ui::FONT_BODY_BOLD, col::TextDim, false, Anchor::HCENTER | Anchor::VCENTER);
                }
            }
        }

        fontRenderer.drawString(_("Click: pick from a list. Arrows: step. Tab: next page"), l.dialog.x + 20, l.backBtn.centerY(), ui::FONT_SMALL, col::TextMuted, false, Anchor::LEFT | Anchor::VCENTER);
        ui::drawButton(renderer, fontRenderer, l.defaultsBtn, _("DEFAULTS"), ui::ButtonStyle::Secondary, l.defaultsBtn.contains(m_mouseX, m_mouseY));
        ui::drawButton(renderer, fontRenderer, l.backBtn, _("BACK"), ui::ButtonStyle::Secondary, l.backBtn.contains(m_mouseX, m_mouseY));
        // The open list over everything
        if(m_combo.open >= 0 && m_combo.open < static_cast<int>(rows.size()) && m_combo.open < l.rowCount && !rows[m_combo.open].options.empty())
        {
            ui::drawComboList(renderer, fontRenderer, settingsControlRect(l.rows[m_combo.open]), rows[m_combo.open].options, rows[m_combo.open].current, m_combo, windowH, m_mouseX, m_mouseY);
        }
    }

    void MenuController::renderBriefing(Renderer &renderer, BitmapFontRenderer &fontRenderer, const MissionController &mission, int windowW, int windowH)
    {
        renderer.fillRectScreen(0, 0, windowW, windowH, col::Backdrop);
        UiRect d = briefingRect(windowW, windowH);
        std::string title = mission.getMissionName().empty() ? std::string(_("MISSION BRIEFING")) : std::string(_(mission.getMissionName()));
        UiRect content = ui::drawDialog(renderer, fontRenderer, windowW, windowH, d, title);

        int y = drawTextSection(renderer, fontRenderer, content.x, content.y, content.w, _("OBJECTIVES:"), _(mission.getObjectiveText()), col::Ok, ui::FONT_BODY);
        drawTextSection(renderer, fontRenderer, content.x, y, content.w, _("BRIEFING:"), _(mission.getBriefingText()), col::Text, ui::FONT_BODY);

        UiRect start = dialogFooterButton(d, 220);
        fontRenderer.drawString(_("Click or press Enter to begin"), d.x + 20, start.centerY(), ui::FONT_SMALL, col::TextMuted, false, Anchor::LEFT | Anchor::VCENTER);
        ui::drawButton(renderer, fontRenderer, start, _("START MISSION"), ui::ButtonStyle::Primary, start.contains(m_mouseX, m_mouseY));
    }

    void MenuController::renderInGamePauseMenu(Renderer &renderer, BitmapFontRenderer &fontRenderer, const SoundEngine &sound, int windowW, int windowH)
    {
        PauseLayout l = pauseLayout(windowW, windowH);
        ui::drawDialog(renderer, fontRenderer, windowW, windowH, l.dialog, _("GAME PAUSED"));

        std::string soundLabel = std::string(_("Sound & Music")) + ": " + (sound.isMuted() ? _("OFF") : _("ON"));
        std::string items[kPauseItems] = {_("CONTINUE"), _("OBJECTIVES"), _("SETTINGS"), soundLabel, _("RESTART MISSION"), _("MAIN MENU")};

        for(int i = 0; i < kPauseItems; ++i)
        {
            bool hot = (i == m_inGameMenuSelected) || l.items[i].contains(m_mouseX, m_mouseY);
            ui::ButtonStyle style = (i == 0) ? ui::ButtonStyle::Primary : (i == kPauseItems - 1 ? ui::ButtonStyle::Danger : ui::ButtonStyle::Secondary);
            ui::drawButton(renderer, fontRenderer, l.items[i], items[i], style, hot);
        }
        renderMusicPlayer(renderer, fontRenderer, sound, windowH, false, m_mouseX, m_mouseY);
        ui::drawConfirm(renderer, fontRenderer, windowW, windowH, m_confirm, m_mouseX, m_mouseY);
    }

    void MenuController::renderObjectivesDialog(Renderer &renderer, BitmapFontRenderer &fontRenderer, const MissionController &mission, int windowW, int windowH)
    {
        UiRect d = objectivesRect(windowW, windowH);
        std::string title = _(mission.getMissionName()) + _(" - OBJECTIVES");
        UiRect content = ui::drawDialog(renderer, fontRenderer, windowW, windowH, d, title);

        int y = drawTextSection(renderer, fontRenderer, content.x, content.y, content.w, _("PRIMARY OBJECTIVES:"), _(mission.getObjectiveText()), col::Ok, ui::FONT_BODY);
        drawTextSection(renderer, fontRenderer, content.x, y, content.w, _("OPERATION BRIEFING:"), _(mission.getBriefingText()), col::TextDim, ui::FONT_SMALL);

        UiRect btn = dialogFooterButton(d);
        ui::drawButton(renderer, fontRenderer, btn, _("CONTINUE"), ui::ButtonStyle::Primary, btn.contains(m_mouseX, m_mouseY));
    }

    namespace
    {

        // The picture of a map in the bottom part of the details panel, scaled to fit and centered
        void drawMapPreview(Renderer &renderer, SDL_Texture *tex, int texW, int texH, const UiRect &panel)
        {
            const int margin = 20;
            const int maxH = std::min(panel.h / 2 - 12, 220);
            const UiRect area {panel.x + margin, panel.bottom() - margin - maxH, panel.w - 2 * margin, maxH};
            renderer.fillRectScreen(area.x, area.y, area.w, area.h, col::PanelSunken);
            renderer.drawRectScreen(area.x, area.y, area.w, area.h, col::Border);
            if(!tex || texW <= 0 || texH <= 0)
                return;
            const float scale = std::min((area.w - 4) / static_cast<float>(texW), (area.h - 4) / static_cast<float>(texH));
            const int w = std::max(1, static_cast<int>(texW * scale)), h = std::max(1, static_cast<int>(texH * scale));
            SDL_FRect dst {static_cast<float>(area.centerX() - w / 2), static_cast<float>(area.centerY() - h / 2), static_cast<float>(w), static_cast<float>(h)};
            SDL_RenderTexture(renderer.getSDLRenderer(), tex, nullptr, &dst);
        }

    } // namespace

    void MenuController::renderCampaignMissionSelect(Renderer &renderer, TextureManager &texManager, BitmapFontRenderer &fontRenderer, int windowW, int windowH)
    {
        const auto &missions = MissionController::getAvailableMissions();
        const int missionCount = std::min(MissionController::kCampaignMissionCount, static_cast<int>(missions.size()));
        const int count = m_customGame ? static_cast<int>(m_skirmishMaps.size()) : missionCount;
        CampaignLayout l = campaignLayout(windowW, windowH, count);
        const int rows = m_customGame ? customListRows(l) : count;
        const int first = m_customGame ? std::clamp(m_skirmishTop, 0, std::max(0, count - rows)) : 0;
        const int selected = m_customGame ? m_skirmishSelected : m_campaignMissionSelected;
        ui::drawDialog(renderer, fontRenderer, windowW, windowH, l.dialog, m_customGame ? _("SELECT MAP") : _("SELECT CAMPAIGN MISSION"));

        for(int row = 0; row < rows && first + row < count; ++row)
        {
            const int i = first + row;
            UiRect r = l.item(row);
            bool isSelected = (i == selected);
            bool hovered = r.contains(m_mouseX, m_mouseY);

            renderer.fillRectScreen(r.x, r.y, r.w, r.h, isSelected ? col::Hover : (hovered ? col::PanelRaised : col::PanelSunken));
            renderer.drawRectScreen(r.x, r.y, r.w, r.h, isSelected ? col::BorderStrong : col::Border);
            if(isSelected)
                renderer.fillRectScreen(r.x, r.y, 3, r.h, col::Accent);

            std::string num = std::to_string(i + 1);
            fontRenderer.drawString(num, r.x + 26, r.centerY(), ui::FONT_BODY_BOLD, isSelected ? col::Accent : col::TextMuted, false, Anchor::HCENTER | Anchor::VCENTER);
            std::string name;
            if(m_customGame)
            {
                name = m_skirmishMaps[i].displayName();
            }
            else
            {
                // Mission names are "Mission N: Title" - show just the title next to the number
                name = _(missions[i].name);
                size_t colon = name.find(": ");
                if(colon != std::string::npos)
                    name = name.substr(colon + 2);
            }
            fontRenderer.drawString(ui::fitText(fontRenderer, name, ui::FONT_BODY, r.w - 56), r.x + 48, r.centerY(), ui::FONT_BODY, isSelected ? col::Text : col::TextDim, false, Anchor::LEFT | Anchor::VCENTER);
        }

        if(m_customGame && count > rows)
        {
            // Scroll bar beside the list
            const int trackX = l.list.right() + 4;
            renderer.fillRectScreen(trackX, l.list.y, 4, l.list.h, col::PanelSunken);
            const int thumbH = std::max(20, l.list.h * rows / count);
            const int thumbY = l.list.y + (l.list.h - thumbH) * first / std::max(1, count - rows);
            renderer.fillRectScreen(trackX, thumbY, 4, thumbH, col::BorderStrong);
        }

        if(m_customGame && selected >= 0 && selected < count)
        {
            const SkirmishMapInfo &info = m_skirmishMaps[selected];
            const UiRect &d = l.details;
            ui::drawPanel(renderer, d, col::PanelSunken, col::Border);

            int x = d.x + 20;
            int w = d.w - 40;
            const int titleW = info.hasPrebuilt ? w - 220 : w;
            fontRenderer.drawString(ui::fitText(fontRenderer, info.displayName(), ui::FONT_H2, titleW), x, d.y + 18, ui::FONT_H2, col::Accent, true);
            if(info.hasPrebuilt)
            {
                ui::drawComboField(renderer, fontRenderer, l.layoutBtn, m_skirmishPrebuilt ? _("PREBUILT BASES") : _("FROM SCRATCH"), m_combo.open == kComboLayout, l.layoutBtn.contains(m_mouseX, m_mouseY));
            }
            fontRenderer.drawString(info.name + ".json", x, d.y + 50, ui::FONT_SMALL, col::TextMuted, false);
            renderer.fillRectScreen(x, d.y + 74, w, 1, col::Border);
            const std::string size = std::to_string(info.width) + " x " + std::to_string(info.height);
            drawTextSection(renderer, fontRenderer, x, d.y + 90, w, _("MAP SIZE:"), size, col::Ok, ui::FONT_BODY);
            drawTextSection(renderer, fontRenderer, x, d.y + 150, w, _("OBJECTIVES:"), _("- Build up your base.\n- Destroy the enemy base and all enemy forces."), col::TextDim, ui::FONT_SMALL);
            int pw = 0, ph = 0;
            SDL_Texture *tex = m_mapPreview.get(renderer.getSDLRenderer(), texManager, info.path, info.path, {}, pw, ph);
            drawMapPreview(renderer, tex, pw, ph, d);
        }
        else if(!m_customGame && selected >= 0 && selected < count)
        {
            const auto &minfo = missions[selected];
            const UiRect &d = l.details;
            ui::drawPanel(renderer, d, col::PanelSunken, col::Border);

            int x = d.x + 20;
            int w = d.w - 40;
            fontRenderer.drawString(ui::fitText(fontRenderer, _(minfo.name), ui::FONT_H2, w), x, d.y + 18, ui::FONT_H2, col::Accent, true);
            fontRenderer.drawString(_(minfo.mapName), x, d.y + 50, ui::FONT_SMALL, col::TextMuted, false);
            renderer.fillRectScreen(x, d.y + 74, w, 1, col::Border);

            int y = drawTextSection(renderer, fontRenderer, x, d.y + 90, w, _("OBJECTIVES:"), _(minfo.objective), col::Ok, ui::FONT_BODY);
            drawTextSection(renderer, fontRenderer, x, y, w, _("BRIEFING:"), _(minfo.briefing), col::TextDim, ui::FONT_SMALL);

            // The mission's map with its preplaced bases: Resistance red, Confederation blue
            const MissionData &data = Missions::campaign(playerFaction())[selected].data;
            std::vector<PreviewMarker> markers;
            for(const MissionEntity &e : data.entities)
            {
                if(e.type >= 15)
                    continue;
                markers.push_back({e.x, e.y, e.type >= 8 ? SDL_Color {230, 70, 60, 255} : SDL_Color {70, 150, 240, 255}});
            }
            int pw = 0, ph = 0;
            // Each side has its own campaign: the key keeps their pictures apart
            const std::string previewKey = std::string("mission:") + (playerFaction() == Faction::Confederation ? "c" : "r") + std::to_string(selected);
            SDL_Texture *tex = m_mapPreview.get(renderer.getSDLRenderer(), texManager, previewKey, "assets/maps/" + data.mapFile + ".json", markers, pw, ph);
            drawMapPreview(renderer, tex, pw, ph, d);
        }

        // Side and difficulty: drop-down lists
        {
            const SDL_Color sideColor = playerFaction() == Faction::Confederation ? SDL_Color {64, 150, 226, 255} : SDL_Color {214, 72, 56, 255};
            ui::drawComboField(renderer, fontRenderer, l.sideBtn[0], campaignComboOptions(kComboSide)[campaignComboCurrent(kComboSide)], m_combo.open == kComboSide, l.sideBtn[0].contains(m_mouseX, m_mouseY), true, sideColor);
            ui::drawComboField(renderer, fontRenderer, l.difficultyBtn, std::string(_("DIFFICULTY")) + ": " + difficultyName(m_difficulty), m_combo.open == kComboDifficulty, l.difficultyBtn.contains(m_mouseX, m_mouseY));
        }
        ui::drawButton(renderer, fontRenderer, l.editorBtn, _("MAP EDITOR"), ui::ButtonStyle::Secondary, l.editorBtn.contains(m_mouseX, m_mouseY));
        if(!m_editorNotice.empty())
        {
            fontRenderer.drawString(ui::fitText(fontRenderer, m_editorNotice, ui::FONT_SMALL, l.dialog.w - 280), l.dialog.right() - 20, l.dialog.y + 26, ui::FONT_SMALL, col::Danger, false, Anchor::RIGHT | Anchor::VCENTER);
        }
        ui::drawButton(renderer, fontRenderer, l.startBtn, m_customGame ? _("START GAME") : _("START MISSION"), ui::ButtonStyle::Primary, l.startBtn.contains(m_mouseX, m_mouseY));
        ui::drawButton(renderer, fontRenderer, l.backBtn, _("BACK"), ui::ButtonStyle::Secondary, l.backBtn.contains(m_mouseX, m_mouseY));
        if(m_combo.open >= kComboSide)
        { // the open list, over everything
            const int id = m_combo.open;
            const UiRect field = id == kComboSide ? l.sideBtn[0] : (id == kComboDifficulty ? l.difficultyBtn : l.layoutBtn);
            ui::drawComboList(renderer, fontRenderer, field, campaignComboOptions(id), campaignComboCurrent(id), m_combo, windowH, m_mouseX, m_mouseY);
        }
    }

    std::vector<std::string> MenuController::campaignComboOptions(int id) const
    {
        if(id == kComboSide)
            return {_("RESISTANCE"), _("CONFEDERACY")};
        if(id == kComboDifficulty)
        {
            return {difficultyName(AIDifficulty::Easy), difficultyName(AIDifficulty::Medium), difficultyName(AIDifficulty::Hard), difficultyName(AIDifficulty::Hardcore)};
        }
        return {_("PREBUILT BASES"), _("FROM SCRATCH")};
    }

    int MenuController::campaignComboCurrent(int id) const
    {
        if(id == kComboSide)
            return playerFaction() == Faction::Confederation ? 1 : 0;
        if(id == kComboDifficulty)
            return std::clamp(static_cast<int>(m_difficulty), 0, 3);
        return m_skirmishPrebuilt ? 0 : 1;
    }

    void MenuController::campaignComboApply(int id, int index)
    {
        if(id == kComboSide)
        {
            setPlayerFaction(index == 1 ? Faction::Confederation : Faction::Resistance);
        }
        else if(id == kComboDifficulty)
        {
            if(!m_settings)
                return;
            m_settings->difficulty = std::clamp(index, 0, 3);
            m_difficulty = static_cast<AIDifficulty>(m_settings->difficulty);
            if(m_onSettingsChanged)
                m_onSettingsChanged(SettingsArea::Game);
        }
        else
        {
            m_skirmishPrebuilt = index == 0;
        }
    }

} // namespace OpenArtOfWar
