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

void AIController::UpdateBaseDefense(EntityManager &entities, Pathfinder &pathfinder)
{
    // Find all player units trespassing within 18 tiles of enemy base
    int threatEntityId = 0;
    float closestDist = 18.0f * TILE_WIDTH;

    for (const auto &e : entities.GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die || !IsFoe(e))
            continue;
        if (!AiSees(e, entities))
            continue; // AI only detects threats it actually sees!
        float dx = e.GetUnitCenterPixelX() - (m_enemyHqX * TILE_WIDTH + TILE_WIDTH * 0.5f);
        float dy = e.GetUnitCenterPixelY() - (m_enemyHqY * TILE_HEIGHT + TILE_HEIGHT * 0.5f);
        float d = std::sqrt(dx * dx + dy * dy);
        if (d < closestDist)
        {
            closestDist = d;
            threatEntityId = e.id;
        }
    }

    if (threatEntityId == 0)
        return;

    Entity *threat = entities.GetEntity(threatEntityId);
    if (!threat)
        return;
    m_lastThreatX = threat->tileX; // the base builder reinforces this side
    m_lastThreatY = threat->tileY;
    if (threat->id != m_lastThreatId)
    {
        m_lastThreatId = threat->id;
        Think(
            "DEFENSE",
            "Enemy spotted near the base at (" + std::to_string(threat->tileX) + ", " + std::to_string(threat->tileY)
                + "): sending idle troops to intercept");
    }

    // Rally nearby idle reserve units to intercept the threat (marching groups keep their own orders)
    std::vector<int> defenders;
    for (const auto &e : entities.GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die || e.isBuilding || e.owner != m_player)
            continue;
        if (m_squadController.IsSquadMember(e.id))
            continue;
        if (e.state == ActionState::Idle && e.targetEntityId == 0)
        {
            float dx = e.GetUnitCenterPixelX() - threat->GetUnitCenterPixelX();
            float dy = e.GetUnitCenterPixelY() - threat->GetUnitCenterPixelY();
            float d = std::sqrt(dx * dx + dy * dy);
            if (d <= 22.0f * TILE_WIDTH)
            {
                defenders.push_back(e.id);
            }
        }
    }

    if (!defenders.empty())
    {
        entities.IssueAttackMoveOrder(defenders, threat->tileX, threat->tileY, pathfinder);
    }
}

bool AIController::FindSiteNear(
    int &x,
    int &y,
    int w,
    int h,
    int searchRadius,
    const TileMap &map,
    const EntityManager &entities,
    bool *unitBlocked,
    bool needsExit) const
{
    if (unitBlocked)
        *unitBlocked = false;
    auto clearOfGates = [&](int px, int py) {
        for (int g = 0; g < 2; ++g)
        {
            if (m_gateX[g] < 0)
                continue;
            // keep a 2-tile corridor around each exit free of structures
            if (px + w + 1 >= m_gateX[g] && px - 2 <= m_gateX[g] && py + h + 1 >= m_gateY[g] && py - 2 <= m_gateY[g])
                return false;
        }
        return true;
    };
    for (int r = 0; r <= searchRadius; ++r)
    {
        for (int oy = -r; oy <= r; ++oy)
        {
            for (int ox = -r; ox <= r; ++ox)
            {
                if (std::max(std::abs(ox), std::abs(oy)) != r)
                    continue;
                const int px = x + ox, py = y + oy;
                if (!clearOfGates(px, py))
                    continue;
                if (!entities.CanPlaceBuilding(px, py, w, h, map, m_player))
                    continue;
                // Not on a road between the bases, not on an ally's ground
                bool free = true;
                for (int ty = py; ty < py + h && free; ++ty)
                {
                    for (int tx = px; tx < px + w && free; ++tx)
                        free = !IsRoad(tx, ty) && IsOwnGround(tx, ty);
                }
                if (!free)
                    continue;
                bool unitInside = false;
                for (int ty = py; ty < py + h && !unitInside; ++ty)
                {
                    for (int tx = px; tx < px + w && !unitInside; ++tx)
                        unitInside = entities.IsTileOccupiedByUnit(tx, ty);
                }
                if (unitInside)
                {
                    if (unitBlocked)
                        *unitBlocked = true;
                    continue;
                }
                if (!KeepsWayOut(px, py, w, h, map, entities, needsExit))
                    continue; // it would close the base in (or itself)
                x = px;
                y = py;
                return true;
            }
        }
    }
    return false;
}

