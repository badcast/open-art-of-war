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

namespace OpenArtOfWar
{

    namespace
    {

        constexpr float kTickSeconds = 0.25f;

        // Unit position in fractional tile coordinates (tracks sub-tile movement)
        float unitTileX(const Unit &u)
        {
            return u.getWorldPixelX() / TILE_WIDTH;
        }
        float unitTileY(const Unit &u)
        {
            return u.getWorldPixelY() / TILE_HEIGHT;
        }

        float distTo(const Unit &u, float x, float y)
        {
            return DetMath::hypot(unitTileX(u) - x, unitTileY(u) - y);
        }

        bool isEngaged(const Unit &u)
        {
            return u.targetEntityId != 0 || u.targetWallTileX >= 0 || u.state == ActionState::Attack;
        }

        bool isIdleWithoutOrders(const Unit &u)
        {
            return u.state == ActionState::Idle && u.path.empty() && !u.waitingForPath && u.targetEntityId == 0;
        }

        // Both factions: Fortress / Hammer / Zeus and Coyote / Armadillo / Rhino
        bool isTank(int arch)
        {
            return arch == 6 || arch == 7 || arch == 8 || arch == 9 || arch == 10 || arch == 11 || arch == 18;
        }
        bool isMelee(int arch)
        {
            return arch == 5;
        } // flamethrower
        // Rocket infantry and MLRS of both factions
        bool isRangedSupport(int arch)
        {
            return arch == 2 || arch == 3 || arch == 12 || arch == 13 || arch == 16;
        }

        // Cohesion radius for a group of n units. Big groups get a proportionally larger radius (a column of
        // 15 is naturally longer than 3 tiles), and narrow passages stretch any group into a line.
        float effectiveLeash(const AITacticsConfig &t, size_t n, bool inCorridor)
        {
            const float count = static_cast<float>(n);
            float leash = t.leashTiles + t.leashPerUnit * std::max(0.0f, count - 4.0f);
            leash = std::max(leash, std::sqrt(count) * 0.8f + 1.0f); // never tighter than the formation itself
            if(inCorridor)
                leash *= std::max(1.0f, t.corridorLeashFactor);
            return leash;
        }

    } // namespace

