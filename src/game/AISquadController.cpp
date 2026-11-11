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
float UnitTileX(const Unit &u)
{
    return u.GetWorldPixelX() / TILE_WIDTH;
}
float UnitTileY(const Unit &u)
{
    return u.GetWorldPixelY() / TILE_HEIGHT;
}

float DistTo(const Unit &u, float x, float y)
{
    return DetMath::Hypot(UnitTileX(u) - x, UnitTileY(u) - y);
}

bool IsEngaged(const Unit &u)
{
    return u.targetEntityId != 0 || u.targetWallTileX >= 0 || u.state == ActionState::Attack;
}

bool IsIdleWithoutOrders(const Unit &u)
{
    return u.state == ActionState::Idle && u.path.empty() && !u.waitingForPath && u.targetEntityId == 0;
}

// Both factions: Fortress / Hammer / Zeus and Coyote / Armadillo / Rhino
bool IsTank(int arch)
{
    return arch == 6 || arch == 7 || arch == 8 || arch == 9 || arch == 10 || arch == 11 || arch == 18;
}
bool IsMelee(int arch)
{
    return arch == 5;
} // flamethrower
// Rocket infantry and MLRS of both factions
bool IsRangedSupport(int arch)
{
    return arch == 2 || arch == 3 || arch == 12 || arch == 13 || arch == 16;
}

