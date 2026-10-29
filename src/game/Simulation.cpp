#include "game/Simulation.hpp"
#include "game/AIController.hpp"
#include "game/Building.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/Pathfinder.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "game/TileMap.hpp"
#include "game/Unit.hpp"
#include "core/Utility.hpp"

#include <algorithm>
#include <cstring>

namespace OpenArtOfWar
{

    namespace
    {

        bool alive(const Entity *e)
        {
            return e && e->alive && e->state != ActionState::Die;
        }

        // FNV-1a, 64 bit
        struct Hasher
        {
            uint64_t h {0xcbf29ce484222325ull};
            void bytes(const void *p, size_t n)
            {
                const auto *b = static_cast<const uint8_t *>(p);
                for(size_t i = 0; i < n; ++i)
                {
                    h ^= b[i];
                    h *= 0x100000001b3ull;
                }
            }
            void i32(int32_t v)
            {
                bytes(&v, sizeof v);
            }
            void f32(float v)
            { // the exact bits: peers in sync have bit-identical floats
                uint32_t u;
                std::memcpy(&u, &v, sizeof u);
                bytes(&u, sizeof u);
            }
        };

    } // namespace

    const AITacticsConfig &Simulation::groupTactics()
    {
        // Cohesion rules for player control groups: the column keeps within the leash and gathers at
        // chokepoints, but never retreats on its own
        static const AITacticsConfig t = []
        {
            AITacticsConfig c;
            c.leashTiles = 3.0f;
            c.leashPerUnit = 0.35f;
            c.corridorLeashFactor = 1.6f;
            c.waitAtChokepoints = true;
            c.preChokeHoldSeconds = 1.0f;
            c.syncToSlowest = false;
            c.armorFront = false;
            c.meleeRangedPairs = false;
            c.retreatLossRatio = 0.0f;
            c.stragglerTimeoutSeconds = 6.0f;
            c.marchStrideTiles = 5;
            return c;
        }();
        return t;
    }

    void Simulation::reset(uint32_t seed)
    {
        Utility::rng().reseed(seed);
        m_tick = 0;
        m_economyMask = 0;
        m_pending.clear();
        m_log.clear();
        for(auto &s : m_squads)
            s.clear();
        for(auto &r : m_research)
            r = ResearchProgress {};
        m_events.clear();
    }

    void Simulation::startMatch(const MatchSetup &setup)
    {
        reset(setup.seed);
        for(PlayerId p = 0; p < kMaxPlayers; ++p)
        {
            if(!setup.slots[p].active || p == setup.local)
                continue;
            m_economies[p].reset(setup.startingCredits);
            m_economies[p].setPlayer(p);
            m_economyMask |= 1u << p;
        }
    }

    Economy &Simulation::economyOf(PlayerId player, SimContext &ctx)
    {
        if(player < kMaxPlayers && (m_economyMask >> player) & 1u)
            return m_economies[player];
        return ctx.economy;
    }

    void Simulation::updateEconomies(float dt, SimContext &ctx)
    {
        for(PlayerId p = 0; p < kMaxPlayers; ++p)
        {
            if((m_economyMask >> p) & 1u)
                m_economies[p].update(dt, ctx.entities, ctx.map, ctx.pathfinder);
        }
    }

    void Simulation::issue(SimCommand command)
    {
        if(m_sink)
        {
            m_sink(std::move(command));
            return;
        }
        schedule(m_tick, std::move(command));
    }

    void Simulation::schedule(uint32_t tick, SimCommand command)
    {
        std::sort(command.ids.begin(), command.ids.end());
        command.ids.erase(std::unique(command.ids.begin(), command.ids.end()), command.ids.end());
        m_pending.push_back({tick, std::move(command)});
    }

    void Simulation::applyCommands(SimContext &ctx)
    {
        // Due commands in a fixed order: by tick, then by player; issue order within a player (stable sort)
        std::vector<TimedCommand> due;
        auto split = std::stable_partition(m_pending.begin(), m_pending.end(), [this](const TimedCommand &t) { return t.tick > m_tick; });
        due.assign(std::make_move_iterator(split), std::make_move_iterator(m_pending.end()));
        m_pending.erase(split, m_pending.end());
        std::stable_sort(
            due.begin(),
            due.end(),
            [](const TimedCommand &l, const TimedCommand &r)
            {
                if(l.tick != r.tick)
                    return l.tick < r.tick;
                return l.command.player < r.command.player;
            });
        for(TimedCommand &t : due)
        {
            apply(t.command, ctx);
            if(m_recording)
                m_log.push_back({m_tick, std::move(t.command)});
        }
    }

