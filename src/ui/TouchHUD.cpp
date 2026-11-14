#include "ui/TouchHUD.hpp"
#include "HUDControllerInternal.hpp"

#include "core/Settings.hpp"
#include "game/Mines.hpp"
#include "game/Simulation.hpp"
#include "graphics/SpriteRegistry.hpp"
#include "ui/UiLayout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>

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
    const uilayout::Conditions conditions = [&](std::string_view name) {
        if (name == "placing")
            return m_placing;
        if (name == "tactical")
            return m_tactical;
        if (name == "observer")
            return m_observer;
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
        if (name == "stats_open")
            return m_statsOpen;
        return false;
    };
    static const std::map<std::string, Act> kActions
        = {{"menu", Act::Menu},
           {"goals", Act::Goals},
           {"stats", Act::Stats},
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
    const int offset = m_cardScroll[scrollSlot].PixelOffset() % (height + gap);
    return {
        layout.cards.x + (slot % kCardColumns) * (width + gap),
        layout.cards.y + (slot / kCardColumns) * (height + gap) - offset,
        width,
        height};
}

int TouchHUD::PlacingBuildingIndex() const
{
    const auto &blueprints = EntityManager::GetBlueprints();
    if (!(m_placing || m_buildMode) || m_selectedBlueprint < 0
        || m_selectedBlueprint >= static_cast<int>(blueprints.size()))
        return -1;
    return blueprints[static_cast<std::size_t>(m_selectedBlueprint)].buildingIndex;
}

SDL_Rect TouchHUD::TutorialRect(TutorialTarget target, int arg, int windowWidth, int windowHeight) const
{
    const Layout layout = MakeLayout(windowWidth, windowHeight, m_lastHasCombat, m_lastHasBuilding, m_lastSelectedCount);
    const auto toSdl = [](const UiRect &r) {
        return SDL_Rect {r.x, r.y, r.w, r.h};
    };
    const auto button = [&](Act act) {
        for (const Button &b : layout.buttons)
        {
            if (b.act == act)
                return toSdl(b.r);
        }
        return SDL_Rect {0, 0, 0, 0};
    };
    // The visible card of a tab whose item passes `match` (the card area when it is scrolled away)
    const auto card = [&](int tab, int scrollSlot, auto match) {
        const int first = FirstCard(layout, scrollSlot);
        for (int slot = 0; slot < layout.cardsFit; ++slot)
        {
            const int item = ItemAt(tab, first + slot);
            if (item < 0)
                break;
            if (!match(item))
                continue;
            const UiRect r = CardRect(layout, slot);
            if (r.y >= layout.cards.y && r.Bottom() <= layout.cards.Bottom())
                return toSdl(r);
        }
        return toSdl(layout.cards);
    };
    switch (target)
    {
        case TutorialTarget::Radar:
            return toSdl(layout.radar);
        case TutorialTarget::Resources:
            return toSdl(layout.resources);
        case TutorialTarget::BuildTab:
            return button(Act::DockBuild);
        case TutorialTarget::BuildCard:
            if (m_placing) // another building is being placed (the wall): it ends first
                return button(Act::Cancel);
            if (m_panel != Panel::Build)
                return button(Act::DockBuild);
            return card(TabBuild, 0, [&](int item) {
                return EntityManager::GetBlueprints()[static_cast<std::size_t>(item)].buildingIndex == arg;
            });
        case TutorialTarget::UnitCard:
        {
            const int wanted = arg == 1 ? TabVehicles : TabInfantry;
            if (m_panel != Panel::Units)
                return button(Act::DockUnits);
            if (m_unitTab != wanted)
                return button(wanted == TabVehicles ? Act::UnitsVehicles : Act::UnitsInfantry);
            return card(m_unitTab, m_unitTab == TabInfantry ? 1 : 2, [](int) { return true; });
        }
        case TutorialTarget::PlaceConfirm:
            return button(Act::Confirm);
        case TutorialTarget::ResearchButton:
            return button(Act::Tech);
        case TutorialTarget::ObjectivesButton:
            return button(Act::Goals);
        case TutorialTarget::None:
            break;
    }
    return {0, 0, 0, 0};
}

