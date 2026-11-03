// Base layout of the computer player: the roads between the bases that stay free, the ground of each ally, the way
// out of the base that no structure may close, the places of the core buildings and the walls across passages.
// Everything here is deterministic (DetMath, fixed iteration order, the AI's own seeded generator).
#include "game/AIController.hpp"
#include "core/DetMath.hpp"
#include "core/Utility.hpp"
#include "game/EntityManager.hpp"
#include "game/Pathfinder.hpp"
#include "game/TileMap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <iostream>
#include <sstream>

namespace OpenArtOfWar
{

namespace
{

// A tile next to the door of a headquarters (below it) where a path can start
bool DoorOf(const Building &hq, const TileMap &map, int &outX, int &outY)
{
    const int cx = hq.tileX + hq.buildWidth / 2;
    const int below = hq.tileY + hq.buildHeight;
    for (int r = 0; r <= 4; ++r)
    {
        for (int dy = 0; dy <= r; ++dy)
        {
            for (int dx = -r; dx <= r; ++dx)
            {
                if (std::max(std::abs(dx), dy) != r)
                    continue;
                const int x = cx + dx, y = below + dy;
                if (x >= 0 && y >= 0 && x < map.GetWidth() && y < map.GetHeight() && map.IsPassable(x, y)
                    && map.GetWall(x, y) == 0)
                {
                    outX = x;
                    outY = y;
                    return true;
                }
            }
        }
    }
    // Above it, then
    for (int x = hq.tileX; x < hq.tileX + hq.buildWidth; ++x)
    {
        const int y = hq.tileY - 1;
        if (x >= 0 && y >= 0 && x < map.GetWidth() && map.IsPassable(x, y))
        {
            outX = x;
            outY = y;
            return true;
        }
    }
    return false;
}

bool IsExitBuilding(UnitType t)
{
    return t == UnitType::Headquarters || t == UnitType::Barracks || t == UnitType::Factory;
}

} // namespace

std::string AIBaseStyle::Describe() const
{
    static const char *const kLayouts[] = {"compact", "along the road", "fanned out"};
    std::ostringstream s;
    s << "Base style: " << kLayouts[layout] << (mirror ? ", mirrored" : "") << ", turned "
      << static_cast<int>(turnDeg) << " deg, " << (labEarly ? "laboratory early" : "laboratory after the factory")
      << (barracksFirst ? ", two barracks first" : "") << ", " << sparePower << " spare power, "
      << (towersOnly ? "towers instead of walls" : std::to_string(wallThickness) + "-row walls") << ", gates "
      << gateWidth << " wide, line +" << lineOffset << (bunkerHeavy ? ", bunkers first" : "");
    return s.str();
}

void AIController::SetSeed(uint32_t seed)
{
    m_seed = seed;
    Utility::Random r(seed ? seed : 0x2545f491u);
    r.Next(); // the first numbers of a fresh xorshift follow the seed too closely
    r.Next();
    AIBaseStyle s;
    s.layout = static_cast<AIBaseStyle::Layout>(r.Range(0, 2));
    s.turnDeg = static_cast<float>(r.Range(-30, 30));
    s.mirror = r.Range(0, 1) == 1;
    s.labEarly = r.Range(0, 2) == 0;
    s.barracksFirst = r.Range(0, 3) == 0;
    s.sparePower = 40 + 20 * r.Range(0, 2);
    s.wallThickness = r.Range(0, 3) == 0 ? 1 : 2;
    s.gateWidth = r.Range(2, 3);
    s.lineOffset = r.Range(0, 3);
    s.towersOnly = r.Range(0, 9) == 0;
    s.bunkerHeavy = r.Range(0, 1) == 1;
    m_style = s;
}

bool AIController::IsRoad(int x, int y) const
{
    if (!m_territoryReady || x < 0 || y < 0 || x >= m_terrW || y >= m_terrH)
        return false;
    return m_road[static_cast<size_t>(y) * m_terrW + x] != 0;
}

bool AIController::IsRoadCore(int x, int y) const
{
    if (!m_territoryReady || x < 0 || y < 0 || x >= m_terrW || y >= m_terrH)
        return false;
    return m_road[static_cast<size_t>(y) * m_terrW + x] == 2;
}

bool AIController::IsOwnGround(int x, int y) const
{
    if (!m_territoryReady)
        return true;
    if (x < 0 || y < 0 || x >= m_terrW || y >= m_terrH)
        return false;
    return m_ownGround[static_cast<size_t>(y) * m_terrW + x] != 0;
}

void AIController::PlanTerritory(const TileMap &map, const EntityManager &entities, Pathfinder &pathfinder)
{
    m_terrW = map.GetWidth();
    m_terrH = map.GetHeight();
    const size_t n = static_cast<size_t>(m_terrW) * m_terrH;
    m_road.assign(n, 0);
    m_ownGround.assign(n, 1);
    m_enemyRoads.clear();

    // The first headquarters of every player (entity order: the same on every computer)
    std::array<const Building *, kMaxPlayers> hq {};
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->state != ActionState::Die && b->type == UnitType::Headquarters && b->owner < kMaxPlayers
            && !hq[b->owner])
            hq[b->owner] = b;
    }
    auto centreOf = [&](PlayerId p, float &x, float &y) {
        x = hq[p]->tileX + hq[p]->buildWidth * 0.5f;
        y = hq[p]->tileY + hq[p]->buildHeight * 0.5f;
    };
    if (m_player < kMaxPlayers && hq[m_player])
    {
        m_homeX = hq[m_player]->tileX + hq[m_player]->buildWidth / 2;
        m_homeY = hq[m_player]->tileY + hq[m_player]->buildHeight / 2;
    }
    else
    {
        m_homeX = m_enemyHqX;
        m_homeY = m_enemyHqY;
    }

    // The ground between allies: every tile belongs to the nearest headquarters (the lower slot on a tie)
    if (m_player < kMaxPlayers && hq[m_player])
    {
        float ox = 0.0f, oy = 0.0f;
        centreOf(m_player, ox, oy);
        for (PlayerId q = 0; q < kMaxPlayers; ++q)
        {
            if (q == m_player || !hq[q] || Players::Hostile(m_player, q))
                continue;
            float qx = 0.0f, qy = 0.0f;
            centreOf(q, qx, qy);
            for (int y = 0; y < m_terrH; ++y)
            {
                for (int x = 0; x < m_terrW; ++x)
                {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float own = DetMath::Hypot(px - ox, py - oy), other = DetMath::Hypot(px - qx, py - qy);
                    if (other < own || (other == own && q < m_player))
                        m_ownGround[static_cast<size_t>(y) * m_terrW + x] = 0;
                }
            }
        }
    }

    // Roads: between every two headquarters of the map (allies and enemies alike), with a band along them
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        for (PlayerId q = static_cast<PlayerId>(p + 1); q < kMaxPlayers; ++q)
        {
            if (!hq[p] || !hq[q])
                continue;
            int sx = 0, sy = 0, tx = 0, ty = 0;
            if (!DoorOf(*hq[p], map, sx, sy) || !DoorOf(*hq[q], map, tx, ty))
                continue;
            std::vector<PathNode> path;
            bool partial = false;
            pathfinder.SetIgnoreUnitsOnce();
            pathfinder.FindPath(sx, sy, tx, ty, path, 0, &partial, true);
            if (path.empty())
                continue;
            std::vector<std::pair<int, int>> tiles;
            tiles.reserve(path.size() + 1);
            tiles.emplace_back(sx, sy);
            for (const PathNode &node : path)
                if (tiles.back() != std::make_pair(node.x, node.y))
                    tiles.emplace_back(node.x, node.y);
            for (const auto &[x, y] : tiles)
            {
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int bx = x + dx, by = y + dy;
                        if (bx < 0 || by < 0 || bx >= m_terrW || by >= m_terrH)
                            continue;
                        uint8_t &cell = m_road[static_cast<size_t>(by) * m_terrW + bx];
                        cell = std::max<uint8_t>(cell, (dx == 0 && dy == 0) ? 2 : 1);
                    }
                }
            }
            const bool mine = p == m_player || q == m_player;
            if (mine && Players::Hostile(p, q))
            {
                if (q == m_player)
                    std::reverse(tiles.begin(), tiles.end());
                m_enemyRoads.push_back(std::move(tiles));
            }
        }
    }
    m_territoryReady = true;
}