// Cohesion radius for a group of n units. Big groups get a proportionally larger radius (a column of
// 15 is naturally longer than 3 tiles), and narrow passages stretch any group into a line.
float EffectiveLeash(const AITacticsConfig &t, std::size_t n, bool inCorridor)
{
    const float count = static_cast<float>(n);
    float leash = t.leashTiles + t.leashPerUnit * std::max(0.0f, count - 4.0f);
    leash = std::max(leash, std::sqrt(count) * 0.8f + 1.0f); // never tighter than the formation itself
    if (inCorridor)
        leash *= std::max(1.0f, t.corridorLeashFactor);
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

    auto open = [&](int x, int y) {
        return map.IsPassable(x, y) && map.GetWall(x, y) == 0;
    };

    // Length of the contiguous open run through every tile, horizontally and vertically
    std::vector<int> runH(m_choke.size(), 0);
    std::vector<int> runV(m_choke.size(), 0);
    for (int y = 0; y < m_height; ++y)
    {
        int x = 0;
        while (x < m_width)
        {
            if (!open(x, y))
            {
                ++x;
                continue;
            }
            int start = x;
            while (x < m_width && open(x, y))
                ++x;
            for (int i = start; i < x; ++i)
                runH[y * m_width + i] = x - start;
        }
    }
    for (int x = 0; x < m_width; ++x)
    {
        int y = 0;
        while (y < m_height)
        {
            if (!open(x, y))
            {
                ++y;
                continue;
            }
            int start = y;
            while (y < m_height && open(x, y))
                ++y;
            for (int i = start; i < y; ++i)
                runV[i * m_width + x] = y - start;
        }
    }

    // A corridor tile is at most 2 tiles wide across and at least 3 tiles long along the passage
    for (std::size_t i = 0; i < m_choke.size(); ++i)
    {
        int narrow = std::min(runH[i], runV[i]);
        int along = std::max(runH[i], runV[i]);
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
    for (const auto &sq : m_squads)
    {
        if (std::find(sq.unitIds.begin(), sq.unitIds.end(), unitId) != sq.unitIds.end())
            return true;
    }
    return false;
}

std::vector<int> AISquadController::TakeReleasedUnits()
{
    std::vector<int> out;
    out.swap(m_released);
    return out;
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

    AISquad sq;
    sq.id = m_nextSquadId++;
    sq.waveNumber = waveNumber;
    sq.unitIds = unitIds;
    sq.targetX = targetX;
    sq.targetY = targetY;
    sq.viaX = viaX;
    sq.viaY = viaY;
    sq.initialSize = static_cast<int>(unitIds.size());
    sq.leaderId = PickLeader(sq, entities, tactics);
    for (int id : unitIds)
    {
        if (const Entity *e = entities.GetEntity(id))
        {
            sq.owner = e->owner;
            break;
        }
    }

    RebuildRoute(sq, entities, pathfinder);
    sq.state = AISquadState::Marching;
    sq.orderTimer = 1e6f; // the first leg is ordered on the next tick, which has the map at hand
    m_squads.push_back(std::move(sq));
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
    float dt = m_tickAccumulator;
    m_tickAccumulator = 0.0f;

    for (auto &sq : m_squads)
    {
        Tick(sq, dt, map, entities, pathfinder, chokes, tactics);
    }
    m_squads.erase(
        std::remove_if(m_squads.begin(), m_squads.end(), [](const AISquad &s) { return s.unitIds.empty(); }),
        m_squads.end());
}

AISquad *AISquadController::FindSquad(int squadId)
{
    for (auto &sq : m_squads)
    {
        if (sq.id == squadId)
            return &sq;
    }
    return nullptr;
}

bool AISquadController::AddUnits(int squadId, const std::vector<int> &unitIds, const EntityManager &entities)
{
    for (auto &sq : m_squads)
    {
        if (sq.id != squadId)
            continue;
        for (int id : unitIds)
        {
            const Unit *u = entities.GetUnit(id);
            if (!u || !u->alive || u->state == ActionState::Die || u->owner != sq.owner || IsSquadMember(id))
                continue;
            sq.unitIds.push_back(id);
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
    for (const AISquad &sq : m_squads)
    {
        sink.Record("squad");
        sink.I32("id", sq.id);
        sink.I32("wave", sq.waveNumber);
        sink.I32("owner", sq.owner);
        sink.I32("state", static_cast<std::int32_t>(sq.state));
        sink.I32("resume", static_cast<std::int32_t>(sq.resumeState));
        sink.I32("leader", sq.leaderId);
        sink.I32("tx", sq.targetX);
        sink.I32("ty", sq.targetY);
        sink.I32("progress", sq.progress);
        sink.I32("march", sq.marchIndex);
        sink.I32("focus", sq.focusTargetId);
        sink.I32("flags", (sq.attackMove ? 1 : 0) | (sq.playerSquad ? 2 : 0) | (sq.formedUp ? 4 : 0));
        sink.F32("stateTimer", sq.stateTimer);
        sink.F32("orderTimer", sq.orderTimer);
        sink.F32("leash", sq.leashRadius);
        for (int id : sq.unitIds)
            sink.I32("u", id);
    }
}

void AISquadController::ReleaseUnits(const std::vector<int> &unitIds, EntityManager &entities)
{
    for (auto &sq : m_squads)
    {
        for (int id : unitIds)
        {
            if (std::find(sq.unitIds.begin(), sq.unitIds.end(), id) != sq.unitIds.end())
                ReleaseUnit(sq, id, entities);
        }
        if (!sq.unitIds.empty() && std::find(sq.unitIds.begin(), sq.unitIds.end(), sq.leaderId) == sq.unitIds.end())
        {
            sq.leaderId = sq.unitIds.front();
        }
    }
    m_squads.erase(
        std::remove_if(m_squads.begin(), m_squads.end(), [](const AISquad &s) { return s.unitIds.empty(); }),
        m_squads.end());
}

void AISquadController::OrderLeg(
    const AISquad &sq, const std::vector<int> &ids, int x, int y, EntityManager &entities, Pathfinder &pathfinder)
{
    // The computer always goes by auto-attack (units answer fire on the way); a plain Move is only for a retreat
    (void) sq;
    entities.IssueAttackMoveOrder(ids, x, y, pathfinder);
    CapDetours(sq, entities, pathfinder.GetMap());
}

void AISquadController::CapDetours(const AISquad &sq, EntityManager &entities, const TileMap *map)
{
    // A hostile wall within a few tiles of the unit: worth breaching instead of the detour
    auto hostileWallNear = [&](const Unit &u) {
        if (!map)
            return false;
        constexpr int r = 6;
        for (int y = std::max(0, u.tileY - r); y <= std::min(map->GetHeight() - 1, u.tileY + r); ++y)
        {
            for (int x = std::max(0, u.tileX - r); x <= std::min(map->GetWidth() - 1, u.tileX + r); ++x)
            {
                if (map->GetWall(x, y) > 0 && Players::Hostile(map->GetWallOwner(x, y), u.owner))
                    return true;
            }
        }
        return false;
    };
    const Unit *leader = entities.GetUnit(sq.leaderId);
    if (!leader)
        return;
    const float leash = std::max(3.0f, sq.leashRadius);
    std::vector<int> hold;
    for (int id : sq.unitIds)
    {
        if (id == sq.leaderId)
            continue;
        const Unit *u = entities.GetUnit(id);
        if (!u || u->path.size() <= u->pathIndex)
            continue;
        const PathNode &goal = u->path.back();
        const float straight
            = DetMath::Hypot(static_cast<float>(goal.x - u->tileX), static_cast<float>(goal.y - u->tileY));
        const float pathLen = static_cast<float>(u->path.size() - u->pathIndex);
        // Allowed: the straight way, some slack for small obstacles, plus the group's leash
        const float limit = std::max(6.0f, straight * 1.8f + leash * 1.5f);
        // Only units already with the group: a straggler far away must still be allowed to come back
        const float toLeader = DistTo(*u, static_cast<float>(leader->tileX), static_cast<float>(leader->tileY));
        // A map-wide detour is never worth it, wherever the leader is
        const bool mapWide = pathLen > straight * 3.0f + 20.0f;
        if (pathLen > limit && (toLeader <= leash * 2.0f || mapWide))
        {
            if (hostileWallNear(*u))
            {
                // Keep the goal, drop the long route: the unit's own breach check picks a wall next tick
                Unit *mu = entities.GetUnit(id);
                mu->ClearPathState();
                if (mu->state == ActionState::Move)
                    mu->state = ActionState::Idle;
                mu->waitingForPath = true;
                mu->wallCheckCooldown = 0.0f;
                continue;
            }
            hold.push_back(id);
        }
    }
    if (!hold.empty())
        HoldUnits(hold, entities);
}

void AISquadController::SetState(AISquad &sq, AISquadState state)
{
    sq.state = state;
    sq.stateTimer = 0.0f;
    sq.quietTimer = 0.0f;
}

void AISquadController::UpdateFocusTarget(AISquad &sq, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
{
    const Unit *leader = entities.GetUnit(sq.leaderId);
    if (!leader || !leader->alive)
        return;
    const float zone = std::max(static_cast<float>(leader->sightRadius), leader->attackRange);
    auto distToLeader = [&](const Entity &e) {
        return DetMath::Hypot(static_cast<float>(e.tileX - leader->tileX), static_cast<float>(e.tileY - leader->tileY));
    };
    auto usable = [&](const Entity &e) {
        return e.alive && e.state != ActionState::Die && e.faction != Faction::Neutral
               && Players::Hostile(e.owner, sq.owner) && distToLeader(e) <= zone
               && entities.IsEntityVisibleToPlayer(e, sq.owner, map);
    };
    // The target stays while it lives and is in sight
    const Entity *focus = sq.focusTargetId != 0 ? entities.GetEntity(sq.focusTargetId) : nullptr;
    if (!focus || !usable(*focus))
    {
        focus = nullptr;
        int bestTier = 99;
        float bestDist = 1e9f;
        for (const Entity &e : entities.GetAllEntities())
        {
            if (!usable(e))
                continue;
            const int tier = TargetPriorityTier(e);
            const float d = distToLeader(e);
            if (tier < bestTier || (tier == bestTier && d < bestDist))
            {
                bestTier = tier;
                bestDist = d;
                focus = &e;
            }
        }
        sq.focusTargetId = focus ? focus->id : 0;
    }
    if (!focus)
        return;
    // Members that move and can get there fire at the group's target; siege and immobile units, and those with no way
    // to it, keep to their own
    std::vector<int> order;
    for (int id : sq.unitIds)
    {
        const Unit *u = entities.GetUnit(id);
        if (!u || !u->alive || u->state == ActionState::Die || u->targetEntityId == sq.focusTargetId)
            continue;
        if (u->isSiegeMode || u->moveSpeed <= 0.0f || u->HoldsGround() || u->attackDamage <= 0)
            continue;
        if (!u->HasOpenRouteToEntity(*focus, pathfinder))
            continue;
        order.push_back(id);
    }
    if (!order.empty())
        entities.IssueAttackOrder(order, sq.focusTargetId, &pathfinder);
}

void AISquadController::ReleaseUnit(AISquad &sq, int unitId, EntityManager &entities)
{
    sq.unitIds.erase(std::remove(sq.unitIds.begin(), sq.unitIds.end(), unitId), sq.unitIds.end());
    sq.stragglerBest.erase(unitId);
    sq.stragglerTimer.erase(unitId);
    if (Unit *u = entities.GetUnit(unitId))
    {
        if (sq.focusTargetId != 0 && u->orderedTargetId == sq.focusTargetId)
            u->orderedTargetId = 0; // no longer the group's order
        u->flowMode = false;
        u->groupHold = false;
        m_released.push_back(unitId);
    }
}

int AISquadController::PickLeader(const AISquad &sq, EntityManager &entities, const AITacticsConfig &tactics) const
{
    int best = 0;
    float bestScore = 1e9f;
    for (int id : sq.unitIds)
    {
        const Unit *u = entities.GetUnit(id);
        if (!u || !u->alive || u->state == ActionState::Die)
            continue;
        float score;
        if (tactics.syncToSlowest)
        {
            // Slowest unit sets the pace; prefer armor on ties so the column is led by a tank
            score = u->moveSpeed - (IsTank(u->archetypeId) ? 0.01f : 0.0f);
        }
        else
        {
            // Head of the column: the unit closest to the objective
            score = DistTo(*u, static_cast<float>(sq.targetX), static_cast<float>(sq.targetY));
        }
        if (score < bestScore)
        {
            bestScore = score;
            best = id;
        }
    }
    return best;
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
    const int w = map.GetWidth(), h = map.GetHeight();
    if (w <= 0 || h <= 0 || startX < 0 || startY < 0 || startX >= w || startY >= h)
        return false;
    goalX = std::clamp(goalX, 0, w - 1);
    goalY = std::clamp(goalY, 0, h - 1);
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    auto at = [w](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
    };

    // Tuning (integer costs, a straight step = 10)
    constexpr int kStraight = 10, kDiagonal = 14;
    constexpr int kNextToObstacle = 6; // a tile touching a cliff, wall or building
    constexpr int kNearObstacle = 2;   // two tiles away from one
    constexpr int kUnderFire = 25;     // within reach of a known enemy defense
    constexpr int kHostileWall = 120;  // shooting through a wall segment ~ a twelve-tile detour

    // 0 open, 1 blocked, 2 hostile wall (passable at a price)
    std::vector<std::uint8_t> kind(n, 0);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            if (map.GetWall(x, y) > 0)
                kind[at(x, y)] = Players::Hostile(map.GetWallOwner(x, y), owner) ? 2 : 1;
            else if (!map.IsPassable(x, y))
                kind[at(x, y)] = 1;
        }
    }
    std::vector<int> extra(n, 0);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            if (kind[at(x, y)] != 0)
                continue;
            int near1 = 0, near2 = 0;
            for (int dy = -2; dy <= 2; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    const int nx = x + dx, ny = y + dy;
                    const bool blocked = nx < 0 || ny < 0 || nx >= w || ny >= h || kind[at(nx, ny)] != 0;
                    if (!blocked)
                        continue;
                    (std::max(std::abs(dx), std::abs(dy)) == 1 ? near1 : near2) += 1;
                }
            }
            extra[at(x, y)] = near1 > 0 ? kNextToObstacle : (near2 > 0 ? kNearObstacle : 0);
        }
    }
    for (const auto &e : entities.GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding || e.attackDamage <= 0 || e.owner >= kMaxPlayers
            || !Players::Hostile(e.owner, owner))
            continue;
        if (!seesAll && !entities.IsEntityVisibleToAi(e, owner))
            continue;
        const int r = static_cast<int>(std::ceil(e.attackRange)) + 1;
        const float cx = e.GetUnitCenterPixelX() / TILE_WIDTH, cy = e.GetUnitCenterPixelY() / TILE_HEIGHT;
        for (int y = std::max(0, static_cast<int>(cy) - r); y <= std::min(h - 1, static_cast<int>(cy) + r); ++y)
        {
            for (int x = std::max(0, static_cast<int>(cx) - r); x <= std::min(w - 1, static_cast<int>(cx) + r); ++x)
            {
                if (DetMath::Hypot(x + 0.5f - cx, y + 0.5f - cy) <= static_cast<float>(r))
                    extra[at(x, y)] += kUnderFire;
            }
        }
    }

    // The objective is often a building (blocked): reaching its surroundings is enough
    const bool goalBlocked = kind[at(goalX, goalY)] == 1;
    auto isGoal = [&](int x, int y) {
        const int d = std::max(std::abs(x - goalX), std::abs(y - goalY));
        return d == 0 || (goalBlocked && d <= 3);
    };
    auto heuristic = [&](int x, int y) {
        const int dx = std::abs(x - goalX), dy = std::abs(y - goalY);
        return kStraight * std::max(dx, dy) + (kDiagonal - kStraight) * std::min(dx, dy);
    };

    constexpr int kInf = 1 << 30;
    std::vector<int> cost(n, kInf);
    std::vector<int> from(n, -1);
    // (f, index): equal f values pop by tile index, the same on every computer
    using Item = std::pair<int, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    cost[at(startX, startY)] = 0;
    open.push({heuristic(startX, startY), static_cast<int>(at(startX, startY))});
    int reached = -1;
    while (!open.empty())
    {
        const auto [f, idx] = open.top();
        open.pop();
        const int x = idx % w, y = idx / w;
        if (f - heuristic(x, y) > cost[static_cast<std::size_t>(idx)])
            continue; // stale
        if (isGoal(x, y))
        {
            reached = idx;
            break;
        }
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0)
                    continue;
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                    continue;
                const std::uint8_t k = kind[at(nx, ny)];
                if (k == 1)
                    continue;
                // No cutting corners past cliffs or buildings
                if (dx != 0 && dy != 0 && (kind[at(x + dx, y)] == 1 || kind[at(x, y + dy)] == 1))
                    continue;
                const int step
                    = (dx != 0 && dy != 0 ? kDiagonal : kStraight) + extra[at(nx, ny)] + (k == 2 ? kHostileWall : 0);
                const int g = cost[static_cast<std::size_t>(idx)] + step;
                const std::size_t ni = at(nx, ny);
                if (g < cost[ni])
                {
                    cost[ni] = g;
                    from[ni] = idx;
                    open.push({g + heuristic(nx, ny), static_cast<int>(ni)});
                }
            }
        }
    }
    if (reached < 0)
        return false;
    for (int i = reached; i >= 0 && i != static_cast<int>(at(startX, startY)); i = from[static_cast<std::size_t>(i)])
        outPath.push_back({i % w, i / w});
    std::reverse(outPath.begin(), outPath.end());
    return true;
}

