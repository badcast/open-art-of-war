#include "ui/TouchHUD.hpp"
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

constexpr int kCardGap = 8;
constexpr int kCardColumns = 3;
constexpr int kCardRows = 3;

bool HasCombatUnits(const EntityManager &entities, const std::vector<int> &selected)
{
    for (int entityId : selected)
    {
        const Entity *entity = entities.GetEntity(entityId);
        if (entity && entity->alive && !entity->isBuilding)
            return true;
    }
    return false;
}

} // namespace

TouchHUD::Layout TouchHUD::MakeLayout(
    int windowWidth, int windowHeight, bool hasCombat, bool hasBuilding, int selectedCount) const
{
    // Where things stand is ui/layout.yaml (screen hud, profile touch); this only answers its conditions
    const uilayout::Conditions conditions = [&](const std::string &name) {
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
    static const std::map<std::string, Act> kActions
        = {{"menu", Act::Menu},
           {"goals", Act::Goals},
           {"tech", Act::Tech},
           {"dock_build", Act::DockBuild},
           {"dock_units", Act::DockUnits},
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

    const uilayout::Solved solved = uilayout::Current().Solve("hud", "touch", windowWidth, windowHeight, conditions);
    Layout layout;
    layout.u = solved.u;
    layout.resources = solved.Rect("resources");
    layout.radar = solved.Rect("radar");
    layout.cards = solved.Rect("cards");
    const auto px = [&](int v) {
        return static_cast<int>(std::lround(v * layout.u));
    };
    layout.cards = {windowWidth - px(424), windowHeight - px(406), px(416), px(320)};
    layout.notice = solved.Rect("notice");
    for (const uilayout::Node &node : solved.nodes)
    {
        if (!node.visible || node.type != "button")
            continue;
        const auto actionIt = kActions.find(node.action);
        if (actionIt == kActions.end())
            continue;
        if (actionIt->second == Act::ScrollLeft || actionIt->second == Act::ScrollRight)
            continue;
        Button button;
        button.r = node.rect;
        if (actionIt->second == Act::UnitsInfantry)
            button.r = {layout.cards.x, layout.cards.y - px(48), px(140), px(42)};
        if (actionIt->second == Act::UnitsVehicles)
            button.r = {layout.cards.x + px(148), layout.cards.y - px(48), px(140), px(42)};
        button.act = actionIt->second;
        button.label = _(node.label.c_str());
        button.active = node.active;
        button.disabled = node.disabled;
        button.fontScale = node.fontScale;
        button.style = node.style == "danger"    ? ui::ButtonStyle::Danger
                       : node.style == "primary" ? ui::ButtonStyle::Primary
                                                 : ui::ButtonStyle::Secondary;
        layout.buttons.push_back(std::move(button));
    }
    layout.cardsFit = kCardColumns * (kCardRows + 1);
    return layout;
}

UiRect TouchHUD::CardRect(const Layout &layout, int slot) const
{
    const auto scalePx = [&](int value) {
        return static_cast<int>(std::lround(value * layout.u));
    };
    const int gap = scalePx(kCardGap);
    const int width = (layout.cards.w - (kCardColumns - 1) * gap) / kCardColumns;
    const int height = (layout.cards.h - (kCardRows - 1) * gap) / kCardRows;
    const int scrollSlot = m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2);
    const int offset = static_cast<int>(m_scroll[scrollSlot]) % (height + gap);
    return {
        layout.cards.x + (slot % kCardColumns) * (width + gap),
        layout.cards.y + (slot / kCardColumns) * (height + gap) - offset,
        width,
        height};
}

int TouchHUD::FirstCard(const Layout &layout, int scrollSlot) const
{
    const int gap = static_cast<int>(std::lround(kCardGap * layout.u));
    const int step = (layout.cards.h - (kCardRows - 1) * gap) / kCardRows + gap;
    return kCardColumns * (static_cast<int>(m_scroll[scrollSlot]) / std::max(1, step));
}

float TouchHUD::MaxCardScroll(const Layout &layout, int tab) const
{
    const int gap = static_cast<int>(std::lround(kCardGap * layout.u));
    const int step = (layout.cards.h - (kCardRows - 1) * gap) / kCardRows + gap;
    return static_cast<float>(std::max(0, (ItemCountForTab(tab) + kCardColumns - 1) / kCardColumns - kCardRows) * step);
}

bool TouchHUD::OverPanels(const Layout &layout, int mouseX, int mouseY) const
{
    if (layout.resources.Contains(mouseX, mouseY) || layout.radar.Contains(mouseX, mouseY))
        return true;
    for (const Button &button : layout.buttons)
        if (button.r.Contains(mouseX, mouseY))
            return true;
    if (m_panel != Panel::None && !m_placing && layout.cards.Contains(mouseX, mouseY))
        return true;
    return false;
}

bool TouchHUD::IsOverHud(int mouseX, int mouseY, int windowWidth, int windowHeight) const
{
    const Layout layout
        = MakeLayout(windowWidth, windowHeight, m_lastHasCombat, m_lastHasBuilding, m_lastSelectedCount);
    return OverPanels(layout, mouseX, mouseY);
}

SDL_Rect TouchHUD::TouchScrollPanel(int windowWidth, int windowHeight) const
{
    if (m_panel == Panel::None || m_placing || IsResearchWindowOpen())
        return {0, 0, 0, 0};
    const auto layout = MakeLayout(windowWidth, windowHeight, m_lastHasCombat, m_lastHasBuilding, m_lastSelectedCount);
    return {layout.cards.x, layout.cards.y, layout.cards.w, layout.cards.h};
}

std::vector<SDL_Rect> TouchHUD::TouchPanels(int windowWidth, int windowHeight) const
{
    const Layout layout
        = MakeLayout(windowWidth, windowHeight, m_lastHasCombat, m_lastHasBuilding, m_lastSelectedCount);
    std::vector<SDL_Rect> panelRects;
    auto pushRect = [&](const UiRect &rect) {
        if (rect.w > 0 && rect.h > 0)
            panelRects.push_back({rect.x, rect.y, rect.w, rect.h});
    };
    pushRect(layout.resources);
    pushRect(layout.radar);
    for (const Button &button : layout.buttons)
        pushRect(button.r);
    if (m_panel != Panel::None && !m_placing)
        pushRect(layout.cards);
    if (m_placing && m_ghostRect.w > 0)
        panelRects.push_back(m_ghostRect);
    return panelRects;
}

void TouchHUD::UpdateGhostRect(const Camera &camera)
{
    const auto &blueprints = EntityManager::GetBlueprints();
    if (!m_placing || m_selectedBlueprint < 0 || m_selectedBlueprint >= static_cast<int>(blueprints.size()))
    {
        m_ghostRect = {0, 0, 0, 0};
        return;
    }
    const auto &blueprint = blueprints[static_cast<std::size_t>(m_selectedBlueprint)];
    float x0, y0, x1, y1;
    camera.WorldToScreen(static_cast<float>(m_ghostX * TILE_WIDTH), static_cast<float>(m_ghostY * TILE_HEIGHT), x0, y0);
    camera.WorldToScreen(
        static_cast<float>((m_ghostX + blueprint.width) * TILE_WIDTH),
        static_cast<float>((m_ghostY + blueprint.height) * TILE_HEIGHT),
        x1,
        y1);
    m_ghostRect = {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)};
}