void AIController::PlanBaseDefenses(const TileMap &map, const EntityManager &entities, Pathfinder &pathfinder)
{
    m_defensePlan.clear();
    m_defensePlanned = true;
    m_gateX[0] = m_gateX[1] = m_gateY[0] = m_gateY[1] = -1;

    // Base centre: the AI headquarters; threat direction: toward the player's base
    int cx = m_enemyHqX, cy = m_enemyHqY;
    int hqRadius = 2;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->owner == m_player && b->type == UnitType::Headquarters)
        {
            cx = b->tileX + b->buildWidth / 2;
            cy = b->tileY + b->buildHeight / 2;
            hqRadius = std::max(b->buildWidth, b->buildHeight) / 2 + 1;
            break;
        }
    }
    // The roads between the bases and the ground of the allies come first: nothing below may close them
    if (!m_territoryReady)
        PlanTerritory(map, entities, pathfinder);
    Think("BUILD", m_style.Describe());

    float tdx = static_cast<float>(m_playerHqX - cx);
    float tdy = static_cast<float>(m_playerHqY - cy);
    const float tlen = std::max(1.0f, DetMath::Hypot(tdx, tdy));
    tdx /= tlen;
    tdy /= tlen;
    const float threatAngle = DetMath::Atan2(tdy, tdx);
    // The line of defense: as far out as the building zone allows (a wall near the HQ would close the base in)
    const float maxD = static_cast<float>(entities.GetBuildRadius(m_player)) - 0.5f;
    const float wallRadius = std::max(
        static_cast<float>(hqRadius + 4),
        std::min(static_cast<float>(hqRadius + 10 + m_style.lineOffset), maxD - 1.0f));
    auto ringTile = [&](float radius, float angleOffsetDeg, int &x, int &y) {
        const float a = threatAngle + angleOffsetDeg * static_cast<float>(M_PI) / 180.0f;
        x = std::clamp(static_cast<int>(std::lround(cx + DetMath::Cos(a) * radius)), 1, map.GetWidth() - 2);
        y = std::clamp(static_cast<int>(std::lround(cy + DetMath::Sin(a) * radius)), 1, map.GetHeight() - 2);
    };
    auto add = [&](AIDefenseSite::Kind kind, int x, int y) {
        m_defensePlan.push_back({kind, x, y, false});
    };

    // Power plants, the barracks, the factory and the laboratory come from the core build order (UpdateCoreBuildOrder)

    // 1. Like the bases of the campaign: a wall straight across the narrowest passage of every road to an enemy,
    //    the gate on the road, a tower and a bunker behind it
    std::vector<std::pair<int, int>> wallTiles;
    std::vector<int> wallRow; // arc only: 0 the inner row, 1 the outer one
    const bool passages = PlanPassageWalls(cx, cy, hqRadius, map, entities, wallTiles);
    if (passages)
    {
        Think("BUILD", "Closing " + std::to_string(wallTiles.size()) + " tiles of the passages toward the enemy");
    }
    else
    {
        // 2. Open ground: a short arc in front of the base, the exits where the road to the enemy crosses it and
        //    beside it
        std::vector<PathNode> road;
        bool partial = false;
        pathfinder.SetIgnoreUnitsOnce();
        pathfinder.FindPath(cx, cy + hqRadius + 1, m_playerHqX, m_playerHqY, road, 0, &partial, true);
        for (const auto &n : road)
        {
            if (DetMath::Hypot(static_cast<float>(n.x - cx), static_cast<float>(n.y - cy)) >= wallRadius - 0.5f)
            {
                m_gateX[0] = n.x;
                m_gateY[0] = n.y;
                break;
            }
        }
        if (m_gateX[0] < 0)
            ringTile(wallRadius, 0.0f, m_gateX[0], m_gateY[0]);
        const float gateAngle
            = DetMath::Atan2(static_cast<float>(m_gateY[0] - cy), static_cast<float>(m_gateX[0] - cx));
        auto angleDiff = [](float a, float b) {
            return std::abs(std::remainder(a - b, 2.0f * static_cast<float>(M_PI)));
        };
        const float rad30 = 30.0f * static_cast<float>(M_PI) / 180.0f;
        const float firstSide
            = angleDiff(gateAngle + rad30, threatAngle) <= angleDiff(gateAngle - rad30, threatAngle) ? 1.0f : -1.0f;
        for (float side : {firstSide, -firstSide})
        {
            const float a = gateAngle + side * rad30;
            const int gx
                = std::clamp(static_cast<int>(std::lround(cx + DetMath::Cos(a) * wallRadius)), 1, map.GetWidth() - 2);
            const int gy
                = std::clamp(static_cast<int>(std::lround(cy + DetMath::Sin(a) * wallRadius)), 1, map.GetHeight() - 2);
            if (map.IsPassable(gx, gy))
            {
                m_gateX[1] = gx;
                m_gateY[1] = gy;
                break;
            }
        }
        const int rows = m_style.towersOnly ? 0 : std::clamp(m_style.wallThickness, 1, 2);
        const float halfArc = 40.0f * static_cast<float>(M_PI) / 180.0f;
        for (int row = 0; row < rows; ++row)
        {
            const float radius = wallRadius + static_cast<float>(row);
            const float step = 0.45f / radius;
            for (float a = -halfArc; a <= halfArc; a += step)
            {
                const int wx = static_cast<int>(std::lround(cx + DetMath::Cos(threatAngle + a) * radius));
                const int wy = static_cast<int>(std::lround(cy + DetMath::Sin(threatAngle + a) * radius));
                if (wx < 1 || wy < 1 || wx >= map.GetWidth() - 1 || wy >= map.GetHeight() - 1)
                    continue;
                if (std::find(wallTiles.begin(), wallTiles.end(), std::make_pair(wx, wy)) != wallTiles.end())
                    continue;
                bool nearGate = false;
                for (int g = 0; g < 2; ++g)
                {
                    if (m_gateX[g] >= 0 && std::max(std::abs(wx - m_gateX[g]), std::abs(wy - m_gateY[g])) <= 1)
                        nearGate = true;
                }
                // A road to an ally or the band of one stays open (an extra opening), so does an ally's ground;
                // nothing walks over rock or water anyway
                if (nearGate || IsRoad(wx, wy) || !IsOwnGround(wx, wy) || !map.IsPassable(wx, wy)
                    || map.GetWall(wx, wy) > 0)
                    continue;
                wallTiles.emplace_back(wx, wy);
                wallRow.push_back(row);
            }
        }
        // Towers right behind the main exit, a bunker covering the second one, the flanks of the arc
        const float innerRadius = wallRadius - 3.0f;
        int x = 0, y = 0;
        ringTile(innerRadius, 0.0f, x, y);
        add(AIDefenseSite::Tower, x - 1, y - 1);
        ringTile(innerRadius, -30.0f, x, y);
        add(m_style.bunkerHeavy ? AIDefenseSite::Tower : AIDefenseSite::Bunker, x - 1, y - 1);
        ringTile(innerRadius, 30.0f, x, y);
        add(AIDefenseSite::Tower, x - 1, y - 1);
    }
    // More firepower when the walls are few
    if (m_style.towersOnly)
    {
        int x = 0, y = 0;
        ringTile(wallRadius - 2.0f, -15.0f, x, y);
        add(AIDefenseSite::Tower, x - 1, y - 1);
        ringTile(wallRadius - 2.0f, 15.0f, x, y);
        add(AIDefenseSite::Bunker, x - 1, y - 1);
        if (passages)
            wallTiles.clear();
    }

    // 3. Walls last, and only where an enemy can walk in: a ring tile that links the inside of the ring with
    //    the outside one reachable from a hostile base. Segments in front of trees, cliffs or a closed pocket
    //    stop nobody and are left out.
    if (!passages)
    {
        const int mw = map.GetWidth(), mh = map.GetHeight();
        auto idx = [mw](int px, int py) {
            return py * mw + px;
        };
        auto open = [&](int px, int py) {
            return px >= 0 && py >= 0 && px < mw && py < mh && map.IsPassable(px, py) && map.GetWall(px, py) == 0;
        };
        std::vector<std::uint8_t> ring(static_cast<std::size_t>(mw) * mh, 0), inside(ring.size(), 0),
            outside(ring.size(), 0);
        for (std::size_t i = 0; i < wallTiles.size(); ++i)
            if (wallRow[i] == 0)
                ring[idx(wallTiles[i].first, wallTiles[i].second)] = 1; // the outer row lies outside the ring
        auto distOf = [&](int px, int py) {
            return DetMath::Hypot(static_cast<float>(px - cx), static_cast<float>(py - cy));
        };
        auto flood = [&](std::vector<std::uint8_t> &mark, std::vector<std::pair<int, int>> frontier, bool in) {
            for (std::size_t i = 0; i < frontier.size(); ++i)
            {
                const auto [fx, fy] = frontier[i];
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nx = fx + dx, ny = fy + dy;
                        if ((dx == 0 && dy == 0) || !open(nx, ny) || ring[idx(nx, ny)] || mark[idx(nx, ny)])
                            continue;
                        if (in != (distOf(nx, ny) < wallRadius))
                            continue;
                        mark[idx(nx, ny)] = 1;
                        frontier.emplace_back(nx, ny);
                    }
                }
            }
        };
        std::vector<std::pair<int, int>> seedsIn, seedsOut;
        for (int py = std::max(0, cy - hqRadius - 2); py <= std::min(mh - 1, cy + hqRadius + 2); ++py)
        {
            for (int px = std::max(0, cx - hqRadius - 2); px <= std::min(mw - 1, cx + hqRadius + 2); ++px)
            {
                if (open(px, py) && !ring[idx(px, py)] && distOf(px, py) < wallRadius)
                {
                    inside[idx(px, py)] = 1;
                    seedsIn.emplace_back(px, py);
                }
            }
        }
        for (const auto &e : entities.GetAllEntities())
        {
            if (!e.alive || e.state == ActionState::Die || !Players::Hostile(e.owner, m_player)
                || e.faction == Faction::Neutral)
                continue;
            for (int dy = -2; dy <= 2; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    const int px = e.tileX + dx, py = e.tileY + dy;
                    if (open(px, py) && !ring[idx(px, py)] && !outside[idx(px, py)] && distOf(px, py) >= wallRadius)
                    {
                        outside[idx(px, py)] = 1;
                        seedsOut.emplace_back(px, py);
                    }
                }
            }
        }
        flood(inside, std::move(seedsIn), true);
        const bool knowsEnemy = !seedsOut.empty();
        flood(outside, std::move(seedsOut), false);
        int skipped = 0;
        // The inner row decides (it links the inside with the outside the enemy comes from); the outer row only
        // goes where the inner one next to it stays
        std::vector<std::uint8_t> kept(wallTiles.size(), 0);
        for (std::size_t i = 0; i < wallTiles.size(); ++i)
        {
            if (wallRow[i] != 0)
                continue;
            const auto [wx, wy] = wallTiles[i];
            bool linksIn = false, linksOut = false;
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int nx = wx + dx, ny = wy + dy;
                    if (nx < 0 || ny < 0 || nx >= mw || ny >= mh)
                        continue;
                    linksIn = linksIn || inside[idx(nx, ny)];
                    linksOut = linksOut || outside[idx(nx, ny)];
                }
            }
            kept[i] = !knowsEnemy || (linksIn && linksOut);
        }
        for (std::size_t i = 0; i < wallTiles.size(); ++i)
        {
            if (wallRow[i] == 0)
                continue;
            for (std::size_t j = 0; j < wallTiles.size() && !kept[i]; ++j)
                kept[i] = wallRow[j] == 0 && kept[j]
                          && std::max(
                                 std::abs(wallTiles[i].first - wallTiles[j].first),
                                 std::abs(wallTiles[i].second - wallTiles[j].second))
                                 <= 1;
        }
        for (std::size_t i = 0; i < wallTiles.size(); ++i)
        {
            if (!kept[i])
            {
                ++skipped;
                continue;
            }
            add(AIDefenseSite::Wall, wallTiles[i].first, wallTiles[i].second);
        }
        if (skipped > 0)
            Think("BUILD", "Leaving out " + std::to_string(skipped) + " wall tiles nobody can walk through");
    }

    else
    {
        for (const auto &[wx, wy] : wallTiles)
            add(AIDefenseSite::Wall, wx, wy);
    }

    std::ostringstream plan;
    plan << "Base plan: " << m_defensePlan.size() << " items, " << wallTiles.size()
         << (passages ? " wall tiles across passages" : " wall tiles in front of the base") << ", exits (" << m_gateX[0]
         << "," << m_gateY[0] << ") and (" << m_gateX[1] << "," << m_gateY[1] << ")";
    Think("BUILD", plan.str());
}

