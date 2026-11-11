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
    s << "Base style: " << kLayouts[layout] << (mirror ? ", mirrored" : "") << ", turned " << static_cast<int>(turnDeg)
      << " deg, " << (labEarly ? "laboratory early" : "laboratory after the factory")
      << (barracksFirst ? ", two barracks first" : "") << ", " << sparePower << " spare power, "
      << (towersOnly ? "towers instead of walls" : std::to_string(wallThickness) + "-row walls") << ", gates "
      << gateWidth << " wide, line +" << lineOffset << (bunkerHeavy ? ", bunkers first" : "");
    return s.str();
}

void AIController::SetSeed(std::uint32_t seed)
{
    m_seed = seed;
    Utility::RandomGenerator r(seed ? seed : 0x2545f491u);
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
    return m_road[static_cast<std::size_t>(y) * m_terrW + x] != 0;
}

bool AIController::IsRoadCore(int x, int y) const
{
    if (!m_territoryReady || x < 0 || y < 0 || x >= m_terrW || y >= m_terrH)
        return false;
    return m_road[static_cast<std::size_t>(y) * m_terrW + x] == 2;
}

bool AIController::IsOwnGround(int x, int y) const
{
    if (!m_territoryReady)
        return true;
    if (x < 0 || y < 0 || x >= m_terrW || y >= m_terrH)
        return false;
    return m_ownGround[static_cast<std::size_t>(y) * m_terrW + x] != 0;
}

