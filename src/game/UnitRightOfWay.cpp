#include "game/Unit.hpp"
#include "EntityManagerInternal.hpp"
#include "game/Projectiles.hpp"
#include "core/DetMath.hpp"
#include "core/Utility.hpp"
#include "game/Effects.hpp"
#include "game/Sides.hpp"
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include "game/Building.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>
namespace OpenArtOfWar
{

bool Unit::HasOpenRouteTo(int goalX, int goalY, Pathfinder &pathfinder, int *outRouteLen) const
{
    if (outRouteLen)
        *outRouteLen = 0;
    if (goalX < 0 || goalY < 0)
        return false;
    if (goalX == tileX && goalY == tileY)
        return true;
    std::vector<PathNode> route;
    bool partial = false;
    pathfinder.SetIgnoreUnitsOnce();
    bool found = pathfinder.FindPath(tileX, tileY, goalX, goalY, route, id, &partial);
    if (outRouteLen)
        *outRouteLen = static_cast<int>(route.size());
    return found && !partial && !route.empty();
}

// ---------------------------------------------------------------------------
// Making way: allies let each other through narrow passages
// ---------------------------------------------------------------------------

int Unit::MovePriority() const
{
    int p = 0;
    if (groupOrder || isAttackMoving)
        p += 2;
    if (HasMoveTarget() || targetEntityId != 0)
        p += 1;
    return p;
}

bool Unit::PassesBefore(const Unit &other) const
{
    const int a = MovePriority(), b = other.MovePriority();
    return a != b ? a > b : id < other.id;
}

bool Unit::CanMakeWayFor(const Unit &requester) const
{
    if (!alive || state == ActionState::Die || state == ActionState::Transform || state == ActionState::Attack
        || state == ActionState::Move)
        return false;
    if (id == requester.id || moveSpeed <= 0.0f || isSiegeMode || isDirectEngaged || HasReservation()
        || targetWallTileX >= 0)
        return false;
    if (Players::Hostile(owner, requester.owner))
        return false;
    const bool ownGoal = HasMoveTarget() || targetEntityId != 0;
    return !ownGoal || requester.PassesBefore(*this);
}

void Unit::MakeWayStep(int x, int y)
{
    const bool ownGoal = HasMoveTarget() || targetEntityId != 0;
    if (!ownGoal && yieldHomeX < 0)
    {
        yieldHomeX = tileX;
        yieldHomeY = tileY;
        yieldHomeTries = 0;
    }
    yieldHomeTimer = 3.0f;
    path = {{x, y}};
    pathIndex = 0;
    state = ActionState::Move;
    waitBlockedTimer = 0.0f;
    allyWaitTimer = 2.5f;
    // A unit with somewhere to go plans its own route again once it stands aside
    waitingForPath = ownGoal;
    pathRetryTimer = 0.0f;
}

bool Unit::RequestMakeWay(Unit &blocker, const TileMap &map, const UnitDelegates &delegates)
{
    if (!delegates.getUnitAtTile || !delegates.getEntity)
        return false;
    // Tuning: how far the chain may reach, how many tiles the search looks at, how long a chain may be
    constexpr int kRadius = 6;
    constexpr int kMaxNodes = 96;
    constexpr int kMaxChain = 6;

    const int mapW = map.GetWidth(), mapH = map.GetHeight();
    auto open = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < mapW && y < mapH && map.IsPassable(x, y) && map.GetWall(x, y) == 0;
    };

    // Our road ahead: the chain moves off it, not along it (where it would be in the way again)
    auto roadCost = [&](int x, int y) {
        for (std::size_t i = pathIndex; i < path.size() && i < pathIndex + 6; ++i)
        {
            if (path[i].x == x && path[i].y == y)
                return i == pathIndex + 1 ? 20 : 8;
        }
        return 0;
    };

    struct Node
    {
        int x, y, parent, cost, depth;
        bool done;
    };
    std::vector<Node> nodes;
    nodes.reserve(kMaxNodes);
    nodes.push_back({blocker.tileX, blocker.tileY, -1, 0, 0, false});
    auto known = [&](int x, int y) {
        for (const Node &n : nodes)
        {
            if (n.x == x && n.y == y)
                return true;
        }
        return false;
    };

