// The layout file of the interface: expressions, references between nodes, flows, conditions, profiles, bad files.
#include "ui/UiLayout.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

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

    const uilayout::Conditions cond = [](const std::string &n) { return n == "on"; };
    uilayout::Solved s = l.Solve("hud", "touch", 1280, 720, cond);
    Check(s.u == 1.0f, "scale 1 at 720p");
    Check(s.Rect("a").x == 1180 && s.Rect("a").w == 90, "expressions with units");
    Check(s.Rect("b").x == 1170 && s.Rect("b").y == 50, "a node refers to the one above it");
    Check(s.Shown("b"), "visible_if with ! and a known name");

    const uilayout::Solved big = l.Solve("hud", "touch", 2880, 1440, cond);
    Check(big.u == 2.0f && big.Rect("a").w == 180, "the scale follows the window, clamped");

    const uilayout::Conditions off = [](const std::string &n) { return n == "on" || n == "off"; };
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
        Check(menu.Rect("item0").x == 91 && menu.Rect("item0").y == 176 && menu.Rect("item9").y == 626, "main menu numbers");
        const uilayout::Solved pause = shipped.Solve("pause", "pc", 1280, 720, {});
        Check(pause.Rect("dialog").y == 154 && pause.Rect("item5").y == 154 + 342, "pause numbers");
        const uilayout::Solved maps = shipped.Solve("map_select", "pc", 1280, 720, {});
        Check(maps.Rect("list").x == 20 && maps.Rect("list").y == 68 && maps.Rect("list").w == 358 && maps.Rect("list").h == 580,
              "map window list");
        Check(maps.Rect("details").x == 394 && maps.Rect("back").x == 1120 && maps.Rect("start").x == 908, "map window details and footer");
        Check(maps.Rect("room_chat_field").y == 68 + 580 - 36, "room chat under the list");
        const uilayout::Solved yn = shipped.Solve("confirm", "touch", 1280, 720, {});
        Check(yn.Rect("yes").h >= 70 && yn.Rect("no").h >= 70 && yn.Rect("yes").w >= 300, "touch Yes / No buttons are big");
        Check(shipped.Solve("pause", "touch", 1280, 720, {}).Rect("item0").h > 46, "touch pause buttons are bigger");
        const uilayout::Solved net = shipped.Solve("network", "pc", 1280, 720, {});
        Check(net.Rect("dialog").w == 1000 && net.Rect("join").w == 220 && net.Rect("host").x == net.Rect("join").x - 232,
              "network numbers");
    }

    if (g_failures == 0)
        std::printf("all passed\n");
    return g_failures == 0 ? 0 : 1;
}
