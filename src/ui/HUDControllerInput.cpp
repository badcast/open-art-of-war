#include "ui/HUDController.hpp"
#include "HUDControllerInternal.hpp"
#include "graphics/SpriteRegistry.hpp"
#include "audio/SoundRequests.hpp"
#include "game/Players.hpp"
#include "game/BuildLimits.hpp"
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

void HUDController::ActivateCell(
    int tab,
    int index,
    bool cancel,
    Economy &economy,
    EntityManager &entities,
    const TileMap &map,
    std::vector<int> &selectedUnits)
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
            else if (
                index < static_cast<int>(blueprints.size()) && blueprints[index].mineKind < 0
                && !BuildLimits::CanBuildMore(entities, map, Players::Local(), blueprints[index].buildingIndex))
            {
                SoundRequests::Play("ui_error");
                SetNotification(
                    BuildLimits::LimitOf(entities, blueprints[index].buildingIndex) == 0
                        ? _("THIS BUILDING IS NOT ALLOWED!")
                        : _("BUILDING LIMIT REACHED!"));
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
    const bool pressed = leftMouseDown && !m_prevLeftDown;
    m_prevLeftDown = leftMouseDown;
    if (!leftMouseDown)
        m_radarDragging = false;
    // A radar drag owns the mouse until the button is released, also over the map (no box selection)
    if (m_radarDragging)
        return true;

    SidebarLayout l = sidebarLayout(windowW, windowH, m_topInset, !m_observer && m_speedEnabled);
    const int gridRows = (ItemCountForTab(m_activeTab) + SidebarLayout::kCols - 1) / SidebarLayout::kCols;
    const int gridContentH
        = gridRows > 0 ? gridRows * (SidebarLayout::kCellH + SidebarLayout::kGap) - SidebarLayout::kGap : 0;
    const float maxScrollPx = std::max(0.0f, static_cast<float>(gridContentH - l.grid.h));
    ui::UpdateSmoothList(
        m_gridScroll[m_activeTab],
        l.grid,
        {},
        gridContentH,
        SidebarLayout::kCellH + SidebarLayout::kGap,
        mx,
        my,
        leftMouseDown,
        pressed,
        m_gridPressX,
        m_gridPressY,
        wheelSteps,
        leftClicked,
        false);
    if (m_gridScroll[m_activeTab].dragging)
        return true;
    if (!IsOverSidebar(mx, windowW))
        return false;

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

    if (!m_observer && rightClicked && l.grid.Contains(mx, my))
    {
        int count = ItemCountForTab(m_activeTab);
        const int scrollPx = m_gridScroll[m_activeTab].PixelOffset();
        for (int i = 0; i < count; ++i)
        {
            UiRect c = l.Cell(i, scrollPx);
            if (c.Contains(mx, my))
            {
                ActivateCell(m_activeTab, ItemAt(m_activeTab, i), true, economy, entities, map, selectedUnits);
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
            c.ids.push_back(static_cast<std::uint32_t>(b->id));
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
            c.ids.push_back(static_cast<std::uint32_t>(b->id));
        // They are gone on the tick the command runs: out of the selection already
        selectedUnits.erase(
            std::remove_if(
                selectedUnits.begin(),
                selectedUnits.end(),
                [&](int id) {
                    return std::find(c.ids.begin(), c.ids.end(), static_cast<std::uint32_t>(id)) != c.ids.end();
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
    if (!m_observer && m_speedEnabled && m_onSpeedStep)
    {
        if (l.slowerBtn.Contains(mx, my))
        {
            m_onSpeedStep(-1);
            return true;
        }
        if (l.speedBtn.Contains(mx, my))
        {
            m_onSpeedStep(0);
            return true;
        }
        if (l.fasterBtn.Contains(mx, my))
        {
            m_onSpeedStep(1);
            return true;
        }
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
    if (maxScrollPx > 0.0f && l.scrollUp.Contains(mx, my))
    {
        m_gridScroll[m_activeTab].velocity = 0.0f;
        m_gridScroll[m_activeTab].target = std::clamp(
            m_gridScroll[m_activeTab].target - static_cast<float>(SidebarLayout::kCellH + SidebarLayout::kGap),
            0.0f,
            maxScrollPx);
        return true;
    }
    if (maxScrollPx > 0.0f && l.scrollDown.Contains(mx, my))
    {
        m_gridScroll[m_activeTab].velocity = 0.0f;
        m_gridScroll[m_activeTab].target = std::clamp(
            m_gridScroll[m_activeTab].target + static_cast<float>(SidebarLayout::kCellH + SidebarLayout::kGap),
            0.0f,
            maxScrollPx);
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
        const int scrollPx = m_gridScroll[m_activeTab].PixelOffset();
        for (int i = 0; i < count; ++i)
        {
            UiRect c = l.Cell(i, scrollPx);
            if (c.Contains(mx, my))
            {
                if ((m_activeTab == TabInfantry || m_activeTab == TabVehicles) && InfinityBtn(c).Contains(mx, my))
                {
                    const bool inf = m_activeTab == TabInfantry;
                    const int archetype = unitCatalog(inf).items[ItemAt(m_activeTab, i)].archetype;
                    if (IsUnitUnlocked(Players::Local(), archetype))
                        ToggleRepeat(archetype, inf ? UnitType::Barracks : UnitType::Factory, entities, selectedUnits);
                    break;
                }
                ActivateCell(m_activeTab, ItemAt(m_activeTab, i), false, economy, entities, map, selectedUnits);
                break;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

} // namespace OpenArtOfWar
