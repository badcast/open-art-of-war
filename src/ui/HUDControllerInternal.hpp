// Layout helpers shared by the HUDController*.cpp files (not part of the public interface)
#pragma once

#include "ui/HUDController.hpp"
#include "graphics/SpriteRegistry.hpp"
#include "audio/SoundRequests.hpp"
#include "game/Players.hpp"
#include "game/BuildLimits.hpp"
#include "ui/UiKit.hpp"
#include "ui/UiLayout.hpp"
#include "core/Localization.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/ShaderEffects.hpp"
#include "graphics/Camera.hpp"
#include "graphics/TextureManager.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/Research.hpp"
#include "graphics/RenderUtility.hpp"
#include "game/Sides.hpp"
#include <algorithm>
#include <cmath>

namespace OpenArtOfWar
{

using ui::UiRect;
namespace col = ui::color;

// Cameo drawing shared by the PC sidebar and the touch interface (HUDControllerRender.cpp)
void DrawCostTag(Renderer &r, BitmapFontRenderer &font, const UiRect &cell, std::string_view text, SDL_Color color);
void DrawPowerTag(Renderer &r, BitmapFontRenderer &font, const UiRect &cell, int powerCost, int iconSpriteId);
void DrawCameo(
    Renderer &r,
    BitmapFontRenderer &font,
    const UiRect &cell,
    int compId,
    std::string_view name,
    bool hovered,
    bool selected,
    bool locked);
void DrawProgressShutter(Renderer &r, BitmapFontRenderer &font, const UiRect &cell, float progress, SDL_Color tint);

namespace menu_detail
{

// ---------------------------------------------------------------------------
// Static catalogs shown in the sidebar
// ---------------------------------------------------------------------------

// Sidebar cards for trainable units; name / cost come from the YAML config, the description from its translation key
struct UnitCameoDef
{
    int archetype;
    const char *icon; // cameo sprite
    const char *name;
    int cost;
    const char *desc;
};

inline const UnitCameoDef kResistanceInfantry[] = {
    {0, "icons_res.rifleman", "RIFLEMAN", 80, "Standard Resistance infantry rifleman"},
    {2, "icons_res.grenadier", "GRENADIER", 120, "Heavy rocket soldier vs vehicles and walls"},
    {4, "icons_res.sniper", "SNIPER", 160, "Long-range sniper (Camouflage mode [G])"},
};

inline const UnitCameoDef kResistanceVehicles[] = {
    {6, "icons_res.coyote", "COYOTE", 220, "Fast scout buggy (spawns at Factory)"},
    {8, "icons_res.assault_tank", "ARMADILLO", 350, "Main battle tank of Resistance"},
    {10, "icons_res.heavy_tank", "RHINO", 480, "Heavy twin-cannon tank (Siege mode [G])"},
    {12, "icons_res.mlrs", "PORCUPINE", 550, "Multiple Launch Rocket System (MLRS)"},
};

inline const UnitCameoDef kConfederationInfantry[] = {
    {1, "icons_conf.rifleman", "RIFLEMAN", 80, "Standard Confederation infantry rifleman"},
    {3, "icons_conf.heavy_assault", "HEAVY ASSAULT", 120, "Rocket infantry vs vehicles and walls"},
    {5, "icons_conf.flamethrower", "FLAMETHROWER", 140, "Short-range flame infantry vs infantry"},
};

inline const UnitCameoDef kConfederationVehicles[] = {
    {7, "icons_conf.fortress", "FORTRESS", 220, "Armored machine-gun carrier"},
    {9, "icons_conf.hammer", "HAMMER", 350, "Main battle tank of Confederation"},
    {11, "icons_conf.zeus", "ZEUS", 500, "Heavy tank (Siege mode [G])"},
    {13, "icons_conf.grad", "GRAD", 700, "Multiple Launch Rocket System (MLRS)"},
};

struct UnitCatalog
{
    const UnitCameoDef *items;
    int count;
};

// Units the player's faction trains at Barracks (infantry) or Factory (vehicles)
inline UnitCatalog unitCatalog(bool infantry)
{
    const bool resistance = PlayerFaction() == Faction::Resistance;
    if (infantry)
    {
        return resistance ? UnitCatalog {kResistanceInfantry, static_cast<int>(std::size(kResistanceInfantry))}
                          : UnitCatalog {kConfederationInfantry, static_cast<int>(std::size(kConfederationInfantry))};
    }
    return resistance ? UnitCatalog {kResistanceVehicles, static_cast<int>(std::size(kResistanceVehicles))}
                      : UnitCatalog {kConfederationVehicles, static_cast<int>(std::size(kConfederationVehicles))};
}

struct OrderDef
{
    const char *key; // keyboard shortcut, empty when the order is button-only
    const char *name;
    const char *desc;
};

inline const OrderDef kOrders[] = {
    {"Q", "ATTACK-MOVE", "Units advance to destination, attacking all encountered enemies"},
    {"G", "SIEGE", "Toggle Siege mode for Sniper and Rhino"},
    {"", "STOP", "Immediately halt all orders and movement"},
    {"", "DEFEND", "Hold current position and engage enemies in range"},
    // Stances (UnitStance order): what the units do about enemies when they have no orders
    {"Z", "GUARD", "Stance: engage enemies in sight, chase them a short way, then return"},
    {"X", "HOLD GROUND", "Stance: fire only at enemies in range, never leave the position"},
    {"C", "PURSUE", "Stance: engage enemies in sight and chase them to the end (default)"},
};

inline constexpr int kFirstStanceOrder = 4;

// ---------------------------------------------------------------------------
// Sidebar geometry (shared by rendering and hit-testing)
// ---------------------------------------------------------------------------

struct SidebarLayout
{
    UiRect panel;
    UiRect header;
    UiRect radar;
    UiRect menuBtn;
    UiRect objectivesBtn;
    UiRect slowerBtn;
    UiRect speedBtn;
    UiRect fasterBtn;
    UiRect researchBtn; // opens the window of the technology tree
    UiRect tabs[HUDController::TabCount];
    UiRect strip;
    UiRect scrollUp;
    UiRect scrollDown;
    UiRect primaryBtn;
    UiRect grid;
    UiRect info;