bool AIController::KeepsWayOut(
    int x, int y, int w, int h, const TileMap &map, const EntityManager &entities, bool needsExit) const
{
    if (!m_territoryReady)
        return true;
    const int mw = map.GetWidth(), mh = map.GetHeight();
    auto inRect = [&](int tx, int ty) {
        return tx >= x && ty >= y && tx < x + w && ty < y + h;
    };
    // A block of 2 x 2 free tiles: where a vehicle fits through
    auto freeTile = [&](int tx, int ty, bool withRect) {
        if (tx < 0 || ty < 0 || tx >= mw || ty >= mh)
            return false;
        if (withRect && inRect(tx, ty))
            return false;
        return map.IsPassable(tx, ty) && map.GetWall(tx, ty) == 0;
    };
    auto wide = [&](int bx, int by, bool withRect) {
        return freeTile(bx, by, withRect) && freeTile(bx + 1, by, withRect) && freeTile(bx, by + 1, withRect)
               && freeTile(bx + 1, by + 1, withRect);
    };
    // Where the way out leads: the roads farther from the base than its building zone
    const float far = static_cast<float>(entities.GetBuildRadius(m_player) + 4);
    std::vector<std::pair<int, int>> targets;
    for (int ty = 0; ty < mh; ++ty)
    {
        for (int tx = 0; tx < mw; ++tx)
        {
            if (m_road[static_cast<size_t>(ty) * mw + tx] != 2)
                continue;
            if (DetMath::Hypot(static_cast<float>(tx - m_homeX), static_cast<float>(ty - m_homeY)) <= far)
                continue;
            targets.emplace_back(tx, ty);
        }
    }
    if (targets.empty())
        return true;

    auto flood = [&](bool withRect) {
        std::vector<uint8_t> reached(static_cast<size_t>(mw) * mh, 0);
        std::vector<std::pair<int, int>> frontier;
        for (const auto &[tx, ty] : targets)
        {
            for (int oy = -1; oy <= 0; ++oy)
            {
                for (int ox = -1; ox <= 0; ++ox)
                {
                    const int bx = tx + ox, by = ty + oy;
                    if (bx < 0 || by < 0 || bx >= mw || by >= mh || reached[static_cast<size_t>(by) * mw + bx]
                        || !wide(bx, by, withRect))
                        continue;
                    reached[static_cast<size_t>(by) * mw + bx] = 1;
                    frontier.emplace_back(bx, by);
                }
            }
        }
        static const int kDx[4] = {1, -1, 0, 0}, kDy[4] = {0, 0, 1, -1};
        for (size_t i = 0; i < frontier.size(); ++i)
        {
            const auto [fx, fy] = frontier[i];
            for (int k = 0; k < 4; ++k)
            {
                const int nx = fx + kDx[k], ny = fy + kDy[k];
                if (nx < 0 || ny < 0 || nx >= mw || ny >= mh || reached[static_cast<size_t>(ny) * mw + nx]
                    || !wide(nx, ny, withRect))
                    continue;
                reached[static_cast<size_t>(ny) * mw + nx] = 1;
                frontier.emplace_back(nx, ny);
            }
        }
        return reached;
    };
    // Does a reached 2 x 2 block touch the ring around (bx, by, bw, bh)?
    auto touches = [&](const std::vector<uint8_t> &reached, int bx, int by, int bw, int bh) {
        for (int ty = by - 1; ty <= by + bh; ++ty)
        {
            for (int tx = bx - 1; tx <= bx + bw; ++tx)
            {
                if (tx >= bx && tx < bx + bw && ty >= by && ty < by + bh)
                    continue;
                for (int oy = -1; oy <= 0; ++oy)
                {
                    for (int ox = -1; ox <= 0; ++ox)
                    {
                        const int kx = tx + ox, ky = ty + oy;
                        if (kx >= 0 && ky >= 0 && kx < mw && ky < mh && reached[static_cast<size_t>(ky) * mw + kx])
                            return true;
                    }
                }
            }
        }
        return false;
    };

    const std::vector<uint8_t> after = flood(true);
    // A barracks, a factory or a headquarters in a pocket the others have nearly closed: its troops would never
    // get out (only the structures already standing were checked before). Unless the base has no wide way out at
    // all (narrow exits of the map): then its headquarters is not reached either and the pocket is no worse
    if (needsExit && !touches(after, x, y, w, h))
    {
        for (const auto &e : entities.GetAllEntities())
        {
            const Building *b = e.AsBuilding();
            if (b && b->alive && b->state != ActionState::Die && b->owner == m_player && b->type == UnitType::Headquarters
                && touches(after, b->tileX, b->tileY, b->buildWidth, b->buildHeight))
                return false;
        }
    }
    std::vector<uint8_t> before;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->owner != m_player || !IsExitBuilding(b->type))
            continue;
        if (touches(after, b->tileX, b->tileY, b->buildWidth, b->buildHeight))
            continue;
        // Closed in already (a map's own base, terrain): this structure is not the one to blame
        if (before.empty())
            before = flood(false);
        if (touches(before, b->tileX, b->tileY, b->buildWidth, b->buildHeight))
            return false;
    }
    return true;
}

