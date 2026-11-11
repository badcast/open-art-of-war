// Determinism of the simulation with computer players: the same skirmish (two computer players building, training and
// attacking each other) is played twice in one process and must give the same world checksum on every tick.
// Between the two games things that must not matter are changed: leftover heap allocations (other addresses), the
// generator state the first game left behind. A second game in the same process also catches state
// that is not reset between games (statics, caches). On a mismatch the first differing tick, the checksum part and
// the first differing line of the two world dumps are printed.
#include "core/Utility.hpp"
#include "game/AIController.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/Pathfinder.hpp"
#include "game/Players.hpp"
#include "game/Simulation.hpp"
#include "game/TileMap.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace OpenArtOfWar;

namespace
{
constexpr std::uint32_t kSeed = 0x5eed1234u;
constexpr int kTicks = 20 * 60 * 4; // four minutes of game time: bases, production, the first waves and fights
constexpr int kMapW = 90, kMapH = 70;

struct Recording
{
    std::vector<std::uint64_t> checksums;
    std::vector<std::array<std::uint64_t, Simulation::kChecksumSections>> sections;
    int unitsAtEnd {0};
    int deadAtEnd {0};
};

// One game as one peer sees it; `dumpTick` >= 0: also returns the world dump of that tick. `peer` changes what
// differs between the computers of a network game but must not reach the checksum: the idle mission AI is set up
// from the local side
Recording Play(PlayerId local, int peer, int dumpTick, std::string *dump)
{
    std::array<PlayerSlot, kMaxPlayers> slots {};
    slots[0].active = true;
    slots[0].faction = Faction::Resistance;
    slots[0].team = 0;
    slots[0].aiLevel = 2;
    slots[1].active = true;
    slots[1].faction = Faction::Confederation;
    slots[1].team = 1;
    slots[1].aiLevel = 2;
    Players::Setup(slots, local);

    TileMap map;
    map.InitBlankArena(kMapW, kMapH);
    map.SetFogEnabled(true);
    EntityManager entities;
    entities.LoadCharacterConfigs();
    Pathfinder pf;
    pf.SetMap(&map);
    pf.SetEntityManager(&entities);
    Economy economy;
    economy.SetPlayer(local);
    AIController missionAi; // (idle in a match: the match's computer players count)
    missionAi.SetPlayer(static_cast<PlayerId>(1 - peer));
    missionAi.Init(peer == 0 ? 74 : 10, peer == 0 ? 54 : 10, peer == 0 ? 10 : 74, peer == 0 ? 10 : 54);
    Simulation sim;
    sim.Reset(kSeed);

    const int hq[2][2] = {{10, 10}, {74, 54}};
    std::vector<std::unique_ptr<AIController>> ais;
    for (PlayerId p = 0; p < 2; ++p)
    {
        const Faction f = slots[p].faction;
        entities.SpawnBuildingByIndex(f == Faction::Resistance ? 8 : 0, hq[p][0], hq[p][1], f, 1.0f, p);
        for (int i = 0; i < 4; ++i)
            entities.SpawnUnitByArchetype(f == Faction::Resistance ? 0 : 1, hq[p][0] + 4 + i, hq[p][1] + 5, f, p);
    }
    for (PlayerId p = 0; p < 2; ++p)
    {
        auto ai = std::make_unique<AIController>();
        ai->SetPlayer(p);
        ai->SetDifficulty(AIDifficulty::Hard);
        ai->Init(hq[p][0], hq[p][1], hq[1 - p][0], hq[1 - p][1]);
        ai->SetSeed(kSeed ^ (static_cast<std::uint32_t>(p) + 1u) * 0x9E3779B9u);
        ai->RecordStartingBase(entities, map);
        ai->SetSkirmish(true);
        ais.push_back(std::move(ai));
    }
    std::vector<const AIController *> players;
    for (const auto &ai : ais)
        players.push_back(ai.get());
    sim.SetComputerPlayers(players, true);
    SimContext ctx {entities, map, pf, economy, missionAi};

    Recording rec;
    for (int tick = 0; tick < kTicks; ++tick)
    {
        // The order of GameplayController::SimulationStep
        sim.ApplyCommands(ctx);
        sim.UpdateResearch(SIMULATION_TIMESTEP, ctx);
        entities.Update(SIMULATION_TIMESTEP, map, pf);
        for (PlayerId out : entities.UpdateElimination(map))
            sim.NoteEvent({SimEvent::Type::PlayerEliminated, out, 0});
        sim.UpdateOrderQueues(ctx);
        sim.UpdateSquads(SIMULATION_TIMESTEP, ctx, missionAi.GetChokepoints());
        sim.UpdateEconomies(SIMULATION_TIMESTEP, ctx);
        for (auto &ai : ais)
        {
            if (!entities.IsEliminated(ai->player()))
                ai->Update(SIMULATION_TIMESTEP, map, entities, pf);
        }
        sim.EndTick();
        std::array<std::uint64_t, Simulation::kChecksumSections> parts {};
        std::uint64_t raw[Simulation::kChecksumSections];
        rec.checksums.push_back(sim.ChecksumSections(ctx, raw));
        for (int i = 0; i < Simulation::kChecksumSections; ++i)
            parts[static_cast<std::size_t>(i)] = raw[i];
        rec.sections.push_back(parts);
        if (tick == dumpTick && dump)
            *dump = sim.DumpState(ctx);
    }
    for (const Entity &e : entities.GetAllEntities())
    {
        if (e.isBuilding)
            continue;
        if (e.alive && e.state != ActionState::Die)
            ++rec.unitsAtEnd;
        else
            ++rec.deadAtEnd;
    }
    return rec;
}

std::string FirstDifferentLine(const std::string &a, const std::string &b)
{
    std::istringstream sa(a), sb(b);
    std::string la, lb;
    int n = 0;
    while (true)
    {
        const bool ga = static_cast<bool>(std::getline(sa, la));
        const bool gb = static_cast<bool>(std::getline(sb, lb));
        ++n;
        if (ga && gb && !la.empty() && la[0] == '#' && !lb.empty() && lb[0] == '#')
            continue; // headers and part totals carry the checksums: the lines below them show what differs
        if (!ga && !gb)
            return "Played again, both games agree at that tick: the difference comes from state an earlier game "
                   "left behind (a static, a cache, a global that is not reset when a game starts)";
        if (!ga || !gb || la != lb)
            return "line " + std::to_string(n) + ":\n  first:  " + (ga ? la : "<end>") + "\n  second: "
                   + (gb ? lb : "<end>");
    }
}
} // namespace