    std::vector<int> Simulation::ownUnits(const SimCommand &cmd, const SimContext &ctx) const
    {
        std::vector<int> out;
        for(uint32_t id : cmd.ids)
        {
            const Entity *e = ctx.entities.getEntity(static_cast<int>(id));
            if(e && e->alive && !e->isBuilding && (cmd.debug || e->owner == cmd.player))
                out.push_back(static_cast<int>(id));
        }
        return out;
    }

    bool Simulation::marchAsGroup(PlayerId player, const std::vector<int> &unitIds, int tileX, int tileY, bool attackMove, SimContext &ctx)
    {
        if(unitIds.size() < 2 || player >= kMaxPlayers)
            return false;
        AISquadController &squads = m_squads[player];
        const int squadId = squads.createSquad(unitIds, tileX, tileY, 0, ctx.entities, ctx.pathfinder, groupTactics());
        AISquad *sq = squads.findSquad(squadId);
        if(!sq)
            return false;
        sq->attackMove = attackMove;
        sq->playerSquad = true;
        // Units stop what they were doing; the squad orders the first leg on its next tick
        for(int id : unitIds)
        {
            if(Unit *u = ctx.entities.getUnit(id))
            {
                u->clearPathState();
                u->clearMoveTarget();
                u->targetEntityId = 0;
            }
        }
        return true;
    }

