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
#include <iostream>
#include <map>
#include <tuple>

namespace OpenArtOfWar
{

namespace
{

bool Alive(const Entity *e)
{
    return e && e->alive && e->state != ActionState::Die;
}

// FNV-1a, 64 bit
struct Hasher
{
    std::uint64_t h {0xcbf29ce484222325ull};
    void Bytes(const void *p, std::size_t n)
    {
        const auto *b = static_cast<const std::uint8_t *>(p);
        for (std::size_t i = 0; i < n; ++i)
        {
            h ^= b[i];
            h *= 0x100000001b3ull;
        }
    }
    void I32(std::int32_t v)
    {
        Bytes(&v, sizeof v);
    }
    void F32(float v)
    { // the exact bits: peers in sync have bit-identical floats
        std::uint32_t u;
        std::memcpy(&u, &v, sizeof u);
        Bytes(&u, sizeof u);
    }
};

} // namespace

const AITacticsConfig &Simulation::GroupTactics()
{
    // Cohesion rules for player control groups: the column keeps within the leash and gathers at
    // chokepoints, but never retreats on its own
    static const AITacticsConfig t = [] {
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

void Simulation::Reset(std::uint32_t seed)
{
    Utility::Rng().Reseed(seed);
    m_tick = 0;
    m_economyMask = 0;
    m_pending.clear();
    m_log.clear();
    for (auto &s : m_squads)
        s.Clear();
    for (auto &r : m_research)
        r = ResearchProgress {};
    m_events.clear();
}

void Simulation::StartMatch(const MatchSetup &setup)
{
    Reset(setup.seed);
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        if (!setup.slots[p].active || p == setup.local)
            continue;
        m_economies[p].Reset(setup.startingCredits * HandicapPercentOf(setup.slots[p].handicap) / 100);
        m_economies[p].SetPlayer(p);
        m_economyMask |= 1u << p;
    }
}

Economy &Simulation::EconomyOf(PlayerId player, SimContext &ctx)
{
    if (player < kMaxPlayers && (m_economyMask >> player) & 1u)
        return m_economies[player];
    return ctx.economy;
}

void Simulation::UpdateEconomies(float dt, SimContext &ctx)
{
    // Every economy in slot order, the local one (SimContext::economy) at its own slot. Economies create
    // units: updating the local one first gave the new units other ids on every computer (desync, and the
    // owner's later commands no longer reached its units)
    bool localDone = false;
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        if ((m_economyMask >> p) & 1u)
            m_economies[p].Update(dt, ctx.entities, ctx.map, ctx.pathfinder);
        else if (!localDone && p == ctx.economy.player())
        {
            ctx.economy.Update(dt, ctx.entities, ctx.map, ctx.pathfinder);
            localDone = true;
        }
    }
    if (!localDone)
        ctx.economy.Update(dt, ctx.entities, ctx.map, ctx.pathfinder);
}

void Simulation::Issue(SimCommand command)
{
    if (m_sink)
    {
        m_sink(std::move(command));
        return;
    }
    if (m_record)
        m_record(m_tick, command);
    Schedule(m_tick, std::move(command));
}

void Simulation::Schedule(std::uint32_t tick, SimCommand command)
{
    std::sort(command.ids.begin(), command.ids.end());
    command.ids.erase(std::unique(command.ids.begin(), command.ids.end()), command.ids.end());
    m_pending.push_back({tick, std::move(command)});
}

void Simulation::ApplyCommands(SimContext &ctx)
{
    // Due commands in a fixed order: by tick, then by player; issue order within a player (stable sort)
    std::vector<TimedCommand> due;
    auto split = std::stable_partition(
        m_pending.begin(), m_pending.end(), [this](const TimedCommand &t) { return t.tick > m_tick; });
    due.assign(std::make_move_iterator(split), std::make_move_iterator(m_pending.end()));
    m_pending.erase(split, m_pending.end());
    std::stable_sort(due.begin(), due.end(), [](const TimedCommand &l, const TimedCommand &r) {
        if (l.tick != r.tick)
            return l.tick < r.tick;
        return l.command.player < r.command.player;
    });
    for (TimedCommand &t : due)
    {
        Apply(t.command, ctx);
        if (m_recording)
            m_log.push_back({m_tick, std::move(t.command)});
    }
}

std::vector<int> Simulation::OwnUnits(const SimCommand &cmd, const SimContext &ctx) const
{
    std::vector<int> out;
    for (std::uint32_t id : cmd.ids)
    {
        const Entity *e = ctx.entities.GetEntity(static_cast<int>(id));
        if (e && e->alive && !e->isBuilding && (cmd.debug || e->owner == cmd.player))
            out.push_back(static_cast<int>(id));
    }
    return out;
}

bool Simulation::MarchAsGroup(
    PlayerId player, const std::vector<int> &unitIds, int tileX, int tileY, bool attackMove, SimContext &ctx)
{
    if (unitIds.size() < 2 || player >= kMaxPlayers)
        return false;
    AISquadController &squads = m_squads[player];
    const int squadId = squads.CreateSquad(unitIds, tileX, tileY, 0, ctx.entities, ctx.pathfinder, GroupTactics());
    AISquad *sq = squads.FindSquad(squadId);
    if (!sq)
        return false;
    sq->attackMove = attackMove;
    sq->playerSquad = true;
    // Units stop what they were doing; the squad orders the first leg on its next tick
    for (int id : unitIds)
    {
        if (Unit *u = ctx.entities.GetUnit(id))
        {
            u->ClearPathState();
            u->ClearMoveTarget();
            u->targetEntityId = 0;
        }
    }
    return true;
}

void Simulation::OrderUnitsAt(
    PlayerId player, const std::vector<int> &units, int kind, int x, int y, bool asGroup, SimContext &ctx)
{
    EntityManager &entities = ctx.entities;
    TileMap &map = ctx.map;
    Pathfinder &pathfinder = ctx.pathfinder;
    const float wx = static_cast<float>(x), wy = static_cast<float>(y);
    const int clickedId = entities.GetUnitAt(wx, wy);
    int destX = x / TILE_WIDTH, destY = y / TILE_HEIGHT;
    // The click may be on the very edge of the map or on ground nobody can stand on: the nearest open tile is the goal
    // (a wall that is clicked on is attacked, see below, so it keeps its tile)
    const bool onWall = destX >= 0 && destY >= 0 && destX < map.GetWidth() && destY < map.GetHeight()
                        && map.GetWall(destX, destY) > 0;
    if (!onWall)
        EntityManager::NearestOpenTile(map, destX, destY);
    if (static_cast<OrderKind>(kind) == OrderKind::AttackMove)
    {
        if (clickedId > 0)
        {
            const Entity *target = entities.GetEntity(clickedId);
            if (target && Players::Hostile(target->owner, player)
                && entities.IsEntityVisibleToPlayer(*target, player, map))
            {
                entities.IssueAttackOrder(units, clickedId, &pathfinder);
            }
            else
            {
                entities.IssueAttackMoveOrder(units, destX, destY, pathfinder);
            }
        }
        else if (map.GetWall(destX, destY) > 0 && Players::Hostile(map.GetWallOwner(destX, destY), player))
        {
            entities.IssueAttackWallOrder(units, destX, destY, pathfinder);
        }
        else if (!(asGroup && MarchAsGroup(player, units, destX, destY, true, ctx)))
        {
            entities.IssueAttackMoveOrder(units, destX, destY, pathfinder);
        }
    }
    else
    {
        if (clickedId > 0)
        {
            const Entity *target = entities.GetEntity(clickedId);
            const Entity *first = entities.GetEntity(units[0]);
            if (target && Players::Hostile(target->owner, first->owner)
                && entities.IsEntityVisibleToPlayer(*target, first->owner, map))
            {
                entities.IssueAttackOrder(units, clickedId, &pathfinder);
            }
            else
            {
                entities.IssueMoveOrder(units, destX, destY, pathfinder, false);
            }
        }
        else if (map.GetWall(destX, destY) > 0)
        {
            // Clicked right on a wall: attack it (even an own one, when ordered so)
            entities.IssueAttackWallOrder(units, destX, destY, pathfinder);
        }
        else if (!(asGroup && MarchAsGroup(player, units, destX, destY, true, ctx)))
        {
            // (a control group marches like an AI wave: it fights whatever it meets on the way)
            entities.IssueMoveOrder(units, destX, destY, pathfinder, false);
        }
    }
    // The order line ends at the clicked point
    for (int id : units)
    {
        if (Unit *u = entities.GetUnit(id))
        {
            u->orderX = x;
            u->orderY = y;
        }
    }
}

void Simulation::ClearOrders(const std::vector<int> &units, EntityManager &entities)
{
    for (int id : units)
    {
        if (Unit *u = entities.GetUnit(id))
        {
            u->orderQueue.clear();
            u->orderX = u->orderY = -1;
        }
    }
}

void Simulation::UpdateOrderQueues(SimContext &ctx)
{
    // Units whose order is done take the next queued one; units of a player that start the same order this tick go
    // together (one formation). Entity order and the ordered map keep it the same on every peer.
    std::map<std::tuple<PlayerId, int, int, int>, std::vector<int>> starting;
    for (auto &e : ctx.entities.GetAllEntities())
    {
        Unit *u = e.AsUnit();
        if (!u || !u->alive || u->state == ActionState::Die)
            continue;
        if (u->orderQueue.empty())
        {
            if (u->orderX >= 0 && u->OrderFinished())
                u->orderX = u->orderY = -1; // arrived: no line any more
            continue;
        }
        if (u->owner >= kMaxPlayers || !u->OrderFinished() || m_squads[u->owner].IsSquadMember(u->id))
            continue;
        const Unit::QueuedOrder next = u->orderQueue.front();
        u->orderQueue.erase(u->orderQueue.begin());
        starting[{u->owner, next.kind, next.x, next.y}].push_back(u->id);
    }
    for (auto &[key, ids] : starting)
    {
        std::sort(ids.begin(), ids.end());
        OrderUnitsAt(std::get<0>(key), ids, std::get<1>(key), std::get<2>(key), std::get<3>(key), false, ctx);
    }
}

void Simulation::Apply(const SimCommand &cmd, SimContext &ctx)
{
    EntityManager &entities = ctx.entities;
    TileMap &map = ctx.map;
    Pathfinder &pathfinder = ctx.pathfinder;
    if (cmd.player >= kMaxPlayers)
        return;
    Economy &economy = EconomyOf(cmd.player, ctx);
    const Faction faction = Players::FactionOf(cmd.player);

    switch (cmd.type)
    {
        case CommandType::OrderAt:
        {
            std::vector<int> units = OwnUnits(cmd, ctx);
            const bool queued = (cmd.b & 2) != 0; // Shift: after the orders the units already have
            const bool asGroup = (cmd.b & 1) != 0;
            if (queued)
            {
                // Busy units keep their order and line this one up; idle ones start it now (alone, no march)
                std::vector<int> now;
                for (int id : units)
                {
                    Unit *u = entities.GetUnit(id);
                    if (!u)
                        continue;
                    if (u->orderQueue.empty() && u->OrderFinished() && !m_squads[cmd.player].IsSquadMember(id))
                        now.push_back(id);
                    else if (u->orderQueue.size() < kMaxQueuedOrders)
                        u->orderQueue.push_back({cmd.a, cmd.x, cmd.y});
                }
                if (!now.empty())
                    OrderUnitsAt(cmd.player, now, cmd.a, cmd.x, cmd.y, false, ctx);
                break;
            }
            m_squads[cmd.player].ReleaseUnits(units, entities);
            ClearOrders(units, entities);
            std::vector<int> immediateUnits;
            for (int id : units)
            {
                Unit *u = entities.GetUnit(id);
                if (!u)
                    continue;
                if (u->state == ActionState::Transform)
                {
                    if (u->orderQueue.size() < kMaxQueuedOrders)
                        u->orderQueue.push_back(Unit::QueuedOrder {cmd.a, cmd.x, cmd.y});
                    u->orderX = cmd.x;
                    u->orderY = cmd.y;
                }
                else
                {
                    immediateUnits.push_back(id);
                }
            }
            if (!immediateUnits.empty())
                OrderUnitsAt(cmd.player, immediateUnits, cmd.a, cmd.x, cmd.y, asGroup, ctx);
            break;
        }
        case CommandType::AttackWall:
        {
            const std::vector<int> units = OwnUnits(cmd, ctx);
            m_squads[cmd.player].ReleaseUnits(units, entities);
            ClearOrders(units, entities);
            if (!units.empty())
                entities.IssueAttackWallOrder(units, cmd.x, cmd.y, pathfinder);
            break;
        }
        case CommandType::SetRally:
            for (std::uint32_t id : cmd.ids)
            {
                Building *b = entities.GetBuilding(static_cast<int>(id));
                if (b && b->alive && b->IsUnitProducer() && b->owner == cmd.player)
                {
                    b->rallyTileX = std::clamp(cmd.x / TILE_WIDTH, 0, map.GetWidth() - 1);
                    b->rallyTileY = std::clamp(cmd.y / TILE_HEIGHT, 0, map.GetHeight() - 1);
                }
            }
            break;
        case CommandType::ToggleSiege:
        {
            const std::vector<int> units = OwnUnits(cmd, ctx);
            m_squads[cmd.player].ReleaseUnits(units, entities);
            ClearOrders(units, entities);
            entities.ToggleSiegeMode(units);
            break;
        }
        case CommandType::Stop:
            ClearOrders(OwnUnits(cmd, ctx), entities);
            for (int id : OwnUnits(cmd, ctx))
            {
                Unit *u = entities.GetUnit(id);
                if (!u)
                    continue;
                u->path.clear();
                u->pathIndex = 0;
                u->targetEntityId = 0;
                u->targetWallTileX = -1;
                u->targetWallTileY = -1;
                u->isAttackMoving = false;
                u->groupOrder = false;
                u->holdPosition = false;
                u->waitingForPath = false;
                u->state = ActionState::Idle;
            }
            break;
        case CommandType::HoldPosition:
            ClearOrders(OwnUnits(cmd, ctx), entities);
            for (int id : OwnUnits(cmd, ctx))
            {
                Unit *u = ctx.entities.GetUnit(id);
                if (!u)
                    continue;
                u->path.clear();
                u->pathIndex = 0;
                u->isAttackMoving = false;
                u->groupOrder = false;
                u->holdPosition = true;
                u->ClearMoveTarget();
                u->waitingForPath = false;
                u->state = ActionState::Idle;
            }
            break;
        case CommandType::SetStance:
            if (cmd.a < 0 || cmd.a >= static_cast<int>(UnitStance::Count))
                break;
            for (int id : OwnUnits(cmd, ctx))
            {
                Unit *u = ctx.entities.GetUnit(id);
                if (!u || u->stance == static_cast<UnitStance>(cmd.a))
                    continue;
                u->stance = static_cast<UnitStance>(cmd.a);
                u->stanceHomeX = u->stanceHomeY = -1;
                u->stanceReturning = false;
            }
            break;
        case CommandType::PlaceBuilding:
        {
            // cmd.a is the canonical blueprint slot; the building is the one of the ordering player's faction
            // (looking it up in the local player's menu gave the other side's buildings)
            const BuildingBlueprint *found = EntityManager::FindBlueprint(faction, cmd.a);
            if (!found)
                return;
            const auto &bp = *found;
            // The Radar Locator is built only once its technology is researched (the interface says so too)
            if (!IsBuildingUnlocked(cmd.player, bp.buildingIndex))
                return;
            if (!entities.CanPlaceBuilding(cmd.x, cmd.y, bp.width, bp.height, map, cmd.player))
                return;
            if (!economy.CanAfford(bp.creditCost))
                return;
            economy.SpendCredits(bp.creditCost);
            if (bp.isWall)
            {
                map.SetWall(cmd.x, cmd.y, faction, 100, cmd.player);
            }
            else if (entities.SpawnBuildingByIndex(bp.buildingIndex, cmd.x, cmd.y, faction, 0.0f, cmd.player) > 0)
            {
                for (int ty = cmd.y; ty < cmd.y + bp.height && ty < map.GetHeight(); ++ty)
                {
                    for (int tx = cmd.x; tx < cmd.x + bp.width && tx < map.GetWidth(); ++tx)
                        map.SetPassable(tx, ty, false);
                }
            }
            break;
        }
        case CommandType::Produce:
        {
            const Building *b = entities.GetBuilding(cmd.b);
            if (!Alive(b) || b->owner != cmd.player || !b->IsUnitProducer() || b->buildProgress < 1.0f)
                return;
            const auto *cfg = entities.GetUnitConfig(cmd.a);
            // A batch (Shift: 5, Ctrl: 10): as many as the credits and the queue allow
            const int count = std::clamp(cmd.c, 1, 20);
            for (int i = 0; i < count; ++i)
            {
                if (!economy.QueueUnit(
                        cmd.a, cfg ? cfg->creditCost : 100, cfg ? cfg->buildTime : 4.0f, cmd.b, entities))
                    break;
            }
            break;
        }
        case CommandType::SetRepeat:
        {
            Building *b = entities.GetBuilding(cmd.b);
            if (!Alive(b) || b->owner != cmd.player || !b->IsUnitProducer() || b->buildProgress < 1.0f)
                return;
            b->repeatArchetype = b->repeatArchetype == cmd.a ? -1 : cmd.a;
            break;
        }
        case CommandType::CancelProduce:
        {
            const Building *b = entities.GetBuilding(cmd.b);
            if (!b || b->owner != cmd.player)
                return;
            economy.CancelUnit(cmd.a, cmd.b, entities);
            break;
        }
        case CommandType::StartResearch:
        {
            ResearchProgress &r = m_research[cmd.player];
            if (cmd.a < 0 || cmd.a >= kResearchCount || r.active >= 0)
                return;
            if (!HasResearchBuilding(cmd.player, entities) || IsResearched(cmd.player, cmd.a)
                || !IsResearchAvailable(cmd.player, cmd.a))
            {
                return;
            }
            const ResearchDef &def = researchDef(faction, cmd.a);
            if (!economy.CanAfford(def.cost))
                return;
            economy.SpendCredits(def.cost);
            r.active = cmd.a;
            r.timer = 0.0f;
            r.duration = def.seconds;
            break;
        }
        case CommandType::Demolish:
        {
            int refund = 0;
            for (std::uint32_t id : cmd.ids)
            {
                const Building *b = entities.GetBuilding(static_cast<int>(id));
                if (Alive(b) && b->owner == cmd.player)
                    refund += entities.DemolishBuilding(b->id, map);
            }
            if (refund > 0)
                economy.AddCredits(refund);
            break;
        }
        case CommandType::ToggleRepair:
        {
            std::vector<Building *> own;
            bool anyIdle = false;
            for (std::uint32_t id : cmd.ids)
            {
                Building *b = entities.GetBuilding(static_cast<int>(id));
                if (Alive(b) && b->owner == cmd.player && b->rawBuildingType < 15 && b->CanBeRepaired())
                {
                    own.push_back(b);
                    anyIdle = anyIdle || !b->repairing;
                }
            }
            for (Building *b : own)
                b->repairing = anyIdle;
            break;
        }
        case CommandType::DebugCredits:
            if (cmd.debug)
                economy.AddCredits(cmd.a);
            break;
        case CommandType::DebugSpawn:
        {
            if (!cmd.debug)
                return;
            switch (static_cast<DebugSpawnKind>(cmd.b))
            {
                case DebugSpawnKind::Wall:
                    map.SetWall(cmd.x, cmd.y, faction, 100, cmd.player);
                    break;
                case DebugSpawnKind::Building:
                    if (entities.SpawnBuildingByIndex(cmd.a, cmd.x, cmd.y, faction, 1.0f, cmd.player) > 0)
                    {
                        for (int ty = cmd.y; ty < cmd.y + cmd.d && ty < map.GetHeight(); ++ty)
                        {
                            for (int tx = cmd.x; tx < cmd.x + cmd.c && tx < map.GetWidth(); ++tx)
                                map.SetPassable(tx, ty, false);
                        }
                    }
                    break;
                case DebugSpawnKind::Unit:
                    entities.SpawnUnitByArchetype(cmd.a, cmd.x, cmd.y, faction, cmd.player);
                    break;
            }
            break;
        }
        case CommandType::View:
            break; // the camera of a player: only for replays
        case CommandType::PlayerLeft:
            // Gone from the match: nothing of the player stays to be fought over (or left standing in the way)
            entities.DestroyPlayer(cmd.player, map);
            std::cout << "[Simulation] Player " << int(cmd.player) << " left the match: its forces are destroyed"
                      << std::endl;
            break;
        case CommandType::Count:
            break;
    }
}

void Simulation::UpdateResearch(float dt, SimContext &ctx)
{
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        ResearchProgress &r = m_research[p];
        // Research pauses while the Tech Center / Laboratory is down or without power
        if (r.active < 0 || r.active >= kResearchCount || !ResearchBuildingPowered(p, ctx.entities))
            continue;
        r.timer += dt;
        if (r.timer < r.duration)
            continue;
        CompleteResearch(p, r.active, ctx.entities);
        m_events.push_back({SimEvent::Type::ResearchCompleted, p, r.active});
        r.active = -1;
        r.timer = 0.0f;
    }
    // The labs of a researching player animate (the computer players set theirs in AIController::Update, after this)
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
        SetResearchAnimation(p, m_research[p].active >= 0 && ResearchBuildingPowered(p, ctx.entities), ctx.entities);
}