int AISquadController::FormUpIndex(const AISquad &sq, const EntityManager &entities) const
{
    const int last = static_cast<int>(sq.route.size()) - 1;
    if (last <= 0)
        return -1;
    constexpr float kObjectiveRadius = 14.0f; // defenses this close to the objective cover it
    constexpr float kMargin = 2.0f;           // the line stands this far outside their reach
    struct Defense
    {
        float x, y, reach;
    };
    std::vector<Defense> defenses;
    for (const auto &e : entities.GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding || e.attackDamage <= 0 || e.owner >= kMaxPlayers
            || !Players::Hostile(e.owner, sq.owner))
            continue;
        if (!m_seesAll && !entities.IsEntityVisibleToAi(e, sq.owner))
            continue;
        const float x = e.GetUnitCenterPixelX() / TILE_WIDTH, y = e.GetUnitCenterPixelY() / TILE_HEIGHT;
        if (DetMath::Hypot(x - sq.targetX, y - sq.targetY) <= kObjectiveRadius)
            defenses.push_back({x, y, e.attackRange + kMargin});
    }
    if (defenses.empty())
        return -1;
    for (int i = last; i > sq.progress; --i)
    {
        const PathNode &p = sq.route[i];
        bool safe = true;
        for (const Defense &d : defenses)
            safe = safe && DetMath::Hypot(p.x + 0.5f - d.x, p.y + 0.5f - d.y) > d.reach;
        if (safe)
            return i;
    }
    return -1;
}