    static constexpr int kCols = 2;
    static constexpr int kCellW = 136;
    static constexpr int kCellH = 84;
    static constexpr int kGap = 8;

    UiRect Cell(int index, int scrollPx) const
    {
        int col = index % kCols;
        int row = index / kCols;
        return {grid.x + col * (kCellW + kGap), grid.y + row * (kCellH + kGap) - scrollPx, kCellW, kCellH};
    }
    int VisibleRows() const
    {
        return std::max(1, (grid.h + kGap) / (kCellH + kGap));
    }
};

inline SidebarLayout sidebarLayoutBuiltIn(int windowW, int windowH, int top, bool showSpeed)
{
    SidebarLayout l;
    const int w = HUDController::kSidebarWidth;
    const int x = windowW - w;
    const int pad = 10;
    const int innerW = w - 2 * pad;

    l.panel = {x, 0, w, windowH};
    l.header = {x + pad, top + 8, innerW, 40};
    l.radar = {x + pad, top + 54, innerW, 150};
    if (showSpeed)
    {
        l.menuBtn = {x + pad, top + 212, 82, 30};
        l.objectivesBtn = {l.menuBtn.Right() + 6, top + 212, 92, 30};
        l.slowerBtn = {l.objectivesBtn.Right() + 6, top + 212, 24, 30};
        l.speedBtn = {l.slowerBtn.Right() + 2, top + 212, 42, 30};
        l.fasterBtn = {l.speedBtn.Right() + 2, top + 212, 24, 30};
    }
    else
    {
        l.menuBtn = {x + pad, top + 212, (innerW - 8) / 2, 30};
        l.objectivesBtn = {l.menuBtn.Right() + 8, top + 212, innerW - l.menuBtn.w - 8, 30};
    }
    l.researchBtn = {x + pad, top + 248, innerW, 30};

    const int tabGap = 5;
    const int tabW = (innerW - (HUDController::TabCount - 1) * tabGap) / HUDController::TabCount;
    for (int t = 0; t < HUDController::TabCount; ++t)
    {
        l.tabs[t] = {x + pad + t * (tabW + tabGap), top + 286, tabW, 44};
    }

    l.strip = {x + pad, top + 336, innerW, 24};
    l.scrollDown = {l.strip.Right() - 24, l.strip.y, 24, l.strip.h};
    l.scrollUp = {l.scrollDown.x - 26, l.strip.y, 24, l.strip.h};
    l.primaryBtn = {l.strip.x + innerW / 2 - 10, l.strip.y, innerW / 2 - 44, l.strip.h};

    l.info = {x + pad, windowH - 82, innerW, 72};
    l.grid = {x + pad, top + 366, innerW, std::max(SidebarLayout::kCellH, l.info.y - 8 - (top + 366))};
    return l;
}

// The sidebar of the desktop interface: ui/layout.yaml (screen hud, profile pc); the built-in numbers above stand in
// when the file has no such profile
inline SidebarLayout sidebarLayout(int windowW, int windowH, int top, bool showSpeed = false)
{
    const uilayout::Layout &file = uilayout::Current();
    if (!file.Has("hud", "pc"))
        return sidebarLayoutBuiltIn(windowW, windowH, top, showSpeed);
    const std::map<std::string, float> extra {{"T", static_cast<float>(top)}};
    const uilayout::Solved s = file.Solve(
        "hud", "pc", windowW, windowH, [showSpeed](std::string_view n) { return n == "speed" && showSpeed; }, &extra);
    SidebarLayout l;
    l.panel = s.Rect("panel");
    l.header = s.Rect("header");
    l.radar = s.Rect("radar");
    l.menuBtn = showSpeed ? s.Rect("menu_s") : s.Rect("menu");
    l.objectivesBtn = showSpeed ? s.Rect("objectives_s") : s.Rect("objectives");
    l.slowerBtn = s.Rect("slower");
    l.speedBtn = s.Rect("speed");
    l.fasterBtn = s.Rect("faster");
    l.researchBtn = s.Rect("research");
    for (int t = 0; t < HUDController::TabCount; ++t)
        l.tabs[t] = s.Rect("tab" + std::to_string(t));
    l.strip = s.Rect("strip");
    l.scrollUp = s.Rect("scroll_up");
    l.scrollDown = s.Rect("scroll_down");
    l.primaryBtn = s.Rect("primary");
    l.grid = s.Rect("grid");
    l.info = s.Rect("info");
    return l;
}

// Map-to-radar transform: the whole map letterboxed, or with zoom a window of it around (centerX, centerY)
struct RadarView
{
    float scale {1.0f};
    float scaleY {1.0f}; // pixels per tile down the screen (the tactical view's tiles are not square)
    int ox {0};
    int oy {0};
    int w {0};
    int h {0};
    float viewX {0.0f}; // first tile shown (zoomed radar)
    float viewY {0.0f};

