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

Net::ClientConfig MakeClientConfig(std::string_view name)
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

    // Direct calls cannot advance a replica or leave commands to execute after a role change.
    Simulation guardSim;
    guardSim.Reset(12345u);
    guardSim.SetRecording(true);
    guardSim.SetExecutionMode(Simulation::ExecutionMode::Replica);
    guardSim.Schedule(0, moveCmd);
    guardSim.EndTick();
    Check(guardSim.Tick() == 0, "direct EndTick does not advance a replica");
    bool sentToAuthority = false;
    guardSim.SetCommandSink([&](SimCommand) { sentToAuthority = true; });
    guardSim.Issue(moveCmd);
    Check(sentToAuthority, "replica still sends player commands to its network sink");
    guardSim.SetCommandSink({});
    guardSim.SetExecutionMode(Simulation::ExecutionMode::Authority);
    guardSim.ApplyCommands(replicaCtx);
    Check(guardSim.log().empty(), "commands scheduled on a replica are not retained");
    guardSim.EndTick();
    Check(guardSim.Tick() == 1, "authority still advances after role change");

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
    Check(
        WorldSnapshot::CaptureEntities(replicaEntities, base.tick) == base,
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

    // 6. Three-team match end + PlayerLeft flow: when the authority (team 0) falls while teams 1 & 2
    //    remain active, authority continues stepping/publishing snapshots until one team remains; then
    //    PlayerLeft destroys the left player's entities and delivers victory to the last survivor.
    {
        Net::LoopbackHub hub3;
        Net::HostConfig hc3;
        hc3.identity = TestIdentity();
        hc3.turnMs = 50;
        hc3.reconnectWindowMs = 500;
        Net::SessionHost host3(hc3);
        host3.addTransport(hub3.server());

        Net::SessionClient p0(hub3.client(), MakeClientConfig("P0"));
        Net::SessionClient p1(hub3.client(), MakeClientConfig("P1"));
        Net::SessionClient p2(hub3.client(), MakeClientConfig("P2"));
        std::uint32_t t3 = 0;
        auto pump3 = [&](std::uint32_t ms) {
            for (std::uint32_t step = 0; step < ms; step += 10)
            {
                t3 += 10;
                p0.update(t3);
                p1.update(t3);
                p2.update(t3);
                host3.update(t3);
                p0.update(t3);
                p1.update(t3);
                p2.update(t3);
            }
        };
        pump3(60);
        Check(host3.playerCount() == 3, "three-team loopback peers joined");
        host3.startMatch({8, 8, 8}, t3, 100);
        pump3(30);
        Check(p0.matchStarted() && p1.matchStarted() && p2.matchStarted(), "3-team match started");

        std::array<PlayerSlot, kMaxPlayers> slots3 {};
        slots3[0] = {true, Faction::Resistance, 0, 0, false};
        slots3[1] = {true, Faction::Confederation, 1, 0, false};
        slots3[2] = {true, Faction::Resistance, 2, 0, false};
        Players::Setup(slots3, 0);

        TileMap m3;
        m3.InitBlankArena(64, 64);
        m3.SetFogEnabled(false);
        EntityManager em3;
        em3.LoadCharacterConfigs();
        Pathfinder pf3;
        pf3.SetMap(&m3);
        pf3.SetEntityManager(&em3);
        Economy ec3;
        ec3.SetPlayer(0);
        AIController ai3;
        ai3.SetPlayer(0);
        Simulation s3;
        s3.Reset(101u);
        s3.SetExecutionMode(Simulation::ExecutionMode::Authority);
        SimContext ctx3 {em3, m3, pf3, ec3, ai3};

        const int b0 = em3.SpawnBuilding(UnitType::Headquarters, 6, 6, Faction::Resistance);
        const int b1 = em3.SpawnBuilding(UnitType::Headquarters, 24, 6, Faction::Confederation);
        const int b2 = em3.SpawnBuilding(UnitType::Headquarters, 42, 6, Faction::Resistance);
        em3.GetEntity(b0)->owner = 0;
        em3.GetEntity(b1)->owner = 1;
        em3.GetEntity(b2)->owner = 2;

        // Destroy player 0's headquarters: player 0 is eliminated, teams 1 and 2 continue.
        em3.GetEntity(b0)->alive = false;
        em3.UpdateElimination(m3);
        Check(
            em3.IsEliminated(0) && !em3.IsEliminated(1) && !em3.IsEliminated(2), "authority player 0 eliminated first");

        auto sendFiltered = [&](std::uint8_t slot) {
            WorldSnapshot s = WorldSnapshot::CaptureEntities(em3, s3.Tick());
            s.FilterForPlayer(em3, m3, slot);
            Net::AuthorityState st;
            st.recipient = slot;
            st.tick = s.tick;
            st.bytes = s.Encode();
            return p0.sendAuthorityState(st);
        };
        WorldSnapshot sFor0 = WorldSnapshot::CaptureEntities(em3, s3.Tick());
        sFor0.FilterForPlayer(em3, m3, 0);
        Check(sFor0.result == 2, "defeated authority sees defeat result");
        Check(sendFiltered(1) && sendFiltered(2), "authority publishes snapshots to surviving teams");
        pump3(20);
        Net::AuthorityState r1, r2;
        WorldSnapshot d1, d2;
        Check(
            p1.takeAuthorityState(r1) && WorldSnapshot::Decode(r1.bytes.data(), r1.bytes.size(), d1),
            "p1 received snapshot");
        Check(
            p2.takeAuthorityState(r2) && WorldSnapshot::Decode(r2.bytes.data(), r2.bytes.size(), d2),
            "p2 received snapshot");
        Check(d1.result == 0 && d2.result == 0, "surviving teams keep playing after authority eliminated");

        // Advance host ticks while player 0 is eliminated and have player 1 leave via SessionHost.
        StepAuthorityTick(s3, ctx3);
        StepAuthorityTick(s3, ctx3);
        Check(s3.Tick() == 2, "authority simulation continues stepping after own defeat");
        p1.sendLeave();
        pump3(100);
        bool sawLeft = false;
        for (std::uint32_t turnIdx = 0; turnIdx < p0.turnsReceived(); ++turnIdx)
        {
            if (const Net::TurnBundle *bundle = p0.turn(turnIdx))
            {
                for (std::uint8_t leftSlot : bundle->left)
                {
                    if (leftSlot == 1)
                    {
                        sawLeft = true;
                        SimCommand lc;
                        lc.type = CommandType::PlayerLeft;
                        lc.player = leftSlot;
                        s3.Schedule(s3.Tick(), lc);
                    }
                }
            }
        }
        Check(sawLeft, "host bundle delivered PlayerLeft for slot 1");
        StepAuthorityTick(s3, ctx3);
        em3.UpdateElimination(m3);
        Check(em3.IsEliminated(1) && !em3.GetEntity(b1), "PlayerLeft destroyed leaving player's buildings");
        Check(sendFiltered(2), "authority sent post-leave snapshot to team 2");
        pump3(20);
        Net::AuthorityState r2End;
        WorldSnapshot d2End;
        Check(
            p2.takeAuthorityState(r2End) && WorldSnapshot::Decode(r2End.bytes.data(), r2End.bytes.size(), d2End),
            "p2 received post-leave snapshot");
        Check(d2End.result == 1 && !em3.GetEntity(b1), "remaining team receives victory after opponent leaves");
    }

    // 7. Reconnect delivers a full snapshot and restores replica state without turn replay
    {
        Net::LoopbackHub hubR;
        Net::HostConfig hcR;
        hcR.identity = TestIdentity();
        hcR.turnMs = 50;
        hcR.reconnectWindowMs = 5000;
        Net::SessionHost hostR(hcR);
        hostR.addTransport(hubR.server());

        Net::SessionClient hostC(hubR.client(), MakeClientConfig("HostR"));
        auto replCfg = MakeClientConfig("ReplR");
        auto replC = std::make_unique<Net::SessionClient>(hubR.client(), replCfg);
        std::uint32_t tR = 0;
        auto pumpR = [&](std::uint32_t ms) {
            for (std::uint32_t step = 0; step < ms; step += 10)
            {
                tR += 10;
                hostC.update(tR);
                if (replC)
                    replC->update(tR);
                hostR.update(tR);
                hostC.update(tR);
                if (replC)
                    replC->update(tR);
            }
        };
        pumpR(60);
        hostR.startMatch({9, 9, 9}, tR, 100);
        pumpR(30);
        const Net::SessionToken savedToken = replC->token();
        Check(savedToken != Net::SessionToken {}, "reconnecting client holds non-zero token");

        std::array<PlayerSlot, kMaxPlayers> slotsR {};
        slotsR[0] = {true, Faction::Resistance, 0, 0, false};
        slotsR[1] = {true, Faction::Confederation, 1, 0, false};
        Players::Setup(slotsR, 0);

        TileMap hostMapR, replMapR;
        hostMapR.InitBlankArena(64, 64);
        hostMapR.SetFogEnabled(false);
        replMapR.InitBlankArena(64, 64);
        replMapR.SetFogEnabled(false);
        EntityManager hostEmR, replEmR;
        hostEmR.LoadCharacterConfigs();
        replEmR.LoadCharacterConfigs();
        Pathfinder hostPfR, replPfR;
        hostPfR.SetMap(&hostMapR);
        hostPfR.SetEntityManager(&hostEmR);
        replPfR.SetMap(&replMapR);
        replPfR.SetEntityManager(&replEmR);
        Economy hostEcR, replEcR;
        hostEcR.SetPlayer(0);
        replEcR.SetPlayer(1);
        AIController hostAiR, replAiR;
        Simulation hostSimR, replSimR;
        hostSimR.Reset(777u);
        hostSimR.SetExecutionMode(Simulation::ExecutionMode::Authority);
        replSimR.Reset(777u);
        replSimR.SetExecutionMode(Simulation::ExecutionMode::Replica);
        SimContext hostCtxR {hostEmR, hostMapR, hostPfR, hostEcR, hostAiR};
        SimContext replCtxR {replEmR, replMapR, replPfR, replEcR, replAiR};

        hostEmR.SpawnBuilding(UnitType::Headquarters, 10, 10, Faction::Resistance);
        for (int i = 0; i < 5; ++i)
            StepAuthorityTick(hostSimR, hostCtxR);

        // Replica disconnects; host spawns new state and keeps advancing turns.
        replC->disconnect();
        pumpR(40);
        replC.reset();
        auto slotConnected = [&](std::uint8_t slot) {
            for (const Net::RosterEntry &e : hostC.roster().players)
                if (e.slot == slot)
                    return e.connected;
            return false;
        };
        Check(!slotConnected(1), "host noticed replica disconnect");

        const int extraUnit = hostEmR.SpawnUnit(UnitType::ResistanceRifleman, 30, 30, Faction::Resistance);
        for (int i = 0; i < 5; ++i)
        {
            StepAuthorityTick(hostSimR, hostCtxR);
            TryAdvanceReplica(replSimR, replCtxR);
        }
        pumpR(200);

        // Replica reconnects with its token and resets its local world as TakeStartedMatch/LoadMatch does.
        replCfg.token = savedToken;
        replC = std::make_unique<Net::SessionClient>(hubR.client(), replCfg);
        replEmR.Clear();
        pumpR(80);
        Check(replC->state() == Net::SessionClient::State::Joined && replC->matchStarted(), "replica reconnected");
        Check(slotConnected(1), "host sees reconnected peer");
        Check(replSimR.Tick() == 0 && !replSimR.AdvancesWorld(), "reconnected replica still does not replay turns");

        const WorldSnapshot fullSnap = WorldSnapshot::CaptureEntities(hostEmR, hostSimR.Tick());
        Check(fullSnap.full, "post-reconnect snapshot is a full snapshot");
        Net::AuthorityState outR;
        outR.recipient = 1;
        outR.tick = fullSnap.tick;
        outR.bytes = fullSnap.Encode();
        Check(hostC.sendAuthorityState(outR), "authority sends full snapshot after reconnect");
        pumpR(30);
        Net::AuthorityState inR;
        WorldSnapshot decR;
        Check(
            replC->takeAuthorityState(inR) && WorldSnapshot::Decode(inR.bytes.data(), inR.bytes.size(), decR)
                && decR.full && replEmR.ApplySnapshot(decR),
            "reconnected replica decodes and applies full snapshot");
        Check(
            replEmR.GetEntity(extraUnit) != nullptr
                && WorldSnapshot::CaptureEntities(replEmR, hostSimR.Tick()) == fullSnap,
            "reconnected replica matches current host world without turn replay");
    }

    if (failures == 0)
        std::printf("replica loopback tests: ok\n");
    return failures == 0 ? 0 : 1;
}