void TouchHUD::StartPlacing(const Camera &camera)
{
    // What the cameo click started (the ghost of the desktop interface) becomes the touch placement
    m_buildMode = false;
    m_placing = true;
    m_panel = Panel::None;
    m_attackMoveMode = false;
    m_grab = m_fingerWas = m_grabMoved = m_grabPressedOnGhost = false;
    float worldX = 0.0f, worldY = 0.0f;
    camera.ScreenToWorld(camera.GetViewportWidth() * 0.5f, camera.GetViewportHeight() * 0.5f, worldX, worldY);
    const auto &blueprints = EntityManager::GetBlueprints();
    const auto &blueprint = blueprints[static_cast<std::size_t>(m_selectedBlueprint)];
    m_ghostX = static_cast<int>(std::floor(worldX / TILE_WIDTH - blueprint.width * 0.5f));
    m_ghostY = static_cast<int>(std::floor(worldY / TILE_HEIGHT - blueprint.height * 0.5f));
    UpdateGhostRect(camera);
}

bool TouchHUD::ConfirmPlacing(Economy &economy, const EntityManager &entities, const TileMap &map)
{
    const auto &blueprints = EntityManager::GetBlueprints();
    if (m_selectedBlueprint < 0 || m_selectedBlueprint >= static_cast<int>(blueprints.size()))
        return false;
    const BuildingBlueprint &blueprint = blueprints[static_cast<std::size_t>(m_selectedBlueprint)];
    const bool isMine = blueprint.mineKind >= 0;
    const bool canPlace
        = isMine
              ? entities.CanPlaceMine(blueprint.mineKind, m_ghostX, m_ghostY, map, Players::Local())
              : entities.CanPlaceBuilding(m_ghostX, m_ghostY, blueprint.width, blueprint.height, map, Players::Local());
    if (!isMine && !BuildLimits::CanBuildMore(entities, map, Players::Local(), blueprint.buildingIndex))
    {
        SoundRequests::Play("ui_error");
        SetNotification(
            BuildLimits::LimitOf(entities, blueprint.buildingIndex) == 0 ? _("THIS BUILDING IS NOT ALLOWED!")
                                                                         : _("BUILDING LIMIT REACHED!"));
        return false;
    }
    if (!economy.CanAfford(blueprint.creditCost))
    {
        SoundRequests::Announce("ann_insufficient_funds");
        SetNotification(_("INSUFFICIENT FUNDS!"));
        return false;
    }
    if (!canPlace)
    {
        SoundRequests::Announce("ann_cannot_build");
        SetNotification(_("CANNOT BUILD HERE!"));
        return false;
    }
    SimCommand command;
    command.type = CommandType::PlaceBuilding;
    command.a = blueprint.blueprintId;
    command.x = m_ghostX;
    command.y = m_ghostY;
    Issue(std::move(command));
    SoundRequests::Play("build_place");
    if (!blueprint.isWall && !isMine)
    {
        SoundRequests::Announce("ann_building");
        m_placing = false; // a building ends the placement; walls and mines are laid one after the other
    }
    return true;
}

