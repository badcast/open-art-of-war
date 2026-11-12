#include "ui/AndroidHUD.hpp"
#include "HUDControllerInternal.hpp"

#include "game/Mines.hpp"
#include "game/Simulation.hpp"
#include "ui/UiLayout.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace OpenArtOfWar
{

namespace
{

constexpr int kCardW = 136;
constexpr int kCardH = 88;
constexpr int kCardGap = 8;

bool HasCombatUnits(const EntityManager &entities, const std::vector<int> &selected)
{
    for (int id : selected)
    {
        const Entity *e = entities.GetEntity(id);
        if (e && e->alive && !e->isBuilding)
            return true;
    }
    return false;
}

} // namespace

AndroidHUD::Layout AndroidHUD::MakeLayout(
    int windowW, int windowH, bool hasCombat, bool hasBuilding, int selectedCount) const
{
    // Where things stand is ui/layout.yaml (screen hud, profile touch); this only answers its conditions
    const uilayout::Conditions cond = [&](const std::string &name) {
        if (name == "placing")
            return m_placing;
        if (name == "panel_open")
            return m_panel != Panel::None;
        if (name == "panel_build")
            return m_panel == Panel::Build;
        if (name == "panel_units")
            return m_panel == Panel::Units;
        if (name == "units_inf")
            return m_unitTab == TabInfantry;
        if (name == "units_veh")
            return m_unitTab == TabVehicles;
        if (name == "box_select")
            return m_boxSelect;
        if (name == "attack_mode")
            return m_attackMoveMode;
        if (name == "sell_armed")
            return m_destroyArmed > 0.0f;
        if (name == "radar_zoom_min")
            return m_radarZoom <= 1;
        if (name == "radar_zoom_max")
            return m_radarZoom >= kRadarMaxZoom;
        if (name == "has_combat")
            return hasCombat;
        if (name == "has_building")
            return hasBuilding;
        if (name == "has_selection")
            return selectedCount > 0;
        return false;
    };
    static const std::map<std::string, Act> kActs
        = {{"menu", Act::Menu},
           {"goals", Act::Goals},
           {"tech", Act::Tech},
           {"dock_build", Act::DockBuild},
           {"dock_units", Act::DockUnits},
           {"dock_box", Act::DockBox},
           {"attack_move", Act::AttackMove},
           {"stop", Act::Stop},
           {"hold", Act::Hold},
           {"siege", Act::Siege},
           {"clear", Act::ClearSel},
           {"select_all", Act::SelectAll},
           {"repair", Act::Repair},
           {"sell", Act::Sell},
           {"confirm", Act::Confirm},
           {"cancel", Act::Cancel},
           {"units_infantry", Act::UnitsInfantry},
           {"units_vehicles", Act::UnitsVehicles},
           {"scroll_left", Act::ScrollLeft},
           {"scroll_right", Act::ScrollRight},
           {"radar_zoom_in", Act::RadarZoomIn},
           {"radar_zoom_out", Act::RadarZoomOut}};

    const uilayout::Solved solved = uilayout::Current().Solve("hud", "touch", windowW, windowH, cond);
    Layout l;
    l.u = solved.u;
    l.resources = solved.Rect("resources");
    l.radar = solved.Rect("radar");
    l.cards = solved.Rect("cards");
    l.notice = solved.Rect("notice");
    for (const uilayout::Node &n : solved.nodes)
    {
        if (!n.visible || n.type != "button")
            continue;
        const auto it = kActs.find(n.action);
        if (it == kActs.end())
            continue;
        Button b;
        b.r = n.rect;
        b.act = it->second;
        b.label = _(n.label.c_str());
        b.active = n.active;
        b.disabled = n.disabled;
        b.style = n.style == "danger" ? ui::ButtonStyle::Danger
                  : n.style == "primary" ? ui::ButtonStyle::Primary
                                         : ui::ButtonStyle::Secondary;
        l.buttons.push_back(std::move(b));
    }
    if (l.cards.w > 0)
    {
        const int arrow = static_cast<int>(std::lround(48 * l.u));
        const int offset = m_panel == Panel::Units ? static_cast<int>(std::lround(70 * l.u)) + 4 : 0;
        l.cardsFit = std::max(1, (l.cards.w - 2 * arrow - offset) / (kCardW + kCardGap));
    }
    return l;
}

UiRect AndroidHUD::CardRect(const Layout &l, int slot) const
{
    const int arrow = static_cast<int>(std::lround(48 * l.u));
    const int offset = m_panel == Panel::Units ? static_cast<int>(std::lround(70 * l.u)) + 4 : 0;
    return {l.cards.x + arrow + offset + slot * (kCardW + kCardGap), l.cards.y, kCardW, kCardH};
}

bool AndroidHUD::OverPanels(const Layout &l, int mx, int my) const
{
    if (l.resources.Contains(mx, my) || l.radar.Contains(mx, my))
        return true;
    for (const Button &b : l.buttons)
        if (b.r.Contains(mx, my))
            return true;
    if (m_panel != Panel::None && !m_placing && l.cards.Contains(mx, my))
        return true;
    return false;
}

bool AndroidHUD::IsOverHud(int mx, int my, int windowW, int windowH) const
{
    const Layout l = MakeLayout(windowW, windowH, false, false, 0);
    return OverPanels(l, mx, my);
}

std::vector<SDL_Rect> AndroidHUD::TouchPanels(int windowW, int windowH) const
{
    const Layout l = MakeLayout(windowW, windowH, true, true, 1);
    std::vector<SDL_Rect> out;
    auto push = [&](const UiRect &r) { out.push_back({r.x, r.y, r.w, r.h}); };
    push(l.resources);
    push(l.radar);
    for (const Button &b : l.buttons)
        push(b.r);
    if (m_panel != Panel::None && !m_placing)
        push(l.cards);
    if (m_placing && m_ghostRect.w > 0)
        out.push_back(m_ghostRect);
    return out;
}

void AndroidHUD::UpdateGhostRect(const Camera &camera)
{
    const auto &blueprints = EntityManager::GetBlueprints();
    if (!m_placing || m_selectedBlueprint < 0 || m_selectedBlueprint >= static_cast<int>(blueprints.size()))
    {
        m_ghostRect = {0, 0, 0, 0};
        return;
    }
    const BuildingBlueprint &bp = blueprints[static_cast<std::size_t>(m_selectedBlueprint)];
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    camera.WorldToScreen(static_cast<float>(m_ghostX * TILE_WIDTH), static_cast<float>(m_ghostY * TILE_HEIGHT), x0, y0);
    camera.WorldToScreen(
        static_cast<float>((m_ghostX + bp.width) * TILE_WIDTH), static_cast<float>((m_ghostY + bp.height) * TILE_HEIGHT), x1, y1);
    const int pad = 24; // a finger is not a pixel
    m_ghostRect = {static_cast<int>(x0) - pad, static_cast<int>(y0) - pad, static_cast<int>(x1 - x0) + 2 * pad, static_cast<int>(y1 - y0) + 2 * pad};
}

void AndroidHUD::StartPlacing(const Camera &camera)
{
    // What the cameo click started (the ghost of the desktop interface) becomes the touch placement
    m_buildMode = false;
    m_placing = true;
    m_panel = Panel::None;
    m_attackMoveMode = false;
    m_grab = m_fingerWas = false;
    float wx = 0.0f, wy = 0.0f;
    camera.ScreenToWorld(
        camera.GetViewportWidth() * 0.5f, camera.GetViewportHeight() * 0.5f - 40.0f, wx, wy);
    m_ghostX = static_cast<int>(std::floor(wx / TILE_WIDTH));
    m_ghostY = static_cast<int>(std::floor(wy / TILE_HEIGHT));
}

bool AndroidHUD::ConfirmPlacing(Economy &economy, const EntityManager &entities, const TileMap &map)
{
    const auto &blueprints = EntityManager::GetBlueprints();
    if (m_selectedBlueprint < 0 || m_selectedBlueprint >= static_cast<int>(blueprints.size()))
        return false;
    const BuildingBlueprint &bp = blueprints[static_cast<std::size_t>(m_selectedBlueprint)];
    const bool isMine = bp.mineKind >= 0;
    const bool canPlace = isMine ? entities.CanPlaceMine(bp.mineKind, m_ghostX, m_ghostY, map, Players::Local())
                                 : entities.CanPlaceBuilding(m_ghostX, m_ghostY, bp.width, bp.height, map, Players::Local());
    if (!isMine && !BuildLimits::CanBuildMore(entities, map, Players::Local(), bp.buildingIndex))
    {
        SoundRequests::Play("ui_error");
        SetNotification(
            BuildLimits::LimitOf(entities, bp.buildingIndex) == 0 ? _("THIS BUILDING IS NOT ALLOWED!")
                                                                  : _("BUILDING LIMIT REACHED!"));
        return false;
    }
    if (!economy.CanAfford(bp.creditCost))
    {
        SoundRequests::Announce("ann_insufficient_funds");
        return false;
    }
    if (!canPlace)
    {
        SoundRequests::Announce("ann_cannot_build");
        return false;
    }
    SimCommand c;
    c.type = CommandType::PlaceBuilding;
    c.a = bp.blueprintId;
    c.x = m_ghostX;
    c.y = m_ghostY;
    Issue(std::move(c));
    SoundRequests::Play("build_place");
    if (!bp.isWall && !isMine)
    {
        SoundRequests::Announce("ann_building");
        m_placing = false; // a building ends the placement; walls and mines are laid one after the other
    }
    return true;
}

void AndroidHUD::SyncPanelWithTab()
{
    // Selecting a Barracks / Factory switches the sidebar tab of the desktop interface: here it opens the cards
    if (m_activeTab == m_seenTab)
        return;
    m_seenTab = m_activeTab;
    if (m_placing)
        return;
    if (m_activeTab == TabInfantry || m_activeTab == TabVehicles)
    {
        m_unitTab = m_activeTab;
        m_panel = Panel::Units;
    }
}

void AndroidHUD::Press(
    Act act,
    const Layout &l,
    Economy &economy,
    EntityManager &entities,
    TileMap &map,
    Camera &camera,
    bool &outOpenMenu,
    bool &outOpenObjectives,
    std::vector<int> &selectedUnits)
{
    (void) l;
    // One mode at a time: any button but the two of the placement itself ends the placement
    if (m_placing && act != Act::Confirm && act != Act::Cancel)
        m_placing = false;
    switch (act)
    {
        case Act::Menu:
            outOpenMenu = true;
            break;
        case Act::Goals:
            outOpenObjectives = true;
            break;
        case Act::Tech:
            OpenResearchWindow();
            break;
        case Act::DockBuild:
            m_panel = m_panel == Panel::Build ? Panel::None : Panel::Build;
            m_activeTab = TabBuild;
            m_attackMoveMode = false;
            break;
        case Act::DockUnits:
            m_panel = m_panel == Panel::Units ? Panel::None : Panel::Units;
            m_activeTab = m_unitTab;
            m_attackMoveMode = false;
            break;
        case Act::DockBox:
            m_boxSelect = !m_boxSelect;
            break;
        case Act::AttackMove:
            ActivateCell(TabOrders, 0, false, economy, entities, map, selectedUnits);
            break;
        case Act::Siege:
            ActivateCell(TabOrders, 1, false, economy, entities, map, selectedUnits);
            break;
        case Act::Stop:
            ActivateCell(TabOrders, 2, false, economy, entities, map, selectedUnits);
            break;
        case Act::Hold:
            ActivateCell(TabOrders, 3, false, economy, entities, map, selectedUnits);
            break;
        case Act::ClearSel:
            selectedUnits.clear();
            for (auto &e : entities.GetAllEntities())
                e.selected = false;
            m_attackMoveMode = false;
            break;
        case Act::SelectAll:
            selectedUnits.clear();
            for (auto &e : entities.GetAllEntities())
            {
                const Unit *u = e.AsUnit();
                e.selected = u && u->alive && u->state != ActionState::Die && u->owner == Players::Local();
                if (e.selected)
                    selectedUnits.push_back(e.id);
            }
            break;
        case Act::Repair:
        {
            const auto own = OwnSelectedBuildings(entities, selectedUnits);
            if (own.empty())
                break;
            SimCommand c;
            c.type = CommandType::ToggleRepair;
            for (const Building *b : own)
                c.ids.push_back(static_cast<std::uint32_t>(b->id));
            Issue(std::move(c));
            break;
        }
        case Act::Sell:
        {
            const auto own = OwnSelectedBuildings(entities, selectedUnits);
            if (own.empty())
                break;
            if (m_destroyArmed <= 0.0f)
            {
                m_destroyArmed = 3.0f; // the second tap confirms
                break;
            }
            m_destroyArmed = 0.0f;
            SimCommand c;
            c.type = CommandType::Demolish;
            for (const Building *b : own)
                c.ids.push_back(static_cast<std::uint32_t>(b->id));
            selectedUnits.erase(
                std::remove_if(
                    selectedUnits.begin(),
                    selectedUnits.end(),
                    [&](int id) {
                        return std::find(c.ids.begin(), c.ids.end(), static_cast<std::uint32_t>(id)) != c.ids.end();
                    }),
                selectedUnits.end());
            Issue(std::move(c));
            break;
        }
        case Act::Confirm:
            ConfirmPlacing(economy, entities, map);
            break;
        case Act::Cancel:
            m_placing = false;
            break;
        case Act::UnitsInfantry:
            m_unitTab = m_activeTab = TabInfantry;
            m_scroll[1] = 0;
            break;
        case Act::UnitsVehicles:
            m_unitTab = m_activeTab = TabVehicles;
            m_scroll[2] = 0;
            break;
        case Act::ScrollLeft:
        case Act::ScrollRight:
        {
            const int slot = m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2);
            const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
            const int maxStart = std::max(0, ItemCountForTab(tab) - l.cardsFit);
            m_scroll[slot] = std::clamp(m_scroll[slot] + (act == Act::ScrollLeft ? -1 : 1), 0, maxStart);
            break;
        }
        case Act::RadarZoomIn:
            m_radarZoom = std::min(kRadarMaxZoom, m_radarZoom + 1);
            break;
        case Act::RadarZoomOut:
            m_radarZoom = std::max(1, m_radarZoom - 1);
            break;
        case Act::None:
            break;
    }
    (void) camera;
}

