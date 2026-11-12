#include "ui/HUDController.hpp"
#include "HUDControllerInternal.hpp"
#include "graphics/SpriteRegistry.hpp"
#include "audio/SoundRequests.hpp"
#include "game/Players.hpp"
#include "game/BuildLimits.hpp"
#include "game/Capabilities.hpp"
#include "ui/UiKit.hpp"
#include "core/Localization.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/ShaderEffects.hpp"
#include "graphics/Camera.hpp"
#include "graphics/TextureManager.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include "game/Mines.hpp"
#include "game/Economy.hpp"
#include "game/Research.hpp"
#include "graphics/RenderUtility.hpp"
#include "game/Sides.hpp"
#include <algorithm>
#include <cmath>

namespace OpenArtOfWar
{

namespace
{

constexpr int kCameoH = 38;

} // namespace

int HUDController::PowerIconSprite()
{
    static const CachedSprite sprite("ui_power_icon.icon", SpriteKind::Frame);
    return sprite;
}

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
                    return SpriteHandle(cat.items[i].icon, SpriteKind::Part, "unit cameo");
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

namespace
{

// Catalog entries of a tab, before the tech level
int CatalogCount(int tab)
{
    switch (tab)
    {
        case HUDController::TabBuild:
            return static_cast<int>(EntityManager::GetBlueprints().size());
        case HUDController::TabInfantry:
            return unitCatalog(true).count;
        case HUDController::TabVehicles:
            return unitCatalog(false).count;
        case HUDController::TabOrders:
            return static_cast<int>(std::size(kOrders));
    }
    return 0;
}

// The local player's tech level leaves nothing of it out
bool Shown(int tab, int index)
{
    switch (tab)
    {
        case HUDController::TabBuild:
        {
            const BuildingBlueprint &bp = EntityManager::GetBlueprints()[static_cast<std::size_t>(index)];
            if (bp.mineKind >= 0) // a mine shows up once its technology is done
            {
                const auto &mines = Mines::Specs();
                return bp.mineKind < static_cast<int>(mines.size())
                       && IsMineResearched(
                           Players::Local(), mines[static_cast<std::size_t>(bp.mineKind)].researchValue);
            }
            return Capabilities::AllowsBuilding(Players::Local(), bp.buildingIndex);
        }
        case HUDController::TabInfantry:
        case HUDController::TabVehicles:
            return Capabilities::AllowsUnit(
                Players::Local(), unitCatalog(tab == HUDController::TabInfantry).items[index].archetype);
    }
    return true;
}

} // namespace

int HUDController::ItemCountForTab(int tab) const
{
    int count = 0;
    for (int i = 0, n = CatalogCount(tab); i < n; ++i)
        count += Shown(tab, i);
    return count;
}

int HUDController::ItemAt(int tab, int slot) const
{
    for (int i = 0, n = CatalogCount(tab); i < n; ++i)
    {
        if (Shown(tab, i) && slot-- == 0)
            return i;
    }
    return -1;
}

int HUDController::MaxScrollRows(int tab, int gridHeight) const
{
    int rows = (ItemCountForTab(tab) + SidebarLayout::kCols - 1) / SidebarLayout::kCols;
    int visible = std::max(1, (gridHeight + SidebarLayout::kGap) / (SidebarLayout::kCellH + SidebarLayout::kGap));
    return std::max(0, rows - visible);
}

} // namespace OpenArtOfWar