    float ToScreenX(float tileX) const
    {
        return ox + (tileX - viewX) * scale;
    }
    float ToScreenY(float tileY) const
    {
        return oy + (tileY - viewY) * scaleY;
    }
};

inline RadarView radarView(
    const UiRect &radar, int mapW, int mapH, int zoom = 1, float centerX = 0.0f, float centerY = 0.0f)
{
    RadarView v;
    if (mapW <= 0 || mapH <= 0)
        return v;
    const float innerW = static_cast<float>(radar.w - 4);
    const float innerH = static_cast<float>(radar.h - 4);
    v.scale = std::min(innerW / mapW, innerH / mapH) * std::max(1, zoom);
    v.scaleY = v.scale;
    auto axis = [&](float inner, int mapSize, float center, int start, int size, int &o, int &len, float &view) {
        const float tiles = inner / v.scale;
        if (tiles >= mapSize)
        {
            len = static_cast<int>(mapSize * v.scale);
            o = start + (size - len) / 2;
            view = 0.0f;
        }
        else
        {
            len = static_cast<int>(inner);
            o = start + 2;
            view = std::clamp(center - tiles * 0.5f, 0.0f, mapSize - tiles);
        }
    };
    axis(innerW, mapW, centerX, radar.x, radar.w, v.ox, v.w, v.viewX);
    axis(innerH, mapH, centerY, radar.y, radar.h, v.oy, v.h, v.viewY);
    return v;
}

// "Destroy" for the player's selected structures, bottom-right in the info box
inline UiRect DestroyBtn(const UiRect &info)
{
    return {info.Right() - 92, info.Bottom() - 26, 84, 20};
}

inline UiRect RepairBtn(const UiRect &info)
{
    return {info.Right() - 184, info.Bottom() - 26, 84, 20};
}

inline std::vector<const Building *> OwnSelectedBuildings(
    const EntityManager &entities, const std::vector<int> &selected)
{
    std::vector<const Building *> out;
    for (int id : selected)
    {
        const Building *b = entities.GetBuilding(id);
        if (b && b->alive && b->state != ActionState::Die && b->owner == Players::Local())
            out.push_back(b);
    }
    return out;
}

// Zoom buttons in the radar's bottom-right corner
inline UiRect RadarZoomInBtn(const UiRect &radar)
{
    return {radar.Right() - 22, radar.Bottom() - 22, 20, 20};
}

inline UiRect RadarZoomOutBtn(const UiRect &radar)
{
    return {radar.Right() - 44, radar.Bottom() - 22, 20, 20};
}

// The infinity button in the bottom right corner of a unit cameo
inline UiRect InfinityBtn(const UiRect &cell)
{
    return {cell.Right() - 28, cell.Bottom() - 22, 24, 18};
}

} // namespace menu_detail

using namespace menu_detail;

} // namespace OpenArtOfWar