void TouchHUD::SyncPanelWithTab()
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

void TouchHUD::Press(
    Act act,
    const Layout &layout,
    Economy &economy,
    EntityManager &entities,
    TileMap &map,
    Camera &camera,
    bool &outOpenMenu,
    bool &outOpenObjectives,
    std::vector<int> &selectedUnits)
{
    (void) layout;
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
            for (auto &entity : entities.GetAllEntities())
                entity.selected = false;
            m_attackMoveMode = false;
            break;
        case Act::SelectAll:
            selectedUnits.clear();
            for (auto &entity : entities.GetAllEntities())
            {
                const Unit *unit = entity.AsUnit();
                entity.selected
                    = unit && unit->alive && unit->state != ActionState::Die && unit->owner == Players::Local();
                if (entity.selected)
                    selectedUnits.push_back(entity.id);
            }
            break;
        case Act::Repair:
        {
            const auto ownedBuildings = OwnSelectedBuildings(entities, selectedUnits);
            if (ownedBuildings.empty())
                break;
            SimCommand command;
            command.type = CommandType::ToggleRepair;
            for (const Building *building : ownedBuildings)
                command.ids.push_back(static_cast<std::uint32_t>(building->id));
            Issue(std::move(command));
            break;
        }
        case Act::Sell:
        {
            const auto ownedBuildings = OwnSelectedBuildings(entities, selectedUnits);
            if (ownedBuildings.empty())
                break;
            if (m_destroyArmed <= 0.0f)
            {
                m_destroyArmed = 3.0f; // the second tap confirms
                break;
            }
            m_destroyArmed = 0.0f;
            SimCommand command;
            command.type = CommandType::Demolish;
            for (const Building *building : ownedBuildings)
                command.ids.push_back(static_cast<std::uint32_t>(building->id));
            selectedUnits.erase(
                std::remove_if(
                    selectedUnits.begin(),
                    selectedUnits.end(),
                    [&](int selectedId) {
                        return std::find(command.ids.begin(), command.ids.end(), static_cast<std::uint32_t>(selectedId))
                               != command.ids.end();
                    }),
                selectedUnits.end());
            Issue(std::move(command));
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
            const int scrollSlot = m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2);
            const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
            const float maxStart = MaxCardScroll(layout, tab);
            m_scroll[scrollSlot]
                = std::clamp(m_scroll[scrollSlot] + (act == Act::ScrollLeft ? -48.0f : 48.0f), 0.0f, maxStart);
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

bool TouchHUD::HandleHudClick(
    int mouseX,
    int mouseY,
    bool leftClicked,
    bool rightClicked,
    bool leftMouseDown,
    int wheelSteps,
    int windowWidth,
    int windowHeight,
    Economy &economy,
    EntityManager &entities,
    TileMap &map,
    Camera &camera,
    bool &outOpenMenu,
    bool &outOpenObjectives,
    std::vector<int> &selectedUnits)
{
    SyncPanelWithTab();
    if (m_destroyArmed > 0.0f && OwnSelectedBuildings(entities, selectedUnits).empty())
        m_destroyArmed = 0.0f;
    const bool hasCombat = HasCombatUnits(entities, selectedUnits);
    const bool hasBuilding = !OwnSelectedBuildings(entities, selectedUnits).empty();
    m_lastHasCombat = hasCombat;
    m_lastHasBuilding = hasBuilding;
    m_lastSelectedCount = static_cast<int>(selectedUnits.size());
    const Layout layout = MakeLayout(windowWidth, windowHeight, hasCombat, hasBuilding, m_lastSelectedCount);
    if (m_panel != Panel::None)
    {
        const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
        const int scrollSlot = m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2);
        const float maxStart = MaxCardScroll(layout, tab);
        m_scroll[scrollSlot] = std::clamp(m_scroll[scrollSlot], 0.0f, maxStart);
    }

    if (!leftMouseDown)
        m_radarHeld = false;

    // The radar: a tap or a drag on it puts the camera there (the zoom buttons on it are buttons)
    bool onButton = false;
    for (const Button &button : layout.buttons)
        onButton = onButton || button.r.Contains(mouseX, mouseY);
    if (layout.radar.Contains(mouseX, mouseY) && !onButton && (leftMouseDown || leftClicked))
        m_radarHeld = true;
    m_radarDragging = m_radarHeld && leftMouseDown; // a zoomed radar holds still while the camera follows the finger
    if (m_radarHeld)
    {
        const RadarView view
            = radarView(layout.radar, map.GetWidth(), map.GetHeight(), m_radarZoom, m_radarCenterX, m_radarCenterY);
        if (view.w > 0 && view.h > 0)
        {
            const float tileX = std::clamp(
                view.viewX + static_cast<float>(mouseX - view.ox) / view.scale,
                0.0f,
                static_cast<float>(map.GetWidth()));
            const float tileY = std::clamp(
                view.viewY + static_cast<float>(mouseY - view.oy) / view.scale,
                0.0f,
                static_cast<float>(map.GetHeight()));
            camera.CenterOn(tileX * TILE_WIDTH, tileY * TILE_HEIGHT);
        }
        return true;
    }

    if (!leftMouseDown)
        m_cardsTracking = m_cardsDragging = false;
    const bool onPanel
        = OverPanels(layout, mouseX, mouseY)
          || (leftMouseDown
              && (m_cardsTracking
                  || (m_panel != Panel::None && !m_placing && layout.cards.Contains(m_pressX, m_pressY))));
    UpdateGhostRect(camera);
    if (!m_placing)
        m_grab = m_fingerWas = m_grabMoved = m_grabPressedOnGhost = false;
    const bool pressOnGhost = !onPanel && m_pressX >= m_ghostRect.x && m_pressX < m_ghostRect.x + m_ghostRect.w
                              && m_pressY >= m_ghostRect.y && m_pressY < m_ghostRect.y + m_ghostRect.h;
    if (m_placing && (!onPanel || m_grab || (leftMouseDown && pressOnGhost)))
    {
        if (!leftMouseDown && leftClicked)
        {
            const auto &blueprints = EntityManager::GetBlueprints();
            float worldX = 0.0f, worldY = 0.0f;
            camera.ScreenToWorld(static_cast<float>(mouseX), static_cast<float>(mouseY), worldX, worldY);
            const int tileX = static_cast<int>(std::floor(worldX / TILE_WIDTH));
            const int tileY = static_cast<int>(std::floor(worldY / TILE_HEIGHT));
            if (m_selectedBlueprint >= 0 && m_selectedBlueprint < static_cast<int>(blueprints.size()))
            {
                const auto &blueprint = blueprints[static_cast<std::size_t>(m_selectedBlueprint)];
                const bool onGhost = tileX >= m_ghostX && tileX < m_ghostX + blueprint.width && tileY >= m_ghostY
                                     && tileY < m_ghostY + blueprint.height;
                const bool samePlace = m_fingerWas ? (m_grabPressedOnGhost && !m_grabMoved && m_ghostX == m_grabStartX
                                                      && m_ghostY == m_grabStartY)
                                                   : pressOnGhost;
                if (onGhost && samePlace)
                    ConfirmPlacing(economy, entities, map);
                else if (!m_grabMoved)
                {
                    m_ghostX = tileX - blueprint.width / 2;
                    m_ghostY = tileY - blueprint.height / 2;
                }
            }
            m_grab = m_fingerWas = m_grabMoved = m_grabPressedOnGhost = false;
        }
        else if (!leftMouseDown)
        {
            m_grab = m_fingerWas = m_grabMoved = m_grabPressedOnGhost = false;
        }
        else
        {
            float worldX = 0.0f, worldY = 0.0f;
            camera.ScreenToWorld(static_cast<float>(mouseX), static_cast<float>(mouseY), worldX, worldY);
            if (!m_fingerWas)
            {
                m_fingerWas = true;
                m_grabPressedOnGhost = pressOnGhost;
                m_grab = pressOnGhost;
                m_grabStartX = m_ghostX;
                m_grabStartY = m_ghostY;
                m_grabMoved = false;
                float pressWorldX = 0.0f, pressWorldY = 0.0f;
                camera.ScreenToWorld(
                    static_cast<float>(m_pressX), static_cast<float>(m_pressY), pressWorldX, pressWorldY);
                m_grabOffX = static_cast<float>(m_ghostX * TILE_WIDTH) - pressWorldX;
                m_grabOffY = static_cast<float>(m_ghostY * TILE_HEIGHT) - pressWorldY;
            }
            if (m_grab)
            {
                m_ghostX = static_cast<int>(std::lround((worldX + m_grabOffX) / TILE_WIDTH));
                m_ghostY = static_cast<int>(std::lround((worldY + m_grabOffY) / TILE_HEIGHT));
                if (m_ghostX != m_grabStartX || m_ghostY != m_grabStartY)
                    m_grabMoved = true;
                constexpr int edgeMargin = 56;
                constexpr float panStep = 9.0f;
                const float dx = mouseX < edgeMargin                               ? -panStep
                                 : mouseX > camera.GetViewportWidth() - edgeMargin ? panStep
                                                                                   : 0.0f;
                const float dy = mouseY < edgeMargin                                ? -panStep
                                 : mouseY > camera.GetViewportHeight() - edgeMargin ? panStep
                                                                                    : 0.0f;
                if (dx != 0.0f || dy != 0.0f)
                {
                    camera.Pan(dx, dy);
                    m_grabMoved = true;
                }
            }
        }
        return true;
    }
    if (!leftMouseDown)
        m_grab = m_fingerWas = m_grabMoved = m_grabPressedOnGhost = false;
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

    if (m_panel != Panel::None && !m_placing && leftMouseDown)
    {
        const int scrollSlot = m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2);
        const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
        if (!m_cardsTracking && layout.cards.Contains(m_pressX, m_pressY))
        {
            m_cardsTracking = true;
            m_cardsDragging = false;
            m_cardsStartScroll = m_scroll[scrollSlot];
        }
        if (m_cardsTracking && (m_cardsDragging || std::abs(mouseY - m_pressY) > 12))
        {
            m_cardsDragging = true;
            m_scroll[scrollSlot] = std::clamp(m_cardsStartScroll + m_pressY - mouseY, 0.0f, MaxCardScroll(layout, tab));
        }
    }
    else if (!leftMouseDown)
    {
        m_cardsTracking = m_cardsDragging = false;
    }
    if (wheelSteps && !m_cardsTracking && m_panel != Panel::None && layout.cards.Contains(mouseX, mouseY))
    {
        const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
        const int scrollSlot = m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2);
        const float maxStart = MaxCardScroll(layout, tab);
        m_scroll[scrollSlot] = std::clamp(m_scroll[scrollSlot] - wheelSteps * 48.0f * layout.u, 0.0f, maxStart);
    }
    if (rightClicked && m_panel == Panel::Units && layout.cards.Contains(mouseX, mouseY))
    {
        // Holding a unit card takes one order back
        for (int cardSlot = 0; cardSlot < layout.cardsFit; ++cardSlot)
        {
            if (!CardRect(layout, cardSlot).Contains(mouseX, mouseY))
                continue;
            const int itemIndex = ItemAt(m_unitTab, FirstCard(layout, m_unitTab == TabInfantry ? 1 : 2) + cardSlot);
            if (itemIndex >= 0)
                ActivateCell(m_unitTab, itemIndex, true, economy, entities, map, selectedUnits);
        }
        return true;
    }
    if (!leftClicked)
        return true;

    for (const Button &button : layout.buttons)
    {
        if (button.r.Contains(mouseX, mouseY) && !button.disabled)
        {
            Press(button.act, layout, economy, entities, map, camera, outOpenMenu, outOpenObjectives, selectedUnits);
            return true;
        }
    }
    if (m_panel != Panel::None && layout.cards.Contains(mouseX, mouseY))
    {
        const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
        const int firstIndex = FirstCard(layout, m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2));
        for (int cardSlot = 0; cardSlot < layout.cardsFit; ++cardSlot)
        {
            if (!CardRect(layout, cardSlot).Contains(mouseX, mouseY))
                continue;
            const int itemIndex = ItemAt(tab, firstIndex + cardSlot);
            if (itemIndex < 0)
                break;
            ActivateCell(tab, itemIndex, false, economy, entities, map, selectedUnits);
            if (m_buildMode)
                StartPlacing(camera);
            break;
        }
    }
    return true;
}

