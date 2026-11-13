// The layout file of the interface: expressions, references between nodes, flows, conditions, profiles, bad files.
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/ShaderEffects.hpp"
#include "ui/TouchHUD.hpp"
#include "ui/UiKit.hpp"
#include "ui/UiLayout.hpp"
#include "../src/ui/MenuControllerInternal.hpp"
#include "../src/ui/TouchMenuGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

using namespace OpenArtOfWar;

namespace
{
int g_failures = 0;
void Check(bool ok, const char *what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}
const char *kFile = R"Y(
screens:
  hud:
    touch:
      scale: "clamp(H / 720, 1, 2)"
      nodes:
        - {id: a, type: button, label: A, action: x, rect: ["W - 100u", "8u", "90u", "40u"]}
        - {id: b, type: button, label: B, action: y, rect: ["a.x - 10u", "a.bottom + 2", "90u", "40u"], visible_if: "on && !off"}
        - id: row
          type: flow
          dir: row
          gap: 10
          rect: [0, 0, 250, 30]
          children:
            - {id: c1, type: button, size: [100, 30]}
            - {id: c2, type: button, size: [100, 30], visible_if: never}
            - {id: c3, type: button, size: [100, 30]}
            - {id: c4, type: button, size: [100, 30]}
    pc:
      nodes:
        - {id: a, type: panel, rect: [0, 0, "300", "H"]}
)Y";
} // namespace

