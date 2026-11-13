// WorldSnapshot: encode / decode round trip, version and damage checks (TASKNEW.md step 2)
#include "game/EntityManager.hpp"
#include "game/Players.hpp"
#include "game/Pathfinder.hpp"
#include "game/TileMap.hpp"
#include "game/Unit.hpp"
#include "game/Building.hpp"
#include "net/WorldSnapshot.hpp"

#include <cstdio>
#include <limits>

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

WorldSnapshot Sample()
{
    WorldSnapshot s;
    s.tick = 4242;
    s.baseTick = 4240;
    s.full = false;
    EntitySnap unit;
    unit.id = 4;
    unit.type = 8;
    unit.archetype = 3;
    unit.faction = 1;
    unit.owner = 2;
    unit.tileX = 49;
    unit.tileY = 44;
    unit.subX = -4.125f;
    unit.subY = 2.75f;
    unit.hp = 25;
    unit.maxHp = 30;
    unit.state = 1;
    unit.facing = 3;
    unit.turret = 5;
    unit.target = 11;
    unit.orderX = 1252;
    unit.orderY = 840;
    unit.siege = true;
    EntitySnap building;
    building.id = 9;
    building.isBuilding = true;
    building.type = 17;
    building.rawBuilding = 2;
    building.tileX = 56;
    building.tileY = 54;
    building.hp = 1200;
    building.maxHp = 1200;
    building.buildPermille = 640;
    building.powered = false;
    building.producing = true;
    building.rallyX = 3;
    building.rallyY = 4;
    building.queue = {{1, 100}, {7, 0}};
    s.entities = {unit, building};
    s.removed = {2, 3, 8};
    s.hiddenAlive = {12, 14};
    s.economies = {{0, 1050, 150, 40}, {1, 900, 150, 20}};
    s.research = {{0, 5, 3, 2.5f, 30.0f}, {1, 0, -1, 0.0f, 20.0f}};
    s.mines = {{7, 5, 6, 1, 0, 0.5f}};
    s.result = 2;
    s.hasWalls = true;
    s.walls = {{5, 6, 1, 100, 0}, {5, 7, 1, 40, 0}};
    s.projectiles = {{1, 120.5f, 80.25f, 3, 17, 1, 0}};
    s.cues = {{2, 10.0f, 20.0f, 1, "rifle_shot", 0.75f}, {0, 1.0f, 2.0f, 255, "", 1.0f}};
    s.blasts = {{300.0f, 400.0f, 1.5f}};
    s.explosions = {{50.0f, 60.0f, 12, 0.25f}};
    s.groundMarks = {{10.0f, 20.0f, 1784, 30.0f}};
    s.deaths = {{4, 49, 44, -4.125f, 2.75f, 12.5f, 247, 1741, 5, true}};
    return s;
}
} // namespace

