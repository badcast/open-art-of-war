// Flow field of group orders (Pathfinder::BuildFlowField / FlowPath, EntityManager::IssueMoveOrder): every unit
// of a group gets a whole, walkable way to its slot through the one gap of a rock line; a start the field does
// not reach gets a search of its own; the same field twice gives the same ways.
#include "game/EntityManager.hpp"
#include "game/Pathfinder.hpp"
#include "game/Players.hpp"
#include "game/TileMap.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace OpenArtOfWar;

namespace
{
int failures = 0;
void Check(bool ok, const char *name)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", name);
        ++failures;
    }
}

constexpr int kW = 80, kH = 60;
constexpr int kRockX = 40, kGapY = 45; // a rock line across the map at x = 40, open only at y = 45

void BuildMap(TileMap &map)
{
    map.InitBlankArena(kW, kH);
    map.SetFogEnabled(false);
    for (int y = 0; y < kH; ++y)
    {
        if (y != kGapY)
            map.SetPassable(kRockX, y, false);
    }
    // A pocket closed on every side (the field cannot reach into it)
    for (int y = 4; y <= 8; ++y)
        for (int x = 4; x <= 8; ++x)
            if (x == 4 || x == 8 || y == 4 || y == 8)
                map.SetPassable(x, y, false);
}

// Contiguous 8-neighbour steps over passable ground from (sx, sy), ending at (dx, dy)
bool Walkable(const TileMap &map, int sx, int sy, const std::vector<PathNode> &path, int dx, int dy)
{
    if (path.empty())
        return false;
    int px = sx, py = sy;
    for (const PathNode &n : path)
    {
        if (std::abs(n.x - px) > 1 || std::abs(n.y - py) > 1 || !map.IsPassable(n.x, n.y))
            return false;
        px = n.x;
        py = n.y;
    }
    return px == dx && py == dy;
}

bool ThroughGap(const std::vector<PathNode> &path)
{
    for (const PathNode &n : path)
        if (n.x == kRockX && n.y == kGapY)
            return true;
    return false;
}
} // namespace

int main()
{
    Players::SetupSinglePlayer(Faction::Resistance);
    TileMap map;
    BuildMap(map);
    EntityManager entities;
    entities.LoadCharacterConfigs();
    Pathfinder pf;
    pf.SetMap(&map);
    pf.SetEntityManager(&entities);

    // 1. Field ways from 12 starts left of the rock line to slots around a goal on its right
    const int goalX = 65, goalY = 15;
    Check(pf.BuildFlowField(goalX, goalY), "the field is built");
    Check(!pf.BuildFlowField(kRockX, 10), "no field toward rock");
    Check(pf.BuildFlowField(goalX, goalY), "the field is built again");
    std::vector<std::vector<PathNode>> first;
    for (int i = 0; i < 12; ++i)
    {
        const int sx = 12 + (i % 4) * 5, sy = 20 + (i / 4) * 8;
        const int dx = goalX - 1 + i % 3, dy = goalY - 1 + i / 4;
        std::vector<PathNode> path;
        bool partial = true;
        const bool ok = pf.FlowPath(sx, sy, dx, dy, path, 0, &partial);
        Check(ok && !partial, "a unit gets a full way");
        Check(Walkable(map, sx, sy, path, dx, dy), "the way is whole, walkable and ends at the slot");
        Check(ThroughGap(path), "the way goes through the gap");
        first.push_back(path);
    }
    // The same field again: the same ways
    pf.BuildFlowField(goalX, goalY);
    for (int i = 0; i < 12; ++i)
    {
        const int sx = 12 + (i % 4) * 5, sy = 20 + (i / 4) * 8;
        const int dx = goalX - 1 + i % 3, dy = goalY - 1 + i / 4;
        std::vector<PathNode> path;
        pf.FlowPath(sx, sy, dx, dy, path);
        Check(path == first[static_cast<std::size_t>(i)], "the same field gives the same way");
    }

    // 2. A start inside the closed pocket: the field does not reach it, a search of its own (partial) instead
    {
        std::vector<PathNode> path, plain;
        bool partial = false, plainPartial = false;
        pf.FlowPath(6, 6, goalX, goalY, path, 0, &partial);
        pf.FindPath(6, 6, goalX, goalY, plain, 0, &plainPartial);
        Check(path == plain && partial == plainPartial, "an unreachable start gets the plain search");
    }

    // 3. A group order: every unit walks to its slot, on the field's road
    {
        std::vector<int> ids;
        for (int i = 0; i < 10; ++i)
            ids.push_back(entities.SpawnUnitByArchetype(0, 15 + (i % 5) * 2, 30 + (i / 5) * 2, Faction::Resistance));
        entities.IssueMoveOrder(ids, goalX, goalY, pf);
        int moving = 0, gap = 0;
        for (int id : ids)
        {
            const Unit *u = entities.GetUnit(id);
            if (u && u->state == ActionState::Move && !u->path.empty())
                ++moving;
            if (u && ThroughGap(u->path))
                ++gap;
        }
        Check(moving == 10, "every unit of the group got a way");
        Check(gap == 10, "the whole group takes the gap");
    }

    // 4. Cost: 30 units ordered together, with the field and one search each
    {
        std::vector<int> ids;
        for (int i = 0; i < 30; ++i)
            ids.push_back(entities.SpawnUnitByArchetype(0, 10 + (i % 10) * 2, 40 + (i / 10) * 2, Faction::Resistance));
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        for (int k = 0; k < 10; ++k)
            entities.IssueMoveOrder(ids, goalX, goalY + (k % 2), pf);
        const auto t1 = Clock::now();
        for (int k = 0; k < 10; ++k)
        {
            for (int id : ids)
            {
                const Unit *u = entities.GetUnit(id);
                std::vector<PathNode> path;
                pf.FindPath(u->tileX, u->tileY, goalX, goalY + (k % 2), path, id);
            }
        }
        const auto t2 = Clock::now();
        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count() / 10.0; };
        std::printf(
            "30 units: group order with the field %.2f ms, 30 searches %.2f ms\n", ms(t1 - t0), ms(t2 - t1));
    }

    if (failures == 0)
        std::printf("flow field: all checks passed\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
