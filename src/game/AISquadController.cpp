#include "graphics/RenderUtility.hpp"
#include "game/AISquadController.hpp"
#include "core/DetMath.hpp"
#include "game/EntityManager.hpp"
#include "game/TileMap.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <tuple>

namespace OpenArtOfWar
{

namespace
{

constexpr float kTickSeconds = 0.25f;

// Unit position in fractional tile coordinates (tracks sub-tile movement)
float UnitTileX(const Unit &unit)
{
    return unit.GetWorldPixelX() / TILE_WIDTH;
}
float UnitTileY(const Unit &unit)
{
    return unit.GetWorldPixelY() / TILE_HEIGHT;
}

float DistTo(const Unit &unit, float x, float y)
{
    return DetMath::Hypot(UnitTileX(unit) - x, UnitTileY(unit) - y);
}

bool IsEngaged(const Unit &unit)
{
    return unit.targetEntityId != 0 || unit.targetWallTileX >= 0 || unit.state == ActionState::Attack;
}

bool IsIdleWithoutOrders(const Unit &unit)
{
    return unit.state == ActionState::Idle && unit.path.empty() && !unit.waitingForPath && unit.targetEntityId == 0;
}

// Both factions: Fortress / Hammer / Zeus and Coyote / Armadillo / Rhino
bool IsTank(int archetypeId)
{
    return archetypeId == 6 || archetypeId == 7 || archetypeId == 8 || archetypeId == 9 || archetypeId == 10
           || archetypeId == 11 || archetypeId == 18;
}
bool IsMelee(int archetypeId)
{
    return archetypeId == 5;
} // flamethrower
// Rocket infantry and MLRS of both factions
bool IsRangedSupport(int archetypeId)
{
    return archetypeId == 2 || archetypeId == 3 || archetypeId == 12 || archetypeId == 13 || archetypeId == 16;
}

// Cohesion radius for a group of unitCount units. Big groups get a proportionally larger radius (a column of
// 15 is naturally longer than 3 tiles), and narrow passages stretch any group into a line.
float EffectiveLeash(const AITacticsConfig &tactics, std::size_t unitCount, bool inCorridor)
{
    const float count = static_cast<float>(unitCount);
    float leash = tactics.leashTiles + tactics.leashPerUnit * std::max(0.0f, count - 4.0f);
    leash = std::max(leash, std::sqrt(count) * 0.8f + 1.0f); // never tighter than the formation itself
    if (inCorridor)
        leash *= std::max(1.0f, tactics.corridorLeashFactor);
    return leash;
}

} // namespace

const char *SquadStateName(AISquadState state)
{
    switch (state)
    {
        case AISquadState::Marching:
            return "MARCH";
        case AISquadState::Regrouping:
            return "REGROUP";
        case AISquadState::ChokeApproach:
            return "PRE-CHOKE";
        case AISquadState::ChokeTransit:
            return "COLUMN";
        case AISquadState::Deploying:
            return "DEPLOY";
        case AISquadState::Engaged:
            return "COMBAT";
        case AISquadState::FormingUp:
            return "FORM UP";
        case AISquadState::Assault:
            return "ASSAULT";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Chokepoint detection
// ---------------------------------------------------------------------------

void ChokepointMap::Rebuild(const TileMap &map)
{
    m_width = map.GetWidth();
    m_height = map.GetHeight();
    m_choke.assign(static_cast<std::size_t>(m_width) * m_height, 0);
    m_openWidth.assign(static_cast<std::size_t>(m_width) * m_height, 0);
    if (m_width <= 0 || m_height <= 0)
        return;

    auto isOpen = [&](int x, int y) {
        return map.IsPassable(x, y) && map.GetWall(x, y) == 0;
    };

    // Length of the contiguous open run through every tile, horizontally and vertically
    std::vector<int> horizontalRun(m_choke.size(), 0);
    std::vector<int> verticalRun(m_choke.size(), 0);
    for (int y = 0; y < m_height; ++y)
    {
        int x = 0;
        while (x < m_width)
        {
            if (!isOpen(x, y))
            {
                ++x;
                continue;
            }
            int start = x;
            while (x < m_width && isOpen(x, y))
                ++x;
            for (int i = start; i < x; ++i)
                horizontalRun[y * m_width + i] = x - start;
        }
    }
    for (int x = 0; x < m_width; ++x)
    {
        int y = 0;
        while (y < m_height)
        {
            if (!isOpen(x, y))
            {
                ++y;
                continue;
            }
            int start = y;
            while (y < m_height && isOpen(x, y))
                ++y;
            for (int i = start; i < y; ++i)
                verticalRun[i * m_width + x] = y - start;
        }
    }

    // A corridor tile is at most 2 tiles wide across and at least 3 tiles long along the passage
    for (std::size_t i = 0; i < m_choke.size(); ++i)
    {
        int narrow = std::min(horizontalRun[i], verticalRun[i]);
        int along = std::max(horizontalRun[i], verticalRun[i]);
        m_openWidth[i] = static_cast<std::uint8_t>(std::min(narrow, 255));
        m_choke[i] = (narrow > 0 && narrow <= 2 && along >= 3) ? 1 : 0;
    }
}

int ChokepointMap::OpenWidth(int x, int y) const
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height)
        return 0;
    return m_openWidth[static_cast<std::size_t>(y) * m_width + x];
}

bool ChokepointMap::IsChoke(int x, int y) const
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height)
        return false;
    return m_choke[static_cast<std::size_t>(y) * m_width + x] != 0;
}

// ---------------------------------------------------------------------------
// Squad lifecycle
// ---------------------------------------------------------------------------

void AISquadController::Clear()
{
    m_squads.clear();
    m_released.clear();
    m_nextSquadId = 1;
    m_tickAccumulator = 0.0f;
}

bool AISquadController::IsSquadMember(int unitId) const
{
    for (const auto &squad : m_squads)
    {
        if (std::find(squad.unitIds.begin(), squad.unitIds.end(), unitId) != squad.unitIds.end())
            return true;
    }
    return false;
}

std::vector<int> AISquadController::TakeReleasedUnits()
{
    std::vector<int> released;
    released.swap(m_released);
    return released;
}

int AISquadController::CreateSquad(
    const std::vector<int> &unitIds,
    int targetX,
    int targetY,
    int waveNumber,
    EntityManager &entities,
    Pathfinder &pathfinder,
    const AITacticsConfig &tactics,
    int viaX,
    int viaY)
{
    if (unitIds.empty())
        return 0;

    AISquad squad;
    squad.id = m_nextSquadId++;
    squad.waveNumber = waveNumber;
    squad.unitIds = unitIds;
    squad.targetX = targetX;
    squad.targetY = targetY;
    squad.viaX = viaX;
    squad.viaY = viaY;
    squad.initialSize = static_cast<int>(unitIds.size());
    squad.leaderId = PickLeader(squad, entities, tactics);
    for (int unitId : unitIds)
    {
        if (const Entity *entity = entities.GetEntity(unitId))
        {
            squad.owner = entity->owner;
            break;
        }
    }

    RebuildRoute(squad, entities, pathfinder);
    squad.state = AISquadState::Marching;
    squad.orderTimer = 1e6f; // the first leg is ordered on the next tick, which has the map at hand
    m_squads.push_back(std::move(squad));
    return m_squads.back().id;
}

void AISquadController::Update(
    float deltaTime,
    TileMap &map,
    EntityManager &entities,
    Pathfinder &pathfinder,
    const ChokepointMap &chokes,
    const AITacticsConfig &tactics)
{
    m_tickAccumulator += deltaTime;
    if (m_tickAccumulator < kTickSeconds)
        return;
    float tickDelta = m_tickAccumulator;
    m_tickAccumulator = 0.0f;

    for (auto &squad : m_squads)
    {
        Tick(squad, tickDelta, map, entities, pathfinder, chokes, tactics);
    }
    m_squads.erase(
        std::remove_if(m_squads.begin(), m_squads.end(), [](const AISquad &squad) { return squad.unitIds.empty(); }),
        m_squads.end());
}

AISquad *AISquadController::FindSquad(int squadId)
{
    for (auto &squad : m_squads)
    {
        if (squad.id == squadId)
            return &squad;
    }
    return nullptr;
}