bool AIController::BuildDefenseSite(AIDefenseSite &site, TileMap &map, EntityManager &entities)
{
    if (!entities.HasBuildingType(UnitType::Headquarters, m_player, true))
        return false; // no finished HQ, no construction (the spot stays planned)
    const int cost = DefenseCost(site.kind, entities, side());
    // A unit on the spot only delays the work; after this many build steps (3 s each) the spot is given up
    constexpr int kMaxUnitWaits = 20;
    if (site.kind == AIDefenseSite::Wall)
    {
        const int x = site.tileX, y = site.tileY;
        // Hard limits (data/limits.yaml) bind the computer as well: the segment is given up
        if (!BuildLimits::CanBuildMore(entities, map, m_player, side() == Faction::Confederation ? 15 : 25))
        {
            site.done = true;
            ThinkOnce("limitwall", "BUILD", "Walls: the building limit is reached");
            return false;
        }
        // Checked first: a unit also makes the tile impassable and fails canPlaceBuilding
        if (entities.IsTileOccupiedByUnit(x, y))
        {
            site.done = ++site.attempts >= kMaxUnitWaits; // a unit stands there: try again next step
            return false;
        }
        if (!map.IsPassable(x, y) || map.GetWall(x, y) > 0 || !entities.CanPlaceBuilding(x, y, 1, 1, map, m_player)
            || !IsOwnGround(x, y) || !KeepsWayOut(x, y, 1, 1, map, entities))
        {
            site.done = true; // blocked for good (terrain / building / it would close the base): skip this segment
            return false;
        }
        m_credits -= cost;
        map.SetWall(x, y, side(), 100, m_player);
        m_startingWalls.push_back({x, y, side(), cost, false, 0.0f}); // rebuilt when destroyed
        site.done = true;
        return true;
    }

    const int index = FactionBuildingIndex(site.kind, side());
    // Hard limits (data/limits.yaml) bind the computer as well: no more of that kind, the spot is given up
    if (!BuildLimits::CanBuildMore(entities, map, m_player, index))
    {
        site.done = true;
        ThinkOnce(
            "limit" + std::to_string(index),
            "BUILD",
            std::string("No ") + BuildLimits::KeyOf(BuildLimits::KindOf(index))
                + " is started: the building limit is reached");
        return false;
    }
    const BuildingConfig *cfg = entities.GetBuildingConfig(index);
    const int w = cfg ? cfg->width : 2;
    const int h = cfg ? cfg->height : 2;
    static const char *const kKindNames[]
        = {"tower",
           "bunker",
           "wall",
           "laboratory (behind the HQ)",
           "power plant (behind the HQ)",
           "barracks",
           "factory",
           "headquarters"};
    // Foresight: a building that takes power goes up only when the finished plants carry it as well; else it waits
    // and the next step raises a power plant (a base in the dark trains, researches and shoots nothing)
    const int power = entities.BuildingPower(index);
    if (power < 0)
    {
        const int balance = PowerBalance(entities, false);
        if (balance + power < 0)
        {
            m_powerNeeded = std::max(m_powerNeeded, -power);
            ThinkOnce(
                "power" + std::to_string(site.kind),
                "BUILD",
                std::string(kKindNames[std::clamp(static_cast<int>(site.kind), 0, 7)]) + " waits: it takes "
                    + std::to_string(-power) + " kW and only " + std::to_string(std::max(0, balance))
                    + " kW are spare - a power plant first");
            return false;
        }
    }
    int x = site.tileX, y = site.tileY;
    const bool core = site.kind == AIDefenseSite::Barracks || site.kind == AIDefenseSite::Factory
                      || site.kind == AIDefenseSite::PowerPlant || site.kind == AIDefenseSite::Lab
                      || site.kind == AIDefenseSite::Headquarters;
    const int searchRadius = site.exact ? 0 : (core ? 8 : 3);
    bool unitBlocked = false;
    const bool needsExit = site.kind == AIDefenseSite::Barracks || site.kind == AIDefenseSite::Factory
                           || site.kind == AIDefenseSite::Headquarters;
    if (!FindSiteNear(x, y, w, h, searchRadius, map, entities, &unitBlocked, needsExit))
    {
        // No room around the planned spot; when only units were in the way, try again later
        site.done = !unitBlocked || ++site.attempts >= kMaxUnitWaits;
        return false;
    }
    const int id = entities.SpawnBuildingByIndex(index, x, y, side(), 0.0f, m_player);
    if (id <= 0)
    {
        site.done = true;
        return false;
    }
    m_credits -= cost;
    for (int ty = y; ty < y + h && ty < map.GetHeight(); ++ty)
    {
        for (int tx = x; tx < x + w && tx < map.GetWidth(); ++tx)
            map.SetPassable(tx, ty, false);
    }
    if (site.kind == AIDefenseSite::PowerPlant)
        m_powerNeeded = 0; // the plant for what waited is coming
    Think(
        "BUILD",
        std::string("Building a ") + kKindNames[site.kind] + " at (" + std::to_string(x) + ", " + std::to_string(y)
            + ") for $" + std::to_string(cost));
    if (const Building *b = entities.GetBuilding(id))
    {
        AIStructurePlan plan;
        plan.type = b->type;
        plan.rawBuildingType = index;
        plan.tileX = x;
        plan.tileY = y;
        plan.width = w;
        plan.height = h;
        plan.creditCost = cost;
        m_startingBuildings.push_back(plan); // remembered for rebuilding
    }
    site.tileX = x;
    site.tileY = y;
    site.done = true;
    return true;
}

} // namespace OpenArtOfWar