    void Simulation::apply(const SimCommand &cmd, SimContext &ctx)
    {
        EntityManager &entities = ctx.entities;
        TileMap &map = ctx.map;
        Pathfinder &pathfinder = ctx.pathfinder;
        if(cmd.player >= kMaxPlayers)
            return;
        Economy &economy = economyOf(cmd.player, ctx);
        const Faction faction = Players::factionOf(cmd.player);

        switch(cmd.type)
        {
            case CommandType::OrderAt:
            {
                const std::vector<int> units = ownUnits(cmd, ctx);
                m_squads[cmd.player].releaseUnits(units, entities);
                if(units.empty())
                    return;
                const float wx = static_cast<float>(cmd.x), wy = static_cast<float>(cmd.y);
                const int clickedId = entities.getUnitAt(wx, wy);
                const int destX = cmd.x / TILE_WIDTH, destY = cmd.y / TILE_HEIGHT;
                const bool asGroup = cmd.b != 0;
                if(static_cast<OrderKind>(cmd.a) == OrderKind::AttackMove)
                {
                    if(clickedId > 0)
                    {
                        const Entity *target = entities.getEntity(clickedId);
                        if(target && Players::hostile(target->owner, cmd.player) && entities.isEntityVisibleToPlayer(*target, cmd.player, map))
                        {
                            entities.issueAttackOrder(units, clickedId, &pathfinder);
                        }
                        else
                        {
                            entities.issueAttackMoveOrder(units, destX, destY, pathfinder);
                        }
                    }
                    else if(map.getWall(destX, destY) > 0 && Players::hostile(map.getWallOwner(destX, destY), cmd.player))
                    {
                        entities.issueAttackWallOrder(units, destX, destY, pathfinder);
                    }
                    else if(!(asGroup && marchAsGroup(cmd.player, units, destX, destY, true, ctx)))
                    {
                        entities.issueAttackMoveOrder(units, destX, destY, pathfinder);
                    }
                }
                else
                {
                    if(clickedId > 0)
                    {
                        const Entity *target = entities.getEntity(clickedId);
                        const Entity *first = entities.getEntity(units[0]);
                        if(target && Players::hostile(target->owner, first->owner) && entities.isEntityVisibleToPlayer(*target, first->owner, map))
                        {
                            entities.issueAttackOrder(units, clickedId, &pathfinder);
                        }
                        else
                        {
                            entities.issueMoveOrder(units, destX, destY, pathfinder, false);
                        }
                    }
                    else if(map.getWall(destX, destY) > 0)
                    {
                        // Clicked right on a wall: attack it (even an own one, when ordered so)
                        entities.issueAttackWallOrder(units, destX, destY, pathfinder);
                    }
                    else if(!(asGroup && marchAsGroup(cmd.player, units, destX, destY, true, ctx)))
                    {
                        // (a control group marches like an AI wave: it fights whatever it meets on the way)
                        entities.issueMoveOrder(units, destX, destY, pathfinder, false);
                    }
                }
                break;
            }
            case CommandType::AttackWall:
            {
                const std::vector<int> units = ownUnits(cmd, ctx);
                m_squads[cmd.player].releaseUnits(units, entities);
                if(!units.empty())
                    entities.issueAttackWallOrder(units, cmd.x, cmd.y, pathfinder);
                break;
            }
            case CommandType::SetRally:
                for(uint32_t id : cmd.ids)
                {
                    Building *b = entities.getBuilding(static_cast<int>(id));
                    if(b && b->alive && b->isUnitProducer() && b->owner == cmd.player)
                    {
                        b->rallyTileX = std::clamp(cmd.x / TILE_WIDTH, 0, map.getWidth() - 1);
                        b->rallyTileY = std::clamp(cmd.y / TILE_HEIGHT, 0, map.getHeight() - 1);
                    }
                }
                break;
            case CommandType::ToggleSiege:
                entities.toggleSiegeMode(ownUnits(cmd, ctx));
                break;
            case CommandType::Stop:
                for(int id : ownUnits(cmd, ctx))
                {
                    Unit *u = entities.getUnit(id);
                    if(!u)
                        continue;
                    u->path.clear();
                    u->pathIndex = 0;
                    u->targetEntityId = 0;
                    u->targetWallTileX = -1;
                    u->targetWallTileY = -1;
                    u->isAttackMoving = false;
                    u->waitingForPath = false;
                    u->state = ActionState::Idle;
                }
                break;
            case CommandType::HoldPosition:
                for(int id : ownUnits(cmd, ctx))
                {
                    Unit *u = ctx.entities.getUnit(id);
                    if(!u)
                        continue;
                    u->path.clear();
                    u->pathIndex = 0;
                    u->isAttackMoving = false;
                    u->waitingForPath = false;
                    u->state = ActionState::Idle;
                }
                break;
            case CommandType::PlaceBuilding:
            {
                const auto &blueprints = EntityManager::getBlueprints();
                if(cmd.a < 0 || cmd.a >= static_cast<int>(blueprints.size()))
                    return;
                const auto &bp = blueprints[cmd.a];
                if(!entities.canPlaceBuilding(cmd.x, cmd.y, bp.width, bp.height, map, cmd.player))
                    return;
                if(!economy.canAfford(bp.creditCost))
                    return;
                economy.spendCredits(bp.creditCost);
                if(bp.isWall)
                {
                    map.setWall(cmd.x, cmd.y, faction, 100, cmd.player);
                }
                else if(entities.spawnBuildingByIndex(bp.buildingIndex, cmd.x, cmd.y, faction, 0.0f, cmd.player) > 0)
                {
                    for(int ty = cmd.y; ty < cmd.y + bp.height && ty < map.getHeight(); ++ty)
                    {
                        for(int tx = cmd.x; tx < cmd.x + bp.width && tx < map.getWidth(); ++tx)
                            map.setPassable(tx, ty, false);
                    }
                }
                break;
            }
            case CommandType::Produce:
            {
                const Building *b = entities.getBuilding(cmd.b);
                if(!alive(b) || b->owner != cmd.player || !b->isUnitProducer() || b->buildProgress < 1.0f)
                    return;
                const auto *cfg = entities.getUnitConfig(cmd.a);
                economy.queueUnit(cmd.a, cfg ? cfg->creditCost : 100, cfg ? cfg->buildTime : 4.0f, cmd.b, entities);
                break;
            }
            case CommandType::CancelProduce:
            {
                const Building *b = entities.getBuilding(cmd.b);
                if(!b || b->owner != cmd.player)
                    return;
                economy.cancelUnit(cmd.a, cmd.b, entities);
                break;
            }
            case CommandType::StartResearch:
            {
                ResearchProgress &r = m_research[cmd.player];
                if(cmd.a < 0 || cmd.a >= kResearchCount || r.active >= 0)
                    return;
                if(!hasResearchBuilding(cmd.player, entities) || isResearched(cmd.player, cmd.a) || !isResearchAvailable(cmd.player, cmd.a))
                {
                    return;
                }
                const ResearchDef &def = researchDef(faction, cmd.a);
                if(!economy.canAfford(def.cost))
                    return;
                economy.spendCredits(def.cost);
                r.active = cmd.a;
                r.timer = 0.0f;
                break;
            }
            case CommandType::Demolish:
            {
                int refund = 0;
                for(uint32_t id : cmd.ids)
                {
                    const Building *b = entities.getBuilding(static_cast<int>(id));
                    if(alive(b) && b->owner == cmd.player)
                        refund += entities.demolishBuilding(b->id, map);
                }
                if(refund > 0)
                    economy.addCredits(refund);
                break;
            }
            case CommandType::DebugCredits:
                if(cmd.debug)
                    economy.addCredits(cmd.a);
                break;
            case CommandType::DebugSpawn:
            {
                if(!cmd.debug)
                    return;
                switch(static_cast<DebugSpawnKind>(cmd.b))
                {
                    case DebugSpawnKind::Wall:
                        map.setWall(cmd.x, cmd.y, faction, 100, cmd.player);
                        break;
                    case DebugSpawnKind::Building:
                        if(entities.spawnBuildingByIndex(cmd.a, cmd.x, cmd.y, faction, 1.0f, cmd.player) > 0)
                        {
                            for(int ty = cmd.y; ty < cmd.y + cmd.d && ty < map.getHeight(); ++ty)
                            {
                                for(int tx = cmd.x; tx < cmd.x + cmd.c && tx < map.getWidth(); ++tx)
                                    map.setPassable(tx, ty, false);
                            }
                        }
                        break;
                    case DebugSpawnKind::Unit:
                        entities.spawnUnitByArchetype(cmd.a, cmd.x, cmd.y, faction, cmd.player);
                        break;
                }
                break;
            }
            case CommandType::Count:
                break;
        }
    }