bool AISquadController::AddUnits(int squadId, const std::vector<int> &unitIds, const EntityManager &entities)
{
    for (auto &squad : m_squads)
    {
        if (squad.id != squadId)
            continue;
        for (int unitId : unitIds)
        {
            const Unit *unit = entities.GetUnit(unitId);
            if (!unit || !unit->alive || unit->state == ActionState::Die || unit->owner != squad.owner
                || IsSquadMember(unitId))
                continue;
            squad.unitIds.push_back(unitId);
        }
        return true;
    }
    return false;
}

void AISquadController::VisitState(AIStateSink &sink) const
{
    sink.Record("squads");
    sink.I32("next", m_nextSquadId);
    sink.F32("tick", m_tickAccumulator);
    sink.I32("released", static_cast<std::int32_t>(m_released.size()));
    for (const AISquad &squad : m_squads)
    {
        sink.Record("squad");
        sink.I32("id", squad.id);
        sink.I32("wave", squad.waveNumber);
        sink.I32("owner", squad.owner);
        sink.I32("state", static_cast<std::int32_t>(squad.state));
        sink.I32("resume", static_cast<std::int32_t>(squad.resumeState));
        sink.I32("leader", squad.leaderId);
        sink.I32("tx", squad.targetX);
        sink.I32("ty", squad.targetY);
        sink.I32("progress", squad.progress);
        sink.I32("march", squad.marchIndex);
        sink.I32("focus", squad.focusTargetId);
        sink.I32("flags", (squad.attackMove ? 1 : 0) | (squad.playerSquad ? 2 : 0) | (squad.formedUp ? 4 : 0));
        sink.F32("stateTimer", squad.stateTimer);
        sink.F32("orderTimer", squad.orderTimer);
        sink.F32("leash", squad.leashRadius);
        for (int unitId : squad.unitIds)
            sink.I32("u", unitId);
    }
}

void AISquadController::ReleaseUnits(const std::vector<int> &unitIds, EntityManager &entities)
{
    for (auto &squad : m_squads)
    {
        for (int unitId : unitIds)
        {
            if (std::find(squad.unitIds.begin(), squad.unitIds.end(), unitId) != squad.unitIds.end())
                ReleaseUnit(squad, unitId, entities);
        }
        if (!squad.unitIds.empty()
            && std::find(squad.unitIds.begin(), squad.unitIds.end(), squad.leaderId) == squad.unitIds.end())
        {
            squad.leaderId = squad.unitIds.front();
        }
    }
    m_squads.erase(
        std::remove_if(m_squads.begin(), m_squads.end(), [](const AISquad &squad) { return squad.unitIds.empty(); }),
        m_squads.end());
}

void AISquadController::OrderLeg(
    const AISquad &squad,
    const std::vector<int> &unitIds,
    int targetX,
    int targetY,
    EntityManager &entities,
    Pathfinder &pathfinder)
{
    // The computer always goes by auto-attack (units answer fire on the way); a plain Move is only for a retreat
    (void) squad;
    entities.IssueAttackMoveOrder(unitIds, targetX, targetY, pathfinder);
    CapDetours(squad, entities, pathfinder.GetMap());
}

void AISquadController::CapDetours(const AISquad &squad, EntityManager &entities, const TileMap *map)
{
    // A hostile wall within a few tiles of the unit: worth breaching instead of the detour
    auto hostileWallNear = [&](const Unit &unit) {
        if (!map)
            return false;
        constexpr int kSearchRadius = 6;
        for (int y = std::max(0, unit.tileY - kSearchRadius);
             y <= std::min(map->GetHeight() - 1, unit.tileY + kSearchRadius);
             ++y)
        {
            for (int x = std::max(0, unit.tileX - kSearchRadius);
                 x <= std::min(map->GetWidth() - 1, unit.tileX + kSearchRadius);
                 ++x)
            {
                if (map->GetWall(x, y) > 0 && Players::Hostile(map->GetWallOwner(x, y), unit.owner))
                    return true;
            }
        }
        return false;
    };
    const Unit *leader = entities.GetUnit(squad.leaderId);
    if (!leader)
        return;
    const float leash = std::max(3.0f, squad.leashRadius);
    std::vector<int> unitsToHold;
    for (int unitId : squad.unitIds)
    {
        if (unitId == squad.leaderId)
            continue;
        const Unit *unit = entities.GetUnit(unitId);
        if (!unit || unit->path.size() <= unit->pathIndex)
            continue;
        const PathNode &goal = unit->path.back();
        const float straightDist
            = DetMath::Hypot(static_cast<float>(goal.x - unit->tileX), static_cast<float>(goal.y - unit->tileY));
        const float pathLength = static_cast<float>(unit->path.size() - unit->pathIndex);
        // Allowed: the straight way, some slack for small obstacles, plus the group's leash
        const float detourLimit = std::max(6.0f, straightDist * 1.8f + leash * 1.5f);
        // Only units already with the group: a straggler far away must still be allowed to come back
        const float distToLeader = DistTo(*unit, static_cast<float>(leader->tileX), static_cast<float>(leader->tileY));
        // A map-wide detour is never worth it, wherever the leader is
        const bool isMapWideDetour = pathLength > straightDist * 3.0f + 20.0f;
        if (pathLength > detourLimit && (distToLeader <= leash * 2.0f || isMapWideDetour))
        {
            if (hostileWallNear(*unit))
            {
                // Keep the goal, drop the long route: the unit's own breach check picks a wall next tick
                Unit *mutableUnit = entities.GetUnit(unitId);
                mutableUnit->ClearPathState();
                if (mutableUnit->state == ActionState::Move)
                    mutableUnit->state = ActionState::Idle;
                mutableUnit->waitingForPath = true;
                mutableUnit->wallCheckCooldown = 0.0f;
                continue;
            }
            // Only hold a unit that is already right beside the leader and near the goal (so it does not walk around
            // an obstacle just for a lateral formation slot); a unit separated from the leader by a base wall or
            // building must keep its route out of the base
            if (distToLeader <= 3.0f && straightDist <= leash)
                unitsToHold.push_back(unitId);
        }
    }
    if (!unitsToHold.empty())
        HoldUnits(unitsToHold, entities);
}

void AISquadController::SetState(AISquad &squad, AISquadState state)
{
    squad.state = state;
    squad.stateTimer = 0.0f;
    squad.quietTimer = 0.0f;
}