void TouchHUD::RenderGameHud(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const Economy &economy,
    const EntityManager &entities,
    const TileMap &map,
    const Camera &camera,
    int mouseX,
    int mouseY,
    int windowWidth,
    int windowHeight,
    const std::vector<int> &selected)
{
    const bool hasCombat = HasCombatUnits(entities, selected);
    const bool hasBuilding = !OwnSelectedBuildings(entities, selected).empty();
    m_lastHasCombat = hasCombat;
    m_lastHasBuilding = hasBuilding;
    m_lastSelectedCount = static_cast<int>(selected.size());
    const Layout layout = MakeLayout(windowWidth, windowHeight, hasCombat, hasBuilding, m_lastSelectedCount);
    const auto scalePx = [&](int value) {
        return static_cast<int>(std::lround(value * layout.u));
    };

    if (m_placing)
    {
        RenderBuildGhost(renderer, map, camera, m_ghostX, m_ghostY, economy, entities);
        UpdateGhostRect(camera);
    }

    // Credits and power
    {
        const UiRect &resourcesRect = layout.resources;
        const int padding = scalePx(8);
        const int barHeight = std::max(3, scalePx(5));
        renderer.FillRectScreen(resourcesRect.x, resourcesRect.y, resourcesRect.w, resourcesRect.h, {10, 13, 18, 210});
        renderer.DrawRectScreen(resourcesRect.x, resourcesRect.y, resourcesRect.w, resourcesRect.h, col::BorderStrong);
        fontRenderer.DrawString(
            "$ " + std::to_string(economy.GetCredits()),
            resourcesRect.x + padding,
            resourcesRect.y + scalePx(3),
            ui::FONT_H2,
            col::Accent,
            true);
        const int powerProduced = economy.GetPowerProduced();
        const int powerConsumed = economy.GetPowerConsumed();
        const bool hasDeficit = economy.HasPowerDeficit();
        const SDL_Color powerColor = hasDeficit ? col::Danger : col::Ok;
        const std::string powerText = std::to_string(powerConsumed) + "/" + std::to_string(powerProduced) + _(" kW");
        // The lightning of the game next to the figures; it blinks while the power is short
        if (!hasDeficit || PowerIconBlinkOn())
        {
            const int textWidth = fontRenderer.GetTextWidth(powerText, ui::FONT_SMALL_BOLD);
            renderer.DrawRawSpriteScreen(
                PowerIconSprite(),
                resourcesRect.Right() - padding - textWidth - scalePx(26),
                resourcesRect.y + scalePx(2),
                layout.u);
        }
        fontRenderer.DrawString(
            powerText,
            resourcesRect.Right() - padding,
            resourcesRect.y + scalePx(5),
            ui::FONT_SMALL_BOLD,
            powerColor,
            false,
            Anchor::RIGHT | Anchor::TOP);
        ui::DrawProgressBar(
            renderer,
            {resourcesRect.x + scalePx(6),
             resourcesRect.Bottom() - scalePx(4) - barHeight,
             resourcesRect.w - scalePx(12),
             barHeight},
            static_cast<float>(powerConsumed) / static_cast<float>(std::max(1, powerProduced)),
            powerColor);
    }

    // The radar (no zoom buttons: a tap puts the camera there)
    DrawRadar(renderer, fontRenderer, layout.radar, entities, map, camera, -1, -1, false);

    // Cards
    if (m_panel != Panel::None && !m_placing)
    {
        const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
        const int scrollSlot = m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2);
        const float maxStart = MaxCardScroll(layout, tab);
        m_scroll[scrollSlot] = std::clamp(m_scroll[scrollSlot], 0.0f, maxStart);
        const int firstIndex = FirstCard(layout, scrollSlot);
        const bool missingHq = !entities.HasBuildingType(UnitType::Headquarters, Players::Local(), true);
        const bool hasBarracks = entities.HasBuildingType(UnitType::Barracks, Players::Local());
        const bool hasFactory = entities.HasBuildingType(UnitType::Factory, Players::Local());
        const bool softClip = renderer.BeginClipTarget(
            {layout.cards.x, layout.cards.y, layout.cards.w, layout.cards.h}, {10, 13, 18, 255});
        if (!softClip)
            renderer.SetClipRect({layout.cards.x, layout.cards.y, layout.cards.w, layout.cards.h});
        for (int cardSlot = 0; cardSlot < layout.cardsFit; ++cardSlot)
        {
            const int itemIndex = ItemAt(tab, firstIndex + cardSlot);
            if (itemIndex < 0)
                break;
            const UiRect cardRect = CardRect(layout, cardSlot);
            const bool hovered = cardRect.Contains(mouseX, mouseY);
            if (tab == TabBuild)
            {
                const auto &blueprint = EntityManager::GetBlueprints()[static_cast<std::size_t>(itemIndex)];
                const bool isMine = blueprint.mineKind >= 0;
                const int limit = isMine ? Mines::Specs()[static_cast<std::size_t>(blueprint.mineKind)].maxPerPlayer
                                         : BuildLimits::LimitOf(entities, blueprint.buildingIndex);
                const int owned
                    = limit > 0
                          ? (isMine ? entities.CountMines(Players::Local(), blueprint.mineKind)
                                    : BuildLimits::CountOwned(entities, map, Players::Local(), blueprint.buildingIndex))
                          : 0;
                const bool locked = missingHq || limit == 0 || (limit > 0 && owned >= limit)
                                    || (blueprint.buildingIndex == 6 && !IsRadarUnlocked(Players::Local()));
                DrawCameo(
                    renderer, fontRenderer, cardRect, blueprint.iconCompId, _(blueprint.name), hovered, false, locked);
                DrawCostTag(
                    renderer,
                    fontRenderer,
                    cardRect,
                    "$" + std::to_string(blueprint.creditCost),
                    economy.CanAfford(blueprint.creditCost) ? col::Accent : col::Danger);
                if (limit > 0)
                    fontRenderer.DrawString(
                        std::to_string(owned) + "/" + std::to_string(limit),
                        cardRect.x + 6,
                        cardRect.y + 5,
                        ui::FONT_SMALL_BOLD,
                        owned >= limit ? col::Danger : col::TextMuted);
            }
            else
            {
                const bool isInfantry = tab == TabInfantry;
                const UnitCameoDef &unitDef = unitCatalog(isInfantry).items[itemIndex];
                const bool available
                    = (isInfantry ? hasBarracks : hasFactory) && IsUnitUnlocked(Players::Local(), unitDef.archetype);
                DrawCameo(
                    renderer,
                    fontRenderer,
                    cardRect,
                    SpriteHandle(unitDef.icon, SpriteKind::Part, "unit cameo"),
                    _(unitDef.name),
                    hovered,
                    false,
                    !available);
                const auto *unitConfig = entities.GetUnitConfig(unitDef.archetype);
                const int cost = unitConfig ? unitConfig->creditCost : unitDef.cost;
                DrawCostTag(
                    renderer,
                    fontRenderer,
                    cardRect,
                    "$" + std::to_string(cost),
                    economy.CanAfford(cost) ? col::Accent : col::Danger);
                int queued = 0;
                float progress = -1.0f;
                for (int buildingId :
                     ProductionTargets(isInfantry ? UnitType::Barracks : UnitType::Factory, entities, selected))
                {
                    const Building *building = entities.GetBuilding(buildingId);
                    if (!building)
                        continue;
                    for (const auto &queueItem : building->productionQueue)
                        queued += queueItem.archetype == unitDef.archetype ? 1 : 0;
                    if (!building->productionQueue.empty()
                        && building->productionQueue.front().archetype == unitDef.archetype)
                        progress = std::max(progress, building->CurrentProductionProgress());
                }
                if (available && progress >= 0.0f)
                    DrawProgressShutter(renderer, fontRenderer, cardRect, progress, col::Accent);
                if (queued > 0)
                    fontRenderer.DrawString(
                        "\xC3\x97" + std::to_string(queued),
                        cardRect.x + 6,
                        cardRect.y + 5,
                        ui::FONT_BODY_BOLD,
                        col::Accent,
                        true);
            }
        }
        if (softClip)
            renderer.EndClipTarget(200);
        else
            renderer.ResetClipRect();
        const int count = ItemCountForTab(tab);
        if (count == 0)
            fontRenderer.DrawString(
                _("NOTHING AVAILABLE"),
                layout.cards.CenterX(),
                layout.cards.CenterY(),
                ui::FONT_BODY,
                col::TextMuted,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
    }

    // Buttons
    for (const Button &button : layout.buttons)
    {
        ui::DrawButton(
            renderer,
            fontRenderer,
            button.r,
            button.label,
            button.active ? ui::ButtonStyle::Primary : button.style,
            button.r.Contains(mouseX, mouseY),
            !button.disabled,
            ui::ButtonFontForScale(button.fontScale),
            200);
    }

    // What is selected, over the orders (or above the open cards drawer so it never overlaps)
    if (!selected.empty() && !m_placing)
    {
        const std::string info = std::to_string(selected.size()) + " " + _("SELECTED");
        const int labelY = (m_panel != Panel::None ? layout.cards.y : windowHeight - scalePx(68)) - scalePx(22);
        fontRenderer.DrawString(info, layout.radar.Right() + scalePx(8), labelY, ui::FONT_SMALL_BOLD, col::Text, true);
    }

    // The line of text at the top: a notice, or what a mode wants from the next tap
    std::string notice = m_hudNotificationTimer > 0.0f ? m_hudNotification : std::string();
    if (notice.empty() && m_attackMoveMode)
        notice = _("TAP THE TARGET: ATTACK-MOVE");
    if (notice.empty() && m_placing)
        notice = _("TAP A LOCATION, THEN TAP THE BUILDING");
    if (!notice.empty())
    {
        const int maxWidth = std::max(120, windowWidth - scalePx(16));
        const int noticeWidth = std::min(maxWidth, fontRenderer.GetTextWidth(notice, ui::FONT_BODY_BOLD) + scalePx(24));
        const UiRect noticeRect {windowWidth / 2 - noticeWidth / 2, layout.notice.y, noticeWidth, layout.notice.h};
        renderer.FillRectScreen(noticeRect.x, noticeRect.y, noticeRect.w, noticeRect.h, {10, 13, 18, 220});
        renderer.DrawRectScreen(noticeRect.x, noticeRect.y, noticeRect.w, noticeRect.h, col::Accent);
        fontRenderer.DrawString(
            ui::FitText(fontRenderer, notice, ui::FONT_BODY_BOLD, std::max(40, noticeWidth - scalePx(16))),
            noticeRect.CenterX(),
            noticeRect.CenterY(),
            ui::FONT_BODY_BOLD,
            col::Accent,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
    }
    (void) mouseY;
}

} // namespace OpenArtOfWar