bool AndroidHUD::HandleHudClick(
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
    (void) wheelSteps;
    SyncPanelWithTab();
    if (m_destroyArmed > 0.0f && OwnSelectedBuildings(entities, selectedUnits).empty())
        m_destroyArmed = 0.0f;
    const bool combat = HasCombatUnits(entities, selectedUnits);
    const bool building = !OwnSelectedBuildings(entities, selectedUnits).empty();
    const Layout l = MakeLayout(windowW, windowH, combat, building, static_cast<int>(selectedUnits.size()));
    m_panels.clear();

    if (!leftMouseDown)
        m_radarHeld = false;

    // The radar: a tap or a drag on it puts the camera there (the zoom buttons on it are buttons)
    bool onButton = false;
    for (const Button &b : l.buttons)
        onButton = onButton || b.r.Contains(mx, my);
    if (l.radar.Contains(mx, my) && !onButton && (leftMouseDown || leftClicked))
        m_radarHeld = true;
    m_radarDragging = m_radarHeld && leftMouseDown; // a zoomed radar holds still while the camera follows the finger
    if (m_radarHeld)
    {
        const RadarView v = radarView(l.radar, map.GetWidth(), map.GetHeight(), m_radarZoom, m_radarCenterX, m_radarCenterY);
        if (v.w > 0 && v.h > 0)
        {
            const float tx = std::clamp(v.viewX + static_cast<float>(mx - v.ox) / v.scale, 0.0f, static_cast<float>(map.GetWidth()));
            const float ty = std::clamp(v.viewY + static_cast<float>(my - v.oy) / v.scale, 0.0f, static_cast<float>(map.GetHeight()));
            camera.CenterOn(tx * TILE_WIDTH, ty * TILE_HEIGHT);
        }
        return true;
    }

    const bool onPanel = OverPanels(l, mx, my) || (m_panel != Panel::None && !m_placing && l.cards.Contains(mx, my));
    UpdateGhostRect(camera);
    if (!leftMouseDown)
    {
        m_grab = false;
        m_fingerWas = false;
    }

    // Placing: a finger on the map drags the ghost; nothing else on the map happens meanwhile
    // (a tap that is no drag ends the placement: the focus leaves the building)
    if (m_placing && !onPanel)
    {
        if (leftClicked)
        {
            // A tap on the building itself builds it (when the place is fit); a tap anywhere else leaves the placement
            const auto &blueprints = EntityManager::GetBlueprints();
            float wx = 0.0f, wy = 0.0f;
            camera.ScreenToWorld(static_cast<float>(mx), static_cast<float>(my), wx, wy);
            const int tx = static_cast<int>(std::floor(wx / TILE_WIDTH)), ty = static_cast<int>(std::floor(wy / TILE_HEIGHT));
            bool onGhost = false;
            if (m_selectedBlueprint >= 0 && m_selectedBlueprint < static_cast<int>(blueprints.size()))
            {
                const BuildingBlueprint &bp = blueprints[static_cast<std::size_t>(m_selectedBlueprint)];
                onGhost = tx >= m_ghostX && tx < m_ghostX + bp.width && ty >= m_ghostY && ty < m_ghostY + bp.height;
            }
            if (onGhost)
                ConfirmPlacing(economy, entities, map);
            else
                m_placing = false;
        }
        else if (leftMouseDown)
        {
            float fx = 0.0f, fy = 0.0f;
            camera.ScreenToWorld(static_cast<float>(mx), static_cast<float>(my), fx, fy);
            if (!m_fingerWas)
            {
                // The finger just came down: on the ghost it takes it where it touched (the ghost does not jump
                // under the finger); elsewhere the drag is the camera's (InputManager)
                m_fingerWas = true;
                const SDL_Rect &r = m_ghostRect;
                m_grab = mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h;
                m_grabOffX = static_cast<float>(m_ghostX * TILE_WIDTH) - fx;
                m_grabOffY = static_cast<float>(m_ghostY * TILE_HEIGHT) - fy;
            }
            if (m_grab)
            {
                m_ghostX = static_cast<int>(std::lround((fx + m_grabOffX) / TILE_WIDTH));
                m_ghostY = static_cast<int>(std::lround((fy + m_grabOffY) / TILE_HEIGHT));
                // Near the edge of the screen the camera follows, so the building can be taken far
                const int margin = 56;
                const float step = 9.0f;
                const float dx = mx < margin ? -step : (mx > camera.GetViewportWidth() - margin ? step : 0.0f);
                const float dy = my < margin ? -step : (my > camera.GetViewportHeight() - margin ? step : 0.0f);
                if (dx != 0.0f || dy != 0.0f)
                    camera.Pan(dx, dy);
            }
        }
        return true;
    }
    if (!onPanel)
    {
        // A tap on the map closes the open cards (and does nothing else: one mode at a time)
        if (leftClicked && m_panel != Panel::None)
        {
            m_panel = Panel::None;
            return true;
        }
        return false;
    }

    if (rightClicked && m_panel == Panel::Units && l.cards.Contains(mx, my))
    {
        // Holding a unit card takes one order back
        for (int s = 0; s < l.cardsFit; ++s)
        {
            if (!CardRect(l, s).Contains(mx, my))
                continue;
            const int idx = ItemAt(m_unitTab, m_scroll[m_unitTab == TabInfantry ? 1 : 2] + s);
            if (idx >= 0)
                ActivateCell(m_unitTab, idx, true, economy, entities, map, selectedUnits);
        }
        return true;
    }
    if (!leftClicked)
        return true;

    for (const Button &b : l.buttons)
    {
        if (b.r.Contains(mx, my) && !b.disabled)
        {
            Press(b.act, l, economy, entities, map, camera, outOpenMenu, outOpenObjectives, selectedUnits);
            return true;
        }
    }
    if (m_panel != Panel::None && l.cards.Contains(mx, my))
    {
        const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
        const int first = m_scroll[m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2)];
        for (int s = 0; s < l.cardsFit; ++s)
        {
            if (!CardRect(l, s).Contains(mx, my))
                continue;
            const int idx = ItemAt(tab, first + s);
            if (idx < 0)
                break;
            ActivateCell(tab, idx, false, economy, entities, map, selectedUnits);
            if (m_buildMode)
                StartPlacing(camera);
            break;
        }
    }
    return true;
}