void AISquadController::UpdateFocusTarget(AISquad &squad, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
{
    const Unit *leader = entities.GetUnit(squad.leaderId);
    if (!leader || !leader->alive)
        return;
    const float searchRadius = std::max(static_cast<float>(leader->sightRadius), leader->attackRange);
    auto distToLeader = [&](const Entity &entity) {
        return DetMath::Hypot(
            static_cast<float>(entity.tileX - leader->tileX), static_cast<float>(entity.tileY - leader->tileY));
    };
    auto isTargetable = [&](const Entity &entity) {
        return entity.alive && entity.state != ActionState::Die && entity.faction != Faction::Neutral
               && Players::Hostile(entity.owner, squad.owner) && distToLeader(entity) <= searchRadius
               && entities.IsEntityVisibleToPlayer(entity, squad.owner, map);
    };
    // The target stays while it lives and is in sight
    const Entity *focus = squad.focusTargetId != 0 ? entities.GetEntity(squad.focusTargetId) : nullptr;
    if (!focus || !isTargetable(*focus))
    {
        focus = nullptr;
        int bestTier = 99;
        float bestDist = 1e9f;
        for (const Entity &entity : entities.GetAllEntities())
        {
            if (!isTargetable(entity))
                continue;
            const int tier = TargetPriorityTier(entity);
            const float dist = distToLeader(entity);
            if (tier < bestTier || (tier == bestTier && dist < bestDist))
            {
                bestTier = tier;
                bestDist = dist;
                focus = &entity;
            }
        }
        squad.focusTargetId = focus ? focus->id : 0;
    }
    if (!focus)
        return;
    // Members that move and can get there fire at the group's target; siege and immobile units, and those with no way
    // to it, keep to their own
    std::vector<int> unitsToOrder;
    for (int unitId : squad.unitIds)
    {
        const Unit *unit = entities.GetUnit(unitId);
        if (!unit || !unit->alive || unit->state == ActionState::Die || unit->targetEntityId == squad.focusTargetId)
            continue;
        if (unit->isSiegeMode || unit->moveSpeed <= 0.0f || unit->HoldsGround() || unit->attackDamage <= 0)
            continue;
        if (!unit->HasOpenRouteToEntity(*focus, pathfinder))
            continue;
        unitsToOrder.push_back(unitId);
    }
    if (!unitsToOrder.empty())
        entities.IssueAttackOrder(unitsToOrder, squad.focusTargetId, &pathfinder);
}

void AISquadController::ReleaseUnit(AISquad &squad, int unitId, EntityManager &entities)
{
    squad.unitIds.erase(std::remove(squad.unitIds.begin(), squad.unitIds.end(), unitId), squad.unitIds.end());
    squad.stragglerBest.erase(unitId);
    squad.stragglerTimer.erase(unitId);
    if (Unit *unit = entities.GetUnit(unitId))
    {
        if (squad.focusTargetId != 0 && unit->orderedTargetId == squad.focusTargetId)
            unit->orderedTargetId = 0; // no longer the group's order
        unit->flowMode = false;
        unit->groupHold = false;
        m_released.push_back(unitId);
    }
}

int AISquadController::PickLeader(const AISquad &squad, EntityManager &entities, const AITacticsConfig &tactics) const
{
    int bestUnitId = 0;
    float bestScore = 1e9f;
    for (int unitId : squad.unitIds)
    {
        const Unit *unit = entities.GetUnit(unitId);
        if (!unit || !unit->alive || unit->state == ActionState::Die)
            continue;
        float score;
        if (tactics.syncToSlowest)
        {
            // Slowest unit sets the pace; prefer armor on ties so the column is led by a tank
            score = unit->moveSpeed - (IsTank(unit->archetypeId) ? 0.01f : 0.0f);
        }
        else
        {
            // Head of the column: the unit closest to the objective
            score = DistTo(*unit, static_cast<float>(squad.targetX), static_cast<float>(squad.targetY));
        }
        if (score < bestScore)
        {
            bestScore = score;
            bestUnitId = unitId;
        }
    }
    return bestUnitId;
}

bool AISquadController::PlanRoute(
    const TileMap &map,
    const EntityManager &entities,
    PlayerId owner,
    bool seesAll,
    int startX,
    int startY,
    int goalX,
    int goalY,
    std::vector<PathNode> &outPath)
{
    outPath.clear();
    const int mapWidth = map.GetWidth(), mapHeight = map.GetHeight();
    if (mapWidth <= 0 || mapHeight <= 0 || startX < 0 || startY < 0 || startX >= mapWidth || startY >= mapHeight)
        return false;
    goalX = std::clamp(goalX, 0, mapWidth - 1);
    goalY = std::clamp(goalY, 0, mapHeight - 1);
    const std::size_t tileCount = static_cast<std::size_t>(mapWidth) * static_cast<std::size_t>(mapHeight);
    auto tileIndex = [mapWidth](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(mapWidth) + static_cast<std::size_t>(x);
    };

    // Tuning (integer costs, a straight step = 10)
    constexpr int kStraight = 10, kDiagonal = 14;
    constexpr int kNextToObstacle = 6; // a tile touching a cliff, wall or building
    constexpr int kNearObstacle = 2;   // two tiles away from one
    constexpr int kUnderFire = 25;     // within reach of a known enemy defense
    constexpr int kHostileWall = 120;  // shooting through a wall segment ~ a twelve-tile detour

    // 0 open, 1 blocked, 2 hostile wall (passable at a price)
    std::vector<std::uint8_t> tileKind(tileCount, 0);
    for (int y = 0; y < mapHeight; ++y)
    {
        for (int x = 0; x < mapWidth; ++x)
        {
            if (map.GetWall(x, y) > 0)
                tileKind[tileIndex(x, y)] = Players::Hostile(map.GetWallOwner(x, y), owner) ? 2 : 1;
            else if (!map.IsPassable(x, y))
                tileKind[tileIndex(x, y)] = 1;
        }
    }
    std::vector<int> tilePenalty(tileCount, 0);
    for (int y = 0; y < mapHeight; ++y)
    {
        for (int x = 0; x < mapWidth; ++x)
        {
            if (tileKind[tileIndex(x, y)] != 0)
                continue;
            int nearOneTile = 0, nearTwoTiles = 0;
            for (int dy = -2; dy <= 2; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    const int nextX = x + dx, nextY = y + dy;
                    const bool blocked = nextX < 0 || nextY < 0 || nextX >= mapWidth || nextY >= mapHeight
                                         || tileKind[tileIndex(nextX, nextY)] != 0;
                    if (!blocked)
                        continue;
                    (std::max(std::abs(dx), std::abs(dy)) == 1 ? nearOneTile : nearTwoTiles) += 1;
                }
            }
            tilePenalty[tileIndex(x, y)] = nearOneTile > 0 ? kNextToObstacle : (nearTwoTiles > 0 ? kNearObstacle : 0);
        }
    }
    for (const auto &entity : entities.GetAllEntities())
    {
        if (!entity.alive || entity.state == ActionState::Die || !entity.isBuilding || entity.attackDamage <= 0
            || entity.owner >= kMaxPlayers || !Players::Hostile(entity.owner, owner))
            continue;
        if (!seesAll && !entities.IsEntityVisibleToAi(entity, owner))
            continue;
        const int defenseRadius = static_cast<int>(std::ceil(entity.attackRange)) + 1;
        const float centerX = entity.GetUnitCenterPixelX() / TILE_WIDTH;
        const float centerY = entity.GetUnitCenterPixelY() / TILE_HEIGHT;
        for (int y = std::max(0, static_cast<int>(centerY) - defenseRadius);
             y <= std::min(mapHeight - 1, static_cast<int>(centerY) + defenseRadius);
             ++y)
        {
            for (int x = std::max(0, static_cast<int>(centerX) - defenseRadius);
                 x <= std::min(mapWidth - 1, static_cast<int>(centerX) + defenseRadius);
                 ++x)
            {
                if (DetMath::Hypot(x + 0.5f - centerX, y + 0.5f - centerY) <= static_cast<float>(defenseRadius))
                    tilePenalty[tileIndex(x, y)] += kUnderFire;
            }
        }
    }

    // The objective is often a building (blocked): reaching its surroundings is enough
    const bool goalBlocked = tileKind[tileIndex(goalX, goalY)] == 1;
    auto isGoal = [&](int x, int y) {
        const int chebyshevDist = std::max(std::abs(x - goalX), std::abs(y - goalY));
        return chebyshevDist == 0 || (goalBlocked && chebyshevDist <= 3);
    };
    auto heuristic = [&](int x, int y) {
        const int dx = std::abs(x - goalX), dy = std::abs(y - goalY);
        return kStraight * std::max(dx, dy) + (kDiagonal - kStraight) * std::min(dx, dy);
    };

    constexpr int kInfiniteCost = 1 << 30;
    std::vector<int> cost(tileCount, kInfiniteCost);
    std::vector<int> parentIndex(tileCount, -1);
    // (fScore, index): equal fScore values pop by tile index, the same on every computer
    using QueueEntry = std::pair<int, int>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> openQueue;
    cost[tileIndex(startX, startY)] = 0;
    openQueue.push({heuristic(startX, startY), static_cast<int>(tileIndex(startX, startY))});
    int reachedIndex = -1;
    while (!openQueue.empty())
    {
        const auto [fScore, currentIndex] = openQueue.top();
        openQueue.pop();
        const int x = currentIndex % mapWidth, y = currentIndex / mapWidth;
        if (fScore - heuristic(x, y) > cost[static_cast<std::size_t>(currentIndex)])
            continue; // stale
        if (isGoal(x, y))
        {
            reachedIndex = currentIndex;
            break;
        }
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0)
                    continue;
                const int nextX = x + dx, nextY = y + dy;
                if (nextX < 0 || nextY < 0 || nextX >= mapWidth || nextY >= mapHeight)
                    continue;
                const std::uint8_t neighborKind = tileKind[tileIndex(nextX, nextY)];
                if (neighborKind == 1)
                    continue;
                // No cutting corners past cliffs or buildings
                if (dx != 0 && dy != 0 && (tileKind[tileIndex(x + dx, y)] == 1 || tileKind[tileIndex(x, y + dy)] == 1))
                    continue;
                const int stepCost = (dx != 0 && dy != 0 ? kDiagonal : kStraight) + tilePenalty[tileIndex(nextX, nextY)]
                                     + (neighborKind == 2 ? kHostileWall : 0);
                const int nextCost = cost[static_cast<std::size_t>(currentIndex)] + stepCost;
                const std::size_t nextIndex = tileIndex(nextX, nextY);
                if (nextCost < cost[nextIndex])
                {
                    cost[nextIndex] = nextCost;
                    parentIndex[nextIndex] = currentIndex;
                    openQueue.push({nextCost + heuristic(nextX, nextY), static_cast<int>(nextIndex)});
                }
            }
        }
    }
    if (reachedIndex < 0)
        return false;
    for (int i = reachedIndex; i >= 0 && i != static_cast<int>(tileIndex(startX, startY));
         i = parentIndex[static_cast<std::size_t>(i)])
        outPath.push_back({i % mapWidth, i / mapWidth});
    std::reverse(outPath.begin(), outPath.end());
    return true;
}