int main()
{
    const Recording first = Play(0, 0, -1, nullptr);
    // Things that must not matter: leftover heap blocks (other addresses), the generator where the first game left
    // it (the game reseeds it), the other peer's view (peer 1)
    std::vector<std::unique_ptr<int[]>> junk;
    for (int i = 0; i < 257; ++i)
        junk.emplace_back(new int[static_cast<std::size_t>(16 + i * 7)]);
    Utility::Rng().Next();
    const Recording second = Play(0, 1, -1, nullptr);

    std::printf(
        "game: %d ticks, %d units alive, %d dead at the end\n", kTicks, first.unitsAtEnd, first.deadAtEnd);
    int failures = 0;
    if (first.deadAtEnd == 0)
    {
        std::printf("FAIL: nobody died: the computer players did not fight (the test proves little)\n");
        ++failures;
    }
    for (int tick = 0; tick < kTicks; ++tick)
    {
        if (first.checksums[static_cast<std::size_t>(tick)] == second.checksums[static_cast<std::size_t>(tick)])
            continue;
        int part = 0;
        for (; part < Simulation::kChecksumSections; ++part)
            if (first.sections[static_cast<std::size_t>(tick)][static_cast<std::size_t>(part)]
                != second.sections[static_cast<std::size_t>(tick)][static_cast<std::size_t>(part)])
                break;
        std::printf(
            "FAIL: the two games differ from tick %d on, first in part \"%s\"\n",
            tick,
            Simulation::ChecksumSectionName(part));
        std::string a, b;
        Play(0, 0, tick, &a);
        Play(0, 1, tick, &b);
        std::printf("%s\n", FirstDifferentLine(a, b).c_str());
        ++failures;
        break;
    }
    if (failures == 0)
        std::printf("the same game twice: the same world on all %d ticks\n", kTicks);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
