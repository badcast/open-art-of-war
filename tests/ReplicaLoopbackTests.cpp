// Loopback replication test (TASKNEW.md step 1): two loopback peers, host world in Authority mode
// simulates N ticks, replica world in Replica mode never advances without snapshots, and after
// applying the as-sent snapshots its entity set matches the host.
#include "core/Config.hpp"
#include "core/LoaderResources.hpp"
#include "audio/SoundRequests.hpp"
#include "game/AIController.hpp"
#include "game/Building.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/Match.hpp"
#include "game/Pathfinder.hpp"
#include "game/Players.hpp"
#include "game/Research.hpp"
#include "game/SimCommandCodec.hpp"
#include "game/Simulation.hpp"
#include "game/TileMap.hpp"
#include "game/Unit.hpp"
#include "net/NetMatch.hpp"
#include "net/ReplayRecorder.hpp"
#include "net/WorldSnapshot.hpp"
#include "openaow/net/ReplayFile.hpp"
#include "openaow/net/Session.hpp"
#include "openaow/net/Transport.hpp"
#include "ui/HUDController.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
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

    // 8. Pause-freeze: NetMatch::CanRunTick refuses buffered ticks while paused, and replica tick/state stays
    //    frozen until unpaused, then resumes cleanly (and replica replay recording finishes with non-zero ticks).
    {
        auto authNet = std::make_unique<NetMatch>();
        auto replNet = std::make_unique<NetMatch>();
        std::string err;
        std::uint16_t boundPort = 0;
        for (std::uint16_t p = 41210; p < 41240; ++p)
        {
            if (authNet->host(p, "AuthHost", err))
            {
                boundPort = p;
                break;
            }
        }
        Check(boundPort != 0, "NetMatch localhost host bound for pause-freeze test");
        if (boundPort != 0)
        {
            Check(replNet->join("127.0.0.1", boundPort, "ReplicaPeer", err), "NetMatch replica joined localhost");
            std::uint32_t tMs = 100;
            auto stepNet = [&](int n, std::uint32_t dtMs = 20) {
                for (int step = 0; step < n; ++step)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    tMs += dtMs;
                    authNet->Update(tMs);
                    replNet->Update(tMs);
                }
            };
            for (int step = 0; step < 200 && !replNet->connected(); ++step)
                stepNet(1);
            Check(replNet->connected() && authNet->PlayerCount() == 2, "NetMatch peers reached lobby");

            MatchSetup pauseSetup;
            pauseSetup.mapPath = LoaderResources::MapsDir() + "/two_rivers.tmx";
            pauseSetup.authorityMode = true;
            pauseSetup.fogOfWar = false;
            pauseSetup.slots[0] = {true, Faction::Resistance, 0, 0, true, 1};
            pauseSetup.slots[1] = {true, Faction::Confederation, 1, 1, true, 1};
            Net::LobbyChoice readyChoice;
            readyChoice.ready = true;
            authNet->SetChoice(readyChoice);
            replNet->SetChoice(readyChoice);
            stepNet(25);
            authNet->startMatch(pauseSetup, tMs);

            MatchSetup authOut, replOut;
            bool authGot = false, replGot = false;
            for (int step = 0; step < 200 && (!authGot || !replGot); ++step)
            {
                stepNet(1);
                authGot = authGot || authNet->TakeStartedMatch(authOut);
                replGot = replGot || replNet->TakeStartedMatch(replOut);
            }
            Check(
                authGot && replGot && authOut.authorityMode && replOut.authorityMode,
                "NetMatch started authority match");

            Simulation authSim, replSim;
            authSim.Reset(42u);
            replSim.Reset(42u);
            authNet->Attach(authSim);
            replNet->Attach(replSim);
            Check(authSim.AdvancesWorld() && authNet->IsAuthority(), "NetMatch slot 0 attached as authority");
            Check(!replSim.AdvancesWorld() && replNet->IsReplica(), "NetMatch slot 1 attached as replica");

            // Advance past initial turn delay so turn 0 and turn 1 close and are buffered on authority.
            stepNet(160, 25);
            Check(authNet->CanRunTick(0), "authority can run tick 0 before pause");
            authNet->AfterTick(0, 0x1111u);
            authSim.EndTick();
            authNet->AfterTick(1, 0x2222u);
            authSim.EndTick();
            stepNet(10);
            replNet->PumpReplica(tMs);

            // Pause the match and verify NetMatch::CanRunTick refuses buffered turns while paused.
            authNet->TogglePause();
            for (int step = 0; step < 100 && (!authNet->paused() || !replNet->paused()); ++step)
                stepNet(1);
            Check(authNet->paused() && replNet->paused(), "both NetMatch peers observe paused state");
            Check(!authNet->CanRunTick(2), "authority NetMatch::CanRunTick returns false while paused");
            Check(!replNet->CanRunTick(0), "replica NetMatch::CanRunTick remains false while paused");

            // Advance time while paused; no new snapshots or replica ticks arrive.
            WorldSnapshot pausedSnap;
            for (int step = 0; step < 25; ++step)
            {
                stepNet(1, 40);
                replNet->PumpReplica(tMs);
            }
            Check(!replNet->TakeSnapshot(pausedSnap), "replica receives no advancing snapshot while paused");
            Check(replSim.Tick() == 0, "replica sim tick stays frozen at 0 while paused");

            // Unpause and confirm ticking + snapshots resume cleanly.
            authNet->TogglePause();
            for (int step = 0; step < 100 && (authNet->paused() || replNet->paused()); ++step)
                stepNet(1);
            Check(!authNet->paused() && !replNet->paused(), "both NetMatch peers resumed from pause");
            stepNet(30, 25);
            Check(authNet->CanRunTick(2), "authority NetMatch::CanRunTick resumes true after unpause");

            // Publish snapshot at tick 2 -> replica receives it and FinishReplay writes non-empty turns.
            TileMap mPause;
            mPause.InitBlankArena(16, 16);
            EntityManager emPause;
            Economy ecPause;
            Pathfinder pfPause;
            pfPause.SetMap(&mPause);
            pfPause.SetEntityManager(&emPause);
            AIController aiPause;
            SimContext ctxPause {emPause, mPause, pfPause, ecPause, aiPause};
            const int hqPause = emPause.SpawnBuilding(UnitType::Headquarters, 4, 4, Faction::Resistance);
            if (Entity *e = emPause.GetEntity(hqPause))
                e->owner = 0;
            WorldSnapshot pubSnap = WorldSnapshot::CaptureEntities(emPause, 2);
            authSim.FillEconomies(ctxPause, pubSnap);
            authSim.FillResearch(pubSnap);
            authNet->PublishSnapshot(pubSnap, emPause, mPause);
            WorldSnapshot gotSnap;
            bool gotAfterResume = false;
            for (int step = 0; step < 100 && !gotAfterResume; ++step)
            {
                stepNet(1);
                gotAfterResume = replNet->TakeSnapshot(gotSnap);
            }
            Check(gotAfterResume && gotSnap.tick == 2, "replica receives snapshot after resume");
            replNet->PumpReplica(tMs);

            const std::string authReplay = authNet->FinishReplay("victory");
            const std::string replReplay = replNet->FinishReplay("defeat");
            Check(!replReplay.empty(), "replica FinishReplay wrote .awr replay file");
            if (!replReplay.empty())
            {
                Net::ReplayData rd;
                std::string rErr;
                Check(Net::readReplay(replReplay, rd, rErr) && !rd.turns.empty(), "replica .awr holds turn bundles");
                std::filesystem::remove(replReplay);
            }
            if (!authReplay.empty())
                std::filesystem::remove(authReplay);
            replNet->leave();
            authNet->leave();
        }
    }

    // 9. Running-skirmish loopback: Authority ticks combat/economy/building/research while Replica receives
    //    filtered snapshots and keeps HUD timers, primary producers, notifications, fog, economy, and entities
    //    synchronized without ever running simulation locally.
    {
        Net::LoopbackHub hubSkirmish;
        Net::HostConfig hc;
        hc.identity = TestIdentity();
        hc.turnMs = 100;
        hc.ticksPerTurn = 2;
        Net::SessionHost host(hc);
        host.addTransport(hubSkirmish.server());

        auto c0 = std::make_unique<Net::SessionClient>(hubSkirmish.client(), MakeClientConfig("AuthP0"));
        auto c1 = std::make_unique<Net::SessionClient>(hubSkirmish.client(), MakeClientConfig("ReplP1"));

        std::uint32_t tMs = 10;
        auto pumpAll = [&](int steps = 20) {
            for (int i = 0; i < steps; ++i)
            {
                tMs += 20;
                c0->update(tMs);
                c1->update(tMs);
                host.update(tMs);
            }
        };
        pumpAll(30);
        Check(host.playerCount() == 2 && c0->slot() == 0 && c1->slot() == 1, "skirmish loopback peers joined");

        MatchSetup setup;
        setup.authorityMode = true;
        setup.fogOfWar = true;
        setup.startingCredits = 2000;
        setup.slots[0] = {true, Faction::Resistance, 0, 0, true, 1};
        setup.slots[1] = {true, Faction::Confederation, 1, 1, true, 1};
        Net::LobbyChoice l0;
        l0.faction = 0;
        l0.color = 0;
        l0.ready = true;
        Net::LobbyChoice l1;
        l1.faction = 1;
        l1.color = 1;
        l1.ready = true;
        c0->sendLobbyChoice(l0);
        c1->sendLobbyChoice(l1);
        pumpAll(15);
        host.startMatch(NetMatch::EncodeSetup(setup), tMs, 0);
        pumpAll(20);
        Check(c0->matchStarted() && c1->matchStarted(), "skirmish loopback match started");

        // Host world (slot 0 authority)
        Players::Setup(setup.slots, 0);
        TileMap authMap;
        authMap.InitBlankArena(32, 32);
        authMap.SetFogEnabled(true);
        authMap.SetViewerTeam(0);
        EntityManager authEm;
        authEm.SetEntityLimit(MAX_UNITS);
        Economy authEc;
        authEc.Reset();
        authEc.AddCredits(2000);
        Pathfinder authPf;
        authPf.SetMap(&authMap);
        authPf.SetEntityManager(&authEm);
        AIController authAi;
        authAi.SetPlayer(0);
        Simulation authSim;
        authSim.Reset(999u);
        authSim.SetExecutionMode(Simulation::ExecutionMode::Authority);
        authSim.StartMatch(setup);
        SimContext authCtx {authEm, authMap, authPf, authEc, authAi};

        const int authHq0 = authEm.SpawnBuilding(UnitType::Headquarters, 4, 4, Faction::Resistance);
        const int authBar0 = authEm.SpawnBuilding(UnitType::Barracks, 8, 4, Faction::Resistance);
        const int authUnit0 = authEm.SpawnUnitByArchetype(0, 12, 14, Faction::Resistance);
        for (int id : {authHq0, authBar0, authUnit0})
            if (Entity *e = authEm.GetEntity(id))
                e->owner = 0;

        const int authHq1 = authEm.SpawnBuilding(UnitType::Headquarters, 24, 24, Faction::Confederation);
        const int authBar1 = authEm.SpawnBuilding(UnitType::Barracks, 20, 24, Faction::Confederation);
        const int authUnit1 = authEm.SpawnUnitByArchetype(1, 16, 14, Faction::Confederation);
        for (int id : {authHq1, authBar1, authUnit1})
            if (Entity *e = authEm.GetEntity(id))
                e->owner = 1;
        authEm.UpdateFogOfWar(authMap);

        // Replica world (slot 1 replica + HUDController)
        TileMap replMap;
        replMap.InitBlankArena(32, 32);
        replMap.SetFogEnabled(true);
        replMap.SetViewerTeam(1);
        EntityManager replEm;
        replEm.SetEntityLimit(MAX_UNITS);
        Economy replEc;
        replEc.Reset();
        replEc.SetPlayer(1);
        Pathfinder replPf;
        replPf.SetMap(&replMap);
        replPf.SetEntityManager(&replEm);
        AIController replAi;
        replAi.SetPlayer(1);
        Simulation replSim;
        replSim.Reset(999u);
        replSim.SetExecutionMode(Simulation::ExecutionMode::Replica);
        SimContext replCtx {replEm, replMap, replPf, replEc, replAi};
        HUDController replHud;
        replHud.BindSimulation(&replSim);
        replHud.ResetForMission();
        replHud.SetNotification("REPLICA ACTIVE", 1.0f);

        std::vector<SimCommand> replSinkOut;
        replSim.SetCommandSink([&](SimCommand cmd) { replSinkOut.push_back(std::move(cmd)); });

        // Replica issues production + attack-move orders via Simulation::Issue -> sent to host over loopback.
        {
            Players::Setup(setup.slots, 1);
            SimCommand prod;
            prod.type = CommandType::Produce;
            prod.player = 1;
            prod.a = 1; // Confederation Rifleman
            prod.b = authBar1;
            prod.c = 1;
            replSim.Issue(std::move(prod));

            SimCommand atk;
            atk.type = CommandType::OrderAt;
            atk.player = 1;
            atk.ids.push_back(static_cast<std::uint32_t>(authUnit1));
            atk.x = 12 * TILE_WIDTH + TILE_WIDTH / 2;
            atk.y = 14 * TILE_HEIGHT + TILE_HEIGHT / 2;
            atk.a = static_cast<std::int32_t>(OrderKind::AttackMove);
            replSim.Issue(std::move(atk));
        }
        Check(replSinkOut.size() == 2, "replica commands queued via sink instead of local schedule");
        for (const std::vector<std::uint8_t> &batch : NetMatch::EncodeBatches(replSinkOut))
            c1->sendCommands(0, batch);
        replSinkOut.clear();
        pumpAll(15);

        WorldSnapshot lastBaseForRepl1;
        WorldSnapshot replBase;
        bool hasReplBase = false;
        std::uint32_t scheduledHostTurn = 0;
        const int unit0InitialHp = authEm.GetEntity(authUnit0)->currentHp;
        for (int tick = 0; tick < 30; ++tick)
        {
            while (const Net::TurnBundle *b = c0->turn(scheduledHostTurn))
            {
                NetMatch::ScheduleBundle(authSim, *b, 2);
                c0->dropTurnsBefore(scheduledHostTurn);
                ++scheduledHostTurn;
            }
            Players::Setup(setup.slots, 0);
            authEm.SnapshotRenderState();
            authSim.ApplyCommands(authCtx);
            authSim.UpdateResearch(0.05f, authCtx);
            authEm.Update(0.05f, authMap, authPf);
            authSim.UpdateOrderQueues(authCtx);
            authSim.UpdateEconomies(0.05f, authCtx);
            authSim.EndTick();

            if (tick % 2 == 0 || tick == 29)
            {
                WorldSnapshot fullFor1 = WorldSnapshot::CaptureEntities(authEm, static_cast<std::uint32_t>(tick));
                fullFor1.CaptureEffects(authEm);
                authSim.FillEconomies(authCtx, fullFor1);
                authSim.FillResearch(fullFor1);
                fullFor1.FilterForPlayer(authEm, authMap, 1);

                WorldSnapshot wire = hasReplBase ? fullFor1.DeltaFrom(lastBaseForRepl1) : fullFor1;
                lastBaseForRepl1 = fullFor1;
                hasReplBase = true;

                Net::AuthorityState st;
                st.recipient = 1;
                st.tick = wire.tick;
                st.bytes = wire.Encode();
                Check(c0->sendAuthorityState(st), "skirmish authority sent snapshot");
            }
            tMs += 60;
            pumpAll(4);

            Net::AuthorityState inSt;
            while (c1->takeAuthorityState(inSt))
            {
                WorldSnapshot dec;
                Check(WorldSnapshot::Decode(inSt.bytes.data(), inSt.bytes.size(), dec), "replica decoded skirmish snap");
                if (!dec.full)
                    Check(dec.ExpandFrom(replBase), "replica expanded skirmish delta");
                replBase = dec;
                Players::Setup(setup.slots, 1);
                Check(replEm.ApplySnapshot(dec), "replica applied skirmish snapshot");
                dec.ApplyWalls(replMap);
                replEm.UpdateFogOfWar(replMap);
                replSim.ApplyEconomies(dec, replCtx);
                replSim.ApplyResearch(dec, replCtx);
                c1->sendAuthorityAck(dec.tick);
            }

            Players::Setup(setup.slots, 1);
            // Replica HUD update runs without advancing world simulation.
            replHud.Update(0.05f, replEm, replEc, replSim.TakeEvents());
        }

        Players::Setup(setup.slots, 1);
        Check(!replSim.AdvancesWorld() && replSim.Tick() == 0, "replica never ran world simulation ticks");
        Check(
            authEc.GetCredits() >= 0 && replEc.GetCredits() == authSim.EconomyOf(1, authCtx).GetCredits(),
            "replica economy synced from authority after production spending");
        Check(replEc.GetCredits() < 2000, "replica production command spent slot-1 credits on authority");
        const Building *replBar1 = replEm.GetBuilding(authBar1);
        Check(
            replBar1 && (replBar1->isProducing || !replBar1->productionQueue.empty()),
            "replica building shows replicated production queue");
        // Fog hides host's remote base (4,4) from player 1's replica while visible combatants are synced.
        Check(
            replEm.GetEntity(authHq0) == nullptr && replEm.GetEntity(authUnit0) != nullptr,
            "replica keeps fog-filtered entities synced without unseen base");
        const Entity *authU0 = authEm.GetEntity(authUnit0);
        const Entity *replU0 = replEm.GetEntity(authUnit0);
        Check(
            authU0 && replU0 && authU0->currentHp < unit0InitialHp && replU0->currentHp == authU0->currentHp,
            "combat damage on authority replicated cleanly to replica");
        Check(replHud.GetPrimaryBarracksId() == authBar1, "replica HUD tracks primary barracks");
        Check(replHud.GetNotification().empty(), "replica HUD notification timer decremented cleanly");

        // Verify ResearchCompleted and PlayerEliminated events trigger replica HUD notifications.
        SoundRequests::Take();
        replSim.NoteEvent({SimEvent::Type::ResearchCompleted, 1, 0});
        replHud.Update(0.05f, replEm, replEc, replSim.TakeEvents());
        Check(!replHud.GetNotification().empty(), "replica HUD displays ResearchCompleted notification");
        const auto soundReqs = SoundRequests::Take();
        Check(
            !soundReqs.empty() && soundReqs[0].announcer && soundReqs[0].event == "ann_research_complete",
            "replica HUD triggers research completion announcer cue");

        replSim.NoteEvent({SimEvent::Type::PlayerEliminated, 0, 0});
        replHud.Update(0.05f, replEm, replEc, replSim.TakeEvents());
        Check(
            replHud.GetNotification().find("1") != std::string::npos,
            "replica HUD displays PlayerEliminated notification");
    }

    // 10. Replica recovery & render/wall delta isolation:
    //     a) TakeSnapshot sends AuthorityAck(0) NACK when a delta's baseTick is missing, and Authority immediately
    //        recovers with full = true without waiting for tick % 100.
    //     b) Wall delta sync strips unchanged walls while syncing new, damaged, and destroyed walls on non-%20 ticks.
    //     c) EntityManager::ApplySnapshot resets hasPrevRender for newly created or fog-re-emerged entities while
    //        preserving interpolation for continuously visible entities.
    {
        auto authNet = std::make_unique<NetMatch>();
        auto replNet = std::make_unique<NetMatch>();
        std::string err;
        std::uint16_t boundPort = 0;
        for (std::uint16_t p = 41250; p < 41280; ++p)
        {
            if (authNet->host(p, "WallHost", err))
            {
                boundPort = p;
                break;
            }
        }
        Check(boundPort != 0, "NetMatch localhost host bound for NACK & wall delta test");
        if (boundPort != 0)
        {
            Check(replNet->join("127.0.0.1", boundPort, "WallReplica", err), "NetMatch replica joined for test 10");
            std::uint32_t tMs = 100;
            auto stepNet = [&](int n, std::uint32_t dtMs = 20) {
                for (int step = 0; step < n; ++step)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    tMs += dtMs;
                    authNet->Update(tMs);
                    replNet->Update(tMs);
                }
            };
            for (int step = 0; step < 200 && !replNet->connected(); ++step)
                stepNet(1);
            Check(replNet->connected() && authNet->PlayerCount() == 2, "test 10 peers reached lobby");

            MatchSetup setup10;
            setup10.mapPath = LoaderResources::MapsDir() + "/two_rivers.tmx";
            setup10.authorityMode = true;
            setup10.fogOfWar = false;
            setup10.slots[0] = {true, Faction::Resistance, 0, 0, true, 1};
            setup10.slots[1] = {true, Faction::Confederation, 1, 1, true, 1};
            Net::LobbyChoice readyChoice;
            readyChoice.ready = true;
            authNet->SetChoice(readyChoice);
            replNet->SetChoice(readyChoice);
            stepNet(25);
            authNet->startMatch(setup10, tMs);

            MatchSetup authOut, replOut;
            bool authGot = false, replGot = false;
            for (int step = 0; step < 200 && (!authGot || !replGot); ++step)
            {
                stepNet(1);
                authGot = authGot || authNet->TakeStartedMatch(authOut);
                replGot = replGot || replNet->TakeStartedMatch(replOut);
            }
            Check(authGot && replGot, "test 10 started authority match");

            Simulation authSim, replSim;
            authSim.Reset(77u);
            replSim.Reset(77u);
            authNet->Attach(authSim);
            replNet->Attach(replSim);
            Players::Setup(setup10.slots, 0);

            TileMap authMap, replMap;
            authMap.InitBlankArena(16, 16);
            replMap.InitBlankArena(16, 16);
            EntityManager authEm;
            Economy authEc;
            Pathfinder authPf;
            authPf.SetMap(&authMap);
            authPf.SetEntityManager(&authEm);
            AIController authAi;
            SimContext authCtx {authEm, authMap, authPf, authEc, authAi};

            const int hq0 = authEm.SpawnBuilding(UnitType::Headquarters, 3, 3, Faction::Resistance);
            if (Entity *e = authEm.GetEntity(hq0))
                e->owner = 0;
            authMap.SetWall(6, 6, Faction::Resistance, 100, 0);

            auto makeSnap = [&](std::uint32_t tick) {
                WorldSnapshot s = WorldSnapshot::CaptureEntities(authEm, tick);
                s.CaptureWalls(authMap);
                authSim.FillEconomies(authCtx, s);
                authSim.FillResearch(s);
                return s;
            };

            // (a) Full snapshot at tick 2 -> Ack -> evict base 2 from m_replicaSnapshots without advancing
            //     peer.ack -> delta at tick 4 triggers AuthorityAck(0) NACK -> tick 6 recovers with full = true.
            authNet->PublishSnapshot(makeSnap(2), authEm, authMap);
            WorldSnapshot got2;
            bool got2Ok = false;
            for (int step = 0; step < 100 && !got2Ok; ++step)
            {
                stepNet(1);
                got2Ok = replNet->TakeSnapshot(got2);
            }
            Check(got2Ok && got2.full && got2.tick == 2 && got2.hasWalls, "test 10 replica received initial full tick 2");
            got2.ApplyWalls(replMap);
            Check(replMap.GetWall(6, 6) != 0, "test 10 replica applied initial wall");
            replNet->AckSnapshot(got2);
            stepNet(10);

            // Evict tick 2 from m_replicaSnapshots using tick=1 dummies (keeps m_authorityStateTick == 2 and peer.ack == 2).
            WorldSnapshot dummy = got2;
            dummy.tick = 1;
            for (int k = 0; k < 64; ++k)
            {
                replNet->AckSnapshot(dummy);
                if (k % 16 == 15)
                    stepNet(1);
            }
            stepNet(5);

            // Tick 4 is published as a delta against baseTick 2; replica lacks base 2 and sends AuthorityAck(0) NACK.
            authNet->PublishSnapshot(makeSnap(4), authEm, authMap);
            WorldSnapshot got4;
            for (int step = 0; step < 25; ++step)
            {
                stepNet(1);
                Check(!replNet->TakeSnapshot(got4), "test 10 replica refuses delta with missing baseTick 2");
            }
            stepNet(10);

            // Authority handles AuthorityAck(0) and immediately sends a full snapshot at tick 6 (6 % 100 != 0).
            authNet->PublishSnapshot(makeSnap(6), authEm, authMap);
            WorldSnapshot got6;
            bool got6Ok = false;
            for (int step = 0; step < 100 && !got6Ok; ++step)
            {
                stepNet(1);
                got6Ok = replNet->TakeSnapshot(got6);
            }
            Check(got6Ok && got6.full && got6.tick == 6, "test 10 NACK triggered immediate full snapshot recovery");
            replNet->AckSnapshot(got6);
            stepNet(10);

            // (b) Wall delta sync on non-%20 ticks:
            //     - Tick 8: unchanged wall snapshot strips walls from delta packet (hasWalls == false).
            authNet->PublishSnapshot(makeSnap(8), authEm, authMap);
            WorldSnapshot got8;
            bool got8Ok = false;
            for (int step = 0; step < 100 && !got8Ok; ++step)
            {
                stepNet(1);
                got8Ok = replNet->TakeSnapshot(got8);
            }
            Check(got8Ok && got8.baseTick == 6 && !got8.hasWalls && got8.walls.empty(),
                  "test 10 unchanged walls stripped from non-%20 delta snapshot");
            replNet->AckSnapshot(got8);
            stepNet(10);

            //     - Tick 10: add wall (7,6) and damage wall (6,6) -> syncs immediately on non-%20 tick.
            authMap.SetWall(7, 6, Faction::Resistance, 100, 0);
            authMap.DamageWall(6, 6, 75);
            authNet->PublishSnapshot(makeSnap(10), authEm, authMap);
            WorldSnapshot got10;
            bool got10Ok = false;
            for (int step = 0; step < 100 && !got10Ok; ++step)
            {
                stepNet(1);
                got10Ok = replNet->TakeSnapshot(got10);
            }
            Check(got10Ok && got10.baseTick == 8 && got10.hasWalls,
                  "test 10 changed walls arrive in delta on non-%20 tick");
            got10.ApplyWalls(replMap);
            Check(replMap.GetWall(7, 6) != 0 && replMap.GetWallHp(6, 6) < replMap.GetWallHp(7, 6),
                  "test 10 replica applied added and damaged walls from delta");
            replNet->AckSnapshot(got10);
            stepNet(10);

            //     - Tick 12: destroy wall (6,6) -> replica clears (6,6) while keeping (7,6).
            authMap.DestroyWall(6, 6);
            authNet->PublishSnapshot(makeSnap(12), authEm, authMap);
            WorldSnapshot got12;
            bool got12Ok = false;
            for (int step = 0; step < 100 && !got12Ok; ++step)
            {
                stepNet(1);
                got12Ok = replNet->TakeSnapshot(got12);
            }
            Check(got12Ok && got12.baseTick == 10 && got12.hasWalls,
                  "test 10 wall destruction arrives in delta on non-%20 tick");
            got12.ApplyWalls(replMap);
            Check(replMap.GetWall(6, 6) == 0 && replMap.GetWall(7, 6) != 0,
                  "test 10 replica removed destroyed wall and kept remaining wall");

            replNet->leave();
            authNet->leave();
        }

        // (c) EntityManager::ApplySnapshot hasPrevRender reset for newly created and fog-re-emerged entities.
        EntityManager emInterp;
        auto makeUnitSnap = [](std::uint32_t tick, std::uint32_t id, int x, int y, bool visible) {
            WorldSnapshot s;
            s.tick = tick;
            s.full = true;
            if (visible)
            {
                EntitySnap u;
                u.id = id;
                u.isBuilding = false;
                u.type = static_cast<std::int32_t>(UnitType::ResistanceRifleman);
                u.archetype = 0;
                u.faction = static_cast<std::uint8_t>(Faction::Resistance);
                u.owner = 0;
                u.tileX = x;
                u.tileY = y;
                u.hp = 100;
                u.maxHp = 100;
                u.state = static_cast<std::uint8_t>(ActionState::Idle);
                u.alive = true;
                s.entities.push_back(u);
            }
            else
            {
                s.hiddenAlive.push_back(id);
            }
            return s;
        };

        Check(emInterp.ApplySnapshot(makeUnitSnap(1, 5, 4, 4, true)), "interp test spawned entity");
        const Entity *ent = emInterp.GetEntity(5);
        Check(ent && !ent->hasPrevRender && ent->tileX == 4 && ent->tileY == 4,
              "newly created replica entity has hasPrevRender == false");

        Check(emInterp.ApplySnapshot(makeUnitSnap(2, 5, 5, 4, true)), "interp test moved visible entity");
        ent = emInterp.GetEntity(5);
        Check(ent && ent->hasPrevRender && ent->tileX == 5 && ent->tileY == 4,
              "continuously visible replica entity keeps hasPrevRender == true");

        Check(emInterp.ApplySnapshot(makeUnitSnap(3, 5, 5, 4, false)), "interp test hid entity in fog");
        Check(emInterp.GetEntity(5) != nullptr, "hiddenAlive entity stays allocated in replica EntityManager");

        Check(emInterp.ApplySnapshot(makeUnitSnap(4, 5, 11, 9, true)), "interp test re-emerged entity from fog");
        ent = emInterp.GetEntity(5);
        Check(ent && !ent->hasPrevRender && ent->tileX == 11 && ent->tileY == 9,
              "entity re-emerging from fog resets hasPrevRender == false");
    }

    if (failures == 0)
        std::printf("replica loopback tests: ok\n");
    return failures == 0 ? 0 : 1;
}