int main()
{
    const WorldSnapshot s = Sample();
    const std::vector<std::uint8_t> bytes = s.Encode();
    Check(!bytes.empty(), "encodes");

    WorldSnapshot back;
    Check(WorldSnapshot::Decode(bytes.data(), bytes.size(), back), "decodes");
    Check(back == s, "round trip equals");
    Check(back.Encode() == bytes, "decode then encode is byte-identical");

    WorldSnapshot base = s;
    base.full = true;
    base.tick = 1;
    WorldSnapshot next = base;
    next.tick = 2;
    Check(next.DeltaFrom(base).research.empty(), "unchanged research omitted from delta");
    next.research[0].timer += 1.0f;
    WorldSnapshot researchDelta = next.DeltaFrom(base);
    Check(researchDelta.research == next.research && researchDelta.ExpandFrom(base)
              && researchDelta.research == next.research, "changed research survives delta expansion");
    next = base;
    next.tick = 20;
    Check(next.DeltaFrom(base).research == next.research, "research repeated once a second");

    std::vector<std::uint8_t> other = bytes;
    other[0] = static_cast<std::uint8_t>(WorldSnapshot::kVersion + 1);
    WorldSnapshot refused;
    Check(!WorldSnapshot::Decode(other.data(), other.size(), refused), "another version is refused");

    for (std::size_t cut : {std::size_t(0), std::size_t(1), bytes.size() / 2, bytes.size() - 1})
    {
        WorldSnapshot t;
        Check(!WorldSnapshot::Decode(bytes.data(), cut, t), "truncated buffer is refused");
    }
    std::vector<std::uint8_t> longer = bytes;
    longer.push_back(0);
    WorldSnapshot t2;
    Check(!WorldSnapshot::Decode(longer.data(), longer.size(), t2), "trailing bytes are refused");

    {
        WorldSnapshot invalid = Sample();
        invalid.research[0].timer = std::numeric_limits<float>::quiet_NaN();
        const auto bad = invalid.Encode();
        WorldSnapshot untouched = Sample();
        Check(!WorldSnapshot::Decode(bad.data(), bad.size(), untouched) && untouched == Sample(),
              "non-finite research data is refused without changing output");
        invalid = Sample();
        invalid.projectiles[0].x = std::numeric_limits<float>::infinity();
        const auto badShot = invalid.Encode();
        Check(!WorldSnapshot::Decode(badShot.data(), badShot.size(), untouched), "non-finite projectile is refused");
        invalid = Sample();
        invalid.entities[0].id = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
        const auto badId = invalid.Encode();
        Check(!WorldSnapshot::Decode(badId.data(), badId.size(), untouched), "overflowing entity id is refused");

        auto expectRefused = [&](const char *label, auto mutate) {
            WorldSnapshot badSnap = Sample();
            mutate(badSnap);
            const auto encoded = badSnap.Encode();
            WorldSnapshot dest = Sample();
            Check(!WorldSnapshot::Decode(encoded.data(), encoded.size(), dest) && dest == Sample(), label);
        };
        expectRefused("unsorted entity ids are refused", [](WorldSnapshot &w) { std::swap(w.entities[0], w.entities[1]); });
        expectRefused("invalid entity faction is refused", [](WorldSnapshot &w) { w.entities[0].faction = 3; });
        expectRefused("invalid entity owner is refused", [](WorldSnapshot &w) { w.entities[0].owner = 8; });
        expectRefused("invalid entity archetype is refused", [](WorldSnapshot &w) { w.entities[0].archetype = 99; });
        expectRefused("invalid building index is refused", [](WorldSnapshot &w) { w.entities[1].rawBuilding = 15; });
        expectRefused("invalid production permille is refused", [](WorldSnapshot &w) { w.entities[1].queue[0].progressPermille = 1001; });
        expectRefused("unsorted removed ids are refused", [](WorldSnapshot &w) { w.removed = {3, 2}; });
        expectRefused("duplicate hiddenAlive ids are refused", [](WorldSnapshot &w) { w.hiddenAlive = {12, 12}; });
        expectRefused("duplicate economy player is refused", [](WorldSnapshot &w) { w.economies.push_back(w.economies[0]); });
        expectRefused("invalid research index is refused", [](WorldSnapshot &w) { w.research[0].active = 24; });
        expectRefused("zero research duration is refused", [](WorldSnapshot &w) { w.research[0].duration = 0.0f; });
        expectRefused("negative mine arm time is refused", [](WorldSnapshot &w) { w.mines[0].armLeft = -0.1f; });
        expectRefused("zero wall hpPercent is refused", [](WorldSnapshot &w) { w.walls[0].hpPercent = 0; });
        expectRefused("invalid projectile type is refused", [](WorldSnapshot &w) { w.projectiles[0].type = 9; });
        expectRefused("negative sound gain is refused", [](WorldSnapshot &w) { w.cues[0].gain = -1.0f; });
        expectRefused("non-positive blast size is refused", [](WorldSnapshot &w) { w.blasts[0].size = 0.0f; });
        expectRefused("negative explosion delay is refused", [](WorldSnapshot &w) { w.explosions[0].delay = -0.5f; });
        expectRefused("negative death timer is refused", [](WorldSnapshot &w) { w.deaths[0].timer = -1.0f; });
        expectRefused("non-positive groundMark duration is refused", [](WorldSnapshot &w) { w.groundMarks[0].seconds = 0.0f; });
    }

    // Completed projectiles must not accumulate on the authority between snapshots.
    {
        EntityManager shots;
        WorldSnapshot snapshot;
        snapshot.projectiles = {{0, 120.0f, 80.0f, 0, 0, 0, 0}, {0, 180.0f, 80.0f, 0, 0, 0, 0}};
        Check(shots.ApplySnapshot(snapshot) && shots.GetProjectiles().size() == 2, "projectile test starts with shots");
        TileMap map;
        map.InitBlankArena(30, 30);
        Pathfinder pathfinder;
        pathfinder.SetMap(&map);
        pathfinder.SetEntityManager(&shots);
        shots.Update(0.05f, map, pathfinder);
        Check(shots.GetProjectiles().empty(), "completed projectiles are removed after update");
    }

    // A real world (needs ./assets): capture, round trip, the same world captures the same bytes
    {
        Players::SetupSinglePlayer(Faction::Resistance);
        EntityManager entities;
        entities.LoadCharacterConfigs();
        const int a = entities.SpawnUnit(UnitType::ResistanceRifleman, 10, 12, Faction::Resistance);
        const int b = entities.SpawnBuilding(UnitType::Barracks, 20, 22, Faction::Resistance);
        const WorldSnapshot w = WorldSnapshot::CaptureEntities(entities, 7);
        Check(w.entities.size() == 2 && w.entities[0].id == static_cast<std::uint32_t>(a)
                  && w.entities[1].id == static_cast<std::uint32_t>(b) && w.entities[1].isBuilding,
              "capture lists the spawned entities by id");
        const std::vector<std::uint8_t> wb = w.Encode();
        WorldSnapshot wback;
        Check(WorldSnapshot::Decode(wb.data(), wb.size(), wback) && wback == w, "captured world round trip");
        Check(WorldSnapshot::CaptureEntities(entities, 7).Encode() == wb, "same world, same bytes");

        // A replica world fed with the snapshot looks the same, also after the authority's units moved and died
        EntityManager replica;
        replica.LoadCharacterConfigs();
        Check(replica.ApplySnapshot(w), "snapshot applies to an empty world");
        Check(WorldSnapshot::CaptureEntities(replica, 7).Encode() == wb, "replica captures the same bytes");
        Unit *mover = entities.GetUnit(a);
        mover->tileX = 14;
        mover->subX = 3.5f;
        mover->currentHp = 5;
        entities.GetBuilding(b)->buildProgress = 0.5f;
        const WorldSnapshot w2 = WorldSnapshot::CaptureEntities(entities, 8);
        const WorldSnapshot delta = w2.DeltaFrom(w);
        WorldSnapshot deltaBack;
        const auto deltaBytes = delta.Encode();
        Check(!delta.full && delta.baseTick == w.tick && WorldSnapshot::Decode(deltaBytes.data(), deltaBytes.size(), deltaBack)
                  && deltaBack == delta, "delta round trip and base tick");
        WorldSnapshot expanded = deltaBack;
        Check(expanded.ExpandFrom(w) && expanded.entities == w2.entities, "delta expands against acknowledged base");
        WorldSnapshot wrongBase = w;
        ++wrongBase.tick;
        Check(!deltaBack.ExpandFrom(wrongBase), "delta rejects a different base");
        WorldSnapshot reverted = w;
        reverted.tick = 10;
        reverted.cues = {{2, 10.0f, 20.0f, 1, "shot", 1.0f}};
        WorldSnapshot revertedDelta = reverted.DeltaFrom(w);
        Check(revertedDelta.entities.empty() && revertedDelta.cues == reverted.cues, "unchanged entities omitted while cues are retained");
        Check(revertedDelta.ExpandFrom(w) && revertedDelta.entities == w.entities, "delta restores state relative to its base");
        Check(replica.ApplySnapshot(deltaBack), "delta applies over base");
        Check(WorldSnapshot::CaptureEntities(replica, 8) == w2, "delta updates the replica");
        Check(replica.ApplySnapshot(w2), "second snapshot applies");
        Check(WorldSnapshot::CaptureEntities(replica, 8) == w2, "replica follows the authority");
        mover->state = ActionState::Die;
        mover->deathTimer = 10.0f;
        mover->selectedDeathSeq = 247;
        mover->killedByFlame = true;
        WorldSnapshot capturedDeath;
        capturedDeath.CaptureEffects(entities);
        Check(capturedDeath.deaths.size() == 1 && capturedDeath.deaths[0].id == static_cast<std::uint32_t>(a)
                  && capturedDeath.deaths[0].flame && capturedDeath.deaths[0].sequence == 247, "authority captures death");
        capturedDeath.CaptureEffects(entities);
        Check(capturedDeath.deaths.empty(), "death is collected once");
        EntityManager deathReplica;
        deathReplica.LoadCharacterConfigs();
        Check(deathReplica.ApplySnapshot(w2), "death replica starts visible");
        WorldSnapshot death = w2;
        death.entities.erase(death.entities.begin());
        death.deaths = {{static_cast<std::uint32_t>(a), 14, 12, 3.5f, 0.0f, 10.0f, 247, -1, 5, true}};
        WorldSnapshot deathDelta = death.DeltaFrom(w2);
        Check(deathDelta.deaths == death.deaths && deathDelta.ExpandFrom(w2), "death event survives delta");
        Check(deathReplica.ApplySnapshot(deathDelta), "death snapshot applies");
        auto FindRemains = [&]() -> Unit * {
            for (Entity &e : deathReplica.GetAllEntities())
                if (e.id == a)
                    return e.AsUnit();
            return nullptr;
        };
        Unit *remains = FindRemains();
        Check(remains && remains->state == ActionState::Die && remains->killedByFlame
                  && remains->selectedDeathSeq == 247, "death creates matching remains");
        deathReplica.UpdateVisualEffects(0.16f);
        Check(remains && remains->deathTimer < 10.0f && remains->animFrame > 0, "replica death animates");
        Check(deathReplica.ApplySnapshot(death) && FindRemains(), "later full snapshot preserves remains");
        deathReplica.UpdateVisualEffects(1000.0f);
        Check(!FindRemains(), "remains expire");
        WorldSnapshot gone = w2;
        gone.tick = 9;
        gone.entities.erase(gone.entities.begin());
        const WorldSnapshot goneDelta = gone.DeltaFrom(w2);
        Check(goneDelta.removed.size() == 1 && goneDelta.removed[0] == static_cast<std::uint32_t>(a), "delta lists removed id");
        Check(replica.ApplySnapshot(goneDelta) && replica.GetUnit(a) == nullptr, "delta removes a unit");
        replica.ApplySnapshot(gone);
        Check(replica.GetUnit(a) == nullptr, "a unit missing from a full snapshot is gone");
    }

    {
        std::array<PlayerSlot, kMaxPlayers> slots {};
        slots[0] = {true, Faction::Resistance, 0, 0, true};
        slots[1] = {true, Faction::Confederation, 1, 0, true};
        slots[2] = {true, Faction::Resistance, 0, 0, true};
        Players::Setup(slots, 0);
        TileMap map;
        map.InitBlankArena(64, 64);
        map.SetFogEnabled(true);
        EntityManager world;
        world.LoadCharacterConfigs();
        const int own = world.SpawnUnit(UnitType::ResistanceRifleman, 10, 10, Faction::Resistance);
        const int ally = world.SpawnUnit(UnitType::ResistanceRifleman, 50, 50, Faction::Resistance);
        const int enemy = world.SpawnUnit(UnitType::ResistanceRifleman, 40, 40, Faction::Resistance);
        world.GetEntity(ally)->owner = 2;
        world.GetEntity(enemy)->owner = 1;
        world.UpdateFogOfWar(map);
        WorldSnapshot filtered = WorldSnapshot::CaptureEntities(world, 1);
        filtered.economies = {{0, 1, 2, 3}, {1, 4, 5, 6}, {2, 7, 8, 9}};
        filtered.research = {{0, 1, -1, 0, 20}, {1, 2, -1, 0, 20}, {2, 4, -1, 0, 20}};
        filtered.FilterForPlayer(world, map, 0);
        Check(filtered.entities.size() == 2 && filtered.hiddenAlive.size() == 1 && filtered.hiddenAlive[0] == static_cast<std::uint32_t>(enemy), "fog hides enemy and keeps ally");
        Check(filtered.economies.size() == 2, "fog excludes enemy economy");
        Check(filtered.research.size() == 2 && filtered.research[1].player == 2, "research excludes enemies");
        map.RevealRect(40, 40, 1, 1, 0);
        WorldSnapshot visible = WorldSnapshot::CaptureEntities(world, 2);
        visible.FilterForPlayer(world, map, 0);
        Check(visible.entities.size() == 3, "visible enemy is sent");
        EntityManager replica;
        replica.LoadCharacterConfigs();
        Check(replica.ApplySnapshot(visible), "visible world applies");
        Check(replica.ApplySnapshot(filtered) && replica.GetEntity(enemy), "full snapshot preserves hidden living entity");
        filtered.hiddenAlive.clear();
        filtered.deaths = {{static_cast<std::uint32_t>(enemy), 40, 40, 0, 0, 10, 247, -1, 5, false}};
        Check(replica.ApplySnapshot(filtered) && !replica.GetEntity(enemy), "dead hidden entity is removed");
        Check(replica.GetAllEntities().size() == 2, "hidden death does not create remains");
        map.SetFogEnabled(false);
        WorldSnapshot clear = WorldSnapshot::CaptureEntities(world, 3);
        clear.FilterForPlayer(world, map, 0);
        Check(clear.entities.size() == 3 && clear.hiddenAlive.empty() && world.GetEntity(own), "fog disabled sends all entities");
    }

    // Team-based match result in FilterForPlayer (allies + 3+ teams)
    {
        std::array<PlayerSlot, kMaxPlayers> slots {};
        slots[0] = {true, Faction::Resistance, 0, 0, true};
        slots[1] = {true, Faction::Resistance, 0, 0, true};    // ally of 0
        slots[2] = {true, Faction::Confederation, 1, 0, true}; // team 1
        slots[3] = {true, Faction::Confederation, 2, 0, true}; // team 2
        Players::Setup(slots, 0);
        TileMap map;
        map.InitBlankArena(32, 32);
        map.SetFogEnabled(false);
        EntityManager world;
        world.LoadCharacterConfigs();
        const int b1 = world.SpawnBuilding(UnitType::Headquarters, 6, 6, Faction::Resistance);
        const int b2 = world.SpawnBuilding(UnitType::Headquarters, 14, 14, Faction::Confederation);
        const int b3 = world.SpawnBuilding(UnitType::Headquarters, 22, 22, Faction::Confederation);
        world.GetEntity(b1)->owner = 1;
        world.GetEntity(b2)->owner = 2;
        world.GetEntity(b3)->owner = 3;
        // Player 0 has no buildings -> eliminated individually, but teammate 1 keeps team 0 alive.
        Check(world.UpdateElimination(map).size() == 1 && world.IsEliminated(0), "player 0 eliminated individually");
        WorldSnapshot s0 = WorldSnapshot::CaptureEntities(world, 1);
        WorldSnapshot s2 = s0;
        s0.FilterForPlayer(world, map, 0);
        s2.FilterForPlayer(world, map, 2);
        Check(s0.result == 0, "eliminated player stays in match while ally lives");
        Check(s2.result == 0, "hostile team 1 stays in match");
        world.GetEntity(b1)->alive = false;
        world.UpdateElimination(map);
        WorldSnapshot afterTeam0Out = WorldSnapshot::CaptureEntities(world, 2);
        WorldSnapshot a0 = afterTeam0Out, a2 = afterTeam0Out, a3 = afterTeam0Out;
        a0.FilterForPlayer(world, map, 0);
        a2.FilterForPlayer(world, map, 2);
        a3.FilterForPlayer(world, map, 3);
        Check(a0.result == 2, "defeated team gets defeat result");
        Check(a2.result == 0 && a3.result == 0, "remaining two enemy teams continue after team 0 falls");
        world.GetEntity(b2)->alive = false;
        world.UpdateElimination(map);
        WorldSnapshot end2 = WorldSnapshot::CaptureEntities(world, 3);
        WorldSnapshot end3 = end2;
        end2.FilterForPlayer(world, map, 2);
        end3.FilterForPlayer(world, map, 3);
        Check(end2.result == 2 && end3.result == 1, "last standing team receives victory");
    }

    if (failures == 0)
        std::printf("world snapshot tests: ok (%zu bytes)\n", bytes.size());
    return failures == 0 ? 0 : 1;
}
