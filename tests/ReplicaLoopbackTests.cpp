// Loopback replication test (TASKNEW.md step 1): two loopback peers, host world in Authority mode
// simulates N ticks, replica world in Replica mode never advances without snapshots, and after
// applying the as-sent snapshots its entity set matches the host.
#include "core/Config.hpp"
#include "game/AIController.hpp"
#include "game/Building.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/Pathfinder.hpp"
#include "game/Players.hpp"
#include "game/Simulation.hpp"
#include "game/TileMap.hpp"
#include "game/Unit.hpp"
#include "net/WorldSnapshot.hpp"
#include "openaow/net/Session.hpp"
#include "openaow/net/Transport.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
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

Net::GameIdentity TestIdentity()
{
    return Net::GameIdentity {"1.0-test", 0x1234, 0x5678};
}

Net::ClientConfig MakeClientConfig(const std::string &name)
{
    Net::ClientConfig c;
    c.identity = TestIdentity();
    c.playerName = name;
    return c;
}

void StepAuthorityTick(Simulation &sim, SimContext &ctx)
{
    if (!sim.AdvancesWorld())
        return;
    sim.ApplyCommands(ctx);
    sim.UpdateResearch(SIMULATION_TIMESTEP, ctx);
    ctx.entities.Update(SIMULATION_TIMESTEP, ctx.map, ctx.pathfinder);
    sim.UpdateOrderQueues(ctx);
    sim.UpdateSquads(SIMULATION_TIMESTEP, ctx, ctx.ai.GetChokepoints());
    sim.UpdateEconomies(SIMULATION_TIMESTEP, ctx);
    sim.EndTick();
}

void TryAdvanceReplica(Simulation &sim, SimContext &ctx)
{
    // Both the SimulationStep-style AdvancesWorld() guard and direct Simulation calls must leave
    // a Replica world untouched.
    sim.ApplyCommands(ctx);
    sim.UpdateResearch(SIMULATION_TIMESTEP, ctx);
    sim.UpdateOrderQueues(ctx);
    sim.UpdateSquads(SIMULATION_TIMESTEP, ctx, ctx.ai.GetChokepoints());
    sim.UpdateEconomies(SIMULATION_TIMESTEP, ctx);
    if (sim.AdvancesWorld())
    {
        ctx.entities.Update(SIMULATION_TIMESTEP, ctx.map, ctx.pathfinder);
        sim.EndTick();
    }
}
} // namespace