void AndroidHUD::RenderGameHud(
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
    const bool combat = HasCombatUnits(entities, selected);
    const bool building = !OwnSelectedBuildings(entities, selected).empty();
    const Layout l = MakeLayout(windowW, windowH, combat, building, static_cast<int>(selected.size()));
    const auto S = [&](int v) { return static_cast<int>(std::lround(v * l.u)); };

    if (m_placing)
    {
        RenderBuildGhost(renderer, map, camera, m_ghostX, m_ghostY, economy, entities);
        UpdateGhostRect(camera);
    }

    // Credits and power
    {
        const UiRect &h = l.resources;
        renderer.FillRectScreen(h.x, h.y, h.w, h.h, {10, 13, 18, 210});
        renderer.DrawRectScreen(h.x, h.y, h.w, h.h, col::BorderStrong);
        fontRenderer.DrawString(
            "$ " + std::to_string(economy.GetCredits()), h.x + 8, h.y + 3, ui::FONT_H2, col::Accent, true);
        const int prod = economy.GetPowerProduced();
        const int cons = economy.GetPowerConsumed();
        const bool deficit = economy.HasPowerDeficit();
        const SDL_Color pCol = deficit ? col::Danger : col::Ok;
        const std::string pwrText = std::to_string(cons) + "/" + std::to_string(prod) + _(" kW");
        // The lightning of the game next to the figures; it blinks while the power is short
        if (!deficit || PowerIconBlinkOn())
        {
            const int textW = fontRenderer.GetTextWidth(pwrText, ui::FONT_SMALL_BOLD);
            renderer.DrawRawSpriteScreen(PowerIconSprite(), h.Right() - 8 - textW - 26, h.y + 2, 1.0f);
        }
        fontRenderer.DrawString(
            std::to_string(cons) + "/" + std::to_string(prod) + _(" kW"),
            h.Right() - 8,
            h.y + 5,
            ui::FONT_SMALL_BOLD,
            pCol,
            false,
            Anchor::RIGHT | Anchor::TOP);
        ui::DrawProgressBar(
            renderer,
            {h.x + 6, h.Bottom() - 9, h.w - 12, 5},
            static_cast<float>(cons) / static_cast<float>(std::max(1, prod)),
            pCol);
    }

    // The radar (no zoom buttons: a tap puts the camera there)
    DrawRadar(renderer, fontRenderer, l.radar, entities, map, camera, -1, -1, false);

    // Cards
    if (m_panel != Panel::None && !m_placing)
    {
        renderer.FillRectScreen(l.cards.x, l.cards.y - 4, l.cards.w, l.cards.h + 8, {10, 13, 18, 215});
        const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
        const int first = m_scroll[m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2)];
        const bool noHq = !entities.HasBuildingType(UnitType::Headquarters, Players::Local(), true);
        const bool hasBarracks = entities.HasBuildingType(UnitType::Barracks, Players::Local());
        const bool hasFactory = entities.HasBuildingType(UnitType::Factory, Players::Local());
        for (int s = 0; s < l.cardsFit; ++s)
        {
            const int idx = ItemAt(tab, first + s);
            if (idx < 0)
                break;
            const UiRect c = CardRect(l, s);
            const bool hot = c.Contains(mx, my);
            if (tab == TabBuild)
            {
                const auto &bp = EntityManager::GetBlueprints()[static_cast<std::size_t>(idx)];
                const bool isMine = bp.mineKind >= 0;
                const int limit = isMine ? Mines::Specs()[static_cast<std::size_t>(bp.mineKind)].maxPerPlayer
                                         : BuildLimits::LimitOf(entities, bp.buildingIndex);
                const int owned = limit > 0 ? (isMine ? entities.CountMines(Players::Local(), bp.mineKind)
                                                      : BuildLimits::CountOwned(
                                                          entities, map, Players::Local(), bp.buildingIndex))
                                            : 0;
                const bool locked = noHq || limit == 0 || (limit > 0 && owned >= limit)
                                    || (bp.buildingIndex == 6 && !IsRadarUnlocked(Players::Local()));
                DrawCameo(renderer, fontRenderer, c, bp.iconCompId, _(bp.name), hot, false, locked);
                DrawCostTag(
                    renderer,
                    fontRenderer,
                    c,
                    "$" + std::to_string(bp.creditCost),
                    economy.CanAfford(bp.creditCost) ? col::Accent : col::Danger);
                if (limit > 0)
                    fontRenderer.DrawString(
                        std::to_string(owned) + "/" + std::to_string(limit),
                        c.x + 6,
                        c.y + 5,
                        ui::FONT_SMALL_BOLD,
                        owned >= limit ? col::Danger : col::TextMuted);
            }
            else
            {
                const bool inf = tab == TabInfantry;
                const UnitCameoDef &def = unitCatalog(inf).items[idx];
                const bool available = (inf ? hasBarracks : hasFactory) && IsUnitUnlocked(Players::Local(), def.archetype);
                DrawCameo(
                    renderer,
                    fontRenderer,
                    c,
                    SpriteHandle(def.icon, SpriteKind::Part, "unit cameo"),
                    _(def.name),
                    hot,
                    false,
                    !available);
                const auto *cfg = entities.GetUnitConfig(def.archetype);
                const int cost = cfg ? cfg->creditCost : def.cost;
                DrawCostTag(
                    renderer, fontRenderer, c, "$" + std::to_string(cost), economy.CanAfford(cost) ? col::Accent : col::Danger);
                int queued = 0;
                float progress = -1.0f;
                for (int id : ProductionTargets(inf ? UnitType::Barracks : UnitType::Factory, entities, selected))
                {
                    const Building *b = entities.GetBuilding(id);
                    if (!b)
                        continue;
                    for (const auto &q : b->productionQueue)
                        queued += q.archetype == def.archetype ? 1 : 0;
                    if (!b->productionQueue.empty() && b->productionQueue.front().archetype == def.archetype)
                        progress = std::max(progress, b->CurrentProductionProgress());
                }
                if (available && progress >= 0.0f)
                    DrawProgressShutter(renderer, fontRenderer, c, progress, col::Accent);
                if (queued > 0)
                    fontRenderer.DrawString(
                        "\xC3\x97" + std::to_string(queued), c.x + 6, c.y + 5, ui::FONT_BODY_BOLD, col::Accent, true);
            }
        }
        const int count = ItemCountForTab(tab);
        if (count == 0)
            fontRenderer.DrawString(
                _("NOTHING AVAILABLE"), l.cards.CenterX(), l.cards.CenterY(), ui::FONT_BODY, col::TextMuted, false,
                Anchor::HCENTER | Anchor::VCENTER);
    }

    // Buttons
    for (const Button &b : l.buttons)
    {
        ui::DrawButton(
            renderer,
            fontRenderer,
            b.r,
            ui::FitText(fontRenderer, b.label, ui::FONT_SMALL_BOLD, b.r.w - 8),
            b.active ? ui::ButtonStyle::Primary : b.style,
            b.r.Contains(mx, my),
            b.disabled);
    }

    // What is selected, over the orders
    if (!selected.empty() && !m_placing)
    {
        const std::string info = std::to_string(selected.size()) + " " + _("SELECTED");
        fontRenderer.DrawString(
            info,
            l.radar.Right() + S(8),
            windowH - S(8) - S(60) - 18,
            ui::FONT_SMALL_BOLD,
            col::Text,
            true);
    }

    // The line of text at the top: a notice, or what a mode wants from the next tap
    std::string notice = m_hudNotificationTimer > 0.0f ? m_hudNotification : std::string();
    if (notice.empty() && m_attackMoveMode)
        notice = _("TAP THE TARGET: ATTACK-MOVE");
    if (notice.empty() && m_placing)
        notice = _("DRAG THE GHOST, THEN BUILD HERE");
    if (notice.empty() && m_boxSelect)
        notice = _("BOX SELECT: DRAG OVER THE UNITS");
    if (!notice.empty())
    {
        const int tw = fontRenderer.GetTextWidth(notice, ui::FONT_BODY_BOLD) + 24;
        const UiRect n {windowW / 2 - tw / 2, l.notice.y, tw, l.notice.h};
        renderer.FillRectScreen(n.x, n.y, n.w, n.h, {10, 13, 18, 220});
        renderer.DrawRectScreen(n.x, n.y, n.w, n.h, col::Accent);
        fontRenderer.DrawString(
            notice, n.CenterX(), n.CenterY(), ui::FONT_BODY_BOLD, col::Accent, false, Anchor::HCENTER | Anchor::VCENTER);
    }
    (void) my;
}

} // namespace OpenArtOfWar