    const char *squadStateName(AISquadState state)
    {
        switch(state)
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
            case AISquadState::Assault:
                return "ASSAULT";
            case AISquadState::Retreating:
                return "RETREAT";
        }
        return "?";
    }

    // ---------------------------------------------------------------------------
    // Chokepoint detection
    // ---------------------------------------------------------------------------

    void ChokepointMap::rebuild(const TileMap &map)
    {
        m_width = map.getWidth();
        m_height = map.getHeight();
        m_choke.assign(static_cast<size_t>(m_width) * m_height, 0);
        m_openWidth.assign(static_cast<size_t>(m_width) * m_height, 0);
        if(m_width <= 0 || m_height <= 0)
            return;

        auto open = [&](int x, int y) { return map.isPassable(x, y) && map.getWall(x, y) == 0; };

        // Length of the contiguous open run through every tile, horizontally and vertically
        std::vector<int> runH(m_choke.size(), 0);
        std::vector<int> runV(m_choke.size(), 0);
        for(int y = 0; y < m_height; ++y)
        {
            int x = 0;
            while(x < m_width)
            {
                if(!open(x, y))
                {
                    ++x;
                    continue;
                }
                int start = x;
                while(x < m_width && open(x, y))
                    ++x;
                for(int i = start; i < x; ++i)
                    runH[y * m_width + i] = x - start;
            }
        }
        for(int x = 0; x < m_width; ++x)
        {
            int y = 0;
            while(y < m_height)
            {
                if(!open(x, y))
                {
                    ++y;
                    continue;
                }
                int start = y;
                while(y < m_height && open(x, y))
                    ++y;
                for(int i = start; i < y; ++i)
                    runV[i * m_width + x] = y - start;
            }
        }

        // A corridor tile is at most 2 tiles wide across and at least 3 tiles long along the passage
        for(size_t i = 0; i < m_choke.size(); ++i)
        {
            int narrow = std::min(runH[i], runV[i]);
            int along = std::max(runH[i], runV[i]);
            m_openWidth[i] = static_cast<uint8_t>(std::min(narrow, 255));
            m_choke[i] = (narrow > 0 && narrow <= 2 && along >= 3) ? 1 : 0;
        }
    }

    int ChokepointMap::openWidth(int x, int y) const
    {
        if(x < 0 || y < 0 || x >= m_width || y >= m_height)
            return 0;
        return m_openWidth[static_cast<size_t>(y) * m_width + x];
    }

    bool ChokepointMap::isChoke(int x, int y) const
    {
        if(x < 0 || y < 0 || x >= m_width || y >= m_height)
            return false;
        return m_choke[static_cast<size_t>(y) * m_width + x] != 0;
    }

    // ---------------------------------------------------------------------------
    // Squad lifecycle
    // ---------------------------------------------------------------------------

    void AISquadController::clear()
    {
        m_squads.clear();
        m_released.clear();
        m_nextSquadId = 1;
        m_tickAccumulator = 0.0f;
    }

    bool AISquadController::isSquadMember(int unitId) const
    {
        for(const auto &sq : m_squads)
        {
            if(std::find(sq.unitIds.begin(), sq.unitIds.end(), unitId) != sq.unitIds.end())
                return true;
        }
        return false;
    }

    std::vector<int> AISquadController::takeReleasedUnits()
    {
        std::vector<int> out;
        out.swap(m_released);
        return out;
    }

    int AISquadController::createSquad(const std::vector<int> &unitIds, int targetX, int targetY, int waveNumber, EntityManager &entities, Pathfinder &pathfinder, const AITacticsConfig &tactics, int viaX, int viaY)
    {
        if(unitIds.empty())
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
        sq.leaderId = pickLeader(sq, entities, tactics);
        for(int id : unitIds)
        {
            if(const Entity *e = entities.getEntity(id))
            {
                sq.owner = e->owner;
                break;
            }
        }

        if(tactics.retreatLossRatio > 0.0f)
        {
            for(const auto &e : entities.getAllEntities())
            {
                if(e.alive && e.state != ActionState::Die && e.owner < kMaxPlayers && Players::hostile(e.owner, sq.owner))
                {
                    sq.playerHpAtLaunch[e.id] = e.currentHp;
                }
            }
        }

        rebuildRoute(sq, entities, pathfinder);
        sq.state = AISquadState::Marching;
        sq.orderTimer = 1e6f; // the first leg is ordered on the next tick, which has the map at hand
        m_squads.push_back(std::move(sq));
        return m_squads.back().id;
    }

    void AISquadController::update(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder, const ChokepointMap &chokes, const AITacticsConfig &tactics)
    {
        m_tickAccumulator += deltaTime;
        if(m_tickAccumulator < kTickSeconds)
            return;
        float dt = m_tickAccumulator;
        m_tickAccumulator = 0.0f;

        for(auto &sq : m_squads)
        {
            tick(sq, dt, map, entities, pathfinder, chokes, tactics);
        }
        m_squads.erase(std::remove_if(m_squads.begin(), m_squads.end(), [](const AISquad &s) { return s.unitIds.empty(); }), m_squads.end());
    }

    AISquad *AISquadController::findSquad(int squadId)
    {
        for(auto &sq : m_squads)
        {
            if(sq.id == squadId)
                return &sq;
        }
        return nullptr;
    }

    void AISquadController::releaseUnits(const std::vector<int> &unitIds, EntityManager &entities)
    {
        for(auto &sq : m_squads)
        {
            for(int id : unitIds)
            {
                if(std::find(sq.unitIds.begin(), sq.unitIds.end(), id) != sq.unitIds.end())
                    releaseUnit(sq, id, entities);
            }
            if(!sq.unitIds.empty() && std::find(sq.unitIds.begin(), sq.unitIds.end(), sq.leaderId) == sq.unitIds.end())
            {
                sq.leaderId = sq.unitIds.front();
            }
        }
        m_squads.erase(std::remove_if(m_squads.begin(), m_squads.end(), [](const AISquad &s) { return s.unitIds.empty(); }), m_squads.end());
    }

    void AISquadController::orderLeg(const AISquad &sq, const std::vector<int> &ids, int x, int y, EntityManager &entities, Pathfinder &pathfinder)
    {
        if(sq.attackMove)
        {
            entities.issueAttackMoveOrder(ids, x, y, pathfinder);
        }
        else
        {
            entities.issueMoveOrder(ids, x, y, pathfinder, false);
        }
        capDetours(sq, entities);
    }

    void AISquadController::capDetours(const AISquad &sq, EntityManager &entities)
    {
        const Unit *leader = entities.getUnit(sq.leaderId);
        if(!leader)
            return;
        const float leash = std::max(3.0f, sq.leashRadius);
        std::vector<int> hold;
        for(int id : sq.unitIds)
        {
            if(id == sq.leaderId)
                continue;
            const Unit *u = entities.getUnit(id);
            if(!u || u->path.size() <= u->pathIndex)
                continue;
            const PathNode &goal = u->path.back();
            const float straight = DetMath::hypot(static_cast<float>(goal.x - u->tileX), static_cast<float>(goal.y - u->tileY));
            const float pathLen = static_cast<float>(u->path.size() - u->pathIndex);
            // Allowed: the straight way, some slack for small obstacles, plus the group's leash
            const float limit = std::max(6.0f, straight * 1.8f + leash * 1.5f);
            // Only units already with the group: a straggler far away must still be allowed to come back
            const float toLeader = distTo(*u, static_cast<float>(leader->tileX), static_cast<float>(leader->tileY));
            if(pathLen > limit && toLeader <= leash * 2.0f)
                hold.push_back(id);
        }
        if(!hold.empty())
            holdUnits(hold, entities);
    }

    void AISquadController::setState(AISquad &sq, AISquadState state)
    {
        sq.state = state;
        sq.stateTimer = 0.0f;
        sq.quietTimer = 0.0f;
    }

    void AISquadController::releaseUnit(AISquad &sq, int unitId, EntityManager &entities)
    {
        sq.unitIds.erase(std::remove(sq.unitIds.begin(), sq.unitIds.end(), unitId), sq.unitIds.end());
        sq.stragglerBest.erase(unitId);
        sq.stragglerTimer.erase(unitId);
        if(Unit *u = entities.getUnit(unitId))
        {
            u->flowMode = false;
            u->paceScale = 1.0f;
            m_released.push_back(unitId);
        }
    }

    int AISquadController::pickLeader(const AISquad &sq, EntityManager &entities, const AITacticsConfig &tactics) const
    {
        int best = 0;
        float bestScore = 1e9f;
        for(int id : sq.unitIds)
        {
            const Unit *u = entities.getUnit(id);
            if(!u || !u->alive || u->state == ActionState::Die)
                continue;
            float score;
            if(tactics.syncToSlowest)
            {
                // Slowest unit sets the pace; prefer armor on ties so the column is led by a tank
                score = u->moveSpeed - (isTank(u->archetypeId) ? 0.01f : 0.0f);
            }
            else
            {
                // Head of the column: the unit closest to the objective
                score = distTo(*u, static_cast<float>(sq.targetX), static_cast<float>(sq.targetY));
            }
            if(score < bestScore)
            {
                bestScore = score;
                best = id;
            }
        }
        return best;
    }

    void AISquadController::rebuildRoute(AISquad &sq, EntityManager &entities, Pathfinder &pathfinder)
    {
        sq.route.clear();
        sq.progress = 0;
        sq.marchIndex = 0;
        const Unit *leader = entities.getUnit(sq.leaderId);
        if(!leader)
            return;

        // A flank point that is still ahead splits the route in two legs; once reached it is forgotten
        if(sq.viaX >= 0 && distTo(*leader, static_cast<float>(sq.viaX), static_cast<float>(sq.viaY)) <= 3.0f)
        {
            sq.viaX = sq.viaY = -1;
        }

        std::vector<PathNode> path;
        bool partial = false;
        sq.route.push_back({leader->tileX, leader->tileY});
        int fromX = leader->tileX, fromY = leader->tileY;
        if(sq.viaX >= 0)
        {
            // Strategic route over terrain only: the group's own idle units must not wall the leader in
            pathfinder.setIgnoreUnitsOnce();
            if(pathfinder.findPath(fromX, fromY, sq.viaX, sq.viaY, path, leader->id, &partial) && !path.empty())
            {
                sq.route.insert(sq.route.end(), path.begin(), path.end());
                fromX = path.back().x;
                fromY = path.back().y;
            }
            path.clear();
        }
        pathfinder.setIgnoreUnitsOnce();
        pathfinder.findPath(fromX, fromY, sq.targetX, sq.targetY, path, leader->id, &partial);
        sq.route.insert(sq.route.end(), path.begin(), path.end());
    }

    void AISquadController::holdUnits(const std::vector<int> &ids, EntityManager &entities)
    {
        for(int id : ids)
        {
            Unit *u = entities.getUnit(id);
            if(!u || isEngaged(*u))
                continue;
            u->clearPathState();
            u->clearMoveTarget();
            u->isAttackMoving = false;
            if(u->state == ActionState::Move)
                u->state = ActionState::Idle;
        }
    }

    void AISquadController::orderMarch(AISquad &sq, int routeIndex, TileMap &map, EntityManager &entities, Pathfinder &pathfinder, const AITacticsConfig &tactics)
    {
        if(sq.route.empty())
            return;
        routeIndex = std::clamp(routeIndex, 0, static_cast<int>(sq.route.size()) - 1);
        const PathNode &p = sq.route[routeIndex];

        // Marching direction from a few nodes back, used to put the front line ahead of the rear
        const PathNode &prev = sq.route[std::max(0, routeIndex - 3)];
        float dx = static_cast<float>(p.x - prev.x);
        float dy = static_cast<float>(p.y - prev.y);
        float len = DetMath::hypot(dx, dy);
        if(len > 0.001f)
        {
            dx /= len;
            dy /= len;
        }

        std::vector<int> front, middle, back;
        for(int id : sq.unitIds)
        {
            Unit *u = entities.getUnit(id);
            if(!u)
                continue;
            u->flowMode = false;
            if(isEngaged(*u))
                continue; // never pull a unit out of a fight
            int arch = u->archetypeId;
            if((tactics.armorFront && isTank(arch)) || (tactics.meleeRangedPairs && isMelee(arch)))
            {
                front.push_back(id);
            }
            else if((tactics.meleeRangedPairs || tactics.armorFront) && isRangedSupport(arch))
            {
                back.push_back(id);
            }
            else
            {
                middle.push_back(id);
            }
        }

        auto pointAt = [&](float offset) -> PathNode
        {
            int x = std::clamp(static_cast<int>(std::lround(p.x + dx * offset)), 0, map.getWidth() - 1);
            int y = std::clamp(static_cast<int>(std::lround(p.y + dy * offset)), 0, map.getHeight() - 1);
            if(!map.isPassable(x, y) || map.getWall(x, y) > 0)
                return p;
            return {x, y};
        };

        auto issue = [&](const std::vector<int> &ids, float offset)
        {
            if(ids.empty())
                return;
            PathNode t = pointAt(offset);
            orderLeg(sq, ids, t.x, t.y, entities, pathfinder);
        };
        issue(front, 2.0f);
        issue(middle, 0.0f);
        issue(back, -2.0f);
        sq.orderTimer = 0.0f;
    }

    void AISquadController::advanceMarch(AISquad &sq, TileMap &map, EntityManager &entities, Pathfinder &pathfinder, const ChokepointMap &chokes, const AITacticsConfig &tactics)
    {
        int last = static_cast<int>(sq.route.size()) - 1;
        if(last <= 0 || sq.progress >= last)
        {
            orderLeg(sq, sq.unitIds, sq.targetX, sq.targetY, entities, pathfinder);
            for(int id : sq.unitIds)
            {
                if(Unit *u = entities.getUnit(id))
                    u->flowMode = false;
            }
            setState(sq, AISquadState::Assault);
            sq.orderTimer = 0.0f;
            if(sq.playerSquad)
            {
                // The player's group has arrived: the units finish the move on their own
                std::vector<int> all = sq.unitIds;
                for(int id : all)
                    releaseUnit(sq, id, entities);
            }
            return;
        }

        int next = std::min(sq.progress + std::max(2, tactics.marchStrideTiles), last);

        // Look slightly past the next waypoint so the group never stops inside a corridor entrance
        int chokeAt = -1;
        for(int i = sq.progress; i <= std::min(next + 2, last); ++i)
        {
            if(chokes.isChoke(sq.route[i].x, sq.route[i].y))
            {
                chokeAt = i;
                break;
            }
        }

        if(chokeAt >= 0)
        {
            int rally = std::max(sq.progress, chokeAt - 2);
            if(tactics.waitAtChokepoints && rally > sq.progress)
            {
                sq.marchIndex = rally;
                sq.deployIndex = chokeAt; // remembered entry; the real deploy point is set by beginTransit
                orderMarch(sq, rally, map, entities, pathfinder, tactics);
                setState(sq, AISquadState::ChokeApproach);
                return;
            }
            beginTransit(sq, chokeAt, entities, pathfinder, chokes);
            return;
        }

        sq.marchIndex = next;
        orderMarch(sq, next, map, entities, pathfinder, tactics);
        setState(sq, AISquadState::Marching);
    }

    void AISquadController::beginTransit(AISquad &sq, int chokeIndex, EntityManager &entities, Pathfinder &pathfinder, const ChokepointMap &chokes)
    {
        int last = static_cast<int>(sq.route.size()) - 1;
        int exit = chokeIndex;
        while(exit <= last && chokes.isChoke(sq.route[exit].x, sq.route[exit].y))
            ++exit;
        int deploy = std::min(exit + 4, last);

        sq.deployIndex = deploy;
        sq.marchIndex = deploy;

        // Single file: one shared destination, no sidesteps; reservations make the column queue up
        for(int id : sq.unitIds)
        {
            if(Unit *u = entities.getUnit(id))
                u->flowMode = true;
        }
        const PathNode &d = sq.route[deploy];
        orderLeg(sq, sq.unitIds, d.x, d.y, entities, pathfinder);
        setState(sq, AISquadState::ChokeTransit);
        sq.orderTimer = 0.0f;
    }

    int AISquadController::damageDealtSinceLaunch(const AISquad &sq, const EntityManager &entities) const
    {
        int dealt = 0;
        for(const auto &[id, hp] : sq.playerHpAtLaunch)
        {
            const Entity *e = entities.getEntity(id);
            if(!e || !e->alive || e->state == ActionState::Die)
            {
                dealt += hp;
            }
            else
            {
                dealt += std::max(0, hp - e->currentHp);
            }
        }
        return dealt;
    }

    // The column moves at the pace of its slowest member. Whoever is ahead of the group's centre (along the
    // way to the waypoint) slows down a bit more, whoever is behind walks at full speed: the group closes up
    // smoothly instead of stopping and starting again.
    void AISquadController::paceGroup(const AISquad &sq, EntityManager &entities, float cx, float cy, float leash, bool active)
    {
        float slowest = 1e9f;
        for(int id : sq.unitIds)
        {
            const Unit *u = entities.getUnit(id);
            if(u->moveSpeed > 0.0f)
                slowest = std::min(slowest, u->moveSpeed);
        }
        float dirX = 0.0f, dirY = 0.0f;
        if(active && !sq.route.empty())
        {
            const PathNode &wp = sq.route[std::clamp(sq.marchIndex, 0, static_cast<int>(sq.route.size()) - 1)];
            dirX = static_cast<float>(wp.x) - cx;
            dirY = static_cast<float>(wp.y) - cy;
            const float len = DetMath::hypot(dirX, dirY);
            if(len > 0.5f)
            {
                dirX /= len;
                dirY /= len;
            }
            else
            {
                dirX = dirY = 0.0f;
            }
        }
        for(int id : sq.unitIds)
        {
            Unit *u = entities.getUnit(id);
            if(!active || u->moveSpeed <= 0.0f || slowest >= 1e8f)
            {
                u->paceScale = 1.0f;
                continue;
            }
            const float along = (unitTileX(*u) - cx) * dirX + (unitTileY(*u) - cy) * dirY;
            float scale = slowest / u->moveSpeed;
            if(along < -0.5f * leash)
            {
                scale = 1.0f; // behind: catch up
            }
            else if(along > 0.5f * leash)
            {
                // ahead: the further, the slower (down to a half of the pace, never a stop)
                scale *= std::max(0.5f, 1.0f - 0.5f * (along - 0.5f * leash) / std::max(1.0f, leash));
            }
            u->paceScale = scale;
        }
    }

    // ---------------------------------------------------------------------------
    // Per-squad state machine (runs every kTickSeconds)
    // ---------------------------------------------------------------------------

    void AISquadController::tick(AISquad &sq, float dt, TileMap &map, EntityManager &entities, Pathfinder &pathfinder, const ChokepointMap &chokes, const AITacticsConfig &tactics)
    {
        // Drop the dead
        sq.unitIds.erase(
            std::remove_if(
                sq.unitIds.begin(),
                sq.unitIds.end(),
                [&](int id)
                {
                    const Unit *u = entities.getUnit(id);
                    return !u || !u->alive || u->state == ActionState::Die;
                }),
            sq.unitIds.end());
        if(sq.unitIds.empty())
            return;

        if(!entities.getUnit(sq.leaderId) || std::find(sq.unitIds.begin(), sq.unitIds.end(), sq.leaderId) == sq.unitIds.end())
        {
            sq.leaderId = pickLeader(sq, entities, tactics);
        }
        const Unit *leader = entities.getUnit(sq.leaderId);
        if(!leader)
            return;

        const int localWidth = chokes.openWidth(leader->tileX, leader->tileY);
        const float leash = effectiveLeash(tactics, sq.unitIds.size(), localWidth > 0 && localWidth <= 4);
        sq.leashRadius = leash;
        capDetours(sq, entities); // also catches re-paths the units made on their own (blocked by the crowd)

        // Cohesion metrics: centroid of the group core and the farthest unit from it (Dmax).
        // The core is everyone within two leash radii of the leader, so one far straggler
        // does not drag the centre away from the rest of the group.
        float cx = 0.0f, cy = 0.0f;
        int coreCount = 0;
        for(int id : sq.unitIds)
        {
            const Unit *u = entities.getUnit(id);
            if(distTo(*u, unitTileX(*leader), unitTileY(*leader)) > 2.0f * leash)
                continue;
            cx += unitTileX(*u);
            cy += unitTileY(*u);
            ++coreCount;
        }
        cx /= static_cast<float>(std::max(1, coreCount));
        cy /= static_cast<float>(std::max(1, coreCount));
        float dmax = 0.0f;
        bool engaged = false;
        for(int id : sq.unitIds)
        {
            const Unit *u = entities.getUnit(id);
            dmax = std::max(dmax, distTo(*u, cx, cy));
            engaged = engaged || isEngaged(*u);
        }
        sq.centroidX = cx;
        sq.centroidY = cy;
        sq.maxSpread = dmax;
        // Members clearly outside the leash (stop) and outside it at all (resume): a few stragglers do not
        // hold the whole group up, they catch up while the front slows down
        int outsideStop = 0, outsideResume = 0;
        for(int id : sq.unitIds)
        {
            const float d = distTo(*entities.getUnit(id), cx, cy);
            if(d > leash * 1.2f)
                ++outsideStop;
            if(d > leash)
                ++outsideResume;
        }
        const int tolerated = std::max(1, static_cast<int>(sq.unitIds.size()) / 4);
        paceGroup(sq, entities, cx, cy, leash, sq.state == AISquadState::Marching || sq.state == AISquadState::ChokeApproach);

        sq.stateTimer += dt;
        sq.orderTimer += dt;

        // First leg after launch
        if(sq.state == AISquadState::Marching && sq.orderTimer > 1e5f)
        {
            advanceMarch(sq, map, entities, pathfinder, chokes, tactics);
            return;
        }

        // Hard: a wave that lost most of its strength without hurting the player falls back to staging
        if(tactics.retreatLossRatio > 0.0f && sq.state != AISquadState::Retreating && sq.state != AISquadState::Assault)
        {
            float lost = 1.0f - static_cast<float>(sq.unitIds.size()) / static_cast<float>(std::max(1, sq.initialSize));
            if(lost >= tactics.retreatLossRatio && damageDealtSinceLaunch(sq, entities) < 100)
            {
                for(int id : sq.unitIds)
                {
                    if(Unit *u = entities.getUnit(id))
                    {
                        u->flowMode = false;
                        u->targetEntityId = 0;
                    }
                }
                entities.issueMoveOrder(sq.unitIds, m_stagingX, m_stagingY, pathfinder, false);
                setState(sq, AISquadState::Retreating);
                return;
            }
        }

        // A plain move (player group) ignores enemies on the way: nobody stops to pick a fight
        if(!sq.attackMove)
        {
            for(int id : sq.unitIds)
            {
                Unit *u = entities.getUnit(id);
                if(u->targetEntityId != 0 || u->targetWallTileX >= 0)
                {
                    u->targetEntityId = 0;
                    u->targetWallTileX = u->targetWallTileY = -1;
                    if(u->state == ActionState::Attack)
                        u->state = ActionState::Idle;
                    if(sq.state == AISquadState::Marching && id != sq.leaderId)
                    {
                        orderLeg(sq, {id}, leader->tileX, leader->tileY, entities, pathfinder);
                    }
                }
            }
            engaged = false;
        }

        // Combat pauses the cohesion logic (except inside a corridor, where the column must keep flowing)
        if(engaged && (sq.state == AISquadState::Marching || sq.state == AISquadState::Regrouping || sq.state == AISquadState::ChokeApproach || sq.state == AISquadState::Deploying))
        {
            sq.resumeState = sq.state;
            setState(sq, AISquadState::Engaged);
            return;
        }

        // Returns true when at least one straggler got closer this tick
        auto trackStragglers = [&](float radius, float cxRef, float cyRef) -> bool
        {
            std::vector<int> drop;
            bool progress = false;
            for(int id : sq.unitIds)
            {
                Unit *u = entities.getUnit(id);
                float d = distTo(*u, cxRef, cyRef);
                if(d <= radius)
                {
                    sq.stragglerBest.erase(id);
                    sq.stragglerTimer.erase(id);
                    continue;
                }
                auto it = sq.stragglerBest.find(id);
                if(it == sq.stragglerBest.end() || d < it->second - 0.5f)
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
                    if(sq.stragglerTimer[id] > tactics.stragglerTimeoutSeconds && d > radius * 1.5f)
                        drop.push_back(id);
                }
                // Highest priority for the straggler: walk straight to the leader
                if(isIdleWithoutOrders(*u) && id != sq.leaderId)
                {
                    orderLeg(sq, {id}, leader->tileX, leader->tileY, entities, pathfinder);
                }
            }
            for(int id : drop)
            {
                releaseUnit(sq, id, entities);
                // The player's unit is not left standing: it goes on to the destination on its own
                if(sq.playerSquad)
                    orderLeg(sq, {id}, sq.targetX, sq.targetY, entities, pathfinder);
            }
            return progress;
        };

        switch(sq.state)
        {
            case AISquadState::Marching:
            {
                // Hysteresis: stop only clearly beyond the radius, resume once back inside it,
                // so a long column does not flicker between marching and waiting
                // Inside a narrow passage the column cannot regroup (the waiting vanguard would only block the
                // tail behind it), so the rule is suspended there and applies again in open ground.
                const bool inCorridor = localWidth > 0 && localWidth <= 4;
                // Close to the objective the group goes straight into the attack instead of stopping
                const int remaining = static_cast<int>(sq.route.size()) - 1 - sq.progress;
                const bool nearObjective = remaining <= tactics.marchStrideTiles * 3 / 2;
                if(!inCorridor && !nearObjective && outsideStop > tolerated)
                {
                    // Vanguard holds; stragglers get their own catch-up orders
                    std::vector<int> inside;
                    for(int id : sq.unitIds)
                    {
                        if(distTo(*entities.getUnit(id), cx, cy) <= leash)
                            inside.push_back(id);
                    }
                    holdUnits(inside, entities);
                    sq.stragglerBest.clear();
                    sq.stragglerTimer.clear();
                    setState(sq, AISquadState::Regrouping);
                    break;
                }

                if(outsideStop > 0)
                    trackStragglers(leash * 1.2f, cx, cy);

                const PathNode &wp = sq.route.empty() ? PathNode {sq.targetX, sq.targetY} : sq.route[sq.marchIndex];
                bool reached;
                // A unit that stopped at its formation slot around the waypoint has arrived as well
                auto arrived = [&](const Unit &u, float radius)
                {
                    float d = distTo(u, static_cast<float>(wp.x), static_cast<float>(wp.y));
                    return d <= radius || (isIdleWithoutOrders(u) && d <= std::max(radius, leash) * 1.5f);
                };
                if(tactics.syncToSlowest)
                {
                    reached = arrived(*leader, 2.5f);
                    for(int id : sq.unitIds)
                    {
                        if(!reached)
                            break;
                        reached = arrived(*entities.getUnit(id), leash);
                    }
                }
                else
                {
                    reached = arrived(*leader, 2.5f);
                }
                // Nobody makes headway any more (crowd settled around the waypoint): good enough
                if(DetMath::hypot(cx - sq.stallX, cy - sq.stallY) > 0.5f)
                {
                    sq.stallX = cx;
                    sq.stallY = cy;
                    sq.stallTimer = 0.0f;
                }
                else
                {
                    sq.stallTimer += dt;
                }
                if(!reached && sq.stallTimer > 2.0f && distTo(*leader, static_cast<float>(wp.x), static_cast<float>(wp.y)) <= std::max(2.5f, leash))
                {
                    reached = true;
                }
                if(reached)
                    sq.stallTimer = 0.0f;

                if(reached)
                {
                    sq.progress = sq.marchIndex;
                    advanceMarch(sq, map, entities, pathfinder, chokes, tactics);
                }
                else if(sq.stateTimer > 25.0f)
                {
                    // Waypoint unreachable (blocked by new structures): plan again from the leader
                    rebuildRoute(sq, entities, pathfinder);
                    advanceMarch(sq, map, entities, pathfinder, chokes, tactics);
                }
                else if(sq.orderTimer > 4.0f && isIdleWithoutOrders(*leader))
                {
                    // Orders were overridden elsewhere (base defense, call for help): re-issue the leg
                    orderMarch(sq, sq.marchIndex, map, entities, pathfinder, tactics);
                }
                break;
            }

            case AISquadState::Regrouping:
            {
                // Back together, or compacted as far as the crowd allows (no one closed in for 1.5 s)
                const bool compacted = sq.quietTimer >= 1.5f && dmax <= leash * 1.5f;
                if(outsideResume <= tolerated || compacted)
                {
                    sq.stragglerBest.clear();
                    sq.stragglerTimer.clear();
                    orderMarch(sq, sq.marchIndex, map, entities, pathfinder, tactics);
                    setState(sq, AISquadState::Marching);
                    break;
                }
                sq.quietTimer = trackStragglers(leash, cx, cy) ? 0.0f : sq.quietTimer + dt;
                break;
            }

            case AISquadState::ChokeApproach:
            {
                const PathNode &rally = sq.route[sq.marchIndex];
                bool leaderAtRally = distTo(*leader, static_cast<float>(rally.x), static_cast<float>(rally.y)) <= 2.5f;
                sq.quietTimer = leaderAtRally ? sq.quietTimer + dt : 0.0f; // hold time at the rally point
                bool gathered = dmax <= leash;
                if((leaderAtRally && sq.quietTimer >= tactics.preChokeHoldSeconds && gathered) || sq.stateTimer > 12.0f)
                {
                    sq.progress = sq.marchIndex;
                    beginTransit(sq, sq.deployIndex, entities, pathfinder, chokes);
                    break;
                }
                if(leaderAtRally && !gathered)
                    trackStragglers(leash, cx, cy);
                break;
            }

            case AISquadState::ChokeTransit:
            {
                const PathNode &d = sq.route[sq.deployIndex];
                bool allOut = true;
                for(int id : sq.unitIds)
                {
                    const Unit *u = entities.getUnit(id);
                    if(chokes.isChoke(u->tileX, u->tileY) || distTo(*u, static_cast<float>(d.x), static_cast<float>(d.y)) > leash + 1.5f)
                    {
                        allOut = false;
                    }
                }
                float timeout = std::max(15.0f, 2.0f * static_cast<float>(sq.unitIds.size()));
                if(allOut || sq.stateTimer > timeout)
                {
                    if(!allOut)
                    {
                        // Whoever is still stuck in the passage is left behind; the group moves on
                        std::vector<int> stuck;
                        for(int id : sq.unitIds)
                        {
                            const Unit *u = entities.getUnit(id);
                            if(distTo(*u, static_cast<float>(d.x), static_cast<float>(d.y)) > leash + 1.5f)
                                stuck.push_back(id);
                        }
                        for(int id : stuck)
                            releaseUnit(sq, id, entities);
                    }
                    for(int id : sq.unitIds)
                    {
                        if(Unit *u = entities.getUnit(id))
                            u->flowMode = false;
                    }
                    setState(sq, AISquadState::Deploying);
                    break;
                }
                if(sq.orderTimer > 3.0f)
                {
                    std::vector<int> lost;
                    for(int id : sq.unitIds)
                    {
                        const Unit *u = entities.getUnit(id);
                        if(isIdleWithoutOrders(*u) && distTo(*u, static_cast<float>(d.x), static_cast<float>(d.y)) > 2.0f)
                        {
                            lost.push_back(id);
                        }
                    }
                    if(!lost.empty())
                        orderLeg(sq, lost, d.x, d.y, entities, pathfinder);
                    sq.orderTimer = 0.0f;
                }
                break;
            }

            case AISquadState::Deploying:
            {
                if(!tactics.waitAtChokepoints || dmax <= leash || sq.stateTimer > 6.0f)
                {
                    sq.progress = sq.deployIndex;
                    advanceMarch(sq, map, entities, pathfinder, chokes, tactics);
                }
                break;
            }

            case AISquadState::Engaged:
            {
                if(engaged)
                {
                    sq.quietTimer = 0.0f;
                    // Idle members help the ones already fighting
                    if(sq.orderTimer > 3.0f)
                    {
                        std::vector<int> idle;
                        int fx = leader->tileX, fy = leader->tileY;
                        for(int id : sq.unitIds)
                        {
                            const Unit *u = entities.getUnit(id);
                            if(isEngaged(*u))
                            {
                                fx = u->tileX;
                                fy = u->tileY;
                            }
                        }
                        for(int id : sq.unitIds)
                        {
                            const Unit *u = entities.getUnit(id);
                            if(isIdleWithoutOrders(*u))
                                idle.push_back(id);
                        }
                        if(!idle.empty())
                            orderLeg(sq, idle, fx, fy, entities, pathfinder);
                        sq.orderTimer = 0.0f;
                    }
                }
                else
                {
                    sq.quietTimer += dt;
                    if(sq.quietTimer >= 2.0f)
                    {
                        sq.leaderId = pickLeader(sq, entities, tactics);
                        rebuildRoute(sq, entities, pathfinder);
                        advanceMarch(sq, map, entities, pathfinder, chokes, tactics);
                    }
                }
                break;
            }

            case AISquadState::Assault:
            {
                if(sq.orderTimer < 3.0f)
                    break;
                sq.orderTimer = 0.0f;
                std::vector<int> idle;
                for(int id : sq.unitIds)
                {
                    const Unit *u = entities.getUnit(id);
                    if(isIdleWithoutOrders(*u))
                        idle.push_back(id);
                }
                if(idle.empty())
                    break;

                // Objective gone: move on to the nearest remaining player structure
                int bestX = -1, bestY = -1;
                float bestD = 1e9f;
                for(const auto &e : entities.getAllEntities())
                {
                    if(!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner >= kMaxPlayers || !Players::hostile(e.owner, sq.owner))
                    {
                        continue;
                    }
                    float d = DetMath::hypot(e.tileX - cx, e.tileY - cy);
                    if(d < bestD)
                    {
                        bestD = d;
                        bestX = e.tileX;
                        bestY = e.tileY;
                    }
                }
                if(bestX >= 0)
                {
                    orderLeg(sq, idle, bestX, bestY, entities, pathfinder);
                }
                else if(idle.size() == sq.unitIds.size())
                {
                    std::vector<int> all = sq.unitIds;
                    for(int id : all)
                        releaseUnit(sq, id, entities);
                }
                break;
            }

            case AISquadState::Retreating:
            {
                bool home = true;
                for(int id : sq.unitIds)
                {
                    if(distTo(*entities.getUnit(id), static_cast<float>(m_stagingX), static_cast<float>(m_stagingY)) > 6.0f)
                    {
                        home = false;
                        break;
                    }
                }
                if(home || sq.stateTimer > 40.0f)
                {
                    std::vector<int> all = sq.unitIds;
                    for(int id : all)
                        releaseUnit(sq, id, entities); // they join the next wave
                }
                break;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Debug visualization
    // ---------------------------------------------------------------------------

    void AISquadController::renderDebug(Renderer &renderer, BitmapFontRenderer &font, const Camera &camera, const EntityManager &entities, float leashTiles) const
    {
        for(const auto &sq : m_squads)
        {
            const Entity *leader = entities.getEntity(sq.leaderId);
            if(!leader)
                continue;
            float lx = leader->getUnitCenterPixelX();
            float ly = leader->getUnitCenterPixelY();

            // Leash lines: green = in formation, yellow = at the limit, red = straggling
            for(int id : sq.unitIds)
            {
                const Entity *u = entities.getEntity(id);
                if(!u || id == sq.leaderId)
                    continue;
                float d = DetMath::hypot(u->getUnitCenterPixelX() - lx, u->getUnitCenterPixelY() - ly) / TILE_WIDTH;
                SDL_Color c = (d <= 2.0f) ? SDL_Color {60, 230, 90, 220} : (d <= std::max(leashTiles, sq.leashRadius)) ? SDL_Color {240, 210, 60, 220} : SDL_Color {240, 60, 50, 230};
                renderer.drawLineWorld(u->getUnitCenterPixelX(), u->getUnitCenterPixelY(), lx, ly, camera, c);
            }

            // Centroid and the allowed cohesion radius
            float cxp = sq.centroidX * TILE_WIDTH + TILE_WIDTH * 0.5f;
            float cyp = sq.centroidY * TILE_HEIGHT + TILE_HEIGHT * 0.5f;
            float radius = sq.leashRadius;
            RenderUtility::DrawTileRadius(renderer, camera, cxp, cyp, radius, SDL_Color {255, 255, 255, 120}, 1.5f);
            renderer.fillRectWorld(cxp - 3, cyp - 3, 6, 6, camera, {255, 255, 255, 230});

            // Current waypoint
            if(!sq.route.empty())
            {
                const PathNode &wp = sq.route[std::clamp(sq.marchIndex, 0, static_cast<int>(sq.route.size()) - 1)];
                renderer.drawRectWorld(wp.x * TILE_WIDTH + 4.0f, wp.y * TILE_HEIGHT + 4.0f, TILE_WIDTH - 8.0f, TILE_HEIGHT - 8.0f, camera, {120, 200, 255, 220});
            }

            // "G<n>" label above the leader with the current state
            float sx, sy;
            camera.worldToScreen(lx, ly - TILE_HEIGHT * 1.2f, sx, sy);
            std::string label = "G" + std::to_string(sq.id);
            int w = font.getTextWidth(label, 6) + 10;
            renderer.fillRectScreen(static_cast<int>(sx) - w / 2, static_cast<int>(sy) - 10, w, 20, {12, 14, 20, 210});
            renderer.drawRectScreen(static_cast<int>(sx) - w / 2, static_cast<int>(sy) - 10, w, 20, {232, 176, 74, 255});
            font.drawString(label, static_cast<int>(sx), static_cast<int>(sy), 6, {232, 176, 74, 255}, false, Anchor::HCENTER | Anchor::VCENTER);
            std::string info = std::string(squadStateName(sq.state)) + "  " + std::to_string(sq.unitIds.size()) + "/" + std::to_string(sq.initialSize);
            font.drawString(info, static_cast<int>(sx), static_cast<int>(sy) + 18, 1, {226, 231, 238, 255}, true, Anchor::HCENTER | Anchor::VCENTER);
        }
    }

} // namespace OpenArtOfWar