int AISquadController::FormUpIndex(const AISquad &squad, const EntityManager &entities) const
{
    const int lastIndex = static_cast<int>(squad.route.size()) - 1;
    if (lastIndex <= 0)
        return -1;
    constexpr float kObjectiveRadius = 14.0f; // defenses this close to the objective cover it
    constexpr float kMargin = 2.0f;           // the line stands this far outside their reach
    struct Defense
    {
        float x, y, reach;
    };
    std::vector<Defense> defenses;
    for (const auto &entity : entities.GetAllEntities())
    {
        if (!entity.alive || entity.state == ActionState::Die || !entity.isBuilding || entity.attackDamage <= 0
            || entity.owner >= kMaxPlayers || !Players::Hostile(entity.owner, squad.owner))
            continue;
        if (!m_seesAll && !entities.IsEntityVisibleToAi(entity, squad.owner))
            continue;
        const float entityX = entity.GetUnitCenterPixelX() / TILE_WIDTH;
        const float entityY = entity.GetUnitCenterPixelY() / TILE_HEIGHT;
        if (DetMath::Hypot(entityX - squad.targetX, entityY - squad.targetY) <= kObjectiveRadius)
            defenses.push_back({entityX, entityY, entity.attackRange + kMargin});
    }
    if (defenses.empty())
        return -1;
    for (int i = lastIndex; i > squad.progress; --i)
    {
        const PathNode &node = squad.route[i];
        bool isSafe = true;
        for (const Defense &defense : defenses)
            isSafe = isSafe && DetMath::Hypot(node.x + 0.5f - defense.x, node.y + 0.5f - defense.y) > defense.reach;
        if (isSafe)
            return i;
    }
    return -1;
}

void AISquadController::RebuildRoute(AISquad &squad, EntityManager &entities, Pathfinder &pathfinder)
{
    squad.route.clear();
    squad.progress = 0;
    squad.marchIndex = 0;
    const Unit *leader = entities.GetUnit(squad.leaderId);
    if (!leader)
        return;

    // A flank point that is still ahead splits the route in two legs; once reached it is forgotten
    if (squad.viaX >= 0 && DistTo(*leader, static_cast<float>(squad.viaX), static_cast<float>(squad.viaY)) <= 3.0f)
    {
        squad.viaX = squad.viaY = -1;
    }

    std::vector<PathNode> path;
    bool partial = false;
    squad.route.push_back({leader->tileX, leader->tileY});
    int fromX = leader->tileX, fromY = leader->tileY;
    const TileMap *map = pathfinder.GetMap();
    // Strategic route over terrain only: the group's own idle units must not wall the leader in
    auto planLeg = [&](int goalX, int goalY) {
        path.clear();
        if (map && PlanRoute(*map, entities, squad.owner, m_seesAll, fromX, fromY, goalX, goalY, path) && !path.empty())
            return true;
        pathfinder.SetIgnoreUnitsOnce();
        pathfinder.FindPath(fromX, fromY, goalX, goalY, path, leader->id, &partial);
        return !path.empty(); // a partial route still leads the way
    };
    if (squad.viaX >= 0)
    {
        if (planLeg(squad.viaX, squad.viaY))
        {
            squad.route.insert(squad.route.end(), path.begin(), path.end());
            fromX = path.back().x;
            fromY = path.back().y;
        }
    }
    if (planLeg(squad.targetX, squad.targetY))
        squad.route.insert(squad.route.end(), path.begin(), path.end());
}

void AISquadController::HoldUnits(const std::vector<int> &unitIds, EntityManager &entities)
{
    for (int unitId : unitIds)
    {
        Unit *unit = entities.GetUnit(unitId);
        if (!unit || IsEngaged(*unit))
            continue;
        unit->ClearPathState();
        unit->ClearMoveTarget();
        unit->isAttackMoving = false;
        if (unit->state == ActionState::Move)
            unit->state = ActionState::Idle;
    }
}

void AISquadController::OrderMarch(
    AISquad &squad,
    int routeIndex,
    TileMap &map,
    EntityManager &entities,
    Pathfinder &pathfinder,
    const AITacticsConfig &tactics)
{
    if (squad.route.empty())
        return;
    routeIndex = std::clamp(routeIndex, 0, static_cast<int>(squad.route.size()) - 1);
    const PathNode &waypoint = squad.route[routeIndex];

    // Marching direction from a few nodes back, used to put the front line ahead of the rear
    const PathNode &prevWaypoint = squad.route[std::max(0, routeIndex - 3)];
    float dirX = static_cast<float>(waypoint.x - prevWaypoint.x);
    float dirY = static_cast<float>(waypoint.y - prevWaypoint.y);
    float dirLength = DetMath::Hypot(dirX, dirY);
    if (dirLength > 0.001f)
    {
        dirX /= dirLength;
        dirY /= dirLength;
    }

    std::vector<int> front, middle, back;
    for (int unitId : squad.unitIds)
    {
        Unit *unit = entities.GetUnit(unitId);
        if (!unit)
            continue;
        unit->flowMode = false;
        if (IsEngaged(*unit))
            continue; // never pull a unit out of a fight
        int archetypeId = unit->archetypeId;
        if ((tactics.armorFront && IsTank(archetypeId)) || (tactics.meleeRangedPairs && IsMelee(archetypeId)))
        {
            front.push_back(unitId);
        }
        else if ((tactics.meleeRangedPairs || tactics.armorFront) && IsRangedSupport(archetypeId))
        {
            back.push_back(unitId);
        }
        else
        {
            middle.push_back(unitId);
        }
    }

    // Rows across the way, as wide as the road allows (a tile of room kept from cliffs and walls), front kinds in
    // the first rows. Each row stands further back along the route, so the column follows its bends instead of
    // the whole group crowding one point at the edge of the road.
    auto openTile = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < map.GetWidth() && y < map.GetHeight() && map.IsPassable(x, y)
               && map.GetWall(x, y) == 0;
    };
    const float perpX = -dirY, perpY = dirX;
    // Open tiles beside the route node; with `requireClearance` a tile counts only when the next one out is open too
    auto roomAcross = [&](const PathNode &center, float sideSign, bool requireClearance) {
        int room = 0;
        for (int k = 1; k <= 3; ++k)
        {
            const int x = static_cast<int>(std::lround(center.x + perpX * sideSign * k));
            const int y = static_cast<int>(std::lround(center.y + perpY * sideSign * k));
            const int xOut = static_cast<int>(std::lround(center.x + perpX * sideSign * (k + 1)));
            const int yOut = static_cast<int>(std::lround(center.y + perpY * sideSign * (k + 1)));
            if (!openTile(x, y) || (requireClearance && !openTile(xOut, yOut)))
                break;
            room = k;
        }
        return room;
    };
    // The leader walks in the first row (the march is measured by it), then front kinds, middle, rear
    std::vector<int> ordered;
    for (const std::vector<int> *group : {&front, &middle, &back})
    {
        for (int unitId : *group)
        {
            if (unitId == squad.leaderId)
                ordered.insert(ordered.begin(), unitId);
            else
                ordered.push_back(unitId);
        }
    }
    const int maxWidth = squad.state == AISquadState::FormingUp ? 6 : 4;
    const int halfWidth = (maxWidth - 1) / 2 + 1;
    int leftRoom = std::min(roomAcross(waypoint, -1.0f, true), halfWidth);
    int rightRoom = std::min(roomAcross(waypoint, 1.0f, true), halfWidth);
    if (leftRoom + rightRoom == 0)
    {
        // A narrow passage: two abreast where it fits, right along its sides
        leftRoom = std::min(roomAcross(waypoint, -1.0f, false), 1);
        rightRoom = leftRoom > 0 ? 0 : std::min(roomAcross(waypoint, 1.0f, false), 1);
    }
    const int formationWidth = std::clamp(1 + leftRoom + rightRoom, 1, maxWidth);
    const int rowStep = formationWidth >= 2 ? 1 : 2; // a single file keeps a little more distance
    for (std::size_t k = 0; k < ordered.size(); ++k)
    {
        const int row = static_cast<int>(k) / formationWidth;
        const int col = static_cast<int>(k) % formationWidth;
        const int unitsInRow = std::min(formationWidth, static_cast<int>(ordered.size()) - row * formationWidth);
        // Rows never point back past where the group already is
        const PathNode &rowCenter
            = squad.route[std::max(std::min(squad.progress, routeIndex), routeIndex - row * rowStep)];
        // Centred on the route, shifted toward the side with more room
        int startOffset = -std::min(leftRoom, (unitsInRow - 1) / 2);
        startOffset = std::max(-leftRoom, std::min(startOffset, rightRoom - (unitsInRow - 1)));
        const int lateralOffset = startOffset + col;
        PathNode targetNode = rowCenter;
        if (lateralOffset != 0)
        {
            const float sideSign = lateralOffset > 0 ? 1.0f : -1.0f;
            for (int step = 1; step <= std::abs(lateralOffset); ++step)
            {
                const int tileX = static_cast<int>(std::lround(rowCenter.x + perpX * sideSign * step));
                const int tileY = static_cast<int>(std::lround(rowCenter.y + perpY * sideSign * step));
                if (!openTile(tileX, tileY))
                {
                    targetNode = rowCenter;
                    break;
                }
                targetNode = {tileX, tileY};
            }
        }
        OrderLeg(squad, {ordered[k]}, targetNode.x, targetNode.y, entities, pathfinder);
    }
    squad.orderTimer = 0.0f;
}