void AIController::PlanTerritory(const TileMap &map, const EntityManager &entities, Pathfinder &pathfinder)
{
    m_terrW = map.GetWidth();
    m_terrH = map.GetHeight();
    const std::size_t n = static_cast<std::size_t>(m_terrW) * m_terrH;
    m_road.assign(n, 0);
    m_ownGround.assign(n, 1);
    m_enemyRoads.clear();

    // The first headquarters of every player (entity order: the same on every computer)
    std::array<const Building *, kMaxPlayers> hq {};
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->state != ActionState::Die && b->type == UnitType::Headquarters && b->owner < kMaxPlayers
            && b->buildProgress >= 1.0f && !hq[b->owner])
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
                    float own = DetMath::Hypot(px - ox, py - oy), other = DetMath::Hypot(px - qx, py - qy);
                    for (const auto &e : entities.GetAllEntities())
                    {
                        const Building *b = e.AsBuilding();
                        if (!b || !b->alive || b->state == ActionState::Die || b->buildProgress < 1.0f
                            || b->type != UnitType::Headquarters || (b->owner != m_player && b->owner != q))
                            continue;
                        float d = DetMath::Hypot(
                            px - (b->tileX + b->buildWidth * 0.5f), py - (b->tileY + b->buildHeight * 0.5f));
                        if (b->owner == m_player)
                            own = std::min(own, d);
                        else
                            other = std::min(other, d);
                    }
                    if (other < own || (other == own && q < m_player))
                        m_ownGround[static_cast<std::size_t>(y) * m_terrW + x] = 0;
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
                        std::uint8_t &cell = m_road[static_cast<std::size_t>(by) * m_terrW + bx];
                        cell = std::max<std::uint8_t>(cell, (dx == 0 && dy == 0) ? 2 : 1);
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
    const int mw = map.GetWidth(), mh = map.GetHeight();
    // Compare connected components before/after placement, at infantry and vehicle clearance.
    // This also works when the whole map lies inside the HQ's construction radius.
    std::vector<unsigned char> blocked(mw * mh, 0);
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die)
            continue;
        for (int ty = std::max(0, b->tileY); ty < std::min(mh, b->tileY + b->buildHeight); ++ty)
            for (int tx = std::max(0, b->tileX); tx < std::min(mw, b->tileX + b->buildWidth); ++tx)
                blocked[ty * mw + tx] = 1;
    }
    // Tiles a passage must keep together: roads, the ground around every exit building of this player and around
    // every headquarters
    std::vector<unsigned char> important(mw * mh, 0);
    for (int ty = 0; ty < mh; ++ty)
        for (int tx = 0; tx < mw; ++tx)
            if (IsRoadCore(tx, ty))
                important[ty * mw + tx] = 1;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die)
            continue;
        const bool hq = b->type == UnitType::Headquarters;
        if (!hq && !(b->owner == m_player && IsExitBuilding(b->type)))
            continue;
        for (int ty = b->tileY - 2; ty <= b->tileY + b->buildHeight + 1; ++ty)
            for (int tx = b->tileX - 2; tx <= b->tileX + b->buildWidth + 1; ++tx)
                if (tx >= 0 && ty >= 0 && tx < mw && ty < mh)
                    important[ty * mw + tx] = 1;
    }
    for (int clearance : {1, 2})
    {
        auto free = [&](int bx, int by, bool after) {
            for (int dy = 0; dy < clearance; ++dy)
                for (int dx = 0; dx < clearance; ++dx)
                {
                    int tx = bx + dx, ty = by + dy;
                    if (tx < 0 || ty < 0 || tx >= mw || ty >= mh || blocked[ty * mw + tx] || !map.IsPassable(tx, ty)
                        || map.GetWall(tx, ty) != 0 || (after && tx >= x && tx < x + w && ty >= y && ty < y + h))
                        return false;
                }
            return true;
        };
        auto components = [&](bool after) {
            std::vector<int> labels(mw * mh, -1), queue;
            int label = 0;
            for (int ty = 0; ty < mh; ++ty)
                for (int tx = 0; tx < mw; ++tx)
                {
                    int start = ty * mw + tx;
                    if (labels[start] >= 0 || !free(tx, ty, after))
                        continue;
                    queue.clear();
                    queue.push_back(start);
                    labels[start] = label;
                    for (std::size_t i = 0; i < queue.size(); ++i)
                    {
                        int px = queue[i] % mw, py = queue[i] / mw;
                        for (const auto &[dx, dy] :
                             std::array<std::pair<int, int>, 4> {{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}})
                        {
                            int nx = px + dx, ny = py + dy;
                            if (nx < 0 || ny < 0 || nx >= mw || ny >= mh)
                                continue;
                            int next = ny * mw + nx;
                            if (labels[next] < 0 && free(nx, ny, after))
                            {
                                labels[next] = label;
                                queue.push_back(next);
                            }
                        }
                    }
                    ++label;
                }
            return labels;
        };
        const auto before = components(false), after = components(true);
        // A passage that falls apart is refused, unless the parts cut off are only small dead ends (a niche by a
        // cliff, a corner behind the building): on narrow maps nearly every spot closes one, and refusing them all
        // left the computer without a place to build. A part that holds a road, the door of a building or a
        // headquarters' surroundings is never cut off.
        int afterCount = 0;
        for (int i = 0; i < mw * mh; ++i)
            afterCount = std::max(afterCount, after[i] + 1);
        std::vector<int> partSize(afterCount, 0), partOwner(afterCount, -1);
        std::vector<char> partImportant(afterCount, 0);
        for (int i = 0; i < mw * mh; ++i)
        {
            if (after[i] < 0)
                continue;
            ++partSize[after[i]];
            partOwner[after[i]] = before[i];
            if (important[i])
                partImportant[after[i]] = 1;
        }
        std::vector<int> mainPart(mw * mh, -1); // per component before: its largest part after
        for (int a = 0; a < afterCount; ++a)
        {
            const int b = partOwner[a];
            if (b < 0)
                continue;
            int &m = mainPart[b];
            if (m < 0 || partSize[a] > partSize[m]
                || (partSize[a] == partSize[m] && partImportant[a] && !partImportant[m]))
                m = a;
        }
        constexpr int kDeadEndTiles = 32;
        for (int a = 0; a < afterCount; ++a)
        {
            const int b = partOwner[a];
            if (b < 0 || mainPart[b] == a)
                continue;
            // (a dead end is small, and small next to what stays connected: halving a small map is no dead end)
            if (partImportant[a] || partSize[a] > kDeadEndTiles || partSize[a] * 4 > partSize[mainPart[b]])
                return false; // a previously connected passage was split
        }
        auto touches = [&](const std::vector<int> &labels, int bx, int by, int bw, int bh) {
            for (int ty = by - clearance; ty <= by + bh; ++ty)
                for (int tx = bx - clearance; tx <= bx + bw; ++tx)
                    if (tx >= 0 && ty >= 0 && tx < mw && ty < mh && labels[ty * mw + tx] >= 0)
                        return true;
            return false;
        };
        bool hadWideExit = false;
        for (const auto &e : entities.GetAllEntities())
        {
            const Building *b = e.AsBuilding();
            if (!b || !b->alive || b->state == ActionState::Die || b->owner != m_player || !IsExitBuilding(b->type))
                continue;
            bool accessible = touches(before, b->tileX, b->tileY, b->buildWidth, b->buildHeight);
            hadWideExit |= accessible;
            if (accessible && !touches(after, b->tileX, b->tileY, b->buildWidth, b->buildHeight))
                return false;
        }
        if (needsExit && (clearance == 1 || hadWideExit) && !touches(after, x, y, w, h))
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
    bool needsExit,
    int centreX,
    int centreY,
    std::string *failure) const
{
    const int cx = centreX < 0 ? m_homeX : centreX, cy = centreY < 0 ? m_homeY : centreY;
    if (cx < 0 || cy < 0)
        return false;
    float tdx = static_cast<float>(m_playerHqX - cx), tdy = static_cast<float>(m_playerHqY - cy);
    const float tlen = std::max(1.0f, DetMath::Hypot(tdx, tdy));
    tdx /= tlen;
    tdy /= tlen;
    const float dlen = std::max(0.001f, DetMath::Hypot(dirX, dirY));
    dirX /= dlen;
    dirY /= dlen;
    // The base is built tight around the headquarters: no gaps between the buildings, the only ways through are the
    // roads (IsRoad, kept free below) and the ways out of the production buildings (KeepsWayOut). The style only
    // chooses the side and the order.
    const int maxRing = entities.GetBuildRadius(m_player) + 2;
    const int mapW = map.GetWidth(), mapH = map.GetHeight();
    std::vector<std::uint8_t> builtOn(static_cast<std::size_t>(mapW) * mapH, 0);
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die)
            continue;
        for (int ty = std::max(0, b->tileY); ty < std::min(mapH, b->tileY + b->buildHeight); ++ty)
            for (int tx = std::max(0, b->tileX); tx < std::min(mapW, b->tileX + b->buildWidth); ++tx)
                builtOn[static_cast<std::size_t>(ty) * mapW + tx] = 1;
    }
    // Share of the tiles around the footprint (sides only) that are a building, rock / water or the map's edge:
    // the more the new building leans on, the fewer holes are left in the base
    auto contact = [&](int px, int py) {
        int touching = 0, total = 0;
        auto at = [&](int tx, int ty) {
            ++total;
            if (tx < 0 || ty < 0 || tx >= mapW || ty >= mapH || !map.IsPassable(tx, ty)
                || builtOn[static_cast<std::size_t>(ty) * mapW + tx])
                ++touching;
        };
        for (int tx = px; tx < px + w; ++tx)
        {
            at(tx, py - 1);
            at(tx, py + h);
        }
        for (int ty = py; ty < py + h; ++ty)
        {
            at(px - 1, ty);
            at(px + w, ty);
        }
        return total > 0 ? static_cast<float>(touching) / static_cast<float>(total) : 0.0f;
    };
    constexpr float kContactWeight = 0.8f;

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
        float side; // toward the wanted direction only (-1..1)
    };
    for (int r = (crowding >= 3 ? 0 : std::max(1, minRing)); r <= maxRing; ++r)
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
                if (crowding < 3 && rearOnly && facing > 0.05f)
                    continue;
                const int px = cx + ox - w / 2, py = cy + oy - h / 2;
                if (crowding < 3 && nearGate(px, py))
                    continue;
                const char *reason = nullptr;
                if (!entities.CanPlaceBuilding(px, py, w, h, map, m_player, &reason))
                {
                    if (failure && reason && failure->find(reason) == std::string::npos)
                        *failure += std::string(reason) + "; ";
                    continue;
                }
                bool ok = true;
                for (int ty = py; ty < py + h && ok; ++ty)
                {
                    for (int tx = px; tx < px + w && ok; ++tx)
                        ok = (crowding >= 3 || (crowding >= 2 ? !IsRoadCore(tx, ty) : !IsRoad(tx, ty)))
                             && IsOwnGround(tx, ty) && !entities.IsTileOccupiedByUnit(tx, ty);
                }
                if (!ok || overlapsBuilding(px, py, w, h))
                {
                    if (failure && failure->find("layout") == std::string::npos)
                        *failure += "layout/territory; ";
                    continue;
                }
                const float side = (ox * dirX + oy * dirY) / ol;
                float score = side + kContactWeight * contact(px, py);
                if (m_style.layout == AIBaseStyle::Road)
                {
                    // Along the road: next to the band of a road is better
                    bool byRoad = false;
                    for (int ty = py - 2; ty < py + h + 2 && !byRoad; ++ty)
                        for (int tx = px - 2; tx < px + w + 2 && !byRoad; ++tx)
                            byRoad = IsRoad(tx, ty);
                    score += byRoad ? 0.35f : 0.0f;
                }
                ring.push_back({score, px, py, side});
            }
        }
        std::stable_sort(
            ring.begin(), ring.end(), [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
        // The worst side of a ring is still taken before the next ring (compact), but not a spot facing away
        // from where the building wants to be when the next ring has better ones: only the better half counts
        int tried = 0;
        for (const Candidate &c : ring)
        {
            if (crowding < 3 && c.side < -0.2f && m_style.layout != AIBaseStyle::Compact)
                continue; // (sorted by the score with the contact bonus: the spots after it may face the right way)
            if (++tried > 24 && crowding < 3)
                break; // the way-out check floods the map: a ring gets a limited number of tries
            if (KeepsWayOut(c.x, c.y, w, h, map, entities, needsExit))
            {
                outX = c.x;
                outY = c.y;
                return true;
            }
            if (failure && failure->find("exit") == std::string::npos)
                *failure += "exit connectivity; ";
        }
    }
    return false;
}