int TouchHUD::FirstCard(const Layout &layout, int scrollSlot) const
{
    const int gap = static_cast<int>(std::lround(kCardGap * layout.u));
    const int step = (layout.cards.h - (kCardRows - 1) * gap) / kCardRows + gap;
    return kCardColumns * (m_cardScroll[scrollSlot].PixelOffset() / std::max(1, step));
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

void TouchHUD::FocusBuilding(int buildingId, const EntityManager &entities)
{
    const Building *b = entities.GetBuilding(buildingId);
    if (!b || b->owner != Players::Local())
        return;
    m_buildMode = false;
    m_placing = false;
    switch (b->type)
    {
        case UnitType::Barracks:
            m_primaryBarracksId = b->id;
            SetActiveTab(TabInfantry);
            m_seenTab = TabInfantry;
            m_unitTab = TabInfantry;
            m_panel = Panel::Units;
            break;
        case UnitType::Factory:
            m_primaryFactoryId = b->id;
            SetActiveTab(TabVehicles);
            m_seenTab = TabVehicles;
            m_unitTab = TabVehicles;
            m_panel = Panel::Units;
            break;
        case UnitType::Headquarters:
            if (m_tactical)
            {
                m_panel = Panel::None;
                break;
            }
            SetActiveTab(TabBuild);
            m_seenTab = TabBuild;
            m_panel = Panel::Build;
            break;
        case UnitType::ResearchLab:
            OpenResearchWindow();
            break;
        default:
            break;
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
        case Act::Stats:
            m_statsToggleRequested = true;
            break;
        case Act::Tech:
            OpenResearchWindow();
            break;
        case Act::DockBuild:
            if (m_tactical)
                break;
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
            if (m_unitTab != TabInfantry)
                m_cardScroll[1].Reset();
            m_unitTab = m_activeTab = TabInfantry;
            break;
        case Act::UnitsVehicles:
            if (m_unitTab != TabVehicles)
                m_cardScroll[2].Reset();
            m_unitTab = m_activeTab = TabVehicles;
            break;
        case Act::ScrollLeft:
        case Act::ScrollRight:
        {
            const int scrollSlot = m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2);
            const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
            const float maxStart = MaxCardScroll(layout, tab);
            m_cardScroll[scrollSlot].StopGlide();
            m_cardScroll[scrollSlot].target = std::clamp(
                m_cardScroll[scrollSlot].target + (act == Act::ScrollLeft ? -72.0f : 72.0f) * layout.u, 0.0f, maxStart);
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
    if (m_tactical && (m_panel == Panel::Build || m_placing))
    {
        // The tactical view has no building: the build panel and a ghost on the map go away
        m_panel = Panel::None;
        m_placing = false;
        m_buildMode = false;
        if (m_activeTab == TabBuild)
            m_activeTab = m_unitTab;
    }
    SyncPanelWithTab();
    if (m_destroyArmed > 0.0f && OwnSelectedBuildings(entities, selectedUnits).empty())
        m_destroyArmed = 0.0f;
    const bool hasCombat = HasCombatUnits(entities, selectedUnits);
    const bool hasBuilding = !OwnSelectedBuildings(entities, selectedUnits).empty();
    m_lastHasCombat = hasCombat;
    m_lastHasBuilding = hasBuilding;
    m_lastSelectedCount = static_cast<int>(selectedUnits.size());
    const bool pressed = leftMouseDown && !m_prevLeftDown;
    m_prevLeftDown = leftMouseDown;
    const Layout layout = MakeLayout(windowWidth, windowHeight, hasCombat, hasBuilding, m_lastSelectedCount);

    if (!leftMouseDown)
        m_radarHeld = false;

    // The radar: a tap or a drag on it puts the camera there (the zoom buttons on it are buttons)
    bool onButton = false;
    for (const Button &button : layout.buttons)
        onButton = onButton || button.r.Contains(mouseX, mouseY);
    if (layout.radar.Contains(mouseX, mouseY) && layout.radar.Contains(m_pressX, m_pressY) && !onButton
        && (leftMouseDown || leftClicked))
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

    const int activeSlot = m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2);
    if (m_panel != Panel::None && !m_placing)
    {
        const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
        const int contentH = layout.cards.h + static_cast<int>(std::lround(MaxCardScroll(layout, tab)));
        const int stepPx = std::max(48, static_cast<int>(std::lround(72.0f * layout.u)));
        ui::UpdateSmoothList(
            m_cardScroll[activeSlot],
            layout.cards,
            {},
            contentH,
            stepPx,
            mouseX,
            mouseY,
            leftMouseDown,
            pressed,
            m_pressX,
            m_pressY,
            wheelSteps,
            leftClicked,
            false);
    }
    const bool onPanel
        = OverPanels(layout, mouseX, mouseY)
          || (leftMouseDown
              && (m_cardScroll[activeSlot].dragging
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
        // Let a tap on an own building reach GameplayController without closing the open cards.
        // Empty ground still closes them and clears a building selection there.
        if (leftClicked && m_panel != Panel::None)
        {
            float worldX = 0.0f, worldY = 0.0f;
            camera.ScreenToWorld(static_cast<float>(mouseX), static_cast<float>(mouseY), worldX, worldY);
            const Building *tappedBuilding = entities.GetBuilding(entities.GetUnitAt(worldX, worldY));
            const bool selectingOwnBuilding
                = tappedBuilding && tappedBuilding->alive && tappedBuilding->owner == Players::Local();
            if (!selectingOwnBuilding)
            {
                m_panel = Panel::None;
                if (!hasBuilding)
                    return true;
            }
        }
        return false;
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

int TouchHUD::IconSpriteForAction(Act act)
{
    static const std::pair<Act, CachedSprite> kIcons[] = {
        {Act::DockBuild, CachedSprite("ui_touch.build", SpriteKind::Frame)},
        {Act::DockUnits, CachedSprite("ui_touch.units", SpriteKind::Frame)},
        {Act::Stats, CachedSprite("ui_touch.stats", SpriteKind::Frame)},
        {Act::Tech, CachedSprite("ui_touch.tech", SpriteKind::Frame)},
        {Act::Goals, CachedSprite("ui_touch.goals", SpriteKind::Frame)},
        {Act::Menu, CachedSprite("ui_touch.menu", SpriteKind::Frame)},
        {Act::AttackMove, CachedSprite("ui_touch.attack", SpriteKind::Frame)},
        {Act::Stop, CachedSprite("ui_touch.stop", SpriteKind::Frame)},
        {Act::Hold, CachedSprite("ui_touch.hold", SpriteKind::Frame)},
        {Act::Siege, CachedSprite("ui_touch.siege", SpriteKind::Frame)},
        {Act::Repair, CachedSprite("ui_touch.repair", SpriteKind::Frame)},
        {Act::Sell, CachedSprite("ui_touch.sell", SpriteKind::Frame)},
        {Act::ClearSel, CachedSprite("ui_touch.clear", SpriteKind::Frame)},
        {Act::SelectAll, CachedSprite("ui_touch.all", SpriteKind::Frame)},
        {Act::Confirm, CachedSprite("ui_touch.confirm", SpriteKind::Frame)},
        {Act::Cancel, CachedSprite("ui_touch.cancel", SpriteKind::Frame)},
        {Act::UnitsInfantry, CachedSprite("ui_touch.infantry", SpriteKind::Frame)},
        {Act::UnitsVehicles, CachedSprite("ui_touch.vehicles", SpriteKind::Frame)},
        {Act::RadarZoomIn, CachedSprite("ui_touch.zoom_in", SpriteKind::Frame)},
        {Act::RadarZoomOut, CachedSprite("ui_touch.zoom_out", SpriteKind::Frame)},
    };
    for (const auto &entry : kIcons)
    {
        if (entry.first == act)
            return entry.second.Get();
    }
    return -1;
}

TouchHUD::ButtonColors TouchHUD::ResolveButtonColors(
    ui::ButtonStyle style, bool active, bool hovered, bool disabled, bool armed, SDL_Color factionColor)
{
    const float luma = 0.299f * factionColor.r + 0.587f * factionColor.g + 0.114f * factionColor.b;
    const SDL_Color rich = (luma > 165.0f) ? ui::Mix(factionColor, col::PanelSunken, 0.32f) : factionColor;
    const ui::ButtonStyle effectiveStyle = active ? ui::ButtonStyle::Primary : style;

    ButtonColors c;
    c.activeBar = ui::WithAlpha(ui::Mix(factionColor, col::White, 0.65f), 245);
    c.iconTint = col::White;
    c.iconAlpha = disabled ? Uint8(115) : kHudIconAlpha;
    c.text = disabled ? ui::WithAlpha(col::White, 130) : ui::WithAlpha(col::White, 240);

    if (disabled)
    {
        c.fill = ui::WithAlpha(ui::Mix(rich, col::PanelSunken, 0.72f), 140);
        c.border = ui::WithAlpha(ui::Mix(rich, col::Border, 0.50f), 140);
    }
    else if (effectiveStyle == ui::ButtonStyle::Danger)
    {
        c.fill = (armed || hovered) ? SDL_Color {186, 42, 34, 224} : SDL_Color {142, 34, 28, 200};
        c.border = (armed || hovered) ? SDL_Color {255, 118, 106, 242} : SDL_Color {226, 86, 76, 215};
    }
    else if (effectiveStyle == ui::ButtonStyle::Primary)
    {
        c.fill = ui::WithAlpha(hovered ? ui::Mix(rich, col::White, 0.14f) : ui::Mix(rich, col::PanelSunken, 0.10f), 224);
        c.border = ui::WithAlpha(ui::Mix(factionColor, col::White, hovered ? 0.72f : 0.50f), 245);
    }
    else
    {
        c.fill = ui::WithAlpha(
            ui::Mix(rich, col::PanelSunken, hovered ? 0.24f : 0.42f), hovered ? Uint8(215) : Uint8(195));
        c.border = ui::WithAlpha(
            ui::Mix(factionColor, col::White, hovered ? 0.55f : 0.35f), hovered ? Uint8(240) : Uint8(215));
    }
    return c;
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
    const auto selectedBuildings = OwnSelectedBuildings(entities, selected);
    const bool hasBuilding = !selectedBuildings.empty();
    const bool hasProducer = std::any_of(
        selectedBuildings.begin(), selectedBuildings.end(), [](const Building *b) { return b && b->IsUnitProducer(); });
    m_lastHasCombat = hasCombat;
    m_lastHasBuilding = hasBuilding;
    m_lastHasProducer = hasProducer;
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

    const SDL_Color armyColor = []() -> SDL_Color {
        const int colorIdx = Players::Slot(Players::Local()).color;
        if (colorIdx >= 1 && colorIdx <= Settings::kArmyColors)
            return ArmyColor(colorIdx);
        const Faction f = Players::Slot(Players::Local()).faction;
        if (f == Faction::Confederation)
            return ArmyColor(1);
        if (f == Faction::Resistance)
            return ArmyColor(2);
        return col::Accent;
    }();

    // Credits and power (TouchHUD flat tactical slate plate matching touch buttons); an observer has the bar instead
    if (!m_observer)
    {
        const UiRect &resourcesRect = layout.resources;
        const int padX = scalePx(10);
        const int stripeW = std::max(2, scalePx(3));
        const int barHeight = std::max(4, scalePx(6));

        const int powerProduced = economy.GetPowerProduced();
        const int powerConsumed = economy.GetPowerConsumed();
        const bool hasDeficit = economy.HasPowerDeficit();
        const SDL_Color powerColor = hasDeficit ? col::Danger : col::Ok;
        const SDL_Color borderColor = (hasDeficit && PowerIconBlinkOn()) ? col::Danger : SDL_Color {160, 172, 190, 145};

        renderer.FillRectScreen(resourcesRect.x, resourcesRect.y, resourcesRect.w, resourcesRect.h, {12, 16, 22, 165});
        renderer.DrawRectScreen(resourcesRect.x, resourcesRect.y, resourcesRect.w, resourcesRect.h, borderColor);
        if (resourcesRect.h > 2)
            renderer.FillRectScreen(
                resourcesRect.x + 1, resourcesRect.y + 1, stripeW, resourcesRect.h - 2, ui::WithAlpha(armyColor, 240));

        const int contentLeft = resourcesRect.x + 1 + stripeW + padX;
        const int dividerX
            = resourcesRect.x + 1 + stripeW + std::max(scalePx(102), (resourcesRect.w - stripeW) * 43 / 100);
        if (resourcesRect.h > scalePx(14))
            renderer.FillRectScreen(
                dividerX, resourcesRect.y + scalePx(7), 1, resourcesRect.h - scalePx(14), {160, 172, 190, 75});

        // Left compartment: Credits
        const std::string amountText = std::to_string(economy.GetCredits());
        const int maxCreditsW = std::max(20, dividerX - padX - contentLeft);
        const int dollarW = fontRenderer.GetTextWidth("$", ui::FONT_H2);
        const int dollarGap = scalePx(4);
        const auto amountFont
            = (dollarW + dollarGap + fontRenderer.GetTextWidth(amountText, ui::FONT_H2) <= maxCreditsW) ? ui::FONT_H2
                                                                                                        : ui::FONT_BODY;
        fontRenderer.DrawString(
            "$",
            contentLeft,
            resourcesRect.CenterY(),
            ui::FONT_H2,
            col::White,
            true,
            Anchor::LEFT | Anchor::VCENTER);
        fontRenderer.DrawString(
            amountText,
            contentLeft + dollarW + dollarGap,
            resourcesRect.CenterY(),
            amountFont,
            col::White,
            true,
            Anchor::LEFT | Anchor::VCENTER);

        // Right compartment: Power readout + bar
        const int powerLeft = dividerX + padX;
        const int powerRight = resourcesRect.Right() - padX;
        const int powerW = std::max(16, powerRight - powerLeft);
        const std::string powerText = std::to_string(powerConsumed) + "/" + std::to_string(powerProduced) + _(" kW");

        if (!hasDeficit || PowerIconBlinkOn())
        {
            renderer.DrawRawSpriteScreen(PowerIconSprite(), powerLeft, resourcesRect.y + scalePx(3), layout.u);
        }
        fontRenderer.DrawString(
            powerText,
            powerRight,
            resourcesRect.y + scalePx(6),
            ui::FONT_SMALL_BOLD,
            powerColor,
            true,
            Anchor::RIGHT | Anchor::TOP);

        const int barY = resourcesRect.Bottom() - scalePx(7) - barHeight;
        renderer.FillRectScreen(powerLeft, barY, powerW, barHeight, {8, 11, 16, 220});
        const float ratio = std::clamp(
            static_cast<float>(powerConsumed) / static_cast<float>(std::max(1, powerProduced)), 0.0f, 1.0f);
        const int innerW = std::max(0, powerW - 2);
        const int fillW = static_cast<int>(std::lround(innerW * ratio));
        if (fillW > 0 && barHeight > 2)
        {
            renderer.FillRectScreen(powerLeft + 1, barY + 1, fillW, barHeight - 2, powerColor);
            renderer.FillRectScreen(powerLeft + 1, barY + 1, fillW, 1, {255, 255, 255, 60});
        }
        renderer.DrawRectScreen(powerLeft, barY, powerW, barHeight, {160, 172, 190, 95});
    }

    // The radar (no zoom buttons: a tap puts the camera there)
    DrawRadar(renderer, fontRenderer, layout.radar, entities, map, camera, -1, -1, false);

    // Cards
    if (m_panel != Panel::None && !m_placing)
    {
        const int tab = m_panel == Panel::Build ? static_cast<int>(TabBuild) : m_unitTab;
        const int scrollSlot = m_panel == Panel::Build ? 0 : (m_unitTab == TabInfantry ? 1 : 2);
        const float maxStart = MaxCardScroll(layout, tab);
        m_cardScroll[scrollSlot].offset = std::clamp(m_cardScroll[scrollSlot].offset, 0.0f, maxStart);
        m_cardScroll[scrollSlot].target = std::clamp(m_cardScroll[scrollSlot].target, 0.0f, maxStart);
        const int firstIndex = FirstCard(layout, scrollSlot);
        const bool missingHq = !entities.HasBuildingType(UnitType::Headquarters, Players::Local(), true);
        const bool hasBarracks = entities.HasBuildingType(UnitType::Barracks, Players::Local());
        const bool hasFactory = entities.HasBuildingType(UnitType::Factory, Players::Local());
        const bool softClip = renderer.BeginClipTarget(
            {layout.cards.x, layout.cards.y, layout.cards.w, layout.cards.h}, {12, 16, 22, 200});
        if (!softClip)
        {
            renderer.FillRectScreen(layout.cards.x, layout.cards.y, layout.cards.w, layout.cards.h, {12, 16, 22, 200});
            renderer.SetClipRect({layout.cards.x, layout.cards.y, layout.cards.w, layout.cards.h});
        }
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
                    economy.CanAfford(blueprint.creditCost) ? col::White : col::Danger);
                if (blueprint.powerCost != 0)
                    DrawPowerTag(renderer, fontRenderer, cardRect, blueprint.powerCost, PowerIconSprite());
                if (limit > 0)
                {
                    const int tagHeight = std::clamp(cardRect.h * 18 / 84, 15, 28);
                    fontRenderer.DrawString(
                        std::to_string(owned) + "/" + std::to_string(limit),
                        cardRect.Right() - 6,
                        cardRect.y + tagHeight + 5,
                        ui::FONT_SMALL_BOLD,
                        owned >= limit ? col::Danger : col::TextMuted,
                        true,
                        Anchor::RIGHT | Anchor::TOP);
                }
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
                    economy.CanAfford(cost) ? col::White : col::Danger);
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
            renderer.EndClipTarget(255);
        else
            renderer.ResetClipRect();
        renderer.DrawRectScreen(layout.cards.x, layout.cards.y, layout.cards.w, layout.cards.h, {160, 172, 190, 145});
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

    // Buttons: faction-adapted plates with crisp white icons at opacity 200 (building/unit cameos stay untouched above)
    for (const Button &button : layout.buttons)
    {
        const bool hovered = button.r.Contains(mouseX, mouseY);
        if (hovered && !button.disabled)
            SoundRequests::MarkHover();

        const bool armed = (button.act == Act::Sell && m_destroyArmed > 0.0f);
        const ButtonColors colors
            = ResolveButtonColors(button.style, button.active, hovered, button.disabled, armed, armyColor);

        renderer.FillRectScreen(button.r.x, button.r.y, button.r.w, button.r.h, colors.fill);
        renderer.DrawRectScreen(button.r.x, button.r.y, button.r.w, button.r.h, colors.border);
        if (!button.disabled && button.active && button.r.h > 4)
            renderer.FillRectScreen(
                button.r.x + 1, button.r.Bottom() - 3, std::max(0, button.r.w - 2), 2, colors.activeBar);

        const Act iconAct = armed ? Act::Confirm : button.act;
        const int iconId = IconSpriteForAction(iconAct);
        if (iconId < 0)
        {
            const std::string shown = ui::FitText(
                fontRenderer,
                button.label,
                ui::ButtonFontForScale(button.fontScale),
                std::max(16, button.r.w - scalePx(8)));
            fontRenderer.DrawString(
                shown,
                button.r.CenterX(),
                button.r.CenterY(),
                ui::ButtonFontForScale(button.fontScale),
                colors.text,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
            continue;
        }

        const int iconSz = std::max(16, static_cast<int>(std::lround(std::min(button.r.w, button.r.h) * 0.60f)));
        renderer.DrawSpritePartScreen(
            iconId,
            {static_cast<float>(button.r.CenterX() - iconSz / 2),
             static_cast<float>(button.r.CenterY() - iconSz / 2),
             static_cast<float>(iconSz),
             static_cast<float>(iconSz)},
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            SDL_FLIP_NONE,
            Faction::Resistance,
            colors.iconAlpha,
            colors.iconTint);
    }

    // What is selected, over the orders (or above the open cards drawer so it never overlaps)
    if (!selected.empty() && !m_placing)
    {
        const std::string info = std::to_string(selected.size()) + " " + _("SELECTED");
        const int labelY = (m_panel != Panel::None ? layout.cards.y : windowHeight - scalePx(68)) - scalePx(22);
        // At the bottom, in the middle of the free span between the radar and the right edge
        fontRenderer.DrawString(
            info,
            (layout.radar.Right() + windowWidth) / 2,
            labelY,
            ui::FONT_SMALL_BOLD,
            col::Text,
            true,
            Anchor::HCENTER | Anchor::VCENTER);
    }

    // The timed notification slides in from the left; what a mode wants from the next tap stands in the same place
    const int noticeY = m_observer ? std::max(layout.notice.y, m_observerBarHeight + scalePx(8)) : layout.notice.y;
    DrawHudToast(renderer, fontRenderer, scalePx(8), noticeY, layout.notice.h, std::max(120, windowWidth * 2 / 3));
    std::string notice;
    const bool toastShown = m_hudNotificationTimer > 0.0f && !m_hudNotification.empty();
    if (!toastShown && m_attackMoveMode)
        notice = _("TAP THE TARGET: ATTACK-MOVE");
    if (!toastShown && notice.empty() && m_placing)
        notice = _("TAP A LOCATION, THEN TAP THE BUILDING");
    if (!toastShown && notice.empty() && !hasCombat && m_lastHasProducer && !m_statsOpen && !IsResearchWindowOpen())
        notice = _("DOUBLE-TAP MAP: SET RALLY POINT");
    if (!notice.empty())
    {
        ui::DrawToast(
            renderer,
            fontRenderer,
            scalePx(8),
            noticeY,
            layout.notice.h,
            std::max(120, windowWidth * 2 / 3),
            notice,
            1.0f,
            col::Accent);
    }
    (void) mouseY;
}

} // namespace OpenArtOfWar