    void Simulation::updateResearch(float dt, SimContext &ctx)
    {
        for(PlayerId p = 0; p < kMaxPlayers; ++p)
        {
            ResearchProgress &r = m_research[p];
            // Research pauses while the Tech Center / Laboratory is down
            if(r.active < 0 || r.active >= kResearchCount || !hasResearchBuilding(p, ctx.entities))
                continue;
            r.timer += dt;
            if(r.timer < r.duration)
                continue;
            completeResearch(p, r.active, ctx.entities);
            m_events.push_back({SimEvent::Type::ResearchCompleted, p, r.active});
            r.active = -1;
            r.timer = 0.0f;
        }
    }

    void Simulation::updateSquads(float dt, SimContext &ctx, const ChokepointMap &chokepoints)
    {
        for(auto &squads : m_squads)
        {
            squads.update(dt, ctx.map, ctx.entities, ctx.pathfinder, chokepoints, groupTactics());
            squads.takeReleasedUnits();
        }
    }

    std::vector<SimEvent> Simulation::takeEvents()
    {
        std::vector<SimEvent> out;
        out.swap(m_events);
        return out;
    }

    uint64_t Simulation::checksum(const SimContext &ctx) const
    {
        Hasher h;
        h.i32(static_cast<int32_t>(m_tick));
        for(const Entity &e : ctx.entities.getAllEntities())
        {
            h.i32(e.id);
            h.i32(static_cast<int32_t>(e.type));
            h.i32(static_cast<int32_t>(e.faction));
            h.i32(e.owner);
            h.i32(e.alive ? 1 : 0);
            h.i32(static_cast<int32_t>(e.state));
            h.i32(e.tileX);
            h.i32(e.tileY);
            h.f32(e.subX);
            h.f32(e.subY);
            h.i32(e.currentHp);
            h.i32(e.targetEntityId);
        }
        // Credits in player order (the local player's economy is SimContext::economy on every peer, so the
        // order must not depend on it)
        for(PlayerId p = 0; p < kMaxPlayers; ++p)
        {
            if((m_economyMask >> p) & 1u)
                h.i32(m_economies[p].getCredits());
            else if(p == ctx.economy.player())
                h.i32(ctx.economy.getCredits());
        }
        const TileMap &map = ctx.map;
        for(int y = 0; y < map.getHeight(); ++y)
        {
            for(int x = 0; x < map.getWidth(); ++x)
            {
                const int wall = map.getWall(x, y);
                if(wall > 0)
                {
                    h.i32(x);
                    h.i32(y);
                    h.i32(wall);
                    h.i32(map.getWallOwner(x, y));
                }
            }
        }
        for(PlayerId p = 0; p < kMaxPlayers; ++p)
        {
            for(int i = 0; i < kResearchCount; ++i)
                h.i32(isResearched(p, i) ? 1 : 0);
        }
        return h.h;
    }

} // namespace OpenArtOfWar