bool AIController::FindCoreSite(
    int &outX,
    int &outY,
    int w,
    int h,
    float dirX,
    float dirY,
    bool rearOnly,
    int minRing,
    const TileMap &map,
    const EntityManager &entities,
    int crowding,
    bool needsExit) const
{
    const int cx = m_homeX, cy = m_homeY;
    if (cx < 0 || cy < 0)
        return false;
    float tdx = static_cast<float>(m_playerHqX - cx), tdy = static_cast<float>(m_playerHqY - cy);
    const float tlen = std::max(1.0f, DetMath::Hypot(tdx, tdy));
    tdx /= tlen;
    tdy /= tlen;
    const float dlen = std::max(0.001f, DetMath::Hypot(dirX, dirY));
    dirX /= dlen;
    dirY /= dlen;
    // Fanned out bases keep two tiles between buildings, the others one
    int gap = m_style.layout == AIBaseStyle::Fan ? 2 : 1;
    const int maxRing = entities.GetBuildRadius(m_player) + 2;
    if (crowding >= 1)
        gap = 0; // a crowded base: the buildings may stand side by side (the way out is still checked)

    auto overlapsBuilding = [&](int px, int py, int pw, int ph) {
        for (const auto &e : entities.GetAllEntities())
        {
            const Building *b = e.AsBuilding();
            if (!b || !b->alive || b->state == ActionState::Die)
                continue;
            if (px < b->tileX + b->buildWidth && px + pw > b->tileX && py < b->tileY + b->buildHeight
                && py + ph > b->tileY)
                return true;
        }
        return false;
    };
    auto nearGate = [&](int px, int py) {
        for (int g = 0; g < 2; ++g)
        {
            if (m_gateX[g] < 0)
                continue;
            const int keep = rearOnly ? 6 : 2; // power plants and the like stay well away from the exits
            if (px + w + keep > m_gateX[g] && px - keep <= m_gateX[g] && py + h + keep > m_gateY[g]
                && py - keep <= m_gateY[g])
                return true;
        }
        return false;
    };

    struct Candidate
    {
        float score;
        int x, y;
    };
    for (int r = std::max(1, minRing); r <= maxRing; ++r)
    {
        std::vector<Candidate> ring;
        for (int oy = -r; oy <= r; ++oy)
        {
            for (int ox = -r; ox <= r; ++ox)
            {
                if (std::max(std::abs(ox), std::abs(oy)) != r)
                    continue;
                const float ol = std::max(0.001f, DetMath::Hypot(static_cast<float>(ox), static_cast<float>(oy)));
                const float facing = (ox * tdx + oy * tdy) / ol; // 1: toward the enemy
                if (rearOnly && facing > 0.05f)
                    continue;
                const int px = cx + ox - w / 2, py = cy + oy - h / 2;
                if (nearGate(px, py))
                    continue;
                if (!entities.CanPlaceBuilding(px, py, w, h, map, m_player))
                    continue;
                bool ok = true;
                for (int ty = py; ty < py + h && ok; ++ty)
                {
                    for (int tx = px; tx < px + w && ok; ++tx)
                        ok = (crowding >= 2 ? !IsRoadCore(tx, ty) : !IsRoad(tx, ty)) && IsOwnGround(tx, ty)
                             && !entities.IsTileOccupiedByUnit(tx, ty);
                }
                if (!ok || (gap > 0 && overlapsBuilding(px - gap, py - gap, w + 2 * gap, h + 2 * gap)))
                    continue;
                float score = (ox * dirX + oy * dirY) / ol;
                if (m_style.layout == AIBaseStyle::Road)
                {
                    // Along the road: next to the band of a road is better
                    bool byRoad = false;
                    for (int ty = py - 2; ty < py + h + 2 && !byRoad; ++ty)
                        for (int tx = px - 2; tx < px + w + 2 && !byRoad; ++tx)
                            byRoad = IsRoad(tx, ty);
                    score += byRoad ? 0.35f : 0.0f;
                }
                ring.push_back({score, px, py});
            }
        }
        std::stable_sort(ring.begin(), ring.end(), [](const Candidate &a, const Candidate &b) {
            return a.score > b.score;
        });
        // The worst side of a ring is still taken before the next ring (compact), but not a spot facing away
        // from where the building wants to be when the next ring has better ones: only the better half counts
        int tried = 0;
        for (const Candidate &c : ring)
        {
            if (c.score < -0.2f && m_style.layout != AIBaseStyle::Compact)
                break;
            if (++tried > 24)
                break; // the way-out check floods the map: a ring gets a limited number of tries
            if (KeepsWayOut(c.x, c.y, w, h, map, entities, needsExit))
            {
                outX = c.x;
                outY = c.y;
                return true;
            }
        }
    }
    return false;
}