    // Dijkstra over a handful of tiles (a linear scan for the cheapest open node keeps it deterministic)
    int found = -1;
    for (;;)
    {
        int cur = -1;
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
        {
            if (!nodes[i].done && (cur < 0 || nodes[i].cost < nodes[cur].cost))
                cur = i;
        }
        if (cur < 0)
            break;
        nodes[cur].done = true;
        if (cur > 0)
        {
            const int occupant = delegates.getUnitAtTile(nodes[cur].x, nodes[cur].y, 0);
            if (occupant == 0)
            {
                found = cur;
                break;
            }
        }
        if (nodes[cur].depth >= kMaxChain)
            continue;
        const int cx = nodes[cur].x, cy = nodes[cur].y;
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0)
                    continue;
                const int nx = cx + dx, ny = cy + dy;
                if (!open(nx, ny) || (nx == tileX && ny == tileY) || known(nx, ny))
                    continue;
                if (std::max(std::abs(nx - blocker.tileX), std::abs(ny - blocker.tileY)) > kRadius)
                    continue;
                const int occupant = delegates.getUnitAtTile(nx, ny, 0);
                if (occupant != 0)
                {
                    // Through an ally only when it can step on as well
                    const Entity *e = delegates.getEntity(occupant);
                    const Unit *u = e ? e->AsUnit() : nullptr;
                    if (!u || u->tileX != nx || u->tileY != ny || !u->CanMakeWayFor(*this))
                        continue;
                }
                if (static_cast<int>(nodes.size()) >= kMaxNodes)
                    continue;
                const int cost = nodes[cur].cost + ((dx != 0 && dy != 0) ? 3 : 2) + roadCost(nx, ny);
                nodes.push_back({nx, ny, cur, cost, nodes[cur].depth + 1, false});
            }
        }
    }
    if (found < 0)
        return false;

    // Shift the chain: every unit steps into the tile of the next one, the last one into the free tile
    std::vector<int> chain; // node indices from the free tile back to the blocker
    for (int i = found; i >= 0; i = nodes[i].parent)
        chain.push_back(i);
    for (std::size_t k = 1; k < chain.size(); ++k)
    {
        const Node &from = nodes[chain[k]];
        const Node &to = nodes[chain[k - 1]];
        Unit *u = &blocker;
        if (chain[k] != 0)
        {
            Entity *e = delegates.getEntity(delegates.getUnitAtTile(from.x, from.y, 0));
            u = e ? e->AsUnit() : nullptr;
        }
        if (u)
            u->MakeWayStep(to.x, to.y);
    }
    return true;
}

bool Unit::FindPathThroughAllies(
    int goalX,
    int goalY,
    Pathfinder &pathfinder,
    std::vector<PathNode> &outPath,
    bool *outPartial,
    int avoidX,
    int avoidY) const
{
    bool partial = false;
    if (avoidX >= 0 && avoidY >= 0)
        pathfinder.SetAvoidTiles({{avoidX, avoidY}});
    const bool found = pathfinder.FindPath(tileX, tileY, goalX, goalY, outPath, id, &partial);
    if (outPartial)
        *outPartial = partial;
    // Same rule as for walls: a detour longer than twice the straight way plus 10 tiles is not a way around
    const int straight = std::max(std::abs(goalX - tileX), std::abs(goalY - tileY));
    if (found && !partial && static_cast<int>(outPath.size()) <= straight * 2 + 10)
        return true;

    // Allies standing in a passage are no wall: plan through them, they make way when we get there.
    // Allies that cannot step aside (deployed, firing, immobile, busy with a goal that goes first) stay obstacles.
    std::vector<PathNode> avoid;
    if (avoidX >= 0 && avoidY >= 0)
        avoid.push_back({avoidX, avoidY});
    if (const EntityManager *entities = pathfinder.GetEntityManager())
    {
        for (const auto &e : entities->GetAllEntities())
        {
            const Unit *u = e.AsUnit();
            if (!u || u->id == id || !u->alive || u->state == ActionState::Die || u->state == ActionState::Move
                || Players::Hostile(u->owner, owner))
                continue;
            if (!u->CanMakeWayFor(*this))
                avoid.push_back({u->tileX, u->tileY});
        }
    }
    std::vector<PathNode> through;
    bool throughPartial = false;
    pathfinder.SetAvoidTiles(std::move(avoid));
    pathfinder.SetIgnoreUnitsOnce();
    if (pathfinder.FindPath(tileX, tileY, goalX, goalY, through, id, &throughPartial) && !through.empty()
        && !throughPartial && (!found || partial || through.size() < outPath.size()))
    {
        outPath = std::move(through);
        if (outPartial)
            *outPartial = false;
        return true;
    }
    return found;
}

} // namespace OpenArtOfWar
