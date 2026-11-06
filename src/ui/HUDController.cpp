#include "ui/HUDController.hpp"
#include "audio/SoundRequests.hpp"
#include "game/Players.hpp"
#include "ui/UiKit.hpp"
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

namespace
{

// ---------------------------------------------------------------------------
// Static catalogs shown in the sidebar
// ---------------------------------------------------------------------------

// Sidebar cards for trainable units; name / cost come from the YAML config, the description from its translation key
struct UnitCameoDef
{
    int archetype;
    int compId;
    const char *name;
    int cost;
    const char *desc;
};

const UnitCameoDef kResistanceInfantry[] = {
    {0, 2057, "RIFLEMAN", 80, "Standard Resistance infantry rifleman"},
    {2, 2058, "GRENADIER", 120, "Heavy rocket soldier vs vehicles and walls"},
    {4, 2059, "SNIPER", 160, "Long-range sniper (Camouflage mode [G])"},
};

const UnitCameoDef kResistanceVehicles[] = {
    {6, 2060, "COYOTE", 220, "Fast scout buggy (spawns at Factory)"},
    {8, 2061, "ARMADILLO", 350, "Main battle tank of Resistance"},
    {10, 2062, "RHINO", 480, "Heavy twin-cannon tank (Siege mode [G])"},
    {12, 2063, "PORCUPINE", 550, "Multiple Launch Rocket System (MLRS)"},
};

const UnitCameoDef kConfederationInfantry[] = {
    {1, 2049, "RIFLEMAN", 80, "Standard Confederation infantry rifleman"},
    {3, 2050, "HEAVY ASSAULT", 120, "Rocket infantry vs vehicles and walls"},
    {5, 2051, "FLAMETHROWER", 140, "Short-range flame infantry vs infantry"},
};

const UnitCameoDef kConfederationVehicles[] = {
    {7, 2052, "FORTRESS", 220, "Armored machine-gun carrier"},
    {9, 2053, "HAMMER", 350, "Main battle tank of Confederation"},
    {11, 2054, "ZEUS", 500, "Heavy tank (Siege mode [G])"},
    {13, 2056, "GRAD", 700, "Multiple Launch Rocket System (MLRS)"},
};

struct UnitCatalog
{
    const UnitCameoDef *items;
    int count;
};

// Units the player's faction trains at Barracks (infantry) or Factory (vehicles)
UnitCatalog unitCatalog(bool infantry)
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

const OrderDef kOrders[] = {
    {"Q", "ATTACK-MOVE", "Units advance to destination, attacking all encountered enemies"},
    {"G", "SIEGE", "Toggle Siege mode for Sniper and Rhino"},
    {"", "STOP", "Immediately halt all orders and movement"},
    {"", "DEFEND", "Hold current position and engage enemies in range"},
    // Stances (UnitStance order): what the units do about enemies when they have no orders
    {"Z", "GUARD", "Stance: engage enemies in sight, chase them a short way, then return"},
    {"X", "HOLD GROUND", "Stance: fire only at enemies in range, never leave the position"},
    {"C", "PURSUE", "Stance: engage enemies in sight and chase them to the end (default)"},
};
constexpr int kFirstStanceOrder = 4;

const char *const kTabNames[HUDController::TabCount] = {"BUILD", "INFANTRY", "VEHICLES", "ORDERS"};
// Tab icons per faction: HQ, rifleman, main tank, a research cameo; Orders draws a crosshair
const int kResistanceTabIcons[HUDController::TabCount] = {2074, 2057, 2061, -1};
const int kConfederationTabIcons[HUDController::TabCount] = {2064, 2049, 2053, -1};

int TabIcon(int tab)
{
    return (PlayerFaction() == Faction::Confederation ? kConfederationTabIcons : kResistanceTabIcons)[tab];
}

constexpr int kCameoW = 60; // native cameo composite size
constexpr int kCameoH = 38;

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

    UiRect Cell(int index, int scrollRow) const
    {
        int col = index % kCols;
        int row = index / kCols - scrollRow;
        return {grid.x + col * (kCellW + kGap), grid.y + row * (kCellH + kGap), kCellW, kCellH};
    }
    int VisibleRows() const
    {
        return std::max(1, (grid.h + kGap) / (kCellH + kGap));
    }
};

SidebarLayout sidebarLayout(int windowW, int windowH, int top)
{
    SidebarLayout l;
    const int w = HUDController::kSidebarWidth;
    const int x = windowW - w;
    const int pad = 10;
    const int innerW = w - 2 * pad;

    l.panel = {x, 0, w, windowH};
    l.header = {x + pad, top + 8, innerW, 40};
    l.radar = {x + pad, top + 54, innerW, 150};
    l.menuBtn = {x + pad, top + 212, (innerW - 8) / 2, 30};
    l.objectivesBtn = {l.menuBtn.Right() + 8, top + 212, innerW - l.menuBtn.w - 8, 30};
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

// Map-to-radar transform: the whole map letterboxed, or with zoom a window of it around (centerX, centerY)
struct RadarView
{
    float scale {1.0f};
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
        return oy + (tileY - viewY) * scale;
    }
};

RadarView radarView(const UiRect &radar, int mapW, int mapH, int zoom = 1, float centerX = 0.0f, float centerY = 0.0f)
{
    RadarView v;
    if (mapW <= 0 || mapH <= 0)
        return v;
    const float innerW = static_cast<float>(radar.w - 4);
    const float innerH = static_cast<float>(radar.h - 4);
    v.scale = std::min(innerW / mapW, innerH / mapH) * std::max(1, zoom);
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
UiRect DestroyBtn(const UiRect &info)
{
    return {info.Right() - 92, info.Bottom() - 26, 84, 20};
}

UiRect RepairBtn(const UiRect &info)
{
    return {info.Right() - 184, info.Bottom() - 26, 84, 20};
}

std::vector<const Building *> OwnSelectedBuildings(const EntityManager &entities, const std::vector<int> &selected)
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

// A true circle outline in screen pixels (64 segments)
void DrawCircleScreen(Renderer &renderer, float cx, float cy, float radius, SDL_Color color)
{
    constexpr int kSegments = 64;
    float px = cx + radius, py = cy;
    for (int i = 1; i <= kSegments; ++i)
    {
        const float a = 6.28318530718f * static_cast<float>(i) / kSegments;
        const float nx = cx + radius * std::cos(a), ny = cy + radius * std::sin(a);
        renderer.DrawLineScreen(
            static_cast<int>(std::lround(px)),
            static_cast<int>(std::lround(py)),
            static_cast<int>(std::lround(nx)),
            static_cast<int>(std::lround(ny)),
            color);
        px = nx;
        py = ny;
    }
}
// A filled disc, one horizontal span per pixel row
void FillCircleScreen(Renderer &renderer, float cx, float cy, float radius, SDL_Color color)
{
    const int r = static_cast<int>(std::ceil(radius));
    for (int dy = -r; dy <= r; ++dy)
    {
        const float span = std::sqrt(std::max(0.0f, radius * radius - static_cast<float>(dy * dy)));
        const int x0 = static_cast<int>(std::lround(cx - span)), x1 = static_cast<int>(std::lround(cx + span));
        renderer.FillRectScreen(x0, static_cast<int>(std::lround(cy)) + dy, std::max(1, x1 - x0), 1, color);
    }
}

// Zoom buttons in the radar's bottom-right corner
UiRect RadarZoomInBtn(const UiRect &radar)
{
    return {radar.Right() - 22, radar.Bottom() - 22, 20, 20};
}
UiRect RadarZoomOutBtn(const UiRect &radar)
{
    return {radar.Right() - 44, radar.Bottom() - 22, 20, 20};
}

SDL_Color RadarFactionColor(Faction f, bool unit);

// A player's own army colour on the radar when it has one, else the classic colour of its faction
SDL_Color RadarOwnerColor(PlayerId owner, Faction f, bool unit)
{
    if (owner < kMaxPlayers && Players::IsActive(owner) && Players::Slot(owner).color > 0)
    {
        SDL_Color c = ArmyColor(Players::Slot(owner).color);
        if (unit) // units a little lighter than buildings, as with the classic colours
            c
                = {static_cast<Uint8>(c.r + (255 - c.r) / 3),
                   static_cast<Uint8>(c.g + (255 - c.g) / 3),
                   static_cast<Uint8>(c.b + (255 - c.b) / 3),
                   255};
        return c;
    }
    return RadarFactionColor(f, unit);
}

SDL_Color RadarFactionColor(Faction f, bool unit)
{
    switch (f)
    {
        case Faction::Resistance:
            return unit ? SDL_Color {255, 160, 140, 255} : SDL_Color {236, 92, 76, 255};
        case Faction::Confederation:
            return unit ? SDL_Color {150, 215, 255, 255} : SDL_Color {72, 170, 236, 255};
        default:
            return unit ? SDL_Color {190, 230, 150, 255} : SDL_Color {120, 190, 90, 255};
    }
}

std::string UnitName(const UnitCameoDef &def, const EntityManager &entities)
{
    const auto *cfg = entities.GetUnitConfig(def.archetype);
    return (cfg && !cfg->name.empty()) ? cfg->name : std::string(def.name);
}

int UnitCost(const UnitCameoDef &def, const EntityManager &entities)
{
    const auto *cfg = entities.GetUnitConfig(def.archetype);
    return cfg ? cfg->creditCost : def.cost;
}

std::string UnitDesc(const UnitCameoDef &def, const EntityManager &entities)
{
    const auto *cfg = entities.GetUnitConfig(def.archetype);
    return (cfg && !cfg->description.empty()) ? cfg->description : std::string(def.desc);
}

// The infinity button in the bottom right corner of a unit cameo
UiRect InfinityBtn(const UiRect &cell)
{
    return {cell.Right() - 28, cell.Bottom() - 22, 24, 18};
}
void DrawInfinity(Renderer &r, const UiRect &b, bool on, bool hovered)
{
    r.FillRectScreen(
        b.x, b.y, b.w, b.h, on ? SDL_Color {60, 44, 20, 235} : SDL_Color {6, 8, 12, hovered ? Uint8(235) : Uint8(190)});
    r.DrawRectScreen(b.x, b.y, b.w, b.h, on ? col::Accent : (hovered ? col::BorderStrong : col::Border));
    // two rings side by side: the infinity sign
    const SDL_Color c = on ? col::Accent : col::TextDim;
    r.DrawRingScreen(static_cast<float>(b.x + 7), static_cast<float>(b.CenterY()), 4.0f, 3.5f, 1.6f, c, 24);
    r.DrawRingScreen(static_cast<float>(b.x + 17), static_cast<float>(b.CenterY()), 4.0f, 3.5f, 1.6f, c, 24);
}

void DrawCostTag(Renderer &r, BitmapFontRenderer &font, const UiRect &cell, const std::string &text, SDL_Color color)
{
    int tw = font.GetTextWidth(text, ui::FONT_SMALL_BOLD) + 10;
    UiRect tag {cell.Right() - tw - 3, cell.y + 3, tw, 18};
    r.FillRectScreen(tag.x, tag.y, tag.w, tag.h, {6, 8, 12, 215});
    font.DrawString(
        text, tag.CenterX(), tag.CenterY(), ui::FONT_SMALL_BOLD, color, false, Anchor::HCENTER | Anchor::VCENTER);
}

// Common cameo frame: icon, bottom name band, hover/selection border
void DrawCameo(
    Renderer &r,
    BitmapFontRenderer &font,
    const UiRect &cell,
    int compId,
    const std::string &name,
    bool hovered,
    bool selected,
    bool locked)
{
    r.FillRectScreen(cell.x, cell.y, cell.w, cell.h, col::PanelSunken);
    if (compId >= 0)
    {
        int iconX = cell.x + (cell.w - kCameoW * 2) / 2;
        int iconY = cell.y + 3;
        // The icons wear the player's army colour, like its buildings and units
        r.SetDrawOwner(Players::Local());
        r.SetStructureFaction(PlayerFaction());
        r.DrawCompositeScreen(compId, iconX, iconY, 2.0f, PlayerFaction());
        r.SetStructureFaction(Faction::Neutral);
        r.SetDrawOwner(kNoPlayer);
    }
    if (locked)
    {
        r.FillRectScreen(cell.x, cell.y, cell.w, cell.h, {10, 12, 16, 170});
    }
    UiRect band {cell.x, cell.Bottom() - 20, cell.w, 20};
    r.FillRectScreen(band.x, band.y, band.w, band.h, {6, 8, 12, 225});
    font.DrawString(
        ui::FitText(font, name, ui::FONT_SMALL_BOLD, band.w - 10),
        band.CenterX(),
        band.CenterY(),
        ui::FONT_SMALL_BOLD,
        locked ? col::TextMuted : (selected ? col::Accent : col::Text),
        false,
        Anchor::HCENTER | Anchor::VCENTER);

    SDL_Color border = selected ? col::Accent : (hovered ? col::BorderStrong : col::Border);
    r.DrawRectScreen(cell.x, cell.y, cell.w, cell.h, border);
    if (selected)
        r.DrawRectScreen(cell.x + 1, cell.y + 1, cell.w - 2, cell.h - 2, ui::WithAlpha(col::Accent, 120));
}

// Progress "shutter" that uncovers the cameo from the top while an item is being produced
void DrawProgressShutter(Renderer &r, BitmapFontRenderer &font, const UiRect &cell, float progress, SDL_Color tint)
{
    int coverH = static_cast<int>((cell.h - 20) * (1.0f - std::clamp(progress, 0.0f, 1.0f)));
    if (coverH > 0)
        r.FillRectScreen(cell.x + 1, cell.Bottom() - 20 - coverH, cell.w - 2, coverH, {6, 8, 12, 160});
    r.FillRectScreen(cell.x + 1, cell.Bottom() - 22, static_cast<int>((cell.w - 2) * progress), 2, tint);
    std::string pct = std::to_string(static_cast<int>(progress * 100.0f)) + "%";
    font.DrawString(
        pct,
        cell.CenterX(),
        cell.y + (cell.h - 20) / 2,
        ui::FONT_H2,
        ui::color::Text,
        true,
        Anchor::HCENTER | Anchor::VCENTER);
}

void DrawLockBadge(Renderer &r, BitmapFontRenderer &font, const UiRect &cell)
{
    font.DrawString(
        _("LOCK"),
        cell.CenterX(),
        cell.y + (cell.h - 20) / 2,
        ui::FONT_SMALL_BOLD,
        col::Danger,
        true,
        Anchor::HCENTER | Anchor::VCENTER);
    (void) r;
}

// Crosshair glyph used as the Orders tab icon
void DrawCrosshair(Renderer &r, int cx, int cy, int size, SDL_Color c)
{
    r.DrawRectScreen(cx - size / 2, cy - size / 2, size, size, c);
    r.FillRectScreen(cx - size / 2 - 4, cy, size + 9, 1, c);
    r.FillRectScreen(cx, cy - size / 2 - 4, 1, size + 9, c);
}

} // namespace