void AISquadController::RebuildRoute(AISquad &sq, EntityManager &entities, Pathfinder &pathfinder)
{
    sq.route.clear();
    sq.progress = 0;
    sq.marchIndex = 0;
    const Unit *leader = entities.GetUnit(sq.leaderId);
    if (!leader)
        return;

    // A flank point that is still ahead splits the route in two legs; once reached it is forgotten
    if (sq.viaX >= 0 && DistTo(*leader, static_cast<float>(sq.viaX), static_cast<float>(sq.viaY)) <= 3.0f)
    {
        sq.viaX = sq.viaY = -1;
    }

    std::vector<PathNode> path;
    bool partial = false;
    sq.route.push_back({leader->tileX, leader->tileY});
    int fromX = leader->tileX, fromY = leader->tileY;
    const TileMap *map = pathfinder.GetMap();
    // Strategic route over terrain only: the group's own idle units must not wall the leader in
    auto plan = [&](int tx, int ty) {
        path.clear();
        if (map && PlanRoute(*map, entities, sq.owner, m_seesAll, fromX, fromY, tx, ty, path) && !path.empty())
            return true;
        pathfinder.SetIgnoreUnitsOnce();
        pathfinder.FindPath(fromX, fromY, tx, ty, path, leader->id, &partial);
        return !path.empty(); // a partial route still leads the way
    };
    if (sq.viaX >= 0)
    {
        if (plan(sq.viaX, sq.viaY))
        {
            sq.route.insert(sq.route.end(), path.begin(), path.end());
            fromX = path.back().x;
            fromY = path.back().y;
        }
    }
    if (plan(sq.targetX, sq.targetY))
        sq.route.insert(sq.route.end(), path.begin(), path.end());
}

void AISquadController::HoldUnits(const std::vector<int> &ids, EntityManager &entities)
{
    for (int id : ids)
    {
        Unit *u = entities.GetUnit(id);
        if (!u || IsEngaged(*u))
            continue;
        u->ClearPathState();
        u->ClearMoveTarget();
        u->isAttackMoving = false;
        if (u->state == ActionState::Move)
            u->state = ActionState::Idle;
    }
}

void AISquadController::OrderMarch(
    AISquad &sq,
    int routeIndex,
    TileMap &map,
    EntityManager &entities,
    Pathfinder &pathfinder,
    const AITacticsConfig &tactics)
{
    if (sq.route.empty())
        return;
    routeIndex = std::clamp(routeIndex, 0, static_cast<int>(sq.route.size()) - 1);
    const PathNode &p = sq.route[routeIndex];

    // Marching direction from a few nodes back, used to put the front line ahead of the rear
    const PathNode &prev = sq.route[std::max(0, routeIndex - 3)];
    float dx = static_cast<float>(p.x - prev.x);
    float dy = static_cast<float>(p.y - prev.y);
    float len = DetMath::Hypot(dx, dy);
    if (len > 0.001f)
    {
        dx /= len;
        dy /= len;
    }

    std::vector<int> front, middle, back;
    for (int id : sq.unitIds)
    {
        Unit *u = entities.GetUnit(id);
        if (!u)
            continue;
        u->flowMode = false;
        if (IsEngaged(*u))
            continue; // never pull a unit out of a fight
        int arch = u->archetypeId;
        if ((tactics.armorFront && IsTank(arch)) || (tactics.meleeRangedPairs && IsMelee(arch)))
        {
            front.push_back(id);
        }
        else if ((tactics.meleeRangedPairs || tactics.armorFront) && IsRangedSupport(arch))
        {
            back.push_back(id);
        }
        else
        {
            middle.push_back(id);
        }
    }

    // Rows across the way, as wide as the road allows (a tile of room kept from cliffs and walls), front kinds in
    // the first rows. Each row stands further back along the route, so the column follows its bends instead of
    // the whole group crowding one point at the edge of the road.
    auto openTile = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < map.GetWidth() && y < map.GetHeight() && map.IsPassable(x, y)
               && map.GetWall(x, y) == 0;
    };
    const float perpX = -dy, perpY = dx;
    // Open tiles beside the route node; with `clearance` a tile counts only when the next one out is open too
    auto roomAcross = [&](const PathNode &c, float sideSign, bool clearance) {
        int room = 0;
        for (int k = 1; k <= 3; ++k)
        {
            const int x = static_cast<int>(std::lround(c.x + perpX * sideSign * k));
            const int y = static_cast<int>(std::lround(c.y + perpY * sideSign * k));
            const int xOut = static_cast<int>(std::lround(c.x + perpX * sideSign * (k + 1)));
            const int yOut = static_cast<int>(std::lround(c.y + perpY * sideSign * (k + 1)));
            if (!openTile(x, y) || (clearance && !openTile(xOut, yOut)))
                break;
            room = k;
        }
        return room;
    };
    // The leader walks in the first row (the march is measured by it), then front kinds, middle, rear
    std::vector<int> ordered;
    for (const std::vector<int> *part : {&front, &middle, &back})
    {
        for (int id : *part)
        {
            if (id == sq.leaderId)
                ordered.insert(ordered.begin(), id);
            else
                ordered.push_back(id);
        }
    }
    const int maxWidth = sq.state == AISquadState::FormingUp ? 6 : 4;
    const int half = (maxWidth - 1) / 2 + 1;
    int left = std::min(roomAcross(p, -1.0f, true), half);
    int right = std::min(roomAcross(p, 1.0f, true), half);
    if (left + right == 0)
    {
        // A narrow passage: two abreast where it fits, right along its sides
        left = std::min(roomAcross(p, -1.0f, false), 1);
        right = left > 0 ? 0 : std::min(roomAcross(p, 1.0f, false), 1);
    }
    const int width = std::clamp(1 + left + right, 1, maxWidth);
    const int rowStep = width >= 2 ? 1 : 2; // a single file keeps a little more distance
    for (std::size_t k = 0; k < ordered.size(); ++k)
    {
        const int row = static_cast<int>(k) / width;
        const int col = static_cast<int>(k) % width;
        const int inRow = std::min(width, static_cast<int>(ordered.size()) - row * width);
        // Rows never point back past where the group already is
        const PathNode &c = sq.route[std::max(std::min(sq.progress, routeIndex), routeIndex - row * rowStep)];
        // Centred on the route, shifted toward the side with more room
        int start = -std::min(left, (inRow - 1) / 2);
        start = std::max(-left, std::min(start, right - (inRow - 1)));
        const float lateral = static_cast<float>(start + col);
        PathNode t {
            static_cast<int>(std::lround(c.x + perpX * lateral)), static_cast<int>(std::lround(c.y + perpY * lateral))};
        if (!openTile(t.x, t.y))
            t = c;
        OrderLeg(sq, {ordered[k]}, t.x, t.y, entities, pathfinder);
    }
    sq.orderTimer = 0.0f;
}