void AISquadController::AdvanceMarch(
    AISquad &squad,
    TileMap &map,
    EntityManager &entities,
    Pathfinder &pathfinder,
    const ChokepointMap &chokes,
    const AITacticsConfig &tactics)
{
    int lastIndex = static_cast<int>(squad.route.size()) - 1;
    // In front of the objective's defenses: line up out of their reach first, then go in together
    if (!squad.formedUp && squad.attackMove && !squad.playerSquad && lastIndex > 0)
    {
        const int formUpIndex = FormUpIndex(squad, entities);
        if (formUpIndex > squad.progress
            && formUpIndex <= std::min(lastIndex, squad.progress + std::max(2, tactics.marchStrideTiles) + 2))
        {
            squad.formedUp = true;
            squad.marchIndex = formUpIndex;
            SetState(squad, AISquadState::FormingUp);
            OrderMarch(squad, formUpIndex, map, entities, pathfinder, tactics);
            return;
        }
    }
    if (lastIndex <= 0 || squad.progress >= lastIndex)
    {
        OrderLeg(squad, squad.unitIds, squad.targetX, squad.targetY, entities, pathfinder);
        for (int unitId : squad.unitIds)
        {
            if (Unit *unit = entities.GetUnit(unitId))
                unit->flowMode = false;
        }
        SetState(squad, AISquadState::Assault);
        squad.orderTimer = 0.0f;
        if (squad.playerSquad)
        {
            // The player's group has arrived: the units finish the move on their own
            std::vector<int> allUnitIds = squad.unitIds;
            for (int unitId : allUnitIds)
                ReleaseUnit(squad, unitId, entities);
        }
        return;
    }

    int nextIndex = std::min(squad.progress + std::max(2, tactics.marchStrideTiles), lastIndex);

    // Look slightly past the next waypoint so the group never stops inside a corridor entrance
    int chokeIndex = -1;
    for (int i = squad.progress; i <= std::min(nextIndex + 2, lastIndex); ++i)
    {
        if (chokes.IsChoke(squad.route[i].x, squad.route[i].y))
        {
            chokeIndex = i;
            break;
        }
    }

    if (chokeIndex >= 0)
    {
        int rallyIndex = std::max(squad.progress, chokeIndex - 2);
        if (tactics.waitAtChokepoints && rallyIndex > squad.progress)
        {
            squad.marchIndex = rallyIndex;
            squad.deployIndex = chokeIndex; // remembered entry; the real deploy point is set by beginTransit
            OrderMarch(squad, rallyIndex, map, entities, pathfinder, tactics);
            SetState(squad, AISquadState::ChokeApproach);
            return;
        }
        BeginTransit(squad, chokeIndex, entities, pathfinder, chokes);
        return;
    }

    squad.marchIndex = nextIndex;
    OrderMarch(squad, nextIndex, map, entities, pathfinder, tactics);
    SetState(squad, AISquadState::Marching);
}

void AISquadController::BeginTransit(
    AISquad &squad, int chokeIndex, EntityManager &entities, Pathfinder &pathfinder, const ChokepointMap &chokes)
{
    int lastIndex = static_cast<int>(squad.route.size()) - 1;
    int exitIndex = chokeIndex;
    while (exitIndex <= lastIndex && chokes.IsChoke(squad.route[exitIndex].x, squad.route[exitIndex].y))
        ++exitIndex;
    int deployIndex = std::min(exitIndex + 4, lastIndex);

    squad.deployIndex = deployIndex;
    squad.marchIndex = deployIndex;

    // Single file: one shared destination, no sidesteps; reservations make the column queue up
    for (int unitId : squad.unitIds)
    {
        if (Unit *unit = entities.GetUnit(unitId))
            unit->flowMode = true;
    }
    const PathNode &deployNode = squad.route[deployIndex];
    OrderLeg(squad, squad.unitIds, deployNode.x, deployNode.y, entities, pathfinder);
    SetState(squad, AISquadState::ChokeTransit);
    squad.orderTimer = 0.0f;
}

// Everyone walks at its own speed. A member that got more than a leash ahead of the group's centre (along the way
// to the waypoint) stands on its tile until the ones behind have caught up (back within 0.6 leash: hysteresis, no
// stop-and-go). Nobody waits in combat, near the objective, or inside a narrow passage (it would block the way for
// the rest: it waits beyond it); a member behind that has not got any closer for kHoldStuckSeconds is not waited for.
void AISquadController::HoldVanguard(
    AISquad &squad,
    EntityManager &entities,
    const ChokepointMap &chokes,
    float centroidX,
    float centroidY,
    float leash,
    bool active,
    bool nearObjective,
    float deltaTime)
{
    constexpr float kHoldStuckSeconds = 4.0f;
    constexpr float kResumeFraction = 0.6f;

    float dirX = 0.0f, dirY = 0.0f;
    if (active && !nearObjective && !squad.route.empty())
    {
        const PathNode &waypoint
            = squad.route[std::clamp(squad.marchIndex, 0, static_cast<int>(squad.route.size()) - 1)];
        dirX = static_cast<float>(waypoint.x) - centroidX;
        dirY = static_cast<float>(waypoint.y) - centroidY;
        const float dirLength = DetMath::Hypot(dirX, dirY);
        if (dirLength > 0.5f)
        {
            dirX /= dirLength;
            dirY /= dirLength;
        }
        else
        {
            dirX = dirY = 0.0f;
        }
    }
    if (dirX == 0.0f && dirY == 0.0f)
    {
        for (int unitId : squad.unitIds)
            entities.GetUnit(unitId)->groupHold = false;
        squad.holdBest.clear();
        squad.holdTimer.clear();
        return;
    }

    // Members behind that are stuck (no headway for a while) are left out of the centre the vanguard waits for
    auto isStuck = [&](int unitId) {
        auto it = squad.holdTimer.find(unitId);
        return it != squad.holdTimer.end() && it->second > kHoldStuckSeconds;
    };
    for (int unitId : squad.unitIds)
    {
        const Unit *unit = entities.GetUnit(unitId);
        const float dist = DistTo(*unit, centroidX, centroidY);
        const float forwardOffset = (UnitTileX(*unit) - centroidX) * dirX + (UnitTileY(*unit) - centroidY) * dirY;
        if (dist <= leash || forwardOffset >= 0.0f)
        {
            squad.holdBest.erase(unitId);
            squad.holdTimer.erase(unitId);
            continue;
        }
        auto it = squad.holdBest.find(unitId);
        if (it == squad.holdBest.end() || dist < it->second - 0.5f)
        {
            squad.holdBest[unitId] = dist;
            squad.holdTimer[unitId] = 0.0f;
        }
        else
        {
            squad.holdTimer[unitId] += deltaTime;
        }
    }
    float refX = 0.0f, refY = 0.0f;
    int counted = 0;
    for (int unitId : squad.unitIds)
    {
        if (isStuck(unitId))
            continue;
        const Unit *unit = entities.GetUnit(unitId);
        refX += UnitTileX(*unit);
        refY += UnitTileY(*unit);
        ++counted;
    }
    if (counted > 0)
    {
        refX /= static_cast<float>(counted);
        refY /= static_cast<float>(counted);
    }
    else
    {
        refX = centroidX;
        refY = centroidY;
    }

    for (int unitId : squad.unitIds)
    {
        Unit *unit = entities.GetUnit(unitId);
        if (unit->moveSpeed <= 0.0f || unit->state != ActionState::Move || IsEngaged(*unit)
            || unit->moveFireTargetId != 0)
        {
            unit->groupHold = false;
            continue;
        }
        const int passageWidth = chokes.OpenWidth(unit->tileX, unit->tileY);
        if (passageWidth > 0 && passageWidth <= 4)
        {
            unit->groupHold = false; // inside a passage: goes through, waits beyond it
            continue;
        }
        const float forwardOffset = (UnitTileX(*unit) - refX) * dirX + (UnitTileY(*unit) - refY) * dirY;
        if (forwardOffset > leash)
            unit->groupHold = true;
        else if (forwardOffset <= kResumeFraction * leash)
            unit->groupHold = false;
    }
}