int UnitCameoCompId(int archetype)
{
    for (bool infantry : {true, false})
    {
        for (Faction f : {Faction::Resistance, Faction::Confederation})
        {
            const bool res = (f == Faction::Resistance);
            UnitCatalog cat
                = infantry
                      ? (res ? UnitCatalog {kResistanceInfantry, static_cast<int>(std::size(kResistanceInfantry))}
                             : UnitCatalog {kConfederationInfantry, static_cast<int>(std::size(kConfederationInfantry))})
                      : (res ? UnitCatalog {kResistanceVehicles, static_cast<int>(std::size(kResistanceVehicles))}
                             : UnitCatalog {
                                   kConfederationVehicles, static_cast<int>(std::size(kConfederationVehicles))});
            for (int i = 0; i < cat.count; ++i)
            {
                if (cat.items[i].archetype == archetype)
                    return cat.items[i].compId;
            }
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Simulation-side state
// ---------------------------------------------------------------------------

void HUDController::SetActiveTab(int tab)
{
    if (tab >= 0 && tab < TabCount)
        m_activeTab = tab;
}

void HUDController::Issue(SimCommand command, bool withSound)
{
    if (withSound)
    {
        if (command.type == CommandType::Produce || command.type == CommandType::StartResearch)
            SoundRequests::Play("production_start");
        else if (
            command.type == CommandType::Stop || command.type == CommandType::HoldPosition
            || command.type == CommandType::SetStance)
            SoundRequests::Play("@hold");
    }
    if (!m_sim)
        return;
    command.player = Players::Local();
    m_sim->Issue(std::move(command));
}

void HUDController::Update(
    float deltaTime, EntityManager &entities, Economy &economy, const std::vector<SimEvent> &events)
{
    (void) economy;
    m_destroyArmed = std::max(0.0f, m_destroyArmed - deltaTime);
    for (const SimEvent &e : events)
    {
        if (e.type == SimEvent::Type::ResearchCompleted && e.player == Players::Local())
            AnnounceResearch(e.value);
        else if (e.type == SimEvent::Type::PlayerEliminated)
            SetNotification(
                std::string(Players::Slot(e.player).human ? _("PLAYER ") : _("COMPUTER "))
                + std::to_string(e.player + 1) + " " + _("IS OUT: NO BUILDINGS LEFT"));
    }

    // Validate & auto-assign primary barracks and factory if needed
    auto barracksList = entities.GetBuildingsByType(UnitType::Barracks, Players::Local());
    if (m_primaryBarracksId != 0)
    {
        const Entity *b = entities.GetEntity(m_primaryBarracksId);
        if (!b || !b->alive || b->owner != Players::Local() || b->type != UnitType::Barracks)
        {
            m_primaryBarracksId = 0;
        }
    }
    if (m_primaryBarracksId == 0 && !barracksList.empty())
    {
        m_primaryBarracksId = barracksList[0];
    }

    auto factoryList = entities.GetBuildingsByType(UnitType::Factory, Players::Local());
    if (m_primaryFactoryId != 0)
    {
        const Entity *f = entities.GetEntity(m_primaryFactoryId);
        if (!f || !f->alive || f->owner != Players::Local() || f->type != UnitType::Factory)
        {
            m_primaryFactoryId = 0;
        }
    }
    if (m_primaryFactoryId == 0 && !factoryList.empty())
    {
        m_primaryFactoryId = factoryList[0];
    }

    if (m_hudNotificationTimer > 0.0f)
    {
        m_hudNotificationTimer -= deltaTime;
        if (m_hudNotificationTimer <= 0.0f)
        {
            m_hudNotification.clear();
        }
    }
}

void HUDController::AnnounceResearch(int upgradeIndex)
{
    SoundRequests::Announce("ann_research_complete");
    const ResearchDef &r = researchDef(PlayerFaction(), upgradeIndex);
    if (r.effect == ResearchEffect::FortressFlamethrower)
    {
        SetNotification(_("FORTRESS FLAMETHROWER UPGRADE UNLOCKED!"), 3.0f);
    }
    else if (r.effect == ResearchEffect::BuildRadius)
    {
        SetNotification(_("HQ BUILD RADIUS INCREASED (+25%)!"), 3.0f);
    }
    else
    {
        SetNotification(std::string(_("RESEARCH COMPLETE: ")) + _(r.name), 3.0f);
    }
}

void HUDController::FocusBuilding(int buildingId, const EntityManager &entities)
{
    const Building *b = entities.GetBuilding(buildingId);
    if (!b || b->owner != Players::Local())
        return;
    switch (b->type)
    {
        case UnitType::Barracks:
            m_primaryBarracksId = b->id;
            SetActiveTab(TabInfantry);
            break;
        case UnitType::Factory:
            m_primaryFactoryId = b->id;
            SetActiveTab(TabVehicles);
            break;
        case UnitType::ResearchLab:
            break;
        default:
            SetActiveTab(TabBuild);
            break;
    }
    m_buildMode = false;
}

void HUDController::ResetForMission()
{
    m_radarAlert.Reset();
    m_primaryBarracksId = 0;
    m_primaryFactoryId = 0;
    m_buildMode = false;
    m_attackMoveMode = false;
    CloseResearchWindow();
    m_hudNotification.clear();
    m_hudNotificationTimer = 0.0f;
}

std::vector<int> HUDController::ProductionTargets(
    UnitType buildingType, const EntityManager &entities, const std::vector<int> &selected) const
{
    auto usable = [&](int id) {
        const Building *b = entities.GetBuilding(id);
        return b && b->alive && b->state != ActionState::Die && b->owner == Players::Local() && b->type == buildingType
               && b->buildProgress >= 1.0f;
    };
    std::vector<int> targets;
    for (int id : selected)
    {
        if (usable(id))
            targets.push_back(id);
    }
    if (targets.empty())
    {
        const int primary = (buildingType == UnitType::Barracks) ? m_primaryBarracksId : m_primaryFactoryId;
        if (usable(primary))
            targets.push_back(primary);
    }
    return targets;
}

void HUDController::ToggleRepeat(
    int archetype, UnitType buildingType, EntityManager &entities, const std::vector<int> &selected)
{
    const std::vector<int> targets = ProductionTargets(buildingType, entities, selected);
    bool anyOn = false;
    for (int id : targets)
        anyOn = anyOn || entities.GetBuilding(id)->repeatArchetype == archetype;
    for (int id : targets)
    {
        // Off: only the producers that repeat it; on: every producer that does not yet
        if ((entities.GetBuilding(id)->repeatArchetype == archetype) != anyOn)
            continue;
        SimCommand c;
        c.type = CommandType::SetRepeat;
        c.a = archetype;
        c.b = id;
        Issue(std::move(c));
    }
    SoundRequests::Play("ui_click");
}

bool HUDController::OrderUnit(
    int archetype, UnitType buildingType, EntityManager &entities, Economy &economy, const std::vector<int> &selected)
{
    const std::vector<int> targets = ProductionTargets(buildingType, entities, selected);
    if (targets.empty())
        return false;

    const auto *cfg = entities.GetUnitConfig(archetype);
    const int cost = cfg ? cfg->creditCost : 100;
    if (!economy.CanAfford(cost))
    {
        SoundRequests::Announce("ann_insufficient_funds");
        SetNotification(_("INSUFFICIENT CREDITS!"));
        return false;
    }
    // Every selected building of the kind trains it at once: one click, one order each (as far as the credits go:
    // the simulation queues what they pay for). Shift: a batch of 5 each, Ctrl: a batch of 10 each
    const SDL_Keymod mod = SDL_GetModState();
    const int batch = (mod & SDL_KMOD_CTRL) ? 10 : ((mod & SDL_KMOD_SHIFT) ? 5 : 1);
    bool first = true;
    for (int id : targets)
    {
        SimCommand c;
        c.type = CommandType::Produce;
        c.a = archetype;
        c.b = id;
        c.c = batch;
        Issue(std::move(c), first);
        first = false;
    }
    return true;
}

bool HUDController::CancelUnitOrder(
    int archetype, UnitType buildingType, EntityManager &entities, Economy &economy, const std::vector<int> &selected)
{
    // Take the order back from the building that has most of that unit queued
    int best = 0;
    int bestCount = 0;
    for (int id : ProductionTargets(buildingType, entities, selected))
    {
        const Building *b = entities.GetBuilding(id);
        int n = static_cast<int>(
            std::count_if(b->productionQueue.begin(), b->productionQueue.end(), [&](const ProductionItem &it) {
                return it.archetype == archetype;
            }));
        if (n >= bestCount && n > 0)
        {
            bestCount = n;
            best = id;
        }
    }
    (void) economy;
    if (best <= 0)
        return false;
    SimCommand c;
    c.type = CommandType::CancelProduce;
    c.a = archetype;
    c.b = best;
    Issue(std::move(c));
    return true;
}

int HUDController::ItemCountForTab(int tab) const
{
    switch (tab)
    {
        case TabBuild:
            return std::min(8, static_cast<int>(EntityManager::GetBlueprints().size()));
        case TabInfantry:
            return unitCatalog(true).count;
        case TabVehicles:
            return unitCatalog(false).count;
        case TabOrders:
            return static_cast<int>(std::size(kOrders));
    }
    return 0;
}

int HUDController::MaxScrollRows(int tab, int gridHeight) const
{
    int rows = (ItemCountForTab(tab) + SidebarLayout::kCols - 1) / SidebarLayout::kCols;
    int visible = std::max(1, (gridHeight + SidebarLayout::kGap) / (SidebarLayout::kCellH + SidebarLayout::kGap));
    return std::max(0, rows - visible);
}

void HUDController::ActivateCell(
    int tab, int index, bool cancel, Economy &economy, EntityManager &entities, std::vector<int> &selectedUnits)
{
    if (cancel)
    {
        // Right click on a unit card takes one order back (with a refund)
        if (tab == TabInfantry || tab == TabVehicles)
        {
            const bool inf = (tab == TabInfantry);
            CancelUnitOrder(
                unitCatalog(inf).items[index].archetype,
                inf ? UnitType::Barracks : UnitType::Factory,
                entities,
                economy,
                selectedUnits);
        }
        return;
    }
    switch (tab)
    {
        case TabBuild:
        {
            const auto &blueprints = EntityManager::GetBlueprints();
            if (!entities.HasBuildingType(UnitType::Headquarters, Players::Local(), true))
            {
                SoundRequests::Play("ui_error");
                SetNotification(_("HEADQUARTERS REQUIRED TO BUILD!"));
            }
            else if (
                index < static_cast<int>(blueprints.size()) && blueprints[index].buildingIndex == 6
                && !IsRadarUnlocked(Players::Local()))
            {
                SoundRequests::Play("ui_error");
                SetNotification(_("RESEARCH REQUIRED TO BUILD RADAR LOCATORS!"));
            }
            else if (index < static_cast<int>(blueprints.size()))
            {
                m_selectedBlueprint = index;
                m_buildMode = true;
                m_attackMoveMode = false;
            }
            break;
        }
        case TabInfantry:
            if (!IsUnitUnlocked(Players::Local(), unitCatalog(true).items[index].archetype))
            {
                SoundRequests::Play("ui_error");
                SetNotification(_("RESEARCH REQUIRED TO TRAIN THIS UNIT!"));
            }
            else if (!entities.HasBuildingType(UnitType::Barracks, Players::Local()))
            {
                SoundRequests::Play("ui_error");
                SetNotification(_("BARRACKS REQUIRED FOR INFANTRY RECRUITMENT!"));
            }
            else
            {
                OrderUnit(
                    unitCatalog(true).items[index].archetype, UnitType::Barracks, entities, economy, selectedUnits);
            }
            break;
        case TabVehicles:
            if (!IsUnitUnlocked(Players::Local(), unitCatalog(false).items[index].archetype))
            {
                SoundRequests::Play("ui_error");
                SetNotification(_("RESEARCH REQUIRED TO BUILD THIS UNIT!"));
            }
            else if (!entities.HasBuildingType(UnitType::Factory, Players::Local()))
            {
                SoundRequests::Play("ui_error");
                SetNotification(_("VEHICLE FACTORY REQUIRED FOR PRODUCTION!"));
            }
            else
            {
                OrderUnit(
                    unitCatalog(false).items[index].archetype, UnitType::Factory, entities, economy, selectedUnits);
            }
            break;
        case TabOrders:
            switch (index)
            {
                case 0:
                { // Attack-move
                    bool hasCombat = false;
                    for (int uid : selectedUnits)
                    {
                        const Entity *e = entities.GetEntity(uid);
                        if (e && e->alive && !e->isBuilding)
                        {
                            hasCombat = true;
                            break;
                        }
                    }
                    if (hasCombat)
                    {
                        m_attackMoveMode = !m_attackMoveMode;
                        m_buildMode = false;
                    }
                    break;
                }
                case 1: // Siege / transform
                case 2: // Stop
                case 3:
                { // Defend: hold position
                    static const CommandType kTypes[]
                        = {CommandType::ToggleSiege, CommandType::Stop, CommandType::HoldPosition};
                    SimCommand c;
                    c.type = kTypes[index - 1];
                    c.ids.assign(selectedUnits.begin(), selectedUnits.end());
                    Issue(std::move(c));
                    break;
                }
                case 4: // Stances
                case 5:
                case 6:
                {
                    SimCommand c;
                    c.type = CommandType::SetStance;
                    c.a = index - kFirstStanceOrder;
                    c.ids.assign(selectedUnits.begin(), selectedUnits.end());
                    Issue(std::move(c));
                    break;
                }
            }
            break;
    }
}

void HUDController::CyclePrimaryBuilding(int tab, EntityManager &entities, Camera &camera)
{
    bool infantry = (tab == TabInfantry);
    auto list = entities.GetBuildingsByType(infantry ? UnitType::Barracks : UnitType::Factory, Players::Local());
    if (list.empty())
        return;
    int &primary = infantry ? m_primaryBarracksId : m_primaryFactoryId;
    auto it = std::find(list.begin(), list.end(), primary);
    primary = (it == list.end() || std::next(it) == list.end()) ? list.front() : *std::next(it);
    if (const Entity *ent = entities.GetEntity(primary))
    {
        camera.CenterOn(ent->tileX * TILE_WIDTH, ent->tileY * TILE_HEIGHT);
    }
}

void HUDController::UpdateRadarDrag(
    int mx,
    int my,
    bool leftMouseDown,
    float frameDeltaTime,
    int windowW,
    int windowH,
    const TileMap &map,
    Camera &camera)
{
    if (!leftMouseDown)
        m_radarDragging = false;
    if (!m_radarDragging)
        return;
    const SidebarLayout l = sidebarLayout(windowW, windowH, m_topInset);
    const RadarView v
        = radarView(l.radar, map.GetWidth(), map.GetHeight(), m_radarZoom, m_radarCenterX, m_radarCenterY);
    if (v.w <= 0 || v.h <= 0)
        return;
    // Outside the radar the point sticks to its edge
    const float px = std::clamp(static_cast<float>(mx), static_cast<float>(v.ox), static_cast<float>(v.ox + v.w));
    const float py = std::clamp(static_cast<float>(my), static_cast<float>(v.oy), static_cast<float>(v.oy + v.h));
    const float targetX = (v.viewX + (px - v.ox) / v.scale) * TILE_WIDTH;
    const float targetY = (v.viewY + (py - v.oy) / v.scale) * TILE_HEIGHT;
    // One radar pixel is more than a tile: easing instead of following the pixel steps keeps it fluid,
    // and the exponential form gives the same feel at any frame rate
    const float halfW = camera.GetViewportWidth() * 0.5f / camera.GetZoom();
    const float halfH = camera.GetViewportHeight() * 0.5f / camera.GetZoom();
    const float curX = camera.GetX() + halfW, curY = camera.GetY() + halfH;
    const float k = 1.0f - std::exp(-frameDeltaTime * kRadarFollowRate);
    camera.CenterOn(curX + (targetX - curX) * k, curY + (targetY - curY) * k);
}

bool HUDController::HandleHudClick(
    int mx,
    int my,
    bool leftClicked,
    bool rightClicked,
    bool leftMouseDown,
    int wheelSteps,
    int windowW,
    int windowH,
    Economy &economy,
    EntityManager &entities,
    TileMap &map,
    Camera &camera,
    bool &outOpenMenu,
    bool &outOpenObjectives,
    std::vector<int> &selectedUnits)
{
    if (!leftMouseDown)
        m_radarDragging = false;
    // A radar drag owns the mouse until the button is released, also over the map (no box selection)
    if (m_radarDragging)
        return true;
    if (!IsOverSidebar(mx, windowW))
        return false;

    SidebarLayout l = sidebarLayout(windowW, windowH, m_topInset);

    // Radar: wheel and the +/- buttons zoom, click or drag moves the camera
    if (l.radar.Contains(mx, my))
    {
        if (wheelSteps != 0)
        {
            m_radarZoom = std::clamp(m_radarZoom + (wheelSteps > 0 ? 1 : -1), 1, kRadarMaxZoom);
        }
        if (leftClicked && !m_radarDragging)
        {
            if (RadarZoomInBtn(l.radar).Contains(mx, my))
            {
                m_radarZoom = std::min(kRadarMaxZoom, m_radarZoom + 1);
                return true;
            }
            if (RadarZoomOutBtn(l.radar).Contains(mx, my))
            {
                m_radarZoom = std::max(1, m_radarZoom - 1);
                return true;
            }
        }
        const bool onButtons = RadarZoomInBtn(l.radar).Contains(mx, my) || RadarZoomOutBtn(l.radar).Contains(mx, my);
        if ((leftMouseDown || leftClicked) && !onButtons)
        {
            // The click jumps there at once; holding the button goes on in updateRadarDrag, every frame
            RadarView v
                = radarView(l.radar, map.GetWidth(), map.GetHeight(), m_radarZoom, m_radarCenterX, m_radarCenterY);
            if (v.w > 0 && v.h > 0)
            {
                m_radarDragging = leftMouseDown;
                float tx = std::clamp(
                    v.viewX + static_cast<float>(mx - v.ox) / v.scale, 0.0f, static_cast<float>(map.GetWidth()));
                float ty = std::clamp(
                    v.viewY + static_cast<float>(my - v.oy) / v.scale, 0.0f, static_cast<float>(map.GetHeight()));
                camera.CenterOn(tx * TILE_WIDTH, ty * TILE_HEIGHT);
            }
        }
        return true;
    }

    int maxScroll = MaxScrollRows(m_activeTab, l.grid.h);
    m_scrollRow[m_activeTab] = std::clamp(m_scrollRow[m_activeTab] - wheelSteps, 0, maxScroll);

    if (!m_observer && rightClicked && l.grid.Contains(mx, my))
    {
        int count = ItemCountForTab(m_activeTab);
        for (int i = 0; i < count; ++i)
        {
            UiRect c = l.Cell(i, m_scrollRow[m_activeTab]);
            if (c.Contains(mx, my))
            {
                ActivateCell(m_activeTab, i, true, economy, entities, selectedUnits);
                break;
            }
        }
        return true;
    }
    if (!leftClicked)
        return true;
    if (m_observer && !l.menuBtn.Contains(mx, my) && !l.objectivesBtn.Contains(mx, my))
        return true;

    // Repair on / off for the selected own structures (paid as it goes)
    if (RepairBtn(l.info).Contains(mx, my))
    {
        const auto own = OwnSelectedBuildings(entities, selectedUnits);
        if (own.empty())
            return true;
        SimCommand c;
        c.type = CommandType::ToggleRepair;
        for (const Building *b : own)
            c.ids.push_back(static_cast<uint32_t>(b->id));
        Issue(std::move(c));
        return true;
    }

    // Destroy the selected own structures: the first click arms the button, the second one confirms
    if (DestroyBtn(l.info).Contains(mx, my))
    {
        const auto own = OwnSelectedBuildings(entities, selectedUnits);
        if (own.empty())
            return true;
        if (m_destroyArmed <= 0.0f)
        {
            m_destroyArmed = 3.0f;
            return true;
        }
        m_destroyArmed = 0.0f;
        SimCommand c;
        c.type = CommandType::Demolish;
        for (const Building *b : own)
            c.ids.push_back(static_cast<uint32_t>(b->id));
        // They are gone on the tick the command runs: out of the selection already
        selectedUnits.erase(
            std::remove_if(
                selectedUnits.begin(),
                selectedUnits.end(),
                [&](int id) {
                    return std::find(c.ids.begin(), c.ids.end(), static_cast<uint32_t>(id)) != c.ids.end();
                }),
            selectedUnits.end());
        Issue(std::move(c));
        return true;
    }

    if (l.menuBtn.Contains(mx, my))
    {
        outOpenMenu = true;
        return true;
    }
    if (l.objectivesBtn.Contains(mx, my))
    {
        outOpenObjectives = true;
        return true;
    }
    if (l.researchBtn.Contains(mx, my))
    {
        OpenResearchWindow();
        return true;
    }
    for (int t = 0; t < TabCount; ++t)
    {
        if (l.tabs[t].Contains(mx, my))
        {
            m_activeTab = t;
            return true;
        }
    }
    if (maxScroll > 0 && l.scrollUp.Contains(mx, my))
    {
        m_scrollRow[m_activeTab] = std::max(0, m_scrollRow[m_activeTab] - 1);
        return true;
    }
    if (maxScroll > 0 && l.scrollDown.Contains(mx, my))
    {
        m_scrollRow[m_activeTab] = std::min(maxScroll, m_scrollRow[m_activeTab] + 1);
        return true;
    }
    if ((m_activeTab == TabInfantry || m_activeTab == TabVehicles) && l.primaryBtn.Contains(mx, my))
    {
        CyclePrimaryBuilding(m_activeTab, entities, camera);
        return true;
    }
    if (l.grid.Contains(mx, my))
    {
        int count = ItemCountForTab(m_activeTab);
        for (int i = 0; i < count; ++i)
        {
            UiRect c = l.Cell(i, m_scrollRow[m_activeTab]);
            if (c.Contains(mx, my) && c.y >= l.grid.y && c.Bottom() <= l.grid.Bottom() + SidebarLayout::kGap)
            {
                if ((m_activeTab == TabInfantry || m_activeTab == TabVehicles) && InfinityBtn(c).Contains(mx, my))
                {
                    const bool inf = m_activeTab == TabInfantry;
                    const int archetype = unitCatalog(inf).items[i].archetype;
                    if (IsUnitUnlocked(Players::Local(), archetype))
                        ToggleRepeat(archetype, inf ? UnitType::Barracks : UnitType::Factory, entities, selectedUnits);
                    break;
                }
                ActivateCell(m_activeTab, i, false, economy, entities, selectedUnits);
                break;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void HUDController::RenderGameHud(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const Economy &economy,
    const EntityManager &entities,
    const TileMap &map,
    const Camera &camera,
    int mx,
    int my,
    int windowW,
    int windowH,
    const std::vector<int> &selected)
{
    SidebarLayout l = sidebarLayout(windowW, windowH, m_topInset);

    // Frame
    ui::FillVerticalGradient(renderer, l.panel, {20, 24, 32, 255}, {12, 15, 20, 255});
    renderer.FillRectScreen(l.panel.x, 0, 1, windowH, col::BorderStrong);
    renderer.FillRectScreen(l.panel.x + 1, 0, 1, windowH, {0, 0, 0, 120});

    // --- Header: credits & power ---------------------------------------------------------
    {
        const UiRect &h = l.header;
        std::string credits = "$ " + std::to_string(economy.GetCredits());
        fontRenderer.DrawString(credits, h.x, h.y - 2, ui::FONT_H2, col::Accent, true);

        int prod = economy.GetPowerProduced();
        int cons = economy.GetPowerConsumed();
        bool deficit = economy.HasPowerDeficit();
        SDL_Color pCol = deficit ? col::Danger : col::Ok;
        std::string pwr = std::to_string(cons) + " / " + std::to_string(prod) + _(" kW");
        fontRenderer.DrawString(pwr, h.Right(), h.y + 2, ui::FONT_SMALL_BOLD, pCol, false, Anchor::RIGHT | Anchor::TOP);
        // The lightning of the game next to the kW figures; it blinks while the power is short
        if (!deficit || PowerIconBlinkOn())
        {
            const int textW = fontRenderer.GetTextWidth(pwr, ui::FONT_SMALL_BOLD);
            renderer.DrawRawSpriteScreen(kPowerIconSprite, h.Right() - textW - 27, h.y, 1.0f);
        }
        fontRenderer.DrawString(
            _("POWER"), h.Right(), h.y + 17, ui::FONT_SMALL, col::TextMuted, false, Anchor::RIGHT | Anchor::TOP);

        float ratio = static_cast<float>(cons) / static_cast<float>(std::max(1, prod));
        ui::DrawProgressBar(renderer, {h.x, h.Bottom() - 6, h.w, 6}, ratio, pCol);
    }

    // --- Radar -----------------------------------------------------------------------------
    {
        const UiRect &rd = l.radar;
        renderer.FillRectScreen(rd.x, rd.y, rd.w, rd.h, {6, 12, 10, 255});
        const int mapW = map.GetWidth();
        const int mapH = map.GetHeight();
        // A zoomed radar follows the camera, except while the player drags on it (that moves the camera)
        const float camCenterX = (camera.GetX() + camera.GetViewportWidth() * 0.5f / camera.GetZoom()) / TILE_WIDTH;
        const float camCenterY = (camera.GetY() + camera.GetViewportHeight() * 0.5f / camera.GetZoom()) / TILE_HEIGHT;
        if (!m_radarDragging || m_radarCenterX < 0.0f)
        {
            m_radarCenterX = camCenterX;
            m_radarCenterY = camCenterY;
        }
        RadarView v = radarView(rd, mapW, mapH, m_radarZoom, m_radarCenterX, m_radarCenterY);
        if (v.w > 0 && v.h > 0)
        {
            renderer.SetClipRect({v.ox, v.oy, v.w, v.h});
            renderer.FillRectScreen(v.ox, v.oy, v.w, v.h, {14, 30, 22, 255});
            const bool fog = m_radarFog && map.IsFogEnabled() && !m_showAll;
            const int tx0 = std::max(0, static_cast<int>(v.viewX));
            const int ty0 = std::max(0, static_cast<int>(v.viewY));
            const int tx1 = std::min(mapW, static_cast<int>(std::ceil(v.viewX + v.w / v.scale)) + 1);
            const int ty1 = std::min(mapH, static_cast<int>(std::ceil(v.viewY + v.h / v.scale)) + 1);
            // Cells from rounded edges: neighbours meet exactly, nothing shimmers as the view moves
            auto cellRect = [&](float x, float y, float w, float h, int &px, int &py, int &pw, int &ph) {
                px = static_cast<int>(std::lround(v.ToScreenX(x)));
                py = static_cast<int>(std::lround(v.ToScreenY(y)));
                pw = std::max(1, static_cast<int>(std::lround(v.ToScreenX(x + w))) - px);
                ph = std::max(1, static_cast<int>(std::lround(v.ToScreenY(y + h))) - py);
            };
            const SDL_Color hiddenCol {4, 6, 8, 255};
            for (int ty = ty0; ty < ty1; ++ty)
            {
                for (int tx = tx0; tx < tx1; ++tx)
                {
                    int px, py, pw, ph;
                    cellRect(static_cast<float>(tx), static_cast<float>(ty), 1.0f, 1.0f, px, py, pw, ph);
                    const FogState fs = fog ? map.GetFogState(tx, ty) : FogState::Visible;
                    if (fs == FogState::Hidden)
                    {
                        renderer.FillRectScreen(px, py, pw, ph, hiddenCol);
                        continue;
                    }
                    // Walls as the local player knows them: under the fog of war the ones seen there last
                    const uint16_t wall = fog ? map.ViewerWall(tx, ty) : map.GetWall(tx, ty);
                    if (wall > 0)
                    {
                        const PlayerId owner = fog ? map.ViewerWallOwner(tx, ty) : map.GetWallOwner(tx, ty);
                        renderer.FillRectScreen(
                            px,
                            py,
                            pw,
                            ph,
                            RadarOwnerColor(owner, wall >= 1101 ? Faction::Resistance : Faction::Confederation, false));
                    }
                    else if (map.GetWall(tx, ty) == 0 && !map.IsPassable(tx, ty)) // an unseen wall is no terrain
                    {
                        renderer.FillRectScreen(px, py, pw, ph, {30, 54, 40, 255});
                    }
                }
            }

            // First enemy drawn on the radar this frame: the radar alarm's contact point
            bool enemyOnRadar = false;
            float contactX = 0.0f, contactY = 0.0f;
            for (const auto &e : entities.GetAllEntities())
            {
                if (!e.alive || e.state == ActionState::Die)
                    continue;
                const bool own = (e.owner == Players::Local());
                const Building *b = e.AsBuilding();
                if (fog && !own)
                {
                    if (b)
                    {
                        // Enemy structures appear once they have been seen (never while they are being built out of
                        // sight)
                        if (!entities.IsHostileBuildingShown(*b, map))
                            continue;
                    }
                    else if (!entities.IsEntityVisibleToPlayer(e, Players::Local(), map))
                    {
                        continue; // units only while in sight
                    }
                }
                int px, py, pw, ph;
                if (b)
                {
                    cellRect(
                        static_cast<float>(b->tileX),
                        static_cast<float>(b->tileY),
                        static_cast<float>(b->buildWidth),
                        static_cast<float>(b->buildHeight),
                        px,
                        py,
                        pw,
                        ph);
                    renderer.FillRectScreen(
                        px, py, std::max(3, pw), std::max(3, ph), RadarOwnerColor(e.owner, e.faction, false));
                }
                else
                {
                    cellRect(
                        e.GetUnitCenterPixelX() / TILE_WIDTH - 0.5f,
                        e.GetUnitCenterPixelY() / TILE_HEIGHT - 0.5f,
                        1.0f,
                        1.0f,
                        px,
                        py,
                        pw,
                        ph);
                    const int s = std::max(2, std::min(pw, ph));
                    renderer.FillRectScreen(
                        px + (pw - s) / 2, py + (ph - s) / 2, s, s, RadarOwnerColor(e.owner, e.faction, true));
                }
                if (!enemyOnRadar && !own && e.faction != Faction::Neutral
                    && Players::Hostile(e.owner, Players::Local()) && (!b || m_radarAlert.config.includeBuildings))
                {
                    enemyOnRadar = true;
                    contactX = e.GetUnitCenterPixelX() / TILE_WIDTH;
                    contactY = e.GetUnitCenterPixelY() / TILE_HEIGHT;
                    if (b)
                    {
                        contactX = b->tileX + b->buildWidth * 0.5f;
                        contactY = b->tileY + b->buildHeight * 0.5f;
                    }
                }
            }

            // Radar alarm: red waves and a flashing dot at the contact (true circles in screen pixels)
            const uint64_t nowMs = SDL_GetTicks();
            m_radarAlert.Update(nowMs, enemyOnRadar, contactX, contactY);
            if (m_radarAlert.Active())
            {
                const RadarAlertConfig &ac = m_radarAlert.config;
                const float cx = v.ToScreenX(m_radarAlert.TileX());
                const float cy = v.ToScreenY(m_radarAlert.TileY());
                for (int i = 0; i < ac.waveCount; ++i)
                {
                    float radius = 0.0f, opacity = 0.0f;
                    if (!m_radarAlert.Wave(i, nowMs, radius, opacity))
                        continue;
                    SDL_Color wc = ac.color;
                    wc.a = static_cast<Uint8>(ac.color.a * opacity);
                    for (int k = 0; k < ac.waveThicknessPx; ++k)
                        DrawCircleScreen(renderer, cx, cy, radius + static_cast<float>(k), wc);
                }
                if (m_radarAlert.BlinkOn(nowMs) && ac.dotRadiusPx > 0.0f)
                    FillCircleScreen(renderer, cx, cy, ac.dotRadiusPx, ac.color);
            }

            // Camera frame from rounded edges (truncating position and size separately made it wobble)
            const float camWorldW = camera.GetViewportWidth() / camera.GetZoom();
            const float camWorldH = camera.GetViewportHeight() / camera.GetZoom();
            int fx, fy, fw, fh;
            cellRect(
                camera.GetX() / TILE_WIDTH,
                camera.GetY() / TILE_HEIGHT,
                camWorldW / TILE_WIDTH,
                camWorldH / TILE_HEIGHT,
                fx,
                fy,
                fw,
                fh);
            renderer.DrawRectScreen(fx, fy, std::max(6, fw), std::max(4, fh), {255, 255, 255, 220});
            renderer.ResetClipRect();
        }
        renderer.DrawRectScreen(rd.x, rd.y, rd.w, rd.h, col::BorderStrong);

        // Radar alarm: the radar's sides flash red, the band fading inward
        if (m_radarAlert.BlinkOn(SDL_GetTicks()))
        {
            const RadarAlertConfig &ac = m_radarAlert.config;
            for (int k = 0; k < ac.edgeWidthPx; ++k)
            {
                SDL_Color ec = ac.color;
                ec.a = static_cast<Uint8>(ac.edgeAlpha * (ac.edgeWidthPx - k) / ac.edgeWidthPx);
                renderer.DrawRectScreen(rd.x + k, rd.y + k, rd.w - 2 * k, rd.h - 2 * k, ec);
            }
        }

        // Zoom buttons
        const UiRect zoomBtns[2] = {RadarZoomOutBtn(rd), RadarZoomInBtn(rd)};
        for (int k = 0; k < 2; ++k)
        {
            const UiRect &b = zoomBtns[k];
            const bool enabled = (k == 0) ? m_radarZoom > 1 : m_radarZoom < kRadarMaxZoom;
            const bool hot = enabled && b.Contains(mx, my);
            if (hot)
                SoundRequests::MarkHover();
            renderer.FillRectScreen(b.x, b.y, b.w, b.h, hot ? col::Hover : SDL_Color {10, 16, 14, 210});
            renderer.DrawRectScreen(b.x, b.y, b.w, b.h, enabled ? col::BorderStrong : col::Border);
            const SDL_Color sc = enabled ? col::Text : col::TextMuted;
            renderer.FillRectScreen(b.x + 5, b.CenterY() - 1, b.w - 10, 2, sc);
            if (k == 1)
                renderer.FillRectScreen(b.CenterX() - 1, b.y + 5, 2, b.h - 10, sc);
        }
        if (m_radarZoom > 1)
        {
            fontRenderer.DrawString(
                "x" + std::to_string(m_radarZoom),
                rd.x + 6,
                rd.Bottom() - 12,
                ui::FONT_SMALL_BOLD,
                col::TextDim,
                false,
                Anchor::LEFT | Anchor::VCENTER);
        }
    }

    // --- Menu / objectives -------------------------------------------------------------------
    ui::DrawButton(
        renderer, fontRenderer, l.menuBtn, _("MENU"), ui::ButtonStyle::Secondary, l.menuBtn.Contains(mx, my));
    ui::DrawButton(
        renderer,
        fontRenderer,
        l.objectivesBtn,
        _("OBJECTIVES"),
        ui::ButtonStyle::Secondary,
        l.objectivesBtn.Contains(mx, my));

    // A replay's observer has no buildings to command: only the radar, the header and the menu stay
    if (!m_observer)
    {
        ui::DrawButton(
            renderer,
            fontRenderer,
            l.researchBtn,
            ui::FitText(fontRenderer, _("RESEARCH NEW TECHNOLOGIES"), ui::FONT_SMALL, l.researchBtn.w - 12),
            ui::ButtonStyle::Secondary,
            l.researchBtn.Contains(mx, my));
        // --- Tabs ----------------------------------------------------------------------------------
        for (int t = 0; t < TabCount; ++t)
        {
            const UiRect &tr = l.tabs[t];
            bool active = (t == m_activeTab);
            bool hovered = tr.Contains(mx, my);
            if (hovered)
                SoundRequests::MarkHover();
            renderer.FillRectScreen(
                tr.x, tr.y, tr.w, tr.h, active ? col::Hover : (hovered ? col::PanelRaised : col::PanelSunken));
            renderer.DrawRectScreen(tr.x, tr.y, tr.w, tr.h, active ? col::BorderStrong : col::Border);
            if (active)
                renderer.FillRectScreen(tr.x, tr.Bottom() - 3, tr.w, 3, col::Accent);

            if (TabIcon(t) >= 0)
            {
                renderer.SetClipRect({tr.x + 1, tr.y + 1, tr.w - 2, tr.h - 4});
                float s = 0.75f;
                int iw = static_cast<int>(kCameoW * s);
                renderer.SetDrawOwner(Players::Local());
                renderer.SetStructureFaction(PlayerFaction());
                renderer.DrawCompositeScreen(TabIcon(t), tr.CenterX() - iw / 2, tr.y + 6, s, PlayerFaction());
                renderer.SetStructureFaction(Faction::Neutral);
                renderer.SetDrawOwner(kNoPlayer);
                renderer.ResetClipRect();
                if (!active)
                    renderer.FillRectScreen(
                        tr.x + 1, tr.y + 1, tr.w - 2, tr.h - 4, {10, 12, 16, hovered ? Uint8(60) : Uint8(110)});
            }
            else
            {
                DrawCrosshair(renderer, tr.CenterX(), tr.CenterY() - 1, 16, active ? col::Accent : col::TextDim);
            }
        }

        // --- Strip: tab title, primary building, scroll arrows ---------------------------------------
        int maxScroll = MaxScrollRows(m_activeTab, l.grid.h);
        int &scroll = m_scrollRow[m_activeTab];
        scroll = std::clamp(scroll, 0, maxScroll);
        {
            ui::DrawCaption(fontRenderer, renderer, l.strip.x, l.strip.y + 4, _(kTabNames[m_activeTab]));

            if (m_activeTab == TabInfantry || m_activeTab == TabVehicles)
            {
                bool inf = (m_activeTab == TabInfantry);
                int primary = inf ? m_primaryBarracksId : m_primaryFactoryId;
                auto list = entities.GetBuildingsByType(inf ? UnitType::Barracks : UnitType::Factory, Players::Local());
                if (!list.empty())
                {
                    std::string label
                        = std::string(inf ? _("Barracks") : _("Factory")) + " #" + std::to_string(primary);
                    if (list.size() > 1)
                        label += "  \xE2\x80\xBA";
                    bool hot = list.size() > 1 && l.primaryBtn.Contains(mx, my);
                    fontRenderer.DrawString(
                        ui::FitText(fontRenderer, label, ui::FONT_SMALL, l.primaryBtn.w),
                        l.primaryBtn.Right(),
                        l.primaryBtn.CenterY(),
                        ui::FONT_SMALL,
                        hot ? col::Accent : col::TextDim,
                        false,
                        Anchor::RIGHT | Anchor::VCENTER);
                }
            }
            else if (m_activeTab == TabOrders)
            {
                int selected = 0;
                for (const auto &e : entities.GetAllEntities())
                {
                    if (e.alive && e.selected && !e.isBuilding)
                        ++selected;
                }
                std::string label = std::string(_("Selected")) + ": " + std::to_string(selected);
                fontRenderer.DrawString(
                    label,
                    l.primaryBtn.Right(),
                    l.primaryBtn.CenterY(),
                    ui::FONT_SMALL,
                    col::TextDim,
                    false,
                    Anchor::RIGHT | Anchor::VCENTER);
            }

            if (maxScroll > 0)
            {
                auto arrow = [&](const UiRect &r, bool up, bool enabled) {
                    bool hot = enabled && r.Contains(mx, my);
                    renderer.FillRectScreen(r.x, r.y, r.w, r.h, hot ? col::Hover : col::PanelRaised);
                    renderer.DrawRectScreen(r.x, r.y, r.w, r.h, hot ? col::BorderStrong : col::Border);
                    SDL_Color c = enabled ? (hot ? col::Accent : col::Text) : col::TextMuted;
                    for (int i = 0; i < 5; ++i)
                    {
                        int yy = up ? (r.CenterY() - 2 + i) : (r.CenterY() + 2 - i);
                        renderer.FillRectScreen(r.CenterX() - i, yy, 2 * i + 1, 1, c);
                    }
                };
                arrow(l.scrollUp, true, scroll > 0);
                arrow(l.scrollDown, false, scroll < maxScroll);
            }
        }

        // --- Cameo grid -----------------------------------------------------------------------------------
        std::string hoverTitle;
        std::string hoverText;
        bool hasBarracks = entities.HasBuildingType(UnitType::Barracks, Players::Local());
        bool hasFactory = entities.HasBuildingType(UnitType::Factory, Players::Local());
        bool hasLab = HasResearchBuilding(Players::Local(), entities);
        const bool noHq
            = !entities.HasBuildingType(UnitType::Headquarters, Players::Local(), true); // nothing can be built
        // Orders shown on unit cards come from the buildings that would train them
        auto queuedAt = [&](UnitType buildingType) {
            std::vector<const Building *> out;
            for (int id : ProductionTargets(buildingType, entities, selected))
                out.push_back(entities.GetBuilding(id));
            return out;
        };
        const std::vector<const Building *> barracksTargets = queuedAt(UnitType::Barracks);
        const std::vector<const Building *> factoryTargets = queuedAt(UnitType::Factory);

        renderer.SetClipRect({l.grid.x, l.grid.y, l.grid.w, l.grid.h});
        int count = ItemCountForTab(m_activeTab);
        for (int i = 0; i < count; ++i)
        {
            UiRect c = l.Cell(i, scroll);
            if (c.Bottom() < l.grid.y || c.y > l.grid.Bottom())
                continue;
            bool hovered = c.Contains(mx, my) && l.grid.Contains(mx, my);
            if (hovered)
                SoundRequests::MarkHover();

            switch (m_activeTab)
            {
                case TabBuild:
                {
                    const auto &bp = EntityManager::GetBlueprints()[i];
                    bool selected = (m_buildMode && m_selectedBlueprint == i);
                    bool canAfford = economy.CanAfford(bp.creditCost);
                    const bool radarLocked = (bp.buildingIndex == 6 && !IsRadarUnlocked(Players::Local())) || noHq;
                    DrawCameo(
                        renderer, fontRenderer, c, bp.iconCompId, _(bp.name), hovered, selected && !noHq, radarLocked);
                    DrawCostTag(
                        renderer,
                        fontRenderer,
                        c,
                        "$" + std::to_string(bp.creditCost),
                        canAfford ? col::Accent : col::Danger);
                    std::string pw = (bp.powerCost >= 0 ? "+" : "") + std::to_string(bp.powerCost);
                    fontRenderer.DrawString(
                        pw,
                        c.x + 6,
                        c.y + 5,
                        ui::FONT_SMALL_BOLD,
                        bp.powerCost >= 0 ? col::Ok : SDL_Color {255, 170, 80, 255},
                        true);
                    if (hovered)
                    {
                        hoverTitle
                            = std::string(_(bp.name)) + "  $" + std::to_string(bp.creditCost) + ", " + pw + _(" kW");
                        hoverText = _(bp.desc);
                    }
                    break;
                }
                case TabInfantry:
                case TabVehicles:
                {
                    bool inf = (m_activeTab == TabInfantry);
                    const UnitCameoDef &def = unitCatalog(inf).items[i];
                    const auto &producers = inf ? barracksTargets : factoryTargets;
                    const bool unlocked = IsUnitUnlocked(Players::Local(), def.archetype);
                    bool available = (inf ? hasBarracks : hasFactory) && unlocked;
                    int cost = UnitCost(def, entities);
                    std::string name = _(UnitName(def, entities));
                    DrawCameo(renderer, fontRenderer, c, def.compId, name, hovered, false, !available);

                    int queued = 0;
                    float progress = -1.0f; // most advanced order of this unit currently in training
                    for (const Building *b : producers)
                    {
                        for (const auto &q : b->productionQueue)
                        {
                            if (q.archetype == def.archetype)
                                ++queued;
                        }
                        if (!b->productionQueue.empty() && b->productionQueue.front().archetype == def.archetype)
                        {
                            progress = std::max(progress, b->CurrentProductionProgress());
                        }
                    }
                    if (!available)
                    {
                        DrawLockBadge(renderer, fontRenderer, c);
                    }
                    else
                    {
                        DrawCostTag(
                            renderer,
                            fontRenderer,
                            c,
                            "$" + std::to_string(cost),
                            economy.CanAfford(cost) ? col::Accent : col::Danger);
                        if (progress >= 0.0f)
                        {
                            DrawProgressShutter(renderer, fontRenderer, c, progress, col::Accent);
                        }
                        if (queued > 0)
                        {
                            std::string badge = "\xC3\x97" + std::to_string(queued);
                            renderer.FillRectScreen(
                                c.x + 3,
                                c.y + 3,
                                fontRenderer.GetTextWidth(badge, ui::FONT_SMALL_BOLD) + 10,
                                18,
                                col::Accent);
                            fontRenderer.DrawString(
                                badge, c.x + 8, c.y + 4, ui::FONT_SMALL_BOLD, col::AccentText, false);
                        }
                    }
                    if (available)
                    {
                        bool repeating = false;
                        for (const Building *pb : producers)
                            repeating = repeating || pb->repeatArchetype == def.archetype;
                        const UiRect inf8 = InfinityBtn(c);
                        DrawInfinity(renderer, inf8, repeating, inf8.Contains(mx, my));
                    }
                    if (hovered)
                    {
                        hoverTitle = name + "  $" + std::to_string(cost);
                        if (!unlocked)
                        {
                            hoverText
                                = std::string(_("[RESEARCH REQUIRED] "))
                                  + _(researchDef(PlayerFaction(), UnitUnlockResearch(PlayerFaction(), def.archetype))
                                          .name);
                        }
                        else if (!available)
                        {
                            hoverText = inf ? _("[BARRACKS REQUIRED] Construct Barracks to recruit ") + name
                                            : _("[FACTORY REQUIRED] Construct Factory to build ") + name;
                        }
                        else
                        {
                            hoverText = _(UnitDesc(def, entities));
                            if (queued > 0)
                                hoverText += std::string("  ") + _("(RMB - cancel one)");
                            hoverText
                                += std::string("  ") + _("(Shift - 5, Ctrl - 10; the infinity button repeats it)");
                        }
                    }
                    break;
                }
                case TabOrders:
                {
                    const auto &o = kOrders[i];
                    bool active = (i == 0 && m_attackMoveMode);
                    if (i >= kFirstStanceOrder)
                    {
                        // Lit when every selected own unit has this stance
                        const auto wanted = static_cast<UnitStance>(i - kFirstStanceOrder);
                        bool any = false, all = true;
                        for (const auto &e : entities.GetAllEntities())
                        {
                            const Unit *u = e.AsUnit();
                            if (!u || !u->alive || !u->selected || u->owner != Players::Local())
                                continue;
                            any = true;
                            all = all && u->stance == wanted;
                        }
                        active = any && all;
                    }
                    renderer.FillRectScreen(
                        c.x,
                        c.y,
                        c.w,
                        c.h,
                        active ? SDL_Color {60, 44, 20, 255} : (hovered ? col::Hover : col::PanelRaised));
                    renderer.DrawRectScreen(
                        c.x, c.y, c.w, c.h, active ? col::Accent : (hovered ? col::BorderStrong : col::Border));
                    fontRenderer.DrawString(
                        ui::FitText(fontRenderer, _(o.name), ui::FONT_BODY_BOLD, c.w - 16),
                        c.CenterX(),
                        c.CenterY() + 6,
                        ui::FONT_BODY_BOLD,
                        active ? col::Accent : col::Text,
                        false,
                        Anchor::HCENTER | Anchor::VCENTER);
                    if (o.key[0] != '\0')
                    {
                        UiRect chip {c.x + 6, c.y + 6, 22, 22};
                        renderer.FillRectScreen(chip.x, chip.y, chip.w, chip.h, col::PanelSunken);
                        renderer.DrawRectScreen(chip.x, chip.y, chip.w, chip.h, col::BorderStrong);
                        fontRenderer.DrawString(
                            o.key,
                            chip.CenterX(),
                            chip.CenterY(),
                            ui::FONT_SMALL_BOLD,
                            col::Accent,
                            false,
                            Anchor::HCENTER | Anchor::VCENTER);
                    }
                    if (hovered)
                    {
                        hoverTitle = _(o.name);
                        if (o.key[0] != '\0')
                            hoverTitle += std::string("  [") + o.key + "]";
                        hoverText = _(o.desc);
                    }
                    break;
                }
            }
        }
        renderer.ResetClipRect();

        // Scroll position hint on the right edge of the grid
        if (maxScroll > 0)
        {
            int trackX = l.panel.Right() - 5;
            renderer.FillRectScreen(trackX, l.grid.y, 2, l.grid.h, col::PanelRaised);
            int rows = maxScroll + l.VisibleRows();
            int thumbH = std::max(16, l.grid.h * l.VisibleRows() / rows);
            int thumbY = l.grid.y + (l.grid.h - thumbH) * scroll / maxScroll;
            renderer.FillRectScreen(trackX, thumbY, 2, thumbH, col::Accent);
        }

        // --- Info box -------------------------------------------------------------------------------------
        {
            const UiRect &ib = l.info;
            ui::DrawPanel(renderer, ib, col::PanelSunken, col::Border);
            int textW = ib.w - 20;

            if (!m_hudNotification.empty())
            {
                renderer.FillRectScreen(ib.x, ib.y, 3, ib.h, col::Accent);
                fontRenderer.DrawTextWrapped(
                    m_hudNotification, ib.x + 10, ib.y + 8, textW, ui::FONT_SMALL_BOLD, col::Accent, false);
            }
            else if (!hoverTitle.empty())
            {
                fontRenderer.DrawString(
                    ui::FitText(fontRenderer, hoverTitle, ui::FONT_SMALL_BOLD, textW),
                    ib.x + 10,
                    ib.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Text,
                    false);
                fontRenderer.DrawTextWrapped(
                    hoverText, ib.x + 10, ib.y + 26, textW, ui::FONT_SMALL, col::TextDim, false);
            }
            else if (
                m_buildMode && m_selectedBlueprint >= 0
                && m_selectedBlueprint < static_cast<int>(EntityManager::GetBlueprints().size()))
            {
                fontRenderer.DrawString(
                    _(EntityManager::GetBlueprints()[m_selectedBlueprint].name),
                    ib.x + 10,
                    ib.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Accent,
                    false);
                fontRenderer.DrawTextWrapped(
                    _("LMB - build, RMB / Esc - cancel"),
                    ib.x + 10,
                    ib.y + 26,
                    textW,
                    ui::FONT_SMALL,
                    col::TextDim,
                    false);
            }
            else if (
                const Building *busy = [&]() -> const Building * {
                    const auto &list = (m_activeTab == TabVehicles) ? factoryTargets : barracksTargets;
                    for (const Building *b : list)
                    {
                        if (!b->productionQueue.empty())
                            return b;
                    }
                    return nullptr;
                }();
                busy && (m_activeTab == TabInfantry || m_activeTab == TabVehicles))
            {
                const auto &item = busy->productionQueue.front();
                std::string what = _("Production");
                if (const auto *cfg = entities.GetUnitConfig(item.archetype))
                    what = _(cfg->name);
                float prog = busy->CurrentProductionProgress();
                std::string line = what + "  " + std::to_string(static_cast<int>(prog * 100.0f)) + "%";
                if (busy->productionQueue.size() > 1)
                {
                    line += "   " + std::string(_("queue")) + ": " + std::to_string(busy->productionQueue.size() - 1);
                }
                fontRenderer.DrawString(
                    ui::FitText(fontRenderer, line, ui::FONT_SMALL_BOLD, textW),
                    ib.x + 10,
                    ib.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Text,
                    false);
                ui::DrawProgressBar(renderer, {ib.x + 10, ib.y + 30, textW, 8}, prog, col::Accent);
            }
            else if (const auto own = OwnSelectedBuildings(entities, selected); !own.empty())
            {
                const Building *b = own.front();
                std::string title = _("Structure");
                if (const BuildingConfig *cfg = entities.GetBuildingConfig(b->rawBuildingType))
                    title = _(cfg->name);
                if (own.size() > 1)
                    title += "  x" + std::to_string(own.size());
                fontRenderer.DrawString(
                    ui::FitText(fontRenderer, title, ui::FONT_SMALL_BOLD, textW),
                    ib.x + 10,
                    ib.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Text,
                    false);
                const float hp = b->maxHp > 0 ? static_cast<float>(b->currentHp) / b->maxHp : 1.0f;
                ui::DrawProgressBar(renderer, {ib.x + 10, ib.y + 28, textW, 6}, hp, hp > 0.35f ? col::Ok : col::Danger);
                fontRenderer.DrawString(
                    std::to_string(b->currentHp) + " / " + std::to_string(b->maxHp),
                    ib.x + 10,
                    ib.y + 40,
                    ui::FONT_SMALL,
                    col::TextDim,
                    false);
            }
            else
            {
                static const char *const kTabHints[TabCount] = {
                    "Select building to construct",
                    "Recruit infantry at the selected or primary Barracks (RMB - cancel)",
                    "Build vehicles at the selected or primary Factory (RMB - cancel)",
                    "Select units and issue a command",
                };
                fontRenderer.DrawTextWrapped(
                    _(kTabHints[m_activeTab]), ib.x + 10, ib.y + 8, textW, ui::FONT_SMALL, col::TextMuted, false);
            }

            if (const auto ownSel = OwnSelectedBuildings(entities, selected); !ownSel.empty())
            {
                bool anyDamaged = false, allRepairing = true;
                for (const Building *b : ownSel)
                {
                    if (b->CanBeRepaired())
                    {
                        anyDamaged = true;
                        allRepairing = allRepairing && b->repairing;
                    }
                }
                if (anyDamaged)
                {
                    const UiRect rb = RepairBtn(ib);
                    const bool on = allRepairing;
                    const bool hotR = rb.Contains(mx, my);
                    renderer.FillRectScreen(
                        rb.x,
                        rb.y,
                        rb.w,
                        rb.h,
                        on ? SDL_Color {60, 44, 20, 255} : (hotR ? col::Hover : col::PanelRaised));
                    renderer.DrawRectScreen(rb.x, rb.y, rb.w, rb.h, on ? col::Accent : col::BorderStrong);
                    fontRenderer.DrawString(
                        on ? _("REPAIRING") : _("REPAIR [R]"),
                        rb.CenterX(),
                        rb.CenterY(),
                        ui::FONT_SMALL_BOLD,
                        on ? col::Accent : col::Text,
                        false,
                        Anchor::HCENTER | Anchor::VCENTER);
                }
            }
            if (!OwnSelectedBuildings(entities, selected).empty())
            {
                const UiRect db = DestroyBtn(ib);
                const bool armed = m_destroyArmed > 0.0f;
                const bool hot = db.Contains(mx, my);
                renderer.FillRectScreen(
                    db.x,
                    db.y,
                    db.w,
                    db.h,
                    armed ? col::Danger : (hot ? ui::Mix(col::PanelRaised, col::Danger, 0.35f) : col::PanelRaised));
                renderer.DrawRectScreen(db.x, db.y, db.w, db.h, col::Danger);
                fontRenderer.DrawString(
                    armed ? _("CONFIRM?") : _("DESTROY"),
                    db.CenterX(),
                    db.CenterY(),
                    ui::FONT_SMALL_BOLD,
                    armed ? col::Text : col::Danger,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
            }
        }
    }
}

namespace
{} // namespace

namespace
{
// Build radius of a headquarters / command center: purple
const SDL_Color kBuildRadiusColor {160, 80, 230, 90};
} // namespace

bool HUDController::PowerIconBlinkOn()
{
    return (SDL_GetTicks() / 400) % 2 == 0; // 0.4 s on, 0.4 s off
}

void HUDController::RenderPowerAlerts(
    Renderer &renderer, const EntityManager &entities, const Economy &economy, const Camera &camera) const
{
    if (!economy.HasPowerDeficit() || !PowerIconBlinkOn())
        return;
    const float zoom = camera.GetZoom();
    const float scale = std::clamp(zoom, 0.75f, 1.5f);
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->owner != Players::Local() || b->buildProgress < 1.0f
            || b->isPowered)
            continue;
        // Only what consumes power (power plants and power-free buildings keep working)
        const BuildingConfig *cfg = entities.GetBuildingConfig(b->rawBuildingType);
        if (!cfg || cfg->powerCost >= 0)
            continue;
        float sx = 0.0f, sy = 0.0f;
        camera.WorldToScreen((b->tileX + b->buildWidth * 0.5f) * TILE_WIDTH, b->tileY * TILE_HEIGHT, sx, sy);
        // Over the roof: the top of the footprint, a little above
        const int w = static_cast<int>(23 * scale), h = static_cast<int>(19 * scale);
        renderer.DrawRawSpriteScreen(
            kPowerIconSprite,
            static_cast<int>(sx) - w / 2,
            static_cast<int>(sy) - h - static_cast<int>(10 * zoom),
            scale);
    }
}

void HUDController::RenderProductionOverlay(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const EntityManager &entities,
    const TileMap &map,
    const Camera &camera) const
{
    const float zoom = camera.GetZoom();

    // Selected headquarters: the zone where buildings and walls may be placed (purple, 70% transparent, 2 px).
    // The radius is measured in tiles and tiles are wider than tall, so on screen it is an ellipse.
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->selected || !b->alive || b->state == ActionState::Die || b->owner != Players::Local()
            || b->type != UnitType::Headquarters || b->buildProgress < 1.0f)
        {
            continue;
        }
        const float cx = (b->tileX + b->buildWidth * 0.5f) * TILE_WIDTH;
        const float cy = (b->tileY + b->buildHeight * 0.5f) * TILE_HEIGHT;
        const float r = static_cast<float>(entities.GetBuildRadius());
        // Counted in tiles (the placement check is a distance in tile units), so it is an ellipse on screen
        RenderUtility::DrawTileRadius(renderer, camera, cx, cy, r, kBuildRadiusColor, 2.0f);
    }

    // Selected defensive structures: their firing range (own green, enemy red)
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->selected || !b->alive || b->state == ActionState::Die || b->attackRange <= 0.0f
            || b->attackDamage <= 0)
        {
            continue;
        }
        // The attack range, red for everybody: the ring the game really measures the firing distance with
        const SDL_Color c {240, 80, 70, 90};
        RenderUtility::DrawRangeRing(
            renderer, camera, b->GetUnitCenterPixelX(), b->GetUnitCenterPixelY(), b->attackRange, c, 2.0f);
    }

    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->owner != Players::Local() || !b->IsUnitProducer())
        {
            continue;
        }
        float sx = 0.0f, sy = 0.0f;
        camera.WorldToScreen(b->tileX * TILE_WIDTH, b->tileY * TILE_HEIGHT, sx, sy);
        const int w = static_cast<int>(b->buildWidth * TILE_WIDTH * zoom);

        // Training progress above the building, with the number of orders waiting behind it
        if (!b->productionQueue.empty())
        {
            const int barW = std::max(36, w - 16);
            const int bx = static_cast<int>(sx) + (w - barW) / 2;
            const int by = static_cast<int>(sy) - 22;
            renderer.FillRectScreen(bx - 1, by - 1, barW + 2, 8, {10, 12, 16, 220});
            renderer.FillRectScreen(bx, by, static_cast<int>(barW * b->CurrentProductionProgress()), 6, col::Accent);
            const int count = static_cast<int>(b->productionQueue.size());
            std::string label = std::to_string(count);
            const int lw = fontRenderer.GetTextWidth(label, ui::FONT_SMALL_BOLD) + 8;
            renderer.FillRectScreen(bx + barW + 3, by - 5, lw, 16, col::Accent);
            fontRenderer.DrawString(
                label,
                bx + barW + 3 + lw / 2,
                by + 3,
                ui::FONT_SMALL_BOLD,
                col::AccentText,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }

        // Rally line from the exit to the rally point, only while the building is selected
        if (!b->selected)
            continue;
        int rx = 0, ry = 0;
        RallyPointOf(*b, map, rx, ry);
        const float x1 = (b->tileX + b->buildWidth * 0.5f) * TILE_WIDTH;
        const float y1 = (b->tileY + b->buildHeight) * TILE_HEIGHT;
        const float x2 = (rx + 0.5f) * TILE_WIDTH;
        const float y2 = (ry + 0.5f) * TILE_HEIGHT;
        const SDL_Color yellow {255, 220, 40, 150};
        float ax = 0, ay = 0, bx2 = 0, by2 = 0;
        camera.WorldToScreen(x1, y1, ax, ay);
        camera.WorldToScreen(x2, y2, bx2, by2);
        // 2 px wide: two parallel 1 px lines offset across the line's main direction
        const bool steep = std::abs(by2 - ay) > std::abs(bx2 - ax);
        for (int k = 0; k < 2; ++k)
        {
            const int ox = steep ? k : 0;
            const int oy = steep ? 0 : k;
            renderer.DrawLineScreen(
                static_cast<int>(ax) + ox,
                static_cast<int>(ay) + oy,
                static_cast<int>(bx2) + ox,
                static_cast<int>(by2) + oy,
                yellow);
        }
        const int m = std::max(4, static_cast<int>(5 * zoom));
        renderer.FillRectScreen(static_cast<int>(bx2) - m / 2, static_cast<int>(by2) - m / 2, m, m, yellow);
        renderer.DrawRectScreen(static_cast<int>(bx2) - m, static_cast<int>(by2) - m, 2 * m, 2 * m, yellow);
    }
}

void HUDController::RenderBuildGhost(
    Renderer &renderer,
    const TileMap &map,
    const Camera &camera,
    int mouseTileX,
    int mouseTileY,
    const Economy &economy,
    const EntityManager &entities)
{
    const auto &blueprints = EntityManager::GetBlueprints();
    if (m_selectedBlueprint < 0 || m_selectedBlueprint >= static_cast<int>(blueprints.size()))
        return;

    const auto &bp = blueprints[m_selectedBlueprint];

    bool canPlace = entities.CanPlaceBuilding(mouseTileX, mouseTileY, bp.width, bp.height, map, Players::Local());
    bool canAfford = economy.CanAfford(bp.creditCost);

    // While a building is picked, the line of the build zone of every finished headquarters shows where it may stand
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *hq = e.AsBuilding();
        if (!hq || !hq->alive || hq->state == ActionState::Die || hq->owner != Players::Local()
            || hq->type != UnitType::Headquarters || hq->buildProgress < 1.0f)
            continue;
        RenderUtility::DrawTileRadius(
            renderer,
            camera,
            (hq->tileX + hq->buildWidth * 0.5f) * TILE_WIDTH,
            (hq->tileY + hq->buildHeight * 0.5f) * TILE_HEIGHT,
            static_cast<float>(entities.GetBuildRadius()),
            kBuildRadiusColor,
            2.0f);
    }

    SDL_Color boxColor = (canPlace && canAfford) ? SDL_Color {0, 255, 0, 80} : SDL_Color {255, 0, 0, 80};
    SDL_Color borderColor = (canPlace && canAfford) ? SDL_Color {0, 255, 0, 220} : SDL_Color {255, 0, 0, 220};

    // Footprint tiles in world space so they scale with the camera zoom like the building itself
    for (int ty = mouseTileY; ty < mouseTileY + bp.height; ++ty)
    {
        for (int tx = mouseTileX; tx < mouseTileX + bp.width; ++tx)
        {
            const float wx = static_cast<float>(tx * TILE_WIDTH);
            const float wy = static_cast<float>(ty * TILE_HEIGHT);
            renderer.FillRectWorld(
                wx, wy, static_cast<float>(TILE_WIDTH), static_cast<float>(TILE_HEIGHT), camera, boxColor);
            renderer.DrawRectWorld(
                wx, wy, static_cast<float>(TILE_WIDTH), static_cast<float>(TILE_HEIGHT), camera, borderColor);
        }
    }

    // A tower / bunker shows where it will reach from this spot
    if (const BuildingConfig *cfg = entities.GetBuildingConfig(bp.buildingIndex))
    {
        if (cfg->canAttack && cfg->attackRange > 0.0f)
        {
            RenderUtility::DrawRangeRing(
                renderer,
                camera,
                (mouseTileX + bp.width * 0.5f) * TILE_WIDTH,
                (mouseTileY + bp.height * 0.5f) * TILE_HEIGHT,
                cfg->attackRange,
                {240, 80, 70, 90},
                2.0f);
        }
    }

    // Walls are drawn exactly like the placed segment (building 15 / 25, the reinforced 20 after Active Wall Armor),
    // not with a stray composite
    const int drawIndex = (bp.buildingIndex == 15 && IsEffectResearched(Players::Local(), ResearchEffect::WallStrength))
                              ? 20
                              : bp.buildingIndex;
    renderer.SetDrawOwner(Players::Local()); // the ghost already wears the player's army colour
    renderer.DrawBuildingWorld(
        drawIndex, mouseTileX * TILE_WIDTH, mouseTileY * TILE_HEIGHT, 0, camera, PlayerFaction(), 0, 1.0f, false);
    renderer.DrawBuildingWorld(
        drawIndex, mouseTileX * TILE_WIDTH, mouseTileY * TILE_HEIGHT, 1, camera, PlayerFaction(), 0, 1.0f, false);
    renderer.SetDrawOwner(kNoPlayer);
}

} // namespace OpenArtOfWar
