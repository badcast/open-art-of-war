// Orders of a single unit: hold position, stop, a plain move under fire, the firing pose that ends with its target
#include "core/AssetStore.hpp"
#include "core/Config.hpp"
#include "core/LoaderResources.hpp"
#include "game/AIController.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/Pathfinder.hpp"
#include "game/Simulation.hpp"
#include "game/TileMap.hpp"

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

constexpr Faction kMine = Faction::Resistance, kTheirs = Faction::Confederation;

struct World
{
    TileMap map;
    EntityManager entities;
    Pathfinder pf;
    Economy economy;
    AIController ai;
    Simulation sim;
    SimContext ctx {entities, map, pf, economy, ai};

    World()
    {
        Players::SetupSinglePlayer(kMine);
        map.InitBlankArena(60, 40);
        map.SetFogEnabled(false);
        entities.LoadCharacterConfigs();
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
        economy.SetPlayer(0);
        ai.SetPlayer(1);
        sim.Reset(1u);
    }
    int Mine(int archetype, int x, int y)
    {
        return entities.SpawnUnitByArchetype(archetype, x, y, kMine);
    }
    int Theirs(int archetype, int x, int y)
    {
        return entities.SpawnUnitByArchetype(archetype, x, y, kTheirs, 1);
    }
    void Tick(int count = 1)
    {
        for (int i = 0; i < count; ++i)
        {
            sim.ApplyCommands(ctx);
            entities.Update(SIMULATION_TIMESTEP, map, pf);
            sim.UpdateOrderQueues(ctx);
            sim.UpdateSquads(SIMULATION_TIMESTEP, ctx, ai.GetChokepoints());
            sim.EndTick();
        }
    }
    void Command(CommandType type, int id, int tileX = 0, int tileY = 0, OrderKind kind = OrderKind::Move)
    {
        SimCommand cmd;
        cmd.type = type;
        cmd.player = 0;
        cmd.ids = {static_cast<std::uint32_t>(id)};
        cmd.x = tileX * TILE_WIDTH + TILE_WIDTH / 2;
        cmd.y = tileY * TILE_HEIGHT + TILE_HEIGHT / 2;
        cmd.a = static_cast<std::int32_t>(kind);
        sim.Schedule(sim.Tick(), cmd);
    }
};

bool Alive(const Unit *u)
{
    return u && u->alive && u->state != ActionState::Die;
}
} // namespace

int main()
{
    if (!Assets::Init() || !LoaderResources::Init())
    {
        std::printf("FAIL: no assets\n");
        return 1;
    }

    // a. Hold position: a shooter outside the holder's range hits it, then leaves; the holder never walks off
    {
        World w;
        const int holder = w.Mine(0, 20, 20);
        // the enemy archetype with the longest weapon range
        int best = 0;
        float bestRange = 0.0f;
        for (int arch = 0; arch < 16; ++arch)
        {
            World probe;
            const int id = probe.Theirs(arch, 5, 5);
            const Unit *u = id > 0 ? probe.entities.GetUnit(id) : nullptr;
            if (u && u->attackDamage > 0 && u->attackRange > bestRange)
            {
                bestRange = u->attackRange;
                best = arch;
            }
        }
        const Unit *h = w.entities.GetUnit(holder);
        Check(h != nullptr && bestRange > h->attackRange, "a longer-ranged enemy archetype exists");
        const int gap = h ? static_cast<int>(h->attackRange) + 1 : 6;
        const int foe = w.Theirs(best, 20 + gap, 20);
        w.Command(CommandType::HoldPosition, holder);
        w.Tick(2);
        const int startX = h->tileX, startY = h->tileY;
        Check(h->HoldsGround(), "the hold order is on");
        w.entities.GetUnit(foe)->targetEntityId = holder;
        w.Tick(40);
        w.Command(CommandType::OrderAt, foe, 50, 35);
        w.Tick(200);
        Check(h->currentHp < h->maxHp, "the holder was shot from outside its range");
        Check(h->tileX == startX && h->tileY == startY, "a holder does not leave its tile");
    }

    // b. Stop: a unit on the move ends up without a goal, a path and a state of movement
    {
        World w;
        const int id = w.Mine(0, 10, 10);
        w.Command(CommandType::OrderAt, id, 40, 30);
        w.Tick(10);
        const Unit *u = w.entities.GetUnit(id);
        Check(u->HasMoveTarget(), "a moving unit has a goal");
        w.Command(CommandType::Stop, id);
        w.Tick(3);
        Check(!u->HasMoveTarget(), "stop: no move target");
        Check(u->path.empty(), "stop: no path");
        Check(u->state == ActionState::Idle, "stop: idle");
        w.Tick(10); // (a step in progress ends on its next tile)
        const int x = u->tileX, y = u->tileY;
        w.Tick(40);
        Check(u->tileX == x && u->tileY == y, "stop: the unit stays on its tile");
    }

    // c. A plain move past an enemy within the weapon range: the unit arrives and has shot on the way
    {
        World w;
        const int id = w.Mine(0, 10, 20);
        const int foe = w.Theirs(0, 30, 21);
        const Unit *u = w.entities.GetUnit(id);
        w.entities.GetUnit(foe)->attackDamage = 0; // the foe is a target only: the mover must not die on the way
        const int hpBefore = w.entities.GetUnit(foe)->currentHp;
        w.entities.GetUnit(foe)->maxHp = w.entities.GetUnit(foe)->currentHp = 100000;
        w.Command(CommandType::OrderAt, id, 50, 20);
        int arrived = -1;
        for (int tick = 0; tick < 600 && arrived < 0; ++tick)
        {
            w.Tick();
            if (std::abs(u->tileX - 50) <= 1 && std::abs(u->tileY - 20) <= 1 && u->state != ActionState::Move)
                arrived = tick;
        }
        Check(arrived >= 0, "a plain move reaches its destination past an enemy");
        Check(w.entities.GetUnit(foe)->currentHp < 100000, "the mover fired on the move");
        (void)hpBefore;
    }

    // d. A firing pose with no target left ends
    {
        World w;
        const int id = w.Mine(0, 20, 20);
        const int foe = w.Theirs(0, 22, 20);
        Unit *u = w.entities.GetUnit(id);
        w.entities.GetUnit(foe)->attackDamage = 0;
        u->state = ActionState::Attack;
        u->targetEntityId = foe;
        u->attackAnimTimer = 0.0f;
        w.Tick(1);
        u->targetEntityId = 0;
        u->orderedTargetId = 0;
        u->state = ActionState::Attack;
        u->attackAnimTimer = 0.0f;
        w.entities.GetUnit(foe)->alive = false; // nothing else to shoot at either
        w.Tick(3);
        Check(u->state != ActionState::Attack, "an attack pose with no target ends");
    }

    // e. Wave size: skipped, see the report (the AI wave scaffolding is tied to AIDefenseTests internals)

    if (failures == 0)
        std::printf("All unit order tests passed\n");
    return failures == 0 ? 0 : 1;
}
