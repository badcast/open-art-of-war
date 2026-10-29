#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#endif
#include "brain_map.hpp"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "game/Pathfinder.hpp"
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace OpenArtOfWar
{

    Pathfinder::Pathfinder(const TileMap *map) : m_map(map)
    {
        if(m_map)
        {
            syncMap();
        }
    }

    Pathfinder::~Pathfinder() = default;

    void Pathfinder::setMap(const TileMap *map)
    {
        m_map = map;
        syncMap();
    }

    void Pathfinder::ensureBrainMapSize(int width, int height)
    {
        if(width <= 0 || height <= 0)
            return;
        if(!m_brainMap || m_lastWidth != width || m_lastHeight != height)
        {
            m_brainMap = std::make_unique<brain::brain_map>(width, height);
            m_brainMap->set_identity(brain::MatrixIdentity::DiagonalMethod);
            m_brainMap->set_heuristic(brain::HeuristicMethod::Octile);
            m_lastWidth = width;
            m_lastHeight = height;
        }
    }

    void Pathfinder::syncMap(int ignoreEntityId, bool lockUnits)
    {
        if(!m_map)
            return;
        int w = m_map->getWidth();
        int h = m_map->getHeight();
        if(w <= 0 || h <= 0)
            return;

        ensureBrainMapSize(w, h);

        // Sync impassable obstacles (terrain, water, buildings, walls) to brain_map locks
        for(int y = 0; y < h; ++y)
        {
            for(int x = 0; x < w; ++x)
            {
                bool blocked = !m_map->isPassable(x, y) || (m_map->getWall(x, y) > 0);
                m_brainMap->set_lock(brain::the_site {x, y}, blocked);
            }
        }

        // Stationary units block routes; moving allies are handled by movement-time tile reservation.
        // - Parked allies lock their tile AND the tile they reserved; enemies are ignored (see below).
        // - Moving allies are not locked: they vacate their tiles before we arrive. Locking them boxes
        //   a tight group in on itself at order time (inner units get no path and trail the outer ones).
        if(m_entities && lockUnits)
        {
            Faction moverFaction = Faction::Neutral;
            if(ignoreEntityId != 0)
            {
                if(const Unit *mu = m_entities->getUnit(ignoreEntityId))
                {
                    moverFaction = mu->faction;
                }
            }

            auto lockTile = [&](int x, int y)
            {
                if(x >= 0 && x < w && y >= 0 && y < h)
                {
                    m_brainMap->set_lock(brain::the_site {x, y}, true);
                }
            };

            for(const auto &e : m_entities->getAllEntities())
            {
                if(!e.alive || e.state == ActionState::Die || e.isBuilding)
                    continue;
                if(e.id == ignoreEntityId)
                    continue;

                const Unit *u = e.asUnit();
                if(u && u->faction == moverFaction && u->state == ActionState::Move)
                {
                    continue;
                }
                // Enemy units never make a route detour: a line of enemies across a passage is not a wall,
                // the unit walks up to it and fights its way through (Breach & Clear in Unit::updateMovement)
                if(u && moverFaction != Faction::Neutral && u->faction != moverFaction)
                {
                    continue;
                }

                lockTile(e.tileX, e.tileY);
                if(e.hasReservation())
                    lockTile(e.resTileX, e.resTileY);
            }
        }

        // One-shot extra obstacles requested by the caller (e.g. an ally we decided to walk around)
        for(const auto &t : m_avoidTiles)
        {
            if(t.x >= 0 && t.x < w && t.y >= 0 && t.y < h)
            {
                m_brainMap->set_lock(brain::the_site {t.x, t.y}, true);
            }
        }
    }

    bool Pathfinder::findPath(int startX, int startY, int targetX, int targetY, std::vector<PathNode> &outPath, int ignoreEntityId, bool *outIsPartial, bool allowDiagonal)
    {
        // Take ownership of the one-shot avoid list so early returns never leak it into the next query
        std::vector<PathNode> avoidTiles;
        avoidTiles.swap(m_avoidTiles);
        const bool ignoreUnits = m_ignoreUnitsOnce;
        m_ignoreUnitsOnce = false;

        outPath.clear();
        if(outIsPartial)
            *outIsPartial = false;
        if(!m_map)
            return false;
        if(startX == targetX && startY == targetY)
            return true;

        int mapW = m_map->getWidth();
        int mapH = m_map->getHeight();

        if(startX < 0 || startX >= mapW || startY < 0 || startY >= mapH)
            return false;
        // Guard against invalid negative target coordinates (Bug #1 protection)
        if(targetX < 0 || targetY < 0)
            return false;

        targetX = std::clamp(targetX, 0, mapW - 1);
        targetY = std::clamp(targetY, 0, mapH - 1);
        if(startX == targetX && startY == targetY)
            return true;

        int origTargetX = targetX;
        int origTargetY = targetY;

        ensureBrainMapSize(mapW, mapH);
        m_brainMap->set_identity(allowDiagonal ? brain::MatrixIdentity::DiagonalMethod : brain::MatrixIdentity::PlusMethod);
        m_brainMap->set_heuristic(allowDiagonal ? brain::HeuristicMethod::Octile : brain::HeuristicMethod::Manhattan);
        m_avoidTiles.swap(avoidTiles);
        syncMap(ignoreEntityId, !ignoreUnits);
        m_avoidTiles.clear(); // avoid list is valid for a single query only

        // Ensure starting tile is unlocked for A* exploration
        m_brainMap->set_lock(brain::the_site {startX, startY}, false);

        // If destination tile itself is locked (e.g. enemy building, wall, unit, or impassable terrain):
        // 1. First check if (targetX, targetY) belongs to a multi-tile Building footprint [bx..bx+bw-1, by..by+bh-1].
        //    If so, pick the closest unlocked tile along the building's entire perimeter facing startX, startY.
        bool goalAdjustedBecauseLocked = false;
        if(m_brainMap->has_lock(brain::the_site {targetX, targetY}))
        {
            int bestDistSq = 999999;
            int altX = -1, altY = -1;

            if(m_entities)
            {
                for(const auto &e : m_entities->getAllEntities())
                {
                    if(!e.alive || e.state == ActionState::Die || !e.isBuilding)
                        continue;
                    const Building *b = e.asBuilding();
                    if(!b)
                        continue;
                    if(targetX >= b->tileX && targetX < b->tileX + b->buildWidth && targetY >= b->tileY && targetY < b->tileY + b->buildHeight)
                    {
                        for(int py = b->tileY - 1; py <= b->tileY + b->buildHeight; ++py)
                        {
                            for(int px = b->tileX - 1; px <= b->tileX + b->buildWidth; ++px)
                            {
                                if(px >= b->tileX && px < b->tileX + b->buildWidth && py >= b->tileY && py < b->tileY + b->buildHeight)
                                {
                                    continue;
                                }
                                if(px >= 0 && px < mapW && py >= 0 && py < mapH && !m_brainMap->has_lock(brain::the_site {px, py}))
                                {
                                    int dsq = (px - startX) * (px - startX) + (py - startY) * (py - startY);
                                    if(dsq < bestDistSq)
                                    {
                                        bestDistSq = dsq;
                                        altX = px;
                                        altY = py;
                                    }
                                }
                            }
                        }
                        break;
                    }
                }
            }

            // 2. Concentric ring search if not a building (or building perimeter was full)
            if(altX == -1)
            {
                for(int r = 1; r <= 5; ++r)
                {
                    for(int dy = -r; dy <= r; ++dy)
                    {
                        for(int dx = -r; dx <= r; ++dx)
                        {
                            if(std::abs(dx) != r && std::abs(dy) != r)
                                continue;
                            int nx = targetX + dx;
                            int ny = targetY + dy;
                            if(nx >= 0 && nx < mapW && ny >= 0 && ny < mapH)
                            {
                                if(!m_brainMap->has_lock(brain::the_site {nx, ny}))
                                {
                                    int dsq = (nx - startX) * (nx - startX) + (ny - startY) * (ny - startY);
                                    if(dsq < bestDistSq)
                                    {
                                        bestDistSq = dsq;
                                        altX = nx;
                                        altY = ny;
                                    }
                                }
                            }
                        }
                    }
                    if(altX != -1)
                        break;
                }
            }

            if(altX != -1)
            {
                targetX = altX;
                targetY = altY;
                goalAdjustedBecauseLocked = true;
            }
        }

        // Helper lambda: Sanitizes any path to guarantee strict 4-way orthogonal connectivity
        // without EVER stepping onto a locked or impassable corner tile.
        auto enforceValidOrthogonalPath = [&](std::vector<PathNode> &pathVec) -> bool
        {
            if(allowDiagonal || pathVec.empty())
                return !pathVec.empty();

            std::vector<PathNode> orthoPath;
            orthoPath.reserve(pathVec.size() * 2);

            int prevX = startX;
            int prevY = startY;

            for(const auto &node : pathVec)
            {
                int dx = node.x - prevX;
                int dy = node.y - prevY;

                if(dx != 0 && dy != 0)
                {
                    // Test both cardinal intermediate corners for passability and lock status
                    int c1x = node.x, c1y = prevY;
                    int c2x = prevX, c2y = node.y;

                    bool c1Valid = (c1x >= 0 && c1x < mapW && c1y >= 0 && c1y < mapH) && m_map->isPassable(c1x, c1y) && m_map->getWall(c1x, c1y) == 0 && !m_brainMap->has_lock(brain::the_site {c1x, c1y});

                    bool c2Valid = (c2x >= 0 && c2x < mapW && c2y >= 0 && c2y < mapH) && m_map->isPassable(c2x, c2y) && m_map->getWall(c2x, c2y) == 0 && !m_brainMap->has_lock(brain::the_site {c2x, c2y});

                    if(c1Valid)
                    {
                        orthoPath.push_back({c1x, c1y});
                    }
                    else if(c2Valid)
                    {
                        orthoPath.push_back({c2x, c2y});
                    }
                    else
                    {
                        // Neither corner is passable: truncate path before the impassable diagonal pinch!
                        if(outIsPartial)
                            *outIsPartial = true;
                        break;
                    }
                }
                orthoPath.push_back(node);
                prevX = node.x;
                prevY = node.y;
            }

            pathVec = std::move(orthoPath);
            return !pathVec.empty();
        };

        // A* returns one of many equally short paths, often a staircase (right, down, right, down...). Vehicles
        // turn in place before every step, so a staircase means stop-turn-go on every tile (worst for the
        // Fortress, which only turns by 90 degrees). Reorder the steps into long straight runs: same tiles
        // count, same length, far fewer turns. A swap is only made when the new corner tile is free.
        auto freeTile = [&](int x, int y) { return x >= 0 && x < mapW && y >= 0 && y < mapH && m_map->isPassable(x, y) && m_map->getWall(x, y) == 0 && !m_brainMap->has_lock(brain::the_site {x, y}); };
        auto stepOk = [&](int fromX, int fromY, int dx, int dy)
        {
            if(!freeTile(fromX + dx, fromY + dy))
                return false;
            if(dx != 0 && dy != 0)
                return freeTile(fromX + dx, fromY) && freeTile(fromX, fromY + dy); // no corner cutting
            return true;
        };
        auto straighten = [&](std::vector<PathNode> &pathVec) -> bool
        {
            const size_t n = pathVec.size();
            if(n < 3)
                return !pathVec.empty();
            std::vector<PathNode> steps(n);
            PathNode prev {startX, startY};
            for(size_t i = 0; i < n; ++i)
            {
                steps[i] = {pathVec[i].x - prev.x, pathVec[i].y - prev.y};
                prev = pathVec[i];
            }
            for(int pass = 0; pass < 8; ++pass)
            {
                bool changed = false;
                PathNode at {startX, startY};
                at.x += steps[0].x;
                at.y += steps[0].y;
                for(size_t k = 1; k + 1 < n; ++k)
                {
                    // ... A, B, A ... -> ... A, A, B ...: the second A joins the run before it
                    const PathNode a = steps[k - 1];
                    const PathNode b = steps[k];
                    const PathNode c = steps[k + 1];
                    if(!(b == c) && c == a && stepOk(at.x, at.y, c.x, c.y) && stepOk(at.x + c.x, at.y + c.y, b.x, b.y))
                    {
                        steps[k] = c;
                        steps[k + 1] = b;
                        changed = true;
                    }
                    at.x += steps[k].x;
                    at.y += steps[k].y;
                }
                if(!changed)
                    break;
            }
            PathNode cur {startX, startY};
            for(size_t i = 0; i < n; ++i)
            {
                cur.x += steps[i].x;
                cur.y += steps[i].y;
                pathVec[i] = cur;
            }
            return true;
        };

        // Clear previous search node data (g, h, f, flags, parent) while preserving obstacle locks
        m_brainMap->clear(false);

        brain::brain_map::result_site navResult;
        brain::the_site startSite {startX, startY};
        brain::the_site goalSite {targetX, targetY};

        bool found = false;
        if(!m_brainMap->has_lock(goalSite))
        {
            found = m_brainMap->find(navResult, startSite, goalSite);
        }

        if(found && !navResult.connections.empty())
        {
            outPath.reserve(navResult.connections.size());
            for(size_t i = 1; i < navResult.connections.size(); ++i)
            {
                outPath.push_back({navResult.connections[i].x, navResult.connections[i].y});
            }
            return enforceValidOrthogonalPath(outPath) && straighten(outPath);
        }

        // If no complete path to goal exists (passage is closed by walls, buildings, or units),
        // brain_map::find has explored the entire reachable component from startSite (or if goalSite was locked,
        // run find toward an unreachable corner to flood-fill all reachable tiles from startSite).
        const brain::the_neuron *startNeuron = m_brainMap->get(startSite);
        if(!startNeuron || startNeuron->flags == 0)
        {
            // Goal was locked so m_brainMap->find returned early; unlock a dummy goal or flood-fill via an unlocked tile
            // Or temporarily unlock origTarget just to run A* until blocked by the barrier around it!
            m_brainMap->clear(false);
            bool origLocked = m_brainMap->has_lock(brain::the_site {origTargetX, origTargetY});
            if(origLocked)
            {
                m_brainMap->set_lock(brain::the_site {origTargetX, origTargetY}, false);
            }
            m_brainMap->find(navResult, startSite, brain::the_site {origTargetX, origTargetY});
            if(origLocked)
            {
                m_brainMap->set_lock(brain::the_site {origTargetX, origTargetY}, true);
            }
        }

        // Find the visited reachable neuron in the closed list that is closest to (origTargetX, origTargetY).
        // Prefer the path with the shortest travel cost (g) among tiles near the blockage,
        // so a unit approaches the closed passage directly rather than walking around the outside of a long wall!
        const brain::the_neuron *bestNeuron = nullptr;
        int startDistSq = (startX - origTargetX) * (startX - origTargetX) + (startY - origTargetY) * (startY - origTargetY);
        // Note: g is scaled by 10 in brain_impl.hpp, so dsq is weighted by 100
        int bestScore = startDistSq * 100;

        for(int y = 0; y < mapH; ++y)
        {
            for(int x = 0; x < mapW; ++x)
            {
                const brain::the_neuron *n = m_brainMap->get(x, y);
                // NEURON_CAPTURE_CLOSED_LIST == 2
                if(!n || (n->flags & 2) == 0)
                    continue;
                if(x == startX && y == startY)
                    continue;
                if(m_brainMap->has_lock(brain::the_site {x, y}))
                    continue;

                int dsq = (x - origTargetX) * (x - origTargetX) + (y - origTargetY) * (y - origTargetY);
                if(dsq >= startDistSq)
                    continue;

                int score = dsq * 100 + n->g * 2;
                if(score < bestScore)
                {
                    bestScore = score;
                    bestNeuron = n;
                }
            }
        }

        if(outIsPartial)
        {
            *outIsPartial = true;
        }

        if(!bestNeuron)
        {
            // Already at the closest reachable tile to the blocked passage!
            return false;
        }

        // Reconstruct partial path up to bestNeuron
        std::vector<PathNode> revPath;
        const brain::the_neuron *cur = bestNeuron;
        while(cur && cur->parent != nullptr)
        {
            brain::the_site pt = m_brainMap->get_point(cur);
            revPath.push_back({pt.x, pt.y});
            cur = cur->parent;
        }
        std::reverse(revPath.begin(), revPath.end());
        outPath = std::move(revPath);

        return enforceValidOrthogonalPath(outPath) && straighten(outPath);
    }

    /*
     * =========================================================================
     * AINavigataionStormcastView: Authentic Group Formation Spiral Algorithm
     * =========================================================================
     *
     *  + - - - - - - - - - +   --   It's storm cast view
     *  ' → → → → → → → → ↓ '   --   It's storm cast view
     *  ' ↑ → → → → → → ↓ ↓ '   --   It's storm cast view
     *  ' ↑ ↑ → → → → ↓ ↓ ↓ '   --   It's storm cast view
     *  ' ↑ ↑ ↑ → → ↓ ↓ ↓ ↓ '   --   It's storm cast view
     *  ' ↑ ↑ ↑ ↑ ← ↓ ↓ ↓ ↓ '   --   It's storm cast view
     *  ' ↑ ↑ ↑ ← ← ← ↓ ↓ ↓ '   --   It's storm cast view
     *  ' ↑ ↑ ← ← ← ← ← ↓ ↓ '   --   It's storm cast view
     *  ' ↑ ← ← ← ← ← ← ← ↓ '   --   It's storm cast view
     *  ' ← ← ← ← ← ← ← ← ← '   --   It's storm cast view
     *  + - - - - - - - - - +   --   It's storm cast view
     *
     * How it works:
     * Starting from the target destination (center point), this algorithm expands
     * outwards in square concentric spiral rings (←, ↑, →, ↓) using the 4
     * storm_vec vectors. Each step checks candidate cells for passability,
     * occupancy by other units, and valid reachable connectivity.
     *
     * Caveat prevention (Bridge / Chasm Detour Protection):
     * "ее основной недостаток это если есть разрыв между путями он может выдать
     *  юниту идти через мост (если такой есть, а не стоять рядом ближе) учитывай это"
     * To prevent selecting slots across rivers/canyons that require large bridge
     * detours, we verify local direct reachability within a bounded Manhattan distance.
     */
    std::vector<StormcastSlot> AINavigataionStormcastView::computeGroupSlots(int targetTileX, int targetTileY, int count, const TileMap &map, const EntityManager *entities, const std::vector<int> &movingUnitIds)
    {
        std::vector<StormcastSlot> result;
        if(count <= 0)
            return result;
        result.reserve(count);

        int mapW = map.getWidth();
        int mapH = map.getHeight();
        if(mapW <= 0 || mapH <= 0)
            return result;

        targetTileX = std::clamp(targetTileX, 0, mapW - 1);
        targetTileY = std::clamp(targetTileY, 0, mapH - 1);

        // Compute approaching squad centroid so clicks on buildings or cliffs
        // snap to the front face of the obstacle facing the approaching units!
        float avgX = static_cast<float>(targetTileX);
        float avgY = static_cast<float>(targetTileY);
        int validMovers = 0;
        if(entities && !movingUnitIds.empty())
        {
            float sumX = 0.0f, sumY = 0.0f;
            for(int uid : movingUnitIds)
            {
                if(const Entity *e = entities->getEntity(uid))
                {
                    sumX += static_cast<float>(e->tileX);
                    sumY += static_cast<float>(e->tileY);
                    validMovers++;
                }
            }
            if(validMovers > 0)
            {
                avgX = sumX / static_cast<float>(validMovers);
                avgY = sumY / static_cast<float>(validMovers);
            }
        }

        std::vector<std::pair<int, int>> assigned;

        auto isTileBlockedByBuilding = [&](int x, int y) -> bool
        {
            if(!entities)
                return false;
            for(const auto &other : entities->getAllEntities())
            {
                if(!other.alive || other.state == ActionState::Die || !other.isBuilding)
                    continue;
                if(const Building *b = other.asBuilding())
                {
                    if(x >= b->tileX && x < b->tileX + b->buildWidth && y >= b->tileY && y < b->tileY + b->buildHeight)
                    {
                        return true;
                    }
                }
            }
            return false;
        };

        auto isSlotValid = [&](int x, int y) -> bool
        {
            if(x < 0 || x >= mapW || y < 0 || y >= mapH)
                return false;
            if(!map.isPassable(x, y))
                return false;
            if(map.getWall(x, y) > 0)
                return false;
            if(isTileBlockedByBuilding(x, y))
                return false;

            for(const auto &a : assigned)
            {
                if(a.first == x && a.second == y)
                    return false;
            }

            // Check if occupied by a stationary unit not in movingUnitIds
            if(entities)
            {
                for(const auto &other : entities->getAllEntities())
                {
                    if(!other.alive || other.state == ActionState::Die || other.isBuilding)
                        continue;
                    if(other.tileX == x && other.tileY == y)
                    {
                        bool inGroup = false;
                        for(int uid : movingUnitIds)
                        {
                            if(uid == other.id)
                            {
                                inGroup = true;
                                break;
                            }
                        }
                        if(!inGroup)
                            return false; // Occupied by stationary entity
                    }
                }
            }
            return true;
        };

        // If (targetTileX, targetTileY) is inside a building, wall, or impassable terrain,
        // snap the spiral center to the nearest valid tile biased toward the approaching squad!
        if(!isSlotValid(targetTileX, targetTileY))
        {
            float bestCost = 1e18f;
            int bestX = targetTileX;
            int bestY = targetTileY;
            bool foundCenter = false;

            for(int r = 1; r <= 6; ++r)
            {
                for(int dy = -r; dy <= r; ++dy)
                {
                    for(int dx = -r; dx <= r; ++dx)
                    {
                        int nx = targetTileX + dx;
                        int ny = targetTileY + dy;
                        if(isSlotValid(nx, ny))
                        {
                            float distTargetSq = static_cast<float>(dx * dx + dy * dy);
                            float distMoverSq = (nx - avgX) * (nx - avgX) + (ny - avgY) * (ny - avgY);
                            float cost = distTargetSq * 16.0f + distMoverSq;
                            if(cost < bestCost)
                            {
                                bestCost = cost;
                                bestX = nx;
                                bestY = ny;
                                foundCenter = true;
                            }
                        }
                    }
                }
                if(foundCenter)
                    break;
            }
            targetTileX = bestX;
            targetTileY = bestY;
        }

        // 1. Check center target tile first
        if(isSlotValid(targetTileX, targetTileY))
        {
            result.push_back({targetTileX, targetTileY});
            assigned.push_back({targetTileX, targetTileY});
            if(static_cast<int>(result.size()) >= count)
            {
                return result;
            }
        }

        struct ISite
        {
            int x;
            int y;
        };
        constexpr int nvec = 4;
        static const ISite storm_vec[nvec] {
            {-1, 0}, // ←
            {0, 1},  // ↑
            {1, 0},  // →
            {0, -1}  // ↓
        };

        int max_span = std::max(mapW, mapH) + 2;
        ISite cur {targetTileX, targetTileY};
        int section = 0;

        // Edge
        for(section = 0; section <= max_span && static_cast<int>(result.size()) < count;)
        {
            // Line
            for(int iv = 0; iv < nvec && static_cast<int>(result.size()) < count; ++iv)
            {
                // Section
                if(!(iv & 1))
                {
                    ++section;
                }

                int s = 0;
                do
                {
                    cur.x += storm_vec[iv].x;
                    cur.y += storm_vec[iv].y;

                    if(isSlotValid(cur.x, cur.y))
                    {
                        // Check local Manhattan distance to prevent large path detours across rivers/bridges
                        int mDist = std::abs(cur.x - targetTileX) + std::abs(cur.y - targetTileY);
                        if(mDist <= section + 2)
                        {
                            result.push_back({cur.x, cur.y});
                            assigned.push_back({cur.x, cur.y});
                            if(static_cast<int>(result.size()) >= count)
                                break;
                        }
                    }
                } while(++s < section);
            }
        }

        // Safe Fallback: Never return (0,0) unless (targetTileX, targetTileY) is legitimately (0,0).
        // Reuse the closest found valid slot, or clamped (targetTileX, targetTileY).
        StormcastSlot safeFallback = !result.empty() ? result.front() : StormcastSlot {targetTileX, targetTileY};
        while(static_cast<int>(result.size()) < count)
        {
            result.push_back(safeFallback);
        }

        return result;
    }

} // namespace OpenArtOfWar
