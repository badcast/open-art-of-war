#include "graphics/RenderUtility.hpp"
#include "game/AIController.hpp"
#include "AIControllerInternal.hpp"
#include "game/BuildLimits.hpp"
#include "core/DetMath.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/TileMap.hpp"
#include "game/Pathfinder.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <sstream>
#include <cmath>

namespace OpenArtOfWar
{

void AIController::UpdateIntel(EntityManager &entities)
{
    m_intelTimer -= SIMULATION_TIMESTEP;
    if (m_intelTimer > 0.0f)
        return;
    m_intelTimer = 1.0f;
    const bool seesAll = GetEffectiveDifficulty() == AIDifficulty::Hardcore;
    std::set<int> alive;
    for (const auto &e : entities.GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die || !IsFoe(e))
            continue;
        alive.insert(e.id);
        if (!seesAll && !AiSees(e, entities))
            continue;
        if (const Building *b = e.AsBuilding())
        {
            if (b->rawBuildingType < 15)
                m_knownBuildings[e.id] = {b->tileX + b->buildWidth / 2, b->tileY + b->buildHeight / 2};
        }
        else if (const Unit *u = e.AsUnit())
        {
            m_seenEnemyUnits[e.id] = !Utility::IsInfantryArchetype(u->archetypeId);
        }
    }
    // Destroyed things are forgotten
    for (auto it = m_knownBuildings.begin(); it != m_knownBuildings.end();)
        it = alive.count(it->first) ? std::next(it) : m_knownBuildings.erase(it);
    for (auto it = m_seenEnemyUnits.begin(); it != m_seenEnemyUnits.end();)
        it = alive.count(it->first) ? std::next(it) : m_seenEnemyUnits.erase(it);
}

