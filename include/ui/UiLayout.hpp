#pragma once

#include "ui/UiKit.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace OpenArtOfWar
{

class Renderer;
class BitmapFontRenderer;

// The layout of the interface as data: assets/ui/layout.yaml (see docs/ui-layout.md).
//
//   screens:
//     hud:
//       touch:                      # a profile: pc / touch
//         scale: "clamp(H / 720, 1, 2)"   # `u` in the expressions below
//         nodes:
//           - {id: menu, type: button, label: MENU, action: menu, rect: ["W - 96u", "8u", "88u", "46u"]}
//
// A rect is [x, y, w, h], each an expression over W, H (the window), u (the scale), numbers with an optional `u`
// ("8u" = 8 * u), + - * / ( ), min(a, b), max(a, b), clamp(v, lo, hi) and the rects of nodes written earlier:
// <id>.x .y .w .h .right .bottom .cx .cy (and `parent.` for the container). `flow` nodes lay their children out in a
// row (or column) one after another, skipping the invisible ones; children that do not fit are left out.
// The file only places and styles things: what a button does (`action`) and when something shows
// (`visible_if`, `active_if`, `disabled_if`: names the interface code answers, with ! && ||) stay in C++.
namespace uilayout
{

struct Node
{
    std::string id, type, label, action, style, visibleIf, activeIf, disabledIf;
    ui::UiRect rect;
    bool visible {true};
    bool active {false};
    bool disabled {false};
    float fontScale {1.0F};
};

// Answers a condition name (an unknown name is false)
using Conditions = std::function<bool(std::string_view)>;

struct Solved
{
    float u {1.0f};
    std::vector<Node> nodes;
    const Node *Find(std::string_view id) const;
    // The rect of a node (empty when there is none or it is not shown)
    ui::UiRect Rect(std::string_view id) const;
    bool Shown(std::string_view id) const;
    // The topmost shown button (enabled or not) under the point; nullptr when none
    const Node *HitButton(int x, int y) const;
    // Any shown node that takes touches (buttons, panels, radar, cards, ...) is under the point
    bool Covers(int x, int y) const;
};

class Layout
{
public:
    // Parses a layout file; on an error nothing changes and `error` says where
    bool Load(std::string_view yamlText, std::string &error);
    bool Has(std::string_view screen, std::string_view profile) const;
    Solved Solve(
        std::string_view screen,
        std::string_view profile,
        int windowW,
        int windowH,
        const Conditions &cond,
        const std::map<std::string, float> *extra = nullptr) const; // extra: more names for the expressions

    struct Def; // (the parsed form; opaque outside the .cpp)

private:
    std::shared_ptr<const std::map<std::string, Def, std::less<>>> m_defs;
};

// The profile the menus use (the HUD picks its own): "touch" with the touch interface, else "pc". A screen without
// the profile falls back to pc.
void SetMenuProfile(std::string_view profile);
const std::string &MenuProfile(const Layout &layout, std::string_view screen);

// The game's layout, read from the file named by main.yaml (`ui_layout`)
const Layout &Current();
// Once a second: the file is read again when it changed; a broken edit keeps the old layout (LastError tells)
void Poll(unsigned nowMs);
bool Reload();
const std::string &LastError();

// Draws the generic nodes of a solved layout: panel, button, label (other types are the interface's own to draw)
void DrawGeneric(Renderer &r, BitmapFontRenderer &font, const Solved &s, int mouseX, int mouseY);

} // namespace uilayout

} // namespace OpenArtOfWar