void AISquadController::AdvanceMarch(
    AISquad &sq,
    TileMap &map,
    EntityManager &entities,
    Pathfinder &pathfinder,
    const ChokepointMap &chokes,
    const AITacticsConfig &tactics)
{
    int last = static_cast<int>(sq.route.size()) - 1;
    // In front of the objective's defenses: line up out of their reach first, then go in together
    if (!sq.formedUp && sq.attackMove && !sq.playerSquad && last > 0)
    {
        const int stand = FormUpIndex(sq, entities);
        if (stand > sq.progress && stand <= std::min(last, sq.progress + std::max(2, tactics.marchStrideTiles) + 2))
        {
            sq.formedUp = true;
            sq.marchIndex = stand;
            SetState(sq, AISquadState::FormingUp);
            OrderMarch(sq, stand, map, entities, pathfinder, tactics);
            return;
        }
    }
    if (last <= 0 || sq.progress >= last)
    {
        OrderLeg(sq, sq.unitIds, sq.targetX, sq.targetY, entities, pathfinder);
        for (int id : sq.unitIds)
        {
            if (Unit *u = entities.GetUnit(id))
                u->flowMode = false;
        }
        SetState(sq, AISquadState::Assault);
        sq.orderTimer = 0.0f;
        if (sq.playerSquad)
        {
            // The player's group has arrived: the units finish the move on their own
            std::vector<int> all = sq.unitIds;
            for (int id : all)
                ReleaseUnit(sq, id, entities);
        }
        return;
    }

    int next = std::min(sq.progress + std::max(2, tactics.marchStrideTiles), last);

    // Look slightly past the next waypoint so the group never stops inside a corridor entrance
    int chokeAt = -1;
    for (int i = sq.progress; i <= std::min(next + 2, last); ++i)
    {
        if (chokes.IsChoke(sq.route[i].x, sq.route[i].y))
        {
            chokeAt = i;
            break;
        }
    }

    if (chokeAt >= 0)
    {
        int rally = std::max(sq.progress, chokeAt - 2);
        if (tactics.waitAtChokepoints && rally > sq.progress)
        {
            sq.marchIndex = rally;
            sq.deployIndex = chokeAt; // remembered entry; the real deploy point is set by beginTransit
            OrderMarch(sq, rally, map, entities, pathfinder, tactics);
            SetState(sq, AISquadState::ChokeApproach);
            return;
        }
        BeginTransit(sq, chokeAt, entities, pathfinder, chokes);
        return;
    }

    sq.marchIndex = next;
    OrderMarch(sq, next, map, entities, pathfinder, tactics);
    SetState(sq, AISquadState::Marching);
}

void AISquadController::BeginTransit(
    AISquad &sq, int chokeIndex, EntityManager &entities, Pathfinder &pathfinder, const ChokepointMap &chokes)
{
    int last = static_cast<int>(sq.route.size()) - 1;
    int exit = chokeIndex;
    while (exit <= last && chokes.IsChoke(sq.route[exit].x, sq.route[exit].y))
        ++exit;
    int deploy = std::min(exit + 4, last);

    sq.deployIndex = deploy;
    sq.marchIndex = deploy;

    // Single file: one shared destination, no sidesteps; reservations make the column queue up
    for (int id : sq.unitIds)
    {
        if (Unit *u = entities.GetUnit(id))
            u->flowMode = true;
    }
    const PathNode &d = sq.route[deploy];
    OrderLeg(sq, sq.unitIds, d.x, d.y, entities, pathfinder);
    SetState(sq, AISquadState::ChokeTransit);
    sq.orderTimer = 0.0f;
}

// Everyone walks at its own speed. A member that got more than a leash ahead of the group's centre (along the way
// to the waypoint) stands on its tile until the ones behind have caught up (back within 0.6 leash: hysteresis, no
// stop-and-go). Nobody waits in combat, near the objective, or inside a narrow passage (it would block the way for
// the rest: it waits beyond it); a member behind that has not got any closer for kHoldStuckSeconds is not waited for.
void AISquadController::HoldVanguard(
    AISquad &sq,
    EntityManager &entities,
    const ChokepointMap &chokes,
    float cx,
    float cy,
    float leash,
    bool active,
    bool nearObjective,
    float dt)
{
    constexpr float kHoldStuckSeconds = 4.0f;
    constexpr float kResumeFraction = 0.6f;

    float dirX = 0.0f, dirY = 0.0f;
    if (active && !nearObjective && !sq.route.empty())
    {
        const PathNode &wp = sq.route[std::clamp(sq.marchIndex, 0, static_cast<int>(sq.route.size()) - 1)];
        dirX = static_cast<float>(wp.x) - cx;
        dirY = static_cast<float>(wp.y) - cy;
        const float len = DetMath::Hypot(dirX, dirY);
        if (len > 0.5f)
        {
            dirX /= len;
            dirY /= len;
        }
        else
        {
            dirX = dirY = 0.0f;
        }
    }
    if (dirX == 0.0f && dirY == 0.0f)
    {
        for (int id : sq.unitIds)
            entities.GetUnit(id)->groupHold = false;
        sq.holdBest.clear();
        sq.holdTimer.clear();
        return;
    }

    // Members behind that are stuck (no headway for a while) are left out of the centre the vanguard waits for
    auto stuck = [&](int id) {
        auto it = sq.holdTimer.find(id);
        return it != sq.holdTimer.end() && it->second > kHoldStuckSeconds;
    };
    for (int id : sq.unitIds)
    {
        const Unit *u = entities.GetUnit(id);
        const float d = DistTo(*u, cx, cy);
        const float along = (UnitTileX(*u) - cx) * dirX + (UnitTileY(*u) - cy) * dirY;
        if (d <= leash || along >= 0.0f)
        {
            sq.holdBest.erase(id);
            sq.holdTimer.erase(id);
            continue;
        }
        auto it = sq.holdBest.find(id);
        if (it == sq.holdBest.end() || d < it->second - 0.5f)
        {
            sq.holdBest[id] = d;
            sq.holdTimer[id] = 0.0f;
        }
        else
        {
            sq.holdTimer[id] += dt;
        }
    }
    float rx = 0.0f, ry = 0.0f;
    int counted = 0;
    for (int id : sq.unitIds)
    {
        if (stuck(id))
            continue;
        const Unit *u = entities.GetUnit(id);
        rx += UnitTileX(*u);
        ry += UnitTileY(*u);
        ++counted;
    }
    if (counted > 0)
    {
        rx /= static_cast<float>(counted);
        ry /= static_cast<float>(counted);
    }
    else
    {
        rx = cx;
        ry = cy;
    }

    for (int id : sq.unitIds)
    {
        Unit *u = entities.GetUnit(id);
        if (u->moveSpeed <= 0.0f || u->state != ActionState::Move || IsEngaged(*u) || u->moveFireTargetId != 0)
        {
            u->groupHold = false;
            continue;
        }
        const int width = chokes.OpenWidth(u->tileX, u->tileY);
        if (width > 0 && width <= 4)
        {
            u->groupHold = false; // inside a passage: goes through, waits beyond it
            continue;
        }
        const float along = (UnitTileX(*u) - rx) * dirX + (UnitTileY(*u) - ry) * dirY;
        if (along > leash)
            u->groupHold = true;
        else if (along <= kResumeFraction * leash)
            u->groupHold = false;
    }
}