void AIController::UpdateScouting(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
{
    const AIDifficulty difficulty = GetEffectiveDifficulty();
    if (difficulty == AIDifficulty::Hardcore || (m_playerHqX <= 0 && m_playerHqY <= 0))
        return;
    m_scoutTimer -= deltaTime;
    Unit *scout = m_scoutId ? entities.GetUnit(m_scoutId) : nullptr;
    if (m_scoutId && (!scout || !scout->alive || scout->state == ActionState::Die))
    {
        Think("SCOUT", "The scout was lost");
        m_scoutId = 0;
        scout = nullptr;
    }
    if (scout)
    {
        // Back to the reserve once it got there, or when it stands without orders for a while
        const bool there = std::max(std::abs(scout->tileX - m_scoutX), std::abs(scout->tileY - m_scoutY)) <= 3;
        const bool idle = scout->state == ActionState::Idle && !scout->HasMoveTarget() && !scout->waitingForPath;
        m_scoutIdleTimer = idle ? m_scoutIdleTimer + deltaTime : 0.0f;
        if (there || m_scoutIdleTimer > 3.0f)
        {
            std::ostringstream msg;
            msg << "Scouting trip " << m_scoutTrips << " over: " << m_knownBuildings.size() << " enemy structures and "
                << m_seenEnemyUnits.size() << " units known";
            Think("SCOUT", msg.str());
            entities.IssueMoveOrder({scout->id}, m_stagingX, m_stagingY, pathfinder, false);
            m_waveRoster.insert(scout->id);
            m_recalled.insert(scout->id);
            m_scoutId = 0;
        }
        return;
    }
    if (m_scoutTimer > 0.0f)
        return;
    // The cheapest reserve unit goes
    int bestId = 0, bestCost = 1 << 30;
    for (const auto &e : entities.GetAllEntities())
    {
        const Unit *u = e.AsUnit();
        if (!u || !IsReserveUnit(*u) || m_repairing.count(u->id) || u->targetEntityId != 0)
            continue;
        const UnitConfig *cfg = entities.GetUnitConfig(u->archetypeId);
        const int cost = cfg ? cfg->creditCost : 1000;
        if (cost < bestCost)
        {
            bestCost = cost;
            bestId = u->id;
        }
    }
    if (!bestId)
        return;
    // First the enemy's base, then its flanks and where it last came from, in turn
    int tx = m_playerHqX, ty = m_playerHqY;
    if (m_scoutTrips % 3 == 1)
        FlankPoint(2, m_playerHqX, m_playerHqY, map, tx, ty);
    else if (m_scoutTrips % 3 == 2)
        FlankPoint(3, m_playerHqX, m_playerHqY, map, tx, ty);
    if (tx < 0)
    {
        tx = m_playerHqX;
        ty = m_playerHqY;
    }
    m_waveRoster.erase(bestId);
    m_scoutId = bestId;
    m_scoutIdleTimer = 0.0f;
    ++m_scoutTrips;
    entities.IssueAttackMoveOrder({bestId}, tx, ty, pathfinder);
    m_scoutX = tx;
    m_scoutY = ty;
    const float interval = difficulty == AIDifficulty::Easy     ? 180.0f
                           : difficulty == AIDifficulty::Medium ? 120.0f
                                                                : 90.0f;
    m_scoutTimer = interval;
    std::ostringstream msg;
    msg << "Sending a scout (trip " << m_scoutTrips << ") toward (" << tx << ", " << ty << ")";
    Think("SCOUT", msg.str());
}

bool AIController::IsValuable(const Unit &u, const EntityManager &entities) const
{
    // Siege forms count as their mobile form: Sniper 14 -> 4, Rhino 15 -> 10, Grad 16 -> 13
    auto baseOf = [](int arch) {
        return arch == 14 ? 4 : arch == 15 ? 10 : arch == 16 ? 13 : arch;
    };
    const int reference = side() == Faction::Confederation ? kZeus : 10; // Zeus / Rhino
    const UnitConfig *ref = entities.GetUnitConfig(reference);
    const UnitConfig *own = entities.GetUnitConfig(baseOf(u.archetypeId));
    return ref && own && own->creditCost >= ref->creditCost;
}

void AIController::UpdateValuableRetreat(EntityManager &entities, Pathfinder &pathfinder)
{
    const AIDifficulty difficulty = GetEffectiveDifficulty();
    const float threshold = difficulty == AIDifficulty::Easy     ? 0.0f
                            : difficulty == AIDifficulty::Medium ? 0.25f
                            : difficulty == AIDifficulty::Hard   ? 0.30f
                                                                 : 0.35f;
    // Where vehicles get repaired: next to an own Factory; otherwise (and for infantry) the staging area
    int factoryX = -1, factoryY = -1;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->owner == m_player && b->type == UnitType::Factory && b->buildProgress >= 1.0f)
        {
            factoryX = b->tileX + b->buildWidth / 2;
            factoryY = b->tileY + b->buildHeight + 1;
            break;
        }
    }
    // Healed (or dead): back to the reserve
    for (auto it = m_repairing.begin(); it != m_repairing.end();)
    {
        Unit *u = entities.GetUnit(*it);
        if (!u || !u->alive || u->state == ActionState::Die)
        {
            it = m_repairing.erase(it);
            continue;
        }
        if (u->currentHp * 10 >= u->maxHp * 9)
        {
            m_waveRoster.insert(u->id);
            Think("RETREAT", "A repaired unit rejoins the reserve");
            it = m_repairing.erase(it);
            continue;
        }
        const bool vehicle = !Utility::IsInfantryArchetype(u->archetypeId);
        const int rx = vehicle && factoryX >= 0 ? factoryX : m_stagingX;
        const int ry = vehicle && factoryX >= 0 ? factoryY : m_stagingY;
        if (u->state == ActionState::Idle && !u->HasMoveTarget()
            && std::max(std::abs(u->tileX - rx), std::abs(u->tileY - ry)) > 3)
            entities.IssueMoveOrder({u->id}, rx, ry, pathfinder, false);
        ++it;
    }
    if (threshold <= 0.0f)
        return;
    for (const AISquad &sq : m_squadController.GetSquads())
    {
        if (sq.playerSquad || sq.unitIds.size() <= 3)
            continue; // a few units left: they fight to the end
        std::vector<int> leaving;
        for (int id : sq.unitIds)
        {
            const Unit *u = entities.GetUnit(id);
            if (u && u->maxHp > 0 && static_cast<float>(u->currentHp) < threshold * static_cast<float>(u->maxHp)
                && IsValuable(*u, entities) && sq.unitIds.size() - leaving.size() > 4)
                leaving.push_back(id);
        }
        if (leaving.empty())
            continue;
        m_squadController.ReleaseUnits(leaving, entities);
        for (int id : leaving)
        {
            m_waveRoster.erase(id);
            m_repairing.insert(id);
            if (Unit *u = entities.GetUnit(id))
            {
                u->targetEntityId = 0;
                u->isAttackMoving = false;
                const bool vehicle = !Utility::IsInfantryArchetype(u->archetypeId);
                const int rx = vehicle && factoryX >= 0 ? factoryX : m_stagingX;
                const int ry = vehicle && factoryX >= 0 ? factoryY : m_stagingY;
                entities.IssueMoveOrder({id}, rx, ry, pathfinder, false);
            }
        }
        Think("RETREAT", std::to_string(leaving.size()) + " badly hurt expensive unit(s) pull back for repairs");
        break; // squads changed: the rest next time
    }
}

void AIController::UpdateIdleUnits(EntityManager &entities, Pathfinder &pathfinder)
{
    for (auto &e : entities.GetAllEntities())
    {
        Unit *u = e.AsUnit();
        if (!u || !u->alive || u->state == ActionState::Die || u->owner != m_player)
            continue;
        // A squad's members take orders from their squad only (a held member waits for its group); the scout
        // and units on their way to repairs do not pick fights either
        if (m_squadController.IsSquadMember(u->id) || u->id == m_scoutId || m_repairing.count(u->id))
            continue;
        if (u->state == ActionState::Idle && u->targetEntityId == 0 && !u->waitingForPath && !u->isAttackMoving)
        {
            float closestDist = u->sightRadius * TILE_WIDTH;
            int targetId = 0;

            for (const auto &other : entities.GetAllEntities())
            {
                if (!other.alive || other.state == ActionState::Die || !IsFoe(other))
                    continue;
                if (!AiSees(other, entities))
                    continue; // AI only engages units visible to Confederation!
                float dx = other.GetUnitCenterPixelX() - u->GetUnitCenterPixelX();
                float dy = other.GetUnitCenterPixelY() - u->GetUnitCenterPixelY();
                float d = DetMath::Hypot(dx, dy);
                if (d < closestDist)
                {
                    closestDist = d;
                    targetId = other.id;
                }
            }

            if (targetId != 0)
            {
                entities.IssueAttackOrder({u->id}, targetId, &pathfinder);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Debug visualization (--debug mode)
// ---------------------------------------------------------------------------

} // namespace OpenArtOfWar
