// The layout file of the interface: expressions, references between nodes, flows, conditions, profiles, bad files.
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "graphics/Renderer.hpp"
#include "ui/TouchHUD.hpp"
#include "ui/UiLayout.hpp"
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
                item.x >= 364 && item.Right() <= size.first && item.y >= 0 && item.Bottom() <= size.second - 30,
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
        SDL_DestroyRenderer(sdlRenderer);
    }
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();

    return g_failures == 0 ? 0 : 1;
}