int main()
{
    uilayout::Layout l;
    std::string err;
    Check(l.Load(kFile, err), "the file parses");
    Check(l.Has("hud", "touch") && l.Has("hud", "pc") && !l.Has("hud", "tv"), "profiles");

    const uilayout::Conditions cond = [](std::string_view n) {
        return n == "on";
    };
    uilayout::Solved s = l.Solve("hud", "touch", 1280, 720, cond);
    Check(s.u == 1.0f, "scale 1 at 720p");
    Check(s.Rect("a").x == 1180 && s.Rect("a").w == 90, "expressions with units");
    Check(s.Rect("b").x == 1170 && s.Rect("b").y == 50, "a node refers to the one above it");
    Check(s.Shown("b"), "visible_if with ! and a known name");

    const uilayout::Solved big = l.Solve("hud", "touch", 2880, 1440, cond);
    Check(big.u == 2.0f && big.Rect("a").w == 180, "the scale follows the window, clamped");

    const uilayout::Conditions off = [](std::string_view n) {
        return n == "on" || n == "off";
    };
    Check(!l.Solve("hud", "touch", 1280, 720, off).Shown("b"), "a false condition hides the node");

    Check(s.Shown("c1") && !s.Shown("c2") && s.Rect("c3").x == 110, "a flow skips hidden children");
    Check(!s.Shown("c4"), "a flow leaves out what does not fit");

    Check(s.HitButton(1190, 20) && s.HitButton(1190, 20)->action == "x", "hit test finds the button");
    Check(!s.HitButton(5, 500), "and nothing on the open map");

    Check(l.Solve("hud", "pc", 1280, 720, cond).Rect("a").h == 720, "the other profile");

    uilayout::Layout bad;
    Check(!bad.Load("screens: [", err) && !err.empty(), "a broken file is refused with a message");
    Check(!bad.Load("screens:\n  hud:\n    pc:\n      nodes:\n        - {type: panel}\n", err), "a node without id");
    Check(!bad.Has("hud", "pc"), "and nothing of it is kept");

    // The layout file the game ships (run from the repo root): every screen is there and gives the old numbers
    {
        std::ifstream in("assets/ui/layout.yaml");
        std::stringstream text;
        text << in.rdbuf();
        uilayout::Layout shipped;
        Check(shipped.Load(text.str(), err), "assets/ui/layout.yaml parses");
        for (const char *screen : {"main_menu", "pause", "settings", "network", "map_select", "confirm"})
            Check(shipped.Has(screen, "pc"), "a menu screen of the shipped file");
        Check(shipped.Has("hud", "pc") && shipped.Has("hud", "touch"), "both HUD profiles");
        const uilayout::Solved hud = shipped.Solve("hud", "pc", 1280, 720, {});
        Check(hud.Rect("radar").x == 990 && hud.Rect("radar").y == 54 && hud.Rect("grid").w == 280, "sidebar numbers");
        const uilayout::Solved menu = shipped.Solve("main_menu", "pc", 1280, 720, {});
        Check(
            menu.Rect("item0").x == 91 && menu.Rect("item0").y == 176 && menu.Rect("item9").y == 626,
            "main menu numbers");
        const uilayout::Solved pause = shipped.Solve("pause", "pc", 1280, 720, {});
        Check(pause.Rect("dialog").y == 142 && pause.Rect("item5").y == 142 + 362, "pause numbers");
        const uilayout::Solved maps = shipped.Solve("map_select", "pc", 1280, 720, {});
        Check(
            maps.Rect("list").x == 20 && maps.Rect("list").y == 68 && maps.Rect("list").w == 358
                && maps.Rect("list").h == 580,
            "map window list");
        Check(
            maps.Rect("details").x == 394 && maps.Rect("back").x == 1120 && maps.Rect("start").x == 908,
            "map window details and footer");
        Check(maps.Rect("room_chat_field").y == 68 + 580 - 36, "room chat under the list");
        const uilayout::Solved yn = shipped.Solve("confirm", "touch", 1280, 720, {});
        Check(
            yn.Rect("yes").h >= 70 && yn.Rect("no").h >= 70 && yn.Rect("yes").w >= 300,
            "touch Yes / No buttons are big");
        Check(shipped.Solve("pause", "touch", 1280, 720, {}).Rect("item0").h > 46, "touch pause buttons are bigger");
        // The technology tree window: the old numbers on the PC, finger-sized controls inside the screen on touch
        Check(shipped.Has("research", "pc") && shipped.Has("research", "touch"), "both research profiles");
        const std::map<std::string, float> top {{"T", 0.0f}};
        const uilayout::Solved tech = shipped.Solve("research", "pc", 1280, 720, {}, &top);
        Check(
            tech.Rect("content").x == 16 && tech.Rect("content").y == 60 && tech.Rect("content").h == 644,
            "research content");
        Check(
            tech.Rect("details").w == 324 && tech.Rect("tree").w == 912 && tech.Rect("zoom_out").x == 776,
            "research tree and details");
        Check(
            tech.Rect("close").h == 36 && tech.Rect("zoom_in").w == 34 && tech.Rect("start").h == 46,
            "research PC controls");
        for (const auto &size : {std::pair<int, int> {1280, 720}, {2400, 1080}, {1600, 720}})
        {
            const uilayout::Solved fingers = shipped.Solve("research", "touch", size.first, size.second, {}, &top);
            const ui::UiRect tree = fingers.Rect("tree"), details = fingers.Rect("details");
            Check(
                fingers.Rect("close").h >= 50 && fingers.Rect("search").h >= 50 && fingers.Rect("start").h >= 64,
                "research touch controls are finger-sized");
            Check(
                fingers.Rect("zoom_in").w >= 56 && fingers.Rect("zoom_in").h >= 56,
                "research touch zoom buttons are big");
            Check(
                tree.w > size.first / 2 && tree.Right() < details.x && details.Right() <= size.first,
                "research tree keeps most of the screen, details fit");
            Check(
                fingers.Rect("start").Bottom() <= details.Bottom() && details.Bottom() <= size.second,
                "research start button fits");
        }
        const std::map<std::string, float> bar {{"T", 64.0f}};
        const uilayout::Solved watched = shipped.Solve("research", "touch", 1280, 720, {}, &bar);
        Check(
            watched.Rect("dialog").y == 64 && watched.Rect("dialog").h == 720 - 64,
            "research window leaves the observer bar");
        const uilayout::Solved net = shipped.Solve("network", "pc", 1280, 720, {});
        Check(
            net.Rect("dialog").w == 1000 && net.Rect("join").w == 220 && net.Rect("host").x == net.Rect("join").x - 232,
            "network numbers");

        // Touch HUD layout across landscape resolutions and aspect ratios: no BOX toggle, no overlaps or off-screen
        // clipping
        const std::pair<int, int> touchSizes[] = {
            {800, 480},
            {800, 600},
            {960, 720},
            {1024, 768},
            {1152, 720},
            {1280, 720},
            {1600, 720},
            {1680, 720},
            {1920, 1080},
            {2400, 1080},
            {2560, 1440},
        };
        auto inBounds = [](const ui::UiRect &r, int w, int h) {
            return r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.Right() <= w && r.Bottom() <= h;
        };
        for (const auto &sz : touchSizes)
        {
            const int w = sz.first, h = sz.second;
            const uilayout::Conditions normalCond = [](std::string_view n) {
                return n == "has_combat" || n == "has_building" || n == "has_selection";
            };
            const uilayout::Solved hudTouch = shipped.Solve("hud", "touch", w, h, normalCond);
            Check(!hudTouch.Find("dock_box"), "BOX toggle button must be removed from hud/touch");
            for (const uilayout::Node &n : hudTouch.nodes)
            {
                Check(n.action != "dock_box" && n.label != "BOX", "no BOX action or label in hud/touch");
                if (n.visible && n.type != "flow" && n.type != "cards")
                    Check(inBounds(n.rect, w, h), "visible touch HUD node fits inside viewport");
            }
            const ui::UiRect res = hudTouch.Rect("resources");
            const ui::UiRect techBtn = hudTouch.Rect("tech");
            const ui::UiRect radar = hudTouch.Rect("radar");
            const ui::UiRect dockBuild = hudTouch.Rect("dock_build");
            const ui::UiRect dockUnits = hudTouch.Rect("dock_units");
            Check(res.Right() < techBtn.x, "resources panel does not overlap top-right tech button");
            Check(
                radar.Right() < dockBuild.x && dockBuild.Right() < dockUnits.x && dockUnits.Right() <= w,
                "radar and dock buttons do not overlap across landscape resolutions");
            const ui::UiRect orders = hudTouch.Rect("orders");
            Check(
                orders.x > radar.Right() && orders.Right() < dockBuild.x,
                "orders bar sits cleanly between radar and dock");

            // Units drawer open: cards and INF/VEH tabs fit and scale with u
            const uilayout::Conditions unitsCond = [](std::string_view n) {
                return n == "panel_open" || n == "panel_units" || n == "units_inf";
            };
            const uilayout::Solved unitsHud = shipped.Solve("hud", "touch", w, h, unitsCond);
            const ui::UiRect cards = unitsHud.Rect("cards");
            const ui::UiRect scrollL = unitsHud.Rect("scroll_left");
            const ui::UiRect scrollR = unitsHud.Rect("scroll_right");
            const ui::UiRect infBtn = unitsHud.Rect("units_inf");
            const ui::UiRect vehBtn = unitsHud.Rect("units_veh");
            Check(
                inBounds(cards, w, h) && inBounds(scrollL, w, h) && inBounds(scrollR, w, h),
                "touch card drawer and scroll arrows stay in bounds");
            Check(cards.h == static_cast<int>(std::lround(88 * unitsHud.u)), "card drawer height scales with u");
            Check(
                infBtn.x > scrollL.Right() && vehBtn.x > scrollL.Right() && vehBtn.Bottom() <= cards.Bottom(),
                "INF/VEH buttons fit inside card drawer without overlapping scroll arrow");

            // Building placement mode: CANCEL and BUILD HERE stay in bounds and clear the radar
            const uilayout::Conditions placingCond = [](std::string_view n) {
                return n == "placing";
            };
            const uilayout::Solved placingHud = shipped.Solve("hud", "touch", w, h, placingCond);
            const ui::UiRect cancelBtn = placingHud.Rect("cancel");
            const ui::UiRect confirmBtn = placingHud.Rect("confirm");
            Check(inBounds(cancelBtn, w, h) && inBounds(confirmBtn, w, h), "placing buttons stay in bounds");
            Check(
                placingHud.Rect("radar").Right() < cancelBtn.x && cancelBtn.Right() < confirmBtn.x,
                "placing buttons do not overlap radar or each other");

            // Touch main menu items stay inside viewport across short and tall landscape screens
            const uilayout::Solved mmTouch = shipped.Solve("main_menu", "touch", w, h, {});
            for (int i = 0; i < 10; ++i)
                Check(
                    inBounds(mmTouch.Rect("item" + std::to_string(i)), w, h), "touch main menu items fit in viewport");
        }

        // TouchHUD runtime behavior: box selection always on except when placing/building; responsive TouchPanels
        Check(Assets::Init() && LoaderResources::Init() && uilayout::Reload(), "load shipped layout into UiLayout");
        TouchHUD touchHud;
        Check(touchHud.ReservedWidth() == 0, "TouchHUD reserves zero sidebar width");
        Check(!touchHud.TouchPansCamera(), "TouchHUD defaults to always-on box selection (no single-finger pan)");
        Check(!touchHud.OwnsMapInput(), "TouchHUD does not own map input when not placing");
        const auto panels1280 = touchHud.TouchPanels(1280, 720);
        const auto panels1600 = touchHud.TouchPanels(1600, 720);
        const auto panels1920 = touchHud.TouchPanels(1920, 1080);
        auto maxRight = [](const std::vector<SDL_Rect> &rects) {
            int mr = 0;
            for (const SDL_Rect &r : rects)
                mr = std::max(mr, r.x + r.w);
            return mr;
        };
        Check(!panels1280.empty() && panels1280.size() == panels1600.size(), "TouchPanels adapts across widths");
        Check(
            maxRight(panels1600) > maxRight(panels1280),
            "TouchPanels shifts right-anchored HUD panels right on wider aspect ratio");
        Check(panels1920[0].w > panels1280[0].w, "TouchPanels scales panels up on higher resolution");

        touchHud.SetBuildMode(true);
        Check(touchHud.TouchPansCamera(), "TouchHUD disables box selection (enables pan) during building mode");
        Check(touchHud.OwnsMapInput(), "TouchHUD owns map input during building mode");
        touchHud.SetBuildMode(false);
        Check(
            !touchHud.TouchPansCamera() && !touchHud.OwnsMapInput(),
            "TouchHUD restores always-on box selection when leaving building mode");
    }

    if (g_failures == 0)
        std::printf("all passed\n");
    for (const auto size : {std::pair<int, int> {960, 720}, {1280, 720}, {1440, 720}})
    {
        for (int i = 0; i < 10; ++i)
        {
            const auto item = menu_detail::TouchMainMenuItemRect(i, size.first, size.second);
            Check(
                item.x >= 0 && item.Right() <= size.first && item.y >= 0 && item.Bottom() <= size.second - 30,
                "touch main menu item stays on screen and clear of the music player");
        }
    }

    // The touch panels share an offscreen crop: contents must survive at normal and scaled output sizes.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    Check(SDL_Init(SDL_INIT_VIDEO), "clip test SDL init");
    SDL_Window *window = SDL_CreateWindow("clip test", 800, 600, SDL_WINDOW_HIDDEN);
    SDL_Renderer *sdlRenderer = window ? SDL_CreateRenderer(window, "software") : nullptr;
    Check(sdlRenderer != nullptr, "clip test renderer");
    if (sdlRenderer)
    {
        {
            Renderer renderer(sdlRenderer, nullptr);
            for (float scale : {1.0f, 1.5f})
            {
                SDL_SetRenderScale(sdlRenderer, scale, scale);
                SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
                SDL_RenderClear(sdlRenderer);
                Check(renderer.BeginClipTarget({300, 200, 200, 150}, {0, 0, 0, 255}), "begin touch clip");
                renderer.FillRectScreen(310, 210, 20, 20, {255, 0, 0, 255});
                renderer.EndClipTarget();
                SDL_SetRenderScale(sdlRenderer, 1.0f, 1.0f);
                SDL_Rect sample {static_cast<int>(315 * scale), static_cast<int>(215 * scale), 1, 1};
                SDL_Surface *pixel = SDL_RenderReadPixels(sdlRenderer, &sample);
                Check(pixel != nullptr, "read touch clip pixel");
                if (pixel)
                {
                    Uint8 r, g, b, a;
                    SDL_GetRGBA(
                        *static_cast<Uint32 *>(pixel->pixels),
                        SDL_GetPixelFormatDetails(pixel->format),
                        nullptr,
                        &r,
                        &g,
                        &b,
                        &a);
                    Check(r > 200 && g < 20 && b < 20, "touch clip displays content at scale");
                    SDL_DestroySurface(pixel);
                }
            }
            SDL_SetRenderScale(sdlRenderer, 1.0f, 1.0f);
            SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
            SDL_RenderClear(sdlRenderer);
            Check(renderer.BeginClipTarget({300, 200, 200, 150}, {0, 0, 0, 255}), "begin translucent touch clip");
            renderer.FillRectScreen(310, 210, 20, 20, {255, 0, 0, 255});
            renderer.EndClipTarget(128);
            SDL_Rect sample {315, 215, 1, 1};
            SDL_Surface *pixel = SDL_RenderReadPixels(sdlRenderer, &sample);
            Check(pixel != nullptr, "read translucent touch clip");
            if (pixel)
            {
                Uint8 r, g, b, a;
                SDL_GetRGBA(
                    *static_cast<Uint32 *>(pixel->pixels),
                    SDL_GetPixelFormatDetails(pixel->format),
                    nullptr,
                    &r,
                    &g,
                    &b,
                    &a);
                Check(r >= 120 && r <= 136 && g < 20 && b < 20, "touch clip uses half opacity");
                SDL_DestroySurface(pixel);
            }
        }

        // Touch main menu items never overlap the bottom music player and activate cleanly when tapped even when
        // Camera is zoomed and panned.
        {
            auto overlaps = [](const ui::UiRect &a, const ui::UiRect &b) {
                return a.x < b.Right() && a.Right() > b.x && a.y < b.Bottom() && a.Bottom() > b.y;
            };
            uilayout::SetMenuProfile("touch");
            for (const auto size :
                 {std::pair<int, int> {960, 540},
                  {960, 720},
                  {1280, 720},
                  {1440, 720},
                  {1920, 1080}})
            {
                const auto player = menu_detail::playerLayout(size.first, size.second);
                for (int i = 0; i < 10; ++i)
                {
                    const auto item = menu_detail::TouchMainMenuItemRect(i, size.first, size.second);
                    Check(
                        !overlaps(item, player.panel) && !overlaps(item, player.seek),
                        "touch main menu button must not overlap music player");
                }
            }

            InputManager input;
            input.SetTouchMode(true);
            input.SetMenuOpen(true);
            TileMap map;
            Pathfinder pathfinder(&map);
            EntityManager entities;
            Economy economy;
            AIController ai;
            MissionController mission;
            TouchHUD hud;
            MenuController menu;
            SoundEngine sound;

            for (const auto size : {std::pair<int, int> {960, 720}, {1280, 720}})
            {
                Camera cam(size.first, size.second);
                cam.SetZoom(2.0f);
                cam.CenterOn(2400.0f, 1800.0f);

                auto tapItem = [&](int index, AppState expectedState, bool expectedRunning) {
                    const auto rect = menu_detail::TouchMainMenuItemRect(index, size.first, size.second);
                    const float cx = static_cast<float>(rect.x + rect.w / 2);
                    const float cy = static_cast<float>(rect.y + rect.h / 2);
                    SDL_Event down {}, up {};
                    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                    down.button.button = SDL_BUTTON_LEFT;
                    down.button.x = cx;
                    down.button.y = cy;
                    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
                    up.button.button = SDL_BUTTON_LEFT;
                    up.button.x = cx;
                    up.button.y = cy;
                    input.ProcessEvent(down, cam);
                    input.ProcessEvent(up, cam);

                    AppState state = AppState::MainMenu;
                    bool running = true;
                    FrameContext ctx {
                        0.016f,
                        size.first,
                        size.second,
                        input,
                        cam,
                        map,
                        pathfinder,
                        entities,
                        economy,
                        ai,
                        mission,
                        hud,
                        menu,
                        sound,
                        state};
                    menu.UpdateMainMenu(ctx, running);
                    Check(
                        state == expectedState && running == expectedRunning,
                        "touch main menu tap must activate the clicked button");
                };

                tapItem(6, AppState::MenuSettings, true);
                tapItem(7, AppState::MenuHelp, true);
                tapItem(8, AppState::MenuAbout, true);
                tapItem(9, AppState::MainMenu, false);
            }
            uilayout::SetMenuProfile("pc");
        }
        // Kinetic smooth scrolling, flick inertia, 2D research physics, combo flicks, and About scroll-drag.
        {
            const ui::UiRect listRect {100, 100, 300, 300};
            const ui::UiRect trackRect {394, 100, 6, 300};
            const int contentH = 2000;

            // 1) Smooth mouse wheel easing (including fractional trackpad deltas) without sub-ms snapping
            ui::SmoothListScroll scroll;
            bool clicked = false;
            ui::UpdateSmoothList(scroll, listRect, trackRect, contentH, 60, 200, 200, false, false, 0, 0, 0.0f, clicked);
            ui::UpdateSmoothList(
                scroll, listRect, trackRect, contentH, 60, 200, 200, false, false, 0, 0, -0.5f, clicked);
            Check(std::fabs(scroll.target - 30.0f) < 1e-4f, "fractional -0.5 wheel targets 30 px");
            Check(scroll.offset < scroll.target, "wheel scroll must ease smoothly rather than snap to target");
            for (int i = 0; i < 6; ++i)
            {
                SDL_Delay(16);
                ui::UpdateSmoothList(
                    scroll, listRect, trackRect, contentH, 60, 200, 200, false, false, 0, 0, 0.0f, clicked);
            }
            Check(scroll.offset > 15.0f && scroll.offset <= 30.5f, "wheel easing advances smoothly over frames");

            // 2) Fast 1-motion-frame flick past 12 px slop: no slop snap, strong velocity, glides immediately on !down
            scroll.Reset();
            clicked = true;
            ui::UpdateSmoothList(
                scroll, listRect, trackRect, contentH, 60, 200, 320, true, true, 200, 320, 0.0f, clicked);
            SDL_Delay(16);
            ui::UpdateSmoothList(
                scroll, listRect, trackRect, contentH, 60, 200, 260, true, false, 200, 320, 0.0f, clicked);
            Check(
                std::fabs(scroll.offset - 48.0f) < 1e-3f,
                "crossing 12 px slop subtracts slop so drag starts smoothly without a jump (60 - 12 = 48 px)");
            Check(scroll.velocity > 1500.0f, "single fast motion frame past slop establishes strong flick velocity");
            Check(!clicked, "drag past slop cancels click");
            SDL_Delay(16);
            // Release frame with zero additional motion (same my = 260, down = false)
            ui::UpdateSmoothList(
                scroll, listRect, trackRect, contentH, 60, 200, 260, false, false, 200, 320, 0.0f, clicked);
            const float afterRelease = scroll.offset;
            Check(afterRelease > 55.0f && scroll.velocity > 1000.0f, "release frame preserves velocity and starts gliding");
            for (int i = 0; i < 5; ++i)
            {
                SDL_Delay(16);
                ui::UpdateSmoothList(
                    scroll, listRect, trackRect, contentH, 60, 200, 260, false, false, 200, 320, 0.0f, clicked);
            }
            Check(scroll.offset > afterRelease + 40.0f, "kinetic glide carries list forward smoothly across frames");

            // 3) Drag, pause/hold stationary, then release must damp velocity and not flick
            scroll.Reset();
            ui::UpdateSmoothList(
                scroll, listRect, trackRect, contentH, 60, 200, 320, true, true, 200, 320, 0.0f, clicked);
            SDL_Delay(16);
            ui::UpdateSmoothList(
                scroll, listRect, trackRect, contentH, 60, 200, 250, true, false, 200, 320, 0.0f, clicked);
            for (int i = 0; i < 8; ++i)
            {
                SDL_Delay(16);
                ui::UpdateSmoothList(
                    scroll, listRect, trackRect, contentH, 60, 200, 250, true, false, 200, 320, 0.0f, clicked);
            }
            ui::UpdateSmoothList(
                scroll, listRect, trackRect, contentH, 60, 200, 250, false, false, 200, 320, 0.0f, clicked);
            Check(std::fabs(scroll.velocity) < 60.0f, "holding stationary before release bleeds flick velocity");

            // 4) 2D kinetic scroll (used by ResearchWindow) glides along both axes without slop jump
            ui::KineticScroll2D k2;
            ui::StepKineticScroll2D(k2, listRect, 800.0f, 800.0f, 60.0f, 60.0f, 260, 260, true, true, 260, 260, 0, 0);
            SDL_Delay(16);
            ui::StepKineticScroll2D(
                k2, listRect, 800.0f, 800.0f, 60.0f, 60.0f, 200, 200, true, false, 260, 260, 0, 0);
            Check(k2.offsetX > 45.0f && k2.offsetX < 55.0f && k2.offsetY > 45.0f && k2.offsetY < 55.0f,
                  "2D kinetic drag subtracts 12 px radial slop smoothly");
            SDL_Delay(16);
            ui::StepKineticScroll2D(
                k2, listRect, 800.0f, 800.0f, 60.0f, 60.0f, 200, 200, false, false, 260, 260, 0, 0);
            Check(k2.velX > 1000.0f && k2.velY > 1000.0f && k2.offsetX > 55.0f,
                  "2D kinetic flick glides smoothly on release");

            // 5) ComboState flick glide works without snapping or picking on release
            ui::ComboState combo;
            combo.open = 0;
            const ui::UiRect field {100, 40, 200, 34};
            int picked = -1;
            ui::ComboUpdate(combo, field, 24, 720, 150, 150, false, 0.0f, picked, true, true, 150, 150);
            SDL_Delay(16);
            ui::ComboUpdate(combo, field, 24, 720, 150, 95, false, 0.0f, picked, true, false, 150, 150);
            SDL_Delay(16);
            const bool consumedRelease
                = ui::ComboUpdate(combo, field, 24, 720, 150, 95, true, 0.0f, picked, false, false, 150, 150);
            Check(consumedRelease && picked == -1 && combo.open == 0 && combo.velPx > 1000.0f,
                  "combo drag flick glides smoothly and does not pick/close on release");

            // 6) Clicking/dragging inside scrollable MenuAbout viewport keeps MenuAbout open and scrolls text
            MenuController menu;
            BitmapFontRenderer font(sdlRenderer);
            Renderer aboutRenderer(sdlRenderer, nullptr);
            AppState aboutState = AppState::MenuAbout;
            menu.RenderMenuAbout(aboutRenderer, font, 1280, 720);
            InputManager aboutInput;
            Camera aboutCam(1280, 720);
            aboutInput.SetMenuOpen(true);
            SDL_Event aboutDown {};
            aboutDown.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            aboutDown.button.button = SDL_BUTTON_LEFT;
            aboutDown.button.x = 640.0f;
            aboutDown.button.y = 320.0f;
            aboutInput.ProcessEvent(aboutDown, aboutCam);
            aboutInput.Update(0.016f, aboutCam);
            menu.UpdateInfoScreen(aboutInput, aboutState);
            Check(
                aboutState == AppState::MenuAbout,
                "pressing inside scrollable MenuAbout list must not immediately close About");
            SDL_Delay(16);
            SDL_Event aboutMove {};
            aboutMove.type = SDL_EVENT_MOUSE_MOTION;
            aboutMove.motion.x = 640.0f;
            aboutMove.motion.y = 240.0f;
            aboutInput.ProcessEvent(aboutMove, aboutCam);
            aboutInput.Update(0.016f, aboutCam);
            menu.UpdateInfoScreen(aboutInput, aboutState);
            Check(aboutState == AppState::MenuAbout, "dragging inside MenuAbout keeps About open");
            SDL_Event aboutUp {};
            aboutUp.type = SDL_EVENT_MOUSE_BUTTON_UP;
            aboutUp.button.button = SDL_BUTTON_LEFT;
            aboutUp.button.x = 640.0f;
            aboutUp.button.y = 240.0f;
            aboutInput.ProcessEvent(aboutUp, aboutCam);
            aboutInput.Update(0.016f, aboutCam);
            menu.UpdateInfoScreen(aboutInput, aboutState);
            Check(aboutState == AppState::MenuAbout, "releasing a drag inside MenuAbout keeps About open");
            SDL_Event aboutEsc {};
            aboutEsc.type = SDL_EVENT_KEY_DOWN;
            aboutEsc.key.scancode = SDL_SCANCODE_ESCAPE;
            aboutInput.ProcessEvent(aboutEsc, aboutCam);
            menu.UpdateInfoScreen(aboutInput, aboutState);
            Check(aboutState == AppState::MainMenu, "pressing Escape in MenuAbout returns to MainMenu");

            // 7) ResearchWindow composite icon fitting scales proportionally and centers offset/non-64x64 sprites
            {
                const ui::UiRect frame62 {100, 200, 62, 62};
                const ui::CompositeFit smallOff = ui::FitCompositeInRect(frame62, 4, 4.0f, 3.0f, 38.0f, 28.0f);
                const float renderedW = 34.0f * smallOff.scale;
                const float renderedH = 25.0f * smallOff.scale;
                const float left = smallOff.x + 4.0f * smallOff.scale;
                const float right = smallOff.x + 38.0f * smallOff.scale;
                const float top = smallOff.y + 3.0f * smallOff.scale;
                const float bottom = smallOff.y + 28.0f * smallOff.scale;
                Check(
                    std::fabs(renderedW - 54.0f) < 0.5f && renderedH <= 54.5f,
                    "FitCompositeInRect scales proportionally to fill padded frame");
                Check(
                    std::fabs((left - frame62.x) - (frame62.Right() - right)) <= 1.5f
                        && std::fabs((top - frame62.y) - (frame62.Bottom() - bottom)) <= 1.5f,
                    "FitCompositeInRect centers offset sprites horizontally and vertically");

                const ui::UiRect zoomedFrame {100, 200, 124, 124};
                const ui::CompositeFit zoomed = ui::FitCompositeInRect(zoomedFrame, 8, 4.0f, 3.0f, 38.0f, 28.0f);
                Check(
                    zoomed.scale > smallOff.scale * 1.9f,
                    "FitCompositeInRect scales up proportionally when icon frame grows");
            }

            // 8) TouchHUD button colors adapt to faction while keeping HUD icons white at opacity 200
            {
                const auto confIdle = TouchHUD::ResolveButtonColors(
                    ui::ButtonStyle::Secondary, false, false, false, false, ArmyColor(1));
                const auto resIdle = TouchHUD::ResolveButtonColors(
                    ui::ButtonStyle::Secondary, false, false, false, false, ArmyColor(2));
                Check(
                    confIdle.fill.b > confIdle.fill.r + 40 && resIdle.fill.r > resIdle.fill.b + 40,
                    "HUD button plates adapt vividly to Confederation blue vs Resistance red");
                Check(
                    confIdle.iconTint.r == 255 && confIdle.iconTint.g == 255 && confIdle.iconTint.b == 255
                        && confIdle.iconAlpha == 200 && resIdle.iconTint.r == 255 && resIdle.iconAlpha == 200,
                    "HUD button icons remain white with opacity 200");
                const auto confActive = TouchHUD::ResolveButtonColors(
                    ui::ButtonStyle::Secondary, true, false, false, false, ArmyColor(1));
                Check(
                    confActive.fill.a > confIdle.fill.a && confActive.iconAlpha == 200,
                    "active HUD button highlights fill while keeping white icon at opacity 200");
            }
        }
        SDL_DestroyRenderer(sdlRenderer);
    }
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();

    return g_failures == 0 ? 0 : 1;
}