// ---------------------------------------------------------------------------
// Per-squad state machine (runs every kTickSeconds)
// ---------------------------------------------------------------------------

void AISquadController::Tick(
    AISquad &sq,
    float dt,
    TileMap &map,
    EntityManager &entities,
    Pathfinder &pathfinder,
    const ChokepointMap &chokes,
    const AITacticsConfig &tactics)
{
    // Drop the dead
    sq.unitIds.erase(
        std::remove_if(
            sq.unitIds.begin(),
            sq.unitIds.end(),
            [&](int id) {
                const Unit *u = entities.GetUnit(id);
                return !u || !u->alive || u->state == ActionState::Die;
            }),
        sq.unitIds.end());
    if (sq.unitIds.empty())
        return;

    if (!entities.GetUnit(sq.leaderId)
        || std::find(sq.unitIds.begin(), sq.unitIds.end(), sq.leaderId) == sq.unitIds.end())
    {
        sq.leaderId = PickLeader(sq, entities, tactics);
    }
    const Unit *leader = entities.GetUnit(sq.leaderId);
    if (!leader)
        return;

    const int localWidth = chokes.OpenWidth(leader->tileX, leader->tileY);
    const float leash = EffectiveLeash(tactics, sq.unitIds.size(), localWidth > 0 && localWidth <= 4);
    sq.leashRadius = leash;
    // Also catches re-paths the units made on their own (blocked by the crowd)
    CapDetours(sq, entities, &map);

    // Cohesion metrics: centroid of the group core and the farthest unit from it (Dmax).
    // The core is everyone within two leash radii of the leader, so one far straggler
    // does not drag the centre away from the rest of the group.
    float cx = 0.0f, cy = 0.0f;
    int coreCount = 0;
    for (int id : sq.unitIds)
    {
        const Unit *u = entities.GetUnit(id);
        if (DistTo(*u, UnitTileX(*leader), UnitTileY(*leader)) > 2.0f * leash)
            continue;
        cx += UnitTileX(*u);
        cy += UnitTileY(*u);
        ++coreCount;
    }
    cx /= static_cast<float>(std::max(1, coreCount));
    cy /= static_cast<float>(std::max(1, coreCount));
    float dmax = 0.0f;
    bool engaged = false;
    for (int id : sq.unitIds)
    {
        const Unit *u = entities.GetUnit(id);
        dmax = std::max(dmax, DistTo(*u, cx, cy));
        engaged = engaged || IsEngaged(*u);
    }
    sq.centroidX = cx;
    sq.centroidY = cy;
    sq.maxSpread = dmax;
    // Members clearly outside the leash (stop) and outside it at all (resume): a few stragglers do not
    // hold the whole group up, they catch up while the front slows down
    int outsideStop = 0, outsideResume = 0;
    for (int id : sq.unitIds)
    {
        const float d = DistTo(*entities.GetUnit(id), cx, cy);
        if (d > leash * 1.2f)
            ++outsideStop;
        if (d > leash)
            ++outsideResume;
    }
    const int tolerated = std::max(1, static_cast<int>(sq.unitIds.size()) / 4);
    // Close to the objective the group goes straight into the attack instead of waiting
    const int remainingRoute = static_cast<int>(sq.route.size()) - 1 - sq.progress;
    const bool nearObjective = remainingRoute <= tactics.marchStrideTiles * 3 / 2
                               || DetMath::Hypot(cx - sq.targetX, cy - sq.targetY) <= leash;
    HoldVanguard(
        sq,
        entities,
        chokes,
        cx,
        cy,
        leash,
        sq.state == AISquadState::Marching || sq.state == AISquadState::ChokeApproach,
        nearObjective,
        dt);

    sq.stateTimer += dt;
    sq.orderTimer += dt;

    // First leg after launch
    if (sq.state == AISquadState::Marching && sq.orderTimer > 1e5f)
    {
        AdvanceMarch(sq, map, entities, pathfinder, chokes, tactics);
        return;
    }

    // A plain move (player group) ignores enemies on the way: nobody stops to pick a fight
    if (!sq.attackMove)
    {
        for (int id : sq.unitIds)
        {
            Unit *u = entities.GetUnit(id);
            if (u->targetEntityId != 0 || u->targetWallTileX >= 0)
            {
                u->targetEntityId = 0;
                u->targetWallTileX = u->targetWallTileY = -1;
                if (u->state == ActionState::Attack)
                    u->state = ActionState::Idle;
                if (sq.state == AISquadState::Marching && id != sq.leaderId)
                {
                    OrderLeg(sq, {id}, leader->tileX, leader->tileY, entities, pathfinder);
                }
            }
        }
        engaged = false;
    }

    if (!sq.playerSquad
        && (sq.state == AISquadState::Engaged || sq.state == AISquadState::Marching
            || sq.state == AISquadState::Regrouping || sq.state == AISquadState::FormingUp
            || sq.state == AISquadState::Assault))
        UpdateFocusTarget(sq, map, entities, pathfinder);
    else
        sq.focusTargetId = 0;

    // Combat pauses the cohesion logic (except inside a corridor, where the column must keep flowing)
    if (engaged
        && (sq.state == AISquadState::Marching || sq.state == AISquadState::Regrouping
            || sq.state == AISquadState::ChokeApproach || sq.state == AISquadState::Deploying
            || sq.state == AISquadState::FormingUp))
    {
        sq.resumeState = sq.state;
        SetState(sq, AISquadState::Engaged);
        return;
    }

    // Returns true when at least one straggler got closer this tick
    auto trackStragglers = [&](float radius, float cxRef, float cyRef) -> bool {
        std::vector<int> drop;
        bool progress = false;
        for (int id : sq.unitIds)
        {
            Unit *u = entities.GetUnit(id);
            float d = DistTo(*u, cxRef, cyRef);
            if (d <= radius)
            {
                sq.stragglerBest.erase(id);
                sq.stragglerTimer.erase(id);
                continue;
            }
            auto it = sq.stragglerBest.find(id);
            if (it == sq.stragglerBest.end() || d < it->second - 0.5f)
            {
                sq.stragglerBest[id] = d;
                sq.stragglerTimer[id] = 0.0f;
                progress = true;
            }
            else
            {
                sq.stragglerTimer[id] += dt;
                // Only a unit that is really far and not closing in leaves the group; one standing at the
                // edge of the crowd simply cannot get any closer
                if (sq.stragglerTimer[id] > tactics.stragglerTimeoutSeconds && d > radius * 1.5f)
                    drop.push_back(id);
            }
            // Highest priority for the straggler: walk straight to the leader
            if (IsIdleWithoutOrders(*u) && id != sq.leaderId)
            {
                OrderLeg(sq, {id}, leader->tileX, leader->tileY, entities, pathfinder);
            }
        }
        for (int id : drop)
        {
            ReleaseUnit(sq, id, entities);
            // The player's unit is not left standing: it goes on to the destination on its own
            if (sq.playerSquad)
                OrderLeg(sq, {id}, sq.targetX, sq.targetY, entities, pathfinder);
        }
        return progress;
    };

    switch (sq.state)
    {
        case AISquadState::Marching:
        {
            // The whole group does not stop for stragglers any more: only the ones too far ahead wait for them
            // (HoldVanguard), everyone else keeps walking at its own speed

            if (outsideStop > 0)
                trackStragglers(leash * 1.2f, cx, cy);

            const PathNode &wp = sq.route.empty() ? PathNode {sq.targetX, sq.targetY} : sq.route[sq.marchIndex];
            bool reached;
            // A unit that stopped at its formation slot around the waypoint has arrived as well
            auto arrived = [&](const Unit &u, float radius) {
                float d = DistTo(u, static_cast<float>(wp.x), static_cast<float>(wp.y));
                return d <= radius || (IsIdleWithoutOrders(u) && d <= std::max(radius, leash) * 1.5f);
            };
            if (tactics.syncToSlowest)
            {
                reached = arrived(*leader, 2.5f);
                for (int id : sq.unitIds)
                {
                    if (!reached)
                        break;
                    reached = arrived(*entities.GetUnit(id), leash);
                }
            }
            else
            {
                reached = arrived(*leader, 2.5f);
            }
            // Nobody makes headway any more (crowd settled around the waypoint): good enough
            if (DetMath::Hypot(cx - sq.stallX, cy - sq.stallY) > 0.5f)
            {
                sq.stallX = cx;
                sq.stallY = cy;
                sq.stallTimer = 0.0f;
            }
            else
            {
                sq.stallTimer += dt;
            }
            if (!reached && sq.stallTimer > 2.0f
                && DistTo(*leader, static_cast<float>(wp.x), static_cast<float>(wp.y)) <= std::max(2.5f, leash))
            {
                reached = true;
            }
            if (reached)
                sq.stallTimer = 0.0f;

            if (reached)
            {
                sq.progress = sq.marchIndex;
                AdvanceMarch(sq, map, entities, pathfinder, chokes, tactics);
            }
            else if (sq.stateTimer > 25.0f)
            {
                // Waypoint unreachable (blocked by new structures): plan again from the leader
                RebuildRoute(sq, entities, pathfinder);
                AdvanceMarch(sq, map, entities, pathfinder, chokes, tactics);
            }
            else if (sq.orderTimer > 4.0f && IsIdleWithoutOrders(*leader))
            {
                // Orders were overridden elsewhere (base defense, call for help): re-issue the leg
                OrderMarch(sq, sq.marchIndex, map, entities, pathfinder, tactics);
            }
            break;
        }

        case AISquadState::Regrouping:
        {
            // Back together, or compacted as far as the crowd allows (no one closed in for 1.5 s)
            const bool compacted = sq.quietTimer >= 1.5f && dmax <= leash * 1.5f;
            if (outsideResume <= tolerated || compacted)
            {
                sq.stragglerBest.clear();
                sq.stragglerTimer.clear();
                OrderMarch(sq, sq.marchIndex, map, entities, pathfinder, tactics);
                SetState(sq, AISquadState::Marching);
                break;
            }
            sq.quietTimer = trackStragglers(leash, cx, cy) ? 0.0f : sq.quietTimer + dt;
            break;
        }

        case AISquadState::ChokeApproach:
        {
            const PathNode &rally = sq.route[sq.marchIndex];
            bool leaderAtRally = DistTo(*leader, static_cast<float>(rally.x), static_cast<float>(rally.y)) <= 2.5f;
            sq.quietTimer = leaderAtRally ? sq.quietTimer + dt : 0.0f; // hold time at the rally point
            bool gathered = dmax <= leash;
            if ((leaderAtRally && sq.quietTimer >= tactics.preChokeHoldSeconds && gathered) || sq.stateTimer > 12.0f)
            {
                sq.progress = sq.marchIndex;
                BeginTransit(sq, sq.deployIndex, entities, pathfinder, chokes);
                break;
            }
            if (leaderAtRally && !gathered)
                trackStragglers(leash, cx, cy);
            break;
        }

        case AISquadState::ChokeTransit:
        {
            const PathNode &d = sq.route[sq.deployIndex];
            bool allOut = true;
            for (int id : sq.unitIds)
            {
                const Unit *u = entities.GetUnit(id);
                if (chokes.IsChoke(u->tileX, u->tileY)
                    || DistTo(*u, static_cast<float>(d.x), static_cast<float>(d.y)) > leash + 1.5f)
                {
                    allOut = false;
                }
            }
            float timeout = std::max(15.0f, 2.0f * static_cast<float>(sq.unitIds.size()));
            if (allOut || sq.stateTimer > timeout)
            {
                if (!allOut)
                {
                    // Whoever is still stuck in the passage is left behind; the group moves on
                    std::vector<int> stuck;
                    for (int id : sq.unitIds)
                    {
                        const Unit *u = entities.GetUnit(id);
                        if (DistTo(*u, static_cast<float>(d.x), static_cast<float>(d.y)) > leash + 1.5f)
                            stuck.push_back(id);
                    }
                    for (int id : stuck)
                        ReleaseUnit(sq, id, entities);
                }
                for (int id : sq.unitIds)
                {
                    if (Unit *u = entities.GetUnit(id))
                        u->flowMode = false;
                }
                SetState(sq, AISquadState::Deploying);
                break;
            }
            if (sq.orderTimer > 3.0f)
            {
                std::vector<int> lost;
                for (int id : sq.unitIds)
                {
                    const Unit *u = entities.GetUnit(id);
                    if (IsIdleWithoutOrders(*u) && DistTo(*u, static_cast<float>(d.x), static_cast<float>(d.y)) > 2.0f)
                    {
                        lost.push_back(id);
                    }
                }
                if (!lost.empty())
                    OrderLeg(sq, lost, d.x, d.y, entities, pathfinder);
                sq.orderTimer = 0.0f;
            }
            break;
        }

        case AISquadState::Deploying:
        {
            if (!tactics.waitAtChokepoints || dmax <= leash || sq.stateTimer > 6.0f)
            {
                sq.progress = sq.deployIndex;
                AdvanceMarch(sq, map, entities, pathfinder, chokes, tactics);
            }
            break;
        }

        case AISquadState::FormingUp:
        {
            // Lined up (or as close as the ground allows): everybody goes in at once
            const PathNode &stand = sq.route[std::clamp(sq.marchIndex, 0, static_cast<int>(sq.route.size()) - 1)];
            const bool leaderThere
                = DistTo(*leader, static_cast<float>(stand.x), static_cast<float>(stand.y)) <= std::max(2.5f, leash);
            sq.quietTimer = (leaderThere && dmax <= leash * 1.5f) ? sq.quietTimer + dt : 0.0f;
            if (sq.quietTimer >= 1.0f || sq.stateTimer > 12.0f)
            {
                sq.progress = sq.marchIndex;
                AdvanceMarch(sq, map, entities, pathfinder, chokes, tactics);
                break;
            }
            if (leaderThere)
                trackStragglers(leash * 1.5f, cx, cy);
            else if (sq.orderTimer > 4.0f && IsIdleWithoutOrders(*leader))
                OrderMarch(sq, sq.marchIndex, map, entities, pathfinder, tactics);
            break;
        }

        case AISquadState::Engaged:
        {
            if (engaged)
            {
                sq.quietTimer = 0.0f;
                // The whole wave answers: idle members join the ones already fighting
                if (sq.orderTimer > 1.0f)
                {
                    std::vector<int> idle;
                    int fx = leader->tileX, fy = leader->tileY;
                    for (int id : sq.unitIds)
                    {
                        const Unit *u = entities.GetUnit(id);
                        if (IsEngaged(*u))
                        {
                            fx = u->tileX;
                            fy = u->tileY;
                        }
                    }
                    for (int id : sq.unitIds)
                    {
                        const Unit *u = entities.GetUnit(id);
                        if (IsIdleWithoutOrders(*u))
                            idle.push_back(id);
                    }
                    if (!idle.empty())
                        OrderLeg(sq, idle, fx, fy, entities, pathfinder);
                    sq.orderTimer = 0.0f;
                }
            }
            else
            {
                sq.quietTimer += dt;
                if (sq.quietTimer >= 2.0f)
                {
                    sq.leaderId = PickLeader(sq, entities, tactics);
                    RebuildRoute(sq, entities, pathfinder);
                    AdvanceMarch(sq, map, entities, pathfinder, chokes, tactics);
                }
            }
            break;
        }

        case AISquadState::Assault:
        {
            if (sq.orderTimer < 3.0f)
                break;
            sq.orderTimer = 0.0f;
            std::vector<int> idle;
            for (int id : sq.unitIds)
            {
                const Unit *u = entities.GetUnit(id);
                if (IsIdleWithoutOrders(*u))
                    idle.push_back(id);
            }
            if (idle.empty())
                break;

            // Objective gone: move on to the nearest remaining player structure
            int bestX = -1, bestY = -1;
            float bestD = 1e9f;
            for (const auto &e : entities.GetAllEntities())
            {
                if (!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner >= kMaxPlayers
                    || !Players::Hostile(e.owner, sq.owner))
                {
                    continue;
                }
                float d = DetMath::Hypot(e.tileX - cx, e.tileY - cy);
                if (d < bestD)
                {
                    bestD = d;
                    bestX = e.tileX;
                    bestY = e.tileY;
                }
            }
            if (bestX >= 0)
            {
                OrderLeg(sq, idle, bestX, bestY, entities, pathfinder);
            }
            else if (idle.size() == sq.unitIds.size())
            {
                std::vector<int> all = sq.unitIds;
                for (int id : all)
                    ReleaseUnit(sq, id, entities);
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
    for (const auto &sq : m_squads)
    {
        const Entity *leader = entities.GetEntity(sq.leaderId);
        if (!leader)
            continue;
        float lx = leader->GetUnitCenterPixelX();
        float ly = leader->GetUnitCenterPixelY();

        // Leash lines: green = in formation, yellow = at the limit, red = straggling
        for (int id : sq.unitIds)
        {
            const Entity *u = entities.GetEntity(id);
            if (!u || id == sq.leaderId)
                continue;
            float d = DetMath::Hypot(u->GetUnitCenterPixelX() - lx, u->GetUnitCenterPixelY() - ly) / TILE_WIDTH;
            SDL_Color c = (d <= 2.0f)                                   ? SDL_Color {60, 230, 90, 220}
                          : (d <= std::max(leashTiles, sq.leashRadius)) ? SDL_Color {240, 210, 60, 220}
                                                                        : SDL_Color {240, 60, 50, 230};
            renderer.DrawLineWorld(u->GetUnitCenterPixelX(), u->GetUnitCenterPixelY(), lx, ly, camera, c);
        }

        // Centroid and the allowed cohesion radius
        float cxp = sq.centroidX * TILE_WIDTH + TILE_WIDTH * 0.5f;
        float cyp = sq.centroidY * TILE_HEIGHT + TILE_HEIGHT * 0.5f;
        float radius = sq.leashRadius;
        RenderUtility::DrawTileRadius(renderer, camera, cxp, cyp, radius, SDL_Color {255, 255, 255, 120}, 1.5f);
        renderer.FillRectWorld(cxp - 3, cyp - 3, 6, 6, camera, {255, 255, 255, 230});

        // Current waypoint
        if (!sq.route.empty())
        {
            const PathNode &wp = sq.route[std::clamp(sq.marchIndex, 0, static_cast<int>(sq.route.size()) - 1)];
            renderer.DrawRectWorld(
                wp.x * TILE_WIDTH + 4.0f,
                wp.y * TILE_HEIGHT + 4.0f,
                TILE_WIDTH - 8.0f,
                TILE_HEIGHT - 8.0f,
                camera,
                {120, 200, 255, 220});
        }

        // "G<n>" label above the leader with the current state
        float sx, sy;
        camera.WorldToScreen(lx, ly - TILE_HEIGHT * 1.2f, sx, sy);
        std::string label = "G" + std::to_string(sq.id);
        int w = font.GetTextWidth(label, 6) + 10;
        renderer.FillRectScreen(static_cast<int>(sx) - w / 2, static_cast<int>(sy) - 10, w, 20, {12, 14, 20, 210});
        renderer.DrawRectScreen(static_cast<int>(sx) - w / 2, static_cast<int>(sy) - 10, w, 20, {232, 176, 74, 255});
        font.DrawString(
            label,
            static_cast<int>(sx),
            static_cast<int>(sy),
            6,
            {232, 176, 74, 255},
            false,
            Anchor::HCENTER | Anchor::VCENTER);
        std::string info = std::string(SquadStateName(sq.state)) + "  " + std::to_string(sq.unitIds.size()) + "/"
                           + std::to_string(sq.initialSize);
        font.DrawString(
            info,
            static_cast<int>(sx),
            static_cast<int>(sy) + 18,
            1,
            {226, 231, 238, 255},
            true,
            Anchor::HCENTER | Anchor::VCENTER);
    }
}

} // namespace OpenArtOfWar
