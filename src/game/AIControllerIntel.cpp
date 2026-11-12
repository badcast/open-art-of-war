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
    if (difficulty == AIDifficulty::Hardcore || m_noFoesLeft || (m_playerHqX <= 0 && m_playerHqY <= 0))
    {
        // No more scouting: a scout still out joins the offensive and never returns to base
        if (Unit *scout = m_scoutId ? entities.GetUnit(m_scoutId) : nullptr; scout && scout->alive)
        {
            m_waveRoster.erase(scout->id);
            m_offensive.insert(scout->id);
        }
        m_scoutId = 0;
        return;
    }
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
        // Once it got there, or when it stands without orders for a while, it joins the offensive instead of returning
        const bool there = std::max(std::abs(scout->tileX - m_scoutX), std::abs(scout->tileY - m_scoutY)) <= 3;
        const bool idle = scout->state == ActionState::Idle && !scout->HasMoveTarget() && !scout->waitingForPath;
        m_scoutIdleTimer = idle ? m_scoutIdleTimer + deltaTime : 0.0f;
        if (there || m_scoutIdleTimer > 3.0f)
        {
            std::ostringstream msg;
            msg << "Scouting trip " << m_scoutTrips << " over: " << m_knownBuildings.size() << " enemy structures and "
                << m_seenEnemyUnits.size() << " units known";
            Think("SCOUT", msg.str());
            m_waveRoster.erase(scout->id);
            m_offensive.insert(scout->id);
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
        if (!u || !IsReserveUnit(*u) || u->targetEntityId != 0 || m_defenders.count(u->id))
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

void AIController::UpdateIdleUnits(EntityManager &entities, Pathfinder &pathfinder)
{
    const std::vector<const Building *> ownStructures = OwnStructures(entities);
    for (auto &e : entities.GetAllEntities())
    {
        Unit *u = e.AsUnit();
        if (!u || !u->alive || u->state == ActionState::Die || u->owner != m_player)
            continue;
        // An attack-move or ground target with no active path is over: clear it before role checks so neither
        // offensive troops nor reserve troops stay marked as busy forever
        if (u->state == ActionState::Idle && u->path.empty() && !u->waitingForPath && u->targetEntityId == 0
            && u->targetWallTileX < 0 && !u->isSiegeMode)
        {
            u->isAttackMoving = false;
            u->ClearMoveTarget();
        }
        // A squad's members take orders from their squad only (a held member waits for its group); the scout
        // and offensive troops do not pick base fights either
        if (m_squadController.IsSquadMember(u->id) || u->id == m_scoutId || m_offensive.count(u->id))
            continue;
        if (u->state == ActionState::Idle && u->targetEntityId == 0 && !u->waitingForPath && !u->isAttackMoving)
        {
            float closestDist = u->sightRadius * TILE_WIDTH;
            int targetId = 0;
            // An attack order pursues without a leash: only against an enemy at the base or a little outside it.
            // Farther out the unit fights by its stance (it takes what comes into sight and returns). The
            // mission's garrisons stand anywhere and keep their order.
            const bool leashed = !m_garrison.count(u->id);

            for (const auto &other : entities.GetAllEntities())
            {
                if (!other.alive || other.state == ActionState::Die || !IsFoe(other))
                    continue;
                if (!AiSees(other, entities))
                    continue; // AI only engages units visible to Confederation!
                float dx = other.GetUnitCenterPixelX() - u->GetUnitCenterPixelX();
                float dy = other.GetUnitCenterPixelY() - u->GetUnitCenterPixelY();
                float d = DetMath::Hypot(dx, dy);
                if (d < closestDist && (!leashed || IsNearBase(other, ownStructures, kBaseChaseTiles)))
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