int main()
{
    // 1. Two loopback peers connected through SessionHost
    Net::LoopbackHub hub;
    Net::HostConfig hc;
    hc.identity = TestIdentity();
    hc.turnMs = 100;
    hc.reconnectWindowMs = 300;
    Net::SessionHost host(hc);
    host.addTransport(hub.server());

    Net::SessionClient hostPeer(hub.client(), MakeClientConfig("Host"));
    Net::SessionClient replicaPeer(hub.client(), MakeClientConfig("Replica"));

    std::uint32_t now = 0;
    auto pump = [&](std::uint32_t ms) {
        for (std::uint32_t t = 0; t < ms; t += 10)
        {
            now += 10;
            hostPeer.update(now);
            replicaPeer.update(now);
            host.update(now);
            hostPeer.update(now);
            replicaPeer.update(now);
        }
    };

    pump(50);
    Check(host.playerCount() == 2, "two loopback peers joined");
    host.startMatch({7, 7, 7}, now, 200);
    pump(30);
    Check(host.matchRunning(), "loopback match started");
    Check(hostPeer.matchStarted() && replicaPeer.matchStarted(), "both peers received StartMatch");
    Check(hostPeer.authoritySlot() == 0 && replicaPeer.authoritySlot() == 0, "slot 0 is authority");

    // 2. Host world (Authority) and Replica world (Replica)
    std::array<PlayerSlot, kMaxPlayers> slots {};
    slots[0] = {true, Faction::Resistance, 0, 0, false};
    slots[1] = {true, Faction::Confederation, 1, 0, false};
    Players::Setup(slots, 0);

    TileMap hostMap;
    hostMap.InitBlankArena(64, 64);
    hostMap.SetFogEnabled(false);
    EntityManager hostEntities;
    hostEntities.LoadCharacterConfigs();
    Pathfinder hostPf;
    hostPf.SetMap(&hostMap);
    hostPf.SetEntityManager(&hostEntities);
    Economy hostEconomy;
    hostEconomy.SetPlayer(0);
    AIController hostAi;
    hostAi.SetPlayer(0);
    Simulation hostSim;
    hostSim.Reset(12345u);
    hostSim.SetExecutionMode(Simulation::ExecutionMode::Authority);
    SimContext hostCtx {hostEntities, hostMap, hostPf, hostEconomy, hostAi};

    TileMap replicaMap;
    replicaMap.InitBlankArena(64, 64);
    replicaMap.SetFogEnabled(false);
    EntityManager replicaEntities;
    replicaEntities.LoadCharacterConfigs();
    Pathfinder replicaPf;
    replicaPf.SetMap(&replicaMap);
    replicaPf.SetEntityManager(&replicaEntities);
    Economy replicaEconomy;
    replicaEconomy.SetPlayer(1);
    AIController replicaAi;
    replicaAi.SetPlayer(1);
    Simulation replicaSim;
    replicaSim.Reset(12345u);
    replicaSim.SetExecutionMode(Simulation::ExecutionMode::Replica);
    SimContext replicaCtx {replicaEntities, replicaMap, replicaPf, replicaEconomy, replicaAi};

    Check(hostSim.AdvancesWorld(), "authority advances world");
    Check(!replicaSim.AdvancesWorld(), "replica does not advance world");

    const int unitId = hostEntities.SpawnUnit(UnitType::ResistanceRifleman, 10, 10, Faction::Resistance);
    const int bldgId = hostEntities.SpawnBuilding(UnitType::Barracks, 20, 20, Faction::Resistance);
    Check(unitId > 0 && bldgId > 0, "host spawned initial entities");
    if (Building *b = hostEntities.GetBuilding(bldgId))
        b->buildProgress = 0.25f;

    // Issue a move order on the host so simulation ticks measurably change world state.
    SimCommand moveCmd;
    moveCmd.type = CommandType::OrderAt;
    moveCmd.player = 0;
    moveCmd.ids = {static_cast<std::uint32_t>(unitId)};
    moveCmd.x = 22 * 24;
    moveCmd.y = 10 * 24;
    moveCmd.a = static_cast<std::int32_t>(OrderKind::Move);
    hostSim.Schedule(0, moveCmd);

    // Also schedule a command on the replica to verify Replica mode ignores it without snapshots.
    replicaSim.Schedule(0, moveCmd);

    const WorldSnapshot hostInitial = WorldSnapshot::CaptureEntities(hostEntities, hostSim.Tick());

    // 3. Host simulates N ticks; replica runs N frame-steps without receiving any snapshots
    constexpr std::uint32_t kTicks = 20;
    for (std::uint32_t i = 0; i < kTicks; ++i)
    {
        StepAuthorityTick(hostSim, hostCtx);
        TryAdvanceReplica(replicaSim, replicaCtx);
    }

    Check(hostSim.Tick() == kTicks, "host simulated N ticks");
    Check(replicaSim.Tick() == 0, "replica tick stayed at 0");
    Check(replicaEntities.GetAllEntities().empty(), "replica stays empty without snapshots");

    const WorldSnapshot hostAfterN = WorldSnapshot::CaptureEntities(hostEntities, hostSim.Tick());
    Check(!(hostAfterN == hostInitial), "host world state changed over N ticks");

    // 4. Send the full snapshot over loopback and verify replica entity set matches host
    {
        Net::AuthorityState out;
        out.recipient = 1;
        out.tick = hostAfterN.tick;
        out.bytes = hostAfterN.Encode();
        Check(!out.bytes.empty(), "encoded full snapshot");
        Check(hostPeer.sendAuthorityState(out), "host sent full snapshot over loopback");

        pump(20);

        Net::AuthorityState in;
        Check(replicaPeer.takeAuthorityState(in), "replica received full snapshot over loopback");
        Check(in.tick == hostAfterN.tick, "received snapshot tick matches");

        WorldSnapshot decoded;
        Check(WorldSnapshot::Decode(in.bytes.data(), in.bytes.size(), decoded), "replica decoded full snapshot");
        Check(replicaEntities.ApplySnapshot(decoded), "replica applied full snapshot");
    }

    const WorldSnapshot replicaAfterFull = WorldSnapshot::CaptureEntities(replicaEntities, hostSim.Tick());
    Check(replicaAfterFull == hostAfterN, "replica entity set matches host after full snapshot");

    // 5. Simulate another N ticks on host while replica tries to advance without snapshots,
    //    then send a delta snapshot over loopback and verify replica still matches.
    const WorldSnapshot base = hostAfterN;
    for (std::uint32_t i = 0; i < kTicks; ++i)
    {
        StepAuthorityTick(hostSim, hostCtx);
        TryAdvanceReplica(replicaSim, replicaCtx);
    }
    Check(hostSim.Tick() == 2 * kTicks, "host simulated second batch of N ticks");
    Check(replicaSim.Tick() == 0, "replica tick still 0 without snapshots");
    Check(WorldSnapshot::CaptureEntities(replicaEntities, base.tick) == base,
          "replica entities stayed untouched while host simulated more ticks");

    const WorldSnapshot hostAfter2N = WorldSnapshot::CaptureEntities(hostEntities, hostSim.Tick());
    Check(!(hostAfter2N == base), "host world advanced again");

    {
        const WorldSnapshot delta = hostAfter2N.DeltaFrom(base);
        Net::AuthorityState out;
        out.recipient = 1;
        out.tick = delta.tick;
        out.bytes = delta.Encode();
        Check(hostPeer.sendAuthorityState(out), "host sent delta snapshot over loopback");

        pump(20);

        Net::AuthorityState in;
        Check(replicaPeer.takeAuthorityState(in), "replica received delta snapshot over loopback");
        WorldSnapshot decoded;
        Check(WorldSnapshot::Decode(in.bytes.data(), in.bytes.size(), decoded), "replica decoded delta snapshot");
        Check(!decoded.full && decoded.baseTick == base.tick, "delta header matches base tick");
        Check(decoded.ExpandFrom(base), "replica expanded delta against base");
        Check(replicaEntities.ApplySnapshot(decoded), "replica applied delta snapshot");
    }

    const WorldSnapshot replicaAfterDelta = WorldSnapshot::CaptureEntities(replicaEntities, hostSim.Tick());
    Check(replicaAfterDelta == hostAfter2N, "replica entity set matches host after delta snapshot");

    if (failures == 0)
        std::printf("replica loopback tests: ok\n");
    return failures == 0 ? 0 : 1;
}