bool AIController::PlanPassageWalls(
    int cx, int cy, int hqRadius, const TileMap &map, const EntityManager &entities,
    std::vector<std::pair<int, int>> &wallTiles)
{
    const int mw = map.GetWidth(), mh = map.GetHeight();
    auto open = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < mw && y < mh && map.IsPassable(x, y) && map.GetWall(x, y) == 0;
    };
    const float maxD = static_cast<float>(entities.GetBuildRadius(m_player)) - 0.5f;
    const float minD = std::min(static_cast<float>(hqRadius + 10 + m_style.lineOffset), maxD - 1.0f);
    constexpr int kMaxHalf = 6; // a passage up to 13 tiles across is closed
    const int gateLo = m_style.gateWidth >= 3 ? -1 : 0, gateHi = 1;

    // One cross-section: from (x, y) both ways along (px, py) to the first obstacle; false when it is too wide
    auto section = [&](int x, int y, int px, int py, int &a, int &b) {
        a = b = 0;
        while (a < kMaxHalf && open(x + px * (a + 1), y + py * (a + 1)))
            ++a;
        while (b < kMaxHalf && open(x - px * (b + 1), y - py * (b + 1)))
            ++b;
        return a < kMaxHalf && b < kMaxHalf;
    };

    std::set<std::pair<int, int>> planned;
    int gates = 0;
    bool any = false;
    for (const auto &road : m_enemyRoads)
    {
        const int n = static_cast<int>(road.size());
        int bestI = -1, bestWidth = 1 << 30, bestPx = 0, bestPy = 0, bestSx = 0, bestSy = 0, bestRows = 1;
        for (int i = 0; i < n; ++i)
        {
            const auto [x, y] = road[i];
            const float d = DetMath::Hypot(static_cast<float>(x - cx), static_cast<float>(y - cy));
            if (d > maxD + 4.0f && i > 0)
                break; // out of the building zone for good
            if (d < minD || d > maxD)
                continue;
            // The way the road runs here, snapped to an axis: the wall goes straight across it
            const auto [ax, ay] = road[std::max(0, i - 2)];
            const auto [bx, by] = road[std::min(n - 1, i + 2)];
            const int rdx = bx - ax, rdy = by - ay;
            int px = 0, py = 0, sx = 0, sy = 0; // across, and one step outward along the road
            if (std::abs(rdx) >= std::abs(rdy))
            {
                py = 1;
                sx = rdx >= 0 ? 1 : -1;
            }
            else
            {
                px = 1;
                sy = rdy >= 0 ? 1 : -1;
            }
            int a = 0, b = 0;
            if (!section(x, y, px, py, a, b))
                continue;
            // Every tile of the line: on own ground, inside the zone, no other road crossing it outside the gate
            bool fits = true;
            for (int o = -b; o <= a && fits; ++o)
            {
                const int tx = x + px * o, ty = y + py * o;
                const bool gate = o >= gateLo && o <= gateHi;
                fits = IsOwnGround(tx, ty)
                       && DetMath::Hypot(static_cast<float>(tx - cx), static_cast<float>(ty - cy)) <= maxD
                       && (gate || !IsRoadCore(tx, ty));
            }
            if (!fits)
                continue;
            const int width = a + b + 1;
            if (width <= gateHi - gateLo + 1)
                continue; // nothing but the gate: nothing to wall
            int rows = 1;
            if (m_style.wallThickness >= 2)
            {
                int a2 = 0, b2 = 0;
                if (section(x + sx, y + sy, px, py, a2, b2))
                    rows = 2;
            }
            if (width < bestWidth)
            {
                bestWidth = width;
                bestI = i;
                bestPx = px;
                bestPy = py;
                bestSx = sx;
                bestSy = sy;
                bestRows = rows;
            }
        }
        if (bestI < 0)
            continue;
        any = true;
        const auto [x, y] = road[bestI];
        for (int row = 0; row < bestRows; ++row)
        {
            const int lx = x + bestSx * row, ly = y + bestSy * row;
            int a = 0, b = 0;
            section(lx, ly, bestPx, bestPy, a, b);
            for (int o = -b; o <= a; ++o)
            {
                if (o >= gateLo && o <= gateHi)
                    continue;
                const int tx = lx + bestPx * o, ty = ly + bestPy * o;
                if (IsOwnGround(tx, ty) && planned.insert({tx, ty}).second)
                    wallTiles.emplace_back(tx, ty);
            }
        }
        // The gate: the first two different passages give the two exits
        bool newGate = true;
        for (int g = 0; g < gates; ++g)
            newGate = newGate && std::max(std::abs(m_gateX[g] - x), std::abs(m_gateY[g] - y)) > 3;
        if (newGate && gates < 2)
        {
            m_gateX[gates] = x;
            m_gateY[gates] = y;
            ++gates;
        }
        if (!newGate)
            continue; // the defenses of that passage are planned already
        // A tower and a bunker behind the gate, on both sides of the road
        const auto [hx, hy] = road[std::max(0, bestI - 3)];
        const int side = gateHi + 2;
        AIDefenseSite::Kind first = m_style.bunkerHeavy ? AIDefenseSite::Bunker : AIDefenseSite::Tower;
        AIDefenseSite::Kind second = m_style.bunkerHeavy ? AIDefenseSite::Tower : AIDefenseSite::Bunker;
        m_defensePlan.push_back({first, hx + bestPx * side - 1, hy + bestPy * side - 1, false});
        m_defensePlan.push_back({second, hx - bestPx * (side + 1) - 1, hy - bestPy * (side + 1) - 1, false});
    }
    return any;
}

} // namespace OpenArtOfWar
