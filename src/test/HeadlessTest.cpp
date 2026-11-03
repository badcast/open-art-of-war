#include "test/HeadlessTest.hpp"
#include "audio/VoiceManager.hpp"
#include "audio/SfxMixer.hpp"
#include "audio/SoundEngine.hpp"
#include "audio/SoundRequests.hpp"
#include "ui/MenuController.hpp"
#include "game/Projectiles.hpp"
#include "game/WeaponBalance.hpp"
#include "core/LoaderResources.hpp"
#include "core/DetMath.hpp"
#include "ui/RadarAlert.hpp"
#include "game/Players.hpp"
#include "game/MissionDialogs.hpp"
#include "input/InputManager.hpp"
#include "core/Application.hpp"
#include "core/Localization.hpp"
#include "game/TileMap.hpp"
#include "game/Pathfinder.hpp"
#include "game/EntityManager.hpp"
#include "game/Effects.hpp"
#include "game/Economy.hpp"
#include "game/AIController.hpp"
#include "game/MissionController.hpp"
#include "game/GameplayController.hpp"
#include "game/Simulation.hpp"
#include "game/SimCommandCodec.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "core/Utility.hpp"
#include "core/AssetStore.hpp"
#include "core/Settings.hpp"
#include "net/NetMatch.hpp"
#include "net/MapFiles.hpp"
#include "replay/ReplayPlayer.hpp"
#include "ui/ReplayMenu.hpp"
#include "net/ReplayRecorder.hpp"
#include "openaow/net/ReplayFile.hpp"
#include <nlohmann/json.hpp>
#include "game/TileCollision.hpp"
#include "graphics/Camera.hpp"
#include "storage/SaveManager.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <yaml-cpp/yaml.h>
#include <vector>
#include <set>
#include <map>
#include <cmath>
#include <cstdlib>
#include <string>

namespace OpenArtOfWar
{

bool HeadlessTest::RunDeterminismTest(Application &app)
{
    std::cout << "[Test] Validating determinism (same commands -> same world checksum)..." << std::endl;
    EntityManager &entities = *app.m_entities;
    TileMap &map = *app.m_map;
    Pathfinder &pf = *app.m_pathfinder;
    Economy &economy = *app.m_economy;
    AIController &ai = *app.m_aiController;
    Simulation &sim = app.m_gameplayController->simulation();

    // A skirmish of two mixed groups driven only by commands; returns the checksums along the way.
    // With `replay` the commands come from that recording instead of the script.
    auto run = [&](const std::vector<TimedCommand> *replay) {
        std::vector<uint64_t> sums;
        entities.Clear();
        map.InitBlankArena(48, 32, 0);
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
        ResetResearch();
        economy.Reset(5000);
        sim.Reset();
        std::vector<uint32_t> red, blue;
        for (int i = 0; i < 10; ++i)
        {
            red.push_back(
                static_cast<uint32_t>(
                    entities.SpawnUnitByArchetype(i % 3, 3 + i % 4, 10 + i / 4 * 2 + i % 2, Faction::Resistance)));
            blue.push_back(
                static_cast<uint32_t>(
                    entities.SpawnUnitByArchetype(1 + i % 2, 42 - i % 4, 12 + i / 4 * 2, Faction::Confederation)));
        }
        ai.m_chokes.Rebuild(map);
        SimContext ctx {entities, map, pf, economy, ai};
        auto order = [&](Faction f, const std::vector<uint32_t> &ids, int tx, int ty, OrderKind kind, bool group) {
            SimCommand c;
            c.type = CommandType::OrderAt;
            c.player = Players::OwnerOf(f);
            c.ids = ids;
            c.x = tx * TILE_WIDTH + TILE_WIDTH / 2;
            c.y = ty * TILE_HEIGHT + TILE_HEIGHT / 2;
            c.a = static_cast<int32_t>(kind);
            c.b = group ? 1 : 0;
            sim.Issue(std::move(c));
        };
        const float dt = static_cast<float>(SIMULATION_TIMESTEP);
        if (replay)
        {
            for (const TimedCommand &t : *replay)
                sim.Schedule(t.tick, t.command);
        }
        for (int tick = 0; tick < 900; ++tick)
        {
            if (replay)
            {
                // nothing scripted: the recording is already scheduled
            }
            else if (tick == 0)
            {
                order(Faction::Resistance, red, 40, 14, OrderKind::AttackMove, true);
                order(Faction::Confederation, blue, 6, 13, OrderKind::Move, false);
            }
            else if (tick == 120)
            {
                SimCommand stop;
                stop.type = CommandType::Stop;
                stop.player = Players::OwnerOf(Faction::Confederation);
                stop.ids.assign(blue.begin(), blue.begin() + 4);
                sim.Issue(std::move(stop));
            }
            else if (tick == 200)
            {
                order(Faction::Confederation, blue, 30, 20, OrderKind::AttackMove, false);
            }
            else if (tick == 230)
            {
                // Shift-queued points behind the attack-move (they start when each unit is done)
                for (int k = 0; k < 2; ++k)
                {
                    SimCommand q;
                    q.type = CommandType::OrderAt;
                    q.player = Players::OwnerOf(Faction::Confederation);
                    q.ids = blue;
                    q.x = (24 - k * 8) * TILE_WIDTH + TILE_WIDTH / 2;
                    q.y = (8 + k * 14) * TILE_HEIGHT + TILE_HEIGHT / 2;
                    q.a = static_cast<int32_t>(k == 0 ? OrderKind::Move : OrderKind::AttackMove);
                    q.b = 2;
                    sim.Issue(std::move(q));
                }
            }
            sim.ApplyCommands(ctx);
            sim.UpdateResearch(dt, ctx);
            entities.Update(dt, map, pf);
            sim.UpdateOrderQueues(ctx);
            sim.UpdateSquads(dt, ctx, ai.m_chokes);
            economy.Update(dt, entities, map, pf);
            if (tick % 20 == 0)
                sums.push_back(sim.Checksum(ctx));
            sim.EndTick();
        }
        return sums;
    };

    const std::vector<uint64_t> first = run(nullptr);
    // The recording through the wire format (encoded per tick and side, decoded back)
    std::vector<TimedCommand> recorded;
    {
        const std::vector<TimedCommand> log = sim.log();
        size_t i = 0;
        while (i < log.size())
        {
            size_t j = i;
            std::vector<SimCommand> turn;
            while (j < log.size() && log[j].tick == log[i].tick && log[j].command.player == log[i].command.player)
            {
                turn.push_back(log[j].command);
                ++j;
            }
            const std::vector<uint8_t> bytes = SimCommandCodec::Encode(turn);
            std::vector<SimCommand> back;
            if (!SimCommandCodec::Decode(bytes.data(), bytes.size(), log[i].command.player, back)
                || back.size() != turn.size())
            {
                std::cerr << "[Test] FAILED: command codec round trip" << std::endl;
                return false;
            }
            std::cout << "  - Tick " << log[i].tick << ": " << turn.size() << " command(s) of side "
                      << static_cast<int>(log[i].command.player) << " in " << bytes.size() << " bytes" << std::endl;
            for (SimCommand &c : back)
                recorded.push_back({log[i].tick, std::move(c)});
            i = j;
        }
    }
    const std::vector<uint64_t> second = run(&recorded);
    size_t firstDiff = first.size();
    for (size_t i = 0; i < first.size() && i < second.size(); ++i)
    {
        if (first[i] != second[i])
        {
            firstDiff = i;
            break;
        }
    }
    const bool moved = first.size() > 2 && first.front() != first.back();
    std::cout << "DETERMINISM-HASH " << std::hex << first.back() << std::dec << std::endl;
    std::cout << "  - " << first.size()
              << " checksums over 900 ticks (second run replayed from the recording), first difference at "
              << (firstDiff == first.size() ? std::string("none") : "tick " + std::to_string(firstDiff * 20))
              << ", world changed " << moved << ", final " << std::hex << first.back() << std::dec << std::endl;
    if (!moved)
    {
        std::cerr << "[Test] FAILED: the determinism scenario does not change the world" << std::endl;
        return false;
    }
    if (firstDiff != first.size() || first.size() != second.size())
    {
        std::cerr << "[Test] FAILED: the same commands gave a different world (desync)" << std::endl;
        return false;
    }
    entities.Clear();
    sim.Reset();
    ResetResearch();
    return true;
}

bool HeadlessTest::RunMatchTest(Application &app)
{
    std::cout << "[Test] Validating a 4-player match (two mixed teams)..." << std::endl;
    EntityManager &entities = *app.m_entities;
    TileMap &map = *app.m_map;
    Pathfinder &pf = *app.m_pathfinder;
    Economy &economy = *app.m_economy;
    AIController &ai = *app.m_aiController;
    MissionController &mission = *app.m_mission;
    Simulation &sim = app.m_gameplayController->simulation();

    // Team 0: Resistance + Confederation; team 1: Resistance + Confederation (same factions on both sides)
    MatchSetup setup;
    setup.mapPath = LoaderResources::MapPath("m26");
    setup.seed = 12345;
    setup.local = 0;
    const Faction factions[4]
        = {Faction::Resistance, Faction::Confederation, Faction::Resistance, Faction::Confederation};
    const uint8_t teams[4] = {0, 0, 1, 1};
    for (int i = 0; i < 4; ++i)
    {
        setup.slots[i] = {true, factions[i], teams[i], static_cast<uint8_t>(i + 1), true};
        setup.startPosition[i] = static_cast<int8_t>(i);
    }
    if (MissionController::StartPositions(setup.mapPath).size() < 4)
    {
        std::cerr << "[Test] FAILED: m26 must offer 4 start positions" << std::endl;
        return false;
    }
    if (MissionController::StartPositions(LoaderResources::MapPath("map_skirmish_2")).size() != 2)
    {
        std::cerr << "[Test] FAILED: a map without start_positions is a map for two players" << std::endl;
        return false;
    }

    auto run = [&](std::vector<uint64_t> &sums) -> bool {
        if (!mission.LoadMatch(setup, map, entities, economy, pf))
            return false;
        sim.startMatch(setup);
        SimContext ctx {entities, map, pf, economy, ai};
        // Every player owns a headquarters and four units of its own faction
        int hq[4] = {}, units[4] = {};
        for (const Entity &e : entities.GetAllEntities())
        {
            if (e.owner >= 4)
                return false;
            if (e.faction != factions[e.owner])
                return false;
            if (e.isBuilding && e.type == UnitType::Headquarters)
                ++hq[e.owner];
            if (!e.isBuilding)
                ++units[e.owner];
        }
        for (int i = 0; i < 4; ++i)
        {
            if (hq[i] != 1 || units[i] != 4)
            {
                std::cerr << "[Test] FAILED: player " << i << " has " << hq[i] << " HQ / " << units[i] << " units"
                          << std::endl;
                return false;
            }
        }
        // Player 2 (Resistance, team 1) sends its units at player 0 (Resistance, team 0)
        std::vector<uint32_t> attackers;
        int targetHq = 0;
        for (const Entity &e : entities.GetAllEntities())
        {
            if (e.owner == 2 && !e.isBuilding)
                attackers.push_back(static_cast<uint32_t>(e.id));
            if (e.owner == 0 && e.isBuilding && e.type == UnitType::Headquarters)
                targetHq = e.id;
        }
        const Entity *hqEntity = entities.GetEntity(targetHq);
        SimCommand order;
        order.type = CommandType::OrderAt;
        order.player = 2;
        order.ids = attackers;
        order.x = hqEntity->tileX * TILE_WIDTH;
        order.y = hqEntity->tileY * TILE_HEIGHT;
        order.a = static_cast<int32_t>(OrderKind::AttackMove);
        sim.Issue(order);
        // A command for someone else's units does nothing
        SimCommand foreign = order;
        foreign.player = 1;
        sim.Issue(foreign);
        // Player 3 leaves the network match at tick 300: its base, walls and units go (see PlayerLeft)
        int leaverHqX = 0, leaverHqY = 0;
        for (const Entity &e : entities.GetAllEntities())
        {
            if (e.owner == 3 && e.isBuilding && e.type == UnitType::Headquarters)
            {
                leaverHqX = e.tileX;
                leaverHqY = e.tileY;
            }
        }
        const int wallX = std::max(0, leaverHqX - 2), wallY = std::max(0, leaverHqY - 2);
        map.SetWall(wallX, wallY, factions[3], 100, 3);
        const float dt = static_cast<float>(SIMULATION_TIMESTEP);
        for (int tick = 0; tick < 1200; ++tick)
        {
            if (tick == 300)
            {
                SimCommand leave;
                leave.type = CommandType::PlayerLeft;
                leave.player = 3;
                sim.Schedule(sim.Tick(), leave);
            }
            sim.ApplyCommands(ctx);
            sim.UpdateResearch(dt, ctx);
            entities.Update(dt, map, pf);
            sim.UpdateSquads(dt, ctx, ai.GetChokepoints());
            sim.UpdateEconomies(dt, ctx); // the local economy included
            if (tick % 60 == 0)
                sums.push_back(sim.Checksum(ctx));
            sim.EndTick();
        }
        int leftStanding = 0, othersStanding = 0;
        for (const Entity &e : entities.GetAllEntities())
        {
            if (!e.alive || e.state == ActionState::Die)
                continue;
            (e.owner == 3 ? leftStanding : othersStanding) += 1;
        }
        std::cout << "  - Player 3 left: " << leftStanding << " of its things standing, wall "
                  << (map.GetWall(wallX, wallY) > 0 ? "standing" : "gone") << ", others keep " << othersStanding
                  << std::endl;
        if (leftStanding != 0 || map.GetWall(wallX, wallY) > 0 || othersStanding == 0)
        {
            std::cerr << "[Test] FAILED: the forces of a player who left must be destroyed (and only those)"
                      << std::endl;
            return false;
        }
        return true;
    };

    std::vector<uint64_t> first, second;
    if (!run(first))
    {
        std::cerr << "[Test] FAILED: the match did not set up" << std::endl;
        return false;
    }
    // Teams: same faction, different team = enemies; different faction, same team = allies
    const bool teamsRight
        = Players::Hostile(0, 2) && !Players::Hostile(0, 1) && Players::Hostile(1, 3) && !Players::Hostile(2, 3);
    // The other players' economies run here too (income every 3 s)
    SimContext probe {entities, map, pf, economy, ai};
    const bool economiesRun = sim.EconomyOf(3, probe).GetCredits() > setup.startingCredits;
    if (!run(second))
        return false;
    const bool same = first == second;
    std::cout << "  - 4 bases set, teams " << teamsRight << ", other economies run " << economiesRun
              << ", deterministic " << same << " (" << first.size() << " checksums)" << std::endl;

    // Back to the single-player defaults for the tests after this one
    SetPlayerFaction(Faction::Resistance);
    entities.Clear();
    sim.Reset();
    ResetResearch();
    if (!teamsRight || !economiesRun || !same)
    {
        std::cerr << "[Test] FAILED: 4-player match" << std::endl;
        return false;
    }
    return true;
}

bool HeadlessTest::RunTechTest(Application &app)
{
    std::cout << "[Test] Validating Zeus turret research, research time, radar range, far-off sounds..." << std::endl;
    EntityManager &entities = *app.m_entities;
    TileMap &map = *app.m_map;
    MatchSetup setup;
    setup.mapPath = LoaderResources::MapPath("m26");
    setup.seed = 99;
    setup.local = 0;
    setup.slots[0] = {true, Faction::Confederation, 0, 1, true};
    setup.slots[1] = {true, Faction::Resistance, 1, 2, true};
    setup.startPosition[0] = 0;
    setup.startPosition[1] = 1;
    if (!app.m_mission->LoadMatch(setup, map, entities, *app.m_economy, *app.m_pathfinder))
        return false;
    ResetResearch();
    const PlayerId me = 0;

    // Research time: 60 s each (20 s + 40 s)
    for (Faction f : {Faction::Confederation, Faction::Resistance})
    {
        for (int i = 0; i < kResearchCount; ++i)
        {
            if (researchDef(f, i).seconds != 60.0f)
            {
                std::cerr << "[Test] FAILED: research " << i << " of faction " << int(f) << " takes "
                          << researchDef(f, i).seconds << " s, expected 60" << std::endl;
                return false;
            }
        }
    }

    // The Zeus comes without its turret until the technology is researched, then every Zeus gets it at once
    int turretIndex = -1;
    for (int i = 0; i < kResearchCount; ++i)
        if (researchDef(Faction::Confederation, i).effect == ResearchEffect::RocketTurret)
            turretIndex = i;
    if (turretIndex < 0 || ProductionForm(me, 11) != 17 || ProductionForm(me, 10) != 10)
    {
        std::cerr << "[Test] FAILED: a Zeus is trained as the base form until the turret is researched" << std::endl;
        return false;
    }
    const int zeus = entities.SpawnUnitByArchetype(ProductionForm(me, 11), 20, 20, Faction::Confederation, me);
    Unit *z = entities.GetUnit(zeus);
    if (!z || z->archetypeId != 17)
        return false;
    const int cannonBefore = z->attackDamage;
    if (z->hasSecondary)
    {
        std::cerr << "[Test] FAILED: the base Zeus has no rocket launcher" << std::endl;
        return false;
    }
    z->currentHp = z->maxHp / 2;
    CompleteResearch(me, turretIndex, entities);
    z = entities.GetUnit(zeus);
    const int fresh = entities.SpawnUnitByArchetype(ProductionForm(me, 11), 22, 20, Faction::Confederation, me);
    if (!z || z->archetypeId != 11 || !z->hasSecondary || z->attackDamage != cannonBefore
        || z->currentHp != z->maxHp / 2 || !entities.GetUnit(fresh) || entities.GetUnit(fresh)->archetypeId != 11
        || !entities.GetUnit(fresh)->hasSecondary)
    {
        std::cerr << "[Test] FAILED: the researched turret must reach the existing Zeus (arch "
                  << (z ? z->archetypeId : -1) << ", hp " << (z ? z->currentHp : -1) << ") and the new ones"
                  << std::endl;
        return false;
    }
    ResetResearch();

    // Radar locator: sees 15 tiles beyond its sight radius once it is finished
    map.SetFogEnabled(true);
    map.ResetAllFog(FogState::Hidden);
    const int radar = entities.SpawnBuildingByIndex(6, 40, 40, Faction::Confederation, 0.5f, me);
    entities.UpdateFogOfWar(map);
    const bool unfinishedFar = map.IsVisibleTo(40 + 1 + 11 + 5, 41, 0);
    if (Building *rb = entities.GetBuilding(radar))
        rb->buildProgress = 1.0f;
    map.ResetAllFog(FogState::Hidden);
    entities.UpdateFogOfWar(map);
    const bool finishedFar = map.IsVisibleTo(40 + 1 + 11 + 5, 41, 0);
    const bool beyond = map.IsVisibleTo(40 + 1 + 11 + 15 + 4, 41, 0);
    map.SetFogEnabled(false);
    if (unfinishedFar || !finishedFar || beyond)
    {
        std::cerr << "[Test] FAILED: radar locator range (unfinished sees far " << unfinishedFar << ", finished "
                  << finishedFar << ", beyond " << beyond << ")" << std::endl;
        return false;
    }
    if (IsRadarUnlocked(me))
    {
        std::cerr << "[Test] FAILED: radar locators are locked until the technology is researched" << std::endl;
        return false;
    }

    // Shots and explosions out of range are still heard, at 0.07 at least (the sound settings say so)
    if (app.m_sound)
    {
        for (const char *event : {"fire_rifle", "fire_cannon", "explode_large", "hit_armor"})
        {
            const SfxMixer::EventConfig *c = app.m_sound->Sfx().Config(event);
            if (!c || c->minGain < 0.0699f || c->minGain > 0.0701f)
            {
                std::cerr << "[Test] FAILED: " << event << " must have min_gain 0.07" << std::endl;
                return false;
            }
        }
    }
    // The Grad in siege mode fires a salvo of six rockets (0.12 s apart); out of siege mode a single one
    {
        auto rockets = [&](int arch) {
            entities.Clear();
            entities.TakeSoundCues();
            entities.SpawnUnitByArchetype(arch, 20, 20, Faction::Confederation, 0);
            const int target = entities.SpawnUnitByArchetype(0, 30, 20, Faction::Resistance, 1);
            if (Unit *t = entities.GetUnit(target))
            {
                t->moveSpeed = 0.0f;
                t->attackDamage = 0;
                t->maxHp = t->currentHp = 100000;
            }
            for (int i = 0; i < 100; ++i) // 5 s: one salvo (the reload is 6 s)
                entities.Update(0.05f, map, *app.m_pathfinder);
            int n = 0;
            for (const auto &c : entities.TakeSoundCues())
                n += c.cue == EntityManager::SoundCue::FireMlrs || c.cue == EntityManager::SoundCue::FireRocket ? 1 : 0;
            return n;
        };
        const int siege = rockets(16);
        std::cout << "  - Grad in siege mode: " << siege << " rockets in the first salvo" << std::endl;
        if (siege != 6)
        {
            std::cerr << "[Test] FAILED: a Grad in siege mode fires 6 rockets per salvo, got " << siege << std::endl;
            return false;
        }
    }
    // The heavy tanks (Zeus, Rhino) fire with the deeper cannon sound, the other tank guns with the usual one
    {
        map.SetFogEnabled(false);
        entities.TakeSoundCues();
        auto cueOf = [&](Faction f, int arch) {
            entities.Clear();
            const PlayerId owner = Players::OwnerOf(f);
            const PlayerId enemy = Players::OwnerOf(f == Faction::Resistance ? Faction::Confederation : Faction::Resistance);
            entities.SpawnUnitByArchetype(arch, 20, 20, f, owner);
            const int target = entities.SpawnUnitByArchetype(
                f == Faction::Resistance ? 1 : 0, 24, 20, f == Faction::Resistance ? Faction::Confederation : Faction::Resistance, enemy);
            if (Unit *t = entities.GetUnit(target))
            {
                t->moveSpeed = 0.0f;
                t->attackDamage = 0;
                t->maxHp = t->currentHp = 100000;
            }
            for (int i = 0; i < 240; ++i) // 12 s: the heavy guns turn and reload slowly
                entities.Update(0.05f, map, *app.m_pathfinder);
            if (std::getenv("OAOW_TEST_VERBOSE"))
                for (const Entity &e : entities.GetAllEntities())
                    if (const Unit *dbg = e.AsUnit())
                        std::cout << "    arch " << dbg->archetypeId << " owner " << int(dbg->owner) << " at " << dbg->tileX << "," << dbg->tileY << " state " << int(dbg->state) << " target " << dbg->targetEntityId << " hp " << dbg->currentHp << " range " << dbg->attackRange << " dmg " << dbg->attackDamage << " cd " << dbg->cooldownTimer << " facing " << dbg->facingDirection << " turnDelay " << dbg->turnStepDelay << " rotTurret " << dbg->hasRotatingTurret << " burst " << dbg->burstShotsRemaining << " attackAnim " << dbg->attackAnimTimer << " engaged " << dbg->isDirectEngaged << " waiting " << dbg->waitingForPath << " hold " << dbg->HoldsGround() << " turnTimer " << dbg->turnTimer << " siege " << dbg->isSiegeMode << std::endl;
            std::set<int> cues;
            for (const auto &c : entities.TakeSoundCues())
                cues.insert(static_cast<int>(c.cue));
            return cues;
        };
        using Cue = EntityManager::SoundCue;
        const auto zeus = cueOf(Faction::Confederation, 11);
        const auto hammer = cueOf(Faction::Confederation, 9);
        const auto rhino = cueOf(Faction::Resistance, 10);
        const int high = static_cast<int>(Cue::FireCannonHigh), usual = static_cast<int>(Cue::FireCannon);
        if (!zeus.count(high) || zeus.count(usual) || !rhino.count(high) || rhino.count(usual) || !hammer.count(usual)
            || hammer.count(high))
        {
            std::cerr << "[Test] FAILED: Zeus and Rhino fire the high cannon sound, the Hammer the usual one (zeus "
                      << zeus.count(high) << "/" << zeus.count(usual) << ", rhino " << rhino.count(high) << "/"
                      << rhino.count(usual) << ", hammer " << hammer.count(high) << "/" << hammer.count(usual) << ")"
                      << std::endl;
            return false;
        }
        // The rocket launcher of the researched Zeus fires beside the cannon; the base form has the cannon only
        {
            auto firing = [&](int arch, int &rockets, int &cannons) {
                entities.Clear();
                entities.TakeSoundCues();
                entities.SpawnUnitByArchetype(arch, 20, 20, Faction::Confederation, 0);
                const int target = entities.SpawnUnitByArchetype(0, 24, 20, Faction::Resistance, 1);
                if (Unit *t = entities.GetUnit(target))
                {
                    t->moveSpeed = 0.0f;
                    t->attackDamage = 0;
                    t->maxHp = t->currentHp = 100000;
                }
                for (int i = 0; i < 240; ++i)
                    entities.Update(0.05f, map, *app.m_pathfinder);
                rockets = cannons = 0;
                for (const auto &c : entities.TakeSoundCues())
                {
                    rockets += c.cue == Cue::FireRocket ? 1 : 0;
                    cannons += c.cue == Cue::FireCannonHigh ? 1 : 0;
                }
            };
            int baseRockets = 0, baseCannons = 0, fullRockets = 0, fullCannons = 0;
            firing(17, baseRockets, baseCannons);
            firing(11, fullRockets, fullCannons);
            std::cout << "  - Zeus in 12 s: base " << baseCannons << " cannon shots / " << baseRockets
                      << " rockets, with the launcher " << fullCannons << " / " << fullRockets << std::endl;
            if (baseRockets != 0 || baseCannons < 2 || fullCannons < 2 || fullRockets < 3)
            {
                std::cerr << "[Test] FAILED: the Zeus fires its cannon; the launcher only after the technology"
                          << std::endl;
                return false;
            }
        }
        if (app.m_sound && (!app.m_sound->SfxConfig(WorldSfx::FireCannonHigh)
                            || app.m_sound->SfxConfig(WorldSfx::FireCannonHigh)->clips.empty()))
        {
            std::cerr << "[Test] FAILED: the event fire_cannon_high needs its clip" << std::endl;
            return false;
        }
    }
    std::cout << "  - Zeus turret on research, 60 s research, radar +15, min_gain 0.07, high cannon sound: ok"
              << std::endl;
    return true;
}

bool HeadlessTest::RunReplayTest(Application &app)
{
    std::cout << "[Test] Validating replays (.awr): record a match, play the file back..." << std::endl;
    EntityManager &entities = *app.m_entities;
    TileMap &map = *app.m_map;
    Pathfinder &pf = *app.m_pathfinder;
    Economy &economy = *app.m_economy;
    AIController &ai = *app.m_aiController;
    MissionController &mission = *app.m_mission;
    Simulation &sim = app.m_gameplayController->simulation();
    namespace fs = std::filesystem;

    MatchSetup setup;
    setup.mapPath = LoaderResources::MapPath("m26");
    setup.mapHash = MapFiles::HashOf(setup.mapPath);
    setup.seed = 777;
    setup.local = 0;
    setup.slots[0] = {true, Faction::Resistance, 0, 1, true};
    setup.slots[1] = {true, Faction::Confederation, 1, 2, true};
    setup.startPosition[0] = 0;
    setup.startPosition[1] = 1;
    constexpr uint32_t kTicksPerTurn = 2;
    constexpr int kTurns = 400;

    std::vector<std::vector<uint8_t>> recordedTurns;
    std::vector<std::pair<uint32_t, uint64_t>> recordedSums;
    uint64_t recordedFinal = 0;
    const float dt = static_cast<float>(SIMULATION_TIMESTEP);

    // Plays the loaded match; `source` makes the bundle of each turn
    auto play = [&](const std::function<bool(uint32_t turn, Net::TurnBundle &)> &source,
                    const std::function<void(uint32_t tick, uint64_t sum)> &onSum) -> uint64_t {
        sim.startMatch(setup);
        SimContext ctx {entities, map, pf, economy, ai};
        for (uint32_t tick = 0; tick < kTurns * kTicksPerTurn; ++tick)
        {
            if (tick % kTicksPerTurn == 0)
            {
                Net::TurnBundle b;
                if (source(tick / kTicksPerTurn, b))
                    NetMatch::ScheduleBundle(sim, b, kTicksPerTurn);
            }
            sim.ApplyCommands(ctx);
            sim.UpdateResearch(dt, ctx);
            entities.Update(dt, map, pf);
            sim.UpdateSquads(dt, ctx, ai.GetChokepoints());
            sim.UpdateEconomies(dt, ctx);
            if (tick % NetMatch::kHashIntervalTicks == 0)
                onSum(tick, sim.Checksum(ctx));
            sim.EndTick();
        }
        SimContext fin {entities, map, pf, economy, ai};
        return sim.Checksum(fin);
    };

    // --- Recording: scripted turns, as a host would have closed them
    if (!mission.LoadMatch(setup, map, entities, economy, pf))
        return false;
    std::vector<uint32_t> own, enemy;
    int enemyHq = 0;
    for (const Entity &e : entities.GetAllEntities())
    {
        if (e.owner == 0 && !e.isBuilding)
            own.push_back(static_cast<uint32_t>(e.id));
        if (e.owner == 1 && e.isBuilding && e.type == UnitType::Headquarters)
            enemyHq = e.id;
    }
    const Entity *hq = entities.GetEntity(enemyHq);
    if (own.empty() || !hq)
    {
        std::cerr << "[Test] FAILED: replay test setup" << std::endl;
        return false;
    }
    const int targetX = hq->tileX * TILE_WIDTH, targetY = hq->tileY * TILE_HEIGHT;
    auto script = [&](uint32_t turn, Net::TurnBundle &b) {
        b.turn = turn;
        std::vector<SimCommand> c0, c1;
        if (turn == 3)
        {
            SimCommand o;
            o.type = CommandType::OrderAt;
            o.ids = own;
            o.x = targetX;
            o.y = targetY;
            o.a = static_cast<int32_t>(OrderKind::AttackMove);
            c0.push_back(o);
        }
        if (turn % 10 == 5)
        {
            SimCommand v;
            v.type = CommandType::View;
            v.x = 1000 + static_cast<int32_t>(turn) * 3;
            v.y = 700;
            v.a = 200;
            c1.push_back(v);
        }
        if (!c0.empty())
            b.players.push_back({0, SimCommandCodec::Encode(c0)});
        if (!c1.empty())
            b.players.push_back({1, SimCommandCodec::Encode(c1)});
        return true;
    };
    recordedFinal = play(
        [&](uint32_t turn, Net::TurnBundle &b) {
            script(turn, b);
            recordedTurns.push_back(Net::encodeTurn(b));
            return true;
        },
        [&](uint32_t tick, uint64_t sum) { recordedSums.emplace_back(tick, sum); });

    // --- The file
    const fs::path dir = fs::temp_directory_path() / "oaow-replay-play-test";
    fs::remove_all(dir);
    Net::ReplayData data;
    data.setup = NetMatch::EncodeSetup(setup);
    data.turns = recordedTurns;
    data.checksums = recordedSums;
    data.meta = nlohmann::json({{"format", 1},
                                {"source", "client"},
                                {"ticks_per_turn", kTicksPerTurn},
                                {"local_slot", 0},
                                {"players", nlohmann::json::array({{{"slot", 0}, {"name", "Red"}},
                                                                   {{"slot", 1}, {"name", "Blue"}}})},
                                {"result", {{"outcome", "left"}, {"tick", kTurns * kTicksPerTurn}}}})
                    .dump();
    std::vector<uint8_t> mapBytes, balanceBytes;
    MapFiles::Read(setup.mapPath, mapBytes);
    Assets::ReadBytes(LoaderResources::weaponBalance(), balanceBytes);
    data.files["maps/m26.json"] = mapBytes;
    data.files["data/assets/data/weapon_balance.json"] = balanceBytes;
    const std::string path = (dir / "test.awr").string();
    std::string error;
    if (!Net::writeReplay(path, data, error))
    {
        std::cerr << "[Test] FAILED: writing the replay: " << error << std::endl;
        return false;
    }

    // --- Playing the file back
    {
        ReplayPlayer player;
        if (!player.Load(path, error))
        {
            std::cerr << "[Test] FAILED: loading the replay: " << error << std::endl;
            return false;
        }
        const ReplayPlayer::Info &info = player.info();
        float cx = 0, cy = 0, zoom = 0;
        const bool hasCamera = player.CameraOf(1, 120, cx, cy, zoom);
        // Turns 5, 15, ... 55 recorded a view of player 1; at tick 120 (turn 60) the one of turn 55 is current
        if (info.turns != kTurns || info.ticks != kTurns * kTicksPerTurn || info.players.size() != 2
            || info.players[0].name != "Red" || info.players[1].faction != Faction::Confederation || !hasCamera
            || cx != 1000.0f + 55.0f * 3.0f || cy != 700.0f || zoom != 2.0f)
        {
            std::cerr << "[Test] FAILED: replay info (turns " << info.turns << ", camera " << cx << "," << cy << ")"
                      << std::endl;
            return false;
        }
        MatchSetup played;
        if (!player.PrepareMatch(1, played, error) || played.local != 1 || !Assets::HasSessionOverlay())
        {
            std::cerr << "[Test] FAILED: preparing the replayed match: " << error << std::endl;
            return false;
        }
        if (!mission.LoadMatch(played, map, entities, economy, pf))
            return false;
        setup.local = played.local;
        const uint64_t finalSum = play(
            [&](uint32_t turn, Net::TurnBundle &) {
                player.ScheduleDue(sim, turn * kTicksPerTurn);
                return false; // the player schedules the turn itself
            },
            [&](uint32_t tick, uint64_t sum) { player.Verify(tick, sum); });
        std::cout << "  - Replayed " << info.turns << " turns: " << recordedSums.size()
                  << " recorded checksums, first difference at " << player.MismatchTick() << ", final "
                  << (finalSum == recordedFinal ? "equal" : "DIFFERENT") << std::endl;
        if (player.MismatchTick() >= 0 || finalSum != recordedFinal)
        {
            std::cerr << "[Test] FAILED: the replay must play back to the same world" << std::endl;
            return false;
        }
        // Another viewpoint changes nothing in the world
        player.Rewind();
    }
    if (Assets::HasSessionOverlay())
    {
        std::cerr << "[Test] FAILED: closing the replay must take its data away" << std::endl;
        return false;
    }

    // --- The "Open replay" screen lists the file and draws without trouble
    {
        const fs::path folder = ReplayRecorder::Directory();
        std::error_code ec;
        fs::create_directories(folder, ec);
        const fs::path listed = folder / "open-art-of-war-20000101-000000_test.awr";
        fs::copy_file(path, listed, fs::copy_options::overwrite_existing, ec);
        ReplayMenu menu;
        menu.Refresh();
        menu.SetNotice("notice");
        app.m_input->ResetFrameEvents();
        const ReplayMenu::Result r = menu.Update(*app.m_input, app.m_windowW, app.m_windowH);
        menu.Render(*app.m_renderer, *app.m_fontRenderer, app.m_windowW, app.m_windowH);
        fs::remove(listed, ec);
        if (r.back || !r.open.empty())
        {
            std::cerr << "[Test] FAILED: the replay screen must wait for the player" << std::endl;
            return false;
        }
    }

    // --- A file that is not a replay, a replay of another map
    {
        ReplayPlayer broken;
        std::ofstream((dir / "junk.awr").string(), std::ios::binary) << "this is not a replay";
        if (broken.Load((dir / "junk.awr").string(), error))
        {
            std::cerr << "[Test] FAILED: junk must not load" << std::endl;
            return false;
        }
        Net::ReplayData other = data;
        other.files.erase("maps/m26.json");
        MatchSetup unknown = setup;
        unknown.mapPath = "assets/maps/does_not_exist.json";
        unknown.mapHash = 0x1234;
        other.setup = NetMatch::EncodeSetup(unknown);
        Net::writeReplay((dir / "nomap.awr").string(), other, error);
        ReplayPlayer noMap;
        MatchSetup unused;
        if (!noMap.Load((dir / "nomap.awr").string(), error) || noMap.PrepareMatch(-1, unused, error) || error.empty()
            || Assets::HasSessionOverlay())
        {
            std::cerr << "[Test] FAILED: a replay whose map is missing must say so" << std::endl;
            return false;
        }
    }
    fs::remove_all(dir);
    std::cout << "  - Replay file, playback, cameras, missing map: ok" << std::endl;
    return true;
}

bool HeadlessTest::RunAiVehicleTest(Application &app)
{
    std::cout << "[Test] Validating that computer players build vehicles and send them (every difficulty)..."
              << std::endl;
    EntityManager &entities = *app.m_entities;
    TileMap &map = *app.m_map;
    Pathfinder &pf = *app.m_pathfinder;
    Economy &economy = *app.m_economy;
    MissionController &mission = *app.m_mission;
    GameplayController &gc = *app.m_gameplayController;
    Simulation &sim = gc.simulation();
    const int minutes = std::getenv("OAOW_AI_MINUTES") ? std::atoi(std::getenv("OAOW_AI_MINUTES")) : 10;
    bool ok = true;
    for (int level = 0; level <= 3; ++level)
    {
        // Two allied computer players of the same difficulty against an idle person (nobody is attacked on the way)
        MatchSetup setup;
        setup.mapPath = LoaderResources::MapPath("m26");
        setup.seed = 4242;
        setup.local = 0;
        setup.slots[0] = {true, Faction::Resistance, 0, 1, true, 1};
        setup.slots[1] = {true, Faction::Confederation, 1, 2, false, static_cast<uint8_t>(level)};
        setup.slots[2] = {true, Faction::Resistance, 1, 3, false, static_cast<uint8_t>(level)};
        for (int i = 0; i < 3; ++i)
            setup.startPosition[i] = static_cast<int8_t>(i);
        if (!mission.LoadMatch(setup, map, entities, economy, pf))
            return false;
        gc.ResetForMission(mission, *app.m_hudController, entities, map);
        SimContext ctx {entities, map, pf, economy, *app.m_aiController};
        const float dt = static_cast<float>(SIMULATION_TIMESTEP);
        std::set<int> vehicles[3], initial;
        int factories[3] = {};
        for (const Entity &e : entities.GetAllEntities())
            if (const Unit *u = e.AsUnit(); u && !u->IsInfantry())
                initial.insert(u->id);
        for (int tick = 0; tick < minutes * 60 * 20; ++tick)
        {
            sim.ApplyCommands(ctx);
            sim.UpdateResearch(dt, ctx);
            entities.Update(dt, map, pf);
            sim.UpdateSquads(dt, ctx, app.m_aiController->GetChokepoints());
            sim.UpdateEconomies(dt, ctx);
            for (auto &ai : gc.m_matchAis)
                ai->Update(dt, map, entities, pf);
            sim.EndTick();
            if (std::getenv("OAOW_TEST_VERBOSE") && tick == 41)
            {
                const int goals[][2] = {{68, 9}, {65, 12}, {64, 20}, {70, 24}, {68, 22}};
                for (const auto &g : goals)
                {
                    std::vector<PathNode> probe;
                    bool partial = false;
                    const bool found = pf.FindPath(71, 9, g[0], g[1], probe, 0, &partial);
                    std::cout << "    probe (71,9)->(" << g[0] << "," << g[1] << "): found " << found << " partial "
                              << partial << " len " << probe.size() << " passableGoal " << map.IsPassable(g[0], g[1])
                              << std::endl;
                }
            }
            if (std::getenv("OAOW_TEST_VERBOSE") && tick == 40)
            {
                for (int y = 4; y <= 26; ++y)
                {
                    std::cout << "    ";
                    for (int x = 58; x <= 79; ++x)
                    {
                        char c = map.IsPassable(x, y) ? '.' : '#';
                        if (map.GetWall(x, y) > 0)
                            c = 'w';
                        for (const Entity &e : entities.GetAllEntities())
                            if (e.alive && !e.isBuilding && e.tileX == x && e.tileY == y)
                                c = 'u';
                        std::cout << c;
                    }
                    std::cout << std::endl;
                }
            }
            if (std::getenv("OAOW_TEST_VERBOSE") && tick > 0 && tick % 6000 == 0)
            {
                for (auto &mai : gc.m_matchAis)
                {
                    int idle = 0, found = 0, partialOnly = 0, none = 0;
                    for (const Entity &e : entities.GetAllEntities())
                    {
                        const Unit *u = e.AsUnit();
                        if (!u || !u->alive || u->owner != mai->m_player || u->state != ActionState::Idle)
                            continue;
                        ++idle;
                        std::vector<PathNode> probe;
                        bool partial = false;
                        const bool ok = pf.FindPath(
                            u->tileX, u->tileY, mai->m_stagingX, mai->m_stagingY, probe, u->id, &partial);
                        (ok ? found : (partial && !probe.empty() ? partialOnly : none))++;
                    }
                    std::cout << "    t=" << tick / 20 << "s ai " << int(mai->m_player) << ": idle " << idle
                              << ", path to staging " << found << ", partial only " << partialOnly << ", none " << none
                              << std::endl;
                }
            }
            if (tick % 20 == 0)
            {
                for (int p = 1; p <= 2; ++p)
                    factories[p] = 0;
                for (const Entity &e : entities.GetAllEntities())
                {
                    if (!e.alive || e.owner < 1 || e.owner > 2)
                        continue;
                    if (e.isBuilding && e.type == UnitType::Factory)
                        ++factories[e.owner];
                    if (const Unit *u = e.AsUnit(); u && !u->IsInfantry() && !initial.count(u->id))
                        vehicles[e.owner].insert(u->id); // trained during the match
                }
            }
        }
        // Vehicles that left the base: farther than 12 tiles from their own headquarters at the end
        int away = 0;
        for (const Entity &e : entities.GetAllEntities())
        {
            const Unit *u = e.AsUnit();
            if (!u || !u->alive || u->IsInfantry() || e.owner < 1 || e.owner > 2)
                continue;
            for (const Entity &h : entities.GetAllEntities())
            {
                if (h.alive && h.isBuilding && h.type == UnitType::Headquarters && h.owner == e.owner
                    && std::abs(h.tileX - u->tileX) + std::abs(h.tileY - u->tileY) > 12)
                {
                    ++away;
                    break;
                }
            }
        }
        if (std::getenv("OAOW_TEST_VERBOSE"))
        {
            for (auto &mai : gc.m_matchAis)
            {
                std::cout << "    [ai " << int(mai->m_player) << "] credits " << mai->m_credits << " vehicleQueue "
                          << mai->m_vehicleQueue.size() << " infantryQueue " << mai->m_infantryQueue.size()
                          << " wave " << mai->m_waveCount << " plan " << mai->m_currentPlan.archetypes.size() << " ready "
                          << mai->m_readyUnits << " units " << std::count_if(entities.GetAllEntities().begin(), entities.GetAllEntities().end(), [&](const Entity &e) { return e.alive && !e.isBuilding && e.owner == mai->m_player; })
                          << " lab " << mai->m_hasLab << " researchActive " << mai->m_researchActive << " idx "
                          << mai->m_researchIndex << std::endl;
            }
            for (auto &mai : gc.m_matchAis)
            {
                std::cout << "    [ai " << int(mai->m_player) << " units] staging " << mai->m_stagingX << ","
                          << mai->m_stagingY << ":";
                int shown = 0;
                for (const Entity &e : entities.GetAllEntities())
                {
                    const Unit *u = e.AsUnit();
                    if (u && u->alive && u->owner == mai->m_player && shown++ < 40)
                        std::cout << " (" << u->tileX << "," << u->tileY << (u->IsInfantry() ? "i" : "V")
                                  << (mai->m_squadController.IsSquadMember(u->id) ? "s" : "") << ")";
                }
                std::cout << std::endl;
                int detail = 0;
                for (const Entity &e : entities.GetAllEntities())
                {
                    const Unit *u = e.AsUnit();
                    if (u && u->alive && u->owner == mai->m_player && detail < 1)
                    {
                        std::vector<PathNode> probe;
                        bool partial = false;
                        const bool found
                            = pf.FindPath(u->tileX, u->tileY, mai->m_stagingX, mai->m_stagingY, probe, u->id, &partial);
                        std::cout << "      path " << u->tileX << "," << u->tileY << " -> staging: found " << found
                                  << " partial " << partial << " len " << probe.size() << std::endl;
                    }
                    if (u && u->alive && u->owner == mai->m_player && detail++ < 4)
                        std::cout << "      unit " << u->id << " at " << u->tileX << "," << u->tileY << " state "
                                  << int(u->state) << " waitingPath " << u->waitingForPath << " hasTarget "
                                  << u->HasMoveTarget() << " target " << u->targetTileX << "," << u->targetTileY
                                  << " pathLeft " << (u->path.size() > u->pathIndex ? u->path.size() - u->pathIndex : 0)
                                  << " blocked " << u->blockedTimer << " waitBlocked " << u->waitBlockedTimer
                                  << std::endl;
                }
            }
            for (const Entity &e : entities.GetAllEntities())
            {
                const Building *fb = e.AsBuilding();
                if (fb && fb->alive && fb->type == UnitType::Factory && fb->owner >= 1)
                {
                    int sx = 0, sy = 0;
                    const bool spawn = entities.GetBuildingSpawnPoint(fb->id, map, sx, sy);
                    std::cout << "    [factory of " << int(fb->owner) << "] at " << fb->tileX << "," << fb->tileY
                              << " progress " << fb->buildProgress << " producing " << fb->isProducing << " timer "
                              << fb->productionTimer << " spawn " << spawn << " (" << sx << "," << sy << ")"
                              << std::endl;
                }
            }
        }
        std::cout << "  - Difficulty " << level << ": factories " << factories[1] << "/" << factories[2]
                  << ", vehicles fielded " << vehicles[1].size() << "/" << vehicles[2].size()
                  << ", vehicles away from base " << away << std::endl;
        if (vehicles[1].empty() || vehicles[2].empty())
        {
            std::cerr << "[Test] FAILED: a computer player of difficulty " << level << " never fielded a vehicle in "
                      << minutes << " minutes" << std::endl;
            ok = false;
        }
    }
    return ok;
}

bool HeadlessTest::RunFogBlockTest(Application &app)
{
    std::cout << "[Test] Validating units sent through the fog against enemy buildings that shut a passage..."
              << std::endl;
    EntityManager &entities = *app.m_entities;
    TileMap &map = *app.m_map;
    Pathfinder &pf = *app.m_pathfinder;
    const float dt = static_cast<float>(SIMULATION_TIMESTEP);
    bool ok = true;
    // 0: attack-move, 1: plain move order
    for (int mode = 0; mode < 2; ++mode)
    {
        entities.Clear();
        map.InitBlankArena(48, 32, 0);
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
        // A wall of rock across the map with a three tile gap (y 14..16), shut by an enemy building
        for (int y = 0; y < 32; ++y)
            if (y < 14 || y > 16)
                map.SetPassable(30, y, false);
        map.SetFogEnabled(true);
        map.ResetAllFog(FogState::Hidden);
        const PlayerId me = Players::OwnerOf(Faction::Resistance);
        const int blocker = entities.SpawnBuildingByIndex(1, 29, 14, Faction::Confederation, 1.0f); // 3x3 power plant
        for (int ty = 14; ty <= 16; ++ty)
            for (int tx = 29; tx <= 31; ++tx)
                map.SetPassable(tx, ty, false);
        std::vector<int> group;
        for (int i = 0; i < 6; ++i)
            group.push_back(entities.SpawnUnitByArchetype(0, 5 + i % 3, 14 + i / 3 * 2, Faction::Resistance, me));
        if (mode == 0)
            entities.IssueAttackMoveOrder(group, 40, 15, pf);
        else
            entities.IssueMoveOrder(group, 40, 15, pf, false);
        for (float t = 0.0f; t < 150.0f; t += dt)
            entities.Update(dt, map, pf);
        const Building *b = entities.GetBuilding(blocker);
        const int hp = b && b->alive ? b->currentHp : 0;
        const int maxHp = b ? b->maxHp : 1;
        int through = 0, idleAtGap = 0;
        for (int id : group)
        {
            const Unit *u = entities.GetUnit(id);
            if (!u)
                continue;
            through += u->tileX > 31 ? 1 : 0;
            idleAtGap += (u->tileX >= 24 && u->tileX <= 28 && u->state == ActionState::Idle) ? 1 : 0;
        }
        std::cout << "  - " << (mode == 0 ? "Attack-move" : "Move order") << ": building " << hp << "/" << maxHp
                  << " hp, " << through << " units beyond the gap, " << idleAtGap << " standing idle at it"
                  << std::endl;
        // The building that shuts the passage is broken down (it is gone from the list once it has fallen) and the
        // troops go on, whether they were sent by attack-move or by a plain move order
        const bool broken = !b || !b->alive || hp < maxHp;
        if (!broken || through == 0)
        {
            std::cerr << "[Test] FAILED: units sent through the fog stand at a shut passage and do not attack the "
                         "enemy building that blocks it (mode "
                      << mode << ")" << std::endl;
            ok = false;
        }
        map.SetFogEnabled(false);
    }
    return ok;
}

bool HeadlessTest::RunBuildRulesTest(Application &app)
{
    std::cout << "[Test] Validating: no headquarters, no construction; players without buildings are out..."
              << std::endl;
    EntityManager &entities = *app.m_entities;
    TileMap &map = *app.m_map;
    auto fail = [&](const std::string &msg) {
        std::cerr << "[Test] Error: " << msg << std::endl;
        return false;
    };
    // A blank arena: a person without a headquarters builds nothing (buildings, walls, a new headquarters)
    entities.Clear();
    map.InitBlankArena(60, 40, 0);
    map.SetFogEnabled(false);
    app.m_pathfinder->SetMap(&map);
    app.m_pathfinder->SetEntityManager(&entities);
    const PlayerId res = Players::OwnerOf(Faction::Resistance);
    if (entities.CanPlaceBuilding(30, 20, 2, 2, map, res) || entities.CanPlaceBuilding(30, 20, 1, 1, map, res)
        || entities.CanPlaceBuilding(30, 20, 3, 4, map, res))
        return fail("without a headquarters a building, a wall and a new headquarters must be refused");
    // A headquarters under construction does not count, a finished one opens the zone
    const int hq = entities.SpawnBuildingByIndex(8, 28, 18, Faction::Resistance, 0.4f);
    if (entities.CanPlaceBuilding(34, 18, 2, 2, map, res))
        return fail("a headquarters under construction must not allow construction");
    if (Building *b = entities.GetBuilding(hq))
        b->buildProgress = 1.0f;
    if (!entities.CanPlaceBuilding(34, 18, 2, 2, map, res) || !entities.CanPlaceBuilding(34, 22, 1, 1, map, res))
        return fail("a finished headquarters must allow construction in its zone");
    // It falls: construction is over again
    if (Building *b = entities.GetBuilding(hq))
    {
        b->alive = false;
        b->state = ActionState::Die;
    }
    if (entities.CanPlaceBuilding(34, 18, 2, 2, map, res) || entities.CanPlaceBuilding(34, 22, 1, 1, map, res))
        return fail("a player whose headquarters fell must build nothing, walls included");
    // The income is the headquarters': none without one; with one it comes, and research stands still without power
    {
        Economy &eco = *app.m_economy;
        eco.SetPlayer(res);
        eco.Reset(0);
        eco.SetPlayer(res);
        for (int i = 0; i < 70; ++i)
            eco.Update(0.05f, entities, map, *app.m_pathfinder);
        if (eco.GetCredits() != 0)
            return fail("without a headquarters no credits may come in");
        const int hq2 = entities.SpawnBuildingByIndex(8, 28, 18, Faction::Resistance, 1.0f);
        for (int i = 0; i < 70; ++i)
            eco.Update(0.05f, entities, map, *app.m_pathfinder);
        if (eco.GetCredits() <= 0)
            return fail("a headquarters must bring income");
        const int lab = entities.SpawnBuildingByIndex(4, 36, 18, Faction::Resistance, 1.0f);
        if (Building *l = entities.GetBuilding(lab))
            l->isPowered = false;
        if (ResearchBuildingPowered(res, entities))
            return fail("a laboratory without power must not count for research");
        if (Building *l = entities.GetBuilding(lab))
            l->isPowered = true;
        if (!ResearchBuildingPowered(res, entities))
            return fail("a powered laboratory must count for research");
        entities.Clear();
        (void)hq2;
        map.InitBlankArena(60, 40, 0);
    }

    // A match with a computer player: it builds under the same rules, and is out when its last building is gone
    MatchSetup setup;
    setup.mapPath = LoaderResources::MapPath("m26");
    setup.seed = 5;
    setup.local = 0;
    setup.slots[0] = {true, Faction::Resistance, 0, 1, true, 1};
    setup.slots[1] = {true, Faction::Confederation, 1, 2, false, 1};
    setup.startPosition[0] = 0;
    setup.startPosition[1] = 1;
    if (!app.m_mission->LoadMatch(setup, map, entities, *app.m_economy, *app.m_pathfinder))
        return false;
    app.m_gameplayController->ResetForMission(*app.m_mission, *app.m_hudController, entities, map);
    const PlayerId ai = 1;
    int aiHq = 0, hqX = 0, hqY = 0;
    for (const Entity &e : entities.GetAllEntities())
        if (e.owner == ai && e.isBuilding && e.type == UnitType::Headquarters)
        {
            aiHq = e.id;
            hqX = e.tileX;
            hqY = e.tileY;
        }
    if (aiHq == 0)
        return fail("the computer player has no headquarters");
    map.SetFogEnabled(false);
    // (somewhere around its headquarters that is free: scan for a tile the rules accept)
    int freeX = -1, freeY = -1;
    for (int dy = -10; dy <= 10 && freeX < 0; ++dy)
        for (int dx = -10; dx <= 10 && freeX < 0; ++dx)
            if (entities.CanPlaceBuilding(hqX + dx, hqY + dy, 2, 2, map, ai))
            {
                freeX = hqX + dx;
                freeY = hqY + dy;
            }
    if (freeX < 0)
        return fail("the computer player must be able to build around its headquarters");
    // Destroy its headquarters: it can build nothing
    if (Entity *h = entities.GetEntity(aiHq))
    {
        h->alive = false;
        h->state = ActionState::Die;
    }
    if (entities.CanPlaceBuilding(freeX, freeY, 2, 2, map, ai))
        return fail("a computer player without a headquarters must build nothing");
    // Its other buildings still stand: it is not out yet; the person is not touched
    if (!entities.UpdateElimination(map).empty())
        return fail("a player with buildings left is not out");
    for (Entity &e : entities.GetAllEntities())
        if (e.owner == ai && e.isBuilding)
        {
            e.alive = false;
            e.state = ActionState::Die;
        }
    const std::vector<PlayerId> out = entities.UpdateElimination(map);
    int unitsLeft = 0;
    for (const Entity &e : entities.GetAllEntities())
        if (e.owner == ai && e.alive && e.state != ActionState::Die)
            ++unitsLeft;
    if (out.size() != 1 || out[0] != ai || !entities.IsEliminated(ai) || entities.IsEliminated(0) || unitsLeft != 0)
    {
        std::cerr << "[Test] Error: a player without buildings is out with all its forces (out " << out.size()
                  << ", units left " << unitsLeft << ")" << std::endl;
        return false;
    }
    if (!entities.UpdateElimination(map).empty())
        return fail("a player drops out once");
    std::cout << "  - No headquarters: nothing built; no buildings: out with all units: ok" << std::endl;
    return true;
}

bool HeadlessTest::RunMatchAiTest(Application &app)
{
    std::cout << "[Test] Validating computer players in a match..." << std::endl;
    EntityManager &entities = *app.m_entities;
    TileMap &map = *app.m_map;
    Pathfinder &pf = *app.m_pathfinder;
    Economy &economy = *app.m_economy;
    MissionController &mission = *app.m_mission;
    GameplayController &gc = *app.m_gameplayController;
    Simulation &sim = gc.simulation();

    // A person (Resistance) against two computer players of both factions in one team
    MatchSetup setup;
    setup.mapPath = LoaderResources::MapPath("m26");
    setup.seed = 777;
    setup.local = 0;
    setup.slots[0] = {true, Faction::Resistance, 0, 1, true, 1};
    setup.slots[1] = {true, Faction::Confederation, 1, 2, false, 2};
    setup.slots[2] = {true, Faction::Resistance, 1, 3, false, 1};
    for (int i = 0; i < 3; ++i)
        setup.startPosition[i] = static_cast<int8_t>(i);

    struct Outcome
    {
        std::vector<uint64_t> sums;
        int aiBuildings {0}, aiUnits {0}, ais {0};
    };
    auto run = [&]() {
        Outcome o;
        if (!mission.LoadMatch(setup, map, entities, economy, pf))
            return o;
        gc.ResetForMission(mission, *app.m_hudController, entities, map);
        o.ais = static_cast<int>(gc.m_matchAis.size());
        SimContext ctx {entities, map, pf, economy, *app.m_aiController};
        const float dt = static_cast<float>(SIMULATION_TIMESTEP);
        for (int tick = 0; tick < 3000; ++tick)
        {
            sim.ApplyCommands(ctx);
            sim.UpdateResearch(dt, ctx);
            entities.Update(dt, map, pf);
            sim.UpdateSquads(dt, ctx, app.m_aiController->GetChokepoints());
            sim.UpdateEconomies(dt, ctx); // the local economy included
            for (auto &ai : gc.m_matchAis)
                ai->Update(dt, map, entities, pf);
            if (tick % 100 == 0)
                o.sums.push_back(sim.Checksum(ctx));
            sim.EndTick();
        }
        for (const Entity &e : entities.GetAllEntities())
        {
            if (!e.alive || (e.owner != 1 && e.owner != 2))
                continue;
            (e.isBuilding ? o.aiBuildings : o.aiUnits)++;
        }
        return o;
    };
    {
        // A custom game against several computer players is a local match: not an online one, it restarts alone
        MatchSetup local = setup;
        local.localOnly = true;
        if (!mission.LoadMatch(local, map, entities, economy, pf) || !mission.IsMatch() || mission.IsOnlineMatch()
            || !mission.Restart(LoaderResources::kDefaultRoot, map, entities, economy, pf) || !mission.IsMatch()
            || mission.IsOnlineMatch())
        {
            std::cerr << "[Test] FAIL: a local match must load, stay local and restart" << std::endl;
            return false;
        }
    }
    const Outcome first = run();
    const Outcome second = run();
    std::cout << "  - " << first.ais << " computer players: " << first.aiBuildings << " buildings, " << first.aiUnits
              << " units after 150 s (started with 6 / 8), deterministic " << (first.sums == second.sums) << std::endl;
    SetPlayerFaction(Faction::Resistance);
    entities.Clear();
    sim.Reset();
    gc.m_matchAis.clear();
    ResetResearch();
    if (first.ais != 2 || first.sums.empty() || first.sums != second.sums)
    {
        std::cerr << "[Test] FAILED: computer players in a match" << std::endl;
        return false;
    }
    if (first.aiBuildings <= 6 && first.aiUnits <= 8)
    {
        std::cerr << "[Test] FAILED: the computer players did nothing" << std::endl;
        return false;
    }
    return true;
}

bool HeadlessTest::RunGroupMovementTest(Application &app)
{
    // Group movement quality: no stacking/teleports, no single-file "train", everyone arrives
    std::cout << "[Test] Validating Group Movement (no overlap, no teleports, no follow-the-leader train)..."
              << std::endl;
    {
        struct GroupRunStats
        {
            int overlapTicks = 0; // ticks where two units shared a tile or one teleported
            int teleports = 0;    // per-tick jumps > 1 tile
            int snaps = 0;        // per-tick pixel jumps faster than the unit can move (visible jerk)
            int reversals = 0;    // per-tick direction flips (unit shaking back and forth)
            int arrived = 0;
            int total = 0;
            float maxStall = 0.0f;   // longest time a unit made no pixel progress while still ordered
            float trainRatio = 0.0f; // share of moving units whose tile was occupied by a mover last tick
            float settleTime = -1.0f;
        };

        auto runGroup = [&](const std::vector<std::pair<int, std::pair<int, int>>> &spawns,
                            int goalX,
                            int goalY,
                            float maxTime,
                            bool headOn) -> GroupRunStats {
            GroupRunStats st;
            app.m_entities->Clear();
            app.m_map->InitBlankArena(60, 40, 0);
            app.m_pathfinder->SetMap(app.m_map.get());
            app.m_pathfinder->SetEntityManager(app.m_entities.get());

            std::vector<int> ids;
            for (const auto &s : spawns)
            {
                int id = app.m_entities->SpawnUnitByArchetype(
                    s.first, s.second.first, s.second.second, Faction::Resistance);
                if (id > 0)
                    ids.push_back(id);
            }
            st.total = static_cast<int>(ids.size());

            if (headOn)
            {
                // Two halves ordered through each other
                std::vector<int> left, right;
                for (int id : ids)
                {
                    if (const Unit *u = app.m_entities->GetUnit(id))
                        (u->tileX < 30 ? left : right).push_back(id);
                }
                app.m_entities->IssueMoveOrder(left, 42, 20, *app.m_pathfinder);
                app.m_entities->IssueMoveOrder(right, 16, 20, *app.m_pathfinder);
            }
            else
            {
                app.m_entities->IssueMoveOrder(ids, goalX, goalY, *app.m_pathfinder);
            }

            std::map<int, std::pair<float, float>> lastPx;
            std::map<int, std::pair<float, float>> lastDelta;
            std::map<int, std::pair<int, int>> lastTile;
            std::map<int, float> stall;
            int moverSamples = 0, trainSamples = 0;
            const float dt = 1.0f / 30.0f;

            for (float t = 0.0f; t < maxTime; t += dt)
            {
                // Who was moving and where, BEFORE the tick
                std::set<std::pair<int, int>> moverTilesPrev;
                for (int id : ids)
                {
                    if (const Unit *u = app.m_entities->GetUnit(id); u && u->state == ActionState::Move)
                        moverTilesPrev.insert({u->tileX, u->tileY});
                }

                app.m_entities->Update(dt, *app.m_map, *app.m_pathfinder);

                std::set<std::pair<int, int>> occ;
                bool overlap = false, anyOrdered = false;
                for (int id : ids)
                {
                    Unit *u = app.m_entities->GetUnit(id);
                    if (!u)
                        continue;
                    if (!occ.insert({u->tileX, u->tileY}).second)
                        overlap = true;

                    float px = u->GetWorldPixelX(), py = u->GetWorldPixelY();
                    auto itT = lastTile.find(id);
                    if (itT != lastTile.end()
                        && std::max(std::abs(itT->second.first - u->tileX), std::abs(itT->second.second - u->tileY))
                               > 1)
                    {
                        st.teleports++;
                        overlap = true;
                    }
                    if (auto itP = lastPx.find(id); itP != lastPx.end())
                    {
                        float ddx = px - itP->second.first, ddy = py - itP->second.second;
                        float maxStep = u->moveSpeed * dt * 60.0f + 0.5f;
                        if (std::hypot(ddx, ddy) > maxStep)
                            st.snaps++;
                        auto itD = lastDelta.find(id);
                        if (itD != lastDelta.end() && std::hypot(ddx, ddy) > 0.05f
                            && ddx * itD->second.first + ddy * itD->second.second < 0.0f)
                            st.reversals++;
                        if (std::hypot(ddx, ddy) > 0.05f)
                            lastDelta[id] = {ddx, ddy};
                    }
                    bool ordered = u->state == ActionState::Move || u->waitingForPath || u->HasMoveTarget();
                    if (ordered)
                    {
                        anyOrdered = true;
                        auto itP = lastPx.find(id);
                        float moved = (itP == lastPx.end())
                                          ? 99.0f
                                          : std::hypot(px - itP->second.first, py - itP->second.second);
                        stall[id] = (moved < 0.05f) ? stall[id] + dt : 0.0f;
                        st.maxStall = std::max(st.maxStall, stall[id]);
                    }
                    if (u->state == ActionState::Move && itT != lastTile.end()
                        && (itT->second.first != u->tileX || itT->second.second != u->tileY))
                    {
                        moverSamples++;
                        // entered a tile that a mover stood on a tick ago => trailing in its footsteps
                        if (moverTilesPrev.count({u->tileX, u->tileY}))
                            trainSamples++;
                    }
                    lastPx[id] = {px, py};
                    lastTile[id] = {u->tileX, u->tileY};
                }
                if (overlap)
                    st.overlapTicks++;
                if (!anyOrdered && st.settleTime < 0.0f)
                {
                    st.settleTime = t;
                    break;
                }
            }

            for (int id : ids)
            {
                const Unit *u = app.m_entities->GetUnit(id);
                if (!u)
                    continue;
                int gx = goalX, gy = goalY;
                if (headOn)
                {
                    gx = (lastTile[id].first > 30) ? 42 : 16;
                    gy = 20;
                }
                if (std::max(std::abs(u->tileX - gx), std::abs(u->tileY - gy)) <= 4)
                    st.arrived++;
                if (std::getenv("OAOW_TEST_VERBOSE")
                    && (u->state == ActionState::Move || u->waitingForPath || u->HasMoveTarget()))
                {
                    std::cout << "    [stuck] id=" << id << " tile=(" << u->tileX << "," << u->tileY << ") sub=("
                              << u->subX << "," << u->subY << ") state=" << static_cast<int>(u->state)
                              << " waiting=" << u->waitingForPath << " target=(" << u->targetTileX << ","
                              << u->targetTileY
                              << ") pathLeft=" << (u->path.size() > u->pathIndex ? u->path.size() - u->pathIndex : 0)
                              << " next="
                              << (u->pathIndex < u->path.size() ? std::to_string(u->path[u->pathIndex].x) + ","
                                                                      + std::to_string(u->path[u->pathIndex].y)
                                                                : std::string("-"))
                              << " wait=" << u->waitBlockedTimer << std::endl;
                }
            }
            st.trainRatio = moverSamples ? static_cast<float>(trainSamples) / moverSamples : 0.0f;
            return st;
        };

        // A) 12 mixed units in a 4x3 blob, marching 26 tiles east
        std::vector<std::pair<int, std::pair<int, int>>> blob;
        const int archs[4] = {0, 2, 8, 10};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                blob.push_back({archs[(r + c) % 4], {8 + c, 18 + r}});
        GroupRunStats a = runGroup(blob, 36, 19, 30.0f, false);
        std::cout << "  - Blob march: arrived " << a.arrived << "/" << a.total << ", overlapTicks=" << a.overlapTicks
                  << ", teleports=" << a.teleports << ", snaps=" << a.snaps << ", reversals=" << a.reversals
                  << ", maxStall=" << a.maxStall << "s, trainRatio=" << a.trainRatio << ", settle=" << a.settleTime
                  << "s" << std::endl;

        // B) Head-on: two groups of 6 crossing through each other
        std::vector<std::pair<int, std::pair<int, int>>> cross;
        for (int i = 0; i < 6; ++i)
        {
            cross.push_back({0, {14 + (i % 2), 18 + i / 2}});
            cross.push_back({0, {44 - (i % 2), 18 + i / 2}});
        }
        GroupRunStats b = runGroup(cross, 0, 0, 30.0f, true);
        std::cout << "  - Head-on crossing: arrived " << b.arrived << "/" << b.total
                  << ", overlapTicks=" << b.overlapTicks << ", teleports=" << b.teleports << ", snaps=" << b.snaps
                  << ", reversals=" << b.reversals << ", maxStall=" << b.maxStall << "s, settle=" << b.settleTime << "s"
                  << std::endl;

        // C) Order spam: re-target a mixed squad every 0.2s so most orders land while units are between tiles
        int reorderSnaps = 0;
        {
            app.m_entities->Clear();
            app.m_map->InitBlankArena(60, 40, 0);
            app.m_pathfinder->SetMap(app.m_map.get());
            app.m_pathfinder->SetEntityManager(app.m_entities.get());
            std::vector<int> squad;
            for (int i = 0; i < 6; ++i)
            {
                int id
                    = app.m_entities->SpawnUnitByArchetype(archs[i % 4], 28 + (i % 3), 19 + i / 3, Faction::Resistance);
                if (id > 0)
                    squad.push_back(id);
            }
            std::map<int, std::pair<float, float>> lastPx;
            const float dt = 1.0f / 30.0f;
            for (int tick = 0; tick < 150; ++tick)
            {
                if (tick % 6 == 0)
                {
                    bool east = (tick / 6) % 2 == 0;
                    app.m_entities->IssueMoveOrder(squad, east ? 40 : 16, east ? 14 : 26, *app.m_pathfinder);
                }
                app.m_entities->Update(dt, *app.m_map, *app.m_pathfinder);
                for (int id : squad)
                {
                    const Unit *u = app.m_entities->GetUnit(id);
                    if (!u)
                        continue;
                    float px = u->GetWorldPixelX(), py = u->GetWorldPixelY();
                    if (auto it = lastPx.find(id);
                        it != lastPx.end()
                        && std::hypot(px - it->second.first, py - it->second.second) > u->moveSpeed * dt * 60.0f + 0.5f)
                    {
                        reorderSnaps++;
                    }
                    lastPx[id] = {px, py};
                }
            }
            std::cout << "  - Order spam (re-target every 0.2s): snaps=" << reorderSnaps << std::endl;
        }

        bool ok = true;
        if (reorderSnaps > 0)
        {
            std::cerr << "[Test] Error: units snapped back when re-ordered mid-step!" << std::endl;
            ok = false;
        }
        if (a.arrived != a.total || b.arrived != b.total)
        {
            std::cerr << "[Test] Error: not all units reached the destination!" << std::endl;
            ok = false;
        }
        if (a.overlapTicks > 0 || b.overlapTicks > 0)
        {
            std::cerr << "[Test] Error: units overlapped / were teleported apart!" << std::endl;
            ok = false;
        }
        if (a.snaps > 0 || b.snaps > 0)
        {
            std::cerr << "[Test] Error: units jerked (pixel position jumped faster than move speed)!" << std::endl;
            ok = false;
        }
        // (a blob of 12 troops of very different speeds, the slowest Zeus at 0.6: three back-steps are fine)
        if (a.reversals > 3 || b.reversals > 12)
        {
            std::cerr << "[Test] Error: units shake back and forth while moving!" << std::endl;
            ok = false;
        }
        if (a.maxStall > 2.0f || b.maxStall > 3.0f)
        {
            std::cerr << "[Test] Error: units stalled in place too long!" << std::endl;
            ok = false;
        }
        if (a.trainRatio > 0.35f)
        {
            std::cerr << "[Test] Error: group degenerated into a single-file train!" << std::endl;
            ok = false;
        }
        if (a.settleTime < 0.0f || b.settleTime < 0.0f)
        {
            std::cerr << "[Test] Error: group never settled (endless shuffling)!" << std::endl;
            ok = false;
        }
        if (!ok)
            return false;
        std::cout << "  - Group movement validated: parallel lanes, no overlap, no train, clean arrival." << std::endl;
    }
    return true;
}

bool HeadlessTest::RunAiBehaviourTest(Application &app)
{
    std::cout << "[Test] Validating AI Director, Squad Controller & Chokepoint Handler..." << std::endl;
    AIController &ai = *app.m_aiController;
    EntityManager &entities = *app.m_entities;
    TileMap &map = *app.m_map;
    Pathfinder &pf = *app.m_pathfinder;
    const AIDebugConfig savedDebug = ai.GetDebugConfig();

    auto withDifficulty = [&](int level) {
        AIDebugConfig d = savedDebug;
        d.forceDebugOverride = true;
        d.debugDifficultyLevel = level;
        ai.SetDebugConfig(d);
    };
    auto count = [](const AIWavePlan &p, std::initializer_list<int> archs) {
        int n = 0;
        for (int a : p.archetypes)
        {
            for (int b : archs)
                if (a == b)
                    ++n;
        }
        return n;
    };
    auto fail = [&](const std::string &msg) {
        std::cerr << "[Test] Error: " << msg << std::endl;
        ai.SetDebugConfig(savedDebug);
        return false;
    };

    // A) Escalation curve: weak infantry first, power grows with the wave number and the difficulty
    int firstWaveSize[3] = {0, 0, 0};
    for (int level = 0; level <= 2; ++level)
    {
        withDifficulty(level);
        const AITacticsConfig &tac = ai.GetTactics();
        AIWavePlan w1 = ai.PlanWave(1, true, true);
        AIWavePlan w3 = ai.PlanWave(tac.vanguardWave, true, true); // the first wave with rocket troopers
        AIWavePlan w5 = ai.PlanWave(tac.assaultWave, true, true);  // the first wave with tanks
        AIWavePlan w7 = ai.PlanWave(tac.siegeWave, true, true);    // the first siege fist
        firstWaveSize[level] = static_cast<int>(w1.archetypes.size());
        if (w1.tier != AIWaveTier::Scout || count(w1, {1}) != static_cast<int>(w1.archetypes.size()))
        {
            return fail("wave 1 must be light infantry only (difficulty " + std::to_string(level) + ")");
        }
        int support = count(w3, {3});
        if (w3.tier != AIWaveTier::Vanguard || support < 1 || support > 2 || count(w3, {7, 9, 11, 13}) != 0)
        {
            return fail("the vanguard wave must be infantry with 1-2 rocket troopers");
        }
        if (w5.tier != AIWaveTier::Assault || count(w5, {7, 9, 11}) < 1)
        {
            return fail("the assault wave must bring tanks");
        }
        if (w1.archetypes.size() > w3.archetypes.size() || w3.archetypes.size() > w5.archetypes.size())
        {
            return fail("wave size must not shrink as the AI escalates");
        }
        // Tanks come earlier on Normal and above than on Easy
        if (level >= 1 && tac.assaultWave > 3)
            return fail("Normal/Hard AI must field tanks from the third wave");
        if (level == 0 && tac.assaultWave < 4)
            return fail("Easy AI sends its first tanks later than the others");
        if (w7.tier != AIWaveTier::Siege || count(w7, {13}) < 1)
        {
            return fail("the siege wave must be a siege fist with MLRS (difficulty " + std::to_string(level) + ")");
        }
        if (count(ai.PlanWave(5, false, false), {7, 9, 11, 13}) != 0)
        {
            return fail("without a factory the plan must not contain vehicles");
        }
    }
    if (!(firstWaveSize[0] < firstWaveSize[1] && firstWaveSize[1] < firstWaveSize[2]))
    {
        return fail("first wave must grow with difficulty (Easy < Normal < Hard)");
    }
    {
        AIDebugConfig d = savedDebug;
        d.forceDebugOverride = true;
        d.debugDifficultyLevel = 1;
        d.waveSpawnMultiplier = 2.0f;
        ai.SetDebugConfig(d);
        if (static_cast<int>(ai.PlanWave(1, true, true).archetypes.size()) != firstWaveSize[1] * 2)
        {
            return fail("wave_spawn_multiplier must scale the wave size");
        }
    }
    std::cout << "  - Escalation: wave 1 = " << firstWaveSize[0] << "/" << firstWaveSize[1] << "/" << firstWaveSize[2]
              << " riflemen (Easy/Normal/Hard), vanguard -> tanks -> siege fist." << std::endl;

    // B) Debug override of the difficulty
    withDifficulty(2);
    if (ai.GetEffectiveDifficulty() != AIDifficulty::Hard || !ai.GetTactics().syncToSlowest
        || ai.GetTactics().retreatLossRatio <= 0.0f)
    {
        return fail("debug override to Hard did not apply Hard tactics");
    }
    withDifficulty(0);
    if (ai.GetTactics().waitAtChokepoints || ai.GetTactics().leashTiles < 5.0f)
    {
        return fail("Easy tactics must allow a loose formation and skip chokepoint gathering");
    }
    ai.SetDebugConfig(savedDebug);
    if (ai.GetEffectiveDifficulty() != ai.GetDifficulty())
        return fail("disabling the override must restore the menu difficulty");
    std::cout << "  - Debug difficulty override switches tactics at runtime and restores the menu setting."
              << std::endl;

    // Helpers for the squad scenarios
    auto setupArena = [&]() {
        entities.Clear();
        map.InitBlankArena(40, 30, 0);
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
        ai.m_squadController.Clear();
    };
    auto buildWallWithGap = [&]() {
        for (int y = 0; y < 30; ++y)
        {
            if (y < 14 || y > 15)
                map.SetPassable(20, y, false);
        }
    };
    struct SquadRun
    {
        std::set<AISquadState> seen;
        int released = 0;
        bool assault = false;
        float vanguardMaxXEarly = 0.0f;
    };
    auto runSquad = [&](const std::vector<int> &ids, int tx, int ty, float maxTime, const std::vector<int> &vanguard) {
        SquadRun r;
        ai.m_chokes.Rebuild(map);
        ai.m_squadController.CreateSquad(ids, tx, ty, 1, entities, pf, ai.GetTactics());
        const float dt = 1.0f / 30.0f;
        for (float t = 0.0f; t < maxTime; t += dt)
        {
            entities.Update(dt, map, pf);
            ai.m_squadController.Update(dt, map, entities, pf, ai.m_chokes, ai.GetTactics());
            r.released += static_cast<int>(ai.m_squadController.TakeReleasedUnits().size());
            const auto &squads = ai.m_squadController.GetSquads();
            if (squads.empty())
                break;
            if (std::getenv("OAOW_TEST_VERBOSE")
                && (r.seen.insert(squads.front().state).second || static_cast<int>(t * 30) % 30 == 0))
            {
                const auto &sq = squads.front();
                std::cout << "    t=" << t << " " << SquadStateName(sq.state) << " n=" << sq.unitIds.size() << " c=("
                          << sq.centroidX << "," << sq.centroidY << ") dmax=" << sq.maxSpread
                          << " march=" << sq.marchIndex << "/" << sq.route.size() << " leader=" << sq.leaderId
                          << std::endl;
            }
            r.seen.insert(squads.front().state);
            if (t < 4.0f)
            {
                for (int id : vanguard)
                {
                    if (const Unit *u = entities.GetUnit(id))
                        r.vanguardMaxXEarly = std::max(r.vanguardMaxXEarly, u->GetWorldPixelX() / TILE_WIDTH);
                }
            }
            if (squads.front().state == AISquadState::Assault)
            {
                r.assault = true;
                break;
            }
        }
        return r;
    };

    // C) Chokepoint detection
    setupArena();
    buildWallWithGap();
    ai.m_chokes.Rebuild(map);
    if (!ai.m_chokes.IsChoke(20, 14) || !ai.m_chokes.IsChoke(20, 15) || ai.m_chokes.IsChoke(10, 10)
        || ai.m_chokes.IsChoke(20, 5))
    {
        return fail("chokepoint map must mark exactly the 2-tile gap in the wall");
    }
    std::cout << "  - Chokepoint detection marks the 2-tile wall gap only." << std::endl;

    // D) Corridor transit: Normal gathers in front of the gap, crosses single-file and deploys
    for (int level : {1, 0, 2})
    {
        withDifficulty(level);
        setupArena();
        buildWallWithGap();
        std::vector<int> ids;
        for (int i = 0; i < 8; ++i)
            ids.push_back(entities.SpawnUnitByArchetype(1, 6 + i % 4, 12 + i / 4, Faction::Confederation));
        SquadRun r = runSquad(ids, 34, 14, 120.0f, {});
        int crossed = 0;
        for (int id : ids)
        {
            if (const Unit *u = entities.GetUnit(id); u && u->alive && u->tileX >= 21)
                ++crossed;
        }
        bool approached = r.seen.count(AISquadState::ChokeApproach) > 0;
        static const char *kNames[] = {"Easy", "Normal", "Hard"};
        std::cout << "  - " << kNames[level] << " corridor transit: crossed " << crossed << "/8, released "
                  << r.released << ", pre-choke gather=" << (approached ? "yes" : "no")
                  << ", column=" << (r.seen.count(AISquadState::ChokeTransit) ? "yes" : "no")
                  << ", reached objective=" << (r.assault ? "yes" : "no") << std::endl;
        if (!r.assault || crossed < 7 || r.seen.count(AISquadState::ChokeTransit) == 0)
        {
            return fail("group failed to pass the chokepoint as a column");
        }
        if ((level >= 1) != approached)
            return fail("only Normal+ must gather in front of the chokepoint");
    }

    // D2) Big column through a 3-tile corridor: the leash scales with group size and passage width,
    //     so 16 units do not keep stopping because the column is longer than 3 tiles
    {
        withDifficulty(1);
        setupArena();
        for (int x = 14; x <= 26; ++x)
        {
            for (int y = 0; y < 30; ++y)
            {
                if (y < 13 || y > 15)
                    map.SetPassable(x, y, false);
            }
        }
        std::vector<int> column;
        for (int i = 0; i < 16; ++i)
            column.push_back(entities.SpawnUnitByArchetype(1, 4 + i % 4, 11 + i / 4, Faction::Confederation));
        ai.m_chokes.Rebuild(map);
        ai.m_squadController.CreateSquad(column, 34, 14, 1, entities, pf, ai.GetTactics());
        const float dt = 1.0f / 30.0f;
        int ticks = 0, regroupTicks = 0, regroupEntries = 0;
        bool reached = false;
        AISquadState prev = AISquadState::Marching;
        for (float t = 0.0f; t < 120.0f; t += dt)
        {
            entities.Update(dt, map, pf);
            ai.m_squadController.Update(dt, map, entities, pf, ai.m_chokes, ai.GetTactics());
            ai.m_squadController.TakeReleasedUnits();
            const auto &squads = ai.m_squadController.GetSquads();
            if (squads.empty())
                break;
            AISquadState st = squads.front().state;
            if (std::getenv("OAOW_TEST_VERBOSE") && (st != prev || ticks % 60 == 0))
            {
                const auto &g = squads.front();
                const Unit *l = entities.GetUnit(g.leaderId);
                std::cout << "    t=" << ticks / 30.0f << " " << SquadStateName(st) << " n=" << g.unitIds.size()
                          << " dmax=" << g.maxSpread << " leash=" << g.leashRadius << " leader=(" << (l ? l->tileX : -1)
                          << "," << (l ? l->tileY : -1) << ") march=" << g.marchIndex << "/" << g.route.size()
                          << std::endl;
            }
            ++ticks;
            if (st == AISquadState::Regrouping)
                ++regroupTicks;
            if (st == AISquadState::Regrouping && prev != AISquadState::Regrouping)
                ++regroupEntries;
            prev = st;
            if (st == AISquadState::Assault)
            {
                reached = true;
                break;
            }
        }
        float share = ticks ? static_cast<float>(regroupTicks) / ticks : 1.0f;
        std::cout << "  - 16-unit column through a 3-tile corridor: reached=" << (reached ? "yes" : "no") << ", time "
                  << ticks / 30.0f << "s, stops " << regroupEntries << ", waiting " << static_cast<int>(share * 100)
                  << "%" << std::endl;
        if (!reached || share > 0.25f)
            return fail("a long column must not keep stopping in a corridor");
    }

    // D3) Player control group (Ctrl+N then a move order): mixed infantry + tanks march as one squad with plain
    //     moves, the column stays within the leash and dissolves at the destination
    {
        setupArena();
        for (int x = 14; x <= 26; ++x)
        {
            for (int y = 0; y < 30; ++y)
            {
                if (y < 13 || y > 15)
                    map.SetPassable(x, y, false);
            }
        }
        std::vector<int> group;
        for (int i = 0; i < 12; ++i)
        {
            int arch = (i % 4 == 3) ? 8 : 0; // every 4th unit an Armadillo tank
            group.push_back(entities.SpawnUnitByArchetype(arch, 4 + i % 4, 11 + i / 4, Faction::Resistance));
        }
        ai.m_chokes.Rebuild(map);
        AITacticsConfig pt;
        pt.syncToSlowest = false;
        pt.meleeRangedPairs = false;
        pt.retreatLossRatio = 0.0f;
        AISquadController player;
        int sid = player.CreateSquad(group, 34, 14, 0, entities, pf, pt);
        AISquad *sq = player.FindSquad(sid);
        sq->attackMove = false;
        sq->playerSquad = true;
        const float dt = 1.0f / 30.0f;
        float t = 0.0f;
        int regroupTicks = 0, ticks = 0;
        for (; t < 120.0f; t += dt)
        {
            entities.Update(dt, map, pf);
            player.Update(dt, map, entities, pf, ai.m_chokes, pt);
            player.TakeReleasedUnits();
            if (player.GetSquads().empty())
                break;
            ++ticks;
            if (player.GetSquads().front().state == AISquadState::Regrouping)
                ++regroupTicks;
            if (std::getenv("OAOW_TEST_VERBOSE") && ticks % 30 == 0)
            {
                const auto &g = player.GetSquads().front();
                const Unit *l = entities.GetUnit(g.leaderId);
                std::cout << "    t=" << ticks / 30 << " " << SquadStateName(g.state) << " n=" << g.unitIds.size()
                          << " dmax=" << g.maxSpread << " leader=" << (l ? l->archetypeId : -1) << "("
                          << (l ? l->tileX : -1) << "," << (l ? l->tileY : -1)
                          << ") st=" << (l ? static_cast<int>(l->state) : -1) << " march=" << g.marchIndex << "/"
                          << g.route.size() << std::endl;
            }
        }
        for (int k = 0; k < 150; ++k)
            entities.Update(dt, map, pf); // released units finish the last move
        int arrived = 0;
        for (int id : group)
        {
            const Unit *u = entities.GetUnit(id);
            if (u && std::hypot(u->tileX - 34.0f, u->tileY - 14.0f) <= 5.0f)
                ++arrived;
        }
        std::cout << "  - Player group (12 mixed) through a corridor: dissolved after " << t << "s, arrived " << arrived
                  << "/12, waiting " << (ticks ? regroupTicks * 100 / ticks : 0) << "%" << std::endl;
        if (!player.GetSquads().empty())
            return fail("player group never reached its destination");
        if (arrived < 10)
            return fail("player group did not arrive together");
    }

    // D3b) A member that could only reach the group by a huge detour (long wall, opening at the far end)
    //      must not run across the map while the group gathers
    {
        entities.Clear();
        map.InitBlankArena(70, 40, 0);
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
        for (int x = 0; x <= 62; ++x)
            map.SetPassable(x, 15, false); // opening only at x 63..69
        std::vector<int> group;
        for (int i = 0; i < 5; ++i)
            group.push_back(entities.SpawnUnitByArchetype(0, 8 + i, 13, Faction::Resistance));
        const int cutOff = entities.SpawnUnitByArchetype(0, 10, 16, Faction::Resistance);
        group.push_back(cutOff);
        ai.m_chokes.Rebuild(map);
        AISquadController player;
        AITacticsConfig pt;
        int sid = player.CreateSquad(group, 10, 6, 0, entities, pf, pt);
        player.FindSquad(sid)->attackMove = false;
        player.FindSquad(sid)->playerSquad = true;
        int maxX = 0;
        const float dt = 1.0f / 30.0f;
        for (float t = 0.0f; t < 20.0f; t += dt)
        {
            entities.Update(dt, map, pf);
            player.Update(dt, map, entities, pf, ai.m_chokes, pt);
            player.TakeReleasedUnits();
            if (const Unit *u = entities.GetUnit(cutOff))
                maxX = std::max(maxX, u->tileX);
        }
        std::cout << "  - Cut-off member (wall opening 53 tiles away): farthest x " << maxX << std::endl;
        if (maxX > 20)
            return fail("a group member must not take a map-wide detour to its slot");
    }

    // D3c) Wave routes: around a hostile wall when the way around is short, through it when there is none,
    //      and out of a known tower's reach in open ground
    {
        const PlayerId me = ai.player();
        PlayerId foe = kNoPlayer;
        for (PlayerId p = 0; p < kMaxPlayers && foe == kNoPlayer; ++p)
        {
            if (Players::Hostile(p, me))
                foe = p;
        }
        if (foe == kNoPlayer)
            return fail("D3c: no hostile player");
        auto crossesWall = [&](const std::vector<PathNode> &route) {
            for (const PathNode &n : route)
            {
                if (map.GetWall(n.x, n.y) > 0)
                    return true;
            }
            return false;
        };
        entities.Clear();
        map.InitBlankArena(40, 30, 0);
        for (int y = 0; y <= 13; ++y)
            map.SetWall(20, y, PlayerFaction(), 100, foe); // open from y 14 on: a short way around
        std::vector<PathNode> around;
        const bool okAround = AISquadController::PlanRoute(map, entities, me, true, 5, 10, 35, 10, around);
        const bool aroundCrosses = crossesWall(around);
        for (int y = 14; y < 30; ++y)
            map.SetWall(20, y, PlayerFaction(), 100, foe); // no way around any more
        std::vector<PathNode> through;
        const bool okThrough = AISquadController::PlanRoute(map, entities, me, true, 5, 10, 35, 10, through);
        const bool throughCrosses = crossesWall(through);

        entities.Clear();
        map.InitBlankArena(60, 44, 0);
        const int towerId // a bunker: reach 10, room to pass it on either side
            = entities.SpawnBuildingByIndex(
                PlayerFaction() == Faction::Confederation ? 5 : 13, 29, 21, PlayerFaction(), 1.0f, foe);
        const Entity *tower = entities.GetEntity(towerId);
        std::vector<PathNode> safe;
        const bool okSafe = AISquadController::PlanRoute(map, entities, me, true, 3, 22, 57, 22, safe);
        float closest = 1e9f;
        if (tower)
        {
            const float tx = tower->GetUnitCenterPixelX() / TILE_WIDTH, ty = tower->GetUnitCenterPixelY() / TILE_HEIGHT;
            for (const PathNode &n : safe)
                closest = std::min(closest, DetMath::Hypot(n.x + 0.5f - tx, n.y + 0.5f - ty));
        }
        const float reach = tower ? tower->attackRange : 0.0f;
        std::cout << "  - Wave routes: short way around " << (okAround && !aroundCrosses ? "taken" : "NOT taken")
                  << ", walled off -> " << (okThrough && throughCrosses ? "breach" : "NO breach") << ", bunker reach "
                  << reach << " closest pass " << closest << std::endl;
        entities.Clear();
        if (!okAround || aroundCrosses)
            return fail("a wave must walk around a hostile wall when the detour is short");
        if (!okThrough || !throughCrosses)
            return fail("a wave must breach a hostile wall when there is no way around");
        if (!tower || !okSafe || closest <= reach)
            return fail("a wave route must keep out of a known bunker's reach when the ground allows");
    }

    // D3e) Paths are straight runs, not staircases (vehicles stop to turn before every changed step)
    {
        setupArena();
        auto turns = [&](int unitId, int sx, int sy, int tx, int ty, bool diag, bool &connected) {
            std::vector<PathNode> path;
            bool partial = false;
            pf.FindPath(sx, sy, tx, ty, path, unitId, &partial, diag);
            int n = 0;
            connected = !path.empty() && path.back().x == tx && path.back().y == ty;
            PathNode prev {sx, sy}, lastStep {0, 0};
            for (size_t i = 0; i < path.size(); ++i)
            {
                PathNode step {path[i].x - prev.x, path[i].y - prev.y};
                if (std::max(std::abs(step.x), std::abs(step.y)) != 1)
                    connected = false;
                if (!diag && step.x != 0 && step.y != 0)
                    connected = false;
                if (i > 0 && !(step == lastStep))
                    ++n;
                lastStep = step;
                prev = path[i];
            }
            return n;
        };
        const int fortress = entities.SpawnUnitByArchetype(7, 3, 3, Faction::Confederation);
        const int tankId = entities.SpawnUnitByArchetype(8, 3, 20, Faction::Resistance);
        bool fOk = false, tOk = false;
        const int fTurns = turns(fortress, 3, 3, 30, 14, true, fOk);
        const int tTurns = turns(tankId, 3, 20, 30, 26, true, tOk);
        std::cout << "  - Straight paths: Fortress " << fTurns << " turn(s), tank " << tTurns << " turn(s), valid "
                  << (fOk && tOk ? "yes" : "no") << std::endl;
        if (!fOk || !tOk || fTurns > 2 || tTurns > 2)
            return fail("paths must be straight runs without staircases");
    }

    // D3h) Firing delay modes from YAML: fixed cooldown, or random within [cooldown_min, cooldown_max]
    {
        YAML::Node fixedDoc = YAML::Load(
            "type: unit\narchetype_id: 0\nfaction: Resistance\ncombat:\n  weapons:\n"
            "    - name: Rifle\n      damage: 5\n      range: 4\n      cooldown: 0.8\n");
        YAML::Node randomDoc = YAML::Load(
            "type: unit\narchetype_id: 2\nfaction: Resistance\ncombat:\n  weapons:\n"
            "    - name: Rocket\n      damage: 30\n      range: 5\n      cooldown_mode: random\n"
            "      cooldown_min: 1.0\n      cooldown_max: 2.0\n");
        entities.LoadCharacterYaml(fixedDoc);
        entities.LoadCharacterYaml(randomDoc);
        setupArena();
        Unit *a = entities.GetUnit(entities.SpawnUnitByArchetype(0, 5, 5, Faction::Resistance));
        Unit *b = entities.GetUnit(entities.SpawnUnitByArchetype(2, 7, 5, Faction::Resistance));
        float fixedMin = 99, fixedMax = 0, randMin = 99, randMax = 0;
        for (int i = 0; i < 200; ++i)
        {
            const float fa = a->NextCooldown(), fb = b->NextCooldown();
            fixedMin = std::min(fixedMin, fa);
            fixedMax = std::max(fixedMax, fa);
            randMin = std::min(randMin, fb);
            randMax = std::max(randMax, fb);
        }
        std::cout << "  - Firing delay: fixed " << fixedMin << ".." << fixedMax << ", random " << randMin << ".."
                  << randMax << std::endl;
        app.m_entities->LoadCharacterConfigs("assets"); // back to the real data
        if (fixedMin != fixedMax || std::abs(fixedMin - 0.8f) > 0.001f)
            return fail("fixed firing delay must not vary");
        if (randMin < 1.0f || randMax > 2.0f || randMax - randMin < 0.5f)
            return fail("random firing delay must spread over [min, max]");
    }

    // D3e2) Vehicles in a group never turn around to sidestep a neighbour (that read as "strange diagonal moves")
    {
        int reversals = 0;
        for (int arch : {7, 8})
        {
            setupArena();
            std::vector<int> ids;
            for (int i = 0; i < 6; ++i)
                ids.push_back(entities.SpawnUnitByArchetype(arch, 5 + i % 3, 5 + i / 3, Faction::Resistance));
            std::vector<int> lastFacing;
            for (int id : ids)
                lastFacing.push_back(entities.GetUnit(id)->facingDirection);
            entities.IssueMoveOrder(ids, 15, 15, pf, false);
            std::vector<int> start(ids.size());
            for (int i = 0; i < 30 * 15; ++i)
            {
                entities.Update(1.0f / 30.0f, map, pf);
                for (size_t k = 0; k < ids.size(); ++k)
                {
                    const Unit *u = entities.GetUnit(ids[k]);
                    if (!u)
                        continue;
                    // a reversal: the hull ends up pointing opposite to where it pointed a moment ago
                    if (i % 15 == 0)
                    {
                        int d = std::abs(u->facingDirection - start[k]) % 8;
                        if (i > 0 && std::min(d, 8 - d) == 4)
                            ++reversals;
                        start[k] = u->facingDirection;
                    }
                }
            }
        }
        std::cout << "  - Group of 6 Fortresses / 6 tanks to a diagonal point: hull reversals " << reversals
                  << std::endl;
        if (reversals > 1)
            return fail("vehicles must not turn around to sidestep their neighbours");
    }

    // D3e3) Turning around: every vehicle swings through "down" (south) - left -> right goes W, SW, S, SE, E
    {
        bool allDown = true;
        for (int arch : {6, 7, 8, 9, 10, 11, 12, 13})
        {
            setupArena();
            const int id = entities.SpawnUnitByArchetype(arch, 10, 10, Faction::Resistance);
            Unit *u = entities.GetUnit(id);
            if (u->moveSpeed <= 0.0f)
                continue; // an immobile gun (the Grad) does not drive anywhere
            u->facingDirection = 4; // moving left
            u->turretFacing = 4;
            entities.IssueMoveOrder({id}, 16, 10, pf, false);
            std::vector<int> seen {4};
            for (int i = 0; i < 90 && u->facingDirection != 0; ++i)
            {
                entities.Update(1.0f / 30.0f, map, pf);
                if (u->facingDirection != seen.back())
                    seen.push_back(u->facingDirection);
            }
            const bool down = (seen == std::vector<int> {4, 3, 2, 1, 0});
            if (!down)
            {
                std::cout << "    archetype " << arch << " turned:";
                for (int f : seen)
                    std::cout << " " << f;
                std::cout << std::endl;
            }
            allDown = allDown && down;
        }
        std::cout << "  - Vehicles turning left -> right swing through south: " << (allDown ? "all" : "NOT all")
                  << std::endl;
        if (!allDown)
            return fail("a vehicle reversing must turn through south (down)");
        const bool rule = Utility::TurnStep8(0, 4) == 1 && Utility::TurnStep8(7, 3) == 1
                          && Utility::TurnStep8(3, 7) == -1 && Utility::TurnStep8(5, 1) == -1
                          && Utility::TurnStep8(0, 1) == 1 && Utility::TurnStep8(1, 0) == -1;
        if (!rule)
            return fail("turnStep8 half-turn rule broken");
    }

    // D3f) A tank with a rotating turret shoots while driving past an enemy, without stopping
    {
        setupArena();
        const int tankId = entities.SpawnUnitByArchetype(8, 3, 10, Faction::Resistance); // Armadillo
        const int dummy = entities.SpawnUnitByArchetype(1, 15, 13, Faction::Confederation);
        Unit *t = entities.GetUnit(tankId);
        const bool turret = t->hasRotatingTurret;
        Unit *d = entities.GetUnit(dummy);
        d->moveSpeed = 0.0f; // stays put
        d->attackDamage = 0; // and does not shoot back
        const int hpBefore = d->currentHp;
        entities.IssueMoveOrder({tankId}, 30, 10, pf, false);
        int stops = 0, turretAway = 0;
        for (int i = 0; i < 30 * 20; ++i)
        {
            entities.Update(1.0f / 30.0f, map, pf);
            const Unit *u = entities.GetUnit(tankId);
            if (!u)
                break;
            if (u->tileX < 28 && u->state != ActionState::Move)
                ++stops;
            if (u->state == ActionState::Move && u->turretFacing != u->facingDirection)
                ++turretAway;
            if (u->tileX >= 30)
                break;
        }
        const Entity *de = entities.GetEntity(dummy);
        const int hpAfter = de ? de->currentHp : 0;
        std::cout << "  - Fire on the move: turret=" << turret << ", enemy hp " << hpBefore << " -> " << hpAfter
                  << ", ticks stopped " << stops << ", ticks turret aimed off the hull " << turretAway << std::endl;
        if (turret && (hpAfter >= hpBefore || stops > 15))
            return fail("a turret vehicle must shoot while driving on");
    }

    // D3g) Auto siege: deploy only for a visible enemy beyond normal range but within siege range
    //      (AI always, player only on attack-move [Q]); auto-deployed units pack up when idle
    {
        auto siegeRun = [&](int arch,
                            Faction side,
                            bool attackMove,
                            bool unlock,
                            float enemyDist,
                            float seconds,
                            bool removeEnemyAfter) {
            setupArena();
            entities.SetSiegeUnlocked(Players::OwnerOf(side), unlock);
            const int unitId = entities.SpawnUnitByArchetype(arch, 5, 15, side);
            const Faction foe = (side == Faction::Resistance) ? Faction::Confederation : Faction::Resistance;
            const int enemy = entities.SpawnUnitByArchetype(
                foe == Faction::Resistance ? 0 : 1, 5 + static_cast<int>(enemyDist), 15, foe);
            Unit *e = entities.GetUnit(enemy);
            e->moveSpeed = 0.0f;
            e->attackDamage = 0;
            // a spotter of the unit's side next to the enemy, so it is visible to that side
            const int spotter = entities.SpawnUnitByArchetype(
                side == Faction::Resistance ? 0 : 1, 5 + static_cast<int>(enemyDist), 21, side);
            entities.GetUnit(spotter)->attackDamage = 0;
            entities.GetUnit(spotter)->moveSpeed = 0.0f;
            if (attackMove)
                entities.IssueAttackMoveOrder({unitId}, 6, 15, pf);
            bool deployed = false;
            for (int i = 0; i < static_cast<int>(seconds * 30); ++i)
            {
                entities.Update(1.0f / 30.0f, map, pf);
                const Unit *u = entities.GetUnit(unitId);
                if (u && u->isSiegeMode)
                    deployed = true;
            }
            bool packed = false;
            if (removeEnemyAfter)
            {
                if (Unit *en = entities.GetUnit(enemy))
                    en->state = ActionState::Die;
                for (int i = 0; i < 30 * 8; ++i)
                    entities.Update(1.0f / 30.0f, map, pf);
                const Unit *u = entities.GetUnit(unitId);
                packed = u && !u->isSiegeMode;
            }
            entities.SetSiegeUnlocked(Players::OwnerOf(side), false);
            return std::make_pair(deployed, packed);
        };
        const Faction ai = AiFaction();
        const int aiGrad = (ai == Faction::Confederation) ? 13 : 12; // Porcupine has no siege: only test the Grad side
        // The Grad deploys by itself whenever an enemy is within its siege range (auto_deploy: always), whatever its
        // range is against the normal one; the Rhino only for an enemy beyond its normal range (far)
        const UnitConfig *gradNormal = entities.GetUnitConfig(13), *rhinoNormal = entities.GetUnitConfig(10);
        if (!gradNormal || !rhinoNormal || gradNormal->autoSiege != UnitConfig::AutoSiege::Always
            || rhinoNormal->autoSiege != UnitConfig::AutoSiege::Far)
            return fail("auto_deploy must read always for the Grad and far for the Rhino");
        const float inRange = std::max(2.0f, gradNormal->attackRange - 1.0f);
        auto aiFar = siegeRun(aiGrad, ai, false, true, inRange, 4.0f, true); // an enemy in range: deploys, packs up after
        auto aiNear = siegeRun(aiGrad, ai, false, true, 5.0f, 4.0f, false);  // close by: deploys as well (always)
        auto rhinoLocked = siegeRun(10, Faction::Resistance, true, false, 8.0f, 4.0f, false);
        auto rhinoQ = siegeRun(10, Faction::Resistance, true, true, 8.0f, 4.0f, false);
        auto rhinoPlain = siegeRun(10, Faction::Resistance, false, true, 8.0f, 4.0f, false);
        std::cout << "  - Auto siege: AI far deploy " << aiFar.first << " / pack up " << aiFar.second
                  << ", AI near deploy " << aiNear.first << ", Rhino locked " << rhinoLocked.first << ", Rhino Q "
                  << rhinoQ.first << ", Rhino plain " << rhinoPlain.first << std::endl;
        if (ai == Faction::Confederation && (!aiFar.first || !aiFar.second || !aiNear.first))
            return fail("AI auto siege broken");
        if (rhinoLocked.first || !rhinoQ.first || rhinoPlain.first)
            return fail("player auto siege rules broken");
    }

    // D3d) Enemy units standing across a passage are not a wall: no long detour, the unit fights through
    {
        entities.Clear();
        map.InitBlankArena(60, 40, 0);
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
        // Wall x=30 with a 3-tile passage at y 14..16 and a far opening at y 37..39
        for (int y = 0; y < 37; ++y)
        {
            if (y < 14 || y > 16)
                map.SetPassable(30, y, false);
        }
        std::vector<int> blockers;
        for (int y = 14; y <= 16; ++y)
            blockers.push_back(entities.SpawnUnitByArchetype(1, 30, y, Faction::Confederation));
        const int tank = entities.SpawnUnitByArchetype(8, 22, 15, Faction::Resistance); // Armadillo
        entities.IssueMoveOrder({tank}, 40, 15, pf, false);
        int maxY = 0;
        for (int i = 0; i < 30 * 40; ++i)
        {
            entities.Update(1.0f / 30.0f, map, pf);
            if (const Unit *u = entities.GetUnit(tank))
                maxY = std::max(maxY, u->tileY);
        }
        int alive = 0;
        for (int id : blockers)
            alive += entities.GetEntity(id) ? 1 : 0;
        const Unit *u = entities.GetUnit(tank);
        std::cout << "  - Enemy barricade in a passage: deepest detour y " << maxY << ", blockers left " << alive
                  << "/3, tank at (" << (u ? u->tileX : -1) << "," << (u ? u->tileY : -1) << ")" << std::endl;
        if (maxY > 20)
            return fail("units must not walk around an enemy barricade");
    }

    // D3c) Fog: an enemy firing from the dark at our unit shows up where it stands, only for a moment
    {
        setupArena();
        map.SetFogEnabled(true);
        map.ResetAllFog(FogState::Hidden);
        const int victim = entities.SpawnUnitByArchetype(0, 5, 15, PlayerFaction());
        const int shooter
            = entities.SpawnUnitByArchetype(PlayerFaction() == Faction::Resistance ? 1 : 0, 25, 15, AiFaction());
        entities.UpdateFogOfWar(map);
        const bool hiddenBefore = map.GetFogState(25, 15) != FogState::Visible;
        const Entity *v = entities.GetEntity(victim);
        entities.NoteHit(v->owner, shooter);
        entities.UpdateFogOfWar(map);
        const bool shownAfterHit = map.GetFogState(25, 15) == FogState::Visible;
        // only the shooter's own tile, not a circle around it
        const bool surroundingsHidden
            = map.GetFogState(26, 15) != FogState::Visible && map.GetFogState(25, 16) != FogState::Visible;
        for (int i = 0; i < 100; ++i)
            entities.Update(0.05f, map, pf); // 5 s without further hits
        const bool goneLater = map.GetFogState(25, 15) != FogState::Visible;
        std::cout << "  - Fog muzzle flash: hidden " << hiddenBefore << ", shown on hit " << shownAfterHit
                  << ", only its spot " << surroundingsHidden << ", hidden again " << goneLater << std::endl;
        map.SetFogEnabled(false);
        if (!hiddenBefore || !shownAfterHit || !surroundingsHidden || !goneLater)
            return fail("attacker reveal in fog broken");
    }

    // D3d) Fog: the rest of a group dies, and the lone survivor keeps its ordered target that only the dead saw
    {
        setupArena();
        map.SetFogEnabled(true);
        map.ResetAllFog(FogState::Hidden);
        const int hunter = entities.SpawnUnitByArchetype(0, 6, 15, PlayerFaction());
        const int spotter = entities.SpawnUnitByArchetype(0, 20, 15, PlayerFaction());
        const int enemy = entities.SpawnUnitByArchetype(0, 24, 15, AiFaction());
        entities.UpdateFogOfWar(map);
        entities.IssueAttackOrder({hunter}, enemy, &pf);
        entities.Update(0.05f, map, pf); // the spotter's sight covers the enemy
        if (Entity *sp = entities.GetEntity(spotter))
            sp->alive = false; // gone from the fog calculation
        for (int i = 0; i < 6; ++i)
            entities.Update(0.05f, map, pf);
        const Unit *h = entities.GetUnit(hunter);
        const bool kept = h && h->targetEntityId == enemy;
        const bool advancing = h && h->tileX > 6;
        std::cout << "  - Fog, lone survivor: keeps ordered target " << kept << ", walks on " << advancing << std::endl;
        map.SetFogEnabled(false);
        if (!kept || !advancing)
            return fail("a unit must keep its ordered target after its group's sight is gone");
    }

    // D4) A control group marches with auto-attack: it shoots enemies met on the way and still arrives;
    //     the group bar survives members dying in the same frame
    {
        setupArena();
        auto &gc = *app.m_gameplayController;
        std::vector<int> group;
        for (int i = 0; i < 6; ++i)
            group.push_back(entities.SpawnUnitByArchetype(0, 4 + i % 3, 12 + i / 3, Faction::Resistance));
        std::vector<int> enemies;
        for (int i = 0; i < 2; ++i)
            enemies.push_back(entities.SpawnUnitByArchetype(1, 20, 9 + i, Faction::Confederation)); // beside the road
        for (auto &g : gc.m_groups)
            g = ControlGroup {};
        Simulation &sim = gc.m_sim;
        sim.Reset();
        gc.m_groups[0].unitIds = group;
        gc.PruneGroups(entities);
        ai.m_chokes.Rebuild(map);
        // The order goes the way of the game: an attack-move command for a control group, applied by the simulation
        SimCommand order;
        order.type = CommandType::OrderAt;
        order.player = Players::OwnerOf(Faction::Resistance);
        order.ids.assign(group.begin(), group.end());
        order.x = 34 * TILE_WIDTH + TILE_WIDTH / 2;
        order.y = 13 * TILE_HEIGHT + TILE_HEIGHT / 2;
        order.a = static_cast<int32_t>(OrderKind::AttackMove);
        order.b = 1;
        sim.Issue(order);
        SimContext ctx {entities, map, pf, *app.m_economy, ai};
        sim.ApplyCommands(ctx);
        AISquadController &squads = sim.Squads(Players::OwnerOf(Faction::Resistance));
        if (squads.GetSquads().empty())
            return fail("Ctrl+1 group must march as a squad");
        const float dt = 1.0f / 30.0f;
        float t = 0.0f;
        for (; t < 90.0f && !squads.GetSquads().empty(); t += dt)
        {
            entities.Update(dt, map, pf);
            sim.UpdateSquads(dt, ctx, ai.m_chokes);
        }
        int enemiesLeft = 0;
        for (int id : enemies)
            enemiesLeft += entities.GetEntity(id) ? 1 : 0;
        std::cout << "  - Player group attack-march: arrived after " << t << "s, enemies on the way left "
                  << enemiesLeft << "/2" << std::endl;
        if (!squads.GetSquads().empty())
            return fail("the group must reach its destination");
        if (enemiesLeft == 2)
            return fail("a marching group must fight the enemies it meets");

        // A member dies after the groups were pruned: drawing the bar must not touch it
        entities.GetUnit(group[1])->state = ActionState::Die;
        gc.RenderGroupBar(*app.m_renderer, *app.m_fontRenderer, entities, 900, 700, 0, 0);
        gc.RenderGroupLeaders(*app.m_renderer, *app.m_camera, entities);
        for (auto &g : gc.m_groups)
            g = ControlGroup {};
        std::cout << "  - Group bar with a member dying this frame: OK" << std::endl;
    }

    // E) Cohesion: one straggler does not stop the group (it is paced, not halted); a stuck straggler is
    //    dropped after the timeout; a group that fell apart (half of it behind) waits for the rest
    withDifficulty(1);
    setupArena();
    for (int dx = -1; dx <= 1; ++dx)
    {
        for (int dy = -1; dy <= 1; ++dy)
        {
            if (dx != 0 || dy != 0)
                map.SetPassable(4 + dx, 25 + dy, false); // sealed pocket
        }
    }
    std::vector<int> vanguard;
    for (int i = 0; i < 5; ++i)
        vanguard.push_back(entities.SpawnUnitByArchetype(1, 6 + i % 3, 10 + i / 3, Faction::Confederation));
    int straggler = entities.SpawnUnitByArchetype(1, 4, 25, Faction::Confederation);
    std::vector<int> all = vanguard;
    all.push_back(straggler);
    SquadRun r = runSquad(all, 34, 10, 90.0f, vanguard);
    std::cout << "  - Straggler: vanguard went on to x=" << r.vanguardMaxXEarly << " in 4 s, regrouped "
              << (r.seen.count(AISquadState::Regrouping) ? "yes" : "no") << ", released " << r.released
              << ", reached objective=" << (r.assault ? "yes" : "no") << std::endl;
    if (r.seen.count(AISquadState::Regrouping) != 0 || r.vanguardMaxXEarly <= 9.0f)
    {
        return fail("one straggler must not stop the whole group");
    }
    if (r.released < 1 || !r.assault)
        return fail("stuck straggler must be dropped and the group must move on");

    {
        withDifficulty(1);
        setupArena();
        std::vector<int> front, group;
        for (int i = 0; i < 3; ++i)
            front.push_back(entities.SpawnUnitByArchetype(1, 6 + i, 10, Faction::Confederation));
        group = front;
        for (int i = 0; i < 3; ++i)
            group.push_back(entities.SpawnUnitByArchetype(1, 4 + i, 25, Faction::Confederation));
        SquadRun half = runSquad(group, 34, 10, 90.0f, front);
        std::cout << "  - Half of the group behind: regrouped "
                  << (half.seen.count(AISquadState::Regrouping) ? "yes" : "no")
                  << ", vanguard at x<=" << half.vanguardMaxXEarly
                  << " in 4 s, reached objective=" << (half.assault ? "yes" : "no") << std::endl;
        // (12.5: a started step is always finished, and diagonal steps between allies may cut corners)
        if (half.seen.count(AISquadState::Regrouping) == 0 || half.vanguardMaxXEarly > 12.5f || !half.assault)
        {
            return fail("a group that fell apart must wait for the rest, then go on");
        }
    }

    // H) Enemy walls: detour through a wide gap, cross a narrow one (only units the wall really holds up may
    //    shoot at it), breach a sealed line. gap = number of open tiles from y=2 (0 = sealed).
    for (int gap : {5, 2, 0})
    {
        const bool sealed = gap == 0;
        withDifficulty(1);
        setupArena();
        for (int y = 0; y < 30; ++y)
        {
            if (y < 2 || y >= 2 + gap)
                map.SetWall(20, y, Faction::Resistance, 100);
        }
        std::vector<int> column;
        for (int i = 0; i < 6; ++i)
            column.push_back(entities.SpawnUnitByArchetype(1, 12 + i % 3, 18 + i / 3, Faction::Confederation));
        entities.IssueAttackMoveOrder(column, 30, 19, pf);
        bool shotWall = false;
        int crossed = 0;
        const float dt = 1.0f / 30.0f;
        for (float t = 0.0f; t < 60.0f; t += dt)
        {
            entities.Update(dt, map, pf);
            for (int id : column)
            {
                if (const Unit *u = entities.GetUnit(id); u && u->targetWallTileX >= 0)
                    shotWall = true;
            }
        }
        for (int id : column)
        {
            if (const Unit *u = entities.GetUnit(id); u && u->alive && u->tileX > 20)
                ++crossed;
        }
        std::cout << "  - Wall " << (sealed ? std::string("sealed") : "with a gap of " + std::to_string(gap))
                  << ": wall targeted=" << (shotWall ? "yes" : "no") << ", crossed " << crossed << "/6" << std::endl;
        // (on auto-attack a unit may shoot a wall beside its way by chance - 50% - but must still get across)
        if (gap > 2 && crossed < 5)
            return fail("units must still get across a wall that has a wide gap");
        if (gap > 0 && gap <= 2 && crossed < 5)
            return fail("a group must get through a narrow gap in an enemy wall");
        if (sealed && !shotWall)
            return fail("units must breach a sealed enemy wall");
    }

    // G) Hard: a wave that lost >60% without hurting the player falls back to the staging area
    withDifficulty(2);
    setupArena();
    entities.SpawnBuildingByIndex(8, 32, 20, Faction::Resistance, 1.0f);
    ai.m_squadController.SetStagingPoint(4, 4);
    std::vector<int> doomed;
    for (int i = 0; i < 5; ++i)
        doomed.push_back(entities.SpawnUnitByArchetype(1, 6 + i, 10, Faction::Confederation));
    ai.m_chokes.Rebuild(map);
    ai.m_squadController.CreateSquad(doomed, 33, 21, 1, entities, pf, ai.GetTactics());
    for (int i = 0; i < 4; ++i)
        ai.m_squadController.Update(0.25f, map, entities, pf, ai.m_chokes, ai.GetTactics());
    for (int i = 0; i < 4; ++i)
        entities.GetUnit(doomed[i])->state = ActionState::Die;
    ai.m_squadController.Update(0.25f, map, entities, pf, ai.m_chokes, ai.GetTactics());
    const auto &groups = ai.m_squadController.GetSquads();
    if (groups.empty() || groups.front().state != AISquadState::Retreating)
    {
        return fail("Hard AI must pull back a wave that died without damaging the player");
    }
    std::cout << "  - Hard retreat: 4/5 lost without effect -> survivors fall back to staging." << std::endl;

    // F) Research Lab: technologies are researched in order, bonuses reach existing troops
    withDifficulty(2);
    setupArena();
    entities.SpawnBuildingByIndex(4, 4, 4, Faction::Confederation, 1.0f);
    int trooper = entities.SpawnUnitByArchetype(1, 12, 12, Faction::Confederation);
    int hpBefore = entities.GetUnit(trooper)->maxHp;
    ai.Init(5, 5, 35, 25);
    ai.m_credits = 10000;
    int lastIndex = 0;
    for (int t = 0; t < 150; ++t) // two technologies of 70 s each (Hard)
    {
        ai.Update(1.0f, map, entities, pf);
        if (ai.GetResearchIndex() < lastIndex)
            return fail("research order went backwards");
        lastIndex = ai.GetResearchIndex();
    }
    const Unit *Tr = entities.GetUnit(trooper);
    std::cout << "  - Research: " << ai.GetResearchIndex()
              << " technologies done in 150s (Hard), damage to a rifleman 100 -> "
              << (Tr ? ReduceDamageByArmor(100, *Tr) : -1) << std::endl;
    if (!ai.IsTechnologyResearched(0) || !ai.IsTechnologyResearched(1) || ai.GetResearchIndex() < 2)
    {
        return fail("AI must research the lab technologies one after another from the first");
    }
    (void) hpBefore;
    if (!Tr || ReduceDamageByArmor(100, *Tr) >= 100)
        return fail("Power Suit must make existing AI infantry tougher");

    // The catalogs: 24 technologies per side, own names, requirements point backwards, units behind research
    for (Faction side : {Faction::Resistance, Faction::Confederation})
    {
        for (int i = 0; i < kResearchCount; ++i)
        {
            const ResearchDef &d = researchDef(side, i);
            if (!d.name[0] || d.cost <= 0 || d.needs >= i)
                return fail("research catalog entry is malformed");
        }
    }
    if (std::string(researchDef(Faction::Confederation, 0).name) == researchDef(Faction::Resistance, 0).name
        || researchDef(Faction::Confederation, 0).compId == researchDef(Faction::Resistance, 0).compId)
    {
        return fail("both sides must have their own research list");
    }
    ResetResearch();
    if (IsUnitUnlocked(Players::OwnerOf(Faction::Confederation), 5)
        || IsUnitUnlocked(Players::OwnerOf(Faction::Resistance), 4)
        || !IsUnitUnlocked(Players::OwnerOf(Faction::Confederation), 1)
        || IsRadarUnlocked(Players::OwnerOf(Faction::Confederation))
        || IsResearchAvailable(Players::OwnerOf(Faction::Confederation), 1))
    {
        return fail("flamethrowers, snipers and radars must wait for their technologies");
    }
    CompleteResearch(Players::OwnerOf(Faction::Confederation), 0, entities);
    CompleteResearch(Players::OwnerOf(Faction::Confederation), 5, entities);
    CompleteResearch(Players::OwnerOf(Faction::Confederation), 18, entities);
    if (!IsUnitUnlocked(Players::OwnerOf(Faction::Confederation), 5)
        || !IsRadarUnlocked(Players::OwnerOf(Faction::Confederation))
        || !IsResearchAvailable(Players::OwnerOf(Faction::Confederation), 1))
    {
        return fail("completed technologies must unlock their units and requirements");
    }
    std::cout << "  - Research catalogs: 24 technologies per side, unit and radar unlocks, requirements OK."
              << std::endl;

    // F2) Without a Tech Center research stands still (no HQ fallback)
    setupArena();
    ai.Init(5, 5, 35, 25);
    ai.m_credits = 10000;
    entities.SpawnBuildingByIndex(0, 4, 4, Faction::Confederation, 1.0f); // HQ only
    for (int t = 0; t < 40; ++t)
        ai.Update(1.0f, map, entities, pf);
    std::cout << "  - Research without Tech Center: " << ai.GetResearchIndex()
              << " done, active=" << (ai.IsResearchActive() ? "yes" : "no") << std::endl;
    if (ai.GetResearchIndex() > 0 || ai.IsResearchActive())
        return fail("research must pause without a Tech Center");

    // G) Attack groups: money and difficulty decide how many groups a wave splits into
    {
        setupArena(); // a small map: no extra flank group for map size
        ai.m_gateX[0] = ai.m_gateX[1] = -1;
        withDifficulty(3); // Hardcore: up to 5 groups
        ai.m_credits = 0;
        const int poor = ai.AttackGroupCount(30, map);
        ai.m_credits = 5000;
        const int rich = ai.AttackGroupCount(30, map);
        const int few = ai.AttackGroupCount(4, map);
        const int tenUnits = ai.AttackGroupCount(10, map); // a squad is never smaller than five
        const int five = ai.AttackGroupCount(5, map);      // fewer than six units stay together
        withDifficulty(0);                                 // Easy: no flank groups at all
        const int easyRich = ai.AttackGroupCount(30, map);
        std::cout << "  - Attack groups: poor " << poor << ", rich " << rich << " (5 max), 4 units -> " << few
                  << ", 5 units -> " << five << ", Easy rich " << easyRich << std::endl;
        if (poor != 1 || rich != 5 || few != 1 || five != 1 || easyRich != 1 || tenUnits > 2)
            return fail("attack group count must follow money / difficulty");
    }

    // H) Skirmish base builder: wall arc toward the player with two exits, defenses in front, lab behind
    {
        withDifficulty(1);
        entities.Clear();
        map.InitBlankArena(70, 50, 0);
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
        ai.m_squadController.Clear();
        const Faction side = AiFaction();
        const bool conf = (side == Faction::Confederation);
        entities.SpawnBuildingByIndex(conf ? 0 : 8, 10, 22, side, 1.0f); // HQ
        entities.SpawnBuildingByIndex(conf ? 2 : 10, 6, 28, side, 1.0f); // barracks
        entities.SpawnBuildingByIndex(PlayerFaction() == Faction::Confederation ? 0 : 8, 60, 20, PlayerFaction(), 1.0f);
        ai.Init(11, 23, 61, 21);
        ai.SetSkirmish(true);
        ai.RecordStartingBase(entities, map);
        ai.m_credits = 20000;
        for (int t = 0; t < 240; ++t)
        {
            ai.UpdateBaseBuilder(1.0f, map, entities, pf);
            entities.Update(1.0f, map, pf);
        }
        int walls = 0, towers = 0, labBehind = 0;
        float towerX = 0.0f;
        for (int y = 0; y < map.GetHeight(); ++y)
        {
            for (int x = 0; x < map.GetWidth(); ++x)
                walls += (map.GetWall(x, y) > 0 && map.GetWallFaction(x, y) == side) ? 1 : 0;
        }
        for (const auto &e : entities.GetAllEntities())
        {
            const Building *b = e.AsBuilding();
            if (!b || b->faction != side)
                continue;
            if (b->type == UnitType::Tower || b->type == UnitType::Bunker)
            {
                ++towers;
                towerX += b->tileX;
            }
            if (b->type == UnitType::ResearchLab && b->tileX < 11)
                ++labBehind;
        }
        towerX /= std::max(1, towers);
        // Both exits must stay open: a path leads from the HQ door to the player's base
        std::vector<PathNode> out;
        bool partial = false;
        pf.SetIgnoreUnitsOnce();
        bool open = pf.FindPath(12, 27, 59, 22, out, 0, &partial, true) && !partial;
        std::cout << "  - Skirmish base: " << walls << " wall tiles, " << towers << " towers/bunkers (avg x " << towerX
                  << "), lab behind HQ=" << labBehind << ", exits (" << ai.m_gateX[0] << "," << ai.m_gateY[0] << ") / ("
                  << ai.m_gateX[1] << "," << ai.m_gateY[1] << "), way out=" << (open ? "yes" : "no") << std::endl;
        if (walls < 10)
            return fail("skirmish AI must wall in the side facing the player");
        if (towers < 3 || towerX <= 11.0f)
            return fail("towers and bunkers must stand between the HQ and the player");
        if (labBehind < 1)
            return fail("the lab belongs behind the HQ, away from the player");
        if (ai.m_gateX[1] < 0 || !open)
            return fail("the wall must keep two exits open");
        ai.SetSkirmish(false);
    }

    // H2) Walls only where an enemy can walk in: the northern half of the ring is closed off by impassable
    //     terrain right outside it, so no wall goes there; core build order raises power, barracks and factory
    {
        withDifficulty(1);
        entities.Clear();
        map.InitBlankArena(70, 50, 0);
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
        ai.m_squadController.Clear();
        ai.m_defensePlan.clear();
        ai.m_defensePlanned = false;
        ai.m_gateX[0] = ai.m_gateX[1] = -1;
        const Faction side = AiFaction();
        const bool conf = (side == Faction::Confederation);
        const int hqId = entities.SpawnBuildingByIndex(conf ? 0 : 8, 10, 22, side, 1.0f); // HQ only
        entities.SpawnBuildingByIndex(PlayerFaction() == Faction::Confederation ? 0 : 8, 60, 20, PlayerFaction(), 1.0f);
        const Building *hq = entities.GetBuilding(hqId);
        if (!hq)
            return fail("H2: no HQ");
        const int cx = hq->tileX + hq->buildWidth / 2, cy = hq->tileY + hq->buildHeight / 2;
        const int hqRadius = std::max(hq->buildWidth, hq->buildHeight) / 2 + 1;
        const float wallRadius = static_cast<float>(hqRadius + 8);
        for (int y = 0; y < cy - 2; ++y)
        {
            for (int x = 0; x < map.GetWidth(); ++x)
            {
                const float d = DetMath::Hypot(static_cast<float>(x - cx), static_cast<float>(y - cy));
                if (d >= wallRadius + 0.6f && d <= wallRadius + 3.0f)
                    map.SetPassable(x, y, false);
            }
        }
        ai.Init(cx, cy, 61, 21);
        ai.SetSkirmish(true);
        ai.RecordStartingBase(entities, map);
        ai.m_credits = 20000;
        for (int t = 0; t < 300; ++t)
        {
            ai.UpdateBaseBuilder(1.0f, map, entities, pf);
            entities.Update(1.0f, map, pf);
        }
        int northWalls = 0, southWalls = 0;
        for (int y = 0; y < map.GetHeight(); ++y)
        {
            for (int x = 0; x < map.GetWidth(); ++x)
            {
                if (map.GetWall(x, y) > 0 && map.GetWallFaction(x, y) == side)
                    (y < cy - 5 ? northWalls : southWalls) += 1; // the band ends at cy - 2: walls may close that gap
            }
        }
        const bool power = entities.HasBuildingType(UnitType::PowerPlant, ai.player(), false);
        const bool barracks = entities.HasBuildingType(UnitType::Barracks, ai.player(), false);
        const bool factory = entities.HasBuildingType(UnitType::Factory, ai.player(), false);
        std::cout << "  - Walls on enemy paths: " << northWalls << " behind closed terrain, " << southWalls
                  << " on open ground; power " << power << ", barracks " << barracks << ", factory " << factory
                  << std::endl;
        if (northWalls > 0 || southWalls < 5)
            return fail("walls must only close ways an enemy can walk");
        if (!power || !barracks || !factory)
            return fail("the core build order must raise power, barracks and factory");
        ai.SetSkirmish(false);
    }

    // I) Debug control: treasury edit and the reasoning log
    {
        ai.SetEnemyCredits(-50);
        const bool zeroed = ai.GetEnemyCredits() == 0;
        ai.SetEnemyCredits(12345);
        const size_t thoughts = ai.GetThoughts().size();
        const size_t now = ai.CurrentIntentions().size();
        std::cout << "  - Debug control: credits clamp to 0=" << (zeroed ? "yes" : "no") << ", set 12345 -> "
                  << ai.GetEnemyCredits() << ", thoughts logged " << thoughts << ", intentions " << now << std::endl;
        if (!zeroed || ai.GetEnemyCredits() != 12345 || thoughts < 3 || now == 0)
            return fail("AI debug control / thoughts broken");
    }

    // J) A unit a squad lets go far from the base walks back and rejoins the reserve
    {
        withDifficulty(1);
        setupArena();
        ai.Init(5, 5, 35, 25);
        ai.Update(1.0f, map, entities, pf); // the garrison is recorded on the first update: the unit comes later
        const int lost = entities.SpawnUnitByArchetype(
            ai.side() == Faction::Confederation ? 1 : 0, 34, 24, ai.side(), ai.player());
        ai.m_squadController.CreateSquad({lost}, 35, 25, 1, entities, pf, ai.GetTactics());
        ai.m_squadController.ReleaseUnits({lost}, entities);
        ai.Update(1.0f, map, entities, pf);
        const bool recalled = entities.GetUnit(lost) && ai.IsReserveUnit(*entities.GetUnit(lost));
        for (int t = 0; t < 80; ++t)
        {
            ai.Update(1.0f, map, entities, pf);
            entities.Update(1.0f, map, pf);
        }
        const Unit *back = entities.GetUnit(lost);
        const float home
            = back ? DetMath::Hypot(static_cast<float>(back->tileX - 5), static_cast<float>(back->tileY - 5)) : 999.0f;
        std::cout << "  - Released far from base: recalled=" << (recalled ? "yes" : "no")
                  << ", distance to the HQ after 80 s " << home << std::endl;
        if (!recalled || home > 18.0f)
            return fail("a unit released far from the base must walk back to the reserve");
    }

    // K) A unit standing on a planned wall tile delays the segment instead of cancelling it
    {
        setupArena();
        ai.Init(5, 5, 35, 25);
        // (a computer player without a headquarters builds nothing)
        entities.SpawnBuildingByIndex(ai.side() == Faction::Confederation ? 0 : 8, 5, 5, ai.side(), 1.0f, ai.player());
        const int blocker = entities.SpawnUnitByArchetype(
            ai.side() == Faction::Confederation ? 1 : 0, 20, 10, ai.side(), ai.player());
        AIDefenseSite site {AIDefenseSite::Wall, 20, 10, false};
        ai.m_credits = 1000;
        const bool builtUnderUnit = ai.BuildDefenseSite(site, map, entities);
        const bool keptForLater = !site.done;
        if (Unit *u = entities.GetUnit(blocker))
        {
            u->alive = false;
            u->state = ActionState::Die;
        }
        const bool builtLater = ai.BuildDefenseSite(site, map, entities);
        std::cout << "  - Wall under a unit: built=" << (builtUnderUnit ? "yes" : "no")
                  << ", kept for later=" << (keptForLater ? "yes" : "no")
                  << ", built once free=" << (builtLater ? "yes" : "no") << std::endl;
        if (builtUnderUnit || !keptForLater || !builtLater || map.GetWall(20, 10) == 0)
            return fail("a unit on a planned wall tile must only delay the segment");
    }

    // L) Counter-composition, home guard, expensive units, scouting
    {
        withDifficulty(1);
        ai.m_seenEnemyUnits.clear();
        auto count = [](const AIWavePlan &plan, int arch) {
            return static_cast<int>(std::count(plan.archetypes.begin(), plan.archetypes.end(), arch));
        };
        const AIWavePlan plain = ai.PlanWave(3, true, true);
        for (int i = 0; i < 6; ++i)
            ai.m_seenEnemyUnits[100000 + i] = true; // an armoured enemy
        const AIWavePlan antiArmour = ai.PlanWave(3, true, true);
        ai.m_seenEnemyUnits.clear();
        for (int i = 0; i < 8; ++i)
            ai.m_seenEnemyUnits[100000 + i] = false; // an infantry enemy
        const AIWavePlan antiInfantry = ai.PlanWave(3, true, true);
        ai.m_seenEnemyUnits.clear();
        std::cout << "  - Counter-composition: rockets " << count(plain, 3) << " -> " << count(antiArmour, 3)
                  << " vs armour, flamers " << count(plain, 5) << " -> " << count(antiInfantry, 5) << " vs infantry"
                  << std::endl;
        if (count(antiArmour, 3) <= count(plain, 3) || count(antiInfantry, 5) <= count(plain, 5))
            return fail("the wave must counter the enemy army it has seen");

        float shares[4];
        for (int d = 0; d < 4; ++d)
        {
            withDifficulty(d);
            shares[d] = ai.HomeGuardShare();
        }
        withDifficulty(1);
        std::cout << "  - Home guard: " << shares[0] << " / " << shares[1] << " / " << shares[2] << " / " << shares[3]
                  << std::endl;
        if (!(shares[0] > shares[1] && shares[1] > shares[2] && shares[2] > shares[3] && shares[3] > 0.0f))
            return fail("the home guard must shrink with the difficulty");

        setupArena();
        const bool conf = ai.side() == Faction::Confederation;
        const int dear = entities.SpawnUnitByArchetype(conf ? 11 : 10, 10, 10, ai.side(), ai.player());
        const int cheap = entities.SpawnUnitByArchetype(conf ? 1 : 0, 12, 10, ai.side(), ai.player());
        const bool dearValuable = entities.GetUnit(dear) && ai.IsValuable(*entities.GetUnit(dear), entities);
        const bool cheapValuable = entities.GetUnit(cheap) && ai.IsValuable(*entities.GetUnit(cheap), entities);
        std::cout << "  - Expensive units: " << (conf ? "Zeus" : "Rhino") << " " << dearValuable << ", rifleman "
                  << cheapValuable << std::endl;
        if (!dearValuable || cheapValuable)
            return fail("Zeus / Rhino and dearer units are the ones worth saving");

        // A scout leaves for the enemy base (not on Hardcore)
        setupArena();
        ai.Init(5, 5, 35, 25);
        ai.m_scoutId = 0;
        ai.m_scoutTimer = 0.0f;             // as on a fresh match (Reset)
        ai.Update(1.0f, map, entities, pf); // the garrison is recorded first: these troops come after it
        std::vector<int> troops;
        for (int i = 0; i < 3; ++i)
            troops.push_back(entities.SpawnUnitByArchetype(conf ? 1 : 0, 6 + i, 8, ai.side(), ai.player()));
        for (int id : troops)
            ai.m_waveRoster.insert(id);
        int scout = 0;
        float closest = 1e9f;
        for (int t = 0; t < 20; ++t)
        {
            ai.Update(1.0f, map, entities, pf);
            entities.Update(1.0f, map, pf);
            if (ai.m_scoutId)
                scout = ai.m_scoutId;
            if (const Unit *u = scout ? entities.GetUnit(scout) : nullptr)
                closest = std::min(
                    closest, DetMath::Hypot(static_cast<float>(u->tileX - 35), static_cast<float>(u->tileY - 25)));
        }
        std::cout << "  - Scouting: scout " << (scout ? "sent" : "NOT sent") << ", closest to the enemy base "
                  << closest << " tiles" << std::endl;
        if (!scout || closest > 10.0f)
            return fail("a scout must go and look at the enemy base");
    }

    ai.SetDebugConfig(savedDebug);
    ai.Reset();
    std::cout << "  - AI behaviour validated." << std::endl;
    return true;
}

bool HeadlessTest::RunSquadFocusTest(Application &app)
{
    std::cout << "[Test] Validating that a computer player's group fires at one common target..." << std::endl;
    const float dt = 1.0f / 30.0f;
    EntityManager &entities = *app.m_entities;
    TileMap &map = *app.m_map;
    Pathfinder &pf = *app.m_pathfinder;
    AIController &ai = *app.m_aiController;
    auto fail_ = [&](const std::string &msg) {
        std::cerr << "[Test] Error: " << msg << std::endl;
        entities.Clear();
        return false;
    };
    entities.Clear();
    map.InitBlankArena(50, 30, 0);
    map.SetFogEnabled(false);
    pf.SetMap(&map);
    pf.SetEntityManager(&entities);
    ai.m_squadController.Clear();
    std::vector<int> group;
    for (int i = 0; i < 5; ++i)
        group.push_back(entities.SpawnUnitByArchetype(1, 8 + i % 2, 12 + i, Faction::Confederation));
    // Enemies spread over the field ahead, each nearest to a different member
    for (int i = 0; i < 4; ++i)
        entities.SpawnUnitByArchetype(1, 24 + i % 2, 9 + i * 4, Faction::Resistance);
    ai.m_chokes.Rebuild(map);
    ai.m_squadController.CreateSquad(group, 44, 15, 1, entities, pf, ai.GetTactics());
    int samples = 0, common = 0, focusSeen = 0;
    for (float t = 0.0f; t < 40.0f; t += dt)
    {
        entities.Update(dt, map, pf);
        ai.m_squadController.Update(dt, map, entities, pf, ai.m_chokes, ai.GetTactics());
        const auto &squads = ai.m_squadController.GetSquads();
        if (squads.empty())
            break;
        const AISquad &sq = squads.front();
        if (sq.focusTargetId == 0)
            continue;
        ++focusSeen;
        std::set<int> targets;
        for (int id : sq.unitIds)
            if (const Unit *u = entities.GetUnit(id); u && u->alive && u->targetEntityId != 0)
                targets.insert(u->targetEntityId);
        if (targets.empty())
            continue;
        ++samples;
        if (targets.size() == 1)
            ++common;
    }
    std::cout << "  - focus seen on " << focusSeen << " ticks, one common target on " << common << "/" << samples
              << " samples" << std::endl;
    if (focusSeen == 0)
        return fail_("the group never chose a common target");
    if (samples == 0 || common * 10 < samples * 7)
        return fail_("the members of a group must fire at one target, not each at its own");
    entities.Clear();
    return true;
}

bool HeadlessTest::RunMakeWayTest(Application &app)
{
    std::cout << "[Test] Validating allies making way in narrow passages..." << std::endl;
    const float dt = 1.0f / 30.0f;
    EntityManager &entities = *app.m_entities;
    TileMap &map = *app.m_map;
    Pathfinder &pf = *app.m_pathfinder;
    auto fail_ = [&](const std::string &msg) {
        std::cerr << "[Test] Error: " << msg << std::endl;
        entities.Clear();
        return false;
    };
    auto fresh = [&] {
        entities.Clear();
        map.InitBlankArena(60, 40, 0);
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
    };
    auto near = [&](int id, int x, int y, int r) {
        const Unit *u = entities.GetUnit(id);
        return u && u->alive && std::max(std::abs(u->tileX - x), std::abs(u->tileY - y)) <= r;
    };

    // A. Own wall at x=30 with a one-tile gap at y=20, packed with idle allies in the gap and right behind
    //    it; a group (infantry and a tank) and a single unit are sent through. Everyone gets across, and the
    //    idle units walk back to their places afterwards.
    for (bool vehicle : {false, true})
    {
        fresh();
        for (int y = 0; y < 40; ++y)
        {
            if (y != 20)
                map.SetWall(30, y, Faction::Confederation, 100);
        }
        const std::vector<std::pair<int, int>> plugSpots {{30, 20}, {31, 20}, {31, 19}, {31, 21}, {29, 20}};
        std::vector<int> plugs;
        for (const auto &[x, y] : plugSpots)
            plugs.push_back(entities.SpawnUnitByArchetype(1, x, y, Faction::Confederation));
        std::vector<int> group;
        for (int i = 0; i < 6; ++i)
            group.push_back(entities.SpawnUnitByArchetype(
                (vehicle && i == 0) ? 11 : 1, 18 + i % 3, 19 + i / 3, Faction::Confederation));
        entities.IssueMoveOrder(group, 42, 20, pf);
        float allAcross = -1.0f;
        for (float t = 0.0f; t < 45.0f && allAcross < 0.0f; t += dt)
        {
            entities.Update(dt, map, pf);
            int across = 0;
            for (int id : group)
                across += near(id, 42, 20, 4) ? 1 : 0;
            if (across == static_cast<int>(group.size()))
                allAcross = t;
        }
        for (float t = 0.0f; t < 15.0f; t += dt)
            entities.Update(dt, map, pf);
        int home = 0;
        for (size_t i = 0; i < plugs.size(); ++i)
            home += near(plugs[i], plugSpots[i].first, plugSpots[i].second, 0) ? 1 : 0;
        int across = 0;
        for (int id : group)
            across += near(id, 42, 20, 4) ? 1 : 0;
        std::cout << "  - Gap packed with 5 idle allies" << (vehicle ? " (group with a tank)" : "") << ": " << across
                  << "/6 across" << (allAcross >= 0.0f ? " after " + std::to_string(allAcross) + "s" : std::string())
                  << ", " << home << "/5 back in place" << std::endl;
        if (across < 6)
            return fail_("a group must get through a gap packed with idle allies");
        if (home < 4)
            return fail_("allies that made way must return to their places");
    }

    // B. Head-on in a 1-tile corridor (own walls at y=19 and y=21 over x=28..32): both units get through
    {
        fresh();
        for (int x = 28; x <= 32; ++x)
        {
            map.SetWall(x, 19, Faction::Confederation, 100);
            map.SetWall(x, 21, Faction::Confederation, 100);
        }
        // The corridor is the only way: close the sides above and below it
        for (int y = 0; y < 40; ++y)
        {
            if (y < 19 || y > 21)
                map.SetWall(30, y, Faction::Confederation, 100);
        }
        const int a = entities.SpawnUnitByArchetype(1, 24, 20, Faction::Confederation);
        const int b = entities.SpawnUnitByArchetype(11, 36, 20, Faction::Confederation);
        entities.IssueMoveOrder({a}, 38, 20, pf);
        entities.IssueMoveOrder({b}, 22, 20, pf);
        float done = -1.0f;
        for (float t = 0.0f; t < 30.0f && done < 0.0f; t += dt)
        {
            entities.Update(dt, map, pf);
            if (near(a, 38, 20, 1) && near(b, 22, 20, 1))
                done = t;
        }
        std::cout << "  - Head-on in a 1-tile corridor: "
                  << (done >= 0.0f ? "both through after " + std::to_string(done) + "s" : std::string("stuck"))
                  << std::endl;
        if (done < 0.0f)
            return fail_("two allies meeting head-on in a corridor must let each other pass");
    }

    // C. Diagonal steps may cut corners: a diagonal wall line (tiles touching only at their corners) is
    //    walked through, by infantry and by a vehicle
    for (int arch : {1, 11})
    {
        fresh();
        for (int y = 0; y < 40; ++y)
            map.SetWall(10 + y, y, Faction::Confederation, 100);
        const int u = entities.SpawnUnitByArchetype(arch, 22, 18, Faction::Confederation);
        entities.IssueMoveOrder({u}, 34, 18, pf);
        float done = -1.0f;
        for (float t = 0.0f; t < 20.0f && done < 0.0f; t += dt)
        {
            entities.Update(dt, map, pf);
            if (near(u, 34, 18, 1))
                done = t;
        }
        std::cout << "  - Diagonal wall line, " << (arch == 1 ? "infantry" : "vehicle") << ": "
                  << (done >= 0.0f ? "crossed after " + std::to_string(done) + "s" : std::string("not crossed"))
                  << std::endl;
        if (done < 0.0f)
            return fail_("a diagonal step between two wall tiles touching at the corners must be allowed");
    }

    entities.Clear();
    std::cout << "  - Making way validated." << std::endl;
    return true;
}

bool HeadlessTest::RunCombatBehaviourTest(Application &app)
{
    std::cout << "[Test] Validating wall breach at a single gap and retaliation against far shooters..." << std::endl;
    const float dt = 1.0f / 30.0f;
    auto fresh = [&] {
        app.m_entities->Clear();
        app.m_map->InitBlankArena(60, 40, 0);
        app.m_pathfinder->SetMap(app.m_map.get());
        app.m_pathfinder->SetEntityManager(app.m_entities.get());
    };
    auto countWalls = [&] {
        int n = 0;
        for (int y = 0; y < app.m_map->GetHeight(); ++y)
            n += app.m_map->GetWall(30, y) > 0 ? 1 : 0;
        return n;
    };

    // A. Hostile wall across the arena at x=30 with one gap; a group ordered to the far side
    for (int gapY : {20, 38})
    {
        fresh();
        for (int y = 0; y < app.m_map->GetHeight(); ++y)
        {
            if (y != gapY)
                app.m_map->SetWall(30, y, Faction::Confederation, 100);
        }
        const int wallsBefore = countWalls();
        std::vector<int> group;
        for (int i = 0; i < 8; ++i)
        {
            int id = app.m_entities->SpawnUnitByArchetype(
                i % 2 == 0 ? 0 : 6, 22 + (i % 4), 6 + i / 4, Faction::Resistance);
            if (id > 0)
                group.push_back(id);
        }
        const Unit *first = app.m_entities->GetUnit(group.front());
        if (!first || Players::Hostile(app.m_map->GetWallOwner(30, 6), first->owner) == false)
        {
            std::cerr << "[Test] Error: test wall is not hostile to the group" << std::endl;
            return false;
        }
        app.m_entities->IssueMoveOrder(group, 40, 7, *app.m_pathfinder);
        float breachAt = -1.0f, maxBlocked = 0.0f, doneAt = -1.0f;
        int across = 0;
        for (float t = 0.0f; t < 90.0f; t += dt)
        {
            for (int id : group)
            {
                if (const Unit *u = app.m_entities->GetUnit(id))
                    maxBlocked = std::max(maxBlocked, u->blockedTimer);
            }
            app.m_entities->Update(dt, *app.m_map, *app.m_pathfinder);
            if (breachAt < 0.0f && countWalls() < wallsBefore)
                breachAt = t;
            across = 0;
            for (int id : group)
            {
                if (const Unit *u = app.m_entities->GetUnit(id); u && u->tileX > 30)
                    across++;
            }
            if (across == static_cast<int>(group.size()))
            {
                doneAt = t;
                break;
            }
        }
        std::cout << "  - Gap at y=" << gapY << " (human group: " << (Players::Slot(first->owner).human ? "yes" : "no")
                  << "): wall breached "
                  << (breachAt >= 0.0f ? "after " + std::to_string(breachAt) + "s" : std::string("never")) << ", "
                  << across << "/" << group.size() << " across at " << doneAt << "s, longest queueing " << maxBlocked
                  << "s" << std::endl;
        // Near gap: an open route of about the straight length, so the group walks through it (units held up
        // in the queue at the wall may shoot it). Far gap: the detour is much longer, the wall is breached.
        if (gapY == 38 && breachAt < 0.0f)
        {
            std::cerr << "[Test] Error: the group took the long detour instead of breaching the hostile wall!"
                      << std::endl;
            return false;
        }
        if (across < static_cast<int>(group.size()))
        {
            std::cerr << "[Test] Error: the group did not get across the hostile wall!" << std::endl;
            return false;
        }
    }

    // B. Retaliation: a unit hit by a far shooter goes after it; a unit holding position does not
    {
        fresh();
        const int shooterId = app.m_entities->SpawnUnitByArchetype(0, 45, 20, Faction::Confederation);
        const int victimId = app.m_entities->SpawnUnitByArchetype(0, 15, 20, Faction::Resistance);
        const int holderId = app.m_entities->SpawnUnitByArchetype(0, 15, 25, Faction::Resistance);
        Unit *shooter = app.m_entities->GetUnit(shooterId);
        Unit *victim = app.m_entities->GetUnit(victimId);
        Unit *holder = app.m_entities->GetUnit(holderId);
        if (!shooter || !victim || !holder)
            return false;
        shooter->moveSpeed = 0.0f; // a stationary target dummy that keeps "shooting"
        shooter->attackDamage = 0;
        holder->holdPosition = true;
        const int victimStartX = victim->tileX;
        for (float t = 0.0f; t < 4.0f; t += dt)
        {
            if (static_cast<int>(t / dt) % 30 == 0)
            {
                app.m_entities->NoteEntityHit(*victim, shooterId);
                app.m_entities->NoteEntityHit(*holder, shooterId);
            }
            app.m_entities->Update(dt, *app.m_map, *app.m_pathfinder);
        }
        std::cout << "  - Retaliation: victim moved " << victimStartX << " -> " << victim->tileX << " (target "
                  << victim->targetEntityId << "), holder at " << holder->tileX << "," << holder->tileY << std::endl;
        if (victim->targetEntityId != shooterId || victim->tileX <= victimStartX + 2)
        {
            std::cerr << "[Test] Error: a unit hit from afar did not go after the shooter!" << std::endl;
            return false;
        }
        if (holder->tileX != 15 || holder->tileY != 25)
        {
            std::cerr << "[Test] Error: a unit holding position walked off after a shooter!" << std::endl;
            return false;
        }
    }
    // B2. Stances: an enemy in sight but out of range. Pursue walks up and stays, PursueAndReturn walks up and
    //     comes back after the kill, HoldGround never moves
    {
        fresh();
        const UnitStance stances[3] = {UnitStance::Pursue, UnitStance::PursueAndReturn, UnitStance::HoldGround};
        int unitIds[3] = {}, dummyIds[3] = {};
        for (int i = 0; i < 3; ++i)
        {
            const int y = 6 + i * 10;
            unitIds[i] = app.m_entities->SpawnUnitByArchetype(0, 15, y, Faction::Resistance);
            Unit *u = app.m_entities->GetUnit(unitIds[i]);
            if (!u)
                return false;
            u->stance = stances[i];
            u->sightRadius = static_cast<int>(u->attackRange) + 5; // sees well beyond its reach
            const int gap = static_cast<int>(u->attackRange) + 3;
            dummyIds[i] = app.m_entities->SpawnUnitByArchetype(1, 15 + gap, y, Faction::Confederation);
            Unit *d = app.m_entities->GetUnit(dummyIds[i]);
            if (!d)
                return false;
            d->moveSpeed = 0.0f;
            d->attackDamage = 0;
            d->maxHp = d->currentHp = 1;
        }
        app.m_entities->UpdateFogOfWar(*app.m_map);
        for (float t = 0.0f; t < 12.0f; t += dt)
            app.m_entities->Update(dt, *app.m_map, *app.m_pathfinder);
        const Unit *pursuer = app.m_entities->GetUnit(unitIds[0]);
        const Unit *guard = app.m_entities->GetUnit(unitIds[1]);
        const Unit *holder = app.m_entities->GetUnit(unitIds[2]);
        if (!pursuer || !guard || !holder)
            return false;
        const bool pursuerKilled = app.m_entities->GetEntity(dummyIds[0]) == nullptr;
        const bool guardKilled = app.m_entities->GetEntity(dummyIds[1]) == nullptr;
        const bool holderKilled = app.m_entities->GetEntity(dummyIds[2]) == nullptr;
        std::cout << "  - Stances: pursue at " << pursuer->tileX << " (kill " << pursuerKilled << "), guard at "
                  << guard->tileX << "," << guard->tileY << " (kill " << guardKilled << "), hold at " << holder->tileX
                  << " (kill " << holderKilled << ")" << std::endl;
        if (!pursuerKilled || pursuer->tileX <= 15)
        {
            std::cerr << "[Test] Error: a pursuing unit did not go after an enemy in sight!" << std::endl;
            return false;
        }
        if (!guardKilled || std::abs(guard->tileX - 15) > 1 || std::abs(guard->tileY - 16) > 1)
        {
            std::cerr << "[Test] Error: a guarding unit did not return home after the kill!" << std::endl;
            return false;
        }
        if (holderKilled || holder->tileX != 15 || holder->tileY != 26)
        {
            std::cerr << "[Test] Error: a unit holding its ground walked off after an enemy out of range!" << std::endl;
            return false;
        }
    }
    // B3. Building repair: half the hit points come back in half of kRepairSeconds for 15% of the price,
    //     and the repair stops when the credits run out
    {
        Building b;
        b.maxHp = 1000;
        b.currentHp = 500;
        b.repairing = true;
        int paid = 0;
        float t = 0.0f;
        for (; t < Building::kRepairSeconds && b.repairing; t += dt)
            paid += b.RepairStep(dt, 1000, 100000);
        Building broke;
        broke.maxHp = 1000;
        broke.currentHp = 100;
        broke.repairing = true;
        int brokePaid = 0;
        for (float s = 0.0f; s < 60.0f && broke.repairing; s += dt)
            brokePaid += broke.RepairStep(dt, 1000, 20 - brokePaid);
        std::cout << "  - Repair: full after " << t << " s for " << paid << " credits; with 20 credits "
                  << broke.currentHp << " HP, stopped " << !broke.repairing << std::endl;
        if (b.currentHp != 1000 || std::abs(t - Building::kRepairSeconds / 2.0f) > 1.0f || std::abs(paid - 150) > 2)
        {
            std::cerr << "[Test] Error: building repair rate or price is wrong!" << std::endl;
            return false;
        }
        if (broke.repairing || brokePaid > 20 || broke.currentHp >= 1000)
        {
            std::cerr << "[Test] Error: a repair without credits must stop!" << std::endl;
            return false;
        }
    }
    // C. A manual attack order: the units stop at their firing range, they do not walk up to the target
    for (int arch : {0, 6, 10})
    {
        fresh();
        const int targetId = app.m_entities->SpawnUnitByArchetype(1, 45, 20, Faction::Confederation);
        Unit *target = app.m_entities->GetUnit(targetId);
        target->moveSpeed = 0.0f;
        target->attackDamage = 0;
        target->maxHp = target->currentHp = 100000;
        std::vector<int> group;
        for (int i = 0; i < 6; ++i)
            group.push_back(
                app.m_entities->SpawnUnitByArchetype(arch, 10 + i % 3, 18 + i / 3 * 2, Faction::Resistance));
        app.m_entities->IssueAttackOrder(group, targetId, app.m_pathfinder.get());
        float closest = 1e9f, rangeTiles = 0.0f;
        for (float t = 0.0f; t < 30.0f; t += dt)
            app.m_entities->Update(dt, *app.m_map, *app.m_pathfinder);
        float farthest = 0.0f;
        for (int id : group)
        {
            const Unit *u = app.m_entities->GetUnit(id);
            if (!u)
                continue;
            rangeTiles = u->attackRange;
            const float d = DetMath::Hypot(
                                u->GetUnitCenterPixelX() - target->GetUnitCenterPixelX(),
                                u->GetUnitCenterPixelY() - target->GetUnitCenterPixelY())
                            / TILE_WIDTH;
            closest = std::min(closest, d);
            farthest = std::max(farthest, d);
            if (std::getenv("OAOW_TEST_VERBOSE"))
                std::cout << "    unit " << id << " at " << u->tileX << "," << u->tileY << " dist " << d << " state "
                          << static_cast<int>(u->state) << std::endl;
        }
        std::cout << "  - Manual attack, archetype " << arch << ": range " << rangeTiles << " tiles, closest unit at "
                  << closest << " tiles" << std::endl;
        if (farthest > rangeTiles + 0.01f || closest < rangeTiles - 1.05f)
        {
            std::cerr << "[Test] Error: units on a manual attack order must stand at their firing range (range "
                      << rangeTiles << ", nearest " << closest << ", farthest " << farthest << ")" << std::endl;
            return false;
        }
    }
    // D. A flamethrower at point-blank range: the flame is seen (the damage projectile arrives within the
    //    tick it is fired; the jet is an effect of its own that plays over time)
    for (int gap : {1, 2, 3})
    {
        fresh();
        const int flamerId = app.m_entities->SpawnUnitByArchetype(5, 20, 20, Faction::Confederation);
        const int victimId = app.m_entities->SpawnUnitByArchetype(0, 20 + gap, 20, Faction::Resistance);
        Unit *victim = app.m_entities->GetUnit(victimId);
        Unit *flamer = app.m_entities->GetUnit(flamerId);
        if (!victim || !flamer || flamer->projectile != ProjectileType::Flame)
        {
            std::cerr << "[Test] Error: no flamethrower to test" << std::endl;
            return false;
        }
        victim->attackDamage = 0;
        victim->moveSpeed = 0.0f;
        victim->maxHp = victim->currentHp = 100000;
        app.m_entities->IssueAttackOrder({flamerId}, victimId, app.m_pathfinder.get());
        int visibleTicks = 0, firstSeen = -1;
        for (int tick = 0; tick < 60; ++tick)
        {
            app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
            if (app.m_entities->FlameBurstCount() > 0)
            {
                ++visibleTicks;
                if (firstSeen < 0)
                    firstSeen = tick;
            }
        }
        std::cout << "  - Flamethrower at " << gap << " tile(s): flame on screen for " << visibleTicks
                  << " ticks (hp lost " << (100000 - victim->currentHp) << ")" << std::endl;
        if (firstSeen < 0 || visibleTicks < 6 || victim->currentHp >= 100000)
        {
            std::cerr << "[Test] Error: a flamethrower shot at close range shows no flame (or does no damage)!"
                      << std::endl;
            return false;
        }
    }
    std::cout << "  - Wall breach and retaliation validated." << std::endl;
    return true;
}

bool HeadlessTest::RunBurstTest(Application &app)
{
    std::cout << "[Test] Validating bursts (a new volley never starts inside a burst) and animation timing..."
              << std::endl;
    app.m_entities->Clear();
    app.m_map->InitBlankArena(60, 40, 0);
    app.m_pathfinder->SetMap(app.m_map.get());
    app.m_pathfinder->SetEntityManager(app.m_entities.get());
    const int shooter = app.m_entities->SpawnUnitByArchetype(12, 20, 20, Faction::Resistance); // Porcupine
    app.m_entities->SpawnBuildingByIndex(0, 25, 19, Faction::Confederation, 1.0f);
    const Unit *u = app.m_entities->GetUnit(shooter);
    if (!u || u->burstCount < 2)
    {
        std::cerr << "[Test] Error: the Porcupine is missing or has no burst" << std::endl;
        return false;
    }
    if (std::abs(u->burstPoseTime - 0.2f) > 0.001f || u->flashMode != 0)
    {
        std::cerr << "[Test] Error: animations: timing of the Porcupine was not read (" << u->burstPoseTime << ")"
                  << std::endl;
        return false;
    }
    const float dt = static_cast<float>(SIMULATION_TIMESTEP);
    float prevCooldown = 0.0f;
    int prevRemaining = 0, volleys = 0, shotsInVolley = 0;
    std::vector<int> shots;
    for (int tick = 0; tick < 400 && (u = app.m_entities->GetUnit(shooter)); ++tick)
    {
        app.m_entities->Update(dt, *app.m_map, *app.m_pathfinder);
        u = app.m_entities->GetUnit(shooter);
        if (!u)
            break;
        const bool jumped = u->cooldownTimer > prevCooldown + 0.1f;
        if (jumped && u->burstShotsRemaining == u->burstCount - 1)
        { // a volley starts
            if (prevRemaining != 0)
            {
                std::cerr << "[Test] Error: a new volley started inside a burst (" << prevRemaining
                          << " shots left) tick " << tick << std::endl;
                return false;
            }
            if (volleys > 0)
                shots.push_back(shotsInVolley);
            shotsInVolley = 1;
            ++volleys;
        }
        else if (jumped && u->burstShotsRemaining == 0 && u->cooldownTimer < u->attackCooldown - 0.01f)
        {
            std::cerr << "[Test] Error: the cooldown after a burst is shorter than the weapon's" << std::endl;
            return false;
        }
        else if (u->burstShotsRemaining < prevRemaining)
        {
            ++shotsInVolley;
        }
        prevCooldown = u->cooldownTimer;
        prevRemaining = u->burstShotsRemaining;
    }
    if (volleys < 2)
    {
        std::cerr << "[Test] Error: the Porcupine fired " << volleys << " volley(s) in 20 s" << std::endl;
        return false;
    }
    for (int n : shots)
    {
        if (n != 10)
        {
            std::cerr << "[Test] Error: a volley had " << n << " shots, expected 10" << std::endl;
            return false;
        }
    }
    std::cout << "  - " << volleys << " volleys of 10 shots, none inside another OK" << std::endl;
    return true;
}

bool HeadlessTest::RunDataSyncTest(Application &app)
{
    std::cout << "[Test] Validating the session data overlay (network data sync)..." << std::endl;
    const uint64_t own = NetMatch::AssetsHash();
    const std::vector<Net::DataEntry> manifest = NetMatch::DataManifest();
    if (manifest.size() != LoaderResources::NetHashFiles().size() + 1
        || manifest.back().path != LoaderResources::kManifestPath)
    {
        std::cerr << "[Test] Error: the data manifest does not list every simulation file (" << manifest.size() << ")"
                  << std::endl;
        return false;
    }
    // A picture file is not part of the hash, a simulation file is
    const std::string unitFile = "assets/characters/resistance_unit_rifleman.yaml";
    std::string original;
    if (!Assets::ReadText(unitFile, original))
    {
        std::cerr << "[Test] Error: cannot read " << unitFile << std::endl;
        return false;
    }
    const int ownHp = app.m_entities->GetUnitConfig(0)->maxHp;
    const std::string visualFile = LoaderResources::NetVisualFiles().front();
    Assets::SetSessionOverlay({{visualFile.substr(7), {1, 2, 3}}});
    if (NetMatch::AssetsHash() != own)
    {
        std::cerr << "[Test] Error: a picture file changed the data hash" << std::endl;
        return false;
    }
    std::string changed = original;
    const size_t at = changed.find("max_hp:");
    if (at == std::string::npos)
    {
        std::cerr << "[Test] Error: no max_hp in " << unitFile << std::endl;
        return false;
    }
    const size_t eol = changed.find('\n', at);
    changed.replace(at, eol - at, "max_hp: " + std::to_string(ownHp + 7));
    Assets::SetSessionOverlay({{unitFile.substr(7), std::vector<uint8_t>(changed.begin(), changed.end())}});
    std::string read;
    app.ReloadGameData();
    if (!Assets::ReadText(unitFile, read) || read != changed || NetMatch::AssetsHash() == own
        || app.m_entities->GetUnitConfig(0)->maxHp != ownHp + 7)
    {
        std::cerr << "[Test] Error: the overlay is not used (hp " << app.m_entities->GetUnitConfig(0)->maxHp << ")"
                  << std::endl;
        return false;
    }
    Assets::ClearSessionOverlay();
    app.ReloadGameData();
    if (!Assets::ReadText(unitFile, read) || read != original || NetMatch::AssetsHash() != own
        || app.m_entities->GetUnitConfig(0)->maxHp != ownHp)
    {
        std::cerr << "[Test] Error: the player's own data did not come back" << std::endl;
        return false;
    }
    std::cout << "  - overlay wins, picture files stay out of the hash, own data restored OK" << std::endl;
    return true;
}

bool HeadlessTest::RunZeusTest(Application &app)
{
    std::cout << "[Test] Validating the Zeus guns and orders to the map edge..." << std::endl;
    const float dt = 1.0f / 30.0f;
    auto fresh = [&] {
        app.m_entities->Clear();
        app.m_map->InitBlankArena(60, 40, 0);
        app.m_pathfinder->SetMap(app.m_map.get());
        app.m_pathfinder->SetEntityManager(app.m_entities.get());
    };
    for (const float tiles : {6.0f, 7.5f})
    {
        fresh();
        const int zeusId = app.m_entities->SpawnUnitByArchetype(11, 20, 20, Faction::Confederation);
        const int foeId = app.m_entities->SpawnUnitByArchetype(0, 20 + static_cast<int>(tiles), 20, Faction::Resistance);
        Unit *zeus = app.m_entities->GetUnit(zeusId);
        Entity *foe = app.m_entities->GetEntity(foeId);
        if (!zeus || !foe)
            return false;
        foe->currentHp = 100000;
        foe->cooldownTimer = 1000.0f;
        zeus->targetEntityId = foeId;
        int shells = 0, rockets = 0;
        size_t seen = 0;
        for (int i = 0; i < 300; ++i)
        {
            app.m_entities->Update(dt, *app.m_map, *app.m_pathfinder);
            const auto &list = app.m_entities->GetProjectiles();
            for (const Projectile &p : list)
            {
                if (!p.active || p.shooterId != zeusId)
                    continue;
                const float travelled = std::hypot(p.x - p.startX, p.y - p.startY);
                if (travelled < p.speed * 1.5f && travelled > 0.0f)
                {
                    shells += p.type == ProjectileType::Shell;
                    rockets += p.type == ProjectileType::Rocket;
                }
            }
            (void) seen;
        }
        std::cout << "  - [Zeus] foe " << tiles << " tiles away: unit now at tile " << zeus->tileX << "," << zeus->tileY
                  << ", shell frames " << shells << ", rocket frames " << rockets << " (range " << zeus->attackRange
                  << "/" << zeus->secondaryRange << ", flash mode " << zeus->flashMode << ")" << std::endl;
    }
    return true;
}

bool HeadlessTest::run(Application &app, int numTicks)
{
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "zeus")
    {
        return RunZeusTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "datasync")
    {
        return RunDataSyncTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "burst")
    {
        return RunBurstTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "matchai")
    {
        return RunMatchAiTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "tech")
    {
        return RunTechTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "aivehicle")
    {
        return RunAiVehicleTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "buildrules")
    {
        return RunBuildRulesTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "fogblock")
    {
        return RunFogBlockTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "group")
    {
        return RunGroupMovementTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "ai")
    {
        return RunAiBehaviourTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "combat")
    {
        return RunCombatBehaviourTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "squadfocus")
    {
        return RunSquadFocusTest(app);
    }
    if (const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "makeway")
    {
        return RunMakeWayTest(app);
    }
    std::cout << "[Test] Starting automated headless simulation test (" << numTicks << " ticks)..." << std::endl;

    // 0. Validate Localization system (gettext, EN default, RU toggle)
    std::cout << "[Test] Validating Localization system (gettext)..." << std::endl;
    if (Localization::GetLanguage() != "en")
    {
        std::cerr << "[Test] Error: Default language is not 'en' (got: " << Localization::GetLanguage() << ")!"
                  << std::endl;
        return false;
    }
    std::string enStr = _("MAIN MENU");
    if (enStr != "Main Menu")
    {
        std::cerr << "[Test] Error: English string unexpected: " << enStr << std::endl;
        return false;
    }
    Localization::SetLanguage("ru");
    std::string ruStr = _("MAIN MENU");
    std::cout << "[Test] Russian string for 'MAIN MENU' is: '" << ruStr << "'" << std::endl;
    if (ruStr != "ГЛАВНОЕ МЕНЮ")
    {
        std::cerr << "[Test] Error: Russian translation failed, got: " << ruStr << std::endl;
        return false;
    }
    Localization::SetLanguage("en");
    std::cout << "  - Localization validated: default EN (\"" << enStr << "\") -> RU (\"" << ruStr
              << "\") -> restored EN." << std::endl;

    // 0.5. Validate YAML Character Configs (yaml-cpp)
    std::cout << "[Test] Validating YAML Character Configuration loading (yaml-cpp)..." << std::endl;
    const auto *u0 = app.m_entities->GetUnitConfig(0); // Rifleman Resistance
    // Descriptions come from the translation catalogue by id, not from the YAML file
    if (!u0 || u0->id != "rifleman_res" || u0->description != "rifleman_res.description"
        || Localization::Tr(u0->description) == u0->description || u0->maxHp != 25)
    {
        std::cerr << "[Test] Error: Unit config 0 (rifleman_res) invalid or failed to load from YAML!" << std::endl;
        return false;
    }
    const auto *b8 = app.m_entities->GetBuildingConfig(8); // Headquarters Resistance
    if (!b8 || b8->id != "headquarters_res" || b8->description.empty() || b8->width != 3 || b8->height != 4)
    {
        std::cerr << "[Test] Error: Building config 8 (headquarters_res) invalid or failed to load from YAML!"
                  << std::endl;
        return false;
    }
    const auto &bps = EntityManager::GetBlueprints();
    if (bps.size() != 8 || bps[1].buildingIndex != 9)
    {
        std::cerr << "[Test] Error: Blueprints from YAML invalid! Count=" << bps.size() << std::endl;
        return false;
    }
    std::cout << "  - YAML validated: unit (id=" << u0->id << ", hp=" << u0->maxHp << ", desc=\""
              << Localization::Tr(u0->description).substr(0, 30) << "...\"), building (id=" << b8->id << ", "
              << b8->width << "x" << b8->height << "), 8 blueprints loaded." << std::endl;

    // 0.6 Options and the per-user directory
    std::cout << "[Test] Validating Settings, the user directory and the zoom / quality options..." << std::endl;
    {
        namespace fs = std::filesystem;
        const fs::path userDir = Assets::UserDataDir();
        const std::string settingsPath = (userDir / "settings-test.json").string();

        Settings custom;
        custom.windowMode = WindowMode::Exclusive;
        custom.windowW = 1600;
        custom.windowH = 900;
        custom.fullscreenW = 1920;
        custom.fullscreenH = 1080;
        custom.vsync = false;
        custom.fpsLimit = 60;
        custom.showFps = true;
        custom.graphicsQuality = 1;
        custom.smoothing = false;
        custom.zoomDefault = 1.5f;
        custom.zoomMin = 0.75f;
        custom.zoomMax = 3.0f;
        custom.musicVolume = 40;
        custom.fxVolume = 70;
        custom.muted = true;
        custom.language = "ru";
        custom.difficulty = 3;
        custom.edgePanMargin = 18;
        Settings loaded;
        if (!custom.Save(settingsPath) || !loaded.load(settingsPath) || loaded.windowMode != WindowMode::Exclusive
            || loaded.windowW != 1600 || loaded.fullscreenH != 1080 || loaded.vsync || loaded.fpsLimit != 60
            || !loaded.showFps || loaded.graphicsQuality != 1 || loaded.smoothing || loaded.zoomDefault != 1.5f
            || loaded.zoomMin != 0.75f || loaded.zoomMax != 3.0f || loaded.musicVolume != 40 || loaded.fxVolume != 70
            || !loaded.muted || loaded.language != "ru" || loaded.difficulty != 3 || loaded.edgePanMargin != 18)
        {
            std::cerr << "[Test] Error: Settings did not survive a save / load round trip!" << std::endl;
            return false;
        }
        {
            std::ofstream bad(settingsPath);
            bad << R"({"windowWidth": 10, "zoomDefault": 99, "zoomMax": 1, "musicVolume": 900, "language": "xx", "difficulty": 9, "edgePanMargin": 999})";
        }
        Settings clamped;
        if (!clamped.load(settingsPath) || clamped.windowW < Settings::kMinWindowW
            || clamped.zoomDefault > clamped.zoomMax || clamped.zoomMax < 2.0f || clamped.musicVolume != 128
            || clamped.language != "en" || clamped.difficulty != 3
            || clamped.edgePanMargin != Settings::kMaxEdgePanMargin)
        {
            std::cerr << "[Test] Error: Out-of-range settings were not clamped!" << std::endl;
            return false;
        }
        fs::remove(settingsPath);

        // The save lives in the user directory; an older one from the saves folder is taken over once
        const fs::path legacyDir = userDir / "saves";
        fs::create_directories(legacyDir);
        {
            std::ofstream legacy(legacyDir / "savegame.json");
            legacy << R"({"unlockedMissions": 5, "difficulty": 2})";
        }
        fs::remove(userDir / "savegame.json");
        SaveManager moved;
        SaveData movedData;
        if (fs::path(moved.Path()).parent_path() != userDir || !moved.load(movedData) || movedData.unlockedMissions != 5
            || movedData.difficulty != 2)
        {
            std::cerr << "[Test] Error: The save was not placed in / taken over into the user directory!" << std::endl;
            return false;
        }
        fs::remove_all(legacyDir);

        // Zoom range of the camera
        Camera zoomCamera(800, 600);
        zoomCamera.SetZoomLimits(1.0f, 3.0f);
        zoomCamera.SetZoom(10.0f);
        const float highest = zoomCamera.GetZoom();
        zoomCamera.SetZoom(0.1f);
        if (highest != 3.0f || zoomCamera.GetZoom() != 1.0f)
        {
            std::cerr << "[Test] Error: Camera zoom ignores its limits!" << std::endl;
            return false;
        }

        // Graphics quality sets the resolution of the world (1/3, 1/2, original), zoom limits reach the camera
        std::vector<int> divisors;
        for (int quality = 0; quality < 3; ++quality)
        {
            app.m_settings.graphicsQuality = quality;
            app.ApplySettings(SettingsArea::Graphics);
            divisors.push_back(app.RenderDivisor());
        }
        app.m_settings.zoomMin = 1.0f;
        app.m_settings.zoomMax = 3.0f;
        app.ApplySettings(SettingsArea::Graphics);
        const bool limitsApplied = app.m_camera->GetMinZoom() == 1.0f && app.m_camera->GetMaxZoom() == 3.0f;
        app.m_settings.zoomMin = 0.5f;
        app.m_settings.zoomMax = 4.0f;
        app.ApplySettings(SettingsArea::Graphics);
        const Camera view = app.m_camera->ScaledDown(3);
        if (divisors != std::vector<int> {3, 2, 1} || Settings().graphicsQuality != 2 || !limitsApplied
            || app.m_camera->GetMinZoom() != 0.5f || app.m_camera->GetMaxZoom() != 4.0f
            || view.GetZoom() >= app.m_camera->GetZoom() || !fs::exists(Settings::DefaultPath()))
        {
            std::cerr << "[Test] Error: Graphics options were not applied / stored!" << std::endl;
            return false;
        }
        std::cout
            << "  - Settings round trip + clamping, save in the user directory (+ migration), camera zoom limits and "
               "graphics quality validated."
            << std::endl;

        // Edge scroll margin and menu-open camera suppression
        InputManager testInput;
        testInput.SetEdgePanMargin(20);
        if (testInput.GetEdgePanMargin() != 20)
        {
            std::cerr << "[Test] Error: InputManager edge pan margin not set!" << std::endl;
            return false;
        }
        testInput.SetEdgePanMargin(2);
        if (testInput.GetEdgePanMargin() != Settings::kMinEdgePanMargin)
        {
            std::cerr << "[Test] Error: InputManager edge pan margin min clamping failed!" << std::endl;
            return false;
        }
        testInput.SetEdgePanMargin(100);
        if (testInput.GetEdgePanMargin() != Settings::kMaxEdgePanMargin)
        {
            std::cerr << "[Test] Error: InputManager edge pan margin max clamping failed!" << std::endl;
            return false;
        }

        Camera panCamera(800, 600);
        panCamera.SetPosition(400.0f, 400.0f);
        testInput.SetEdgePanMargin(15);
        testInput.SetMouseScreenPosition(0, 0); // top-left edge

        // When menu is open, edge panning must be blocked
        testInput.SetMenuOpen(true);
        testInput.UpdateCamera(0.1f, panCamera, 100, 100, true);
        if (panCamera.GetX() != 400.0f || panCamera.GetY() != 400.0f)
        {
            std::cerr << "[Test] Error: Camera panned while menu was open!" << std::endl;
            return false;
        }

        // When menu is closed, edge panning must move the camera
        testInput.SetMenuOpen(false);
        testInput.UpdateCamera(0.1f, panCamera, 100, 100, false);
        if (panCamera.GetX() >= 400.0f || panCamera.GetY() >= 400.0f)
        {
            std::cerr << "[Test] Error: Camera failed to pan when mouse was at the edge!" << std::endl;
            return false;
        }
        std::cout << "  - Edge scroll margin slider clamping and menu-open camera suppression validated." << std::endl;
    }

    // 1. Verify loading of all 8 campaign missions
    std::cout << "[Test] Validating all 8 campaign missions and authentic base positions..." << std::endl;
    for (int mid = 0; mid < 8; ++mid)
    {
        if (!app.m_mission->LoadMission(mid, "assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
        {
            std::cerr << "[Test] Failed to load Mission " << mid << "!" << std::endl;
            return false;
        }
        std::cout << "  - " << app.m_mission->GetMissionName() << " (Map " << app.m_map->GetWidth() << "x"
                  << app.m_map->GetHeight() << ") | Player HQ: (" << app.m_mission->GetPlayerHqX() << ","
                  << app.m_mission->GetPlayerHqY() << ")"
                  << " | Entities: " << app.m_entities->GetAllEntities().size() << std::endl;

        if (mid == 7)
        {
            uint16_t wConf = app.m_map->GetWall(12, 24);
            uint16_t wRes = app.m_map->GetWall(47, 14);
            if (wConf < 1001 || wRes < 1101)
            {
                std::cerr << "[Test] Error: Walls on map m0 (mid 7) failed to load properly! wConf=" << wConf
                          << " wRes=" << wRes << std::endl;
                return false;
            }
            std::cout << "  - Validated defensive walls on Map m0: Confederation wall=" << wConf
                      << ", Resistance wall=" << wRes << std::endl;
        }
    }

    // 1a. Defense missions: the enemy is not standing there at the start, its squads spawn and attack later
    std::cout << "[Test] Validating that defense missions spawn their enemies in waves..." << std::endl;
    for (int mid : {1, 3})
    {
        if (!app.m_mission->LoadMission(mid, "assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
        {
            std::cerr << "[Test] Failed to load defense mission " << mid << "!" << std::endl;
            return false;
        }
        auto enemyUnits = [&]() {
            int n = 0;
            for (const auto &e : app.m_entities->GetAllEntities())
            {
                if (e.alive && !e.isBuilding && e.faction == AiFaction())
                    ++n;
            }
            return n;
        };
        const int atStart = enemyUnits();
        // Squads come by the script's own timeline, stretched by the difficulty (Easy: the slowest)
        int after60 = -1;
        for (int i = 0; i < 240; ++i)
        {
            app.m_mission->Update(1.0f, *app.m_map, *app.m_entities, *app.m_pathfinder);
            if (i == 59)
                after60 = enemyUnits();
        }
        const int later = enemyUnits();
        std::cout << "  - " << app.m_mission->GetMissionName() << ": enemy units at start " << atStart << ", after 60s "
                  << after60 << ", after 240s " << later << std::endl;
        if (atStart != 0 || later == 0 || app.m_mission->IsVictory())
        {
            std::cerr << "[Test] Error: defense mission " << mid << " must spawn its enemies in waves!" << std::endl;
            return false;
        }
    }

    // 1b. The Confederation campaign: its own list, own maps, the player commands the Confederation
    std::cout << "[Test] Validating the Confederation campaign missions..." << std::endl;
    SetPlayerFaction(Faction::Confederation);
    const auto &confMissions = MissionController::GetAvailableMissions();
    if (confMissions.size() != 7 || confMissions[0].name != "Mission 1: Pukalpa")
    {
        std::cerr << "[Test] Error: Confederation campaign list is wrong!" << std::endl;
        SetPlayerFaction(Faction::Resistance);
        return false;
    }
    for (int mid = 0; mid < 7; ++mid)
    {
        const bool loaded
            = app.m_mission->LoadMission(mid, "assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder);
        const auto &conf = Missions::confederation()[mid].data;
        bool ownBase = false;
        for (const auto &e : app.m_entities->GetAllEntities())
        {
            if (e.alive && e.isBuilding && e.faction == Faction::Confederation)
                ownBase = true;
        }
        std::cout << "  - " << app.m_mission->GetMissionName() << " (" << conf.mapFile << ", Map "
                  << app.m_map->GetWidth() << "x" << app.m_map->GetHeight() << ") | Player HQ: ("
                  << app.m_mission->GetPlayerHqX() << "," << app.m_mission->GetPlayerHqY() << ")" << std::endl;
        if (!loaded || app.m_map->GetWidth() <= 1 || conf.mapFile.rfind("map_mission_", 0) != 0
            || (mid != 3 && mid != 2 && !ownBase && conf.entities.empty()))
        {
            std::cerr << "[Test] Error: Confederation mission " << mid << " failed to load!" << std::endl;
            SetPlayerFaction(Faction::Resistance);
            return false;
        }
    }
    SetPlayerFaction(Faction::Resistance);

    // Load Mission 0 for full gameplay simulation
    if (!app.m_mission->LoadMission(0, "assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
    {
        std::cerr << "[Test] Failed to load Mission 0!" << std::endl;
        return false;
    }
    app.m_aiController->Init(
        app.m_mission->GetEnemyHqX(),
        app.m_mission->GetEnemyHqY(),
        app.m_mission->GetPlayerHqX(),
        app.m_mission->GetPlayerHqY());
    app.m_state = AppState::CampaignPlaying;

    // 2. Test Base Construction
    std::cout << "[Test] Testing Base Construction System..." << std::endl;
    const auto &blueprints = EntityManager::GetBlueprints();
    if (blueprints.empty())
    {
        std::cerr << "[Test] Error: No building blueprints found!" << std::endl;
        return false;
    }
    const auto &bp = blueprints[1]; // Power Plant
    int testBuildX = app.m_mission->GetPlayerHqX() + 6;
    int testBuildY = app.m_mission->GetPlayerHqY();
    if (app.m_entities->CanPlaceBuilding(
            testBuildX, testBuildY, bp.width, bp.height, *app.m_map, Players::OwnerOf(Faction::Resistance)))
    {
        int initialCredits = app.m_economy->GetCredits();
        app.m_economy->SpendCredits(bp.creditCost);
        int newBId
            = app.m_entities->SpawnBuildingByIndex(bp.buildingIndex, testBuildX, testBuildY, Faction::Resistance, 0.0f);
        if (newBId > 0)
        {
            std::cout << "  - Successfully placed " << bp.name << " at (" << testBuildX << "," << testBuildY << ")"
                      << " | Deducted $" << bp.creditCost << " (Credits: " << initialCredits << " -> "
                      << app.m_economy->GetCredits() << ")" << std::endl;
        }
    }

    // Verify initial units
    const auto &entities = app.m_entities->GetAllEntities();
    int playerCount = 0, enemyCount = 0;
    std::vector<int> playerUnits;
    for (const auto &e : entities)
    {
        if (!e.alive)
            continue;
        if (e.faction == Faction::Resistance)
        {
            playerCount++;
            if (!e.isBuilding)
                playerUnits.push_back(e.id);
        }
        else
        {
            enemyCount++;
        }
    }
    std::cout << "[Test] Active entities: " << playerCount << " player, " << enemyCount << " enemy." << std::endl;

    // Issue Attack-Move order to player units
    if (!playerUnits.empty())
    {
        std::cout << "[Test] Issuing Attack-Move order [A] for " << playerUnits.size() << " units to (25, 20)..."
                  << std::endl;
        app.m_entities->IssueAttackMoveOrder(playerUnits, 25, 20, *app.m_pathfinder);
    }

    // Run simulation loop
    float dt = static_cast<float>(SIMULATION_TIMESTEP);
    for (int tick = 0; tick < numTicks; ++tick)
    {
        app.Update(dt);
        app.render();

        if (tick == numTicks / 2 && !playerUnits.empty() && enemyCount > 0)
        {
            // Find an enemy entity and issue attack order
            for (const auto &e : app.m_entities->GetAllEntities())
            {
                if (e.alive && e.faction == Faction::Confederation)
                {
                    std::cout << "[Test] Issuing attack order against enemy unit #" << e.id << "..." << std::endl;
                    app.m_entities->IssueAttackOrder(playerUnits, e.id);
                    break;
                }
            }
        }
    }

    // 3. Test Production Prerequisites & Structure-Specific Spawning
    std::cout << "[Test] Validating Production Prerequisites and Structure Spawning..." << std::endl;
    bool hasBarracksInitially = app.m_entities->HasBuildingType(UnitType::Barracks, Faction::Resistance);
    bool hasFactoryInitially = app.m_entities->HasBuildingType(UnitType::Factory, Faction::Resistance);
    std::cout << "  - Initial structure prerequisites: Barracks=" << (hasBarracksInitially ? "YES" : "NO")
              << ", Factory=" << (hasFactoryInitially ? "YES" : "NO") << std::endl;

    // Place a Barracks and a Factory
    int bX = app.m_mission->GetPlayerHqX() + 6;
    int bY = app.m_mission->GetPlayerHqY() + 6;
    int barracksId = app.m_entities->SpawnBuilding(UnitType::Barracks, bX, bY, Faction::Resistance);
    int fX = app.m_mission->GetPlayerHqX() + 12;
    int fY = app.m_mission->GetPlayerHqY() + 6;
    int factoryId = app.m_entities->SpawnBuilding(UnitType::Factory, fX, fY, Faction::Resistance);

    if (barracksId <= 0 || factoryId <= 0)
    {
        std::cerr << "[Test] Error: Failed to spawn Barracks or Factory for production test!" << std::endl;
        return false;
    }

    Building *bEnt = app.m_entities->GetBuilding(barracksId);
    Building *fEnt = app.m_entities->GetBuilding(factoryId);
    if (bEnt)
        bEnt->buildProgress = 1.0f;
    if (fEnt)
        fEnt->buildProgress = 1.0f;

    bool hasBarracksNow = app.m_entities->HasBuildingType(UnitType::Barracks, Faction::Resistance);
    bool hasFactoryNow = app.m_entities->HasBuildingType(UnitType::Factory, Faction::Resistance);
    if (!hasBarracksNow || !hasFactoryNow)
    {
        std::cerr << "[Test] Error: hasBuildingType failed after structure placement!" << std::endl;
        return false;
    }

    int bSpawnX = 0, bSpawnY = 0;
    int fSpawnX = 0, fSpawnY = 0;
    bool bSpawnOk = app.m_entities->GetBuildingSpawnPoint(barracksId, *app.m_map, bSpawnX, bSpawnY);
    bool fSpawnOk = app.m_entities->GetBuildingSpawnPoint(factoryId, *app.m_map, fSpawnX, fSpawnY);
    if (!bSpawnOk || !fSpawnOk)
    {
        std::cerr << "[Test] Error: getBuildingSpawnPoint failed for Barracks or Factory!" << std::endl;
        return false;
    }
    std::cout << "  - Barracks #" << barracksId << " spawn point: (" << bSpawnX << "," << bSpawnY << ")" << std::endl;
    std::cout << "  - Factory #" << factoryId << " spawn point: (" << fSpawnX << "," << fSpawnY << ")" << std::endl;

    // Test recruitment queue with structure-specific spawning
    app.m_economy->AddCredits(1000);
    size_t prevUnitCount = app.m_entities->GetAllEntities().size();
    app.m_economy->QueueUnit(0, 80, 0.05f, barracksId, *app.m_entities); // Resistance rifleman
    app.m_economy->QueueUnit(6, 220, 0.05f, factoryId, *app.m_entities); // Coyote
    if (app.m_economy->QueueUnit(6, 220, 0.05f, barracksId, *app.m_entities)
        && app.m_entities->GetBuilding(barracksId)->productionQueue.size() != 2)
    {
        std::cerr << "[Test] Error: queue bookkeeping mismatch at Barracks!" << std::endl;
        return false;
    }
    // Cancelling refunds and removes the order again
    const int creditsBeforeCancel = app.m_economy->GetCredits();
    if (!app.m_economy->CancelUnit(6, barracksId, *app.m_entities)
        || app.m_economy->GetCredits() != creditsBeforeCancel + 220)
    {
        std::cerr << "[Test] Error: cancelling a queued unit did not refund it!" << std::endl;
        return false;
    }

    // Simulate 20 ticks to complete instant production
    for (int i = 0; i < 20; ++i)
    {
        app.m_economy->Update(dt, *app.m_entities, *app.m_map, *app.m_pathfinder);
        app.m_entities->Update(dt, *app.m_map, *app.m_pathfinder);
    }

    size_t newUnitCount = app.m_entities->GetAllEntities().size();
    if (newUnitCount < prevUnitCount + 2)
    {
        std::cerr << "[Test] Error: Units failed to spawn from designated Barracks and Factory!" << std::endl;
        return false;
    }
    std::cout << "  - Successfully trained infantry at Barracks and produced vehicle at Factory!" << std::endl;

    // 4. Test Attack-Move filter: Ensure buildings are filtered out of combat orders
    std::vector<int> mixedSelection = {barracksId, factoryId};
    if (!playerUnits.empty())
        mixedSelection.push_back(playerUnits[0]);
    std::vector<int> filteredCombatUnits;
    for (int uid : mixedSelection)
    {
        const Entity *e = app.m_entities->GetEntity(uid);
        if (e && e->alive && !e->isBuilding)
        {
            filteredCombatUnits.push_back(uid);
        }
    }
    if (filteredCombatUnits.size() != 1 || filteredCombatUnits[0] != playerUnits[0])
    {
        std::cerr << "[Test] Error: Building filter failed on Attack-Move selection!" << std::endl;
        return false;
    }
    std::cout << "  - Attack-Move combat unit filter validated (structures correctly excluded)." << std::endl;

    // 5. Test Destructible Walls & Attack Order
    std::cout << "[Test] Validating Destructible Walls..." << std::endl;
    int twX = 20, twY = 20;
    app.m_map->SetWall(twX, twY, Faction::Confederation, 100);
    if (app.m_map->GetWall(twX, twY) == 0 || app.m_map->IsPassable(twX, twY))
    {
        std::cerr << "[Test] Error: Failed to place test wall!" << std::endl;
        return false;
    }
    int initialWallHp = app.m_map->GetWallHp(twX, twY);
    app.m_map->DamageWall(twX, twY, 50);
    if (app.m_map->GetWallHp(twX, twY) >= initialWallHp)
    {
        std::cerr << "[Test] Error: damageWall failed to reduce HP!" << std::endl;
        return false;
    }
    app.m_map->DamageWall(twX, twY, 150); // Fatal damage
    if (app.m_map->GetWall(twX, twY) != 0 || !app.m_map->IsPassable(twX, twY))
    {
        std::cerr << "[Test] Error: Wall not destroyed upon fatal damage!" << std::endl;
        return false;
    }
    std::cout << "  - Destructible wall validated (takes damage, collapses, frees passability)." << std::endl;

    // 6. Test AI Rebuilding System for destroyed structures and walls
    std::cout << "[Test] Validating AI Base & Wall Reconstruction System..." << std::endl;
    int aiBx = app.m_mission->GetEnemyHqX() + 4;
    int aiBy = app.m_mission->GetEnemyHqY() + 4;
    int aiBId = app.m_entities->SpawnBuilding(UnitType::Barracks, aiBx, aiBy, Faction::Confederation);
    app.m_map->SetWall(aiBx + 5, aiBy, Faction::Confederation, 100);

    // AI captures base memory
    app.m_aiController->RecordStartingBase(*app.m_entities, *app.m_map);

    // Destroy the AI structure and wall
    Building *aiB = app.m_entities->GetBuilding(aiBId);
    if (aiB)
    {
        aiB->currentHp = 0;
        aiB->state = ActionState::Die;
        for (int ty = aiBy; ty < aiBy + aiB->buildHeight; ++ty)
        {
            for (int tx = aiBx; tx < aiBx + aiB->buildWidth; ++tx)
            {
                app.m_map->SetPassable(tx, ty, true);
            }
        }
    }
    app.m_map->DamageWall(aiBx + 5, aiBy, 200); // Destroy wall

    // Simulate AI with abundant credits and elapsed rebuild timer
    for (int t = 0; t < 60; ++t)
    {
        app.m_aiController->Update(0.25f, *app.m_map, *app.m_entities, *app.m_pathfinder);
    }

    // Verify AI reconstructed the structure and the wall
    bool aiBRebuilt = false;
    for (const auto &e : app.m_entities->GetAllEntities())
    {
        if (e.alive && e.isBuilding && e.faction == Faction::Confederation && e.tileX == aiBx && e.tileY == aiBy)
        {
            aiBRebuilt = true;
            break;
        }
    }
    bool aiWallRebuilt = (app.m_map->GetWall(aiBx + 5, aiBy) > 0);

    if (!aiBRebuilt || !aiWallRebuilt)
    {
        std::cerr << "[Test] Error: AI failed to rebuild destroyed structure or wall! aiBRebuilt=" << aiBRebuilt
                  << " aiWallRebuilt=" << aiWallRebuilt << std::endl;
        return false;
    }
    std::cout << "  - AI successfully reconstructed destroyed starting base building and wall in place!" << std::endl;

    // 7. Standalone Test Map (maps/testmap.json) + Closed Passage Partial Pathing & Unit Blocking
    std::cout << "[Test] Validating Standalone Test Map (testmap.json), Unit Blocking & Closed Passages..."
              << std::endl;
    if (!app.m_mission->LoadTestMap("assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
    {
        std::cerr << "[Test] Error: Failed to load Standalone Test Map (testmap.json)!" << std::endl;
        return false;
    }

    std::cout << "  - testmap.json loaded: " << app.m_map->GetWidth() << "x" << app.m_map->GetHeight() << ", "
              << app.m_entities->GetAllEntities().size() << " entities." << std::endl;

    // The test map is edited in the map editor, so the checks below build their own polygon instead of
    // relying on its layout: a blank arena with a sealed enemy wall at X=23 and a player tank at (19, 7)
    app.m_entities->Clear();
    app.m_map->InitBlankArena(96, 64, 0);
    app.m_map->SetFogEnabled(false);
    app.m_pathfinder->SetMap(app.m_map.get());
    app.m_pathfinder->SetEntityManager(app.m_entities.get());
    for (int y = 0; y < app.m_map->GetHeight(); ++y)
        app.m_map->SetWall(23, y, Faction::Confederation, 100);
    int testTankId = app.m_entities->SpawnUnitByArchetype(8, 19, 7, Faction::Resistance);
    if (testTankId <= 0)
    {
        std::cerr << "[Test] Error: Test tank could not be placed at (19,7)!" << std::endl;
        return false;
    }

    // Order the tank to move to (27, 7) behind the sealed wall at X=23
    app.m_entities->IssueMoveOrder({testTankId}, 27, 7, *app.m_pathfinder, false);
    for (int t = 0; t < 180; ++t)
    {
        app.m_entities->Update(dt, *app.m_map, *app.m_pathfinder);
    }

    const Unit *tank = app.m_entities->GetUnit(testTankId);
    if (!tank || tank->tileX != 22 || tank->state != ActionState::Idle || !tank->waitingForPath)
    {
        std::cerr << "[Test] Error: Unit did not stop cleanly at closest tile (22,7) before closed wall! tileX="
                  << (tank ? tank->tileX : -1) << " waitingForPath=" << (tank ? tank->waitingForPath : false)
                  << std::endl;
        return false;
    }
    std::cout << "  - Unit walked to (" << tank->tileX << "," << tank->tileY
              << ") right in front of closed wall at X=23 and stood still waiting for path." << std::endl;

    // Now breach the wall at (23, 7) and verify the waiting unit automatically resumes moving to (27, 7)!
    app.m_map->DamageWall(23, 7, 200);
    for (int t = 0; t < 240; ++t)
    {
        app.m_entities->Update(dt, *app.m_map, *app.m_pathfinder);
    }
    tank = app.m_entities->GetUnit(testTankId);
    if (!tank || tank->tileX != 27 || tank->tileY != 7)
    {
        std::cerr << "[Test] Error: Unit failed to resume movement after wall breach! tile=("
                  << (tank ? tank->tileX : -1) << "," << (tank ? tank->tileY : -1) << ")" << std::endl;
        return false;
    }
    std::cout << "  - After wall breach at (23,7), waiting unit automatically resumed path and reached (27,7)!"
              << std::endl;

    // Verify no two living mobile units ever overlap on the same tile
    const auto &allEnts = app.m_entities->GetAllEntities();
    for (size_t i = 0; i < allEnts.size(); ++i)
    {
        if (!allEnts[i].alive || allEnts[i].state == ActionState::Die || allEnts[i].isBuilding)
            continue;
        for (size_t j = i + 1; j < allEnts.size(); ++j)
        {
            if (!allEnts[j].alive || allEnts[j].state == ActionState::Die || allEnts[j].isBuilding)
                continue;
            if (allEnts[i].tileX == allEnts[j].tileX && allEnts[i].tileY == allEnts[j].tileY)
            {
                std::cerr << "[Test] Error: Unit overlap detected at (" << allEnts[i].tileX << "," << allEnts[i].tileY
                          << ")!" << std::endl;
                return false;
            }
        }
    }
    std::cout << "  - Unit-to-unit tile collision validated (0 overlapping units)." << std::endl;

    // 8. Test New Mechanics: HQ Build Radius, Stormcast Slots, Rhino Burst & Wall Switching
    std::cout << "[Test] Validating HQ 25-Tile Radius, Lab Expansion (+5), Stormcast & Siege Units..." << std::endl;

    // A. HQ Build Radius (25 -> 30)
    int hqTileX = 5, hqTileY = 5;
    app.m_entities->Clear();
    app.m_map->RevealArea(hqTileX + 15, hqTileY, 3);
    app.m_map->RevealArea(hqTileX + 28, hqTileY, 3);
    app.m_entities->SetBuildRadius(25);
    app.m_entities->SpawnBuildingByIndex(8, hqTileX, hqTileY, Faction::Resistance, 1.0f); // Resistance HQ (3x4)

    // Distance 15 tiles: within radius 25
    bool canBuildNear = app.m_entities->CanPlaceBuilding(
        hqTileX + 15, hqTileY, 1, 1, *app.m_map, Players::OwnerOf(Faction::Resistance));
    // Distance 28 tiles: outside radius 25
    bool canBuildFar = app.m_entities->CanPlaceBuilding(
        hqTileX + 28, hqTileY, 1, 1, *app.m_map, Players::OwnerOf(Faction::Resistance));
    if (!canBuildNear || canBuildFar)
    {
        std::cerr << "[Test] Error: HQ 25-tile build radius failed! near=" << canBuildNear << " far=" << canBuildFar
                  << std::endl;
        return false;
    }
    // Expand radius by +5 (Lab research) -> 30 tiles
    app.m_entities->ExpandBuildRadius(5);
    bool canBuildAfterResearch = app.m_entities->CanPlaceBuilding(
        hqTileX + 28, hqTileY, 1, 1, *app.m_map, Players::OwnerOf(Faction::Resistance));
    if (!canBuildAfterResearch)
    {
        std::cerr << "[Test] Error: Lab +5 radius expansion failed! 28 tiles should be allowed within radius 30."
                  << std::endl;
        return false;
    }
    std::cout << "  - HQ 25-tile radius & Lab +5 expansion validated (25 -> 30 tiles)." << std::endl;

    // B. Stormcast Spiral Slots Validation
    auto slots = AINavigataionStormcastView::ComputeGroupSlots(10, 10, 4, *app.m_map, app.m_entities.get(), {});
    if (slots.size() != 4 || slots[0].x != 10 || slots[0].y != 10)
    {
        std::cerr << "[Test] Error: Stormcast spiral algorithm failed slot count or center!" << std::endl;
        return false;
    }
    std::cout << "  - AINavigataionStormcastView computed 4 distinct spiral formation slots around (10,10)."
              << std::endl;

    // C. Rhino Siege Mode (archetype 10 <-> 15) and double-shot burst with animation
    int rhinoId = app.m_entities->SpawnUnitByArchetype(10, 10, 10, Faction::Resistance);
    Unit *rhino = app.m_entities->GetUnit(rhinoId);
    if (!rhino)
    {
        std::cerr << "[Test] Error: Failed to spawn Rhino!" << std::endl;
        return false;
    }
    app.m_entities->SetSiegeUnlocked(
        Players::OwnerOf(Faction::Resistance), true); // Rhino siege needs the Siege Artillery research
    app.m_entities->ToggleSiegeMode({rhinoId});
    if (rhino->state != ActionState::Transform || rhino->pendingArchetypeId != 15)
    {
        std::cerr << "[Test] Error: Rhino did not enter ActionState::Transform with pendingArchetypeId 15!"
                  << std::endl;
        return false;
    }
    // Simulate deployment animation steps (5 frames @ 0.09s = 0.45s)
    for (int step = 0; step < 6; ++step)
    {
        app.m_entities->Update(0.10f, *app.m_map, *app.m_pathfinder);
    }
    if (rhino->archetypeId != 15 || !rhino->isSiegeMode || rhino->burstCount != 2
        || std::abs(rhino->burstDelay - 0.1f) > 0.01f || std::abs(rhino->attackCooldown - 0.6f) > 0.01f
        || rhino->state != ActionState::Idle)
    {
        std::cerr << "[Test] Error: Rhino siege mode stats mismatch after transform! arch=" << rhino->archetypeId
                  << " burst=" << rhino->burstCount << " delay=" << rhino->burstDelay << " cd=" << rhino->attackCooldown
                  << " state=" << static_cast<int>(rhino->state) << std::endl;
        return false;
    }
    std::cout << "  - Rhino siege deployment animation & 2-shot burst validated (delay: 0.1s, cooldown: 0.9s)."
              << std::endl;

    // Test undeployment back to mobile mode
    app.m_entities->ToggleSiegeMode({rhinoId});
    if (rhino->state != ActionState::Transform || rhino->pendingArchetypeId != 10)
    {
        std::cerr << "[Test] Error: Rhino undeployment did not enter ActionState::Transform with pendingArchetypeId 10!"
                  << std::endl;
        return false;
    }
    for (int step = 0; step < 6; ++step)
    {
        app.m_entities->Update(0.10f, *app.m_map, *app.m_pathfinder);
    }
    if (rhino->archetypeId != 10 || rhino->isSiegeMode || rhino->state != ActionState::Idle)
    {
        std::cerr << "[Test] Error: Rhino undeployment back to archetype 10 failed! arch=" << rhino->archetypeId
                  << " isSiege=" << rhino->isSiegeMode << std::endl;
        return false;
    }
    std::cout << "  - Rhino undeployment animation back to mobile mode validated." << std::endl;

    // Re-enter siege mode for the wall-switch combat test below
    app.m_entities->ToggleSiegeMode({rhinoId});
    for (int step = 0; step < 6; ++step)
    {
        app.m_entities->Update(0.10f, *app.m_map, *app.m_pathfinder);
    }

    // D. Grad MLRS siege range check (13 tiles, as in the original)
    const auto *gradSiegeCfg = app.m_entities->GetUnitConfig(16);
    if (!gradSiegeCfg || gradSiegeCfg->attackRange < 12.9f)
    {
        std::cerr << "[Test] Error: Grad MLRS siege attack range is not 13! range="
                  << (gradSiegeCfg ? gradSiegeCfg->attackRange : -1.0f) << std::endl;
        return false;
    }
    std::cout << "  - Grad MLRS siege range validated: 13 tiles." << std::endl;

    // E. Wall auto-targeting immediate switch to enemy unit
    app.m_map->SetWall(14, 10, Faction::Confederation, 100);
    rhino->targetWallTileX = 14;
    rhino->targetWallTileY = 10;
    // Spawn enemy confederation rifleman at (12, 10) within attack range
    int enemyInfId = app.m_entities->SpawnUnitByArchetype(1, 12, 10, Faction::Confederation);
    app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    if (rhino->targetWallTileX >= 0 || rhino->targetEntityId != enemyInfId)
    {
        std::cerr << "[Test] Error: Unit did not immediately break off wall attack to engage enemy unit! targetWall="
                  << rhino->targetWallTileX << " targetEntity=" << rhino->targetEntityId << std::endl;
        return false;
    }
    std::cout << "  - Immediate target switch from enemy wall to enemy unit validated!" << std::endl;

    // 9. Test New Raycast Bullet, Combat Move-Interrupt, Tank Recoil Cooldown & Camera Bounds
    std::cout << "[Test] Validating Raycast Bullet, Move Order Retreat (No Freezing), Tank Recoil & Camera Bounds..."
              << std::endl;

    // A. Camera Bounds past map boundaries up to screen center (0x0 = half black, half map)
    app.m_camera->SetPosition(-9999.0f, -9999.0f);
    app.m_camera->ClampToBounds(app.m_map->GetWidth(), app.m_map->GetHeight());
    float expectedMinX = -(app.m_camera->GetViewportWidth() / app.m_camera->GetZoom()) * 0.5f;
    float expectedMinY = -(app.m_camera->GetViewportHeight() / app.m_camera->GetZoom()) * 0.5f;
    if (std::abs(app.m_camera->GetX() - expectedMinX) > 1.0f || std::abs(app.m_camera->GetY() - expectedMinY) > 1.0f)
    {
        std::cerr << "[Test] Error: Camera boundary clamp mismatch! got=(" << app.m_camera->GetX() << ","
                  << app.m_camera->GetY() << ") expected=(" << expectedMinX << "," << expectedMinY << ")" << std::endl;
        return false;
    }
    std::cout << "  - Camera boundary past map edge up to center (half-black screen) validated." << std::endl;

    // B. Raycast Bullet Instant Hit (Hitscan)
    int pShooterId = app.m_entities->SpawnUnitByArchetype(0, 10, 10, Faction::Resistance);   // Rifleman
    int eTargetId = app.m_entities->SpawnUnitByArchetype(1, 12, 10, Faction::Confederation); // Rifleman
    Unit *shooter = app.m_entities->GetUnit(pShooterId);
    Unit *victim = app.m_entities->GetUnit(eTargetId);
    int initialVictimHp = victim->currentHp;
    shooter->targetEntityId = eTargetId;
    shooter->cooldownTimer = 0.0f;
    // Advance 1 tick: bullet should deal instant raycast damage immediately
    app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    if (victim->currentHp >= initialVictimHp)
    {
        std::cerr << "[Test] Error: Raycast bullet did not deal damage instantaneously! initial=" << initialVictimHp
                  << " current=" << victim->currentHp << std::endl;
        return false;
    }
    std::cout << "  - Raycast hitscan bullet validated: instant damage applied on frame of shot (" << initialVictimHp
              << " -> " << victim->currentHp << " HP)." << std::endl;

    // C. Move Order While In Combat (Unit must retreat and NOT freeze on the spot)
    // The shooter is currently in combat with targetEntityId = eTargetId.
    // Issue a regular move order to (2, 2)
    app.m_entities->IssueMoveOrder({pShooterId}, 2, 2, *app.m_pathfinder, false);
    if (shooter->targetEntityId != 0 || shooter->state != ActionState::Move || shooter->path.empty())
    {
        std::cerr << "[Test] Error: Move order did not set ActionState::Move or clear combat target!" << std::endl;
        return false;
    }
    int startTileX = shooter->tileX;
    int startTileY = shooter->tileY;
    // Advance simulation for 40 ticks: unit must drive towards (2, 2) and NOT freeze or re-lock target!
    for (int t = 0; t < 40; ++t)
    {
        app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    }
    if (shooter->tileX == startTileX && shooter->tileY == startTileY)
    {
        std::cerr << "[Test] Error: Unit froze on the spot after move order during combat!" << std::endl;
        return false;
    }
    if (shooter->targetEntityId != 0)
    {
        std::cerr << "[Test] Error: Unit executing pure move order re-acquired enemy target!" << std::endl;
        return false;
    }
    std::cout << "  - Move order during combat validated: unit successfully retreated from (" << startTileX << ","
              << startTileY << ") to (" << shooter->tileX << "," << shooter->tileY << ") without freezing."
              << std::endl;

    // D. Tank Firing Animation and Cooldown Transition (Hammer: single-shot recoil, returns to idle during cooldown)
    int hammerId = app.m_entities->SpawnUnitByArchetype(9, 15, 15, Faction::Confederation); // T-21 Hammer
    int dummyId = app.m_entities->SpawnUnitByArchetype(8, 17, 15, Faction::Resistance);     // Target dummy
    Unit *hammer = app.m_entities->GetUnit(hammerId);
    hammer->turretTurnStepDelay = 0.0f;
    hammer->turnStepDelay = 0.0f; // instant aiming (a zero turret delay falls back to the hull one)
    hammer->targetEntityId = dummyId;
    hammer->cooldownTimer = 0.0f;
    app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    if (hammer->state != ActionState::Attack || hammer->attackAnimTimer <= 0.0f)
    {
        std::cerr << "[Test] Error: Hammer did not enter ActionState::Attack on firing shell!" << std::endl;
        return false;
    }
    // Advance 0.25s: attack anim (0.22s) should finish, returning hammer to ActionState::Idle during reload cooldown
    for (int t = 0; t < 5; ++t)
    {
        app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    }
    if (hammer->state != ActionState::Idle || hammer->cooldownTimer <= 0.0f)
    {
        std::cerr << "[Test] Error: Hammer stuck in ActionState::Attack during reload cooldown! state="
                  << static_cast<int>(hammer->state) << " cooldownTimer=" << hammer->cooldownTimer << std::endl;
        return false;
    }
    std::cout << "  - Tank firing single-shot recoil validated: returns to ActionState::Idle during reload cooldown "
                 "(no machine-gun loop)."
              << std::endl;

    // E. Validate Wall Rubble Remnants, Building Production Animation, Flamethrower & Fortress Hull Aiming
    // 1. Wall Rubble: Destroy wall at (30, 30)
    app.m_map->SetWall(30, 30, Faction::Confederation, 100);
    app.m_map->DamageWall(30, 30, 200); // Fatal damage
    if (app.m_map->GetWall(30, 30) != 0 || !app.m_map->IsPassable(30, 30))
    {
        std::cerr << "[Test] Error: Destroyed wall did not clear wall grid or restore passability!" << std::endl;
        return false;
    }
    std::cout << "  - Wall destruction leaves passable rubble remnant on ground without explosion." << std::endl;

    // 2. Building Production Flag: Factory & Barracks
    int barId = app.m_entities->SpawnBuildingByIndex(10, 35, 35, Faction::Resistance, 1.0f); // Barracks
    Building *testBEnt = app.m_entities->GetBuilding(barId);
    if (testBEnt->isProducing)
    {
        std::cerr << "[Test] Error: Idle barracks was marked as isProducing!" << std::endl;
        return false;
    }
    app.m_economy->QueueUnit(0, 80, 5.0f, barId, *app.m_entities);
    app.m_economy->Update(0.1f, *app.m_entities, *app.m_map, *app.m_pathfinder);
    if (!testBEnt->isProducing)
    {
        std::cerr << "[Test] Error: Barracks in active production was not marked as isProducing!" << std::endl;
        return false;
    }
    std::cout << "  - Building active production animation toggle validated (idle=static, producing=active)."
              << std::endl;

    // 3. Fortress rotating machine gun turret & strict orthogonal movement (cannot move diagonally)
    int fortId = app.m_entities->SpawnUnitByArchetype(7, 40, 40, Faction::Confederation);     // TM-40 Fortress
    int targetDummyId = app.m_entities->SpawnUnitByArchetype(0, 42, 40, Faction::Resistance); // East of fortress
    Unit *fortress = app.m_entities->GetUnit(fortId);
    fortress->facingDirection = 2; // Hull faces South
    fortress->turretTurnStepDelay = 0.0f;
    fortress->turnStepDelay = 0.0f;
    fortress->targetEntityId = targetDummyId;
    fortress->cooldownTimer = 0.0f;
    app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    if (fortress->turretFacing != 0)
    {
        std::cerr << "[Test] Error: Fortress machine gun turret did not rotate towards target (East=0)! Got: "
                  << fortress->turretFacing << std::endl;
        return false;
    }
    if (fortress->facingDirection != 2)
    {
        std::cerr << "[Test] Error: Fortress hull rotated when firing while idle! facingDirection="
                  << fortress->facingDirection << std::endl;
        return false;
    }
    std::cout << "  - Fortress rotating machine gun turret validated (turret facing 0 East, hull retained South)."
              << std::endl;

    // Validate Fortress firing animation (ActionState::Attack with advancing animFrame for muzzle flashes)
    if (fortress->state != ActionState::Attack || fortress->attackAnimTimer <= 0.0f)
    {
        std::cerr << "[Test] Error: Fortress did not enter Attack state with active attackAnimTimer! state="
                  << static_cast<int>(fortress->state) << " anim=" << fortress->attackAnimTimer
                  << " cd=" << fortress->cooldownTimer << " target=" << fortress->targetEntityId
                  << " dmg=" << fortress->attackDamage << " range=" << fortress->attackRange << std::endl;
        return false;
    }
    int initialAnimFrame = fortress->animFrame;
    app.m_entities->Update(0.06f, *app.m_map, *app.m_pathfinder);
    if (fortress->animFrame <= initialAnimFrame)
    {
        std::cerr << "[Test] Error: Fortress firing animation frames did not advance! frame=" << fortress->animFrame
                  << std::endl;
        return false;
    }
    std::cout << "  - Fortress firing animation validated: ActionState::Attack with muzzle flash frame advancement ("
              << initialAnimFrame << " -> " << fortress->animFrame << ")." << std::endl;

    // The Fortress moves along all 8 directions like every other vehicle: open ground (40,40) -> (45,45)
    // is a straight diagonal of 5 steps
    std::vector<PathNode> fortPath;
    app.m_pathfinder->FindPath(40, 40, 45, 45, fortPath, fortId);
    int fortDiagonal = 0;
    {
        int px = 40, py = 40;
        for (const auto &node : fortPath)
        {
            if (node.x != px && node.y != py)
                ++fortDiagonal;
            px = node.x;
            py = node.y;
        }
    }
    if (fortPath.size() != 5 || fortDiagonal != 5)
    {
        std::cerr << "[Test] Error: Fortress must drive diagonally! steps=" << fortPath.size()
                  << " diagonal=" << fortDiagonal << std::endl;
        return false;
    }
    std::cout << "  - Fortress diagonal movement validated: (40,40) -> (45,45) in 5 diagonal steps." << std::endl;

    // 10. Validating Friendly Wall Raycast Pass-through, Explicit Manual Wall Targeting, Wall Rubble 1759, Crouching &
    // Fortress Flamethrower
    std::cout << "[Test] Validating Wall Targeting, Rubble 1759, Fortress Flamethrower & Crouching Attack..."
              << std::endl;

    // A. Explicit Manual Wall Targeting (any wall, even friendly, can be explicitly targeted)
    int testShooterId = app.m_entities->SpawnUnitByArchetype(0, 20, 20, Faction::Resistance);
    app.m_map->SetWall(21, 20, Faction::Resistance, 100);
    app.m_entities->IssueAttackWallOrder({testShooterId}, 21, 20, *app.m_pathfinder);
    Unit *testShooter = app.m_entities->GetUnit(testShooterId);
    if (testShooter->targetWallTileX != 21 || !testShooter->isManualWallTarget)
    {
        std::cerr << "[Test] Error: Explicit manual wall targeting failed for friendly wall!" << std::endl;
        return false;
    }
    std::cout << "  - Manual wall targeting validated: explicit order targets wall (any faction) with "
                 "isManualWallTarget=true."
              << std::endl;

    // A2. A shell (flying projectile) brings down an own wall that was ordered to be destroyed
    {
        const int tankId = app.m_entities->SpawnUnitByArchetype(10, 50, 20, Faction::Resistance);
        Unit *tank = app.m_entities->GetUnit(tankId);
        if (tank && tank->projectile == ProjectileType::Shell)
        {
            app.m_map->SetWall(53, 20, Faction::Resistance, 100);
            const int before = app.m_map->GetWallHp(53, 20);
            app.m_entities->IssueAttackWallOrder({tankId}, 53, 20, *app.m_pathfinder);
            for (int i = 0; i < 400 && app.m_map->GetWall(53, 20) > 0 && app.m_map->GetWallHp(53, 20) >= before; ++i)
                app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
            if (app.m_map->GetWall(53, 20) > 0 && app.m_map->GetWallHp(53, 20) >= before)
            {
                std::cerr << "[Test] Error: a shell did not damage an own wall that was ordered destroyed!" << std::endl;
                return false;
            }
            std::cout << "  - Shell damages an own wall ordered destroyed." << std::endl;
            app.m_map->DamageWall(53, 20, 100000);
        }
        if (tank)
            tank->currentHp = 0;
    }

    // A3. Grad: single rockets within 6 tiles while walking, 6-rocket salvos within 13 tiles deployed
    {
        const int gradId = app.m_entities->SpawnUnitByArchetype(13, 60, 20, Faction::Confederation);
        const int gradSiegeId = app.m_entities->SpawnUnitByArchetype(16, 62, 20, Faction::Confederation);
        const Unit *grad = app.m_entities->GetUnit(gradId);
        const Unit *gradSiege = app.m_entities->GetUnit(gradSiegeId);
        if (!grad || !gradSiege || grad->burstCount != 1 || std::abs(grad->attackRange - 6.0f) > 0.01f
            || gradSiege->burstCount != 6 || std::abs(gradSiege->attackRange - 13.0f) > 0.01f || grad->moveSpeed <= 0.0f)
        {
            std::cerr << "[Test] Error: Grad forms: walking 1 rocket / range 6 and siege 6 rockets / range 13 expected!"
                      << std::endl;
            return false;
        }
        std::cout << "  - Grad forms validated (walking 1 rocket at 6 tiles, siege salvo of 6 at 13)." << std::endl;
        app.m_entities->GetUnit(gradId)->currentHp = 0;
        app.m_entities->GetUnit(gradSiegeId)->currentHp = 0;
    }

    // B. Friendly Wall Raycast Pass-Through (non-targeted stray fire ignores friendly wall)
    int enemyDummyId = app.m_entities->SpawnUnitByArchetype(0, 23, 20, Faction::Confederation);
    Entity *enemyDummy = app.m_entities->GetEntity(enemyDummyId);
    enemyDummy->currentHp = 100;
    enemyDummy->cooldownTimer = 10.0f; // Dummy does not fire
    int prevWallHp = app.m_map->GetWallHp(21, 20);
    // Shooter at (20,20) shoots at dummy at (23,20). Friendly wall is at (21,20).
    testShooter->targetWallTileX = -1;
    testShooter->targetWallTileY = -1;
    testShooter->isManualWallTarget = false;
    testShooter->targetEntityId = enemyDummyId;
    testShooter->cooldownTimer = 0.0f;
    app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    if (app.m_map->GetWallHp(21, 20) < prevWallHp)
    {
        std::cerr << "[Test] Error: Friendly wall took damage from allied raycast!" << std::endl;
        return false;
    }
    if (enemyDummy->currentHp >= 100)
    {
        std::cerr << "[Test] Error: Allied raycast was blocked by friendly wall and did not damage target!"
                  << std::endl;
        return false;
    }
    std::cout << "  - Friendly wall raycast pass-through validated: allied shots pass through friendly walls."
              << std::endl;

    // B2. Enemy Wall Pass-Through when targeting Enemy Base / Unit (walls block movement, not projectiles)
    app.m_map->SetWall(21, 20, Faction::Confederation, 100);
    int prevEnemyWallHp = app.m_map->GetWallHp(21, 20);
    enemyDummy->currentHp = 100;
    testShooter->targetEntityId = enemyDummyId;
    testShooter->cooldownTimer = 0.0f;
    app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    if (app.m_map->GetWallHp(21, 20) < prevEnemyWallHp)
    {
        std::cerr << "[Test] Error: Enemy wall took damage when shooting at enemy unit behind it!" << std::endl;
        return false;
    }
    if (enemyDummy->currentHp >= 100)
    {
        std::cerr << "[Test] Error: Bullets did not fly past enemy wall to hit targeted enemy entity!" << std::endl;
        return false;
    }
    std::cout << "  - Enemy wall pass-through validated: bullets fly past enemy wall directly to targeted base/unit."
              << std::endl;

    // C. Wall Destruction Rubble is compact single-tile masonry ruin 1778
    app.m_map->DamageWall(21, 20, 1000);
    if (app.m_map->GetWallRubble(21, 20) != 1778)
    {
        std::cerr << "[Test] Error: Wall rubble is not authentic compact composite 1778! Got: "
                  << app.m_map->GetWallRubble(21, 20) << std::endl;
        return false;
    }
    std::cout << "  - Wall rubble remnant 1778 validated on destruction (compact single-tile ruin)." << std::endl;

    // D. Fortress Flamethrower Upgrade & Range <= 1 Tile
    app.m_entities->SetFortressFlamethrowerUnlocked(Players::OwnerOf(Faction::Confederation), true);
    fortress = app.m_entities->GetUnit(fortId);
    if (!fortress)
    {
        fortId = app.m_entities->SpawnUnitByArchetype(7, 40, 40, Faction::Confederation);
        fortress = app.m_entities->GetUnit(fortId);
    }
    fortress->tileX = 40;
    fortress->tileY = 40;
    int dummyCloseId = app.m_entities->SpawnUnitByArchetype(0, 41, 40, Faction::Resistance);
    Entity *dummyClose = app.m_entities->GetEntity(dummyCloseId);
    dummyClose->alive = true;
    dummyClose->currentHp = 1000;
    dummyClose->state = ActionState::Idle;
    dummyClose->tileX = 41; // dx=1, dy=0 -> close
    dummyClose->tileY = 40;
    fortress->targetEntityId = dummyCloseId;
    fortress->cooldownTimer = 0.0f;
    app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    if (fortress->projectile != ProjectileType::Flame)
    {
        std::cerr << "[Test] Error: Fortress did not use flame when distance <= 1 tile with upgrade!" << std::endl;
        return false;
    }
    dummyClose->alive = true;
    dummyClose->currentHp = 1000;
    dummyClose->state = ActionState::Idle;
    dummyClose->tileX = 43; // dx=3, dy=0 -> far (>1 tile)
    dummyClose->tileY = 40;
    fortress->targetEntityId = dummyCloseId;
    fortress->cooldownTimer = 0.0f;
    fortress->projectile = ProjectileType::Bullet;
    for (int i = 0; i < 4; ++i)
        app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    // With the upgrade a Fortress that knows its target closes in to the flame's reach instead of shooting from afar
    if (fortress->projectile == ProjectileType::Flame || (fortress->state != ActionState::Move && fortress->path.empty()))
    {
        std::cerr << "[Test] Error: Fortress with the upgrade did not close in on a target 3 tiles away!" << std::endl;
        return false;
    }
    if (std::abs(fortress->attackRange - 6.0f) > 0.01f)
    {
        std::cerr << "[Test] Error: Fortress machine-gun range was not restored after the approach!" << std::endl;
        return false;
    }
    std::cout << "  - Fortress flamethrower rule validated: flame <= 1 tile with upgrade, it closes in on far targets."
              << std::endl;

    // E. Heavy Assault / Grenadier Crouching Posture During Attack Cooldown
    dummyClose->tileX = 32;
    dummyClose->tileY = 30; // 2 tiles from Grenadier (well within range 5.0)
    int grenId = app.m_entities->SpawnUnitByArchetype(2, 30, 30, Faction::Resistance); // Grenadier (archetype 2)
    Entity *gren = app.m_entities->GetEntity(grenId);
    gren->targetEntityId = dummyCloseId;
    gren->state = ActionState::Attack;
    gren->attackAnimTimer = 0.05f;
    gren->cooldownTimer = 1.0f;
    app.m_entities->Update(0.06f, *app.m_map, *app.m_pathfinder); // attackAnimTimer expires
    if (gren->state != ActionState::Attack)
    {
        std::cerr << "[Test] Error: Grenadier did not stay in crouching Attack state during cooldown!" << std::endl;
        return false;
    }
    std::cout << "  - Grenadier / Heavy Assault crouching posture during combat cooldown validated." << std::endl;

    // F. Debug Mode Passability Grid State & Inset
    app.m_gameplayController->SetDebugMode(true);
    if (!app.m_gameplayController->IsShowDebugGrid())
    {
        std::cerr << "[Test] Error: Debug passability grid not enabled by default in debug mode!" << std::endl;
        return false;
    }
    app.m_gameplayController->SetShowDebugGrid(false);
    if (app.m_gameplayController->IsShowDebugGrid())
    {
        std::cerr << "[Test] Error: Debug passability grid toggle failed!" << std::endl;
        return false;
    }
    app.m_gameplayController->SetShowDebugGrid(true);
    std::cout << "  - Debug passability grid toggle (70% transparency, 2px contracted grid) validated." << std::endl;

    // D. Unit-Wall Collision Snapping
    // Place unit right next to wall and verify movement update does not enter wall tile
    testShooter->tileX = 21;
    testShooter->tileY = 19;
    testShooter->subX = 0.0f;
    testShooter->subY = 0.0f;
    app.m_map->SetWall(22, 19, Faction::Confederation, 100);
    // Attempt to move East directly into wall
    testShooter->path = {{22, 19}};
    testShooter->pathIndex = 0;
    testShooter->state = ActionState::Move;
    app.m_entities->Update(0.1f, *app.m_map, *app.m_pathfinder);
    if (testShooter->tileX == 22)
    {
        std::cerr << "[Test] Error: Unit stepped inside wall tile!" << std::endl;
        return false;
    }
    std::cout << "  - Unit-wall collision barrier validated: unit snapped to tile center without penetrating wall."
              << std::endl;

    // E. Bunker Construction & Autonomous Defense Combat
    std::cout << "[Test] Validating Bunker Construction & Autonomous Power-Independent Combat..." << std::endl;
    // Spawn Resistance Bunker #13 under construction (0.0f)
    int bunkerId = app.m_entities->SpawnBuildingByIndex(13, 25, 25, Faction::Resistance, 0.0f);
    Building *bunker = app.m_entities->GetBuilding(bunkerId);
    if (!bunker || bunker->buildProgress >= 1.0f)
    {
        std::cerr << "[Test] Error: Bunker was not spawned under construction!" << std::endl;
        return false;
    }
    // Spawn enemy dummy near bunker
    int enemyDummyBunkerId = app.m_entities->SpawnUnitByArchetype(0, 27, 25, Faction::Confederation);
    Entity *enemyDummyBunker = app.m_entities->GetEntity(enemyDummyBunkerId);
    enemyDummyBunker->currentHp = 100;
    int enemyHpBefore = enemyDummyBunker->currentHp;

    // Update 0.5s while under construction: bunker MUST NOT fire
    app.m_entities->Update(0.5f, *app.m_map, *app.m_pathfinder);
    if (enemyDummyBunker->currentHp < enemyHpBefore || bunker->state == ActionState::Attack)
    {
        std::cerr << "[Test] Error: Bunker fired while under construction (progress=" << bunker->buildProgress << ")!"
                  << std::endl;
        return false;
    }

    // Advance construction to completion (1.0f) and simulate power blackout (isPowered = false)
    bunker->buildProgress = 1.0f;
    bunker->isPowered = false;
    bunker->cooldownTimer = 0.0f;
    enemyHpBefore = enemyDummyBunker->currentHp;

    // Bunker must fire because concrete bunkers are autonomous manned pillboxes
    app.m_entities->Update(0.1f, *app.m_map, *app.m_pathfinder);
    if (enemyDummyBunker->currentHp >= enemyHpBefore)
    {
        std::cerr << "[Test] Error: Completed autonomous bunker failed to attack enemy dummy! enemyHp="
                  << enemyDummyBunker->currentHp << std::endl;
        return false;
    }
    std::cout << "  - Bunker autonomous combat validated: inactive during construction, fires upon completion even "
                 "without power grid."
              << std::endl;

    // 11. Test Fog of War Lifecycle & Symmetrical Blindness (Hidden -> Visible -> Explored)
    std::cout << "[Test] Validating Fog of War: Hidden -> Visible -> Explored & AI Fog Blindness..." << std::endl;
    // Map m0 has fog enabled. Pick distant tile (5, 35) far from player base
    app.m_map->ResetAllFog(FogState::Hidden);
    app.m_map->SetFogEnabled(true);
    app.m_entities->UpdateFogOfWar(*app.m_map);

    // Initial state: distant tile (5, 35) must be Hidden (pitch black)
    if (app.m_map->GetFogState(5, 35) != FogState::Hidden)
    {
        std::cerr << "[Test] Error: Unexplored tile (5, 35) is not FogState::Hidden!" << std::endl;
        return false;
    }

    // Spawn a scout unit at (5, 35): tile must become Visible
    int scoutId = app.m_entities->SpawnUnitByArchetype(0, 5, 35, Faction::Resistance);
    app.m_entities->UpdateFogOfWar(*app.m_map);
    if (app.m_map->GetFogState(5, 35) != FogState::Visible)
    {
        std::cerr << "[Test] Error: Tile with player scout (5, 35) did not become FogState::Visible!" << std::endl;
        return false;
    }

    // Move scout far away to (20, 20): tile (5, 35) must demote to Explored (dimmed shroud)
    Unit *scout = app.m_entities->GetUnit(scoutId);
    scout->tileX = 20;
    scout->tileY = 20;
    app.m_entities->UpdateFogOfWar(*app.m_map);
    if (app.m_map->GetFogState(5, 35) != FogState::Explored)
    {
        std::cerr << "[Test] Error: Vacated tile (5, 35) did not demote to FogState::Explored! state="
                  << static_cast<int>(app.m_map->GetFogState(5, 35)) << std::endl;
        return false;
    }

    // Spawn enemy rifleman on Explored tile (5, 35): player must NOT see him (not Visible)
    int hiddenEnemyId = app.m_entities->SpawnUnitByArchetype(1, 5, 35, Faction::Confederation);
    Entity *hiddenEnemy = app.m_entities->GetEntity(hiddenEnemyId);
    if (app.m_map->GetFogState(hiddenEnemy->tileX, hiddenEnemy->tileY) == FogState::Visible)
    {
        std::cerr << "[Test] Error: Enemy on Explored tile is visible in fog!" << std::endl;
        return false;
    }

    // Spawn an isolated player unit at (2, 2) where no Confederation units or buildings exist: AI must NOT see him
    int lonePlayerId = app.m_entities->SpawnUnitByArchetype(0, 2, 2, Faction::Resistance);
    Entity *lonePlayer = app.m_entities->GetEntity(lonePlayerId);
    if (app.m_entities->IsEntityVisibleToAi(*lonePlayer))
    {
        std::cerr << "[Test] Error: Confederation AI can see isolated player unit across the map in fog!" << std::endl;
        return false;
    }

    // Cleanup test entities
    scout->alive = false;
    hiddenEnemy->alive = false;
    lonePlayer->alive = false;
    std::cout << "  - Fog of War dynamic lifecycle validated: Hidden -> Visible -> Explored." << std::endl;
    std::cout << "  - Player & AI symmetrical fog blindness validated: units hidden in fog cannot be targeted."
              << std::endl;

    // 12. Test AI Difficulty Modes & YAML Configuration (Easy, Medium, Hard, Hardcore)
    std::cout << "[Test] Validating AI Difficulty System & assets/characters/difficulty.yaml..." << std::endl;
    if (!app.m_aiController->LoadDifficultyConfig(LoaderResources::aiDifficulty()))
    {
        std::cerr << "[Test] Error: Failed to load difficulty configuration from assets/characters/difficulty.yaml!"
                  << std::endl;
        return false;
    }

    const auto &diffConfigs = app.m_aiController->GetAllDifficultyConfigs();
    if (diffConfigs.size() < 4)
    {
        std::cerr << "[Test] Error: Expected at least 4 difficulty tiers, found " << diffConfigs.size() << std::endl;
        return false;
    }

    // Verify Easy: 50% resource handicap
    auto itEasy = diffConfigs.find(AIDifficulty::Easy);
    if (itEasy == diffConfigs.end() || itEasy->second.startingCredits != 1000
        || itEasy->second.resourceMultiplier != 0.50f)
    {
        std::cerr << "[Test] Error: Easy difficulty configuration invalid!" << std::endl;
        return false;
    }

    // Verify Medium: 20% resource handicap
    auto itMed = diffConfigs.find(AIDifficulty::Medium);
    if (itMed == diffConfigs.end() || itMed->second.startingCredits != 1600
        || itMed->second.resourceMultiplier != 0.80f)
    {
        std::cerr << "[Test] Error: Medium difficulty configuration invalid!" << std::endl;
        return false;
    }

    // Verify Hard: No handicap (100% fair)
    auto itHard = diffConfigs.find(AIDifficulty::Hard);
    if (itHard == diffConfigs.end() || itHard->second.startingCredits != 2000
        || itHard->second.resourceMultiplier != 1.00f)
    {
        std::cerr << "[Test] Error: Hard difficulty configuration invalid!" << std::endl;
        return false;
    }

    // Verify Hardcore: +30% resources, +10% production speed
    auto itHc = diffConfigs.find(AIDifficulty::Hardcore);
    if (itHc == diffConfigs.end() || itHc->second.startingCredits != 2600 || itHc->second.resourceMultiplier != 1.30f
        || itHc->second.productionSpeedMultiplier != 1.10f)
    {
        std::cerr << "[Test] Error: Hardcore difficulty configuration invalid!" << std::endl;
        return false;
    }

    // Test difficulty application in simulation
    app.m_aiController->SetDifficulty(AIDifficulty::Easy);
    app.m_aiController->Reset();
    if (app.m_aiController->GetEnemyCredits() != 1000)
    {
        std::cerr << "[Test] Error: Easy AI starting credits mismatch: " << app.m_aiController->GetEnemyCredits()
                  << std::endl;
        return false;
    }

    app.m_aiController->SetDifficulty(AIDifficulty::Hardcore);
    app.m_aiController->Reset();
    if (app.m_aiController->GetEnemyCredits() != 2600)
    {
        std::cerr << "[Test] Error: Hardcore AI starting credits mismatch: " << app.m_aiController->GetEnemyCredits()
                  << std::endl;
        return false;
    }
    std::cout << "  - AI Difficulty modes loaded from assets/characters/difficulty.yaml successfully:" << std::endl;
    std::cout << "    * Easy: -50% resources (credits=1000, income=7)" << std::endl;
    std::cout << "    * Medium: -20% resources (credits=1600, income=12)" << std::endl;
    std::cout << "    * Hard: fair (credits=2000, income=15)" << std::endl;
    std::cout << "    * Hardcore: +30% resources (credits=2600, income=20), +10% production speed (1.10x)" << std::endl;

    // 13. Test Moving Unit Damage Reduction (50% - 100% fast C-code RNG algorithm)
    std::cout << "[Test] Validating Moving Unit Damage Reduction (50-100% fast C algorithm)..." << std::endl;
    int testMovingUnitId = app.m_entities->SpawnUnitByArchetype(0, 10, 10, Faction::Resistance);
    Unit *testUnit = app.m_entities->GetUnit(testMovingUnitId);
    if (!testUnit)
    {
        std::cerr << "[Test] Error: Failed to spawn test unit!" << std::endl;
        return false;
    }

    // Stationary test: 100% damage must always be applied
    testUnit->state = ActionState::Idle;
    testUnit->path.clear();
    const int baseDamage = 100;
    // The kind multiplier is tested elsewhere: here a neutral one isolates the moving-target roll
    WeaponBalance::LoadText(R"({"multipliers":{"bullet":{"infantry":1.0}}})");
    for (int i = 0; i < 20; ++i)
    {
        int dmg = EntityManager::ApplyMovingUnitDamage(baseDamage, testUnit, ProjectileType::Bullet);
        if (dmg != baseDamage)
        {
            std::cerr << "[Test] Error: Stationary unit took reduced damage: " << dmg << " instead of " << baseDamage
                      << std::endl;
            return false;
        }
    }

    // Moving test: 50% - 100% damage
    testUnit->state = ActionState::Move;
    int minDmgSeen = 999;
    int maxDmgSeen = -999;
    for (int i = 0; i < 500; ++i)
    {
        int dmg = EntityManager::ApplyMovingUnitDamage(baseDamage, testUnit, ProjectileType::Bullet);
        if (dmg < 50 || dmg > 100)
        {
            std::cerr << "[Test] Error: Moving unit took out-of-bounds damage: " << dmg << " (expected 50..100)"
                      << std::endl;
            return false;
        }
        minDmgSeen = std::min(minDmgSeen, dmg);
        maxDmgSeen = std::max(maxDmgSeen, dmg);
    }
    if (minDmgSeen > 55 || maxDmgSeen < 95)
    {
        std::cerr << "[Test] Error: Moving unit damage distribution too narrow: min=" << minDmgSeen
                  << " max=" << maxDmgSeen << std::endl;
        return false;
    }
    WeaponBalance::Reload();
    testUnit->alive = false;
    std::cout << "  - Moving unit damage reduction validated: idle=100%, moving=50%..100% (observed: " << minDmgSeen
              << "%.." << maxDmgSeen << "%)." << std::endl;

    // 14. Test Bullet Muzzle Exit Point & localePosition (Default 0,0 & Turret Binding)
    std::cout << "[Test] Validating Bullet Muzzle Exit Point (turret binding & default center for non-turret)..."
              << std::endl;
    int testMuzzleUnitId = app.m_entities->SpawnUnitByArchetype(0, 15, 15, Faction::Resistance);
    Unit *mUnit = app.m_entities->GetUnit(testMuzzleUnitId);
    if (!mUnit)
    {
        std::cerr << "[Test] Error: Failed to spawn muzzle test unit!" << std::endl;
        return false;
    }

    // A. Units without rotating turret: localePosition is fully active and rotates with unit facingDirection
    float cx = mUnit->GetUnitCenterPixelX();
    float cy = mUnit->GetUnitCenterPixelY();
    float mX = 0.0f, mY = 0.0f;
    mUnit->hasRotatingTurret = false;
    mUnit->archetypeId = 0; // Infantry
    mUnit->muzzleOffsetX = 10.0f;
    mUnit->muzzleOffsetY = 2.0f;

    // Default facing 0 (East): forward=+X, lateral=+Y
    mUnit->facingDirection = 0;
    mUnit->GetMuzzlePoint(mX, mY);
    if (std::abs(mX - (cx + 10.0f)) > 0.01f || std::abs(mY - ((cy - 4.0f) + 2.0f)) > 0.01f)
    {
        std::cerr << "[Test] Error: Non-turret unit did not apply localePosition rotated with facingDirection!"
                  << std::endl;
        return false;
    }

    // Default zero offset (0, 0): returns unit center pivot
    mUnit->muzzleOffsetX = 0.0f;
    mUnit->muzzleOffsetY = 0.0f;
    mUnit->GetMuzzlePoint(mX, mY);
    if (std::abs(mX - cx) > 0.01f || std::abs(mY - (cy - 4.0f)) > 0.01f)
    {
        std::cerr << "[Test] Error: Non-turret unit default zero offset failed!" << std::endl;
        return false;
    }

    // B. Units with rotating turret (mUnit->hasRotatingTurret == true) attach strictly to turret and rotate with
    // turretFacing
    mUnit->hasRotatingTurret = true;
    mUnit->archetypeId = 6; // Assault Tank: turret pivot is at (cx - 2.0f, cy - 14.0f)
    mUnit->hasCustomTurretOffset = false;
    mUnit->muzzleOffsetX = 12.0f;
    mUnit->muzzleOffsetY = 3.0f;

    // Without a custom offset the pivot is the unit centre moved by the sprite's turret layer shift
    const float tPivotX = cx + mUnit->turretShiftX;
    const float tPivotY = cy + mUnit->turretShiftY;

    // Facing 0 (East): forward=+X, lateral=+Y
    mUnit->turretFacing = 0;
    mUnit->GetMuzzlePoint(mX, mY);
    if (std::abs(mX - (tPivotX + 12.0f)) > 0.01f || std::abs(mY - (tPivotY + 3.0f)) > 0.01f)
    {
        std::cerr << "[Test] Error: Facing East turret muzzle point mismatch! Got (" << mX << "," << mY
                  << ") expected (" << (tPivotX + 12.0f) << "," << (tPivotY + 3.0f) << ")" << std::endl;
        return false;
    }

    // Facing 2 (South): forward=+Y, lateral=-X
    mUnit->turretFacing = 2;
    mUnit->GetMuzzlePoint(mX, mY);
    if (std::abs(mX - (tPivotX - 3.0f)) > 0.01f || std::abs(mY - (tPivotY + 12.0f)) > 0.01f)
    {
        std::cerr << "[Test] Error: Facing South turret muzzle point mismatch! Got (" << mX << "," << mY
                  << ") expected (" << (tPivotX - 3.0f) << "," << (tPivotY + 12.0f) << ")" << std::endl;
        return false;
    }

    mUnit->alive = false;
    std::cout << "  - Bullet muzzle point validated: non-turret units strictly use default (0,0) center, turret units "
                 "strictly attach to elevated rotating turret."
              << std::endl;

    // 15. Test Projectile Spread Dispersion (random angle offset per shot & pinpoint when spread=0)
    std::cout << "[Test] Validating Projectile Spread Dispersion (random angle offset per shot & 0=pinpoint)..."
              << std::endl;
    int spreadShooterId = app.m_entities->SpawnUnitByArchetype(0, 10, 10, Faction::Resistance);
    Unit *spreadShooter = app.m_entities->GetUnit(spreadShooterId);
    if (!spreadShooter)
    {
        std::cerr << "[Test] Error: Failed to spawn shooter unit!" << std::endl;
        return false;
    }

    // A. Pinpoint high accuracy test when spread = 0
    spreadShooter->spreadAngle = 0.0f;
    const size_t projZeroStart = app.m_entities->GetProjectiles().size();
    for (int i = 0; i < 5; ++i)
    {
        app.m_entities->SpawnProjectileFromEntity(*spreadShooter, 50.0f, 50.0f, 200.0f, 50.0f, 0, *app.m_map);
    }
    const auto &projsZero = app.m_entities->GetProjectiles();
    for (size_t i = projZeroStart; i < projsZero.size(); ++i)
    {
        if (std::abs(projsZero[i].targetY - 50.0f) > 0.001f || std::abs(projsZero[i].targetX - 200.0f) > 0.001f)
        {
            std::cerr << "[Test] Error: Shot spread = 0 failed to produce pinpoint accuracy! Got ("
                      << projsZero[i].targetX << "," << projsZero[i].targetY << ")" << std::endl;
            return false;
        }
    }

    // B. Dispersion test with authentic weapon spread (unset -1.0f or positive angle)
    spreadShooter->spreadAngle = -1.0f;
    const size_t projCountBefore = app.m_entities->GetProjectiles().size();
    for (int i = 0; i < 10; ++i)
    {
        app.m_entities->SpawnProjectileFromEntity(*spreadShooter, 50.0f, 50.0f, 200.0f, 50.0f, 0, *app.m_map);
    }
    const auto &projs = app.m_entities->GetProjectiles();
    if (projs.size() < projCountBefore + 10)
    {
        std::cerr << "[Test] Error: Spawned projectiles count mismatch!" << std::endl;
        return false;
    }
    std::set<int> distinctTargetYInts;
    for (size_t i = projCountBefore; i < projs.size(); ++i)
    {
        distinctTargetYInts.insert(static_cast<int>(std::round(projs[i].targetY * 10.0f)));
    }
    if (distinctTargetYInts.size() < 4)
    {
        std::cerr << "[Test] Error: Projectile spread is not varying across shots! Unique Y count: "
                  << distinctTargetYInts.size() << std::endl;
        return false;
    }
    spreadShooter->alive = false;
    std::cout << "  - Projectile spread validated: spread=0 yields 100% pinpoint accuracy; spread>0 produced "
              << distinctTargetYInts.size() << " distinct trajectories." << std::endl;

    // 16. Splash Damage Percentages, Flame Death Animation & Simultaneous Dual Firing
    std::cout << "[Test] Validating Weapon Splash Percentages, Flame Burning Death & Multi-Weapon..." << std::endl;
    // A. Validate YAML splash percentages
    const auto *hammerCfg = app.m_entities->GetUnitConfig(9);
    const auto *zeusCfg = app.m_entities->GetUnitConfig(11);
    const auto *rhinoCfg = app.m_entities->GetUnitConfig(10);
    const auto *rhinoSiegeCfg = app.m_entities->GetUnitConfig(15);
    const auto *gradCfg = app.m_entities->GetUnitConfig(13);
    const auto *mlrsCfg = app.m_entities->GetUnitConfig(12);
    const auto *grenadierCfg = app.m_entities->GetUnitConfig(2);
    const auto *heavyAssaultCfg = app.m_entities->GetUnitConfig(3);

    // The numbers are the designer's to tune (character files); what must hold is that a unit's splash is the one of its
    // main weapon, for every unit that has a splash
    for (const UnitConfig *cfg :
         {hammerCfg, zeusCfg, rhinoCfg, rhinoSiegeCfg, gradCfg, mlrsCfg, grenadierCfg, heavyAssaultCfg})
    {
        if (!cfg || cfg->weapons.empty() || cfg->weapons[0].splashPercent != cfg->splashPercent)
        {
            std::cerr << "[Test] Error: the splash percent of a unit and of its main weapon differ ("
                      << (cfg ? cfg->id : std::string("missing config")) << ")" << std::endl;
            return false;
        }
    }
    std::cout << "  - Weapon splash damage: unit and main weapon agree (Hammer " << hammerCfg->splashPercent
              << "%, Zeus " << zeusCfg->splashPercent << "%, Rhino " << rhinoCfg->splashPercent << "%/"
              << rhinoSiegeCfg->splashPercent << "%)." << std::endl;

    // B. Validate 2x2 area Splash Damage execution on impact
    int primaryTargetId = app.m_entities->SpawnUnitByArchetype(0, 15, 15, Faction::Confederation);
    int splashTargetId
        = app.m_entities->SpawnUnitByArchetype(0, 16, 15, Faction::Confederation); // 1 tile away (within 2x2 area)
    int farTargetId
        = app.m_entities->SpawnUnitByArchetype(0, 20, 15, Faction::Confederation); // 5 tiles away (outside splash)
    Entity *priTarget = app.m_entities->GetEntity(primaryTargetId);
    Entity *splTarget = app.m_entities->GetEntity(splashTargetId);
    Entity *farTarget = app.m_entities->GetEntity(farTargetId);
    priTarget->currentHp = 100;
    splTarget->currentHp = 100;
    farTarget->currentHp = 100;

    // Spawn a shell from Zeus (damage 85, splash 20% -> 17 dmg to nearby targets)
    Entity testZeusShooter;
    testZeusShooter.id = 999;
    testZeusShooter.faction = Faction::Resistance;
    testZeusShooter.projectile = ProjectileType::Shell;
    testZeusShooter.attackDamage = 85;
    testZeusShooter.splashPercent = 20;
    testZeusShooter.spreadAngle = 0.0f; // pinpoint
    float priX = priTarget->GetUnitCenterPixelX();
    float priY = priTarget->GetUnitCenterPixelY();
    app.m_entities->SpawnProjectileFromEntity(
        testZeusShooter, priX - 60.0f, priY, priX, priY, primaryTargetId, *app.m_map);

    // Update projectiles until impact
    for (int t = 0; t < 20; ++t)
    {
        app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    }
    if (priTarget->currentHp >= 100)
    {
        std::cerr << "[Test] Error: Primary target took no damage from shell!" << std::endl;
        return false;
    }
    if (splTarget->currentHp >= 100)
    {
        std::cerr << "[Test] Error: 2x2 area splash target took no damage from shell!" << std::endl;
        return false;
    }
    if (farTarget->currentHp != 100)
    {
        std::cerr << "[Test] Error: Far target outside splash radius unexpectedly took damage!" << std::endl;
        return false;
    }
    std::cout << "  - 2x2 area splash damage validated: primary target damaged, neighbor in 2x2 area took "
              << (100 - splTarget->currentHp) << " splash damage, far target unharmed." << std::endl;
    // A shell leaves a small crater on the ground (projectiles.yaml: ground_marks)
    if (app.m_entities->GroundMarkCount() == 0)
    {
        std::cerr << "[Test] Error: a shell impact left no crater on the ground!" << std::endl;
        return false;
    }
    std::cout << "  - Shell impact left a crater on the ground." << std::endl;
    // The sniper fires its own rounds: hit-scan like a bullet, its own tracer and hit
    {
        const ProjectileSpec &sniper = Projectiles::of(ProjectileType::Sniper);
        const ProjectileSpec &bullet = Projectiles::of(ProjectileType::Bullet);
        if (Projectiles::FromKey("sniper") != ProjectileType::Sniper || !sniper.hitscan
            || sniper.sprite == bullet.sprite || sniper.impact == bullet.impact)
        {
            std::cerr
                << "[Test] Error: the sniper round must be its own hit-scan projectile with its own tracer and hit!"
                << std::endl;
            return false;
        }
        std::cout << "  - Sniper round: hit-scan, tracer " << sniper.sprite << ", hit animations "
                  << sniper.impact.front() << ".." << sniper.impact.back() << std::endl;
    }

    // C. Validate Flame burning infantry death (killedByFlame=true)
    int infantryVictimId = app.m_entities->SpawnUnitByArchetype(0, 18, 18, Faction::Confederation);
    Entity *flameVictim = app.m_entities->GetEntity(infantryVictimId);
    flameVictim->currentHp = 10;
    Entity flameShooter;
    flameShooter.id = 998;
    flameShooter.faction = Faction::Resistance;
    flameShooter.projectile = ProjectileType::Flame;
    flameShooter.attackDamage = 40;
    flameShooter.splashPercent = 0;
    flameShooter.spreadAngle = 0.0f;
    float vicX = flameVictim->GetUnitCenterPixelX();
    float vicY = flameVictim->GetUnitCenterPixelY();
    app.m_entities->SpawnProjectileFromEntity(
        flameShooter, vicX - 30.0f, vicY, vicX, vicY, infantryVictimId, *app.m_map);
    for (int t = 0; t < 10; ++t)
    {
        app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    }
    Unit *victimUnit = nullptr;
    for (auto &e : app.m_entities->GetAllEntities())
    {
        if (e.id == infantryVictimId)
        {
            victimUnit = e.AsUnit();
            break;
        }
    }
    if (!victimUnit || victimUnit->state != ActionState::Die || !victimUnit->killedByFlame)
    {
        std::cerr << "[Test] Error: Infantry killed by Flame did not trigger burning death (killedByFlame=true)!"
                  << std::endl;
        return false;
    }
    std::cout << "  - Flame burning infantry death animation state validated (killedByFlame=true, seq 259/260)."
              << std::endl;

    // 17. Validate Vehicle Movement & Rotation Mechanics (Left-to-Right turns through Down first, then Right)
    std::cout << "[Test] Validating Vehicle Movement & Rotation Mechanics (Left-to-Right turns through Down)..."
              << std::endl;
    // The earlier sections leave fights behind: end them, so nobody (chasing its attacker) drives into the test lane
    for (Entity &e : app.m_entities->GetAllEntities())
    {
        if (Unit *u = e.AsUnit(); u && u->alive && u->state != ActionState::Die)
        {
            u->targetEntityId = 0;
            u->lastAttackerId = 0;
            u->lastAttackedTimer = 0.0f;
            u->ClearPathState();
            u->ClearMoveTarget();
            u->isAttackMoving = false;
            if (u->state == ActionState::Move || u->state == ActionState::Attack)
                u->state = ActionState::Idle;
        }
        else if (Building *b = e.AsBuilding(); b && b->alive)
        {
            b->targetEntityId = 0;
            b->targetWallTileX = -1;
            b->targetWallTileY = -1;
            b->cooldownTimer = 100.0f;
        }
    }
    // Whatever the earlier sections left standing around the lane (their outcome depends on the damage balance) goes
    for (Entity &e : app.m_entities->GetAllEntities())
    {
        if (std::abs(e.tileX - 12) <= 5 && std::abs(e.tileY - 12) <= 5)
        {
            e.alive = false;
            e.currentHp = 0;
        }
    }
    int turnTestTankId = app.m_entities->SpawnUnitByArchetype(6, 12, 12, Faction::Confederation); // Hammer tank
    Unit *turnTestTank = app.m_entities->GetEntity(turnTestTankId)->AsUnit();
    turnTestTank->facingDirection = 4; // Facing West / Left
    turnTestTank->turretFacing = 4;
    turnTestTank->subX = 0.0f;
    turnTestTank->subY = 0.0f;
    turnTestTank->state = ActionState::Move;
    turnTestTank->path = {{13, 12}, {14, 12}}; // Move East / Right
    turnTestTank->pathIndex = 0;
    turnTestTank->turnStepDelay = 0.08f;
    turnTestTank->turnTimer = 0.0f;

    std::vector<int> observedFacings;
    observedFacings.push_back(turnTestTank->facingDirection);

    // Advance 4 turn ticks (0.08s each) and record vehicle facing
    for (int step = 0; step < 4; ++step)
    {
        app.m_entities->Update(0.081f, *app.m_map, *app.m_pathfinder);
        observedFacings.push_back(turnTestTank->facingDirection);
        // Vehicle must remain at initial tile (12, 12) while rotating in place!
        if (turnTestTank->tileX != 12 || turnTestTank->tileY != 12 || turnTestTank->subX != 0.0f)
        {
            std::cerr << "[Test] Error: Vehicle advanced position before completing in-place turn!" << std::endl;
            return false;
        }
    }

    // Expected sequence turning Left (4) to Right (0): 4 -> 3 -> 2 (Down) -> 1 -> 0 (Right)
    const std::vector<int> expectedFacings = {4, 3, 2, 1, 0};
    if (observedFacings != expectedFacings)
    {
        std::cerr << "[Test] Error: Vehicle turn sequence incorrect! Observed: ";
        for (int f : observedFacings)
            std::cerr << f << " ";
        std::cerr << " Expected: 4 3 2 1 0" << std::endl;
        return false;
    }

    // Advance further: vehicle is now aligned to 0 (Right), forward translation must occur
    app.m_entities->Update(0.1f, *app.m_map, *app.m_pathfinder);
    if (turnTestTank->subX <= 0.0f && turnTestTank->tileX <= 12)
    {
        std::cerr << "[Test] Error: Vehicle did not start forward movement after completing turn to 0!" << std::endl;
        return false;
    }

    // Validate infantry instant turn (no turn delay)
    int turnTestInfId = app.m_entities->SpawnUnitByArchetype(0, 15, 15, Faction::Confederation);
    Unit *turnTestInf = app.m_entities->GetEntity(turnTestInfId)->AsUnit();
    turnTestInf->facingDirection = 4;
    turnTestInf->state = ActionState::Move;
    turnTestInf->path = {{16, 15}};
    turnTestInf->pathIndex = 0;
    app.m_entities->Update(0.02f, *app.m_map, *app.m_pathfinder);
    if (turnTestInf->facingDirection != 0)
    {
        std::cerr << "[Test] Error: Infantry did not turn instantly to movement direction!" << std::endl;
        return false;
    }
    std::cout << "  - Vehicle Left-to-Right turning mechanics validated: rotated 4 -> 3 -> 2(Down) -> 1 -> 0(Right) in "
                 "place before moving forward."
              << std::endl;
    std::cout << "  - Infantry instant turn without rotation delay validated." << std::endl;

    // 18. Validate Tactical Support Assist (Chain Aggro with strict depth = 1)
    std::cout << "[Test] Validating Tactical Support Assist (Chain Aggro with depth = 1)..." << std::endl;
    // Spawn Enemy E at (52, 2) (Confederation)
    int suppEnemyId = app.m_entities->SpawnUnitByArchetype(0, 52, 2, Faction::Confederation);
    Entity *suppEnemy = app.m_entities->GetEntity(suppEnemyId);
    suppEnemy->currentHp = 500;
    suppEnemy->cooldownTimer = 999.0f; // Doesn't fire back

    // Spawn Frontline Ally B at (49, 2) (Resistance, attackRange = 5.0f).
    // dist(B, E) = 3 tiles <= 5.0f.
    int allyBId = app.m_entities->SpawnUnitByArchetype(0, 49, 2, Faction::Resistance);
    Unit *allyB = app.m_entities->GetEntity(allyBId)->AsUnit();
    allyB->targetEntityId = suppEnemyId;
    allyB->state = ActionState::Attack;
    allyB->cooldownTimer = 0.0f;

    // Spawn Ally A at (46, 2) (Resistance, attackRange = 5.0f).
    // dist(A, B) = 3 tiles <= 5.0f (within attack range of B).
    // dist(A, E) = 6 tiles > 5.0f (outside sight and attack range of enemy E).
    int allyAId = app.m_entities->SpawnUnitByArchetype(0, 46, 2, Faction::Resistance);
    Unit *allyA = app.m_entities->GetEntity(allyAId)->AsUnit();
    allyA->state = ActionState::Idle;
    allyA->targetEntityId = 0;

    // Spawn Ally C at (43, 2) (Resistance, attackRange = 5.0f).
    // dist(C, A) = 3 tiles <= 5.0f.
    // dist(C, B) = 6 tiles > 5.0f (outside range of B).
    int allyCId = app.m_entities->SpawnUnitByArchetype(0, 43, 2, Faction::Resistance);
    Unit *allyC = app.m_entities->GetEntity(allyCId)->AsUnit();
    allyC->state = ActionState::Idle;
    allyC->targetEntityId = 0;

    // Update 1 tick (0.05s)
    app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);

    // Ally B must be directly engaged
    if (!allyB->isDirectEngaged)
    {
        std::cerr << "[Test] Error: Frontline ally B should be isDirectEngaged=true!" << std::endl;
        return false;
    }

    // Ally A (depth = 1) must have answered the call for help: target set to suppEnemyId and moving
    if (allyA->targetEntityId != suppEnemyId || allyA->state != ActionState::Move)
    {
        std::cerr << "[Test] Error: Ally A (depth = 1) did not join in support of ally B! target="
                  << allyA->targetEntityId << " state=" << static_cast<int>(allyA->state) << std::endl;
        return false;
    }

    // Ally C (depth > 1) MUST REMAIN IDLE! A is only moving in support, not directly engaged yet!
    if (allyC->targetEntityId != 0 || allyC->state != ActionState::Idle)
    {
        std::cerr << "[Test] Error: Ally C violated depth=1 constraint! It triggered prematurely while A was merely "
                     "moving to support!"
                  << std::endl;
        return false;
    }

    std::cout << "  - Tactical support assist validated: Ally A joined frontline Ally B." << std::endl;
    std::cout << "  - Strict depth = 1 constraint validated: Ally C remained Idle while Ally A moved to support."
              << std::endl;

    // 20. Validating Command Override During Movement, Classic Maps (Brod, Canyon, Oasis) & Flame Stream
    std::cout << "[Test] Validating Command Override During Movement & Classic Maps..." << std::endl;

    // A. Command Override During Movement: unit moving to (25, 25), receives attack order on enemy at (12, 10)
    int movingRiflemanId = app.m_entities->SpawnUnitByArchetype(0, 10, 10, Faction::Resistance);
    Unit *moveU = app.m_entities->GetUnit(movingRiflemanId);
    app.m_entities->IssueMoveOrder({movingRiflemanId}, 25, 25, *app.m_pathfinder, false);
    if (moveU->path.empty() || moveU->state != ActionState::Move)
    {
        std::cerr << "[Test] Error: Unit failed to start move order!" << std::endl;
        return false;
    }
    // Now interrupt with direct attack order on enemy unit
    int enemyTargetId = app.m_entities->SpawnUnitByArchetype(1, 12, 10, Faction::Confederation);
    app.m_entities->IssueAttackOrder({movingRiflemanId}, enemyTargetId, app.m_pathfinder.get());
    if (moveU->targetEntityId != enemyTargetId)
    {
        std::cerr << "[Test] Error: TargetEntityId was not updated on command override!" << std::endl;
        return false;
    }
    // Target is within attack range (dist = 2 tiles <= attackRange 3..4): old path must be cleared and state must be
    // Attack!
    if (!moveU->path.empty() || moveU->state != ActionState::Attack)
    {
        std::cerr << "[Test] Error: Unit old move path was not cleared upon attack order!" << std::endl;
        return false;
    }
    // Advance simulation tick: unit stays on target without jerking into old path
    app.m_entities->Update(0.1f, *app.m_map, *app.m_pathfinder);
    if (moveU->targetEntityId != enemyTargetId || moveU->state == ActionState::Move)
    {
        std::cerr << "[Test] Error: Unit jerked back into ActionState::Move after attack order!" << std::endl;
        return false;
    }
    std::cout << "  - Command override during movement validated: old path cleared, new attack target engaged smoothly."
              << std::endl;

    // B. Menu content: the campaign lists only its missions, a custom game can use any map of assets/maps
    if (MissionController::kCampaignMissionCount != 7
        || MissionController::GetAvailableMissions().size()
               < static_cast<size_t>(MissionController::kCampaignMissionCount))
    {
        std::cerr << "[Test] Error: The campaign must consist of its 7 missions!" << std::endl;
        return false;
    }
    const auto customMaps = MissionController::ListSkirmishMaps();
    if (customMaps.size() < 10)
    {
        std::cerr << "[Test] Error: Custom game lists only " << customMaps.size() << " maps!" << std::endl;
        return false;
    }
    for (const auto &info : customMaps)
    {
        if (!app.m_mission->LoadSkirmish(info.path, *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
        {
            std::cerr << "[Test] Error: Custom game cannot start on " << info.path << "!" << std::endl;
            return false;
        }
        int resistanceBuildings = 0, confederationBuildings = 0;
        for (const auto &e : app.m_entities->GetAllEntities())
        {
            if (!e.alive || !e.isBuilding)
                continue;
            (e.faction == Faction::Resistance ? resistanceBuildings : confederationBuildings)++;
        }
        if (resistanceBuildings == 0 || confederationBuildings == 0 || !app.m_mission->IsSkirmish()
            || (app.m_mission->GetPlayerHqX() == app.m_mission->GetEnemyHqX()
                && app.m_mission->GetPlayerHqY() == app.m_mission->GetEnemyHqY()))
        {
            std::cerr << "[Test] Error: Custom game on " << info.path << " has no two bases!" << std::endl;
            return false;
        }
    }
    const int serialBeforeRestart = app.m_mission->GetLoadSerial();
    if (!app.m_mission->Restart("assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder)
        || app.m_mission->GetLoadSerial() == serialBeforeRestart || !app.m_mission->IsSkirmish())
    {
        std::cerr << "[Test] Error: Restarting a custom game failed!" << std::endl;
        return false;
    }
    // Back to the blank arena the following tests expect
    app.m_entities->Clear();
    app.m_map->InitBlankArena(60, 40, 0);
    app.m_pathfinder->SetMap(app.m_map.get());
    std::cout << "  - Campaign = 7 missions; custom game starts on all " << customMaps.size()
              << " maps of assets/maps and restarts OK." << std::endl;

    // 21. Validating Choke-Point Blocking Enemy Combat & Unit Anti-Stacking Mechanics
    std::cout << "[Test] Validating Choke-Point Blocking Enemy Engagement & Anti-Stacking..." << std::endl;

    // Setup a 1-tile corridor: walls at y=19 and y=21 for x in [45..55], passage at y=20
    for (int wx = 45; wx <= 55; ++wx)
    {
        app.m_map->SetWall(wx, 19, Faction::Neutral, 500);
        app.m_map->SetWall(wx, 21, Faction::Neutral, 500);
    }
    // Place an enemy unit blocking the only corridor at (50, 20)
    int chokeEnemyId = app.m_entities->SpawnUnitByArchetype(1, 50, 20, Faction::Confederation);
    Unit *chokeEnemy = app.m_entities->GetUnit(chokeEnemyId);
    chokeEnemy->currentHp = 8; // Vulnerable enemy blocker (a rifleman has 30 HP, the same as the runner)

    // Spawn friendly unit at (46, 20) and issue Move order to (54, 20) beyond the enemy blocker
    int runnerId = app.m_entities->SpawnUnitByArchetype(0, 46, 20, Faction::Resistance);
    Unit *runner = app.m_entities->GetUnit(runnerId);
    app.m_entities->IssueMoveOrder({runnerId}, 54, 20, *app.m_pathfinder, false);

    // Friendly unit moves towards (50, 20), encounters the blocking enemy
    // Advance simulation: unit should encounter the enemy, engage in combat, destroy it, and continue to (54, 20)
    bool engagedEnemy = false;
    for (int t = 0; t < 150; ++t)
    {
        app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
        if (runner->targetEntityId == chokeEnemyId || runner->state == ActionState::Attack)
        {
            engagedEnemy = true;
        }
        if (!chokeEnemy->alive)
        {
            break;
        }
    }
    if (!engagedEnemy)
    {
        std::cerr << "[Test] Error: Unit failed to auto-engage enemy blocking the choke point!" << std::endl;
        return false;
    }
    std::cout
        << "  - Choke-point blocker engagement validated: unit automatically attacked the enemy blocking the corridor."
        << std::endl;

    // Advance simulation: unit should resume moving through the now-cleared corridor
    for (int t = 0; t < 150; ++t)
    {
        app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
        if (runner->tileX >= 52)
            break;
    }
    if (runner->tileX <= 50)
    {
        std::cerr << "[Test] Error: Unit failed to advance through corridor after blocker was destroyed! tileX="
                  << runner->tileX << std::endl;
        return false;
    }
    std::cout << "  - Passage traversal validated: unit advanced past cleared choke point to destination (tileX="
              << runner->tileX << ")." << std::endl;

    // Validate Unit Anti-Stacking: force two units to same tile and verify automatic separation
    int stackU1Id = app.m_entities->SpawnUnitByArchetype(0, 15, 15, Faction::Resistance);
    int stackU2Id = app.m_entities->SpawnUnitByArchetype(0, 15, 15, Faction::Resistance);
    Unit *su1 = app.m_entities->GetUnit(stackU1Id);
    Unit *su2 = app.m_entities->GetUnit(stackU2Id);
    // Artificially stack su2 on su1
    su2->tileX = su1->tileX;
    su2->tileY = su1->tileY;
    app.m_entities->ResolveUnitStacking(*app.m_map);
    if (su1->tileX == su2->tileX && su1->tileY == su2->tileY)
    {
        std::cerr << "[Test] Error: resolveUnitStacking failed to separate overlapping units!" << std::endl;
        return false;
    }
    std::cout << "  - Anti-stacking validated: overlapping units automatically separated to distinct tiles ("
              << su1->tileX << "," << su1->tileY << ") and (" << su2->tileX << "," << su2->tileY << ")." << std::endl;

    // 22. Validating Unit Top-Corner Wandering Prevention, Siege Mode Idle Out-of-Range, Building Construction
    // Vulnerability & Visible Fog / Multi-HQ Radius (16 tiles)
    std::cout << "[Test] Validating Top-Corner Wandering Fix, Siege Out-of-Range, Building HP Damage & Visible Fog / "
                 "HQ Radius..."
              << std::endl;

    // A. Top-Corner Wandering Prevention
    int idleUId = app.m_entities->SpawnUnitByArchetype(0, 20, 20, Faction::Resistance);
    Unit *idleU = app.m_entities->GetUnit(idleUId);
    if (idleU->targetTileX != -1 || idleU->targetTileY != -1)
    {
        std::cerr << "[Test] Error: Spawned unit should have targetTileX/Y == -1, got (" << idleU->targetTileX << ","
                  << idleU->targetTileY << ")" << std::endl;
        return false;
    }
    app.m_entities->Update(1.0f, *app.m_map, *app.m_pathfinder);
    if (idleU->tileX != 20 || idleU->tileY != 20 || idleU->state == ActionState::Move)
    {
        std::cerr << "[Test] Error: Idle unit moved autonomously without orders! (" << idleU->tileX << ","
                  << idleU->tileY << ")" << std::endl;
        return false;
    }
    std::cout << "  - Top-corner wandering prevention validated: unit remained at (20,20) with targetTileX/Y == -1."
              << std::endl;

    // B. Siege Mode Unit Out-of-Range Attack Check
    int siegeSniperId
        = app.m_entities->SpawnUnitByArchetype(5, 20, 20, Faction::Resistance); // Sniper (archetype 5 is siege sniper)
    Unit *siegeSniper = app.m_entities->GetUnit(siegeSniperId);
    siegeSniper->isSiegeMode = true;
    int farEnemyId
        = app.m_entities->SpawnUnitByArchetype(1, 40, 40, Faction::Confederation); // 28 tiles away, out of range
    app.m_entities->IssueAttackOrder({siegeSniperId}, farEnemyId, app.m_pathfinder.get());
    if (siegeSniper->state == ActionState::Attack)
    {
        std::cerr << "[Test] Error: Siege unit entered ActionState::Attack for out-of-range target!" << std::endl;
        return false;
    }
    app.m_entities->Update(0.2f, *app.m_map, *app.m_pathfinder);
    if (siegeSniper->state == ActionState::Attack)
    {
        std::cerr << "[Test] Error: Siege unit vibrating / animating in ActionState::Attack for out-of-range target!"
                  << std::endl;
        return false;
    }
    std::cout << "  - Siege out-of-range attack check validated: unit stays in Idle and does not vibrate/animate."
              << std::endl;

    // C. Building Construction Vulnerability (takes HP damage and can be destroyed)
    int constrBldgId
        = app.m_entities->SpawnBuildingByIndex(1, 22, 22, Faction::Resistance, 0.0f); // PowerPlant under construction
    Building *constrBldg = app.m_entities->GetBuilding(constrBldgId);
    if (!constrBldg || constrBldg->buildProgress >= 1.0f)
    {
        std::cerr << "[Test] Error: Building under construction failed to spawn with progress 0!" << std::endl;
        return false;
    }
    int initialHp = constrBldg->currentHp;
    // Deal 30 damage to building under construction
    constrBldg->currentHp -= 30;
    int damagedHp = constrBldg->currentHp;
    constrBldg->Update(0.1f, *app.m_map, {}); // Run updateConstruction
    if (constrBldg->currentHp >= initialHp)
    {
        std::cerr << "[Test] Error: Building under construction is invulnerable! HP was reset from " << damagedHp
                  << " to " << constrBldg->currentHp << std::endl;
        return false;
    }
    std::cout << "  - Building construction vulnerability validated: takes real HP damage and does not reset."
              << std::endl;

    // D. Fog of War placement check & Multi-HQ 16-tile build radius
    app.m_entities->Clear();
    app.m_map->InitBlankArena(60, 60, 0);
    app.m_map->SetFogEnabled(true);
    app.m_map->ResetAllFog(FogState::Hidden);
    app.m_entities->SetBuildRadius(16);                                                     // Default 16 tiles
    int hq1Id = app.m_entities->SpawnBuildingByIndex(8, 10, 10, Faction::Resistance, 1.0f); // HQ 1 at (10,10)
    (void) hq1Id;
    app.m_map->RevealArea(10, 10, 20); // Reveal around HQ 1
    // Unrevealed tile at (45, 45) should be forbidden
    bool canBuildInFog
        = app.m_entities->CanPlaceBuilding(45, 45, 2, 2, *app.m_map, Players::OwnerOf(Faction::Resistance));
    if (canBuildInFog)
    {
        std::cerr << "[Test] Error: canPlaceBuilding allowed construction in Fog!" << std::endl;
        return false;
    }
    // Distance 8 from HQ 1: allowed
    bool canBuildNearHq1
        = app.m_entities->CanPlaceBuilding(18, 10, 2, 2, *app.m_map, Players::OwnerOf(Faction::Resistance));
    // Distance 22 from HQ 1: outside 16-tile radius
    app.m_map->RevealArea(32, 10, 10);
    bool canBuildFarHq1
        = app.m_entities->CanPlaceBuilding(32, 10, 2, 2, *app.m_map, Players::OwnerOf(Faction::Resistance));
    if (!canBuildNearHq1 || canBuildFarHq1)
    {
        std::cerr << "[Test] Error: HQ 1 16-tile build radius failed! near=" << canBuildNearHq1
                  << " far=" << canBuildFarHq1 << std::endl;
        return false;
    }
    // Now build a second HQ at (24, 10) within range of HQ 1
    app.m_map->RevealArea(24, 10, 20);
    int hq2Id = app.m_entities->SpawnBuildingByIndex(8, 24, 10, Faction::Resistance, 1.0f);
    (void) hq2Id;
    // Now (32, 10) is distance 8 from HQ 2: should NOW BE ALLOWED thanks to second HQ!
    bool canBuildNearHq2
        = app.m_entities->CanPlaceBuilding(32, 10, 2, 2, *app.m_map, Players::OwnerOf(Faction::Resistance));
    if (!canBuildNearHq2)
    {
        std::cerr << "[Test] Error: Second HQ failed to expand build radius to its vicinity!" << std::endl;
        return false;
    }
    std::cout << "  - Fog-only placement & Multi-HQ 16-tile radius expansion validated successfully." << std::endl;

    // 23. Validating Unit Remains in YAML & Cohesive AI March Movement
    std::cout << "[Test] Validating Unit Remains in YAML & Cohesive Movement..." << std::endl;

    // A. Verify YAML loading of remains:
    const auto *riflemanCfg
        = app.m_entities->GetUnitConfigByUnitType(UnitType::ResistanceRifleman, Faction::Resistance);
    if (!riflemanCfg || riflemanCfg->deathSequences.size() < 2 || riflemanCfg->flameDeathSequences.size() < 2)
    {
        std::cerr << "[Test] Error: Rifleman YAML did not load death sequences!" << std::endl;
        return false;
    }
    const auto *t23HammerCfg = app.m_entities->GetUnitConfigByUnitType(UnitType::Hammer, Faction::Confederation);
    if (!t23HammerCfg || t23HammerCfg->wreckComposite != 1764 || t23HammerCfg->wreckStages != 5)
    {
        std::cerr << "[Test] Error: Hammer YAML did not load wreck composite 1764!" << std::endl;
        return false;
    }
    const auto *coyoteCfg = app.m_entities->GetUnitConfigByUnitType(UnitType::Coyote, Faction::Resistance);
    if (!coyoteCfg || coyoteCfg->wreckComposite != 1759)
    {
        std::cerr << "[Test] Error: Coyote YAML did not load wreck composite 1759!" << std::endl;
        return false;
    }
    const auto *t23ZeusCfg = app.m_entities->GetUnitConfigByUnitType(UnitType::Zeus, Faction::Confederation);
    if (!t23ZeusCfg || t23ZeusCfg->wreckComposite != 1769 || t23ZeusCfg->wreckStages != 4)
    {
        std::cerr << "[Test] Error: Zeus YAML did not load wreck composite 1769!" << std::endl;
        return false;
    }
    std::cout << "  - YAML Remains configurations validated for infantry (death [247, 248], flame [259, 260]) and "
                 "vehicles (1759, 1764, 1769)."
              << std::endl;

    // B. Infantry death variant selection: bullet vs flame
    int inf1Id = app.m_entities->SpawnUnitByArchetype(0, 15, 15, Faction::Resistance);
    Unit *inf1 = app.m_entities->GetUnit(inf1Id);
    inf1->currentHp = 10;
    // Shoot inf1 with bullet
    Entity bulletShooter(false);
    bulletShooter.faction = Faction::Confederation;
    bulletShooter.attackDamage = 50;
    bulletShooter.projectile = ProjectileType::Bullet;
    app.m_entities->SpawnProjectileFromEntity(
        bulletShooter, 15 * 30.0f, 15 * 20.0f, 15 * 30.0f, 15 * 20.0f, inf1Id, *app.m_map);
    app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    if (inf1->state != ActionState::Die || (inf1->selectedDeathSeq != 247 && inf1->selectedDeathSeq != 248))
    {
        std::cerr << "[Test] Error: Normal infantry death did not pick seq 247 or 248! got: " << inf1->selectedDeathSeq
                  << std::endl;
        return false;
    }
    if (inf1->killedByFlame)
    {
        std::cerr << "[Test] Error: Normal death marked killedByFlame=true!" << std::endl;
        return false;
    }

    // Flame death
    int inf2Id = app.m_entities->SpawnUnitByArchetype(0, 16, 15, Faction::Resistance);
    Unit *inf2 = app.m_entities->GetUnit(inf2Id);
    inf2->currentHp = 10;
    Entity t23FlameShooter(false);
    t23FlameShooter.faction = Faction::Confederation;
    t23FlameShooter.attackDamage = 50;
    t23FlameShooter.projectile = ProjectileType::Flame;
    app.m_entities->SpawnProjectileFromEntity(
        t23FlameShooter, 16 * 30.0f, 16 * 20.0f, 16 * 30.0f, 15 * 20.0f, inf2Id, *app.m_map);
    app.m_entities->Update(0.10f, *app.m_map, *app.m_pathfinder);
    if (inf2->state != ActionState::Die || !inf2->killedByFlame
        || (inf2->selectedDeathSeq != 259 && inf2->selectedDeathSeq != 260))
    {
        std::cerr << "[Test] Error: Flame death did not set killedByFlame or seq 259/260! got: "
                  << inf2->selectedDeathSeq << std::endl;
        return false;
    }
    std::cout << "  - Infantry corpse selection validated: bullet chooses 247/248, flame chooses 259/260." << std::endl;

    // C. Cohesive March Column Movement: units in a column don't scatter
    app.m_entities->Clear();
    app.m_map->InitBlankArena(60, 60, 0);
    // Spawn 4 units in a line: (10, 20), (9, 20), (8, 20), (7, 20)
    std::vector<int> colUnits;
    for (int i = 0; i < 4; ++i)
    {
        int uid = app.m_entities->SpawnUnitByArchetype(0, 10 - i, 20, Faction::Confederation);
        colUnits.push_back(uid);
    }
    // Issue attack move towards (30, 20)
    app.m_entities->IssueAttackMoveOrder(colUnits, 30, 20, *app.m_pathfinder);
    // Simulate march for 20 ticks
    for (int t = 0; t < 20; ++t)
    {
        app.m_entities->Update(0.05f, *app.m_map, *app.m_pathfinder);
    }
    // Verify all units are advancing along y=20 (±1 tile for slots) and moving eastward (tileX > 7)
    for (int uid : colUnits)
    {
        Unit *u = app.m_entities->GetUnit(uid);
        if (std::abs(u->tileY - 20) > 2 || u->tileX < 7)
        {
            std::cerr << "[Test] Error: Unit scattered away from column! tile=(" << u->tileX << "," << u->tileY << ")"
                      << std::endl;
            return false;
        }
    }
    std::cout << "  - Cohesive march column validated: units move eastward together without scattering across the map."
              << std::endl;

    // =========================================================================
    // Test #24: Validating Argon 4 Architectural Guarantees:
    //  - Strict Coordinate Invariants (No (0,0) ghost march)
    //  - Siege Mode Out-Of-Range Anti-Vibration
    //  - Fortress Orthogonal Rail Navigation & Decompositions
    //  - Strict Depth-1 Support Assist Graph
    // =========================================================================
    std::cout << "[Test] Validating Argon 4 Solutions (Invariant Coordinates, Siege Anti-Vibration, Depth-1 Assist, "
                 "Fortress Rails)..."
              << std::endl;
    app.m_entities->Clear();
    app.m_map->InitBlankArena(60, 60, 0);

    // 1. Strict Target Coordinate Invariant:
    int invUnitId = app.m_entities->SpawnUnitByArchetype(0, 25, 25, Faction::Resistance);
    Unit *invUnit = app.m_entities->GetUnit(invUnitId);
    invUnit->ClearMoveTarget();
    invUnit->waitingForPath = true;
    invUnit->state = ActionState::Idle;
    invUnit->pathRetryTimer = 0.5f; // trigger retry
    app.m_entities->Update(0.1f, *app.m_map, *app.m_pathfinder);
    if (invUnit->state == ActionState::Move || invUnit->targetTileX != -1 || invUnit->tileX != 25
        || invUnit->tileY != 25)
    {
        std::cerr << "[Test] Error: Unit with clearMoveTarget() moved to (0,0)! state="
                  << static_cast<int>(invUnit->state) << " targetX=" << invUnit->targetTileX << " tile=("
                  << invUnit->tileX << "," << invUnit->tileY << ")" << std::endl;
        return false;
    }
    std::cout << "  - Target Coordinate Invariant validated: units with no ground target never march to (0,0)."
              << std::endl;

    // 2. Siege Mode Out-Of-Range Anti-Vibration:
    int siegeId = app.m_entities->SpawnUnitByArchetype(15, 10, 10, Faction::Resistance); // Rhino Deployed (Siege)
    Unit *siegeU = app.m_entities->GetUnit(siegeId);
    siegeU->isSiegeMode = true;
    siegeU->moveSpeed = 0.0f;
    siegeU->attackRange = 8.0f;

    int t24FarEnemyId = app.m_entities->SpawnUnitByArchetype(0, 30, 30, Faction::Confederation);
    app.m_entities->IssueAttackOrder({siegeId}, t24FarEnemyId, app.m_pathfinder.get());
    if (siegeU->state != ActionState::Idle || siegeU->targetEntityId != 0 || siegeU->animFrame != 0)
    {
        std::cerr << "[Test] Error: Siege unit out-of-range order did not reject cleanly! state="
                  << static_cast<int>(siegeU->state) << " targetId=" << siegeU->targetEntityId << std::endl;
        return false;
    }
    for (int t = 0; t < 10; ++t)
    {
        app.m_entities->Update(0.1f, *app.m_map, *app.m_pathfinder);
        if (siegeU->state == ActionState::Attack || siegeU->animFrame != 0)
        {
            std::cerr << "[Test] Error: Siege unit vibrated/animated while out of range! state="
                      << static_cast<int>(siegeU->state) << " animFrame=" << siegeU->animFrame << std::endl;
            return false;
        }
    }
    std::cout << "  - Siege Mode Out-Of-Range Anti-Vibration validated: stays perfectly Idle with 0 vibration."
              << std::endl;

    // 3. 4-way pathfinding mode (allowDiagonal = false) still produces strictly orthogonal routes:
    std::vector<PathNode> t24FortPath;
    bool t24FortPartial = false;
    if (!app.m_pathfinder->FindPath(10, 10, 20, 20, t24FortPath, 0, &t24FortPartial, false))
    {
        std::cerr << "[Test] Error: Could not compute orthogonal path for Fortress!" << std::endl;
        return false;
    }
    int curX = 10, curY = 10;
    for (const auto &node : t24FortPath)
    {
        int dx = std::abs(node.x - curX);
        int dy = std::abs(node.y - curY);
        if (dx > 0 && dy > 0)
        {
            std::cerr << "[Test] Error: 4-way path contains diagonal step (" << curX << "," << curY << ") -> ("
                      << node.x << "," << node.y << ")!" << std::endl;
            return false;
        }
        curX = node.x;
        curY = node.y;
    }
    std::cout << "  - 4-way pathfinding mode validated: strictly 0 diagonal steps in " << t24FortPath.size()
              << " waypoints." << std::endl;

    // 4. Strict Depth-1 Support Assist Graph:
    int t24EnemyXId = app.m_entities->SpawnUnitByArchetype(0, 10, 15, Faction::Confederation);
    int t24AllyAId = app.m_entities->SpawnUnitByArchetype(0, 13, 15, Faction::Resistance);
    int t24AllyBId = app.m_entities->SpawnUnitByArchetype(0, 16, 15, Faction::Resistance);
    int t24AllyCId = app.m_entities->SpawnUnitByArchetype(0, 19, 15, Faction::Resistance);

    Unit *t24AllyA = app.m_entities->GetUnit(t24AllyAId);
    Unit *t24AllyB = app.m_entities->GetUnit(t24AllyBId);
    Unit *t24AllyC = app.m_entities->GetUnit(t24AllyCId);

    t24AllyA->attackRange = 3.5f;
    t24AllyB->attackRange = 3.5f;
    t24AllyC->attackRange = 3.5f;

    // Ally A is frontline at (13, 15), directly engaged with Enemy X at (10, 15) (dist = 3 <= 3.5)
    t24AllyA->targetEntityId = t24EnemyXId;
    t24AllyA->isDirectEngaged = true;
    t24AllyA->isSupportAssisting = false;

    // Ally B at (16, 15): Enemy X is 6 tiles away (out of range), but Ally A is 3 tiles away (in range).
    // Ally C at (19, 15): Ally B is 3 tiles away, but Ally A is 6 tiles away (out of range).
    app.m_entities->Update(0.1f, *app.m_map, *app.m_pathfinder);

    if (t24AllyB->targetEntityId != t24EnemyXId || !t24AllyB->isSupportAssisting)
    {
        std::cerr << "[Test] Error: Ally B did not assist frontline Ally A! target=" << t24AllyB->targetEntityId
                  << " isSupportAssisting=" << t24AllyB->isSupportAssisting << std::endl;
        return false;
    }
    if (t24AllyC->targetEntityId != 0)
    {
        std::cerr << "[Test] Error: Ally C assisted Ally B, violating strict depth = 1! target="
                  << t24AllyC->targetEntityId << std::endl;
        return false;
    }
    std::cout << "  - Strict Depth-1 Support Assist Graph validated: Ally B assists A, but Ally C does not chain off B."
              << std::endl;

    // 25. Test Comprehensive Architectural Fixes (AI Auto-Init, Scaled 10/14 A*, Local Sidestep, Wall Faction)
    std::cout
        << "[Test] Validating Comprehensive Architectural Solutions (Auto-Init, Scaled A*, Sidestep, Wall Factions)..."
        << std::endl;

    // 1) Automatic AI Initialization on Map Load (via setOnLoaded hook)
    if (!app.m_mission->LoadMission(7, "assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
    {
        std::cerr << "[Test] Error: Failed to load Mission 7 (Skirmish map)!" << std::endl;
        return false;
    }
    if (!app.m_aiController->IsInitialized())
    {
        std::cerr << "[Test] Error: AIController was not initialized automatically on mission load!" << std::endl;
        return false;
    }
    if (app.m_mission->GetEnemyHqX() <= 0 || app.m_mission->GetEnemyHqY() <= 0)
    {
        std::cerr << "[Test] Error: Mission enemy HQ position is invalid (0, 0)!" << std::endl;
        return false;
    }
    std::cout << "  - Automatic AI Initialization & Base Coordinates validated: Skirmish/TestMap auto-hooks "
                 "AIController::init."
              << std::endl;

    // 2) Scaled 10/14 Octile A* in brain_map: 5-step diagonal path should have exactly 5 diagonal steps (no staircases)
    app.m_map->InitBlankArena(40, 40, 0);
    app.m_pathfinder->SetMap(app.m_map.get());
    std::vector<PathNode> diagPath;
    bool diagPartial = false;
    if (!app.m_pathfinder->FindPath(10, 10, 15, 15, diagPath, 0, &diagPartial, true) || diagPath.empty())
    {
        std::cerr << "[Test] Error: Could not find diagonal path in open arena!" << std::endl;
        return false;
    }
    if (diagPath.size() != 5)
    {
        std::cerr << "[Test] Error: 8-way diagonal path from (10,10) to (15,15) expected 5 steps, got "
                  << diagPath.size() << std::endl;
        return false;
    }
    for (size_t i = 0; i < diagPath.size(); ++i)
    {
        int expectedX = 11 + static_cast<int>(i);
        int expectedY = 11 + static_cast<int>(i);
        if (diagPath[i].x != expectedX || diagPath[i].y != expectedY)
        {
            std::cerr << "[Test] Error: Diagonal path deviated into staircase at step " << i << ": (" << diagPath[i].x
                      << "," << diagPath[i].y << ") vs (" << expectedX << "," << expectedY << ")" << std::endl;
            return false;
        }
    }
    std::cout
        << "  - Scaled 10/14 Octile A* Navigation validated: smooth 5-step diagonal trajectory without staircases."
        << std::endl;

    // 3) Wall Faction Breaching Rules:
    // AI unit should breach Resistance walls, but NEVER breach friendly Confederation walls!
    app.m_entities->Clear();
    app.m_map->InitBlankArena(40, 40, 0);
    app.m_pathfinder->SetMap(app.m_map.get());

    // Place a Confederation (friendly) wall at (12, 10)
    app.m_map->SetWall(12, 10, Faction::Confederation, 100);
    int aiUnitId = app.m_entities->SpawnUnitByArchetype(1, 10, 10, Faction::Confederation);
    Unit *aiUnit = app.m_entities->GetUnit(aiUnitId);
    aiUnit->isAttackMoving = true;
    aiUnit->path = {{11, 10}, {12, 10}, {13, 10}};
    aiUnit->pathIndex = 1; // Attempt to enter (12, 10)

    app.m_entities->Update(0.1f, *app.m_map, *app.m_pathfinder);
    if (aiUnit->targetWallTileX == 12 && aiUnit->targetWallTileY == 10)
    {
        std::cerr << "[Test] Error: AI attacked its own friendly Confederation wall!" << std::endl;
        return false;
    }

    // Now a hostile Resistance wall seals column X=12 completely: no way around, so it must be breached
    // (a lone wall tile with an open route around it is walked around instead - see the wall detour tests)
    for (int y = 0; y < app.m_map->GetHeight(); ++y)
        app.m_map->SetWall(12, y, Faction::Resistance, 100);
    aiUnit->tileX = 11;
    aiUnit->tileY = 10;
    aiUnit->subX = 0.0f;
    aiUnit->subY = 0.0f;
    aiUnit->state = ActionState::Move;
    aiUnit->isAttackMoving = true;
    aiUnit->path = {{12, 10}, {13, 10}};
    aiUnit->pathIndex = 0;
    aiUnit->targetWallTileX = -1;
    aiUnit->targetWallTileY = -1;

    app.m_entities->Update(0.1f, *app.m_map, *app.m_pathfinder);
    if (aiUnit->targetWallTileX != 12 || aiUnit->targetWallTileY != 10)
    {
        std::cerr << "[Test] Error: AI failed to target hostile Resistance wall for breach! targetWall=("
                  << aiUnit->targetWallTileX << "," << aiUnit->targetWallTileY << ")" << std::endl;
        return false;
    }
    std::cout << "  - Wall Faction Breach Rules validated: hostile walls breached, friendly walls preserved."
              << std::endl;

    // 4) Instant Local Sidestepping & Non-Crossing Group Movement:
    app.m_entities->Clear();
    app.m_map->InitBlankArena(40, 40, 0);
    app.m_pathfinder->SetMap(app.m_map.get());

    int leadId = app.m_entities->SpawnUnitByArchetype(0, 15, 10, Faction::Resistance);
    int followerId = app.m_entities->SpawnUnitByArchetype(0, 14, 10, Faction::Resistance);
    Unit *leadU = app.m_entities->GetUnit(leadId);
    Unit *followU = app.m_entities->GetUnit(followerId);

    app.m_pathfinder->SetEntityManager(app.m_entities.get());
    leadU->state = ActionState::Move;
    std::vector<PathNode> independentPath;
    if (!app.m_pathfinder->FindPath(14, 10, 16, 10, independentPath, followerId, nullptr, false)
        || independentPath.empty() || !(independentPath.front() == PathNode {15, 10}))
    {
        std::cerr << "[Test] Error: Moving ally still blocks an independent route!" << std::endl;
        return false;
    }

    leadU->state = ActionState::Idle; // Parked ally at (15, 10)

    // Follower wants to step from (14, 10) through (15, 10) to (16, 10)
    followU->state = ActionState::Move;
    followU->path = {{15, 10}, {16, 10}};
    followU->pathIndex = 0;
    followU->targetTileX = 16;
    followU->targetTileY = 10;

    app.m_entities->Update(0.016f, *app.m_map, *app.m_pathfinder);
    // Follower must sidestep into (14, 9) or (14, 11) or (15, 9) or (15, 11) in 1 frame without freezing
    if (followU->path.empty()
        || (followU->path[followU->pathIndex].x == 15 && followU->path[followU->pathIndex].y == 10))
    {
        std::cerr << "[Test] Error: Follower did not sidestep parked ally! next waypoint=("
                  << followU->path[followU->pathIndex].x << "," << followU->path[followU->pathIndex].y << ")"
                  << std::endl;
        return false;
    }
    if (followU->waitBlockedTimer > 0.05f)
    {
        std::cerr << "[Test] Error: Follower froze in waitBlockedTimer instead of local sidestep! timer="
                  << followU->waitBlockedTimer << std::endl;
        return false;
    }
    std::cout
        << "  - Instant Local Sidestepping validated: moving unit detoured around parked ally on frame 1 without stall."
        << std::endl;

    // 25b. Construction next to the enemy, demolition, range technologies across siege forms
    std::cout << "[Test] Validating construction rules, demolition and siege research..." << std::endl;
    {
        app.m_entities->Clear();
        app.m_map->InitBlankArena(40, 40, 0);
        app.m_pathfinder->SetMap(app.m_map.get());
        const Faction me = PlayerFaction();
        const Faction foe = AiFaction();
        app.m_entities->SpawnBuildingByIndex(me == Faction::Resistance ? 8 : 0, 5, 5, me, 1.0f);
        if (!app.m_entities->CanPlaceBuilding(12, 5, 2, 2, *app.m_map, Players::OwnerOf(me)))
        {
            std::cerr << "[Test] Error: free spot next to the HQ must be buildable!" << std::endl;
            return false;
        }
        const int block = app.m_map->GetConstructionRules().enemyBlockTiles;
        const int enemyId
            = app.m_entities->SpawnUnitByArchetype(1, 13 + block, 5, foe); // `block` tiles from the footprint
        if (app.m_entities->CanPlaceBuilding(12, 5, 2, 2, *app.m_map, Players::OwnerOf(me)))
        {
            std::cerr << "[Test] Error: building " << block << " tiles from an enemy must be refused!" << std::endl;
            return false;
        }
        app.m_entities->GetUnit(enemyId)->tileX = 14 + block;
        if (!app.m_entities->CanPlaceBuilding(12, 5, 2, 2, *app.m_map, Players::OwnerOf(me)))
        {
            std::cerr << "[Test] Error: an enemy " << block + 1 << " tiles away must not block construction!"
                      << std::endl;
            return false;
        }

        const int barracksId
            = app.m_entities->SpawnBuildingByIndex(me == Faction::Resistance ? 9 : 1, 20, 20, me, 1.0f);
        Building *barracks = app.m_entities->GetBuilding(barracksId);
        barracks->productionQueue.push_back({0, 120, 5.0f, 1.0f});
        const int refund = app.m_entities->DemolishBuilding(barracksId, *app.m_map);
        if (refund != 120 || barracks->state != ActionState::Die || !app.m_map->IsPassable(21, 21))
        {
            std::cerr
                << "[Test] Error: demolition must destroy the building, free its tiles and refund its queue! refund="
                << refund << std::endl;
            return false;
        }
        std::cout << "  - Construction blocked within " << block << " tiles of an enemy; demolition refunds " << refund
                  << std::endl;

        // Snipers + Hornet-10: 9.5 in normal form, the concealed form keeps its +2 (11.5) after the change
        ResetResearch();
        CompleteResearch(Players::OwnerOf(Faction::Resistance), 5, *app.m_entities);
        CompleteResearch(Players::OwnerOf(Faction::Resistance), 6, *app.m_entities);
        const int sniperId = app.m_entities->SpawnUnitByArchetype(4, 30, 30, Faction::Resistance);
        Unit *sniper = app.m_entities->GetUnit(sniperId);
        const float siegeRange = ResearchedAttackRange(
            Players::OwnerOf(Faction::Resistance), 14, app.m_entities->GetUnitConfig(14)->attackRange);
        if (siegeRange <= sniper->attackRange)
        {
            std::cerr << "[Test] Error: researched siege range " << siegeRange << " must stay above the normal "
                      << sniper->attackRange << std::endl;
            return false;
        }
        app.m_entities->ToggleSiegeMode({sniperId});
        for (int i = 0; i < 20 && sniper->archetypeId != 14; ++i)
            app.m_entities->Update(0.1f, *app.m_map, *app.m_pathfinder);
        if (sniper->archetypeId != 14 || std::abs(sniper->attackRange - siegeRange) > 0.01f)
        {
            std::cerr << "[Test] Error: deployed sniper lost its technologies! range=" << sniper->attackRange
                      << " expected " << siegeRange << std::endl;
            return false;
        }
        ResetResearch();
        std::cout << "  - Sniper range " << siegeRange << " kept in siege form after research" << std::endl;
    }

    // 26. Group movement quality (see runGroupMovementTest)
    if (!RunGroupMovementTest(app))
        return false;

    // 27. AI waves, squads, chokepoints and research (see runAIBehaviourTest)
    if (!RunAiBehaviourTest(app))
        return false;

    // 26x. Shift order queue: the queued points are walked in order, the order line ends at the clicked point, a plain
    // order clears the queue
    {
        std::cout << "[Test] Validating the Shift order queue..." << std::endl;
        EntityManager &entities = *app.m_entities;
        TileMap &map = *app.m_map;
        Pathfinder &pf = *app.m_pathfinder;
        Simulation &sim = app.m_gameplayController->simulation();
        entities.Clear();
        map.InitBlankArena(40, 30, 0);
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
        sim.Reset();
        SimContext ctx {entities, map, pf, *app.m_economy, *app.m_aiController};
        const PlayerId owner = Players::OwnerOf(Faction::Resistance);
        const int id = entities.SpawnUnitByArchetype(0, 5, 5, Faction::Resistance, owner);
        auto order = [&](int tx, int ty, int b) {
            SimCommand c;
            c.type = CommandType::OrderAt;
            c.player = owner;
            c.ids = {static_cast<uint32_t>(id)};
            c.x = tx * TILE_WIDTH + TILE_WIDTH / 2;
            c.y = ty * TILE_HEIGHT + TILE_HEIGHT / 2;
            c.a = static_cast<int32_t>(OrderKind::Move);
            c.b = b;
            sim.Issue(std::move(c));
        };
        const float dt = static_cast<float>(SIMULATION_TIMESTEP);
        auto step = [&](int ticks) {
            for (int t = 0; t < ticks; ++t)
            {
                sim.ApplyCommands(ctx);
                entities.Update(dt, map, pf);
                sim.UpdateOrderQueues(ctx);
                sim.EndTick();
            }
        };
        order(20, 5, 0);
        order(20, 20, 2);
        order(5, 20, 2);
        step(1);
        const Unit *u = entities.GetUnit(id);
        const bool lineToClick = u && u->orderX == 20 * TILE_WIDTH + TILE_WIDTH / 2 && u->orderQueue.size() == 2;
        bool reachedFirst = false, firstBeforeSecond = true;
        for (int t = 0; t < 2400 && u; ++t)
        {
            step(1);
            if (std::abs(u->tileX - 20) <= 1 && std::abs(u->tileY - 5) <= 1)
                reachedFirst = true;
            if (!reachedFirst && std::abs(u->tileY - 20) <= 1)
                firstBeforeSecond = false;
            if (u->orderQueue.empty() && u->OrderFinished() && std::abs(u->tileX - 5) <= 1
                && std::abs(u->tileY - 20) <= 1)
                break;
        }
        const bool walked = u && std::abs(u->tileX - 5) <= 1 && std::abs(u->tileY - 20) <= 1 && u->orderQueue.empty()
                            && u->orderX < 0;
        order(30, 25, 0);
        order(10, 25, 2);
        step(1);
        order(30, 10, 0); // a plain order replaces the route
        step(1);
        const bool cleared = u && u->orderQueue.empty() && u->orderX == 30 * TILE_WIDTH + TILE_WIDTH / 2;
        entities.Clear();
        if (!lineToClick || !reachedFirst || !firstBeforeSecond || !walked || !cleared)
        {
            std::cerr << "[Test] Error: Shift queue: line to click=" << lineToClick
                      << ", reached first=" << reachedFirst << ", in order=" << firstBeforeSecond
                      << ", walked=" << walked << ", cleared=" << cleared << std::endl;
            return false;
        }
        std::cout << "  - Queued points walked in order; a plain order clears the route." << std::endl;
    }

    // 26y. Research animation: a lab animates only while its owner researches
    {
        std::cout << "[Test] Validating the research animation flag..." << std::endl;
        EntityManager &entities = *app.m_entities;
        Simulation &sim = app.m_gameplayController->simulation();
        SimContext ctx {entities, *app.m_map, *app.m_pathfinder, *app.m_economy, *app.m_aiController};
        entities.Clear();
        sim.Reset();
        const PlayerId owner = Players::OwnerOf(Faction::Resistance);
        const int lab = entities.SpawnBuildingByIndex(12, 10, 10, Faction::Resistance, 1.0f);
        Building *b = entities.GetBuilding(lab);
        if (b)
            b->owner = owner;
        sim.UpdateResearch(0.05f, ctx);
        const bool idle = b && !b->isResearching;
        sim.m_research[owner].active = 0;
        sim.UpdateResearch(0.05f, ctx);
        const bool working = b && b->isResearching;
        sim.m_research[owner].active = -1;
        sim.UpdateResearch(0.05f, ctx);
        const bool stopped = b && !b->isResearching;

        // Verify rendering behavior:
        // Resistance Lab (building 12): in IDLE, the radar head must remain visible (not hidden)
        // and stand still at frame 0 (composite 1262), rather than disappearing with cut-off sprites.
        // During research, animFrame > 0 and the radar head rotates.
        bool labIdleCheck = false;
        bool labWorkCheck = false;
        if (app.m_renderer && b)
        {
            b->isResearching = false;
            entities.render(*app.m_renderer, *app.m_camera, *app.m_map, false, 5);
            labIdleCheck
                = (app.m_renderer->lastDrawnBuilding.buildingType == 12
                   && app.m_renderer->lastDrawnBuilding.animFrame == 0
                   && app.m_renderer->lastDrawnBuilding.animatedPartsVisible);

            b->isResearching = true;
            entities.render(*app.m_renderer, *app.m_camera, *app.m_map, false, 5);
            labWorkCheck
                = (app.m_renderer->lastDrawnBuilding.buildingType == 12
                   && app.m_renderer->lastDrawnBuilding.animFrame > 0
                   && app.m_renderer->lastDrawnBuilding.animatedPartsVisible);
        }

        // Confederation Tech Center (building 4): in IDLE, screen lights are hidden (animatedPartsVisible == false),
        // while during research they are visible (animatedPartsVisible == true).
        bool techIdleCheck = false;
        bool techWorkCheck = false;
        const int tech = entities.SpawnBuildingByIndex(4, 20, 20, Faction::Confederation, 1.0f);
        Building *bTech = entities.GetBuilding(tech);
        if (app.m_renderer && bTech)
        {
            // Remove building 12 so building 4 is the last drawn building
            if (b)
                b->alive = false;

            bTech->isResearching = false;
            entities.render(*app.m_renderer, *app.m_camera, *app.m_map, false, 5);
            techIdleCheck
                = (app.m_renderer->lastDrawnBuilding.buildingType == 4
                   && app.m_renderer->lastDrawnBuilding.animFrame == 0
                   && !app.m_renderer->lastDrawnBuilding.animatedPartsVisible);

            bTech->isResearching = true;
            entities.render(*app.m_renderer, *app.m_camera, *app.m_map, false, 5);
            techWorkCheck
                = (app.m_renderer->lastDrawnBuilding.buildingType == 4
                   && app.m_renderer->lastDrawnBuilding.animFrame > 0
                   && app.m_renderer->lastDrawnBuilding.animatedPartsVisible);
        }

        entities.Clear();
        if (!idle || !working || !stopped
            || (app.m_renderer && (!labIdleCheck || !labWorkCheck || !techIdleCheck || !techWorkCheck)))
        {
            std::cerr << "[Test] Error: research animation flag or lab/tech render: idle=" << idle
                      << ", working=" << working << ", stopped=" << stopped << ", labIdle=" << labIdleCheck
                      << ", labWork=" << labWorkCheck << ", techIdle=" << techIdleCheck
                      << ", techWork=" << techWorkCheck << std::endl;
            return false;
        }
        std::cout << "  - The lab animates only while research runs (and radar head stands still in IDLE)."
                  << std::endl;
    }

    // 26w. The mouse wheel: counted for the map or the sidebar, never zooms by itself
    {
        std::cout << "[Test] Validating the mouse wheel routing..." << std::endl;
        InputManager &input = *app.m_input;
        Camera &cam = *app.m_camera;
        input.ConsumeMapWheel();
        input.ConsumeUiWheel();
        const float zoomBefore = cam.GetZoom();
        SDL_Event ev {};
        ev.type = SDL_EVENT_MOUSE_WHEEL;
        ev.wheel.y = 1.0f;
        ev.wheel.mouse_x = 10.0f; // over the map
        ev.wheel.mouse_y = 10.0f;
        input.ProcessEvent(ev, cam);
        ev.wheel.mouse_x = static_cast<float>(cam.GetViewportWidth() + 20); // over the sidebar
        input.ProcessEvent(ev, cam);
        const bool routed = input.ConsumeMapWheel() == 1 && input.ConsumeUiWheel() == 1;
        const bool noZoom = cam.GetZoom() == zoomBefore;
        input.ConsumeWheel();
        if (!routed || !noZoom)
        {
            std::cerr << "[Test] Error: mouse wheel: routed=" << routed << ", no zoom by itself=" << noZoom
                      << std::endl;
            return false;
        }
        std::cout << "  - Wheel over the map and over the sidebar counted apart; zoom only by the gameplay."
                  << std::endl;
    }

    // 26v. Computer groups: only the computer's own list goes into a wave, and squad members take orders from their
    // squad only (no unit is pulled in or called away because it stands inside some circle)
    {
        std::cout << "[Test] Validating the computer's unit lists..." << std::endl;
        EntityManager &entities = *app.m_entities;
        TileMap &map = *app.m_map;
        Pathfinder &pf = *app.m_pathfinder;
        AIController &ai = *app.m_aiController;
        entities.Clear();
        map.InitBlankArena(40, 30, 0);
        map.SetFogEnabled(false);
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
        const PlayerId aiPlayer = ai.m_player;
        const Faction aiSide = Players::FactionOf(aiPlayer);
        const Faction foe = aiSide == Faction::Resistance ? Faction::Confederation : Faction::Resistance;
        const int hqX = ai.m_enemyHqX, hqY = ai.m_enemyHqY;
        const bool seeded = ai.m_rosterSeeded;
        ai.m_enemyHqX = 10;
        ai.m_enemyHqY = 10;
        ai.m_waveRoster.clear();
        ai.m_rosterSeeded = true; // no seeding: the list is built by hand here
        const int stranger = entities.SpawnUnitByArchetype(0, 11, 11, aiSide, aiPlayer); // right at the base
        const int trained = entities.SpawnUnitByArchetype(0, 12, 11, aiSide, aiPlayer);
        ai.m_waveRoster.insert(trained);
        const bool strangerOut = !ai.IsReserveUnit(*entities.GetUnit(stranger));
        const bool trainedIn = ai.IsReserveUnit(*entities.GetUnit(trained));
        // A held squad member next to an enemy: the idle-unit logic leaves it to its squad
        const int member = entities.SpawnUnitByArchetype(0, 25, 15, aiSide, aiPlayer);
        const int enemy = entities.SpawnUnitByArchetype(0, 27, 15, foe, Players::OwnerOf(foe));
        const int squadId
            = ai.m_squadController.CreateSquad({member}, 35, 15, 1, entities, pf, ai.m_currentDifficultyConfig.tactics);
        Unit *m = entities.GetUnit(member);
        if (m)
        {
            m->state = ActionState::Idle;
            m->targetEntityId = 0;
            m->isAttackMoving = false;
            m->ClearMoveTarget();
        }
        ai.UpdateIdleUnits(entities, pf);
        const bool notGrabbed = m && m->targetEntityId != enemy;
        // Explicit reinforcement is the way into a squad
        const bool added = ai.m_squadController.AddUnits(squadId, {stranger}, entities)
                           && ai.m_squadController.IsSquadMember(stranger);
        ai.UpdateWaveRoster(entities);
        const bool rosterKeepsOwn = ai.m_waveRoster.count(trained) == 1 && ai.m_waveRoster.count(stranger) == 0;
        ai.m_squadController.ReleaseUnits({member, stranger}, entities);
        ai.m_squadController.TakeReleasedUnits();
        ai.m_waveRoster.clear();
        ai.m_enemyHqX = hqX;
        ai.m_enemyHqY = hqY;
        ai.m_rosterSeeded = seeded;
        entities.Clear();
        map.SetFogEnabled(true);
        if (!strangerOut || !trainedIn || !notGrabbed || !added || !rosterKeepsOwn)
        {
            std::cerr << "[Test] Error: computer lists: stranger out=" << strangerOut << ", trained in=" << trainedIn
                      << ", member not grabbed=" << notGrabbed << ", added explicitly=" << added
                      << ", roster=" << rosterKeepsOwn << std::endl;
            return false;
        }
        std::cout << "  - Waves take the computer's own list; squad members are led by their squad only." << std::endl;
    }

    // 26z. Campaign mission dialogues: every mission of both campaigns has one, in English and in Russian
    {
        std::cout << "[Test] Validating the campaign mission dialogues..." << std::endl;
        for (const char *lang : {"en", "ru"})
        {
            Localization::SetLanguage(lang);
            for (Faction side : {Faction::Resistance, Faction::Confederation})
            {
                for (int m = 0; m < MissionController::kCampaignMissionCount; ++m)
                {
                    const int id = MissionDialogs::IdForMission(side, m);
                    const MissionDialog dialog = MissionDialogs::Get(id);
                    bool ok = id != 0 && !dialog.title.empty() && dialog.lines.size() >= 2;
                    for (const DialogLine &line : dialog.lines)
                        ok = ok && !line.speaker.empty() && !line.text.empty() && line.text.rfind("dialog.", 0) != 0;
                    if (!ok)
                    {
                        std::cerr << "[Test] Error: mission dialogue " << id << " (" << lang << ") is missing or empty!"
                                  << std::endl;
                        Localization::SetLanguage("en");
                        return false;
                    }
                }
            }
        }
        const bool distinct = MissionDialogs::Get(DialogMission_conf_ID1).lines[0].text
                              != MissionDialogs::Get(DialogMission_res_ID1).lines[0].text;
        Localization::SetLanguage("ru");
        const std::string ruTitle = MissionDialogs::Get(DialogMission_conf_ID1).title;
        Localization::SetLanguage("en");
        if (!distinct || ruTitle == MissionDialogs::Get(DialogMission_conf_ID1).title
            || MissionDialogs::IdForMission(Faction::Resistance, 7) != 0 || MissionDialogs::Get(999).lines.size() != 0)
        {
            std::cerr << "[Test] Error: mission dialogue ids or languages are mixed up!" << std::endl;
            return false;
        }
        std::cout << "  - 14 mission dialogues (7 per campaign) in English and Russian." << std::endl;
    }

    // 27a. Radar alarm: goes off on contact, flashes, waves, stays quiet until the radar was clear long enough
    {
        std::cout << "[Test] Validating the radar alarm..." << std::endl;
        RadarAlert alert;
        nlohmann::json cfg
            = {{"blink_duration_s", 2.0},
               {"blink_hz", 2.0},
               {"rearm_delay_s", 5.0},
               {"wave_count", 3},
               {"wave_duration_s", 1.0},
               {"wave_interval_s", 0.5},
               {"wave_max_radius_px", 30},
               {"dot_radius_px", 2}};
        alert.config.load(cfg);
        alert.Update(1000, false, 0, 0);
        bool ok = !alert.Active() && alert.Armed();
        alert.Update(2000, true, 12.0f, 7.0f); // contact
        ok = ok && alert.Active() && alert.TileX() == 12.0f && alert.BlinkOn(2000) && !alert.BlinkOn(2300);
        float r = 0, o = 0;
        ok = ok && alert.Wave(0, 2500, r, o) && r > 2.0f && r < 30.0f && o > 0.0f && !alert.Wave(2, 2500, r, o)
             && alert.Wave(2, 3100, r, o);
        alert.Update(3000, true, 20.0f, 20.0f); // still in sight: no new alarm, same contact
        ok = ok && alert.TileX() == 12.0f;
        alert.Update(4500, true, 20.0f, 20.0f); // over (2 s flashing / 2 s of waves) but the enemy is still there
        ok = ok && !alert.Active() && !alert.Armed();
        alert.Update(8000, false, 0, 0); // gone 3.5 s: not long enough
        ok = ok && !alert.Armed();
        alert.Update(9600, false, 0, 0); // gone 5.1 s: armed again
        ok = ok && alert.Armed();
        alert.Update(9700, true, 3.0f, 4.0f);
        ok = ok && alert.Active() && alert.TileX() == 3.0f;
        std::cout << "  - Radar alarm: "
                  << (ok ? "contact, flashes, waves, quiet until the radar was clear for the rearm delay" : "BROKEN")
                  << std::endl;
        if (!ok)
        {
            std::cerr << "[Test] Error: radar alarm timing is wrong!" << std::endl;
            return false;
        }
    }

    // 27a. Voices: every language has every line; an unknown language speaks English
    {
        std::cout << "[Test] Validating the voices of each language..." << std::endl;
        VoiceManager en, ru, other;
        en.load("en", LoaderResources::kDefaultRoot);
        ru.load("ru", LoaderResources::kDefaultRoot);
        other.load("xx", LoaderResources::kDefaultRoot);
        static const char *const kLines[]
            = {"ack_select_infantry",
               "ack_move_vehicle",
               "ack_hold",
               "target_destroyed",
               "ann_unit_ready",
               "ann_base_attack",
               "ann_victory",
               "ann_defeat"};
        bool ok = en.LineCount() > 0 && ru.LineCount() == en.LineCount() && other.LineCount() == en.LineCount();
        for (const char *line : kLines)
            ok = ok && en.Has(line) && ru.Has(line) && other.Has(line);
        std::cout << "  - Voice lines: en " << en.LineCount() << ", ru " << ru.LineCount()
                  << ", unknown language (English) " << other.LineCount() << std::endl;
        if (!ok)
        {
            std::cerr << "[Test] Error: a language misses voice lines (assets/audio/voices/<language>)!" << std::endl;
            return false;
        }
    }

    // 27b. Wall breach at a single gap, retaliation against far shooters (see runCombatBehaviourTest)
    if (!RunCombatBehaviourTest(app))
        return false;

    // 27b2. Zeus guns (see RunZeusTest)
    if (!RunZeusTest(app))
        return false;

    // 27c. Allies make way for each other in narrow passages (see runMakeWayTest)
    if (!RunMakeWayTest(app))
        return false;

    // 28. Lockstep groundwork: same commands, same world (see runDeterminismTest)
    if (!RunDeterminismTest(app))
        return false;

    // 29. Several players, teams, per-player economies (see runMatchTest)
    if (!RunMatchTest(app))
        return false;

    // 29a. Zeus turret, research time, radar locator, quiet far-off battle sounds (see RunTechTest)
    if (!RunTechTest(app))
        return false;

    // 29e. No headquarters, no construction; players without buildings are out (see RunBuildRulesTest)
    if (!RunBuildRulesTest(app))
        return false;

    // 29d. Units sent through the fog meet enemy buildings shutting a passage (see RunFogBlockTest)
    if (!RunFogBlockTest(app))
        return false;

    // 29c. Computer players build a factory and field vehicles on every difficulty (see RunAiVehicleTest)
    if (!RunAiVehicleTest(app))
        return false;

    // 29b. A recorded network match plays back to the same worlds (see RunReplayTest)
    if (!RunReplayTest(app))
        return false;

    // 30. Computer players in match slots (see runMatchAiTest)
    if (!RunMatchAiTest(app))
        return false;

    std::cout << "[Test] Simulation completed successfully! Stats:" << std::endl;
    std::cout << "  - Player Credits: " << app.m_economy->GetCredits() << std::endl;
    std::cout << "  - Player Power: " << app.m_economy->GetPowerProduced() << "/" << app.m_economy->GetPowerConsumed()
              << std::endl;
    std::cout << "  - Player Kills: " << app.m_entities->GetPlayerKillCount() << std::endl;
    std::cout << "  - Total Entities remaining: " << app.m_entities->GetAllEntities().size() << std::endl;
    std::cout << "[Test] Test passed with 0 errors!" << std::endl;
    // 23. A computer player never shuts its own base in (the only passage of a room must stay open)
    std::cout << "[Test] Validating that the AI keeps the exit of its base open..." << std::endl;
    {
        EntityManager &em = *app.m_entities;
        TileMap &map = *app.m_map;
        const int roomW = 9, roomH = 7;
        int ox = -1, oy = -1;
        for (int y = 2; y + roomH + 2 < map.GetHeight() && ox < 0; ++y)
        {
            for (int x = 2; x + roomW + 2 < map.GetWidth() && ox < 0; ++x)
            {
                bool clear = true;
                for (int ty = y - 1; ty <= y + roomH && clear; ++ty)
                {
                    for (int tx = x - 1; tx <= x + roomW && clear; ++tx)
                    {
                        clear = map.IsPassable(tx, ty) && map.GetWall(tx, ty) == 0;
                        for (const auto &e : em.GetAllEntities())
                        {
                            const Building *b = e.AsBuilding();
                            if (b && e.alive && tx >= b->tileX - 1 && tx <= b->tileX + b->buildWidth
                                && ty >= b->tileY - 1 && ty <= b->tileY + b->buildHeight)
                                clear = false;
                        }
                    }
                }
                if (clear)
                {
                    ox = x;
                    oy = y;
                }
            }
        }
        if (ox < 0)
        {
            std::cout << "  - (no free 9x7 area on this map: base-exit check skipped)" << std::endl;
        }
        else
        {
            // Walls around the room, one door in the middle of the east side
            const int doorY = oy + roomH / 2;
            std::vector<std::pair<int, int>> walls;
            for (int x = ox - 1; x <= ox + roomW; ++x)
            {
                walls.push_back({x, oy - 1});
                walls.push_back({x, oy + roomH});
            }
            for (int y = oy; y < oy + roomH; ++y)
            {
                walls.push_back({ox - 1, y});
                if (y != doorY)
                    walls.push_back({ox + roomW, y});
            }
            for (const auto &w : walls)
                map.SetWall(w.first, w.second, Faction::Neutral, 100);
            const PlayerId ai = Players::OwnerOf(Faction::Confederation);
            const int hqId = em.SpawnBuildingByIndex(0, ox + 2, oy + 2, Faction::Confederation, 1.0f);
            const bool shutsDoor = em.KeepsBaseOpen(ox + roomW - 1, doorY, 1, 1, map, ai);
            const bool cornerOk = em.KeepsBaseOpen(ox + roomW - 2, oy + roomH - 2, 1, 1, map, ai);
            const bool outsideOk = em.KeepsBaseOpen(ox + roomW + 3, doorY + 2, 2, 2, map, ai);
            if (Building *hq = em.GetBuilding(hqId))
                hq->alive = false;
            for (const auto &w : walls)
                map.SetWall(w.first, w.second, Faction::Neutral, 0);
            if (shutsDoor || !cornerOk || !outsideOk)
            {
                std::cerr << "[Test] Error: base-exit check wrong: door blocked=" << !shutsDoor
                          << " (want 1), corner ok=" << cornerOk << ", outside ok=" << outsideOk << std::endl;
                return false;
            }
            std::cout << "  - The AI may not build in the only passage of its base, but builds elsewhere." << std::endl;
        }
    }

    // 24. Enemy structures do not show through the fog: only once seen, and never their construction out of sight
    std::cout << "[Test] Validating enemy structures under the fog of war..." << std::endl;
    {
        EntityManager &em = *app.m_entities;
        TileMap &map = *app.m_map;
        const Faction enemy
            = Players::Local() == Players::OwnerOf(Faction::Resistance) ? Faction::Confederation : Faction::Resistance;
        const bool fogWas = map.IsFogEnabled();
        map.SetFogEnabled(true);
        const int b1 = em.SpawnBuildingByIndex(
            enemy == Faction::Confederation ? 2 : 10, 8, 8, enemy, 0.3f); // under construction
        const int b2
            = em.SpawnBuildingByIndex(enemy == Faction::Confederation ? 2 : 10, 20, 8, enemy, 1.0f); // finished
        const Building *under = em.GetBuilding(b1);
        const Building *done = em.GetBuilding(b2);
        map.ResetAllFog(FogState::Explored); // explored ground, nothing in sight, nothing seen
        const bool neverSeenHidden = !em.IsHostileBuildingShown(*under, map) && !em.IsHostileBuildingShown(*done, map);
        map.ResetAllFog(FogState::Visible); // everything in sight
        bool inSight = false;
        const bool shownInSight
            = em.IsHostileBuildingShown(*under, map, &inSight) && inSight && em.IsHostileBuildingShown(*done, map);
        map.ResetAllFog(FogState::Explored);                                   // out of sight again
        const bool finishedStaysKnown = em.IsHostileBuildingShown(*done, map); // seen before and finished: still shown
        const bool progressHidden = !em.IsHostileBuildingShown(*under, map);   // construction out of sight: hidden
        map.SetFogEnabled(fogWas);
        if (Building *hq = em.GetBuilding(b1))
            hq->alive = false;
        if (Building *hq = em.GetBuilding(b2))
            hq->alive = false;
        if (!neverSeenHidden || !shownInSight || !finishedStaysKnown || !progressHidden)
        {
            std::cerr << "[Test] Error: enemy structures in the fog: never seen hidden=" << neverSeenHidden
                      << ", shown in sight=" << shownInSight << ", finished stays known=" << finishedStaysKnown
                      << ", construction hidden=" << progressHidden << std::endl;
            return false;
        }
        std::cout << "  - Enemy structures show only after they were seen; construction out of sight stays hidden."
                  << std::endl;

        // Walls under the fog of war: the viewer knows them as they were when last in sight (a network opponent must
        // not watch the other side build or lose walls in the fog)
        map.SetFogEnabled(true);
        const int wx = 30, wy = 30;
        map.DestroyWall(wx, wy);
        map.ResetAllFog(FogState::Hidden);
        map.RevealRect(wx, wy, 1, 1, map.GetViewerTeam());
        map.ResetVisibility();                                    // seen empty, now explored
        map.SetWall(wx, wy, enemy, 100, Players::OwnerOf(enemy)); // built out of sight
        const bool builtUnseen = map.IsWall(wx, wy) && map.ViewerWall(wx, wy) == 0;
        map.RevealRect(wx, wy, 1, 1, map.GetViewerTeam());
        const bool seenInSight = map.ViewerWall(wx, wy) == map.GetWall(wx, wy) && map.ViewerWall(wx, wy) > 0
                                 && map.ViewerWallOwner(wx, wy) == Players::OwnerOf(enemy);
        map.ResetVisibility(); // out of sight again, remembered standing
        map.DestroyWall(wx, wy);
        const bool destroyedUnseen = !map.IsWall(wx, wy) && map.ViewerWall(wx, wy) > 0;
        map.ResetAllFog(FogState::Hidden);
        const bool hiddenNothing = map.ViewerWall(wx, wy) == 0;
        map.SetFogEnabled(fogWas);
        if (!builtUnseen || !seenInSight || !destroyedUnseen || !hiddenNothing)
        {
            std::cerr << "[Test] Error: walls in the fog: built unseen=" << builtUnseen
                      << ", seen in sight=" << seenInSight << ", destroyed unseen=" << destroyedUnseen
                      << ", hidden=" << hiddenNothing << std::endl;
            return false;
        }
        std::cout << "  - Walls built or destroyed in the fog stay as last seen until the tile is in sight again."
                  << std::endl;
    }

    // 25. A map with buildings of its own: prebuilt bases or a start from scratch
    std::cout << "[Test] Validating prebuilt bases and the start from scratch..." << std::endl;
    {
        const std::string path = LoaderResources::TestMap();
        bool listed = false;
        for (const SkirmishMapInfo &m : MissionController::ListSkirmishMaps())
        {
            if (m.path == path)
                listed = m.hasPrebuilt;
        }
        auto countBuildings = [&]() {
            int n = 0;
            for (const auto &e : app.m_entities->GetAllEntities())
                n += (e.alive && e.AsBuilding()) ? 1 : 0;
            return n;
        };
        auto countUnits = [&]() {
            int n = 0;
            for (const auto &e : app.m_entities->GetAllEntities())
                n += (e.alive && !e.isBuilding) ? 1 : 0;
            return n;
        };
        auto countWalls = [&]() {
            int n = 0;
            for (int y = 0; y < app.m_map->GetHeight(); ++y)
                for (int x = 0; x < app.m_map->GetWidth(); ++x)
                    n += app.m_map->IsWall(x, y) ? 1 : 0;
            return n;
        };
        const bool prebuiltOk
            = app.m_mission->LoadSkirmish(path, *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder, true);
        const int prebuiltBuildings = countBuildings(), prebuiltWalls = countWalls();
        const bool scratchOk
            = app.m_mission->LoadSkirmish(path, *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder, false);
        const int scratchBuildings = countBuildings(), scratchUnits = countUnits(), scratchWalls = countWalls();
        if (!listed || !prebuiltOk || !scratchOk || prebuiltBuildings < 40 || prebuiltWalls < 100
            || scratchBuildings != 4 || scratchUnits != 0 || scratchWalls != 0)
        {
            std::cerr << "[Test] Error: prebuilt/scratch start on testmap: listed=" << listed << " buildings "
                      << prebuiltBuildings << "/" << scratchBuildings << " units=" << scratchUnits << " walls "
                      << prebuiltWalls << "/" << scratchWalls << std::endl;
            return false;
        }
        // Normal map without its own buildings: prebuilt = HQ + power + barracks + 4 units each; from scratch = HQ +
        // power only, no units
        const std::string plainMap = LoaderResources::MapPath("m26");
        const bool plainPrebuiltOk = app.m_mission->LoadSkirmish(
            plainMap, *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder, true);
        const int plainPrebuiltBuildings = countBuildings(), plainPrebuiltUnits = countUnits();
        const bool plainScratchOk = app.m_mission->LoadSkirmish(
            plainMap, *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder, false);
        const int plainScratchBuildings = countBuildings(), plainScratchUnits = countUnits();
        if (!plainPrebuiltOk || !plainScratchOk || plainPrebuiltBuildings != 6 || plainPrebuiltUnits != 8
            || plainScratchBuildings != 4 || plainScratchUnits != 0)
        {
            std::cerr << "[Test] Error: prebuilt/scratch start on m26: prebuilt=" << plainPrebuiltBuildings << "b/"
                      << plainPrebuiltUnits << "u, scratch=" << plainScratchBuildings << "b/" << plainScratchUnits
                      << "u" << std::endl;
            return false;
        }
        // AI starting from scratch (even on Easy, where defenses are off) builds a barracks and trains units
        app.m_aiController->SetDifficulty(AIDifficulty::Easy);
        for (int t = 0; t < 60; ++t)
        {
            app.m_aiController->Update(1.0f, *app.m_map, *app.m_entities, *app.m_pathfinder);
            app.m_entities->Update(1.0f, *app.m_map, *app.m_pathfinder);
        }
        if (!app.m_entities->HasBuildingType(UnitType::Barracks, app.m_aiController->player(), true)
            || countUnits() == 0)
        {
            std::cerr << "[Test] Error: AI starting from scratch did not build a barracks and train units!"
                      << std::endl;
            return false;
        }
        // Multiplayer match from scratch: HQ + power plant per slot, no barracks, no units
        {
            MatchSetup setup;
            setup.mapPath = plainMap;
            setup.prebuilt = false;
            setup.slots[0] = {true, Faction::Resistance, 0, 1, true};
            setup.slots[1] = {true, Faction::Confederation, 1, 2, true};
            setup.startPosition[0] = 0;
            setup.startPosition[1] = 1;
            if (!app.m_mission->LoadMatch(setup, *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder)
                || countBuildings() != 4 || countUnits() != 0)
            {
                std::cerr << "[Test] Error: multiplayer match from scratch must spawn 4 buildings and 0 units!"
                          << std::endl;
                return false;
            }
        }
        // Effects of destruction come from assets/characters/effects.yaml (defaults = the original numbers)
        {
            const EffectsConfig &fx = EffectsConfig::Get();
            EffectsConfig custom;
            const bool parsed = custom.LoadText(
                "explosions:\n  frame_time: 0.1\n  vehicle:\n    main: [20, 21]\nwrecks:\n  burial_seconds: 60\n  "
                "light_archetypes: []\n");
            if (fx.WreckOfVehicle(6).composite != 1759 || fx.WreckOfVehicle(10).stages != 4
                || fx.WreckOfVehicle(8).composite != 1764 || fx.WreckOfBuilding(2, 2).composite != 1741
                || fx.WreckOfBuilding(4, 3).composite != 1753 || fx.burialSeconds < 100.0f || !parsed
                || custom.vehicleMain.size() != 2 || custom.WreckOfVehicle(6).composite != 1764
                || custom.burialSeconds != 60.0f || custom.frameTime != 0.1f)
            {
                std::cerr << "[Test] Error: effects.yaml (wrecks / explosions)" << std::endl;
                return false;
            }
        }
        std::cout << "  - Prebuilt start: " << prebuiltBuildings << " buildings, " << prebuiltWalls
                  << " wall tiles; from scratch: " << scratchBuildings << " buildings, no walls." << std::endl;
    }

    // Camera ZOOM sound attenuation test (PlaceSfx)
    {
        // 1. Default zoom (2.0x): inside viewport gain is nominal (1.0f)
        const SfxPlacement pDefault = PlaceSfx(500.0f, 300.0f, 0.0f, 0.0f, 1000.0f, 600.0f, 360.0f, 2.0f);
        if (!pDefault.audible || std::abs(pDefault.gain - 1.0f) > 0.01f)
        {
            std::cerr << "[Test] Error: PlaceSfx at default zoom (2.0x) expected gain ~1.0, got " << pDefault.gain
                      << std::endl;
            return false;
        }

        // 2. Zoomed out (0.5x): camera high altitude reduces perceived battlefield sound volume
        const SfxPlacement pFar = PlaceSfx(500.0f, 300.0f, 0.0f, 0.0f, 1000.0f, 600.0f, 360.0f, 0.5f);
        if (!pFar.audible || pFar.gain >= 0.50f || pFar.gain <= 0.35f)
        {
            std::cerr << "[Test] Error: PlaceSfx at min zoom (0.5x) expected gain in [0.35, 0.50], got " << pFar.gain
                      << std::endl;
            return false;
        }

        // 3. Zoomed in (4.0x): camera close to ground amplifies perceived sound
        const SfxPlacement pClose = PlaceSfx(500.0f, 300.0f, 0.0f, 0.0f, 1000.0f, 600.0f, 360.0f, 4.0f);
        if (!pClose.audible || pClose.gain <= 1.20f || pClose.gain > 1.26f)
        {
            std::cerr << "[Test] Error: PlaceSfx at max zoom (4.0x) expected gain in (1.20, 1.26], got " << pClose.gain
                      << std::endl;
            return false;
        }

        // 4. Off-screen distance attenuation scales with zoom
        const SfxPlacement pOffDefault = PlaceSfx(1100.0f, 300.0f, 0.0f, 0.0f, 1000.0f, 600.0f, 360.0f, 2.0f);
        const SfxPlacement pOffFar = PlaceSfx(1100.0f, 300.0f, 0.0f, 0.0f, 1000.0f, 600.0f, 360.0f, 0.5f);
        if (!pOffDefault.audible || !pOffFar.audible || pOffFar.gain >= pOffDefault.gain)
        {
            std::cerr << "[Test] Error: Off-screen sound should attenuate further with zoom out: pOffFar="
                      << pOffFar.gain << ", pOffDefault=" << pOffDefault.gain << std::endl;
            return false;
        }

        // 5. Sound beyond range is not audible regardless of zoom
        const SfxPlacement pOut = PlaceSfx(1500.0f, 300.0f, 0.0f, 0.0f, 1000.0f, 600.0f, 360.0f, 4.0f);
        if (pOut.audible)
        {
            std::cerr << "[Test] Error: Out of range sound must not be audible" << std::endl;
            return false;
        }

        std::cout << "  - Camera ZOOM acoustic attenuation verified (0.5x -> " << pFar.gain << ", 2.0x -> "
                  << pDefault.gain << ", 4.0x -> " << pClose.gain << ")." << std::endl;
    }

    // Headquarters under construction opens no building zone
    {
        app.m_entities->Clear();
        const PlayerId owner = Players::OwnerOf(Faction::Resistance);
        app.m_map->RevealArea(60, 10, 40, Players::TeamOf(owner));
        app.m_entities->SetBuildRadius(16);
        app.m_entities->SpawnBuildingByIndex(8, 5, 5, Faction::Resistance, 1.0f); // finished HQ
        const int second = app.m_entities->SpawnBuildingByIndex(8, 50, 5, Faction::Resistance, 0.3f); // under construction
        const bool underConstruction = app.m_entities->CanPlaceBuilding(52, 10, 1, 1, *app.m_map, owner);
        if (Building *b = app.m_entities->GetEntity(second) ? app.m_entities->GetEntity(second)->AsBuilding() : nullptr)
            b->buildProgress = 1.0f;
        const bool finished = app.m_entities->CanPlaceBuilding(52, 10, 1, 1, *app.m_map, owner);
        std::cout << "  - HQ build zone: under construction " << underConstruction << ", finished " << finished
                  << std::endl;
        if (underConstruction || !finished)
        {
            std::cerr << "[Test] Error: a headquarters under construction must not open a build zone" << std::endl;
            return false;
        }
        app.m_entities->Clear();
    }

    // Damage multipliers by projectile kind and target class (data/weapon_balance.json)
    {
        WeaponBalance::Reload();
        const int bldg = WeaponBalance::Scale(100, ProjectileType::Bullet, TargetClass::Building);
        const int wall = WeaponBalance::Scale(100, ProjectileType::Bullet, TargetClass::Wall);
        const int inf = WeaponBalance::Scale(100, ProjectileType::Bullet, TargetClass::Infantry);
        const int veh = WeaponBalance::Scale(100, ProjectileType::Bullet, TargetClass::Vehicle);
        const int shellBld = WeaponBalance::Scale(100, ProjectileType::Shell, TargetClass::Building);
        if (bldg != 80 || wall != 80 || inf != 125 || veh != 100 || shellBld != 125
            || WeaponBalance::Scale(1, ProjectileType::Sniper, TargetClass::Wall) != 1)
        {
            std::cerr << "[Test] Error: weapon balance bullet building/wall/infantry/vehicle " << bldg << "/" << wall
                      << "/" << inf << "/" << veh << ", shell building " << shellBld << std::endl;
            return false;
        }
        WeaponBalance::LoadText(R"({"multipliers":{"bullet":{"building":0.5}}})");
        const bool overlay = WeaponBalance::Scale(100, ProjectileType::Bullet, TargetClass::Building) == 50
                             && WeaponBalance::Scale(100, ProjectileType::Bullet, TargetClass::Infantry) == 125;
        WeaponBalance::Reload();
        std::cout << "  - Weapon balance: bullet building " << bldg << "%, infantry " << inf << "%, overlay " << overlay
                  << std::endl;
        if (!overlay)
            return false;
    }

    // Low-pass (muffle) and reverb of the sound mixer
    {
        // A shot far out of range stays faintly audible when the event has a minimum volume
        const SfxPlacement faint = PlaceSfx(5000.0f, 300.0f, 0.0f, 0.0f, 1000.0f, 600.0f, 360.0f, 2.0f, 0.07f);
        const SfxPlacement silent = PlaceSfx(5000.0f, 300.0f, 0.0f, 0.0f, 1000.0f, 600.0f, 360.0f, 2.0f, 0.0f);
        if (!faint.audible || faint.gain < 0.0699f || faint.gain > 0.0701f || faint.muffle < 0.8f || silent.audible)
        {
            std::cerr << "[Test] Error: min_gain out of range: audible " << faint.audible << " gain " << faint.gain
                      << " muffle " << faint.muffle << ", without min_gain audible " << silent.audible << std::endl;
            return false;
        }
        {
            SfxMixer cfg;
            cfg.Configure(nlohmann::json::parse(
                R"({"events":{"fire_x":{"category":"weapon","positional":true,"min_gain":0.07}}})"));
            const SfxMixer::EventConfig *c = cfg.Config("fire_x");
            if (!c || c->minGain < 0.0699f || c->minGain > 0.0701f)
            {
                std::cerr << "[Test] Error: min_gain is not read from the sound settings" << std::endl;
                return false;
            }
        }
        const SfxPlacement near = PlaceSfx(500.0f, 300.0f, 0.0f, 0.0f, 1000.0f, 600.0f, 360.0f, 2.0f);
        const SfxPlacement far = PlaceSfx(1300.0f, 300.0f, 0.0f, 0.0f, 1000.0f, 600.0f, 360.0f, 2.0f);
        const SfxPlacement high = PlaceSfx(500.0f, 300.0f, 0.0f, 0.0f, 1000.0f, 600.0f, 360.0f, 0.5f);
        if (near.muffle > 0.001f || far.muffle <= 0.3f || high.muffle <= 0.3f)
        {
            std::cerr << "[Test] Error: muffle near/far/zoomed-out expected 0 / >0.3 / >0.3, got " << near.muffle
                      << " / " << far.muffle << " / " << high.muffle << std::endl;
            return false;
        }
        // A loud noise burst: the muffled copy has far less energy of change (highs), reverb leaves a tail
        auto render = [](float muffle, bool reverb, int &tailPeak) {
            SfxMixer mixer;
            mixer.Configure(nlohmann::json::parse(
                R"({"events":{"boom":{"category":"impact","volume":1.0,"reverb":1.0,"max_instances":2}}})"));
            std::vector<int16_t> pcm(2000);
            uint32_t seed = 12345;
            for (auto &v : pcm)
            {
                seed = seed * 1664525u + 1013904223u;
                v = static_cast<int16_t>((seed >> 16) % 20000) - 10000;
            }
            mixer.AddClip("boom", pcm);
            mixer.SetReverb(reverb);
            mixer.Play("boom", 1.0f, 0.0f, 1000, muffle);
            std::vector<int16_t> out(2 * 4096, 0);
            mixer.Mix(out.data(), 4096);
            double diff = 0.0;
            for (int i = 1; i < 2000; ++i)
                diff += std::abs(static_cast<int>(out[i * 2]) - static_cast<int>(out[(i - 1) * 2]));
            tailPeak = 0;
            for (int i = 2500; i < 4096; ++i)
                tailPeak = std::max(tailPeak, std::abs(static_cast<int>(out[i * 2])));
            return diff;
        };
        int tailDry = 0, tailWet = 0, tailMuffled = 0;
        const double clean = render(0.0f, false, tailDry);
        const double dull = render(0.9f, false, tailMuffled);
        render(0.0f, true, tailWet);
        if (!(dull < clean * 0.6) || tailDry != 0 || tailWet <= 0)
        {
            std::cerr << "[Test] Error: low-pass/reverb broken: roughness " << clean << " -> " << dull
                      << ", dry tail " << tailDry << ", reverb tail " << tailWet << std::endl;
            return false;
        }
        std::cout << "  - Low-pass roughness " << clean << " -> " << dull << ", reverb tail peak " << tailWet
                  << " (dry " << tailDry << ")." << std::endl;
    }

    // UI show dialog sound event test
    {
        // 1. EventOfFile parsing
        if (EventOfFile("assets/audio/sfx/ui_show_dialog.ogg") != "ui_show_dialog")
        {
            std::cerr << "[Test] Error: EventOfFile failed for ui_show_dialog.ogg, got: "
                      << EventOfFile("assets/audio/sfx/ui_show_dialog.ogg") << std::endl;
            return false;
        }
        if (EventOfFile("assets/audio/sfx/ui_click_1.ogg") != "ui_click")
        {
            std::cerr << "[Test] Error: EventOfFile failed for ui_click_1.ogg" << std::endl;
            return false;
        }

        // 2. SfxMixer bank loading has ui_show_dialog event
        if (app.m_sound)
        {
            if (!app.m_sound->Sfx().Has("ui_show_dialog"))
            {
                std::cerr << "[Test] Error: SfxMixer does not have 'ui_show_dialog' event!" << std::endl;
                return false;
            }
            const SfxMixer::EventConfig *cfg = app.m_sound->Sfx().Config("ui_show_dialog");
            if (!cfg || cfg->clips.empty())
            {
                std::cerr << "[Test] Error: 'ui_show_dialog' has no loaded audio clips!" << std::endl;
                return false;
            }
        }

        // 3. MenuController sound requests on opening menus / dialogs
        MenuController menu;
        SoundRequests::Take(); // Clear queue

        menu.SetInGameMenuOpen(true);
        std::vector<SoundRequests::Request> reqs = SoundRequests::Take();
        if (reqs.size() != 1 || reqs[0].event != "ui_show_dialog")
        {
            std::cerr << "[Test] Error: SetInGameMenuOpen(true) did not request 'ui_show_dialog'" << std::endl;
            return false;
        }

        // Already open -> no second sound
        menu.SetInGameMenuOpen(true);
        reqs = SoundRequests::Take();
        if (!reqs.empty())
        {
            std::cerr << "[Test] Error: SetInGameMenuOpen(true) when already open should not trigger sound"
                      << std::endl;
            return false;
        }

        menu.SetObjectivesDialogOpen(true);
        reqs = SoundRequests::Take();
        if (reqs.size() != 1 || reqs[0].event != "ui_show_dialog")
        {
            std::cerr << "[Test] Error: SetObjectivesDialogOpen(true) did not request 'ui_show_dialog'" << std::endl;
            return false;
        }

        menu.SetCampaignSelectOpen(true);
        reqs = SoundRequests::Take();
        if (reqs.size() != 1 || reqs[0].event != "ui_show_dialog")
        {
            std::cerr << "[Test] Error: SetCampaignSelectOpen(true) did not request 'ui_show_dialog'" << std::endl;
            return false;
        }

        std::cout << "  - UI show dialog sound (ui_show_dialog.ogg) verified for menus and dialog windows."
                  << std::endl;
    }

    return true;
}

} // namespace OpenArtOfWar