bool AIController::PlanPassageWalls(
    int cx,
    int cy,
    int hqRadius,
    const TileMap &map,
    const EntityManager &entities,
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
        // (three tiles in: nearer, the corridor kept free around the gate would push one of them aside)
        // Two bunkers right behind the wall, one on each side of the road: they take the fire. The towers stand
        // behind them, deeper in the base (they reach farther): two, or one for a style of many bunkers
        const int side = gateHi + 2;
        const auto [bx, by] = road[std::max(0, bestI - 3)];
        m_defensePlan.push_back({AIDefenseSite::Bunker, bx + bestPx * side - 1, by + bestPy * side - 1, false});
        m_defensePlan.push_back(
            {AIDefenseSite::Bunker, bx - bestPx * (side + 1) - 1, by - bestPy * (side + 1) - 1, false});
        const auto [tx, ty] = road[std::max(0, bestI - 6)];
        m_defensePlan.push_back({AIDefenseSite::Tower, tx + bestPx * side - 1, ty + bestPy * side - 1, false});
        if (!m_style.bunkerHeavy)
            m_defensePlan.push_back(
                {AIDefenseSite::Tower, tx - bestPx * (side + 1) - 1, ty - bestPy * (side + 1) - 1, false});
    }
    return any;
}

} // namespace OpenArtOfWar