// ---------------------------------------------------------------------------
// Per-squad state machine (runs every kTickSeconds)
// ---------------------------------------------------------------------------

void AISquadController::Tick(
    AISquad &squad,
    float deltaTime,
    TileMap &map,
    EntityManager &entities,
    Pathfinder &pathfinder,
    const ChokepointMap &chokes,
    const AITacticsConfig &tactics)
{
    // Drop the dead
    squad.unitIds.erase(
        std::remove_if(
            squad.unitIds.begin(),
            squad.unitIds.end(),
            [&](int unitId) {
                const Unit *unit = entities.GetUnit(unitId);
                return !unit || !unit->alive || unit->state == ActionState::Die;
            }),
        squad.unitIds.end());
    if (squad.unitIds.empty())
        return;

    if (!entities.GetUnit(squad.leaderId)
        || std::find(squad.unitIds.begin(), squad.unitIds.end(), squad.leaderId) == squad.unitIds.end())
    {
        squad.leaderId = PickLeader(squad, entities, tactics);
    }
    const Unit *leader = entities.GetUnit(squad.leaderId);
    if (!leader)
        return;

    const int localWidth = chokes.OpenWidth(leader->tileX, leader->tileY);
    const float leash = EffectiveLeash(tactics, squad.unitIds.size(), localWidth > 0 && localWidth <= 4);
    squad.leashRadius = leash;
    // Also catches re-paths the units made on their own (blocked by the crowd)
    CapDetours(squad, entities, &map);

    // Cohesion metrics: centroid of the group core and the farthest unit from it (Dmax).
    // The core is everyone within two leash radii of the leader, so one far straggler
    // does not drag the centre away from the rest of the group.
    float centroidX = 0.0f, centroidY = 0.0f;
    int coreCount = 0;
    for (int unitId : squad.unitIds)
    {
        const Unit *unit = entities.GetUnit(unitId);
        if (DistTo(*unit, UnitTileX(*leader), UnitTileY(*leader)) > 2.0f * leash)
            continue;
        centroidX += UnitTileX(*unit);
        centroidY += UnitTileY(*unit);
        ++coreCount;
    }
    centroidX /= static_cast<float>(std::max(1, coreCount));
    centroidY /= static_cast<float>(std::max(1, coreCount));
    float maxSpread = 0.0f;
    bool engaged = false;
    for (int unitId : squad.unitIds)
    {
        const Unit *unit = entities.GetUnit(unitId);
        maxSpread = std::max(maxSpread, DistTo(*unit, centroidX, centroidY));
        engaged = engaged || IsEngaged(*unit);
    }
    squad.centroidX = centroidX;
    squad.centroidY = centroidY;
    squad.maxSpread = maxSpread;
    // Members clearly outside the leash (stop) and outside it at all (resume): a few stragglers do not
    // hold the whole group up, they catch up while the front slows down
    int outsideStop = 0, outsideResume = 0;
    for (int unitId : squad.unitIds)
    {
        const float dist = DistTo(*entities.GetUnit(unitId), centroidX, centroidY);
        if (dist > leash * 1.2f)
            ++outsideStop;
        if (dist > leash)
            ++outsideResume;
    }
    const int toleratedStragglers = std::max(1, static_cast<int>(squad.unitIds.size()) / 4);
    // Close to the objective the group goes straight into the attack instead of waiting
    const int remainingRoute = static_cast<int>(squad.route.size()) - 1 - squad.progress;
    const bool nearObjective = remainingRoute <= tactics.marchStrideTiles * 3 / 2
                               || DetMath::Hypot(centroidX - squad.targetX, centroidY - squad.targetY) <= leash;
    HoldVanguard(
        squad,
        entities,
        chokes,
        centroidX,
        centroidY,
        leash,
        squad.state == AISquadState::Marching || squad.state == AISquadState::ChokeApproach,
        nearObjective,
        deltaTime);

    squad.stateTimer += deltaTime;
    squad.orderTimer += deltaTime;

    // First leg after launch
    if (squad.state == AISquadState::Marching && squad.orderTimer > 1e5f)
    {
        AdvanceMarch(squad, map, entities, pathfinder, chokes, tactics);
        return;
    }

    // A plain move (player group) ignores enemies on the way: nobody stops to pick a fight
    if (!squad.attackMove)
    {
        for (int unitId : squad.unitIds)
        {
            Unit *unit = entities.GetUnit(unitId);
            if (unit->targetEntityId != 0 || unit->targetWallTileX >= 0)
            {
                unit->targetEntityId = 0;
                unit->targetWallTileX = unit->targetWallTileY = -1;
                if (unit->state == ActionState::Attack)
                    unit->state = ActionState::Idle;
                if (squad.state == AISquadState::Marching && unitId != squad.leaderId)
                {
                    OrderLeg(squad, {unitId}, leader->tileX, leader->tileY, entities, pathfinder);
                }
            }
        }
        engaged = false;
    }

    if (!squad.playerSquad
        && (squad.state == AISquadState::Engaged || squad.state == AISquadState::Marching
            || squad.state == AISquadState::Regrouping || squad.state == AISquadState::FormingUp
            || squad.state == AISquadState::Assault))
        UpdateFocusTarget(squad, map, entities, pathfinder);
    else
        squad.focusTargetId = 0;

    // Combat pauses the cohesion logic (except inside a corridor, where the column must keep flowing)
    if (engaged
        && (squad.state == AISquadState::Marching || squad.state == AISquadState::Regrouping
            || squad.state == AISquadState::ChokeApproach || squad.state == AISquadState::Deploying
            || squad.state == AISquadState::FormingUp))
    {
        squad.resumeState = squad.state;
        SetState(squad, AISquadState::Engaged);
        return;
    }

    // Returns true when at least one straggler got closer this tick
    auto trackStragglers = [&](float radius, float refCentroidX, float refCentroidY) -> bool {
        std::vector<int> unitsToDrop;
        bool madeProgress = false;
        for (int unitId : squad.unitIds)
        {
            Unit *unit = entities.GetUnit(unitId);
            float dist = DistTo(*unit, refCentroidX, refCentroidY);
            if (dist <= radius)
            {
                squad.stragglerBest.erase(unitId);
                squad.stragglerTimer.erase(unitId);
                continue;
            }
            auto it = squad.stragglerBest.find(unitId);
            if (it == squad.stragglerBest.end() || dist < it->second - 0.5f)
            {
                squad.stragglerBest[unitId] = dist;
                squad.stragglerTimer[unitId] = 0.0f;
                madeProgress = true;
            }
            else
            {
                squad.stragglerTimer[unitId] += deltaTime;
                // Only a unit that is really far and not closing in leaves the group; one standing at the
                // edge of the crowd simply cannot get any closer
                if (squad.stragglerTimer[unitId] > tactics.stragglerTimeoutSeconds && dist > radius * 1.5f)
                    unitsToDrop.push_back(unitId);
            }
            // Highest priority for the straggler: walk straight to the leader
            if (IsIdleWithoutOrders(*unit) && unitId != squad.leaderId)
            {
                OrderLeg(squad, {unitId}, leader->tileX, leader->tileY, entities, pathfinder);
            }
        }
        for (int unitId : unitsToDrop)
        {
            ReleaseUnit(squad, unitId, entities);
            // The player's unit is not left standing: it goes on to the destination on its own
            if (squad.playerSquad)
                OrderLeg(squad, {unitId}, squad.targetX, squad.targetY, entities, pathfinder);
        }
        return madeProgress;
    };

    switch (squad.state)
    {
        case AISquadState::Marching:
        {
            // The whole group does not stop for stragglers any more: only the ones too far ahead wait for them
            // (HoldVanguard), everyone else keeps walking at its own speed

            if (outsideStop > 0)
                trackStragglers(leash * 1.2f, centroidX, centroidY);

            const PathNode &waypoint
                = squad.route.empty() ? PathNode {squad.targetX, squad.targetY} : squad.route[squad.marchIndex];
            bool reached;
            // A unit that stopped at its formation slot around the waypoint has arrived as well
            auto arrived = [&](const Unit &unit, float radius) {
                float dist = DistTo(unit, static_cast<float>(waypoint.x), static_cast<float>(waypoint.y));
                return dist <= radius || (IsIdleWithoutOrders(unit) && dist <= std::max(radius, leash) * 1.5f);
            };
            if (tactics.syncToSlowest)
            {
                reached = arrived(*leader, 2.5f);
                for (int unitId : squad.unitIds)
                {
                    if (!reached)
                        break;
                    reached = arrived(*entities.GetUnit(unitId), leash);
                }
            }
            else
            {
                reached = arrived(*leader, 2.5f);
            }
            // Nobody makes headway any more (crowd settled around the waypoint): good enough
            if (DetMath::Hypot(centroidX - squad.stallX, centroidY - squad.stallY) > 0.5f)
            {
                squad.stallX = centroidX;
                squad.stallY = centroidY;
                squad.stallTimer = 0.0f;
            }
            else
            {
                squad.stallTimer += deltaTime;
            }
            if (!reached && squad.stallTimer > 2.0f
                && DistTo(*leader, static_cast<float>(waypoint.x), static_cast<float>(waypoint.y))
                       <= std::max(2.5f, leash))
            {
                reached = true;
            }
            if (reached)
                squad.stallTimer = 0.0f;

            if (reached)
            {
                squad.progress = squad.marchIndex;
                AdvanceMarch(squad, map, entities, pathfinder, chokes, tactics);
            }
            else if (squad.stateTimer > 25.0f)
            {
                // Waypoint unreachable (blocked by new structures): plan again from the leader
                RebuildRoute(squad, entities, pathfinder);
                AdvanceMarch(squad, map, entities, pathfinder, chokes, tactics);
            }
            else if (squad.orderTimer > 4.0f)
            {
                if (IsIdleWithoutOrders(*leader))
                {
                    // Orders were overridden elsewhere (base defense, call for help): re-issue the leg
                    OrderMarch(squad, squad.marchIndex, map, entities, pathfinder, tactics);
                }
                else
                {
                    std::vector<int> idleMembers;
                    for (int unitId : squad.unitIds)
                    {
                        const Unit *unit = entities.GetUnit(unitId);
                        if (unit && IsIdleWithoutOrders(*unit) && !arrived(*unit, 2.5f))
                            idleMembers.push_back(unitId);
                    }
                    if (!idleMembers.empty())
                    {
                        OrderLeg(squad, idleMembers, waypoint.x, waypoint.y, entities, pathfinder);
                        squad.orderTimer = 0.0f;
                    }
                }
            }
            break;
        }

        case AISquadState::Regrouping:
        {
            // Back together, or compacted as far as the crowd allows (no one closed in for 1.5 s)
            const bool compacted = squad.quietTimer >= 1.5f && maxSpread <= leash * 1.5f;
            if (outsideResume <= toleratedStragglers || compacted)
            {
                squad.stragglerBest.clear();
                squad.stragglerTimer.clear();
                OrderMarch(squad, squad.marchIndex, map, entities, pathfinder, tactics);
                SetState(squad, AISquadState::Marching);
                break;
            }
            squad.quietTimer = trackStragglers(leash, centroidX, centroidY) ? 0.0f : squad.quietTimer + deltaTime;
            break;
        }

        case AISquadState::ChokeApproach:
        {
            const PathNode &rallyNode = squad.route[squad.marchIndex];
            bool leaderAtRally
                = DistTo(*leader, static_cast<float>(rallyNode.x), static_cast<float>(rallyNode.y)) <= 2.5f;
            squad.quietTimer = leaderAtRally ? squad.quietTimer + deltaTime : 0.0f; // hold time at the rally point
            bool isGathered = maxSpread <= leash;
            if ((leaderAtRally && squad.quietTimer >= tactics.preChokeHoldSeconds && isGathered)
                || squad.stateTimer > 12.0f)
            {
                squad.progress = squad.marchIndex;
                BeginTransit(squad, squad.deployIndex, entities, pathfinder, chokes);
                break;
            }
            if (leaderAtRally && !isGathered)
                trackStragglers(leash, centroidX, centroidY);
            break;
        }

        case AISquadState::ChokeTransit:
        {
            const PathNode &deployNode = squad.route[squad.deployIndex];
            bool allOut = true;
            for (int unitId : squad.unitIds)
            {
                const Unit *unit = entities.GetUnit(unitId);
                if (chokes.IsChoke(unit->tileX, unit->tileY)
                    || DistTo(*unit, static_cast<float>(deployNode.x), static_cast<float>(deployNode.y)) > leash + 1.5f)
                {
                    allOut = false;
                }
            }
            float timeout = std::max(15.0f, 2.0f * static_cast<float>(squad.unitIds.size()));
            if (allOut || squad.stateTimer > timeout)
            {
                if (!allOut)
                {
                    // Whoever is still stuck in the passage is left behind; the group moves on
                    std::vector<int> stuckUnitIds;
                    for (int unitId : squad.unitIds)
                    {
                        const Unit *unit = entities.GetUnit(unitId);
                        if (DistTo(*unit, static_cast<float>(deployNode.x), static_cast<float>(deployNode.y))
                            > leash + 1.5f)
                            stuckUnitIds.push_back(unitId);
                    }
                    for (int unitId : stuckUnitIds)
                        ReleaseUnit(squad, unitId, entities);
                }
                for (int unitId : squad.unitIds)
                {
                    if (Unit *unit = entities.GetUnit(unitId))
                        unit->flowMode = false;
                }
                SetState(squad, AISquadState::Deploying);
                break;
            }
            if (squad.orderTimer > 3.0f)
            {
                std::vector<int> idleUnitIds;
                for (int unitId : squad.unitIds)
                {
                    const Unit *unit = entities.GetUnit(unitId);
                    if (IsIdleWithoutOrders(*unit)
                        && DistTo(*unit, static_cast<float>(deployNode.x), static_cast<float>(deployNode.y)) > 2.0f)
                    {
                        idleUnitIds.push_back(unitId);
                    }
                }
                if (!idleUnitIds.empty())
                    OrderLeg(squad, idleUnitIds, deployNode.x, deployNode.y, entities, pathfinder);
                squad.orderTimer = 0.0f;
            }
            break;
        }

        case AISquadState::Deploying:
        {
            if (!tactics.waitAtChokepoints || maxSpread <= leash || squad.stateTimer > 6.0f)
            {
                squad.progress = squad.deployIndex;
                AdvanceMarch(squad, map, entities, pathfinder, chokes, tactics);
            }
            break;
        }

        case AISquadState::FormingUp:
        {
            // Lined up (or as close as the ground allows): everybody goes in at once
            const PathNode &formUpNode
                = squad.route[std::clamp(squad.marchIndex, 0, static_cast<int>(squad.route.size()) - 1)];
            const bool leaderAtFormUp
                = DistTo(*leader, static_cast<float>(formUpNode.x), static_cast<float>(formUpNode.y))
                  <= std::max(2.5f, leash);
            squad.quietTimer = (leaderAtFormUp && maxSpread <= leash * 1.5f) ? squad.quietTimer + deltaTime : 0.0f;
            if (squad.quietTimer >= 1.0f || squad.stateTimer > 12.0f)
            {
                squad.progress = squad.marchIndex;
                AdvanceMarch(squad, map, entities, pathfinder, chokes, tactics);
                break;
            }
            if (leaderAtFormUp)
                trackStragglers(leash * 1.5f, centroidX, centroidY);
            else if (squad.orderTimer > 4.0f && IsIdleWithoutOrders(*leader))
                OrderMarch(squad, squad.marchIndex, map, entities, pathfinder, tactics);
            break;
        }

        case AISquadState::Engaged:
        {
            if (engaged)
            {
                squad.quietTimer = 0.0f;
                // The whole wave answers: idle members join the ones already fighting
                if (squad.orderTimer > 1.0f)
                {
                    std::vector<int> idleUnitIds;
                    int fightX = leader->tileX, fightY = leader->tileY;
                    for (int unitId : squad.unitIds)
                    {
                        const Unit *unit = entities.GetUnit(unitId);
                        if (IsEngaged(*unit))
                        {
                            fightX = unit->tileX;
                            fightY = unit->tileY;
                        }
                    }
                    for (int unitId : squad.unitIds)
                    {
                        const Unit *unit = entities.GetUnit(unitId);
                        if (IsIdleWithoutOrders(*unit))
                            idleUnitIds.push_back(unitId);
                    }
                    if (!idleUnitIds.empty())
                        OrderLeg(squad, idleUnitIds, fightX, fightY, entities, pathfinder);
                    squad.orderTimer = 0.0f;
                }
            }
            else
            {
                squad.quietTimer += deltaTime;
                if (squad.quietTimer >= 2.0f)
                {
                    squad.leaderId = PickLeader(squad, entities, tactics);
                    RebuildRoute(squad, entities, pathfinder);
                    AdvanceMarch(squad, map, entities, pathfinder, chokes, tactics);
                }
            }
            break;
        }

        case AISquadState::Assault:
        {
            if (squad.orderTimer < 3.0f)
                break;
            squad.orderTimer = 0.0f;
            std::vector<int> idleUnitIds;
            for (int unitId : squad.unitIds)
            {
                const Unit *unit = entities.GetUnit(unitId);
                if (IsIdleWithoutOrders(*unit))
                    idleUnitIds.push_back(unitId);
            }
            if (idleUnitIds.empty())
                break;

            // Objective gone: move on to the nearest remaining hostile structure, or hostile unit if none remain
            int bestX = -1, bestY = -1;
            float bestDist = 1e9f;
            bool foundBuilding = false;
            for (const auto &entity : entities.GetAllEntities())
            {
                if (!entity.alive || entity.state == ActionState::Die || entity.owner >= kMaxPlayers
                    || !Players::Hostile(entity.owner, squad.owner))
                {
                    continue;
                }
                const float dist = DetMath::Hypot(entity.tileX - centroidX, entity.tileY - centroidY);
                if ((entity.isBuilding && !foundBuilding) || (entity.isBuilding == foundBuilding && dist < bestDist))
                {
                    foundBuilding = entity.isBuilding;
                    bestDist = dist;
                    bestX = entity.tileX;
                    bestY = entity.tileY;
                }
            }
            if (bestX >= 0)
            {
                const bool targetChanged = (squad.targetX != bestX || squad.targetY != bestY);
                squad.targetX = bestX;
                squad.targetY = bestY;
                if (targetChanged && bestDist > 20.0f)
                {
                    squad.leaderId = PickLeader(squad, entities, tactics);
                    RebuildRoute(squad, entities, pathfinder);
                    AdvanceMarch(squad, map, entities, pathfinder, chokes, tactics);
                }
                else
                {
                    OrderLeg(squad, idleUnitIds, bestX, bestY, entities, pathfinder);
                }
            }
            else if (idleUnitIds.size() == squad.unitIds.size())
            {
                std::vector<int> allUnitIds = squad.unitIds;
                for (int unitId : allUnitIds)
                    ReleaseUnit(squad, unitId, entities);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Debug visualization
// ---------------------------------------------------------------------------

void AISquadController::RenderDebug(
    Renderer &renderer,
    BitmapFontRenderer &font,
    const Camera &camera,
    const EntityManager &entities,
    float leashTiles) const
{
    for (const auto &squad : m_squads)
    {
        const Entity *leader = entities.GetEntity(squad.leaderId);
        if (!leader)
            continue;
        float leaderX = leader->GetUnitCenterPixelX();
        float leaderY = leader->GetUnitCenterPixelY();

        // Leash lines: green = in formation, yellow = at the limit, red = straggling
        for (int unitId : squad.unitIds)
        {
            const Entity *member = entities.GetEntity(unitId);
            if (!member || unitId == squad.leaderId)
                continue;
            float distTiles
                = DetMath::Hypot(member->GetUnitCenterPixelX() - leaderX, member->GetUnitCenterPixelY() - leaderY)
                  / TILE_WIDTH;
            SDL_Color lineColor = (distTiles <= 2.0f) ? SDL_Color {60, 230, 90, 220}
                                  : (distTiles <= std::max(leashTiles, squad.leashRadius))
                                      ? SDL_Color {240, 210, 60, 220}
                                      : SDL_Color {240, 60, 50, 230};
            renderer.DrawLineWorld(
                member->GetUnitCenterPixelX(), member->GetUnitCenterPixelY(), leaderX, leaderY, camera, lineColor);
        }

        // Centroid and the allowed cohesion radius
        float centroidPixelX = squad.centroidX * TILE_WIDTH + TILE_WIDTH * 0.5f;
        float centroidPixelY = squad.centroidY * TILE_HEIGHT + TILE_HEIGHT * 0.5f;
        float radius = squad.leashRadius;
        RenderUtility::DrawTileRadius(
            renderer, camera, centroidPixelX, centroidPixelY, radius, SDL_Color {255, 255, 255, 120}, 1.5f);
        renderer.FillRectWorld(centroidPixelX - 3, centroidPixelY - 3, 6, 6, camera, {255, 255, 255, 230});

        // Current waypoint
        if (!squad.route.empty())
        {
            const PathNode &waypoint
                = squad.route[std::clamp(squad.marchIndex, 0, static_cast<int>(squad.route.size()) - 1)];
            renderer.DrawRectWorld(
                waypoint.x * TILE_WIDTH + 4.0f,
                waypoint.y * TILE_HEIGHT + 4.0f,
                TILE_WIDTH - 8.0f,
                TILE_HEIGHT - 8.0f,
                camera,
                {120, 200, 255, 220});
        }

        // "G<n>" label above the leader with the current state
        float screenX, screenY;
        camera.WorldToScreen(leaderX, leaderY - TILE_HEIGHT * 1.2f, screenX, screenY);
        std::string label = "G" + std::to_string(squad.id);
        int labelWidth = font.GetTextWidth(label, 6) + 10;
        renderer.FillRectScreen(
            static_cast<int>(screenX) - labelWidth / 2,
            static_cast<int>(screenY) - 10,
            labelWidth,
            20,
            {12, 14, 20, 210});
        renderer.DrawRectScreen(
            static_cast<int>(screenX) - labelWidth / 2,
            static_cast<int>(screenY) - 10,
            labelWidth,
            20,
            {232, 176, 74, 255});
        font.DrawString(
            label,
            static_cast<int>(screenX),
            static_cast<int>(screenY),
            6,
            {232, 176, 74, 255},
            false,
            Anchor::HCENTER | Anchor::VCENTER);
        std::string info = std::string(SquadStateName(squad.state)) + "  " + std::to_string(squad.unitIds.size()) + "/"
                           + std::to_string(squad.initialSize);
        font.DrawString(
            info,
            static_cast<int>(screenX),
            static_cast<int>(screenY) + 18,
            1,
            {226, 231, 238, 255},
            true,
            Anchor::HCENTER | Anchor::VCENTER);
    }
}

} // namespace OpenArtOfWar