void Simulation::UpdateSquads(float dt, SimContext &ctx, const ChokepointMap &chokepoints)
{
    for (auto &squads : m_squads)
    {
        squads.Update(dt, ctx.map, ctx.entities, ctx.pathfinder, chokepoints, GroupTactics());
        squads.TakeReleasedUnits();
    }
}

std::vector<SimEvent> Simulation::TakeEvents()
{
    std::vector<SimEvent> out;
    out.swap(m_events);
    return out;
}

std::uint64_t Simulation::Checksum(const SimContext &ctx) const
{
    std::uint64_t sections[kChecksumSections];
    return ChecksumSections(ctx, sections);
}

std::uint64_t Simulation::ChecksumSections(const SimContext &ctx, std::uint64_t (&sections)[kChecksumSections]) const
{
    Hasher h;
    h.I32(static_cast<std::int32_t>(m_tick));
    for (const Entity &e : ctx.entities.GetAllEntities())
    {
        h.I32(e.id);
        h.I32(static_cast<std::int32_t>(e.type));
        h.I32(static_cast<std::int32_t>(e.faction));
        h.I32(e.owner);
        h.I32(e.alive ? 1 : 0);
        h.I32(static_cast<std::int32_t>(e.state));
        h.I32(e.tileX);
        h.I32(e.tileY);
        h.F32(e.subX);
        h.F32(e.subY);
        h.I32(e.currentHp);
        h.I32(e.targetEntityId);
        h.I32(e.targetWallTileX);
        h.I32(e.targetWallTileY);
        h.I32(e.lastAttackerId);
        if (const Building *b = e.AsBuilding())
            h.I32(b->repairing ? 1 : 0);
        if (const Building *b = e.AsBuilding())
            h.I32(b->repeatArchetype);
        if (const Unit *u = e.AsUnit())
        {
            h.I32(u->orderX);
            h.I32(u->orderY);
            h.I32(static_cast<std::int32_t>(u->stance));
            h.I32(u->stanceHomeX);
            h.I32(u->stanceHomeY);
            h.I32(u->stanceReturning ? 1 : 0);
            h.I32(u->orderedTargetId);
            h.I32(static_cast<std::int32_t>(u->orderQueue.size()));
            for (const Unit::QueuedOrder &q : u->orderQueue)
            {
                h.I32(q.kind);
                h.I32(q.x);
                h.I32(q.y);
            }
        }
    }
    sections[0] = h.h;
    // Credits in player order (the local player's economy is SimContext::economy on every peer, so the
    // order must not depend on it)
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        if ((m_economyMask >> p) & 1u)
            h.I32(m_economies[p].GetCredits());
        else if (p == ctx.economy.player())
            h.I32(ctx.economy.GetCredits());
    }
    sections[1] = h.h;
    const TileMap &map = ctx.map;
    for (int y = 0; y < map.GetHeight(); ++y)
    {
        for (int x = 0; x < map.GetWidth(); ++x)
        {
            const int wall = map.GetWall(x, y);
            if (wall > 0)
            {
                h.I32(x);
                h.I32(y);
                h.I32(wall);
                h.I32(map.GetWallOwner(x, y));
            }
        }
    }
    sections[2] = h.h;
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        for (int i = 0; i < kResearchCount; ++i)
            h.I32(IsResearched(p, i) ? 1 : 0);
    }
    sections[3] = h.h;
    return h.h;
}

} // namespace OpenArtOfWar
