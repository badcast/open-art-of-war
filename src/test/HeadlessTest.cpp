#include "test/HeadlessTest.hpp"
#include "game/Players.hpp"
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

    bool HeadlessTest::runDeterminismTest(Application &app)
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
        auto run = [&](const std::vector<TimedCommand> *replay)
        {
            std::vector<uint64_t> sums;
            entities.clear();
            map.initBlankArena(48, 32, 0);
            pf.setMap(&map);
            pf.setEntityManager(&entities);
            resetResearch();
            economy.reset(5000);
            sim.reset();
            std::vector<uint32_t> red, blue;
            for(int i = 0; i < 10; ++i)
            {
                red.push_back(static_cast<uint32_t>(entities.spawnUnitByArchetype(i % 3, 3 + i % 4, 10 + i / 4 * 2 + i % 2, Faction::Resistance)));
                blue.push_back(static_cast<uint32_t>(entities.spawnUnitByArchetype(1 + i % 2, 42 - i % 4, 12 + i / 4 * 2, Faction::Confederation)));
            }
            ai.m_chokes.rebuild(map);
            SimContext ctx {entities, map, pf, economy, ai};
            auto order = [&](Faction f, const std::vector<uint32_t> &ids, int tx, int ty, OrderKind kind, bool group)
            {
                SimCommand c;
                c.type = CommandType::OrderAt;
                c.player = Players::ownerOf(f);
                c.ids = ids;
                c.x = tx * TILE_WIDTH + TILE_WIDTH / 2;
                c.y = ty * TILE_HEIGHT + TILE_HEIGHT / 2;
                c.a = static_cast<int32_t>(kind);
                c.b = group ? 1 : 0;
                sim.issue(std::move(c));
            };
            const float dt = static_cast<float>(SIMULATION_TIMESTEP);
            if(replay)
            {
                for(const TimedCommand &t : *replay)
                    sim.schedule(t.tick, t.command);
            }
            for(int tick = 0; tick < 900; ++tick)
            {
                if(replay)
                {
                    // nothing scripted: the recording is already scheduled
                }
                else if(tick == 0)
                {
                    order(Faction::Resistance, red, 40, 14, OrderKind::AttackMove, true);
                    order(Faction::Confederation, blue, 6, 13, OrderKind::Move, false);
                }
                else if(tick == 120)
                {
                    SimCommand stop;
                    stop.type = CommandType::Stop;
                    stop.player = Players::ownerOf(Faction::Confederation);
                    stop.ids.assign(blue.begin(), blue.begin() + 4);
                    sim.issue(std::move(stop));
                }
                else if(tick == 200)
                {
                    order(Faction::Confederation, blue, 30, 20, OrderKind::AttackMove, false);
                }
                sim.applyCommands(ctx);
                sim.updateResearch(dt, ctx);
                entities.update(dt, map, pf);
                sim.updateSquads(dt, ctx, ai.m_chokes);
                economy.update(dt, entities, map, pf);
                if(tick % 20 == 0)
                    sums.push_back(sim.checksum(ctx));
                sim.endTick();
            }
            return sums;
        };

        const std::vector<uint64_t> first = run(nullptr);
        // The recording through the wire format (encoded per tick and side, decoded back)
        std::vector<TimedCommand> recorded;
        {
            const std::vector<TimedCommand> log = sim.log();
            size_t i = 0;
            while(i < log.size())
            {
                size_t j = i;
                std::vector<SimCommand> turn;
                while(j < log.size() && log[j].tick == log[i].tick && log[j].command.player == log[i].command.player)
                {
                    turn.push_back(log[j].command);
                    ++j;
                }
                const std::vector<uint8_t> bytes = SimCommandCodec::encode(turn);
                std::vector<SimCommand> back;
                if(!SimCommandCodec::decode(bytes.data(), bytes.size(), log[i].command.player, back) || back.size() != turn.size())
                {
                    std::cerr << "[Test] FAILED: command codec round trip" << std::endl;
                    return false;
                }
                std::cout << "  - Tick " << log[i].tick << ": " << turn.size() << " command(s) of side " << static_cast<int>(log[i].command.player) << " in " << bytes.size() << " bytes" << std::endl;
                for(SimCommand &c : back)
                    recorded.push_back({log[i].tick, std::move(c)});
                i = j;
            }
        }
        const std::vector<uint64_t> second = run(&recorded);
        size_t firstDiff = first.size();
        for(size_t i = 0; i < first.size() && i < second.size(); ++i)
        {
            if(first[i] != second[i])
            {
                firstDiff = i;
                break;
            }
        }
        const bool moved = first.size() > 2 && first.front() != first.back();
        std::cout << "DETERMINISM-HASH " << std::hex << first.back() << std::dec << std::endl;
        std::cout << "  - " << first.size() << " checksums over 900 ticks (second run replayed from the recording), first difference at " << (firstDiff == first.size() ? std::string("none") : "tick " + std::to_string(firstDiff * 20)) << ", world changed " << moved << ", final " << std::hex
                  << first.back() << std::dec << std::endl;
        if(!moved)
        {
            std::cerr << "[Test] FAILED: the determinism scenario does not change the world" << std::endl;
            return false;
        }
        if(firstDiff != first.size() || first.size() != second.size())
        {
            std::cerr << "[Test] FAILED: the same commands gave a different world (desync)" << std::endl;
            return false;
        }
        entities.clear();
        sim.reset();
        resetResearch();
        return true;
    }

    bool HeadlessTest::runMatchTest(Application &app)
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
        setup.mapPath = "assets/maps/m26.json";
        setup.seed = 12345;
        setup.local = 0;
        const Faction factions[4] = {Faction::Resistance, Faction::Confederation, Faction::Resistance, Faction::Confederation};
        const uint8_t teams[4] = {0, 0, 1, 1};
        for(int i = 0; i < 4; ++i)
        {
            setup.slots[i] = {true, factions[i], teams[i], static_cast<uint8_t>(i + 1), true};
            setup.startPosition[i] = static_cast<int8_t>(i);
        }
        if(MissionController::startPositions(setup.mapPath).size() < 4)
        {
            std::cerr << "[Test] FAILED: m26 must offer 4 start positions" << std::endl;
            return false;
        }
        if(MissionController::startPositions("assets/maps/map_skirmish_5.json").size() != 2)
        {
            std::cerr << "[Test] FAILED: a map without start_positions is a map for two players" << std::endl;
            return false;
        }

        auto run = [&](std::vector<uint64_t> &sums) -> bool
        {
            if(!mission.loadMatch(setup, map, entities, economy, pf))
                return false;
            sim.startMatch(setup);
            SimContext ctx {entities, map, pf, economy, ai};
            // Every player owns a headquarters and four units of its own faction
            int hq[4] = {}, units[4] = {};
            for(const Entity &e : entities.getAllEntities())
            {
                if(e.owner >= 4)
                    return false;
                if(e.faction != factions[e.owner])
                    return false;
                if(e.isBuilding && e.type == UnitType::Headquarters)
                    ++hq[e.owner];
                if(!e.isBuilding)
                    ++units[e.owner];
            }
            for(int i = 0; i < 4; ++i)
            {
                if(hq[i] != 1 || units[i] != 4)
                {
                    std::cerr << "[Test] FAILED: player " << i << " has " << hq[i] << " HQ / " << units[i] << " units" << std::endl;
                    return false;
                }
            }
            // Player 2 (Resistance, team 1) sends its units at player 0 (Resistance, team 0)
            std::vector<uint32_t> attackers;
            int targetHq = 0;
            for(const Entity &e : entities.getAllEntities())
            {
                if(e.owner == 2 && !e.isBuilding)
                    attackers.push_back(static_cast<uint32_t>(e.id));
                if(e.owner == 0 && e.isBuilding && e.type == UnitType::Headquarters)
                    targetHq = e.id;
            }
            const Entity *hqEntity = entities.getEntity(targetHq);
            SimCommand order;
            order.type = CommandType::OrderAt;
            order.player = 2;
            order.ids = attackers;
            order.x = hqEntity->tileX * TILE_WIDTH;
            order.y = hqEntity->tileY * TILE_HEIGHT;
            order.a = static_cast<int32_t>(OrderKind::AttackMove);
            sim.issue(order);
            // A command for someone else's units does nothing
            SimCommand foreign = order;
            foreign.player = 1;
            sim.issue(foreign);
            const float dt = static_cast<float>(SIMULATION_TIMESTEP);
            for(int tick = 0; tick < 1200; ++tick)
            {
                sim.applyCommands(ctx);
                sim.updateResearch(dt, ctx);
                entities.update(dt, map, pf);
                sim.updateSquads(dt, ctx, ai.getChokepoints());
                economy.update(dt, entities, map, pf);
                sim.updateEconomies(dt, ctx);
                if(tick % 60 == 0)
                    sums.push_back(sim.checksum(ctx));
                sim.endTick();
            }
            return true;
        };

        std::vector<uint64_t> first, second;
        if(!run(first))
        {
            std::cerr << "[Test] FAILED: the match did not set up" << std::endl;
            return false;
        }
        // Teams: same faction, different team = enemies; different faction, same team = allies
        const bool teamsRight = Players::hostile(0, 2) && !Players::hostile(0, 1) && Players::hostile(1, 3) && !Players::hostile(2, 3);
        // The other players' economies run here too (income every 3 s)
        SimContext probe {entities, map, pf, economy, ai};
        const bool economiesRun = sim.economyOf(3, probe).getCredits() > setup.startingCredits;
        if(!run(second))
            return false;
        const bool same = first == second;
        std::cout << "  - 4 bases set, teams " << teamsRight << ", other economies run " << economiesRun << ", deterministic " << same << " (" << first.size() << " checksums)" << std::endl;

        // Back to the single-player defaults for the tests after this one
        setPlayerFaction(Faction::Resistance);
        entities.clear();
        sim.reset();
        resetResearch();
        if(!teamsRight || !economiesRun || !same)
        {
            std::cerr << "[Test] FAILED: 4-player match" << std::endl;
            return false;
        }
        return true;
    }

    bool HeadlessTest::runMatchAiTest(Application &app)
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
        setup.mapPath = "assets/maps/m26.json";
        setup.seed = 777;
        setup.local = 0;
        setup.slots[0] = {true, Faction::Resistance, 0, 1, true, 1};
        setup.slots[1] = {true, Faction::Confederation, 1, 2, false, 2};
        setup.slots[2] = {true, Faction::Resistance, 1, 3, false, 1};
        for(int i = 0; i < 3; ++i)
            setup.startPosition[i] = static_cast<int8_t>(i);

        struct Outcome
        {
            std::vector<uint64_t> sums;
            int aiBuildings {0}, aiUnits {0}, ais {0};
        };
        auto run = [&]()
        {
            Outcome o;
            if(!mission.loadMatch(setup, map, entities, economy, pf))
                return o;
            gc.resetForMission(mission, *app.m_hudController, entities, map);
            o.ais = static_cast<int>(gc.m_matchAis.size());
            SimContext ctx {entities, map, pf, economy, *app.m_aiController};
            const float dt = static_cast<float>(SIMULATION_TIMESTEP);
            for(int tick = 0; tick < 3000; ++tick)
            {
                sim.applyCommands(ctx);
                sim.updateResearch(dt, ctx);
                entities.update(dt, map, pf);
                sim.updateSquads(dt, ctx, app.m_aiController->getChokepoints());
                economy.update(dt, entities, map, pf);
                sim.updateEconomies(dt, ctx);
                for(auto &ai : gc.m_matchAis)
                    ai->update(dt, map, entities, pf);
                if(tick % 100 == 0)
                    o.sums.push_back(sim.checksum(ctx));
                sim.endTick();
            }
            for(const Entity &e : entities.getAllEntities())
            {
                if(!e.alive || (e.owner != 1 && e.owner != 2))
                    continue;
                (e.isBuilding ? o.aiBuildings : o.aiUnits)++;
            }
            return o;
        };
        const Outcome first = run();
        const Outcome second = run();
        std::cout << "  - " << first.ais << " computer players: " << first.aiBuildings << " buildings, " << first.aiUnits << " units after 150 s (started with 6 / 8), deterministic " << (first.sums == second.sums) << std::endl;
        setPlayerFaction(Faction::Resistance);
        entities.clear();
        sim.reset();
        gc.m_matchAis.clear();
        resetResearch();
        if(first.ais != 2 || first.sums.empty() || first.sums != second.sums)
        {
            std::cerr << "[Test] FAILED: computer players in a match" << std::endl;
            return false;
        }
        if(first.aiBuildings <= 6 && first.aiUnits <= 8)
        {
            std::cerr << "[Test] FAILED: the computer players did nothing" << std::endl;
            return false;
        }
        return true;
    }

    bool HeadlessTest::runGroupMovementTest(Application &app)
    {
        // Group movement quality: no stacking/teleports, no single-file "train", everyone arrives
        std::cout << "[Test] Validating Group Movement (no overlap, no teleports, no follow-the-leader train)..." << std::endl;
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

            auto runGroup = [&](const std::vector<std::pair<int, std::pair<int, int>>> &spawns, int goalX, int goalY, float maxTime, bool headOn) -> GroupRunStats
            {
                GroupRunStats st;
                app.m_entities->clear();
                app.m_map->initBlankArena(60, 40, 0);
                app.m_pathfinder->setMap(app.m_map.get());
                app.m_pathfinder->setEntityManager(app.m_entities.get());

                std::vector<int> ids;
                for(const auto &s : spawns)
                {
                    int id = app.m_entities->spawnUnitByArchetype(s.first, s.second.first, s.second.second, Faction::Resistance);
                    if(id > 0)
                        ids.push_back(id);
                }
                st.total = static_cast<int>(ids.size());

                if(headOn)
                {
                    // Two halves ordered through each other
                    std::vector<int> left, right;
                    for(int id : ids)
                    {
                        if(const Unit *u = app.m_entities->getUnit(id))
                            (u->tileX < 30 ? left : right).push_back(id);
                    }
                    app.m_entities->issueMoveOrder(left, 42, 20, *app.m_pathfinder);
                    app.m_entities->issueMoveOrder(right, 16, 20, *app.m_pathfinder);
                }
                else
                {
                    app.m_entities->issueMoveOrder(ids, goalX, goalY, *app.m_pathfinder);
                }

                std::map<int, std::pair<float, float>> lastPx;
                std::map<int, std::pair<float, float>> lastDelta;
                std::map<int, std::pair<int, int>> lastTile;
                std::map<int, float> stall;
                int moverSamples = 0, trainSamples = 0;
                const float dt = 1.0f / 30.0f;

                for(float t = 0.0f; t < maxTime; t += dt)
                {
                    // Who was moving and where, BEFORE the tick
                    std::set<std::pair<int, int>> moverTilesPrev;
                    for(int id : ids)
                    {
                        if(const Unit *u = app.m_entities->getUnit(id); u && u->state == ActionState::Move)
                            moverTilesPrev.insert({u->tileX, u->tileY});
                    }

                    app.m_entities->update(dt, *app.m_map, *app.m_pathfinder);

                    std::set<std::pair<int, int>> occ;
                    bool overlap = false, anyOrdered = false;
                    for(int id : ids)
                    {
                        Unit *u = app.m_entities->getUnit(id);
                        if(!u)
                            continue;
                        if(!occ.insert({u->tileX, u->tileY}).second)
                            overlap = true;

                        float px = u->getWorldPixelX(), py = u->getWorldPixelY();
                        auto itT = lastTile.find(id);
                        if(itT != lastTile.end() && std::max(std::abs(itT->second.first - u->tileX), std::abs(itT->second.second - u->tileY)) > 1)
                        {
                            st.teleports++;
                            overlap = true;
                        }
                        if(auto itP = lastPx.find(id); itP != lastPx.end())
                        {
                            float ddx = px - itP->second.first, ddy = py - itP->second.second;
                            float maxStep = u->moveSpeed * dt * 60.0f + 0.5f;
                            if(std::hypot(ddx, ddy) > maxStep)
                                st.snaps++;
                            auto itD = lastDelta.find(id);
                            if(itD != lastDelta.end() && std::hypot(ddx, ddy) > 0.05f && ddx * itD->second.first + ddy * itD->second.second < 0.0f)
                                st.reversals++;
                            if(std::hypot(ddx, ddy) > 0.05f)
                                lastDelta[id] = {ddx, ddy};
                        }
                        bool ordered = u->state == ActionState::Move || u->waitingForPath || u->hasMoveTarget();
                        if(ordered)
                        {
                            anyOrdered = true;
                            auto itP = lastPx.find(id);
                            float moved = (itP == lastPx.end()) ? 99.0f : std::hypot(px - itP->second.first, py - itP->second.second);
                            stall[id] = (moved < 0.05f) ? stall[id] + dt : 0.0f;
                            st.maxStall = std::max(st.maxStall, stall[id]);
                        }
                        if(u->state == ActionState::Move && itT != lastTile.end() && (itT->second.first != u->tileX || itT->second.second != u->tileY))
                        {
                            moverSamples++;
                            // entered a tile that a mover stood on a tick ago => trailing in its footsteps
                            if(moverTilesPrev.count({u->tileX, u->tileY}))
                                trainSamples++;
                        }
                        lastPx[id] = {px, py};
                        lastTile[id] = {u->tileX, u->tileY};
                    }
                    if(overlap)
                        st.overlapTicks++;
                    if(!anyOrdered && st.settleTime < 0.0f)
                    {
                        st.settleTime = t;
                        break;
                    }
                }

                for(int id : ids)
                {
                    const Unit *u = app.m_entities->getUnit(id);
                    if(!u)
                        continue;
                    int gx = goalX, gy = goalY;
                    if(headOn)
                    {
                        gx = (lastTile[id].first > 30) ? 42 : 16;
                        gy = 20;
                    }
                    if(std::max(std::abs(u->tileX - gx), std::abs(u->tileY - gy)) <= 4)
                        st.arrived++;
                    if(std::getenv("OAOW_TEST_VERBOSE") && (u->state == ActionState::Move || u->waitingForPath || u->hasMoveTarget()))
                    {
                        std::cout << "    [stuck] id=" << id << " tile=(" << u->tileX << "," << u->tileY << ") sub=(" << u->subX << "," << u->subY << ") state=" << static_cast<int>(u->state) << " waiting=" << u->waitingForPath << " target=(" << u->targetTileX << "," << u->targetTileY
                                  << ") pathLeft=" << (u->path.size() > u->pathIndex ? u->path.size() - u->pathIndex : 0) << " next=" << (u->pathIndex < u->path.size() ? std::to_string(u->path[u->pathIndex].x) + "," + std::to_string(u->path[u->pathIndex].y) : std::string("-"))
                                  << " wait=" << u->waitBlockedTimer << std::endl;
                    }
                }
                st.trainRatio = moverSamples ? static_cast<float>(trainSamples) / moverSamples : 0.0f;
                return st;
            };

            // A) 12 mixed units in a 4x3 blob, marching 26 tiles east
            std::vector<std::pair<int, std::pair<int, int>>> blob;
            const int archs[4] = {0, 2, 8, 10};
            for(int r = 0; r < 3; ++r)
                for(int c = 0; c < 4; ++c)
                    blob.push_back({archs[(r + c) % 4], {8 + c, 18 + r}});
            GroupRunStats a = runGroup(blob, 36, 19, 30.0f, false);
            std::cout << "  - Blob march: arrived " << a.arrived << "/" << a.total << ", overlapTicks=" << a.overlapTicks << ", teleports=" << a.teleports << ", snaps=" << a.snaps << ", reversals=" << a.reversals << ", maxStall=" << a.maxStall << "s, trainRatio=" << a.trainRatio
                      << ", settle=" << a.settleTime << "s" << std::endl;

            // B) Head-on: two groups of 6 crossing through each other
            std::vector<std::pair<int, std::pair<int, int>>> cross;
            for(int i = 0; i < 6; ++i)
            {
                cross.push_back({0, {14 + (i % 2), 18 + i / 2}});
                cross.push_back({0, {44 - (i % 2), 18 + i / 2}});
            }
            GroupRunStats b = runGroup(cross, 0, 0, 30.0f, true);
            std::cout << "  - Head-on crossing: arrived " << b.arrived << "/" << b.total << ", overlapTicks=" << b.overlapTicks << ", teleports=" << b.teleports << ", snaps=" << b.snaps << ", reversals=" << b.reversals << ", maxStall=" << b.maxStall << "s, settle=" << b.settleTime << "s"
                      << std::endl;

            // C) Order spam: re-target a mixed squad every 0.2s so most orders land while units are between tiles
            int reorderSnaps = 0;
            {
                app.m_entities->clear();
                app.m_map->initBlankArena(60, 40, 0);
                app.m_pathfinder->setMap(app.m_map.get());
                app.m_pathfinder->setEntityManager(app.m_entities.get());
                std::vector<int> squad;
                for(int i = 0; i < 6; ++i)
                {
                    int id = app.m_entities->spawnUnitByArchetype(archs[i % 4], 28 + (i % 3), 19 + i / 3, Faction::Resistance);
                    if(id > 0)
                        squad.push_back(id);
                }
                std::map<int, std::pair<float, float>> lastPx;
                const float dt = 1.0f / 30.0f;
                for(int tick = 0; tick < 150; ++tick)
                {
                    if(tick % 6 == 0)
                    {
                        bool east = (tick / 6) % 2 == 0;
                        app.m_entities->issueMoveOrder(squad, east ? 40 : 16, east ? 14 : 26, *app.m_pathfinder);
                    }
                    app.m_entities->update(dt, *app.m_map, *app.m_pathfinder);
                    for(int id : squad)
                    {
                        const Unit *u = app.m_entities->getUnit(id);
                        if(!u)
                            continue;
                        float px = u->getWorldPixelX(), py = u->getWorldPixelY();
                        if(auto it = lastPx.find(id); it != lastPx.end() && std::hypot(px - it->second.first, py - it->second.second) > u->moveSpeed * dt * 60.0f + 0.5f)
                        {
                            reorderSnaps++;
                        }
                        lastPx[id] = {px, py};
                    }
                }
                std::cout << "  - Order spam (re-target every 0.2s): snaps=" << reorderSnaps << std::endl;
            }

            bool ok = true;
            if(reorderSnaps > 0)
            {
                std::cerr << "[Test] Error: units snapped back when re-ordered mid-step!" << std::endl;
                ok = false;
            }
            if(a.arrived != a.total || b.arrived != b.total)
            {
                std::cerr << "[Test] Error: not all units reached the destination!" << std::endl;
                ok = false;
            }
            if(a.overlapTicks > 0 || b.overlapTicks > 0)
            {
                std::cerr << "[Test] Error: units overlapped / were teleported apart!" << std::endl;
                ok = false;
            }
            if(a.snaps > 0 || b.snaps > 0)
            {
                std::cerr << "[Test] Error: units jerked (pixel position jumped faster than move speed)!" << std::endl;
                ok = false;
            }
            if(a.reversals > 2 || b.reversals > 12)
            {
                std::cerr << "[Test] Error: units shake back and forth while moving!" << std::endl;
                ok = false;
            }
            if(a.maxStall > 2.0f || b.maxStall > 3.0f)
            {
                std::cerr << "[Test] Error: units stalled in place too long!" << std::endl;
                ok = false;
            }
            if(a.trainRatio > 0.35f)
            {
                std::cerr << "[Test] Error: group degenerated into a single-file train!" << std::endl;
                ok = false;
            }
            if(a.settleTime < 0.0f || b.settleTime < 0.0f)
            {
                std::cerr << "[Test] Error: group never settled (endless shuffling)!" << std::endl;
                ok = false;
            }
            if(!ok)
                return false;
            std::cout << "  - Group movement validated: parallel lanes, no overlap, no train, clean arrival." << std::endl;
        }
        return true;
    }

    bool HeadlessTest::runAIBehaviourTest(Application &app)
    {
        std::cout << "[Test] Validating AI Director, Squad Controller & Chokepoint Handler..." << std::endl;
        AIController &ai = *app.m_aiController;
        EntityManager &entities = *app.m_entities;
        TileMap &map = *app.m_map;
        Pathfinder &pf = *app.m_pathfinder;
        const AIDebugConfig savedDebug = ai.getDebugConfig();

        auto withDifficulty = [&](int level)
        {
            AIDebugConfig d = savedDebug;
            d.forceDebugOverride = true;
            d.debugDifficultyLevel = level;
            ai.setDebugConfig(d);
        };
        auto count = [](const AIWavePlan &p, std::initializer_list<int> archs)
        {
            int n = 0;
            for(int a : p.archetypes)
            {
                for(int b : archs)
                    if(a == b)
                        ++n;
            }
            return n;
        };
        auto fail = [&](const std::string &msg)
        {
            std::cerr << "[Test] Error: " << msg << std::endl;
            ai.setDebugConfig(savedDebug);
            return false;
        };

        // A) Escalation curve: weak infantry first, power grows with the wave number and the difficulty
        int firstWaveSize[3] = {0, 0, 0};
        for(int level = 0; level <= 2; ++level)
        {
            withDifficulty(level);
            AIWavePlan w1 = ai.planWave(1, true, true);
            AIWavePlan w3 = ai.planWave(3, true, true);
            AIWavePlan w5 = ai.planWave(5, true, true);
            AIWavePlan w7 = ai.planWave(7, true, true);
            firstWaveSize[level] = static_cast<int>(w1.archetypes.size());
            if(w1.tier != AIWaveTier::Scout || count(w1, {1}) != static_cast<int>(w1.archetypes.size()))
            {
                return fail("wave 1 must be light infantry only (difficulty " + std::to_string(level) + ")");
            }
            int support = count(w3, {3});
            if(w3.tier != AIWaveTier::Vanguard || support < 1 || support > 2 || count(w3, {7, 9, 11, 13}) != 0)
            {
                return fail("wave 3 must be an infantry vanguard with 1-2 rocket troopers");
            }
            if(w5.tier != AIWaveTier::Assault || count(w5, {7, 9, 11}) < 1)
            {
                return fail("wave 5 must bring tanks");
            }
            if(w1.archetypes.size() > w3.archetypes.size() || w3.archetypes.size() > w5.archetypes.size())
            {
                return fail("wave size must not shrink as the AI escalates");
            }
            if(level == 0 && w7.tier == AIWaveTier::Siege)
                return fail("Easy AI must not reach the Siege tier");
            if(level >= 1 && (w7.tier != AIWaveTier::Siege || count(w7, {13}) < 1))
            {
                return fail("wave 7 must be a siege fist with MLRS on Normal/Hard");
            }
            if(count(ai.planWave(5, false, false), {7, 9, 11, 13}) != 0)
            {
                return fail("without a factory the plan must not contain vehicles");
            }
        }
        if(!(firstWaveSize[0] < firstWaveSize[1] && firstWaveSize[1] < firstWaveSize[2]))
        {
            return fail("first wave must grow with difficulty (Easy < Normal < Hard)");
        }
        {
            AIDebugConfig d = savedDebug;
            d.forceDebugOverride = true;
            d.debugDifficultyLevel = 1;
            d.waveSpawnMultiplier = 2.0f;
            ai.setDebugConfig(d);
            if(static_cast<int>(ai.planWave(1, true, true).archetypes.size()) != firstWaveSize[1] * 2)
            {
                return fail("wave_spawn_multiplier must scale the wave size");
            }
        }
        std::cout << "  - Escalation: wave 1 = " << firstWaveSize[0] << "/" << firstWaveSize[1] << "/" << firstWaveSize[2] << " riflemen (Easy/Normal/Hard), vanguard -> tanks -> siege fist." << std::endl;

        // B) Debug override of the difficulty
        withDifficulty(2);
        if(ai.getEffectiveDifficulty() != AIDifficulty::Hard || !ai.getTactics().syncToSlowest || ai.getTactics().retreatLossRatio <= 0.0f)
        {
            return fail("debug override to Hard did not apply Hard tactics");
        }
        withDifficulty(0);
        if(ai.getTactics().waitAtChokepoints || ai.getTactics().leashTiles < 5.0f)
        {
            return fail("Easy tactics must allow a loose formation and skip chokepoint gathering");
        }
        ai.setDebugConfig(savedDebug);
        if(ai.getEffectiveDifficulty() != ai.getDifficulty())
            return fail("disabling the override must restore the menu difficulty");
        std::cout << "  - Debug difficulty override switches tactics at runtime and restores the menu setting." << std::endl;

        // Helpers for the squad scenarios
        auto setupArena = [&]()
        {
            entities.clear();
            map.initBlankArena(40, 30, 0);
            pf.setMap(&map);
            pf.setEntityManager(&entities);
            ai.m_squadController.clear();
        };
        auto buildWallWithGap = [&]()
        {
            for(int y = 0; y < 30; ++y)
            {
                if(y < 14 || y > 15)
                    map.setPassable(20, y, false);
            }
        };
        struct SquadRun
        {
            std::set<AISquadState> seen;
            int released = 0;
            bool assault = false;
            float vanguardMaxXEarly = 0.0f;
        };
        auto runSquad = [&](const std::vector<int> &ids, int tx, int ty, float maxTime, const std::vector<int> &vanguard)
        {
            SquadRun r;
            ai.m_chokes.rebuild(map);
            ai.m_squadController.createSquad(ids, tx, ty, 1, entities, pf, ai.getTactics());
            const float dt = 1.0f / 30.0f;
            for(float t = 0.0f; t < maxTime; t += dt)
            {
                entities.update(dt, map, pf);
                ai.m_squadController.update(dt, map, entities, pf, ai.m_chokes, ai.getTactics());
                r.released += static_cast<int>(ai.m_squadController.takeReleasedUnits().size());
                const auto &squads = ai.m_squadController.getSquads();
                if(squads.empty())
                    break;
                if(std::getenv("OAOW_TEST_VERBOSE") && (r.seen.insert(squads.front().state).second || static_cast<int>(t * 30) % 30 == 0))
                {
                    const auto &sq = squads.front();
                    std::cout << "    t=" << t << " " << squadStateName(sq.state) << " n=" << sq.unitIds.size() << " c=(" << sq.centroidX << "," << sq.centroidY << ") dmax=" << sq.maxSpread << " march=" << sq.marchIndex << "/" << sq.route.size() << " leader=" << sq.leaderId << std::endl;
                }
                r.seen.insert(squads.front().state);
                if(t < 4.0f)
                {
                    for(int id : vanguard)
                    {
                        if(const Unit *u = entities.getUnit(id))
                            r.vanguardMaxXEarly = std::max(r.vanguardMaxXEarly, u->getWorldPixelX() / TILE_WIDTH);
                    }
                }
                if(squads.front().state == AISquadState::Assault)
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
        ai.m_chokes.rebuild(map);
        if(!ai.m_chokes.isChoke(20, 14) || !ai.m_chokes.isChoke(20, 15) || ai.m_chokes.isChoke(10, 10) || ai.m_chokes.isChoke(20, 5))
        {
            return fail("chokepoint map must mark exactly the 2-tile gap in the wall");
        }
        std::cout << "  - Chokepoint detection marks the 2-tile wall gap only." << std::endl;

        // D) Corridor transit: Normal gathers in front of the gap, crosses single-file and deploys
        for(int level : {1, 0, 2})
        {
            withDifficulty(level);
            setupArena();
            buildWallWithGap();
            std::vector<int> ids;
            for(int i = 0; i < 8; ++i)
                ids.push_back(entities.spawnUnitByArchetype(1, 6 + i % 4, 12 + i / 4, Faction::Confederation));
            SquadRun r = runSquad(ids, 34, 14, 120.0f, {});
            int crossed = 0;
            for(int id : ids)
            {
                if(const Unit *u = entities.getUnit(id); u && u->alive && u->tileX >= 21)
                    ++crossed;
            }
            bool approached = r.seen.count(AISquadState::ChokeApproach) > 0;
            static const char *kNames[] = {"Easy", "Normal", "Hard"};
            std::cout << "  - " << kNames[level] << " corridor transit: crossed " << crossed << "/8, released " << r.released << ", pre-choke gather=" << (approached ? "yes" : "no") << ", column=" << (r.seen.count(AISquadState::ChokeTransit) ? "yes" : "no")
                      << ", reached objective=" << (r.assault ? "yes" : "no") << std::endl;
            if(!r.assault || crossed < 7 || r.seen.count(AISquadState::ChokeTransit) == 0)
            {
                return fail("group failed to pass the chokepoint as a column");
            }
            if((level >= 1) != approached)
                return fail("only Normal+ must gather in front of the chokepoint");
        }

        // D2) Big column through a 3-tile corridor: the leash scales with group size and passage width,
        //     so 16 units do not keep stopping because the column is longer than 3 tiles
        {
            withDifficulty(1);
            setupArena();
            for(int x = 14; x <= 26; ++x)
            {
                for(int y = 0; y < 30; ++y)
                {
                    if(y < 13 || y > 15)
                        map.setPassable(x, y, false);
                }
            }
            std::vector<int> column;
            for(int i = 0; i < 16; ++i)
                column.push_back(entities.spawnUnitByArchetype(1, 4 + i % 4, 11 + i / 4, Faction::Confederation));
            ai.m_chokes.rebuild(map);
            ai.m_squadController.createSquad(column, 34, 14, 1, entities, pf, ai.getTactics());
            const float dt = 1.0f / 30.0f;
            int ticks = 0, regroupTicks = 0, regroupEntries = 0;
            bool reached = false;
            AISquadState prev = AISquadState::Marching;
            for(float t = 0.0f; t < 120.0f; t += dt)
            {
                entities.update(dt, map, pf);
                ai.m_squadController.update(dt, map, entities, pf, ai.m_chokes, ai.getTactics());
                ai.m_squadController.takeReleasedUnits();
                const auto &squads = ai.m_squadController.getSquads();
                if(squads.empty())
                    break;
                AISquadState st = squads.front().state;
                if(std::getenv("OAOW_TEST_VERBOSE") && (st != prev || ticks % 60 == 0))
                {
                    const auto &g = squads.front();
                    const Unit *l = entities.getUnit(g.leaderId);
                    std::cout << "    t=" << ticks / 30.0f << " " << squadStateName(st) << " n=" << g.unitIds.size() << " dmax=" << g.maxSpread << " leash=" << g.leashRadius << " leader=(" << (l ? l->tileX : -1) << "," << (l ? l->tileY : -1) << ") march=" << g.marchIndex << "/" << g.route.size()
                              << std::endl;
                }
                ++ticks;
                if(st == AISquadState::Regrouping)
                    ++regroupTicks;
                if(st == AISquadState::Regrouping && prev != AISquadState::Regrouping)
                    ++regroupEntries;
                prev = st;
                if(st == AISquadState::Assault)
                {
                    reached = true;
                    break;
                }
            }
            float share = ticks ? static_cast<float>(regroupTicks) / ticks : 1.0f;
            std::cout << "  - 16-unit column through a 3-tile corridor: reached=" << (reached ? "yes" : "no") << ", time " << ticks / 30.0f << "s, stops " << regroupEntries << ", waiting " << static_cast<int>(share * 100) << "%" << std::endl;
            if(!reached || share > 0.25f)
                return fail("a long column must not keep stopping in a corridor");
        }

        // D3) Player control group (Ctrl+N then a move order): mixed infantry + tanks march as one squad with plain
        //     moves, the column stays within the leash and dissolves at the destination
        {
            setupArena();
            for(int x = 14; x <= 26; ++x)
            {
                for(int y = 0; y < 30; ++y)
                {
                    if(y < 13 || y > 15)
                        map.setPassable(x, y, false);
                }
            }
            std::vector<int> group;
            for(int i = 0; i < 12; ++i)
            {
                int arch = (i % 4 == 3) ? 8 : 0; // every 4th unit an Armadillo tank
                group.push_back(entities.spawnUnitByArchetype(arch, 4 + i % 4, 11 + i / 4, Faction::Resistance));
            }
            ai.m_chokes.rebuild(map);
            AITacticsConfig pt;
            pt.syncToSlowest = false;
            pt.meleeRangedPairs = false;
            pt.retreatLossRatio = 0.0f;
            AISquadController player;
            int sid = player.createSquad(group, 34, 14, 0, entities, pf, pt);
            AISquad *sq = player.findSquad(sid);
            sq->attackMove = false;
            sq->playerSquad = true;
            const float dt = 1.0f / 30.0f;
            float t = 0.0f;
            int regroupTicks = 0, ticks = 0;
            for(; t < 120.0f; t += dt)
            {
                entities.update(dt, map, pf);
                player.update(dt, map, entities, pf, ai.m_chokes, pt);
                player.takeReleasedUnits();
                if(player.getSquads().empty())
                    break;
                ++ticks;
                if(player.getSquads().front().state == AISquadState::Regrouping)
                    ++regroupTicks;
                if(std::getenv("OAOW_TEST_VERBOSE") && ticks % 30 == 0)
                {
                    const auto &g = player.getSquads().front();
                    const Unit *l = entities.getUnit(g.leaderId);
                    std::cout << "    t=" << ticks / 30 << " " << squadStateName(g.state) << " n=" << g.unitIds.size() << " dmax=" << g.maxSpread << " leader=" << (l ? l->archetypeId : -1) << "(" << (l ? l->tileX : -1) << "," << (l ? l->tileY : -1) << ") st=" << (l ? static_cast<int>(l->state) : -1)
                              << " march=" << g.marchIndex << "/" << g.route.size() << std::endl;
                }
            }
            for(int k = 0; k < 150; ++k)
                entities.update(dt, map, pf); // released units finish the last move
            int arrived = 0;
            for(int id : group)
            {
                const Unit *u = entities.getUnit(id);
                if(u && std::hypot(u->tileX - 34.0f, u->tileY - 14.0f) <= 5.0f)
                    ++arrived;
            }
            std::cout << "  - Player group (12 mixed) through a corridor: dissolved after " << t << "s, arrived " << arrived << "/12, waiting " << (ticks ? regroupTicks * 100 / ticks : 0) << "%" << std::endl;
            if(!player.getSquads().empty())
                return fail("player group never reached its destination");
            if(arrived < 10)
                return fail("player group did not arrive together");
        }

        // D3b) A member that could only reach the group by a huge detour (long wall, opening at the far end)
        //      must not run across the map while the group gathers
        {
            entities.clear();
            map.initBlankArena(70, 40, 0);
            pf.setMap(&map);
            pf.setEntityManager(&entities);
            for(int x = 0; x <= 62; ++x)
                map.setPassable(x, 15, false); // opening only at x 63..69
            std::vector<int> group;
            for(int i = 0; i < 5; ++i)
                group.push_back(entities.spawnUnitByArchetype(0, 8 + i, 13, Faction::Resistance));
            const int cutOff = entities.spawnUnitByArchetype(0, 10, 16, Faction::Resistance);
            group.push_back(cutOff);
            ai.m_chokes.rebuild(map);
            AISquadController player;
            AITacticsConfig pt;
            int sid = player.createSquad(group, 10, 6, 0, entities, pf, pt);
            player.findSquad(sid)->attackMove = false;
            player.findSquad(sid)->playerSquad = true;
            int maxX = 0;
            const float dt = 1.0f / 30.0f;
            for(float t = 0.0f; t < 20.0f; t += dt)
            {
                entities.update(dt, map, pf);
                player.update(dt, map, entities, pf, ai.m_chokes, pt);
                player.takeReleasedUnits();
                if(const Unit *u = entities.getUnit(cutOff))
                    maxX = std::max(maxX, u->tileX);
            }
            std::cout << "  - Cut-off member (wall opening 53 tiles away): farthest x " << maxX << std::endl;
            if(maxX > 20)
                return fail("a group member must not take a map-wide detour to its slot");
        }

        // D3e) Paths are straight runs, not staircases (vehicles stop to turn before every changed step)
        {
            setupArena();
            auto turns = [&](int unitId, int sx, int sy, int tx, int ty, bool diag, bool &connected)
            {
                std::vector<PathNode> path;
                bool partial = false;
                pf.findPath(sx, sy, tx, ty, path, unitId, &partial, diag);
                int n = 0;
                connected = !path.empty() && path.back().x == tx && path.back().y == ty;
                PathNode prev {sx, sy}, lastStep {0, 0};
                for(size_t i = 0; i < path.size(); ++i)
                {
                    PathNode step {path[i].x - prev.x, path[i].y - prev.y};
                    if(std::max(std::abs(step.x), std::abs(step.y)) != 1)
                        connected = false;
                    if(!diag && step.x != 0 && step.y != 0)
                        connected = false;
                    if(i > 0 && !(step == lastStep))
                        ++n;
                    lastStep = step;
                    prev = path[i];
                }
                return n;
            };
            const int fortress = entities.spawnUnitByArchetype(7, 3, 3, Faction::Confederation);
            const int tankId = entities.spawnUnitByArchetype(8, 3, 20, Faction::Resistance);
            bool fOk = false, tOk = false;
            const int fTurns = turns(fortress, 3, 3, 30, 14, true, fOk);
            const int tTurns = turns(tankId, 3, 20, 30, 26, true, tOk);
            std::cout << "  - Straight paths: Fortress " << fTurns << " turn(s), tank " << tTurns << " turn(s), valid " << (fOk && tOk ? "yes" : "no") << std::endl;
            if(!fOk || !tOk || fTurns > 2 || tTurns > 2)
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
            entities.loadCharacterYaml(fixedDoc);
            entities.loadCharacterYaml(randomDoc);
            setupArena();
            Unit *a = entities.getUnit(entities.spawnUnitByArchetype(0, 5, 5, Faction::Resistance));
            Unit *b = entities.getUnit(entities.spawnUnitByArchetype(2, 7, 5, Faction::Resistance));
            float fixedMin = 99, fixedMax = 0, randMin = 99, randMax = 0;
            for(int i = 0; i < 200; ++i)
            {
                const float fa = a->nextCooldown(), fb = b->nextCooldown();
                fixedMin = std::min(fixedMin, fa);
                fixedMax = std::max(fixedMax, fa);
                randMin = std::min(randMin, fb);
                randMax = std::max(randMax, fb);
            }
            std::cout << "  - Firing delay: fixed " << fixedMin << ".." << fixedMax << ", random " << randMin << ".." << randMax << std::endl;
            app.m_entities->loadCharacterConfigs("assets"); // back to the real data
            if(fixedMin != fixedMax || std::abs(fixedMin - 0.8f) > 0.001f)
                return fail("fixed firing delay must not vary");
            if(randMin < 1.0f || randMax > 2.0f || randMax - randMin < 0.5f)
                return fail("random firing delay must spread over [min, max]");
        }

        // D3e2) Vehicles in a group never turn around to sidestep a neighbour (that read as "strange diagonal moves")
        {
            int reversals = 0;
            for(int arch : {7, 8})
            {
                setupArena();
                std::vector<int> ids;
                for(int i = 0; i < 6; ++i)
                    ids.push_back(entities.spawnUnitByArchetype(arch, 5 + i % 3, 5 + i / 3, Faction::Resistance));
                std::vector<int> lastFacing;
                for(int id : ids)
                    lastFacing.push_back(entities.getUnit(id)->facingDirection);
                entities.issueMoveOrder(ids, 15, 15, pf, false);
                std::vector<int> start(ids.size());
                for(int i = 0; i < 30 * 15; ++i)
                {
                    entities.update(1.0f / 30.0f, map, pf);
                    for(size_t k = 0; k < ids.size(); ++k)
                    {
                        const Unit *u = entities.getUnit(ids[k]);
                        if(!u)
                            continue;
                        // a reversal: the hull ends up pointing opposite to where it pointed a moment ago
                        if(i % 15 == 0)
                        {
                            int d = std::abs(u->facingDirection - start[k]) % 8;
                            if(i > 0 && std::min(d, 8 - d) == 4)
                                ++reversals;
                            start[k] = u->facingDirection;
                        }
                    }
                }
            }
            std::cout << "  - Group of 6 Fortresses / 6 tanks to a diagonal point: hull reversals " << reversals << std::endl;
            if(reversals > 1)
                return fail("vehicles must not turn around to sidestep their neighbours");
        }

        // D3e3) Turning around: every vehicle swings through "down" (south) - left -> right goes W, SW, S, SE, E
        {
            bool allDown = true;
            for(int arch : {6, 7, 8, 9, 10, 11, 12, 13})
            {
                setupArena();
                const int id = entities.spawnUnitByArchetype(arch, 10, 10, Faction::Resistance);
                Unit *u = entities.getUnit(id);
                u->facingDirection = 4; // moving left
                u->turretFacing = 4;
                entities.issueMoveOrder({id}, 16, 10, pf, false);
                std::vector<int> seen {4};
                for(int i = 0; i < 90 && u->facingDirection != 0; ++i)
                {
                    entities.update(1.0f / 30.0f, map, pf);
                    if(u->facingDirection != seen.back())
                        seen.push_back(u->facingDirection);
                }
                const bool down = (seen == std::vector<int> {4, 3, 2, 1, 0});
                if(!down)
                {
                    std::cout << "    archetype " << arch << " turned:";
                    for(int f : seen)
                        std::cout << " " << f;
                    std::cout << std::endl;
                }
                allDown = allDown && down;
            }
            std::cout << "  - Vehicles turning left -> right swing through south: " << (allDown ? "all" : "NOT all") << std::endl;
            if(!allDown)
                return fail("a vehicle reversing must turn through south (down)");
            const bool rule = Utility::turnStep8(0, 4) == 1 && Utility::turnStep8(7, 3) == 1 && Utility::turnStep8(3, 7) == -1 && Utility::turnStep8(5, 1) == -1 && Utility::turnStep8(0, 1) == 1 && Utility::turnStep8(1, 0) == -1;
            if(!rule)
                return fail("turnStep8 half-turn rule broken");
        }

        // D3f) A tank with a rotating turret shoots while driving past an enemy, without stopping
        {
            setupArena();
            const int tankId = entities.spawnUnitByArchetype(8, 3, 10, Faction::Resistance); // Armadillo
            const int dummy = entities.spawnUnitByArchetype(1, 15, 13, Faction::Confederation);
            Unit *t = entities.getUnit(tankId);
            const bool turret = t->hasRotatingTurret;
            Unit *d = entities.getUnit(dummy);
            d->moveSpeed = 0.0f; // stays put
            d->attackDamage = 0; // and does not shoot back
            const int hpBefore = d->currentHp;
            entities.issueMoveOrder({tankId}, 30, 10, pf, false);
            int stops = 0, turretAway = 0;
            for(int i = 0; i < 30 * 20; ++i)
            {
                entities.update(1.0f / 30.0f, map, pf);
                const Unit *u = entities.getUnit(tankId);
                if(!u)
                    break;
                if(u->tileX < 28 && u->state != ActionState::Move)
                    ++stops;
                if(u->state == ActionState::Move && u->turretFacing != u->facingDirection)
                    ++turretAway;
                if(u->tileX >= 30)
                    break;
            }
            const Entity *de = entities.getEntity(dummy);
            const int hpAfter = de ? de->currentHp : 0;
            std::cout << "  - Fire on the move: turret=" << turret << ", enemy hp " << hpBefore << " -> " << hpAfter << ", ticks stopped " << stops << ", ticks turret aimed off the hull " << turretAway << std::endl;
            if(turret && (hpAfter >= hpBefore || stops > 15))
                return fail("a turret vehicle must shoot while driving on");
        }

        // D3g) Auto siege: deploy only for a visible enemy beyond normal range but within siege range
        //      (AI always, player only on attack-move [Q]); auto-deployed units pack up when idle
        {
            auto siegeRun = [&](int arch, Faction side, bool attackMove, bool unlock, float enemyDist, float seconds, bool removeEnemyAfter)
            {
                setupArena();
                entities.setSiegeUnlocked(Players::ownerOf(side), unlock);
                const int unitId = entities.spawnUnitByArchetype(arch, 5, 15, side);
                const Faction foe = (side == Faction::Resistance) ? Faction::Confederation : Faction::Resistance;
                const int enemy = entities.spawnUnitByArchetype(foe == Faction::Resistance ? 0 : 1, 5 + static_cast<int>(enemyDist), 15, foe);
                Unit *e = entities.getUnit(enemy);
                e->moveSpeed = 0.0f;
                e->attackDamage = 0;
                // a spotter of the unit's side next to the enemy, so it is visible to that side
                const int spotter = entities.spawnUnitByArchetype(side == Faction::Resistance ? 0 : 1, 5 + static_cast<int>(enemyDist), 21, side);
                entities.getUnit(spotter)->attackDamage = 0;
                entities.getUnit(spotter)->moveSpeed = 0.0f;
                if(attackMove)
                    entities.issueAttackMoveOrder({unitId}, 6, 15, pf);
                bool deployed = false;
                for(int i = 0; i < static_cast<int>(seconds * 30); ++i)
                {
                    entities.update(1.0f / 30.0f, map, pf);
                    const Unit *u = entities.getUnit(unitId);
                    if(u && u->isSiegeMode)
                        deployed = true;
                }
                bool packed = false;
                if(removeEnemyAfter)
                {
                    if(Unit *en = entities.getUnit(enemy))
                        en->state = ActionState::Die;
                    for(int i = 0; i < 30 * 8; ++i)
                        entities.update(1.0f / 30.0f, map, pf);
                    const Unit *u = entities.getUnit(unitId);
                    packed = u && !u->isSiegeMode;
                }
                entities.setSiegeUnlocked(Players::ownerOf(side), false);
                return std::make_pair(deployed, packed);
            };
            const Faction ai = aiFaction();
            const int aiGrad = (ai == Faction::Confederation) ? 13 : 12;        // Porcupine has no siege: only test the Grad side
            auto aiFar = siegeRun(aiGrad, ai, false, true, 12.0f, 4.0f, true);  // 12 tiles: Grad 8 < 12 <= 50
            auto aiNear = siegeRun(aiGrad, ai, false, true, 5.0f, 4.0f, false); // in normal range: shoots normally
            auto rhinoLocked = siegeRun(10, Faction::Resistance, true, false, 8.0f, 4.0f, false);
            auto rhinoQ = siegeRun(10, Faction::Resistance, true, true, 8.0f, 4.0f, false);
            auto rhinoPlain = siegeRun(10, Faction::Resistance, false, true, 8.0f, 4.0f, false);
            std::cout << "  - Auto siege: AI far deploy " << aiFar.first << " / pack up " << aiFar.second << ", AI near deploy " << aiNear.first << ", Rhino locked " << rhinoLocked.first << ", Rhino Q " << rhinoQ.first << ", Rhino plain " << rhinoPlain.first << std::endl;
            if(ai == Faction::Confederation && (!aiFar.first || !aiFar.second || aiNear.first))
                return fail("AI auto siege broken");
            if(rhinoLocked.first || !rhinoQ.first || rhinoPlain.first)
                return fail("player auto siege rules broken");
        }

        // D3d) Enemy units standing across a passage are not a wall: no long detour, the unit fights through
        {
            entities.clear();
            map.initBlankArena(60, 40, 0);
            pf.setMap(&map);
            pf.setEntityManager(&entities);
            // Wall x=30 with a 3-tile passage at y 14..16 and a far opening at y 37..39
            for(int y = 0; y < 37; ++y)
            {
                if(y < 14 || y > 16)
                    map.setPassable(30, y, false);
            }
            std::vector<int> blockers;
            for(int y = 14; y <= 16; ++y)
                blockers.push_back(entities.spawnUnitByArchetype(1, 30, y, Faction::Confederation));
            const int tank = entities.spawnUnitByArchetype(8, 22, 15, Faction::Resistance); // Armadillo
            entities.issueMoveOrder({tank}, 40, 15, pf, false);
            int maxY = 0;
            for(int i = 0; i < 30 * 40; ++i)
            {
                entities.update(1.0f / 30.0f, map, pf);
                if(const Unit *u = entities.getUnit(tank))
                    maxY = std::max(maxY, u->tileY);
            }
            int alive = 0;
            for(int id : blockers)
                alive += entities.getEntity(id) ? 1 : 0;
            const Unit *u = entities.getUnit(tank);
            std::cout << "  - Enemy barricade in a passage: deepest detour y " << maxY << ", blockers left " << alive << "/3, tank at (" << (u ? u->tileX : -1) << "," << (u ? u->tileY : -1) << ")" << std::endl;
            if(maxY > 20)
                return fail("units must not walk around an enemy barricade");
        }

        // D3c) Fog: an enemy firing from the dark at our unit shows up where it stands, only for a moment
        {
            setupArena();
            map.setFogEnabled(true);
            map.resetAllFog(FogState::Hidden);
            const int victim = entities.spawnUnitByArchetype(0, 5, 15, playerFaction());
            const int shooter = entities.spawnUnitByArchetype(playerFaction() == Faction::Resistance ? 1 : 0, 25, 15, aiFaction());
            entities.updateFogOfWar(map);
            const bool hiddenBefore = map.getFogState(25, 15) != FogState::Visible;
            const Entity *v = entities.getEntity(victim);
            entities.noteHit(v->owner, shooter);
            entities.updateFogOfWar(map);
            const bool shownAfterHit = map.getFogState(25, 15) == FogState::Visible;
            // only the shooter's own tile, not a circle around it
            const bool surroundingsHidden = map.getFogState(26, 15) != FogState::Visible && map.getFogState(25, 16) != FogState::Visible;
            for(int i = 0; i < 100; ++i)
                entities.update(0.05f, map, pf); // 5 s without further hits
            const bool goneLater = map.getFogState(25, 15) != FogState::Visible;
            std::cout << "  - Fog muzzle flash: hidden " << hiddenBefore << ", shown on hit " << shownAfterHit << ", only its spot " << surroundingsHidden << ", hidden again " << goneLater << std::endl;
            map.setFogEnabled(false);
            if(!hiddenBefore || !shownAfterHit || !surroundingsHidden || !goneLater)
                return fail("attacker reveal in fog broken");
        }

        // D4) A control group marches with auto-attack: it shoots enemies met on the way and still arrives;
        //     the group bar survives members dying in the same frame
        {
            setupArena();
            auto &gc = *app.m_gameplayController;
            std::vector<int> group;
            for(int i = 0; i < 6; ++i)
                group.push_back(entities.spawnUnitByArchetype(0, 4 + i % 3, 12 + i / 3, Faction::Resistance));
            std::vector<int> enemies;
            for(int i = 0; i < 2; ++i)
                enemies.push_back(entities.spawnUnitByArchetype(1, 20, 9 + i, Faction::Confederation)); // beside the road
            for(auto &g : gc.m_groups)
                g = ControlGroup {};
            Simulation &sim = gc.m_sim;
            sim.reset();
            gc.m_groups[0].unitIds = group;
            gc.pruneGroups(entities);
            ai.m_chokes.rebuild(map);
            // The order goes the way of the game: an attack-move command for a control group, applied by the simulation
            SimCommand order;
            order.type = CommandType::OrderAt;
            order.player = Players::ownerOf(Faction::Resistance);
            order.ids.assign(group.begin(), group.end());
            order.x = 34 * TILE_WIDTH + TILE_WIDTH / 2;
            order.y = 13 * TILE_HEIGHT + TILE_HEIGHT / 2;
            order.a = static_cast<int32_t>(OrderKind::AttackMove);
            order.b = 1;
            sim.issue(order);
            SimContext ctx {entities, map, pf, *app.m_economy, ai};
            sim.applyCommands(ctx);
            AISquadController &squads = sim.squads(Players::ownerOf(Faction::Resistance));
            if(squads.getSquads().empty())
                return fail("Ctrl+1 group must march as a squad");
            const float dt = 1.0f / 30.0f;
            float t = 0.0f;
            for(; t < 90.0f && !squads.getSquads().empty(); t += dt)
            {
                entities.update(dt, map, pf);
                sim.updateSquads(dt, ctx, ai.m_chokes);
            }
            int enemiesLeft = 0;
            for(int id : enemies)
                enemiesLeft += entities.getEntity(id) ? 1 : 0;
            std::cout << "  - Player group attack-march: arrived after " << t << "s, enemies on the way left " << enemiesLeft << "/2" << std::endl;
            if(!squads.getSquads().empty())
                return fail("the group must reach its destination");
            if(enemiesLeft == 2)
                return fail("a marching group must fight the enemies it meets");

            // A member dies after the groups were pruned: drawing the bar must not touch it
            entities.getUnit(group[1])->state = ActionState::Die;
            gc.renderGroupBar(*app.m_renderer, *app.m_fontRenderer, entities, 900, 700, 0, 0);
            gc.renderGroupLeaders(*app.m_renderer, *app.m_camera, entities);
            for(auto &g : gc.m_groups)
                g = ControlGroup {};
            std::cout << "  - Group bar with a member dying this frame: OK" << std::endl;
        }

        // E) Cohesion: one straggler does not stop the group (it is paced, not halted); a stuck straggler is
        //    dropped after the timeout; a group that fell apart (half of it behind) waits for the rest
        withDifficulty(1);
        setupArena();
        for(int dx = -1; dx <= 1; ++dx)
        {
            for(int dy = -1; dy <= 1; ++dy)
            {
                if(dx != 0 || dy != 0)
                    map.setPassable(4 + dx, 25 + dy, false); // sealed pocket
            }
        }
        std::vector<int> vanguard;
        for(int i = 0; i < 5; ++i)
            vanguard.push_back(entities.spawnUnitByArchetype(1, 6 + i % 3, 10 + i / 3, Faction::Confederation));
        int straggler = entities.spawnUnitByArchetype(1, 4, 25, Faction::Confederation);
        std::vector<int> all = vanguard;
        all.push_back(straggler);
        SquadRun r = runSquad(all, 34, 10, 90.0f, vanguard);
        std::cout << "  - Straggler: vanguard went on to x=" << r.vanguardMaxXEarly << " in 4 s, regrouped " << (r.seen.count(AISquadState::Regrouping) ? "yes" : "no") << ", released " << r.released << ", reached objective=" << (r.assault ? "yes" : "no") << std::endl;
        if(r.seen.count(AISquadState::Regrouping) != 0 || r.vanguardMaxXEarly <= 9.0f)
        {
            return fail("one straggler must not stop the whole group");
        }
        if(r.released < 1 || !r.assault)
            return fail("stuck straggler must be dropped and the group must move on");

        {
            withDifficulty(1);
            setupArena();
            std::vector<int> front, group;
            for(int i = 0; i < 3; ++i)
                front.push_back(entities.spawnUnitByArchetype(1, 6 + i, 10, Faction::Confederation));
            group = front;
            for(int i = 0; i < 3; ++i)
                group.push_back(entities.spawnUnitByArchetype(1, 4 + i, 25, Faction::Confederation));
            SquadRun half = runSquad(group, 34, 10, 90.0f, front);
            std::cout << "  - Half of the group behind: regrouped " << (half.seen.count(AISquadState::Regrouping) ? "yes" : "no") << ", vanguard at x<=" << half.vanguardMaxXEarly << " in 4 s, reached objective=" << (half.assault ? "yes" : "no") << std::endl;
            if(half.seen.count(AISquadState::Regrouping) == 0 || half.vanguardMaxXEarly > 12.0f || !half.assault)
            {
                return fail("a group that fell apart must wait for the rest, then go on");
            }
        }

        // H) Enemy walls: detour when there is a way around, breach only a sealed line
        for(bool sealed : {false, true})
        {
            withDifficulty(1);
            setupArena();
            for(int y = 0; y < 30; ++y)
            {
                if(sealed || y < 3 || y > 4)
                    map.setWall(20, y, Faction::Resistance, 100);
            }
            std::vector<int> column;
            for(int i = 0; i < 6; ++i)
                column.push_back(entities.spawnUnitByArchetype(1, 12 + i % 3, 18 + i / 3, Faction::Confederation));
            entities.issueAttackMoveOrder(column, 30, 19, pf);
            bool shotWall = false;
            int crossed = 0;
            const float dt = 1.0f / 30.0f;
            for(float t = 0.0f; t < 60.0f; t += dt)
            {
                entities.update(dt, map, pf);
                for(int id : column)
                {
                    if(const Unit *u = entities.getUnit(id); u && u->targetWallTileX >= 0)
                        shotWall = true;
                }
            }
            for(int id : column)
            {
                if(const Unit *u = entities.getUnit(id); u && u->alive && u->tileX > 20)
                    ++crossed;
            }
            std::cout << "  - Wall " << (sealed ? "sealed" : "with gap") << ": wall targeted=" << (shotWall ? "yes" : "no") << ", crossed " << crossed << "/6" << std::endl;
            if(!sealed && (shotWall || crossed < 5))
                return fail("units must walk around a wall that has a gap");
            if(sealed && !shotWall)
                return fail("units must breach a sealed enemy wall");
        }

        // G) Hard: a wave that lost >60% without hurting the player falls back to the staging area
        withDifficulty(2);
        setupArena();
        entities.spawnBuildingByIndex(8, 32, 20, Faction::Resistance, 1.0f);
        ai.m_squadController.setStagingPoint(4, 4);
        std::vector<int> doomed;
        for(int i = 0; i < 5; ++i)
            doomed.push_back(entities.spawnUnitByArchetype(1, 6 + i, 10, Faction::Confederation));
        ai.m_chokes.rebuild(map);
        ai.m_squadController.createSquad(doomed, 33, 21, 1, entities, pf, ai.getTactics());
        for(int i = 0; i < 4; ++i)
            ai.m_squadController.update(0.25f, map, entities, pf, ai.m_chokes, ai.getTactics());
        for(int i = 0; i < 4; ++i)
            entities.getUnit(doomed[i])->state = ActionState::Die;
        ai.m_squadController.update(0.25f, map, entities, pf, ai.m_chokes, ai.getTactics());
        const auto &groups = ai.m_squadController.getSquads();
        if(groups.empty() || groups.front().state != AISquadState::Retreating)
        {
            return fail("Hard AI must pull back a wave that died without damaging the player");
        }
        std::cout << "  - Hard retreat: 4/5 lost without effect -> survivors fall back to staging." << std::endl;

        // F) Research Lab: technologies are researched in order, bonuses reach existing troops
        withDifficulty(2);
        setupArena();
        entities.spawnBuildingByIndex(4, 4, 4, Faction::Confederation, 1.0f);
        int trooper = entities.spawnUnitByArchetype(1, 12, 12, Faction::Confederation);
        int hpBefore = entities.getUnit(trooper)->maxHp;
        ai.init(5, 5, 35, 25);
        ai.m_credits = 10000;
        int lastIndex = 0;
        for(int t = 0; t < 75; ++t)
        {
            ai.update(1.0f, map, entities, pf);
            if(ai.getResearchIndex() < lastIndex)
                return fail("research order went backwards");
            lastIndex = ai.getResearchIndex();
        }
        const Unit *tr = entities.getUnit(trooper);
        std::cout << "  - Research: " << ai.getResearchIndex() << " technologies done in 75s (Hard), damage to a rifleman 100 -> " << (tr ? reduceDamageByArmor(100, *tr) : -1) << std::endl;
        if(!ai.isTechnologyResearched(0) || !ai.isTechnologyResearched(1) || ai.getResearchIndex() < 2)
        {
            return fail("AI must research the lab technologies one after another from the first");
        }
        (void) hpBefore;
        if(!tr || reduceDamageByArmor(100, *tr) >= 100)
            return fail("Power Suit must make existing AI infantry tougher");

        // The catalogs: 24 technologies per side, own names, requirements point backwards, units behind research
        for(Faction side : {Faction::Resistance, Faction::Confederation})
        {
            for(int i = 0; i < kResearchCount; ++i)
            {
                const ResearchDef &d = researchDef(side, i);
                if(!d.name[0] || d.cost <= 0 || d.needs >= i)
                    return fail("research catalog entry is malformed");
            }
        }
        if(std::string(researchDef(Faction::Confederation, 0).name) == researchDef(Faction::Resistance, 0).name || researchDef(Faction::Confederation, 0).compId == researchDef(Faction::Resistance, 0).compId)
        {
            return fail("both sides must have their own research list");
        }
        resetResearch();
        if(isUnitUnlocked(Players::ownerOf(Faction::Confederation), 5) || isUnitUnlocked(Players::ownerOf(Faction::Resistance), 4) || !isUnitUnlocked(Players::ownerOf(Faction::Confederation), 1) || isRadarUnlocked(Players::ownerOf(Faction::Confederation)) ||
           isResearchAvailable(Players::ownerOf(Faction::Confederation), 1))
        {
            return fail("flamethrowers, snipers and radars must wait for their technologies");
        }
        completeResearch(Players::ownerOf(Faction::Confederation), 0, entities);
        completeResearch(Players::ownerOf(Faction::Confederation), 5, entities);
        completeResearch(Players::ownerOf(Faction::Confederation), 18, entities);
        if(!isUnitUnlocked(Players::ownerOf(Faction::Confederation), 5) || !isRadarUnlocked(Players::ownerOf(Faction::Confederation)) || !isResearchAvailable(Players::ownerOf(Faction::Confederation), 1))
        {
            return fail("completed technologies must unlock their units and requirements");
        }
        std::cout << "  - Research catalogs: 24 technologies per side, unit and radar unlocks, requirements OK." << std::endl;

        // F2) Without a Tech Center research stands still (no HQ fallback)
        setupArena();
        ai.init(5, 5, 35, 25);
        ai.m_credits = 10000;
        entities.spawnBuildingByIndex(0, 4, 4, Faction::Confederation, 1.0f); // HQ only
        for(int t = 0; t < 40; ++t)
            ai.update(1.0f, map, entities, pf);
        std::cout << "  - Research without Tech Center: " << ai.getResearchIndex() << " done, active=" << (ai.isResearchActive() ? "yes" : "no") << std::endl;
        if(ai.getResearchIndex() > 0 || ai.isResearchActive())
            return fail("research must pause without a Tech Center");

        // G) Attack groups: money and difficulty decide how many groups a wave splits into
        {
            setupArena(); // a small map: no extra flank group for map size
            ai.m_gateX[0] = ai.m_gateX[1] = -1;
            withDifficulty(3); // Hardcore: up to 5 groups
            ai.m_credits = 0;
            const int poor = ai.attackGroupCount(30, map);
            ai.m_credits = 5000;
            const int rich = ai.attackGroupCount(30, map);
            const int few = ai.attackGroupCount(4, map);
            const int tenUnits = ai.attackGroupCount(10, map); // a squad is never smaller than five
            withDifficulty(0);                                 // Easy: at most 2
            const int easyRich = ai.attackGroupCount(30, map);
            std::cout << "  - Attack groups: poor " << poor << ", rich " << rich << " (5 max), 4 units -> " << few << ", Easy rich " << easyRich << std::endl;
            if(poor != 1 || rich != 5 || few != 1 || easyRich != 2 || tenUnits > 2)
                return fail("attack group count must follow money / difficulty");
        }

        // H) Skirmish base builder: wall arc toward the player with two exits, defenses in front, lab behind
        {
            withDifficulty(1);
            entities.clear();
            map.initBlankArena(70, 50, 0);
            pf.setMap(&map);
            pf.setEntityManager(&entities);
            ai.m_squadController.clear();
            const Faction side = aiFaction();
            const bool conf = (side == Faction::Confederation);
            entities.spawnBuildingByIndex(conf ? 0 : 8, 10, 22, side, 1.0f); // HQ
            entities.spawnBuildingByIndex(conf ? 2 : 10, 6, 28, side, 1.0f); // barracks
            entities.spawnBuildingByIndex(playerFaction() == Faction::Confederation ? 0 : 8, 60, 20, playerFaction(), 1.0f);
            ai.init(11, 23, 61, 21);
            ai.setSkirmish(true);
            ai.recordStartingBase(entities, map);
            ai.m_credits = 20000;
            for(int t = 0; t < 240; ++t)
            {
                ai.updateBaseBuilder(1.0f, map, entities, pf);
                entities.update(1.0f, map, pf);
            }
            int walls = 0, towers = 0, labBehind = 0;
            float towerX = 0.0f;
            for(int y = 0; y < map.getHeight(); ++y)
            {
                for(int x = 0; x < map.getWidth(); ++x)
                    walls += (map.getWall(x, y) > 0 && map.getWallFaction(x, y) == side) ? 1 : 0;
            }
            for(const auto &e : entities.getAllEntities())
            {
                const Building *b = e.asBuilding();
                if(!b || b->faction != side)
                    continue;
                if(b->type == UnitType::Tower || b->type == UnitType::Bunker)
                {
                    ++towers;
                    towerX += b->tileX;
                }
                if(b->type == UnitType::ResearchLab && b->tileX < 11)
                    ++labBehind;
            }
            towerX /= std::max(1, towers);
            // Both exits must stay open: a path leads from the HQ door to the player's base
            std::vector<PathNode> out;
            bool partial = false;
            pf.setIgnoreUnitsOnce();
            bool open = pf.findPath(12, 27, 59, 22, out, 0, &partial, true) && !partial;
            std::cout << "  - Skirmish base: " << walls << " wall tiles, " << towers << " towers/bunkers (avg x " << towerX << "), lab behind HQ=" << labBehind << ", exits (" << ai.m_gateX[0] << "," << ai.m_gateY[0] << ") / (" << ai.m_gateX[1] << "," << ai.m_gateY[1]
                      << "), way out=" << (open ? "yes" : "no") << std::endl;
            if(walls < 10)
                return fail("skirmish AI must wall in the side facing the player");
            if(towers < 3 || towerX <= 11.0f)
                return fail("towers and bunkers must stand between the HQ and the player");
            if(labBehind < 1)
                return fail("the lab belongs behind the HQ, away from the player");
            if(ai.m_gateX[1] < 0 || !open)
                return fail("the wall must keep two exits open");
            ai.setSkirmish(false);
        }

        // I) Debug control: treasury edit and the reasoning log
        {
            ai.setEnemyCredits(-50);
            const bool zeroed = ai.getEnemyCredits() == 0;
            ai.setEnemyCredits(12345);
            const size_t thoughts = ai.getThoughts().size();
            const size_t now = ai.currentIntentions().size();
            std::cout << "  - Debug control: credits clamp to 0=" << (zeroed ? "yes" : "no") << ", set 12345 -> " << ai.getEnemyCredits() << ", thoughts logged " << thoughts << ", intentions " << now << std::endl;
            if(!zeroed || ai.getEnemyCredits() != 12345 || thoughts < 3 || now == 0)
                return fail("AI debug control / thoughts broken");
        }

        ai.setDebugConfig(savedDebug);
        ai.reset();
        std::cout << "  - AI behaviour validated." << std::endl;
        return true;
    }

    bool HeadlessTest::run(Application &app, int numTicks)
    {
        if(const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "group")
        {
            return runGroupMovementTest(app);
        }
        if(const char *only = std::getenv("OAOW_TEST_ONLY"); only && std::string(only) == "ai")
        {
            return runAIBehaviourTest(app);
        }
        std::cout << "[Test] Starting automated headless simulation test (" << numTicks << " ticks)..." << std::endl;

        // 0. Validate Localization system (gettext, EN default, RU toggle)
        std::cout << "[Test] Validating Localization system (gettext)..." << std::endl;
        if(Localization::getLanguage() != "en")
        {
            std::cerr << "[Test] Error: Default language is not 'en' (got: " << Localization::getLanguage() << ")!" << std::endl;
            return false;
        }
        std::string enStr = _("MAIN MENU");
        if(enStr != "Main Menu")
        {
            std::cerr << "[Test] Error: English string unexpected: " << enStr << std::endl;
            return false;
        }
        Localization::setLanguage("ru");
        std::string ruStr = _("MAIN MENU");
        std::cout << "[Test] Russian string for 'MAIN MENU' is: '" << ruStr << "'" << std::endl;
        if(ruStr != "ГЛАВНОЕ МЕНЮ")
        {
            std::cerr << "[Test] Error: Russian translation failed, got: " << ruStr << std::endl;
            return false;
        }
        Localization::setLanguage("en");
        std::cout << "  - Localization validated: default EN (\"" << enStr << "\") -> RU (\"" << ruStr << "\") -> restored EN." << std::endl;

        // 0.5. Validate YAML Character Configs (yaml-cpp)
        std::cout << "[Test] Validating YAML Character Configuration loading (yaml-cpp)..." << std::endl;
        const auto *u0 = app.m_entities->getUnitConfig(0); // Rifleman Resistance
        // Descriptions come from the translation catalogue by id, not from the YAML file
        if(!u0 || u0->id != "rifleman_res" || u0->description != "rifleman_res.description" || Localization::tr(u0->description) == u0->description || u0->maxHp != 30)
        {
            std::cerr << "[Test] Error: Unit config 0 (rifleman_res) invalid or failed to load from YAML!" << std::endl;
            return false;
        }
        const auto *b8 = app.m_entities->getBuildingConfig(8); // Headquarters Resistance
        if(!b8 || b8->id != "headquarters_res" || b8->description.empty() || b8->width != 3 || b8->height != 4)
        {
            std::cerr << "[Test] Error: Building config 8 (headquarters_res) invalid or failed to load from YAML!" << std::endl;
            return false;
        }
        const auto &bps = EntityManager::getBlueprints();
        if(bps.size() != 8 || bps[1].buildingIndex != 9)
        {
            std::cerr << "[Test] Error: Blueprints from YAML invalid! Count=" << bps.size() << std::endl;
            return false;
        }
        std::cout << "  - YAML validated: unit (id=" << u0->id << ", hp=" << u0->maxHp << ", desc=\"" << Localization::tr(u0->description).substr(0, 30) << "...\"), building (id=" << b8->id << ", " << b8->width << "x" << b8->height << "), 8 blueprints loaded." << std::endl;

        // 0.6 Options and the per-user directory
        std::cout << "[Test] Validating Settings, the user directory and the zoom / quality options..." << std::endl;
        {
            namespace fs = std::filesystem;
            const fs::path userDir = Assets::userDataDir();
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
            Settings loaded;
            if(!custom.save(settingsPath) || !loaded.load(settingsPath) || loaded.windowMode != WindowMode::Exclusive || loaded.windowW != 1600 || loaded.fullscreenH != 1080 || loaded.vsync || loaded.fpsLimit != 60 || !loaded.showFps || loaded.graphicsQuality != 1 || loaded.smoothing ||
               loaded.zoomDefault != 1.5f || loaded.zoomMin != 0.75f || loaded.zoomMax != 3.0f || loaded.musicVolume != 40 || loaded.fxVolume != 70 || !loaded.muted || loaded.language != "ru" || loaded.difficulty != 3)
            {
                std::cerr << "[Test] Error: Settings did not survive a save / load round trip!" << std::endl;
                return false;
            }
            {
                std::ofstream bad(settingsPath);
                bad << R"({"windowWidth": 10, "zoomDefault": 99, "zoomMax": 1, "musicVolume": 900, "language": "xx", "difficulty": 9})";
            }
            Settings clamped;
            if(!clamped.load(settingsPath) || clamped.windowW < Settings::kMinWindowW || clamped.zoomDefault > clamped.zoomMax || clamped.zoomMax < 2.0f || clamped.musicVolume != 128 || clamped.language != "en" || clamped.difficulty != 3)
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
            if(fs::path(moved.path()).parent_path() != userDir || !moved.load(movedData) || movedData.unlockedMissions != 5 || movedData.difficulty != 2)
            {
                std::cerr << "[Test] Error: The save was not placed in / taken over into the user directory!" << std::endl;
                return false;
            }
            fs::remove_all(legacyDir);

            // Zoom range of the camera
            Camera zoomCamera(800, 600);
            zoomCamera.setZoomLimits(1.0f, 3.0f);
            zoomCamera.setZoom(10.0f);
            const float highest = zoomCamera.getZoom();
            zoomCamera.setZoom(0.1f);
            if(highest != 3.0f || zoomCamera.getZoom() != 1.0f)
            {
                std::cerr << "[Test] Error: Camera zoom ignores its limits!" << std::endl;
                return false;
            }

            // Graphics quality sets the resolution of the world (1/3, 1/2, original), zoom limits reach the camera
            std::vector<int> divisors;
            for(int quality = 0; quality < 3; ++quality)
            {
                app.m_settings.graphicsQuality = quality;
                app.applySettings(SettingsArea::Graphics);
                divisors.push_back(app.renderDivisor());
            }
            app.m_settings.zoomMin = 1.0f;
            app.m_settings.zoomMax = 3.0f;
            app.applySettings(SettingsArea::Graphics);
            const bool limitsApplied = app.m_camera->getMinZoom() == 1.0f && app.m_camera->getMaxZoom() == 3.0f;
            app.m_settings.zoomMin = 0.5f;
            app.m_settings.zoomMax = 4.0f;
            app.applySettings(SettingsArea::Graphics);
            const Camera view = app.m_camera->scaledDown(3);
            if(divisors != std::vector<int> {3, 2, 1} || Settings().graphicsQuality != 2 || !limitsApplied || app.m_camera->getMinZoom() != 0.5f || app.m_camera->getMaxZoom() != 4.0f || view.getZoom() >= app.m_camera->getZoom() || !fs::exists(Settings::defaultPath()))
            {
                std::cerr << "[Test] Error: Graphics options were not applied / stored!" << std::endl;
                return false;
            }
            std::cout << "  - Settings round trip + clamping, save in the user directory (+ migration), camera zoom limits and "
                         "graphics quality validated."
                      << std::endl;
        }

        // 1. Verify loading of all 8 campaign missions
        std::cout << "[Test] Validating all 8 campaign missions and authentic base positions..." << std::endl;
        for(int mid = 0; mid < 8; ++mid)
        {
            if(!app.m_mission->loadMission(mid, "assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
            {
                std::cerr << "[Test] Failed to load Mission " << mid << "!" << std::endl;
                return false;
            }
            std::cout << "  - " << app.m_mission->getMissionName() << " (Map " << app.m_map->getWidth() << "x" << app.m_map->getHeight() << ") | Player HQ: (" << app.m_mission->getPlayerHqX() << "," << app.m_mission->getPlayerHqY() << ")"
                      << " | Entities: " << app.m_entities->getAllEntities().size() << std::endl;

            if(mid == 7)
            {
                uint16_t wConf = app.m_map->getWall(12, 24);
                uint16_t wRes = app.m_map->getWall(47, 14);
                if(wConf < 1001 || wRes < 1101)
                {
                    std::cerr << "[Test] Error: Walls on map m0 (mid 7) failed to load properly! wConf=" << wConf << " wRes=" << wRes << std::endl;
                    return false;
                }
                std::cout << "  - Validated defensive walls on Map m0: Confederation wall=" << wConf << ", Resistance wall=" << wRes << std::endl;
            }
        }

        // 1a. Defense missions: the enemy is not standing there at the start, its squads spawn and attack later
        std::cout << "[Test] Validating that defense missions spawn their enemies in waves..." << std::endl;
        for(int mid : {1, 3})
        {
            if(!app.m_mission->loadMission(mid, "assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
            {
                std::cerr << "[Test] Failed to load defense mission " << mid << "!" << std::endl;
                return false;
            }
            auto enemyUnits = [&]()
            {
                int n = 0;
                for(const auto &e : app.m_entities->getAllEntities())
                {
                    if(e.alive && !e.isBuilding && e.faction == aiFaction())
                        ++n;
                }
                return n;
            };
            const int atStart = enemyUnits();
            // Squads come by the script's own timeline, stretched by the difficulty (Easy: the slowest)
            int after60 = -1;
            for(int i = 0; i < 240; ++i)
            {
                app.m_mission->update(1.0f, *app.m_map, *app.m_entities, *app.m_pathfinder);
                if(i == 59)
                    after60 = enemyUnits();
            }
            const int later = enemyUnits();
            std::cout << "  - " << app.m_mission->getMissionName() << ": enemy units at start " << atStart << ", after 60s " << after60 << ", after 240s " << later << std::endl;
            if(atStart != 0 || later == 0 || app.m_mission->isVictory())
            {
                std::cerr << "[Test] Error: defense mission " << mid << " must spawn its enemies in waves!" << std::endl;
                return false;
            }
        }

        // 1b. The Confederation campaign: its own list, own maps, the player commands the Confederation
        std::cout << "[Test] Validating the Confederation campaign missions..." << std::endl;
        setPlayerFaction(Faction::Confederation);
        const auto &confMissions = MissionController::getAvailableMissions();
        if(confMissions.size() != 7 || confMissions[0].name != "Mission 1: Pukalpa")
        {
            std::cerr << "[Test] Error: Confederation campaign list is wrong!" << std::endl;
            setPlayerFaction(Faction::Resistance);
            return false;
        }
        for(int mid = 0; mid < 7; ++mid)
        {
            const bool loaded = app.m_mission->loadMission(mid, "assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder);
            const auto &conf = Missions::confederation()[mid].data;
            bool ownBase = false;
            for(const auto &e : app.m_entities->getAllEntities())
            {
                if(e.alive && e.isBuilding && e.faction == Faction::Confederation)
                    ownBase = true;
            }
            std::cout << "  - " << app.m_mission->getMissionName() << " (" << conf.mapFile << ", Map " << app.m_map->getWidth() << "x" << app.m_map->getHeight() << ") | Player HQ: (" << app.m_mission->getPlayerHqX() << "," << app.m_mission->getPlayerHqY() << ")" << std::endl;
            if(!loaded || app.m_map->getWidth() <= 1 || conf.mapFile.rfind("map_mission_", 0) != 0 || (mid != 3 && mid != 2 && !ownBase && conf.entities.empty()))
            {
                std::cerr << "[Test] Error: Confederation mission " << mid << " failed to load!" << std::endl;
                setPlayerFaction(Faction::Resistance);
                return false;
            }
        }
        setPlayerFaction(Faction::Resistance);

        // Load Mission 0 for full gameplay simulation
        if(!app.m_mission->loadMission(0, "assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
        {
            std::cerr << "[Test] Failed to load Mission 0!" << std::endl;
            return false;
        }
        app.m_aiController->init(app.m_mission->getEnemyHqX(), app.m_mission->getEnemyHqY(), app.m_mission->getPlayerHqX(), app.m_mission->getPlayerHqY());
        app.m_state = AppState::CampaignPlaying;

        // 2. Test Base Construction (Застройка)
        std::cout << "[Test] Testing Base Construction System (Застройка)..." << std::endl;
        const auto &blueprints = EntityManager::getBlueprints();
        if(blueprints.empty())
        {
            std::cerr << "[Test] Error: No building blueprints found!" << std::endl;
            return false;
        }
        const auto &bp = blueprints[1]; // Power Plant
        int testBuildX = app.m_mission->getPlayerHqX() + 6;
        int testBuildY = app.m_mission->getPlayerHqY();
        if(app.m_entities->canPlaceBuilding(testBuildX, testBuildY, bp.width, bp.height, *app.m_map, Players::ownerOf(Faction::Resistance)))
        {
            int initialCredits = app.m_economy->getCredits();
            app.m_economy->spendCredits(bp.creditCost);
            int newBId = app.m_entities->spawnBuildingByIndex(bp.buildingIndex, testBuildX, testBuildY, Faction::Resistance, 0.0f);
            if(newBId > 0)
            {
                std::cout << "  - Successfully placed " << bp.name << " at (" << testBuildX << "," << testBuildY << ")"
                          << " | Deducted $" << bp.creditCost << " (Credits: " << initialCredits << " -> " << app.m_economy->getCredits() << ")" << std::endl;
            }
        }

        // Verify initial units
        const auto &entities = app.m_entities->getAllEntities();
        int playerCount = 0, enemyCount = 0;
        std::vector<int> playerUnits;
        for(const auto &e : entities)
        {
            if(!e.alive)
                continue;
            if(e.faction == Faction::Resistance)
            {
                playerCount++;
                if(!e.isBuilding)
                    playerUnits.push_back(e.id);
            }
            else
            {
                enemyCount++;
            }
        }
        std::cout << "[Test] Active entities: " << playerCount << " player, " << enemyCount << " enemy." << std::endl;

        // Issue Attack-Move order to player units
        if(!playerUnits.empty())
        {
            std::cout << "[Test] Issuing Attack-Move order [A] for " << playerUnits.size() << " units to (25, 20)..." << std::endl;
            app.m_entities->issueAttackMoveOrder(playerUnits, 25, 20, *app.m_pathfinder);
        }

        // Run simulation loop
        float dt = static_cast<float>(SIMULATION_TIMESTEP);
        for(int tick = 0; tick < numTicks; ++tick)
        {
            app.update(dt);
            app.render();

            if(tick == numTicks / 2 && !playerUnits.empty() && enemyCount > 0)
            {
                // Find an enemy entity and issue attack order
                for(const auto &e : app.m_entities->getAllEntities())
                {
                    if(e.alive && e.faction == Faction::Confederation)
                    {
                        std::cout << "[Test] Issuing attack order against enemy unit #" << e.id << "..." << std::endl;
                        app.m_entities->issueAttackOrder(playerUnits, e.id);
                        break;
                    }
                }
            }
        }

        // 3. Test Production Prerequisites & Structure-Specific Spawning
        std::cout << "[Test] Validating Production Prerequisites and Structure Spawning..." << std::endl;
        bool hasBarracksInitially = app.m_entities->hasBuildingType(UnitType::Barracks, Faction::Resistance);
        bool hasFactoryInitially = app.m_entities->hasBuildingType(UnitType::Factory, Faction::Resistance);
        std::cout << "  - Initial structure prerequisites: Barracks=" << (hasBarracksInitially ? "YES" : "NO") << ", Factory=" << (hasFactoryInitially ? "YES" : "NO") << std::endl;

        // Place a Barracks and a Factory
        int bX = app.m_mission->getPlayerHqX() + 6;
        int bY = app.m_mission->getPlayerHqY() + 6;
        int barracksId = app.m_entities->spawnBuilding(UnitType::Barracks, bX, bY, Faction::Resistance);
        int fX = app.m_mission->getPlayerHqX() + 12;
        int fY = app.m_mission->getPlayerHqY() + 6;
        int factoryId = app.m_entities->spawnBuilding(UnitType::Factory, fX, fY, Faction::Resistance);

        if(barracksId <= 0 || factoryId <= 0)
        {
            std::cerr << "[Test] Error: Failed to spawn Barracks or Factory for production test!" << std::endl;
            return false;
        }

        Building *bEnt = app.m_entities->getBuilding(barracksId);
        Building *fEnt = app.m_entities->getBuilding(factoryId);
        if(bEnt)
            bEnt->buildProgress = 1.0f;
        if(fEnt)
            fEnt->buildProgress = 1.0f;

        bool hasBarracksNow = app.m_entities->hasBuildingType(UnitType::Barracks, Faction::Resistance);
        bool hasFactoryNow = app.m_entities->hasBuildingType(UnitType::Factory, Faction::Resistance);
        if(!hasBarracksNow || !hasFactoryNow)
        {
            std::cerr << "[Test] Error: hasBuildingType failed after structure placement!" << std::endl;
            return false;
        }

        int bSpawnX = 0, bSpawnY = 0;
        int fSpawnX = 0, fSpawnY = 0;
        bool bSpawnOk = app.m_entities->getBuildingSpawnPoint(barracksId, *app.m_map, bSpawnX, bSpawnY);
        bool fSpawnOk = app.m_entities->getBuildingSpawnPoint(factoryId, *app.m_map, fSpawnX, fSpawnY);
        if(!bSpawnOk || !fSpawnOk)
        {
            std::cerr << "[Test] Error: getBuildingSpawnPoint failed for Barracks or Factory!" << std::endl;
            return false;
        }
        std::cout << "  - Barracks #" << barracksId << " spawn point: (" << bSpawnX << "," << bSpawnY << ")" << std::endl;
        std::cout << "  - Factory #" << factoryId << " spawn point: (" << fSpawnX << "," << fSpawnY << ")" << std::endl;

        // Test recruitment queue with structure-specific spawning
        app.m_economy->addCredits(1000);
        size_t prevUnitCount = app.m_entities->getAllEntities().size();
        app.m_economy->queueUnit(0, 80, 0.05f, barracksId, *app.m_entities); // Resistance rifleman
        app.m_economy->queueUnit(6, 220, 0.05f, factoryId, *app.m_entities); // Coyote
        if(app.m_economy->queueUnit(6, 220, 0.05f, barracksId, *app.m_entities) && app.m_entities->getBuilding(barracksId)->productionQueue.size() != 2)
        {
            std::cerr << "[Test] Error: queue bookkeeping mismatch at Barracks!" << std::endl;
            return false;
        }
        // Cancelling refunds and removes the order again
        const int creditsBeforeCancel = app.m_economy->getCredits();
        if(!app.m_economy->cancelUnit(6, barracksId, *app.m_entities) || app.m_economy->getCredits() != creditsBeforeCancel + 220)
        {
            std::cerr << "[Test] Error: cancelling a queued unit did not refund it!" << std::endl;
            return false;
        }

        // Simulate 20 ticks to complete instant production
        for(int i = 0; i < 20; ++i)
        {
            app.m_economy->update(dt, *app.m_entities, *app.m_map, *app.m_pathfinder);
            app.m_entities->update(dt, *app.m_map, *app.m_pathfinder);
        }

        size_t newUnitCount = app.m_entities->getAllEntities().size();
        if(newUnitCount < prevUnitCount + 2)
        {
            std::cerr << "[Test] Error: Units failed to spawn from designated Barracks and Factory!" << std::endl;
            return false;
        }
        std::cout << "  - Successfully trained infantry at Barracks and produced vehicle at Factory!" << std::endl;

        // 4. Test Attack-Move filter: Ensure buildings are filtered out of combat orders
        std::vector<int> mixedSelection = {barracksId, factoryId};
        if(!playerUnits.empty())
            mixedSelection.push_back(playerUnits[0]);
        std::vector<int> filteredCombatUnits;
        for(int uid : mixedSelection)
        {
            const Entity *e = app.m_entities->getEntity(uid);
            if(e && e->alive && !e->isBuilding)
            {
                filteredCombatUnits.push_back(uid);
            }
        }
        if(filteredCombatUnits.size() != 1 || filteredCombatUnits[0] != playerUnits[0])
        {
            std::cerr << "[Test] Error: Building filter failed on Attack-Move selection!" << std::endl;
            return false;
        }
        std::cout << "  - Attack-Move combat unit filter validated (structures correctly excluded)." << std::endl;

        // 5. Test Destructible Walls & Attack Order
        std::cout << "[Test] Validating Destructible Walls..." << std::endl;
        int twX = 20, twY = 20;
        app.m_map->setWall(twX, twY, Faction::Confederation, 100);
        if(app.m_map->getWall(twX, twY) == 0 || app.m_map->isPassable(twX, twY))
        {
            std::cerr << "[Test] Error: Failed to place test wall!" << std::endl;
            return false;
        }
        int initialWallHp = app.m_map->getWallHp(twX, twY);
        app.m_map->damageWall(twX, twY, 50);
        if(app.m_map->getWallHp(twX, twY) >= initialWallHp)
        {
            std::cerr << "[Test] Error: damageWall failed to reduce HP!" << std::endl;
            return false;
        }
        app.m_map->damageWall(twX, twY, 150); // Fatal damage
        if(app.m_map->getWall(twX, twY) != 0 || !app.m_map->isPassable(twX, twY))
        {
            std::cerr << "[Test] Error: Wall not destroyed upon fatal damage!" << std::endl;
            return false;
        }
        std::cout << "  - Destructible wall validated (takes damage, collapses, frees passability)." << std::endl;

        // 6. Test AI Rebuilding System for destroyed structures and walls
        std::cout << "[Test] Validating AI Base & Wall Reconstruction System..." << std::endl;
        int aiBx = app.m_mission->getEnemyHqX() + 4;
        int aiBy = app.m_mission->getEnemyHqY() + 4;
        int aiBId = app.m_entities->spawnBuilding(UnitType::Barracks, aiBx, aiBy, Faction::Confederation);
        app.m_map->setWall(aiBx + 5, aiBy, Faction::Confederation, 100);

        // AI captures base memory
        app.m_aiController->recordStartingBase(*app.m_entities, *app.m_map);

        // Destroy the AI structure and wall
        Building *aiB = app.m_entities->getBuilding(aiBId);
        if(aiB)
        {
            aiB->currentHp = 0;
            aiB->state = ActionState::Die;
            for(int ty = aiBy; ty < aiBy + aiB->buildHeight; ++ty)
            {
                for(int tx = aiBx; tx < aiBx + aiB->buildWidth; ++tx)
                {
                    app.m_map->setPassable(tx, ty, true);
                }
            }
        }
        app.m_map->damageWall(aiBx + 5, aiBy, 200); // Destroy wall

        // Simulate AI with abundant credits and elapsed rebuild timer
        for(int t = 0; t < 60; ++t)
        {
            app.m_aiController->update(0.25f, *app.m_map, *app.m_entities, *app.m_pathfinder);
        }

        // Verify AI reconstructed the structure and the wall
        bool aiBRebuilt = false;
        for(const auto &e : app.m_entities->getAllEntities())
        {
            if(e.alive && e.isBuilding && e.faction == Faction::Confederation && e.tileX == aiBx && e.tileY == aiBy)
            {
                aiBRebuilt = true;
                break;
            }
        }
        bool aiWallRebuilt = (app.m_map->getWall(aiBx + 5, aiBy) > 0);

        if(!aiBRebuilt || !aiWallRebuilt)
        {
            std::cerr << "[Test] Error: AI failed to rebuild destroyed structure or wall! aiBRebuilt=" << aiBRebuilt << " aiWallRebuilt=" << aiWallRebuilt << std::endl;
            return false;
        }
        std::cout << "  - AI successfully reconstructed destroyed starting base building and wall in place!" << std::endl;

        // 7. Standalone Test Map (maps/testmap.json) + Closed Passage Partial Pathing & Unit Blocking
        std::cout << "[Test] Validating Standalone Test Map (testmap.json), Unit Blocking & Closed Passages..." << std::endl;
        if(!app.m_mission->loadTestMap("assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
        {
            std::cerr << "[Test] Error: Failed to load Standalone Test Map (testmap.json)!" << std::endl;
            return false;
        }

        std::cout << "  - testmap.json loaded: " << app.m_map->getWidth() << "x" << app.m_map->getHeight() << ", " << app.m_entities->getAllEntities().size() << " entities." << std::endl;

        // The test map is edited in the map editor, so the checks below build their own polygon instead of
        // relying on its layout: a blank arena with a sealed enemy wall at X=23 and a player tank at (19, 7)
        app.m_entities->clear();
        app.m_map->initBlankArena(96, 64, 0);
        app.m_map->setFogEnabled(false);
        app.m_pathfinder->setMap(app.m_map.get());
        app.m_pathfinder->setEntityManager(app.m_entities.get());
        for(int y = 0; y < app.m_map->getHeight(); ++y)
            app.m_map->setWall(23, y, Faction::Confederation, 100);
        int testTankId = app.m_entities->spawnUnitByArchetype(8, 19, 7, Faction::Resistance);
        if(testTankId <= 0)
        {
            std::cerr << "[Test] Error: Test tank could not be placed at (19,7)!" << std::endl;
            return false;
        }

        // Order the tank to move to (27, 7) behind the sealed wall at X=23
        app.m_entities->issueMoveOrder({testTankId}, 27, 7, *app.m_pathfinder, false);
        for(int t = 0; t < 180; ++t)
        {
            app.m_entities->update(dt, *app.m_map, *app.m_pathfinder);
        }

        const Unit *tank = app.m_entities->getUnit(testTankId);
        if(!tank || tank->tileX != 22 || tank->state != ActionState::Idle || !tank->waitingForPath)
        {
            std::cerr << "[Test] Error: Unit did not stop cleanly at closest tile (22,7) before closed wall! tileX=" << (tank ? tank->tileX : -1) << " waitingForPath=" << (tank ? tank->waitingForPath : false) << std::endl;
            return false;
        }
        std::cout << "  - Unit walked to (" << tank->tileX << "," << tank->tileY << ") right in front of closed wall at X=23 and stood still waiting for path." << std::endl;

        // Now breach the wall at (23, 7) and verify the waiting unit automatically resumes moving to (27, 7)!
        app.m_map->damageWall(23, 7, 200);
        for(int t = 0; t < 240; ++t)
        {
            app.m_entities->update(dt, *app.m_map, *app.m_pathfinder);
        }
        tank = app.m_entities->getUnit(testTankId);
        if(!tank || tank->tileX != 27 || tank->tileY != 7)
        {
            std::cerr << "[Test] Error: Unit failed to resume movement after wall breach! tile=(" << (tank ? tank->tileX : -1) << "," << (tank ? tank->tileY : -1) << ")" << std::endl;
            return false;
        }
        std::cout << "  - After wall breach at (23,7), waiting unit automatically resumed path and reached (27,7)!" << std::endl;

        // Verify no two living mobile units ever overlap on the same tile
        const auto &allEnts = app.m_entities->getAllEntities();
        for(size_t i = 0; i < allEnts.size(); ++i)
        {
            if(!allEnts[i].alive || allEnts[i].state == ActionState::Die || allEnts[i].isBuilding)
                continue;
            for(size_t j = i + 1; j < allEnts.size(); ++j)
            {
                if(!allEnts[j].alive || allEnts[j].state == ActionState::Die || allEnts[j].isBuilding)
                    continue;
                if(allEnts[i].tileX == allEnts[j].tileX && allEnts[i].tileY == allEnts[j].tileY)
                {
                    std::cerr << "[Test] Error: Unit overlap detected at (" << allEnts[i].tileX << "," << allEnts[i].tileY << ")!" << std::endl;
                    return false;
                }
            }
        }
        std::cout << "  - Unit-to-unit tile collision validated (0 overlapping units)." << std::endl;

        // 8. Test New Mechanics: HQ Build Radius, Stormcast Slots, Rhino Burst & Wall Switching
        std::cout << "[Test] Validating HQ 25-Tile Radius, Lab Expansion (+5), Stormcast & Siege Units..." << std::endl;

        // A. HQ Build Radius (25 -> 30)
        int hqTileX = 5, hqTileY = 5;
        app.m_entities->clear();
        app.m_map->revealArea(hqTileX + 15, hqTileY, 3);
        app.m_map->revealArea(hqTileX + 28, hqTileY, 3);
        app.m_entities->setBuildRadius(25);
        app.m_entities->spawnBuildingByIndex(8, hqTileX, hqTileY, Faction::Resistance, 1.0f); // Resistance HQ (3x4)

        // Distance 15 tiles: within radius 25
        bool canBuildNear = app.m_entities->canPlaceBuilding(hqTileX + 15, hqTileY, 1, 1, *app.m_map, Players::ownerOf(Faction::Resistance));
        // Distance 28 tiles: outside radius 25
        bool canBuildFar = app.m_entities->canPlaceBuilding(hqTileX + 28, hqTileY, 1, 1, *app.m_map, Players::ownerOf(Faction::Resistance));
        if(!canBuildNear || canBuildFar)
        {
            std::cerr << "[Test] Error: HQ 25-tile build radius failed! near=" << canBuildNear << " far=" << canBuildFar << std::endl;
            return false;
        }
        // Expand radius by +5 (Lab research) -> 30 tiles
        app.m_entities->expandBuildRadius(5);
        bool canBuildAfterResearch = app.m_entities->canPlaceBuilding(hqTileX + 28, hqTileY, 1, 1, *app.m_map, Players::ownerOf(Faction::Resistance));
        if(!canBuildAfterResearch)
        {
            std::cerr << "[Test] Error: Lab +5 radius expansion failed! 28 tiles should be allowed within radius 30." << std::endl;
            return false;
        }
        std::cout << "  - HQ 25-tile radius & Lab +5 expansion validated (25 -> 30 tiles)." << std::endl;

        // B. Stormcast Spiral Slots Validation
        auto slots = AINavigataionStormcastView::computeGroupSlots(10, 10, 4, *app.m_map, app.m_entities.get(), {});
        if(slots.size() != 4 || slots[0].x != 10 || slots[0].y != 10)
        {
            std::cerr << "[Test] Error: Stormcast spiral algorithm failed slot count or center!" << std::endl;
            return false;
        }
        std::cout << "  - AINavigataionStormcastView computed 4 distinct spiral formation slots around (10,10)." << std::endl;

        // C. Rhino Siege Mode (archetype 10 <-> 15) and double-shot burst with animation
        int rhinoId = app.m_entities->spawnUnitByArchetype(10, 10, 10, Faction::Resistance);
        Unit *rhino = app.m_entities->getUnit(rhinoId);
        if(!rhino)
        {
            std::cerr << "[Test] Error: Failed to spawn Rhino!" << std::endl;
            return false;
        }
        app.m_entities->setSiegeUnlocked(Players::ownerOf(Faction::Resistance), true); // Rhino siege needs the Siege Artillery research
        app.m_entities->toggleSiegeMode({rhinoId});
        if(rhino->state != ActionState::Transform || rhino->pendingArchetypeId != 15)
        {
            std::cerr << "[Test] Error: Rhino did not enter ActionState::Transform with pendingArchetypeId 15!" << std::endl;
            return false;
        }
        // Simulate deployment animation steps (5 frames @ 0.09s = 0.45s)
        for(int step = 0; step < 6; ++step)
        {
            app.m_entities->update(0.10f, *app.m_map, *app.m_pathfinder);
        }
        if(rhino->archetypeId != 15 || !rhino->isSiegeMode || rhino->burstCount != 2 || std::abs(rhino->burstDelay - 0.1f) > 0.01f || std::abs(rhino->attackCooldown - 0.6f) > 0.01f || rhino->state != ActionState::Idle)
        {
            std::cerr << "[Test] Error: Rhino siege mode stats mismatch after transform! arch=" << rhino->archetypeId << " burst=" << rhino->burstCount << " delay=" << rhino->burstDelay << " cd=" << rhino->attackCooldown << " state=" << static_cast<int>(rhino->state) << std::endl;
            return false;
        }
        std::cout << "  - Rhino siege deployment animation & 2-shot burst validated (delay: 0.1s, cooldown: 0.9s)." << std::endl;

        // Test undeployment back to mobile mode
        app.m_entities->toggleSiegeMode({rhinoId});
        if(rhino->state != ActionState::Transform || rhino->pendingArchetypeId != 10)
        {
            std::cerr << "[Test] Error: Rhino undeployment did not enter ActionState::Transform with pendingArchetypeId 10!" << std::endl;
            return false;
        }
        for(int step = 0; step < 6; ++step)
        {
            app.m_entities->update(0.10f, *app.m_map, *app.m_pathfinder);
        }
        if(rhino->archetypeId != 10 || rhino->isSiegeMode || rhino->state != ActionState::Idle)
        {
            std::cerr << "[Test] Error: Rhino undeployment back to archetype 10 failed! arch=" << rhino->archetypeId << " isSiege=" << rhino->isSiegeMode << std::endl;
            return false;
        }
        std::cout << "  - Rhino undeployment animation back to mobile mode validated." << std::endl;

        // Re-enter siege mode for the wall-switch combat test below
        app.m_entities->toggleSiegeMode({rhinoId});
        for(int step = 0; step < 6; ++step)
        {
            app.m_entities->update(0.10f, *app.m_map, *app.m_pathfinder);
        }

        // D. Grad MLRS siege range check (13 tiles, as in the original)
        const auto *gradSiegeCfg = app.m_entities->getUnitConfig(16);
        if(!gradSiegeCfg || gradSiegeCfg->attackRange < 12.9f)
        {
            std::cerr << "[Test] Error: Grad MLRS siege attack range is not 13! range=" << (gradSiegeCfg ? gradSiegeCfg->attackRange : -1.0f) << std::endl;
            return false;
        }
        std::cout << "  - Grad MLRS siege range validated: 13 tiles." << std::endl;

        // E. Wall auto-targeting immediate switch to enemy unit
        app.m_map->setWall(14, 10, Faction::Confederation, 100);
        rhino->targetWallTileX = 14;
        rhino->targetWallTileY = 10;
        // Spawn enemy confederation rifleman at (12, 10) within attack range
        int enemyInfId = app.m_entities->spawnUnitByArchetype(1, 12, 10, Faction::Confederation);
        app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        if(rhino->targetWallTileX >= 0 || rhino->targetEntityId != enemyInfId)
        {
            std::cerr << "[Test] Error: Unit did not immediately break off wall attack to engage enemy unit! targetWall=" << rhino->targetWallTileX << " targetEntity=" << rhino->targetEntityId << std::endl;
            return false;
        }
        std::cout << "  - Immediate target switch from enemy wall to enemy unit validated!" << std::endl;

        // 9. Test New Raycast Bullet, Combat Move-Interrupt, Tank Recoil Cooldown & Camera Bounds
        std::cout << "[Test] Validating Raycast Bullet, Move Order Retreat (No Freezing), Tank Recoil & Camera Bounds..." << std::endl;

        // A. Camera Bounds past map boundaries up to screen center (0x0 = half black, half map)
        app.m_camera->setPosition(-9999.0f, -9999.0f);
        app.m_camera->clampToBounds(app.m_map->getWidth(), app.m_map->getHeight());
        float expectedMinX = -(app.m_camera->getViewportWidth() / app.m_camera->getZoom()) * 0.5f;
        float expectedMinY = -(app.m_camera->getViewportHeight() / app.m_camera->getZoom()) * 0.5f;
        if(std::abs(app.m_camera->getX() - expectedMinX) > 1.0f || std::abs(app.m_camera->getY() - expectedMinY) > 1.0f)
        {
            std::cerr << "[Test] Error: Camera boundary clamp mismatch! got=(" << app.m_camera->getX() << "," << app.m_camera->getY() << ") expected=(" << expectedMinX << "," << expectedMinY << ")" << std::endl;
            return false;
        }
        std::cout << "  - Camera boundary past map edge up to center (half-black screen) validated." << std::endl;

        // B. Raycast Bullet Instant Hit (Hitscan)
        int pShooterId = app.m_entities->spawnUnitByArchetype(0, 10, 10, Faction::Resistance);   // Rifleman
        int eTargetId = app.m_entities->spawnUnitByArchetype(1, 12, 10, Faction::Confederation); // Rifleman
        Unit *shooter = app.m_entities->getUnit(pShooterId);
        Unit *victim = app.m_entities->getUnit(eTargetId);
        int initialVictimHp = victim->currentHp;
        shooter->targetEntityId = eTargetId;
        shooter->cooldownTimer = 0.0f;
        // Advance 1 tick: bullet should deal instant raycast damage immediately
        app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        if(victim->currentHp >= initialVictimHp)
        {
            std::cerr << "[Test] Error: Raycast bullet did not deal damage instantaneously! initial=" << initialVictimHp << " current=" << victim->currentHp << std::endl;
            return false;
        }
        std::cout << "  - Raycast hitscan bullet validated: instant damage applied on frame of shot (" << initialVictimHp << " -> " << victim->currentHp << " HP)." << std::endl;

        // C. Move Order While In Combat (Unit must retreat and NOT freeze on the spot)
        // The shooter is currently in combat with targetEntityId = eTargetId.
        // Issue a regular move order to (2, 2)
        app.m_entities->issueMoveOrder({pShooterId}, 2, 2, *app.m_pathfinder, false);
        if(shooter->targetEntityId != 0 || shooter->state != ActionState::Move || shooter->path.empty())
        {
            std::cerr << "[Test] Error: Move order did not set ActionState::Move or clear combat target!" << std::endl;
            return false;
        }
        int startTileX = shooter->tileX;
        int startTileY = shooter->tileY;
        // Advance simulation for 40 ticks: unit must drive towards (2, 2) and NOT freeze or re-lock target!
        for(int t = 0; t < 40; ++t)
        {
            app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        }
        if(shooter->tileX == startTileX && shooter->tileY == startTileY)
        {
            std::cerr << "[Test] Error: Unit froze on the spot after move order during combat!" << std::endl;
            return false;
        }
        if(shooter->targetEntityId != 0)
        {
            std::cerr << "[Test] Error: Unit executing pure move order re-acquired enemy target!" << std::endl;
            return false;
        }
        std::cout << "  - Move order during combat validated: unit successfully retreated from (" << startTileX << "," << startTileY << ") to (" << shooter->tileX << "," << shooter->tileY << ") without freezing." << std::endl;

        // D. Tank Firing Animation and Cooldown Transition (Hammer: single-shot recoil, returns to idle during cooldown)
        int hammerId = app.m_entities->spawnUnitByArchetype(9, 15, 15, Faction::Confederation); // T-21 Hammer
        int dummyId = app.m_entities->spawnUnitByArchetype(8, 17, 15, Faction::Resistance);     // Target dummy
        Unit *hammer = app.m_entities->getUnit(hammerId);
        hammer->targetEntityId = dummyId;
        hammer->cooldownTimer = 0.0f;
        app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        if(hammer->state != ActionState::Attack || hammer->attackAnimTimer <= 0.0f)
        {
            std::cerr << "[Test] Error: Hammer did not enter ActionState::Attack on firing shell!" << std::endl;
            return false;
        }
        // Advance 0.25s: attack anim (0.22s) should finish, returning hammer to ActionState::Idle during reload cooldown
        for(int t = 0; t < 5; ++t)
        {
            app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        }
        if(hammer->state != ActionState::Idle || hammer->cooldownTimer <= 0.0f)
        {
            std::cerr << "[Test] Error: Hammer stuck in ActionState::Attack during reload cooldown! state=" << static_cast<int>(hammer->state) << " cooldownTimer=" << hammer->cooldownTimer << std::endl;
            return false;
        }
        std::cout << "  - Tank firing single-shot recoil validated: returns to ActionState::Idle during reload cooldown (no machine-gun loop)." << std::endl;

        // E. Validate Wall Rubble Remnants, Building Production Animation, Flamethrower & Fortress Hull Aiming
        // 1. Wall Rubble: Destroy wall at (30, 30)
        app.m_map->setWall(30, 30, Faction::Confederation, 100);
        app.m_map->damageWall(30, 30, 200); // Fatal damage
        if(app.m_map->getWall(30, 30) != 0 || !app.m_map->isPassable(30, 30))
        {
            std::cerr << "[Test] Error: Destroyed wall did not clear wall grid or restore passability!" << std::endl;
            return false;
        }
        std::cout << "  - Wall destruction leaves passable rubble remnant on ground without explosion." << std::endl;

        // 2. Building Production Flag: Factory & Barracks
        int barId = app.m_entities->spawnBuildingByIndex(10, 35, 35, Faction::Resistance, 1.0f); // Barracks
        Building *testBEnt = app.m_entities->getBuilding(barId);
        if(testBEnt->isProducing)
        {
            std::cerr << "[Test] Error: Idle barracks was marked as isProducing!" << std::endl;
            return false;
        }
        app.m_economy->queueUnit(0, 80, 5.0f, barId, *app.m_entities);
        app.m_economy->update(0.1f, *app.m_entities, *app.m_map, *app.m_pathfinder);
        if(!testBEnt->isProducing)
        {
            std::cerr << "[Test] Error: Barracks in active production was not marked as isProducing!" << std::endl;
            return false;
        }
        std::cout << "  - Building active production animation toggle validated (idle=static, producing=active)." << std::endl;

        // 3. Fortress rotating machine gun turret & strict orthogonal movement (cannot move diagonally)
        int fortId = app.m_entities->spawnUnitByArchetype(7, 40, 40, Faction::Confederation);     // TM-40 Fortress
        int targetDummyId = app.m_entities->spawnUnitByArchetype(0, 42, 40, Faction::Resistance); // East of fortress
        Unit *fortress = app.m_entities->getUnit(fortId);
        fortress->facingDirection = 2; // Hull faces South
        fortress->targetEntityId = targetDummyId;
        fortress->cooldownTimer = 0.0f;
        app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        if(fortress->turretFacing != 0)
        {
            std::cerr << "[Test] Error: Fortress machine gun turret did not rotate towards target (East=0)! Got: " << fortress->turretFacing << std::endl;
            return false;
        }
        if(fortress->facingDirection != 2)
        {
            std::cerr << "[Test] Error: Fortress hull rotated when firing while idle! facingDirection=" << fortress->facingDirection << std::endl;
            return false;
        }
        std::cout << "  - Fortress rotating machine gun turret validated (turret facing 0 East, hull retained South)." << std::endl;

        // Validate Fortress firing animation (ActionState::Attack with advancing animFrame for muzzle flashes)
        if(fortress->state != ActionState::Attack || fortress->attackAnimTimer <= 0.0f)
        {
            std::cerr << "[Test] Error: Fortress did not enter Attack state with active attackAnimTimer! state=" << static_cast<int>(fortress->state) << " anim=" << fortress->attackAnimTimer << " cd=" << fortress->cooldownTimer << " target=" << fortress->targetEntityId
                      << " dmg=" << fortress->attackDamage << " range=" << fortress->attackRange << std::endl;
            return false;
        }
        int initialAnimFrame = fortress->animFrame;
        app.m_entities->update(0.06f, *app.m_map, *app.m_pathfinder);
        if(fortress->animFrame <= initialAnimFrame)
        {
            std::cerr << "[Test] Error: Fortress firing animation frames did not advance! frame=" << fortress->animFrame << std::endl;
            return false;
        }
        std::cout << "  - Fortress firing animation validated: ActionState::Attack with muzzle flash frame advancement (" << initialAnimFrame << " -> " << fortress->animFrame << ")." << std::endl;

        // The Fortress moves along all 8 directions like every other vehicle: open ground (40,40) -> (45,45)
        // is a straight diagonal of 5 steps
        std::vector<PathNode> fortPath;
        app.m_pathfinder->findPath(40, 40, 45, 45, fortPath, fortId);
        int fortDiagonal = 0;
        {
            int px = 40, py = 40;
            for(const auto &node : fortPath)
            {
                if(node.x != px && node.y != py)
                    ++fortDiagonal;
                px = node.x;
                py = node.y;
            }
        }
        if(fortPath.size() != 5 || fortDiagonal != 5)
        {
            std::cerr << "[Test] Error: Fortress must drive diagonally! steps=" << fortPath.size() << " diagonal=" << fortDiagonal << std::endl;
            return false;
        }
        std::cout << "  - Fortress diagonal movement validated: (40,40) -> (45,45) in 5 diagonal steps." << std::endl;

        // 10. Validating Friendly Wall Raycast Pass-through, Explicit Manual Wall Targeting, Wall Rubble 1759, Crouching & Fortress Flamethrower
        std::cout << "[Test] Validating Wall Targeting, Rubble 1759, Fortress Flamethrower & Crouching Attack..." << std::endl;

        // A. Explicit Manual Wall Targeting (any wall, even friendly, can be explicitly targeted)
        int testShooterId = app.m_entities->spawnUnitByArchetype(0, 20, 20, Faction::Resistance);
        app.m_map->setWall(21, 20, Faction::Resistance, 100);
        app.m_entities->issueAttackWallOrder({testShooterId}, 21, 20, *app.m_pathfinder);
        Unit *testShooter = app.m_entities->getUnit(testShooterId);
        if(testShooter->targetWallTileX != 21 || !testShooter->isManualWallTarget)
        {
            std::cerr << "[Test] Error: Explicit manual wall targeting failed for friendly wall!" << std::endl;
            return false;
        }
        std::cout << "  - Manual wall targeting validated: explicit order targets wall (any faction) with isManualWallTarget=true." << std::endl;

        // B. Friendly Wall Raycast Pass-Through (non-targeted stray fire ignores friendly wall)
        int enemyDummyId = app.m_entities->spawnUnitByArchetype(0, 23, 20, Faction::Confederation);
        Entity *enemyDummy = app.m_entities->getEntity(enemyDummyId);
        enemyDummy->currentHp = 100;
        enemyDummy->cooldownTimer = 10.0f; // Dummy does not fire
        int prevWallHp = app.m_map->getWallHp(21, 20);
        // Shooter at (20,20) shoots at dummy at (23,20). Friendly wall is at (21,20).
        testShooter->targetWallTileX = -1;
        testShooter->targetWallTileY = -1;
        testShooter->isManualWallTarget = false;
        testShooter->targetEntityId = enemyDummyId;
        testShooter->cooldownTimer = 0.0f;
        app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        if(app.m_map->getWallHp(21, 20) < prevWallHp)
        {
            std::cerr << "[Test] Error: Friendly wall took damage from allied raycast!" << std::endl;
            return false;
        }
        if(enemyDummy->currentHp >= 100)
        {
            std::cerr << "[Test] Error: Allied raycast was blocked by friendly wall and did not damage target!" << std::endl;
            return false;
        }
        std::cout << "  - Friendly wall raycast pass-through validated: allied shots pass through friendly walls." << std::endl;

        // B2. Enemy Wall Pass-Through when targeting Enemy Base / Unit (walls block movement, not projectiles)
        app.m_map->setWall(21, 20, Faction::Confederation, 100);
        int prevEnemyWallHp = app.m_map->getWallHp(21, 20);
        enemyDummy->currentHp = 100;
        testShooter->targetEntityId = enemyDummyId;
        testShooter->cooldownTimer = 0.0f;
        app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        if(app.m_map->getWallHp(21, 20) < prevEnemyWallHp)
        {
            std::cerr << "[Test] Error: Enemy wall took damage when shooting at enemy unit behind it!" << std::endl;
            return false;
        }
        if(enemyDummy->currentHp >= 100)
        {
            std::cerr << "[Test] Error: Bullets did not fly past enemy wall to hit targeted enemy entity!" << std::endl;
            return false;
        }
        std::cout << "  - Enemy wall pass-through validated: bullets fly past enemy wall directly to targeted base/unit." << std::endl;

        // C. Wall Destruction Rubble is compact single-tile masonry ruin 1778
        app.m_map->damageWall(21, 20, 1000);
        if(app.m_map->getWallRubble(21, 20) != 1778)
        {
            std::cerr << "[Test] Error: Wall rubble is not authentic compact composite 1778! Got: " << app.m_map->getWallRubble(21, 20) << std::endl;
            return false;
        }
        std::cout << "  - Wall rubble remnant 1778 validated on destruction (compact single-tile ruin)." << std::endl;

        // D. Fortress Flamethrower Upgrade & Range <= 1 Tile
        app.m_entities->setFortressFlamethrowerUnlocked(Players::ownerOf(Faction::Confederation), true);
        fortress = app.m_entities->getUnit(fortId);
        if(!fortress)
        {
            fortId = app.m_entities->spawnUnitByArchetype(7, 40, 40, Faction::Confederation);
            fortress = app.m_entities->getUnit(fortId);
        }
        fortress->tileX = 40;
        fortress->tileY = 40;
        int dummyCloseId = app.m_entities->spawnUnitByArchetype(0, 41, 40, Faction::Resistance);
        Entity *dummyClose = app.m_entities->getEntity(dummyCloseId);
        dummyClose->alive = true;
        dummyClose->currentHp = 1000;
        dummyClose->state = ActionState::Idle;
        dummyClose->tileX = 41; // dx=1, dy=0 -> close
        dummyClose->tileY = 40;
        fortress->targetEntityId = dummyCloseId;
        fortress->cooldownTimer = 0.0f;
        app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        if(fortress->projectile != ProjectileType::Flame)
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
        app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        if(fortress->projectile != ProjectileType::Bullet)
        {
            std::cerr << "[Test] Error: Fortress did not revert to bullet when distance > 1 tile!" << std::endl;
            return false;
        }
        std::cout << "  - Fortress flamethrower range rule validated: flame <= 1 tile with upgrade, bullet > 1 tile." << std::endl;

        // E. Heavy Assault / Grenadier Crouching Posture During Attack Cooldown
        dummyClose->tileX = 32;
        dummyClose->tileY = 30;                                                            // 2 tiles from Grenadier (well within range 5.0)
        int grenId = app.m_entities->spawnUnitByArchetype(2, 30, 30, Faction::Resistance); // Grenadier (archetype 2)
        Entity *gren = app.m_entities->getEntity(grenId);
        gren->targetEntityId = dummyCloseId;
        gren->state = ActionState::Attack;
        gren->attackAnimTimer = 0.05f;
        gren->cooldownTimer = 1.0f;
        app.m_entities->update(0.06f, *app.m_map, *app.m_pathfinder); // attackAnimTimer expires
        if(gren->state != ActionState::Attack)
        {
            std::cerr << "[Test] Error: Grenadier did not stay in crouching Attack state during cooldown!" << std::endl;
            return false;
        }
        std::cout << "  - Grenadier / Heavy Assault crouching posture during combat cooldown validated." << std::endl;

        // F. Debug Mode Passability Grid State & Inset
        app.m_gameplayController->setDebugMode(true);
        if(!app.m_gameplayController->isShowDebugGrid())
        {
            std::cerr << "[Test] Error: Debug passability grid not enabled by default in debug mode!" << std::endl;
            return false;
        }
        app.m_gameplayController->setShowDebugGrid(false);
        if(app.m_gameplayController->isShowDebugGrid())
        {
            std::cerr << "[Test] Error: Debug passability grid toggle failed!" << std::endl;
            return false;
        }
        app.m_gameplayController->setShowDebugGrid(true);
        std::cout << "  - Debug passability grid toggle (70% transparency, 2px contracted grid) validated." << std::endl;

        // D. Unit-Wall Collision Snapping
        // Place unit right next to wall and verify movement update does not enter wall tile
        testShooter->tileX = 21;
        testShooter->tileY = 19;
        testShooter->subX = 0.0f;
        testShooter->subY = 0.0f;
        app.m_map->setWall(22, 19, Faction::Confederation, 100);
        // Attempt to move East directly into wall
        testShooter->path = {{22, 19}};
        testShooter->pathIndex = 0;
        testShooter->state = ActionState::Move;
        app.m_entities->update(0.1f, *app.m_map, *app.m_pathfinder);
        if(testShooter->tileX == 22)
        {
            std::cerr << "[Test] Error: Unit stepped inside wall tile!" << std::endl;
            return false;
        }
        std::cout << "  - Unit-wall collision barrier validated: unit snapped to tile center without penetrating wall." << std::endl;

        // E. Bunker Construction & Autonomous Defense Combat
        std::cout << "[Test] Validating Bunker Construction & Autonomous Power-Independent Combat..." << std::endl;
        // Spawn Resistance Bunker #13 under construction (0.0f)
        int bunkerId = app.m_entities->spawnBuildingByIndex(13, 25, 25, Faction::Resistance, 0.0f);
        Building *bunker = app.m_entities->getBuilding(bunkerId);
        if(!bunker || bunker->buildProgress >= 1.0f)
        {
            std::cerr << "[Test] Error: Bunker was not spawned under construction!" << std::endl;
            return false;
        }
        // Spawn enemy dummy near bunker
        int enemyDummyBunkerId = app.m_entities->spawnUnitByArchetype(0, 27, 25, Faction::Confederation);
        Entity *enemyDummyBunker = app.m_entities->getEntity(enemyDummyBunkerId);
        enemyDummyBunker->currentHp = 100;
        int enemyHpBefore = enemyDummyBunker->currentHp;

        // Update 0.5s while under construction: bunker MUST NOT fire
        app.m_entities->update(0.5f, *app.m_map, *app.m_pathfinder);
        if(enemyDummyBunker->currentHp < enemyHpBefore || bunker->state == ActionState::Attack)
        {
            std::cerr << "[Test] Error: Bunker fired while under construction (progress=" << bunker->buildProgress << ")!" << std::endl;
            return false;
        }

        // Advance construction to completion (1.0f) and simulate power blackout (isPowered = false)
        bunker->buildProgress = 1.0f;
        bunker->isPowered = false;
        bunker->cooldownTimer = 0.0f;
        enemyHpBefore = enemyDummyBunker->currentHp;

        // Bunker must fire because concrete bunkers are autonomous manned pillboxes
        app.m_entities->update(0.1f, *app.m_map, *app.m_pathfinder);
        if(enemyDummyBunker->currentHp >= enemyHpBefore)
        {
            std::cerr << "[Test] Error: Completed autonomous bunker failed to attack enemy dummy! enemyHp=" << enemyDummyBunker->currentHp << std::endl;
            return false;
        }
        std::cout << "  - Bunker autonomous combat validated: inactive during construction, fires upon completion even without power grid." << std::endl;

        // 11. Test Fog of War Lifecycle & Symmetrical Blindness (Hidden -> Visible -> Explored)
        std::cout << "[Test] Validating Fog of War: Hidden -> Visible -> Explored & AI Fog Blindness..." << std::endl;
        // Map m0 has fog enabled. Pick distant tile (5, 35) far from player base
        app.m_map->resetAllFog(FogState::Hidden);
        app.m_map->setFogEnabled(true);
        app.m_entities->updateFogOfWar(*app.m_map);

        // Initial state: distant tile (5, 35) must be Hidden (pitch black)
        if(app.m_map->getFogState(5, 35) != FogState::Hidden)
        {
            std::cerr << "[Test] Error: Unexplored tile (5, 35) is not FogState::Hidden!" << std::endl;
            return false;
        }

        // Spawn a scout unit at (5, 35): tile must become Visible
        int scoutId = app.m_entities->spawnUnitByArchetype(0, 5, 35, Faction::Resistance);
        app.m_entities->updateFogOfWar(*app.m_map);
        if(app.m_map->getFogState(5, 35) != FogState::Visible)
        {
            std::cerr << "[Test] Error: Tile with player scout (5, 35) did not become FogState::Visible!" << std::endl;
            return false;
        }

        // Move scout far away to (20, 20): tile (5, 35) must demote to Explored (dimmed shroud)
        Unit *scout = app.m_entities->getUnit(scoutId);
        scout->tileX = 20;
        scout->tileY = 20;
        app.m_entities->updateFogOfWar(*app.m_map);
        if(app.m_map->getFogState(5, 35) != FogState::Explored)
        {
            std::cerr << "[Test] Error: Vacated tile (5, 35) did not demote to FogState::Explored! state=" << static_cast<int>(app.m_map->getFogState(5, 35)) << std::endl;
            return false;
        }

        // Spawn enemy rifleman on Explored tile (5, 35): player must NOT see him (not Visible)
        int hiddenEnemyId = app.m_entities->spawnUnitByArchetype(1, 5, 35, Faction::Confederation);
        Entity *hiddenEnemy = app.m_entities->getEntity(hiddenEnemyId);
        if(app.m_map->getFogState(hiddenEnemy->tileX, hiddenEnemy->tileY) == FogState::Visible)
        {
            std::cerr << "[Test] Error: Enemy on Explored tile is visible in fog!" << std::endl;
            return false;
        }

        // Spawn an isolated player unit at (2, 2) where no Confederation units or buildings exist: AI must NOT see him
        int lonePlayerId = app.m_entities->spawnUnitByArchetype(0, 2, 2, Faction::Resistance);
        Entity *lonePlayer = app.m_entities->getEntity(lonePlayerId);
        if(app.m_entities->isEntityVisibleToAi(*lonePlayer))
        {
            std::cerr << "[Test] Error: Confederation AI can see isolated player unit across the map in fog!" << std::endl;
            return false;
        }

        // Cleanup test entities
        scout->alive = false;
        hiddenEnemy->alive = false;
        lonePlayer->alive = false;
        std::cout << "  - Fog of War dynamic lifecycle validated: Hidden -> Visible -> Explored." << std::endl;
        std::cout << "  - Player & AI symmetrical fog blindness validated: units hidden in fog cannot be targeted." << std::endl;

        // 12. Test AI Difficulty Modes & YAML Configuration (Easy, Medium, Hard, Hardcore)
        std::cout << "[Test] Validating AI Difficulty System & assets/characters/difficulty.yaml..." << std::endl;
        if(!app.m_aiController->loadDifficultyConfig("assets/characters/difficulty.yaml"))
        {
            std::cerr << "[Test] Error: Failed to load difficulty configuration from assets/characters/difficulty.yaml!" << std::endl;
            return false;
        }

        const auto &diffConfigs = app.m_aiController->getAllDifficultyConfigs();
        if(diffConfigs.size() < 4)
        {
            std::cerr << "[Test] Error: Expected at least 4 difficulty tiers, found " << diffConfigs.size() << std::endl;
            return false;
        }

        // Verify Easy: 50% resource handicap
        auto itEasy = diffConfigs.find(AIDifficulty::Easy);
        if(itEasy == diffConfigs.end() || itEasy->second.startingCredits != 1000 || itEasy->second.resourceMultiplier != 0.50f)
        {
            std::cerr << "[Test] Error: Easy difficulty configuration invalid!" << std::endl;
            return false;
        }

        // Verify Medium: 20% resource handicap
        auto itMed = diffConfigs.find(AIDifficulty::Medium);
        if(itMed == diffConfigs.end() || itMed->second.startingCredits != 1600 || itMed->second.resourceMultiplier != 0.80f)
        {
            std::cerr << "[Test] Error: Medium difficulty configuration invalid!" << std::endl;
            return false;
        }

        // Verify Hard: No handicap (100% fair)
        auto itHard = diffConfigs.find(AIDifficulty::Hard);
        if(itHard == diffConfigs.end() || itHard->second.startingCredits != 2000 || itHard->second.resourceMultiplier != 1.00f)
        {
            std::cerr << "[Test] Error: Hard difficulty configuration invalid!" << std::endl;
            return false;
        }

        // Verify Hardcore: +30% resources, +10% production speed
        auto itHc = diffConfigs.find(AIDifficulty::Hardcore);
        if(itHc == diffConfigs.end() || itHc->second.startingCredits != 2600 || itHc->second.resourceMultiplier != 1.30f || itHc->second.productionSpeedMultiplier != 1.10f)
        {
            std::cerr << "[Test] Error: Hardcore difficulty configuration invalid!" << std::endl;
            return false;
        }

        // Test difficulty application in simulation
        app.m_aiController->setDifficulty(AIDifficulty::Easy);
        app.m_aiController->reset();
        if(app.m_aiController->getEnemyCredits() != 1000)
        {
            std::cerr << "[Test] Error: Easy AI starting credits mismatch: " << app.m_aiController->getEnemyCredits() << std::endl;
            return false;
        }

        app.m_aiController->setDifficulty(AIDifficulty::Hardcore);
        app.m_aiController->reset();
        if(app.m_aiController->getEnemyCredits() != 2600)
        {
            std::cerr << "[Test] Error: Hardcore AI starting credits mismatch: " << app.m_aiController->getEnemyCredits() << std::endl;
            return false;
        }
        std::cout << "  - AI Difficulty modes loaded from assets/characters/difficulty.yaml successfully:" << std::endl;
        std::cout << "    * Easy: -50% resources (credits=1000, income=7)" << std::endl;
        std::cout << "    * Medium: -20% resources (credits=1600, income=12)" << std::endl;
        std::cout << "    * Hard: fair (credits=2000, income=15)" << std::endl;
        std::cout << "    * Hardcore: +30% resources (credits=2600, income=20), +10% production speed (1.10x)" << std::endl;

        // 13. Test Moving Unit Damage Reduction (50% - 100% fast C-code RNG algorithm)
        std::cout << "[Test] Validating Moving Unit Damage Reduction (50-100% fast C algorithm)..." << std::endl;
        int testMovingUnitId = app.m_entities->spawnUnitByArchetype(0, 10, 10, Faction::Resistance);
        Unit *testUnit = app.m_entities->getUnit(testMovingUnitId);
        if(!testUnit)
        {
            std::cerr << "[Test] Error: Failed to spawn test unit!" << std::endl;
            return false;
        }

        // Stationary test: 100% damage must always be applied
        testUnit->state = ActionState::Idle;
        testUnit->path.clear();
        const int baseDamage = 100;
        for(int i = 0; i < 20; ++i)
        {
            int dmg = EntityManager::applyMovingUnitDamage(baseDamage, testUnit);
            if(dmg != baseDamage)
            {
                std::cerr << "[Test] Error: Stationary unit took reduced damage: " << dmg << " instead of " << baseDamage << std::endl;
                return false;
            }
        }

        // Moving test: 50% - 100% damage
        testUnit->state = ActionState::Move;
        int minDmgSeen = 999;
        int maxDmgSeen = -999;
        for(int i = 0; i < 500; ++i)
        {
            int dmg = EntityManager::applyMovingUnitDamage(baseDamage, testUnit);
            if(dmg < 50 || dmg > 100)
            {
                std::cerr << "[Test] Error: Moving unit took out-of-bounds damage: " << dmg << " (expected 50..100)" << std::endl;
                return false;
            }
            minDmgSeen = std::min(minDmgSeen, dmg);
            maxDmgSeen = std::max(maxDmgSeen, dmg);
        }
        if(minDmgSeen > 55 || maxDmgSeen < 95)
        {
            std::cerr << "[Test] Error: Moving unit damage distribution too narrow: min=" << minDmgSeen << " max=" << maxDmgSeen << std::endl;
            return false;
        }
        testUnit->alive = false;
        std::cout << "  - Moving unit damage reduction validated: idle=100%, moving=50%..100% (observed: " << minDmgSeen << "%.." << maxDmgSeen << "%)." << std::endl;

        // 14. Test Bullet Muzzle Exit Point & localePosition (Default 0,0 & Turret Binding)
        std::cout << "[Test] Validating Bullet Muzzle Exit Point (turret binding & default center for non-turret)..." << std::endl;
        int testMuzzleUnitId = app.m_entities->spawnUnitByArchetype(0, 15, 15, Faction::Resistance);
        Unit *mUnit = app.m_entities->getUnit(testMuzzleUnitId);
        if(!mUnit)
        {
            std::cerr << "[Test] Error: Failed to spawn muzzle test unit!" << std::endl;
            return false;
        }

        // A. Units without rotating turret: localePosition is fully active and rotates with unit facingDirection
        float cx = mUnit->getUnitCenterPixelX();
        float cy = mUnit->getUnitCenterPixelY();
        float mX = 0.0f, mY = 0.0f;
        mUnit->hasRotatingTurret = false;
        mUnit->archetypeId = 0; // Infantry
        mUnit->muzzleOffsetX = 10.0f;
        mUnit->muzzleOffsetY = 2.0f;

        // Default facing 0 (East): forward=+X, lateral=+Y
        mUnit->facingDirection = 0;
        mUnit->getMuzzlePoint(mX, mY);
        if(std::abs(mX - (cx + 10.0f)) > 0.01f || std::abs(mY - ((cy - 4.0f) + 2.0f)) > 0.01f)
        {
            std::cerr << "[Test] Error: Non-turret unit did not apply localePosition rotated with facingDirection!" << std::endl;
            return false;
        }

        // Default zero offset (0, 0): returns unit center pivot
        mUnit->muzzleOffsetX = 0.0f;
        mUnit->muzzleOffsetY = 0.0f;
        mUnit->getMuzzlePoint(mX, mY);
        if(std::abs(mX - cx) > 0.01f || std::abs(mY - (cy - 4.0f)) > 0.01f)
        {
            std::cerr << "[Test] Error: Non-turret unit default zero offset failed!" << std::endl;
            return false;
        }

        // B. Units with rotating turret (mUnit->hasRotatingTurret == true) attach strictly to turret and rotate with turretFacing
        mUnit->hasRotatingTurret = true;
        mUnit->archetypeId = 6; // Assault Tank: turret pivot is at (cx - 2.0f, cy - 14.0f)
        mUnit->hasCustomTurretOffset = false;
        mUnit->muzzleOffsetX = 12.0f;
        mUnit->muzzleOffsetY = 3.0f;

        const float tPivotX = cx - 2.0f;
        const float tPivotY = cy - 14.0f;

        // Facing 0 (East): forward=+X, lateral=+Y
        mUnit->turretFacing = 0;
        mUnit->getMuzzlePoint(mX, mY);
        if(std::abs(mX - (tPivotX + 12.0f)) > 0.01f || std::abs(mY - (tPivotY + 3.0f)) > 0.01f)
        {
            std::cerr << "[Test] Error: Facing East turret muzzle point mismatch! Got (" << mX << "," << mY << ") expected (" << (tPivotX + 12.0f) << "," << (tPivotY + 3.0f) << ")" << std::endl;
            return false;
        }

        // Facing 2 (South): forward=+Y, lateral=-X
        mUnit->turretFacing = 2;
        mUnit->getMuzzlePoint(mX, mY);
        if(std::abs(mX - (tPivotX - 3.0f)) > 0.01f || std::abs(mY - (tPivotY + 12.0f)) > 0.01f)
        {
            std::cerr << "[Test] Error: Facing South turret muzzle point mismatch! Got (" << mX << "," << mY << ") expected (" << (tPivotX - 3.0f) << "," << (tPivotY + 12.0f) << ")" << std::endl;
            return false;
        }

        mUnit->alive = false;
        std::cout << "  - Bullet muzzle point validated: non-turret units strictly use default (0,0) center, turret units strictly attach to elevated rotating turret." << std::endl;

        // 15. Test Projectile Spread Dispersion (random angle offset per shot & pinpoint when spread=0)
        std::cout << "[Test] Validating Projectile Spread Dispersion (random angle offset per shot & 0=pinpoint)..." << std::endl;
        int spreadShooterId = app.m_entities->spawnUnitByArchetype(0, 10, 10, Faction::Resistance);
        Unit *spreadShooter = app.m_entities->getUnit(spreadShooterId);
        if(!spreadShooter)
        {
            std::cerr << "[Test] Error: Failed to spawn shooter unit!" << std::endl;
            return false;
        }

        // A. Pinpoint high accuracy test when spread = 0
        spreadShooter->spreadAngle = 0.0f;
        const size_t projZeroStart = app.m_entities->getProjectiles().size();
        for(int i = 0; i < 5; ++i)
        {
            app.m_entities->spawnProjectileFromEntity(*spreadShooter, 50.0f, 50.0f, 200.0f, 50.0f, 0, *app.m_map);
        }
        const auto &projsZero = app.m_entities->getProjectiles();
        for(size_t i = projZeroStart; i < projsZero.size(); ++i)
        {
            if(std::abs(projsZero[i].targetY - 50.0f) > 0.001f || std::abs(projsZero[i].targetX - 200.0f) > 0.001f)
            {
                std::cerr << "[Test] Error: Shot spread = 0 failed to produce pinpoint accuracy! Got (" << projsZero[i].targetX << "," << projsZero[i].targetY << ")" << std::endl;
                return false;
            }
        }

        // B. Dispersion test with authentic weapon spread (unset -1.0f or positive angle)
        spreadShooter->spreadAngle = -1.0f;
        const size_t projCountBefore = app.m_entities->getProjectiles().size();
        for(int i = 0; i < 10; ++i)
        {
            app.m_entities->spawnProjectileFromEntity(*spreadShooter, 50.0f, 50.0f, 200.0f, 50.0f, 0, *app.m_map);
        }
        const auto &projs = app.m_entities->getProjectiles();
        if(projs.size() < projCountBefore + 10)
        {
            std::cerr << "[Test] Error: Spawned projectiles count mismatch!" << std::endl;
            return false;
        }
        std::set<int> distinctTargetYInts;
        for(size_t i = projCountBefore; i < projs.size(); ++i)
        {
            distinctTargetYInts.insert(static_cast<int>(std::round(projs[i].targetY * 10.0f)));
        }
        if(distinctTargetYInts.size() < 4)
        {
            std::cerr << "[Test] Error: Projectile spread is not varying across shots! Unique Y count: " << distinctTargetYInts.size() << std::endl;
            return false;
        }
        spreadShooter->alive = false;
        std::cout << "  - Projectile spread validated: spread=0 yields 100% pinpoint accuracy; spread>0 produced " << distinctTargetYInts.size() << " distinct trajectories." << std::endl;

        // 16. Splash Damage Percentages, Flame Death Animation & Simultaneous Dual Firing
        std::cout << "[Test] Validating Weapon Splash Percentages, Flame Burning Death & Multi-Weapon..." << std::endl;
        // A. Validate YAML splash percentages
        const auto *hammerCfg = app.m_entities->getUnitConfig(9);
        const auto *zeusCfg = app.m_entities->getUnitConfig(11);
        const auto *rhinoCfg = app.m_entities->getUnitConfig(10);
        const auto *rhinoSiegeCfg = app.m_entities->getUnitConfig(15);
        const auto *gradCfg = app.m_entities->getUnitConfig(13);
        const auto *mlrsCfg = app.m_entities->getUnitConfig(12);
        const auto *grenadierCfg = app.m_entities->getUnitConfig(2);
        const auto *heavyAssaultCfg = app.m_entities->getUnitConfig(3);

        if(!hammerCfg || hammerCfg->splashPercent != 5 || hammerCfg->weapons.empty() || hammerCfg->weapons[0].splashPercent != 5)
        {
            std::cerr << "[Test] Error: Hammer splash percent mismatch! Expected 5%" << std::endl;
            return false;
        }
        if(!zeusCfg || zeusCfg->splashPercent != 20 || zeusCfg->weapons.empty() || zeusCfg->weapons[0].splashPercent != 20)
        {
            std::cerr << "[Test] Error: Zeus splash percent mismatch! Expected 20%" << std::endl;
            return false;
        }
        if(!rhinoCfg || rhinoCfg->splashPercent != 8 || rhinoCfg->weapons.empty() || rhinoCfg->weapons[0].splashPercent != 8)
        {
            std::cerr << "[Test] Error: Rhino splash percent mismatch! Expected 8%" << std::endl;
            return false;
        }
        if(!rhinoSiegeCfg || rhinoSiegeCfg->splashPercent != 15)
        {
            std::cerr << "[Test] Error: Rhino siege splash percent mismatch! Expected 15%" << std::endl;
            return false;
        }
        if(!gradCfg || gradCfg->splashPercent != 16 || gradCfg->weapons.empty() || gradCfg->weapons[0].splashPercent != 16)
        {
            std::cerr << "[Test] Error: Grad MLRS splash percent mismatch! Expected 16%" << std::endl;
            return false;
        }
        if(!mlrsCfg || mlrsCfg->splashPercent != 4 || mlrsCfg->weapons.empty() || mlrsCfg->weapons[0].splashPercent != 4)
        {
            std::cerr << "[Test] Error: Porcupine MLRS splash percent mismatch! Expected 4%" << std::endl;
            return false;
        }
        if(!grenadierCfg || grenadierCfg->splashPercent != 2 || grenadierCfg->weapons.empty() || grenadierCfg->weapons[0].splashPercent != 2)
        {
            std::cerr << "[Test] Error: Grenadier splash percent mismatch! Expected 2%" << std::endl;
            return false;
        }
        if(!heavyAssaultCfg || heavyAssaultCfg->splashPercent != 2 || heavyAssaultCfg->weapons.empty() || heavyAssaultCfg->weapons[0].splashPercent != 2)
        {
            std::cerr << "[Test] Error: Heavy Assault splash percent mismatch! Expected 2%" << std::endl;
            return false;
        }
        std::cout << "  - Weapon splash damage YAML configurations validated: Hammer=5%, Zeus=20%, Rhino=8%/15%, Grad=16%, Porcupine=4%, Grenadier=2%, HeavyAssault=2%." << std::endl;

        // B. Validate 2x2 area Splash Damage execution on impact
        int primaryTargetId = app.m_entities->spawnUnitByArchetype(0, 15, 15, Faction::Confederation);
        int splashTargetId = app.m_entities->spawnUnitByArchetype(0, 16, 15, Faction::Confederation); // 1 tile away (within 2x2 area)
        int farTargetId = app.m_entities->spawnUnitByArchetype(0, 20, 15, Faction::Confederation);    // 5 tiles away (outside splash)
        Entity *priTarget = app.m_entities->getEntity(primaryTargetId);
        Entity *splTarget = app.m_entities->getEntity(splashTargetId);
        Entity *farTarget = app.m_entities->getEntity(farTargetId);
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
        float priX = priTarget->getUnitCenterPixelX();
        float priY = priTarget->getUnitCenterPixelY();
        app.m_entities->spawnProjectileFromEntity(testZeusShooter, priX - 60.0f, priY, priX, priY, primaryTargetId, *app.m_map);

        // Update projectiles until impact
        for(int t = 0; t < 20; ++t)
        {
            app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        }
        if(priTarget->currentHp >= 100)
        {
            std::cerr << "[Test] Error: Primary target took no damage from shell!" << std::endl;
            return false;
        }
        if(splTarget->currentHp >= 100)
        {
            std::cerr << "[Test] Error: 2x2 area splash target took no damage from shell!" << std::endl;
            return false;
        }
        if(farTarget->currentHp != 100)
        {
            std::cerr << "[Test] Error: Far target outside splash radius unexpectedly took damage!" << std::endl;
            return false;
        }
        std::cout << "  - 2x2 area splash damage validated: primary target damaged, neighbor in 2x2 area took " << (100 - splTarget->currentHp) << " splash damage, far target unharmed." << std::endl;

        // C. Validate Flame burning infantry death (killedByFlame=true)
        int infantryVictimId = app.m_entities->spawnUnitByArchetype(0, 18, 18, Faction::Confederation);
        Entity *flameVictim = app.m_entities->getEntity(infantryVictimId);
        flameVictim->currentHp = 10;
        Entity flameShooter;
        flameShooter.id = 998;
        flameShooter.faction = Faction::Resistance;
        flameShooter.projectile = ProjectileType::Flame;
        flameShooter.attackDamage = 40;
        flameShooter.splashPercent = 0;
        flameShooter.spreadAngle = 0.0f;
        float vicX = flameVictim->getUnitCenterPixelX();
        float vicY = flameVictim->getUnitCenterPixelY();
        app.m_entities->spawnProjectileFromEntity(flameShooter, vicX - 30.0f, vicY, vicX, vicY, infantryVictimId, *app.m_map);
        for(int t = 0; t < 10; ++t)
        {
            app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        }
        Unit *victimUnit = nullptr;
        for(auto &e : app.m_entities->getAllEntities())
        {
            if(e.id == infantryVictimId)
            {
                victimUnit = e.asUnit();
                break;
            }
        }
        if(!victimUnit || victimUnit->state != ActionState::Die || !victimUnit->killedByFlame)
        {
            std::cerr << "[Test] Error: Infantry killed by Flame did not trigger burning death (killedByFlame=true)!" << std::endl;
            return false;
        }
        std::cout << "  - Flame burning infantry death animation state validated (killedByFlame=true, seq 259/260)." << std::endl;

        // 17. Validate Vehicle Movement & Rotation Mechanics (Left-to-Right turns through Down first, then Right)
        std::cout << "[Test] Validating Vehicle Movement & Rotation Mechanics (Left-to-Right turns through Down)..." << std::endl;
        int turnTestTankId = app.m_entities->spawnUnitByArchetype(6, 12, 12, Faction::Confederation); // Hammer tank
        Unit *turnTestTank = app.m_entities->getEntity(turnTestTankId)->asUnit();
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
        for(int step = 0; step < 4; ++step)
        {
            app.m_entities->update(0.081f, *app.m_map, *app.m_pathfinder);
            observedFacings.push_back(turnTestTank->facingDirection);
            // Vehicle must remain at initial tile (12, 12) while rotating in place!
            if(turnTestTank->tileX != 12 || turnTestTank->tileY != 12 || turnTestTank->subX != 0.0f)
            {
                std::cerr << "[Test] Error: Vehicle advanced position before completing in-place turn!" << std::endl;
                return false;
            }
        }

        // Expected sequence turning Left (4) to Right (0): 4 -> 3 -> 2 (Down) -> 1 -> 0 (Right)
        const std::vector<int> expectedFacings = {4, 3, 2, 1, 0};
        if(observedFacings != expectedFacings)
        {
            std::cerr << "[Test] Error: Vehicle turn sequence incorrect! Observed: ";
            for(int f : observedFacings)
                std::cerr << f << " ";
            std::cerr << " Expected: 4 3 2 1 0" << std::endl;
            return false;
        }

        // Advance further: vehicle is now aligned to 0 (Right), forward translation must occur
        app.m_entities->update(0.1f, *app.m_map, *app.m_pathfinder);
        if(turnTestTank->subX <= 0.0f && turnTestTank->tileX <= 12)
        {
            std::cerr << "[Test] Error: Vehicle did not start forward movement after completing turn to 0!" << std::endl;
            return false;
        }

        // Validate infantry instant turn (no turn delay)
        int turnTestInfId = app.m_entities->spawnUnitByArchetype(0, 15, 15, Faction::Confederation);
        Unit *turnTestInf = app.m_entities->getEntity(turnTestInfId)->asUnit();
        turnTestInf->facingDirection = 4;
        turnTestInf->state = ActionState::Move;
        turnTestInf->path = {{16, 15}};
        turnTestInf->pathIndex = 0;
        app.m_entities->update(0.02f, *app.m_map, *app.m_pathfinder);
        if(turnTestInf->facingDirection != 0)
        {
            std::cerr << "[Test] Error: Infantry did not turn instantly to movement direction!" << std::endl;
            return false;
        }
        std::cout << "  - Vehicle Left-to-Right turning mechanics validated: rotated 4 -> 3 -> 2(Down) -> 1 -> 0(Right) in place before moving forward." << std::endl;
        std::cout << "  - Infantry instant turn without rotation delay validated." << std::endl;

        // 18. Validate Tactical Support Assist (Chain Aggro with strict depth = 1)
        std::cout << "[Test] Validating Tactical Support Assist (Chain Aggro with depth = 1)..." << std::endl;
        // Spawn Enemy E at (52, 2) (Confederation)
        int suppEnemyId = app.m_entities->spawnUnitByArchetype(0, 52, 2, Faction::Confederation);
        Entity *suppEnemy = app.m_entities->getEntity(suppEnemyId);
        suppEnemy->currentHp = 500;
        suppEnemy->cooldownTimer = 999.0f; // Doesn't fire back

        // Spawn Frontline Ally B at (49, 2) (Resistance, attackRange = 5.0f).
        // dist(B, E) = 3 tiles <= 5.0f.
        int allyBId = app.m_entities->spawnUnitByArchetype(0, 49, 2, Faction::Resistance);
        Unit *allyB = app.m_entities->getEntity(allyBId)->asUnit();
        allyB->targetEntityId = suppEnemyId;
        allyB->state = ActionState::Attack;
        allyB->cooldownTimer = 0.0f;

        // Spawn Ally A at (46, 2) (Resistance, attackRange = 5.0f).
        // dist(A, B) = 3 tiles <= 5.0f (within attack range of B).
        // dist(A, E) = 6 tiles > 5.0f (outside sight and attack range of enemy E).
        int allyAId = app.m_entities->spawnUnitByArchetype(0, 46, 2, Faction::Resistance);
        Unit *allyA = app.m_entities->getEntity(allyAId)->asUnit();
        allyA->state = ActionState::Idle;
        allyA->targetEntityId = 0;

        // Spawn Ally C at (43, 2) (Resistance, attackRange = 5.0f).
        // dist(C, A) = 3 tiles <= 5.0f.
        // dist(C, B) = 6 tiles > 5.0f (outside range of B).
        int allyCId = app.m_entities->spawnUnitByArchetype(0, 43, 2, Faction::Resistance);
        Unit *allyC = app.m_entities->getEntity(allyCId)->asUnit();
        allyC->state = ActionState::Idle;
        allyC->targetEntityId = 0;

        // Update 1 tick (0.05s)
        app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);

        // Ally B must be directly engaged
        if(!allyB->isDirectEngaged)
        {
            std::cerr << "[Test] Error: Frontline ally B should be isDirectEngaged=true!" << std::endl;
            return false;
        }

        // Ally A (depth = 1) must have answered the call for help: target set to suppEnemyId and moving
        if(allyA->targetEntityId != suppEnemyId || allyA->state != ActionState::Move)
        {
            std::cerr << "[Test] Error: Ally A (depth = 1) did not join in support of ally B! target=" << allyA->targetEntityId << " state=" << static_cast<int>(allyA->state) << std::endl;
            return false;
        }

        // Ally C (depth > 1) MUST REMAIN IDLE! A is only moving in support, not directly engaged yet!
        if(allyC->targetEntityId != 0 || allyC->state != ActionState::Idle)
        {
            std::cerr << "[Test] Error: Ally C violated depth=1 constraint! It triggered prematurely while A was merely moving to support!" << std::endl;
            return false;
        }

        std::cout << "  - Tactical support assist validated: Ally A joined frontline Ally B." << std::endl;
        std::cout << "  - Strict depth = 1 constraint validated: Ally C remained Idle while Ally A moved to support." << std::endl;

        // 20. Validating Command Override During Movement, Classic Maps (Brod, Canyon, Oasis) & Flame Stream
        std::cout << "[Test] Validating Command Override During Movement & Classic Maps..." << std::endl;

        // A. Command Override During Movement: unit moving to (25, 25), receives attack order on enemy at (12, 10)
        int movingRiflemanId = app.m_entities->spawnUnitByArchetype(0, 10, 10, Faction::Resistance);
        Unit *moveU = app.m_entities->getUnit(movingRiflemanId);
        app.m_entities->issueMoveOrder({movingRiflemanId}, 25, 25, *app.m_pathfinder, false);
        if(moveU->path.empty() || moveU->state != ActionState::Move)
        {
            std::cerr << "[Test] Error: Unit failed to start move order!" << std::endl;
            return false;
        }
        // Now interrupt with direct attack order on enemy unit
        int enemyTargetId = app.m_entities->spawnUnitByArchetype(1, 12, 10, Faction::Confederation);
        app.m_entities->issueAttackOrder({movingRiflemanId}, enemyTargetId, app.m_pathfinder.get());
        if(moveU->targetEntityId != enemyTargetId)
        {
            std::cerr << "[Test] Error: TargetEntityId was not updated on command override!" << std::endl;
            return false;
        }
        // Target is within attack range (dist = 2 tiles <= attackRange 3..4): old path must be cleared and state must be Attack!
        if(!moveU->path.empty() || moveU->state != ActionState::Attack)
        {
            std::cerr << "[Test] Error: Unit old move path was not cleared upon attack order!" << std::endl;
            return false;
        }
        // Advance simulation tick: unit stays on target without jerking into old path
        app.m_entities->update(0.1f, *app.m_map, *app.m_pathfinder);
        if(moveU->targetEntityId != enemyTargetId || moveU->state == ActionState::Move)
        {
            std::cerr << "[Test] Error: Unit jerked back into ActionState::Move after attack order!" << std::endl;
            return false;
        }
        std::cout << "  - Command override during movement validated: old path cleared, new attack target engaged smoothly." << std::endl;

        // B. Menu content: the campaign lists only its missions, a custom game can use any map of assets/maps
        if(MissionController::kCampaignMissionCount != 7 || MissionController::getAvailableMissions().size() < static_cast<size_t>(MissionController::kCampaignMissionCount))
        {
            std::cerr << "[Test] Error: The campaign must consist of its 7 missions!" << std::endl;
            return false;
        }
        const auto customMaps = MissionController::listSkirmishMaps();
        if(customMaps.size() < 10)
        {
            std::cerr << "[Test] Error: Custom game lists only " << customMaps.size() << " maps!" << std::endl;
            return false;
        }
        for(const auto &info : customMaps)
        {
            if(!app.m_mission->loadSkirmish(info.path, *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
            {
                std::cerr << "[Test] Error: Custom game cannot start on " << info.path << "!" << std::endl;
                return false;
            }
            int resistanceBuildings = 0, confederationBuildings = 0;
            for(const auto &e : app.m_entities->getAllEntities())
            {
                if(!e.alive || !e.isBuilding)
                    continue;
                (e.faction == Faction::Resistance ? resistanceBuildings : confederationBuildings)++;
            }
            if(resistanceBuildings == 0 || confederationBuildings == 0 || !app.m_mission->isSkirmish() || (app.m_mission->getPlayerHqX() == app.m_mission->getEnemyHqX() && app.m_mission->getPlayerHqY() == app.m_mission->getEnemyHqY()))
            {
                std::cerr << "[Test] Error: Custom game on " << info.path << " has no two bases!" << std::endl;
                return false;
            }
        }
        const int serialBeforeRestart = app.m_mission->getLoadSerial();
        if(!app.m_mission->restart("assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder) || app.m_mission->getLoadSerial() == serialBeforeRestart || !app.m_mission->isSkirmish())
        {
            std::cerr << "[Test] Error: Restarting a custom game failed!" << std::endl;
            return false;
        }
        // Back to the blank arena the following tests expect
        app.m_entities->clear();
        app.m_map->initBlankArena(60, 40, 0);
        app.m_pathfinder->setMap(app.m_map.get());
        std::cout << "  - Campaign = 7 missions; custom game starts on all " << customMaps.size() << " maps of assets/maps and restarts OK." << std::endl;

        // 21. Validating Choke-Point Blocking Enemy Combat & Unit Anti-Stacking Mechanics
        std::cout << "[Test] Validating Choke-Point Blocking Enemy Engagement & Anti-Stacking..." << std::endl;

        // Setup a 1-tile corridor: walls at y=19 and y=21 for x in [45..55], passage at y=20
        for(int wx = 45; wx <= 55; ++wx)
        {
            app.m_map->setWall(wx, 19, Faction::Neutral, 500);
            app.m_map->setWall(wx, 21, Faction::Neutral, 500);
        }
        // Place an enemy unit blocking the only corridor at (50, 20)
        int chokeEnemyId = app.m_entities->spawnUnitByArchetype(1, 50, 20, Faction::Confederation);
        Unit *chokeEnemy = app.m_entities->getUnit(chokeEnemyId);
        chokeEnemy->currentHp = 8; // Vulnerable enemy blocker (a rifleman has 30 HP, the same as the runner)

        // Spawn friendly unit at (46, 20) and issue Move order to (54, 20) beyond the enemy blocker
        int runnerId = app.m_entities->spawnUnitByArchetype(0, 46, 20, Faction::Resistance);
        Unit *runner = app.m_entities->getUnit(runnerId);
        app.m_entities->issueMoveOrder({runnerId}, 54, 20, *app.m_pathfinder, false);

        // Friendly unit moves towards (50, 20), encounters the blocking enemy
        // Advance simulation: unit should encounter the enemy, engage in combat, destroy it, and continue to (54, 20)
        bool engagedEnemy = false;
        for(int t = 0; t < 150; ++t)
        {
            app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
            if(runner->targetEntityId == chokeEnemyId || runner->state == ActionState::Attack)
            {
                engagedEnemy = true;
            }
            if(!chokeEnemy->alive)
            {
                break;
            }
        }
        if(!engagedEnemy)
        {
            std::cerr << "[Test] Error: Unit failed to auto-engage enemy blocking the choke point!" << std::endl;
            return false;
        }
        std::cout << "  - Choke-point blocker engagement validated: unit automatically attacked the enemy blocking the corridor." << std::endl;

        // Advance simulation: unit should resume moving through the now-cleared corridor
        for(int t = 0; t < 150; ++t)
        {
            app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
            if(runner->tileX >= 52)
                break;
        }
        if(runner->tileX <= 50)
        {
            std::cerr << "[Test] Error: Unit failed to advance through corridor after blocker was destroyed! tileX=" << runner->tileX << std::endl;
            return false;
        }
        std::cout << "  - Passage traversal validated: unit advanced past cleared choke point to destination (tileX=" << runner->tileX << ")." << std::endl;

        // Validate Unit Anti-Stacking: force two units to same tile and verify automatic separation
        int stackU1Id = app.m_entities->spawnUnitByArchetype(0, 15, 15, Faction::Resistance);
        int stackU2Id = app.m_entities->spawnUnitByArchetype(0, 15, 15, Faction::Resistance);
        Unit *su1 = app.m_entities->getUnit(stackU1Id);
        Unit *su2 = app.m_entities->getUnit(stackU2Id);
        // Artificially stack su2 on su1
        su2->tileX = su1->tileX;
        su2->tileY = su1->tileY;
        app.m_entities->resolveUnitStacking(*app.m_map);
        if(su1->tileX == su2->tileX && su1->tileY == su2->tileY)
        {
            std::cerr << "[Test] Error: resolveUnitStacking failed to separate overlapping units!" << std::endl;
            return false;
        }
        std::cout << "  - Anti-stacking validated: overlapping units automatically separated to distinct tiles (" << su1->tileX << "," << su1->tileY << ") and (" << su2->tileX << "," << su2->tileY << ")." << std::endl;

        // 22. Validating Unit Top-Corner Wandering Prevention, Siege Mode Idle Out-of-Range, Building Construction Vulnerability & Visible Fog / Multi-HQ Radius (16 tiles)
        std::cout << "[Test] Validating Top-Corner Wandering Fix, Siege Out-of-Range, Building HP Damage & Visible Fog / HQ Radius..." << std::endl;

        // A. Top-Corner Wandering Prevention
        int idleUId = app.m_entities->spawnUnitByArchetype(0, 20, 20, Faction::Resistance);
        Unit *idleU = app.m_entities->getUnit(idleUId);
        if(idleU->targetTileX != -1 || idleU->targetTileY != -1)
        {
            std::cerr << "[Test] Error: Spawned unit should have targetTileX/Y == -1, got (" << idleU->targetTileX << "," << idleU->targetTileY << ")" << std::endl;
            return false;
        }
        app.m_entities->update(1.0f, *app.m_map, *app.m_pathfinder);
        if(idleU->tileX != 20 || idleU->tileY != 20 || idleU->state == ActionState::Move)
        {
            std::cerr << "[Test] Error: Idle unit moved autonomously without orders! (" << idleU->tileX << "," << idleU->tileY << ")" << std::endl;
            return false;
        }
        std::cout << "  - Top-corner wandering prevention validated: unit remained at (20,20) with targetTileX/Y == -1." << std::endl;

        // B. Siege Mode Unit Out-of-Range Attack Check
        int siegeSniperId = app.m_entities->spawnUnitByArchetype(5, 20, 20, Faction::Resistance); // Sniper (archetype 5 is siege sniper)
        Unit *siegeSniper = app.m_entities->getUnit(siegeSniperId);
        siegeSniper->isSiegeMode = true;
        int farEnemyId = app.m_entities->spawnUnitByArchetype(1, 40, 40, Faction::Confederation); // 28 tiles away, out of range
        app.m_entities->issueAttackOrder({siegeSniperId}, farEnemyId, app.m_pathfinder.get());
        if(siegeSniper->state == ActionState::Attack)
        {
            std::cerr << "[Test] Error: Siege unit entered ActionState::Attack for out-of-range target!" << std::endl;
            return false;
        }
        app.m_entities->update(0.2f, *app.m_map, *app.m_pathfinder);
        if(siegeSniper->state == ActionState::Attack)
        {
            std::cerr << "[Test] Error: Siege unit vibrating / animating in ActionState::Attack for out-of-range target!" << std::endl;
            return false;
        }
        std::cout << "  - Siege out-of-range attack check validated: unit stays in Idle and does not vibrate/animate." << std::endl;

        // C. Building Construction Vulnerability (takes HP damage and can be destroyed)
        int constrBldgId = app.m_entities->spawnBuildingByIndex(1, 22, 22, Faction::Resistance, 0.0f); // PowerPlant under construction
        Building *constrBldg = app.m_entities->getBuilding(constrBldgId);
        if(!constrBldg || constrBldg->buildProgress >= 1.0f)
        {
            std::cerr << "[Test] Error: Building under construction failed to spawn with progress 0!" << std::endl;
            return false;
        }
        int initialHp = constrBldg->currentHp;
        // Deal 30 damage to building under construction
        constrBldg->currentHp -= 30;
        int damagedHp = constrBldg->currentHp;
        constrBldg->update(0.1f, *app.m_map, {}); // Run updateConstruction
        if(constrBldg->currentHp >= initialHp)
        {
            std::cerr << "[Test] Error: Building under construction is invulnerable! HP was reset from " << damagedHp << " to " << constrBldg->currentHp << std::endl;
            return false;
        }
        std::cout << "  - Building construction vulnerability validated: takes real HP damage and does not reset." << std::endl;

        // D. Fog of War placement check & Multi-HQ 16-tile build radius
        app.m_entities->clear();
        app.m_map->initBlankArena(60, 60, 0);
        app.m_map->setFogEnabled(true);
        app.m_map->resetAllFog(FogState::Hidden);
        app.m_entities->setBuildRadius(16);                                                     // Default 16 tiles
        int hq1Id = app.m_entities->spawnBuildingByIndex(8, 10, 10, Faction::Resistance, 1.0f); // HQ 1 at (10,10)
        (void) hq1Id;
        app.m_map->revealArea(10, 10, 20); // Reveal around HQ 1
        // Unrevealed tile at (45, 45) should be forbidden
        bool canBuildInFog = app.m_entities->canPlaceBuilding(45, 45, 2, 2, *app.m_map, Players::ownerOf(Faction::Resistance));
        if(canBuildInFog)
        {
            std::cerr << "[Test] Error: canPlaceBuilding allowed construction in Fog!" << std::endl;
            return false;
        }
        // Distance 8 from HQ 1: allowed
        bool canBuildNearHq1 = app.m_entities->canPlaceBuilding(18, 10, 2, 2, *app.m_map, Players::ownerOf(Faction::Resistance));
        // Distance 22 from HQ 1: outside 16-tile radius
        app.m_map->revealArea(32, 10, 10);
        bool canBuildFarHq1 = app.m_entities->canPlaceBuilding(32, 10, 2, 2, *app.m_map, Players::ownerOf(Faction::Resistance));
        if(!canBuildNearHq1 || canBuildFarHq1)
        {
            std::cerr << "[Test] Error: HQ 1 16-tile build radius failed! near=" << canBuildNearHq1 << " far=" << canBuildFarHq1 << std::endl;
            return false;
        }
        // Now build a second HQ at (24, 10) within range of HQ 1
        app.m_map->revealArea(24, 10, 20);
        int hq2Id = app.m_entities->spawnBuildingByIndex(8, 24, 10, Faction::Resistance, 1.0f);
        (void) hq2Id;
        // Now (32, 10) is distance 8 from HQ 2: should NOW BE ALLOWED thanks to second HQ!
        bool canBuildNearHq2 = app.m_entities->canPlaceBuilding(32, 10, 2, 2, *app.m_map, Players::ownerOf(Faction::Resistance));
        if(!canBuildNearHq2)
        {
            std::cerr << "[Test] Error: Second HQ failed to expand build radius to its vicinity!" << std::endl;
            return false;
        }
        std::cout << "  - Fog-only placement & Multi-HQ 16-tile radius expansion validated successfully." << std::endl;

        // 23. Validating Unit Remains (Остова) in YAML & Cohesive AI March Movement
        std::cout << "[Test] Validating Unit Remains (Остова) in YAML & Cohesive Movement..." << std::endl;

        // A. Verify YAML loading of remains:
        const auto *riflemanCfg = app.m_entities->getUnitConfigByUnitType(UnitType::ResistanceRifleman, Faction::Resistance);
        if(!riflemanCfg || riflemanCfg->deathSequences.size() < 2 || riflemanCfg->flameDeathSequences.size() < 2)
        {
            std::cerr << "[Test] Error: Rifleman YAML did not load death sequences!" << std::endl;
            return false;
        }
        const auto *t23HammerCfg = app.m_entities->getUnitConfigByUnitType(UnitType::Hammer, Faction::Confederation);
        if(!t23HammerCfg || t23HammerCfg->wreckComposite != 1764 || t23HammerCfg->wreckStages != 5)
        {
            std::cerr << "[Test] Error: Hammer YAML did not load wreck composite 1764!" << std::endl;
            return false;
        }
        const auto *coyoteCfg = app.m_entities->getUnitConfigByUnitType(UnitType::Coyote, Faction::Resistance);
        if(!coyoteCfg || coyoteCfg->wreckComposite != 1759)
        {
            std::cerr << "[Test] Error: Coyote YAML did not load wreck composite 1759!" << std::endl;
            return false;
        }
        const auto *t23ZeusCfg = app.m_entities->getUnitConfigByUnitType(UnitType::Zeus, Faction::Confederation);
        if(!t23ZeusCfg || t23ZeusCfg->wreckComposite != 1769 || t23ZeusCfg->wreckStages != 4)
        {
            std::cerr << "[Test] Error: Zeus YAML did not load wreck composite 1769!" << std::endl;
            return false;
        }
        std::cout << "  - YAML Remains (Остова) configurations validated for infantry (death [247, 248], flame [259, 260]) and vehicles (1759, 1764, 1769)." << std::endl;

        // B. Infantry death variant selection: bullet vs flame
        int inf1Id = app.m_entities->spawnUnitByArchetype(0, 15, 15, Faction::Resistance);
        Unit *inf1 = app.m_entities->getUnit(inf1Id);
        inf1->currentHp = 10;
        // Shoot inf1 with bullet
        Entity bulletShooter(false);
        bulletShooter.faction = Faction::Confederation;
        bulletShooter.attackDamage = 50;
        bulletShooter.projectile = ProjectileType::Bullet;
        app.m_entities->spawnProjectileFromEntity(bulletShooter, 15 * 30.0f, 15 * 20.0f, 15 * 30.0f, 15 * 20.0f, inf1Id, *app.m_map);
        app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        if(inf1->state != ActionState::Die || (inf1->selectedDeathSeq != 247 && inf1->selectedDeathSeq != 248))
        {
            std::cerr << "[Test] Error: Normal infantry death did not pick seq 247 or 248! got: " << inf1->selectedDeathSeq << std::endl;
            return false;
        }
        if(inf1->killedByFlame)
        {
            std::cerr << "[Test] Error: Normal death marked killedByFlame=true!" << std::endl;
            return false;
        }

        // Flame death
        int inf2Id = app.m_entities->spawnUnitByArchetype(0, 16, 15, Faction::Resistance);
        Unit *inf2 = app.m_entities->getUnit(inf2Id);
        inf2->currentHp = 10;
        Entity t23FlameShooter(false);
        t23FlameShooter.faction = Faction::Confederation;
        t23FlameShooter.attackDamage = 50;
        t23FlameShooter.projectile = ProjectileType::Flame;
        app.m_entities->spawnProjectileFromEntity(t23FlameShooter, 16 * 30.0f, 16 * 20.0f, 16 * 30.0f, 15 * 20.0f, inf2Id, *app.m_map);
        app.m_entities->update(0.10f, *app.m_map, *app.m_pathfinder);
        if(inf2->state != ActionState::Die || !inf2->killedByFlame || (inf2->selectedDeathSeq != 259 && inf2->selectedDeathSeq != 260))
        {
            std::cerr << "[Test] Error: Flame death did not set killedByFlame or seq 259/260! got: " << inf2->selectedDeathSeq << std::endl;
            return false;
        }
        std::cout << "  - Infantry corpse selection validated: bullet chooses 247/248, flame chooses 259/260." << std::endl;

        // C. Cohesive March Column Movement: units in a column don't scatter
        app.m_entities->clear();
        app.m_map->initBlankArena(60, 60, 0);
        // Spawn 4 units in a line: (10, 20), (9, 20), (8, 20), (7, 20)
        std::vector<int> colUnits;
        for(int i = 0; i < 4; ++i)
        {
            int uid = app.m_entities->spawnUnitByArchetype(0, 10 - i, 20, Faction::Confederation);
            colUnits.push_back(uid);
        }
        // Issue attack move towards (30, 20)
        app.m_entities->issueAttackMoveOrder(colUnits, 30, 20, *app.m_pathfinder);
        // Simulate march for 20 ticks
        for(int t = 0; t < 20; ++t)
        {
            app.m_entities->update(0.05f, *app.m_map, *app.m_pathfinder);
        }
        // Verify all units are advancing along y=20 (±1 tile for slots) and moving eastward (tileX > 7)
        for(int uid : colUnits)
        {
            Unit *u = app.m_entities->getUnit(uid);
            if(std::abs(u->tileY - 20) > 2 || u->tileX < 7)
            {
                std::cerr << "[Test] Error: Unit scattered away from column! tile=(" << u->tileX << "," << u->tileY << ")" << std::endl;
                return false;
            }
        }
        std::cout << "  - Cohesive march column validated: units move eastward together without scattering across the map." << std::endl;

        // =========================================================================
        // Test #24: Validating Argon 4 Architectural Guarantees:
        //  - Strict Coordinate Invariants (No (0,0) ghost march)
        //  - Siege Mode Out-Of-Range Anti-Vibration
        //  - Fortress Orthogonal Rail Navigation & Decompositions
        //  - Strict Depth-1 Support Assist Graph
        // =========================================================================
        std::cout << "[Test] Validating Argon 4 Solutions (Invariant Coordinates, Siege Anti-Vibration, Depth-1 Assist, Fortress Rails)..." << std::endl;
        app.m_entities->clear();
        app.m_map->initBlankArena(60, 60, 0);

        // 1. Strict Target Coordinate Invariant:
        int invUnitId = app.m_entities->spawnUnitByArchetype(0, 25, 25, Faction::Resistance);
        Unit *invUnit = app.m_entities->getUnit(invUnitId);
        invUnit->clearMoveTarget();
        invUnit->waitingForPath = true;
        invUnit->state = ActionState::Idle;
        invUnit->pathRetryTimer = 0.5f; // trigger retry
        app.m_entities->update(0.1f, *app.m_map, *app.m_pathfinder);
        if(invUnit->state == ActionState::Move || invUnit->targetTileX != -1 || invUnit->tileX != 25 || invUnit->tileY != 25)
        {
            std::cerr << "[Test] Error: Unit with clearMoveTarget() moved to (0,0)! state=" << static_cast<int>(invUnit->state) << " targetX=" << invUnit->targetTileX << " tile=(" << invUnit->tileX << "," << invUnit->tileY << ")" << std::endl;
            return false;
        }
        std::cout << "  - Target Coordinate Invariant validated: units with no ground target never march to (0,0)." << std::endl;

        // 2. Siege Mode Out-Of-Range Anti-Vibration:
        int siegeId = app.m_entities->spawnUnitByArchetype(15, 10, 10, Faction::Resistance); // Rhino Deployed (Siege)
        Unit *siegeU = app.m_entities->getUnit(siegeId);
        siegeU->isSiegeMode = true;
        siegeU->moveSpeed = 0.0f;
        siegeU->attackRange = 8.0f;

        int t24FarEnemyId = app.m_entities->spawnUnitByArchetype(0, 30, 30, Faction::Confederation);
        app.m_entities->issueAttackOrder({siegeId}, t24FarEnemyId, app.m_pathfinder.get());
        if(siegeU->state != ActionState::Idle || siegeU->targetEntityId != 0 || siegeU->animFrame != 0)
        {
            std::cerr << "[Test] Error: Siege unit out-of-range order did not reject cleanly! state=" << static_cast<int>(siegeU->state) << " targetId=" << siegeU->targetEntityId << std::endl;
            return false;
        }
        for(int t = 0; t < 10; ++t)
        {
            app.m_entities->update(0.1f, *app.m_map, *app.m_pathfinder);
            if(siegeU->state == ActionState::Attack || siegeU->animFrame != 0)
            {
                std::cerr << "[Test] Error: Siege unit vibrated/animated while out of range! state=" << static_cast<int>(siegeU->state) << " animFrame=" << siegeU->animFrame << std::endl;
                return false;
            }
        }
        std::cout << "  - Siege Mode Out-Of-Range Anti-Vibration validated: stays perfectly Idle with 0 vibration." << std::endl;

        // 3. 4-way pathfinding mode (allowDiagonal = false) still produces strictly orthogonal routes:
        std::vector<PathNode> t24FortPath;
        bool t24FortPartial = false;
        if(!app.m_pathfinder->findPath(10, 10, 20, 20, t24FortPath, 0, &t24FortPartial, false))
        {
            std::cerr << "[Test] Error: Could not compute orthogonal path for Fortress!" << std::endl;
            return false;
        }
        int curX = 10, curY = 10;
        for(const auto &node : t24FortPath)
        {
            int dx = std::abs(node.x - curX);
            int dy = std::abs(node.y - curY);
            if(dx > 0 && dy > 0)
            {
                std::cerr << "[Test] Error: 4-way path contains diagonal step (" << curX << "," << curY << ") -> (" << node.x << "," << node.y << ")!" << std::endl;
                return false;
            }
            curX = node.x;
            curY = node.y;
        }
        std::cout << "  - 4-way pathfinding mode validated: strictly 0 diagonal steps in " << t24FortPath.size() << " waypoints." << std::endl;

        // 4. Strict Depth-1 Support Assist Graph:
        int t24EnemyXId = app.m_entities->spawnUnitByArchetype(0, 10, 15, Faction::Confederation);
        int t24AllyAId = app.m_entities->spawnUnitByArchetype(0, 13, 15, Faction::Resistance);
        int t24AllyBId = app.m_entities->spawnUnitByArchetype(0, 16, 15, Faction::Resistance);
        int t24AllyCId = app.m_entities->spawnUnitByArchetype(0, 19, 15, Faction::Resistance);

        Unit *t24AllyA = app.m_entities->getUnit(t24AllyAId);
        Unit *t24AllyB = app.m_entities->getUnit(t24AllyBId);
        Unit *t24AllyC = app.m_entities->getUnit(t24AllyCId);

        t24AllyA->attackRange = 3.5f;
        t24AllyB->attackRange = 3.5f;
        t24AllyC->attackRange = 3.5f;

        // Ally A is frontline at (13, 15), directly engaged with Enemy X at (10, 15) (dist = 3 <= 3.5)
        t24AllyA->targetEntityId = t24EnemyXId;
        t24AllyA->isDirectEngaged = true;
        t24AllyA->isSupportAssisting = false;

        // Ally B at (16, 15): Enemy X is 6 tiles away (out of range), but Ally A is 3 tiles away (in range).
        // Ally C at (19, 15): Ally B is 3 tiles away, but Ally A is 6 tiles away (out of range).
        app.m_entities->update(0.1f, *app.m_map, *app.m_pathfinder);

        if(t24AllyB->targetEntityId != t24EnemyXId || !t24AllyB->isSupportAssisting)
        {
            std::cerr << "[Test] Error: Ally B did not assist frontline Ally A! target=" << t24AllyB->targetEntityId << " isSupportAssisting=" << t24AllyB->isSupportAssisting << std::endl;
            return false;
        }
        if(t24AllyC->targetEntityId != 0)
        {
            std::cerr << "[Test] Error: Ally C assisted Ally B, violating strict depth = 1! target=" << t24AllyC->targetEntityId << std::endl;
            return false;
        }
        std::cout << "  - Strict Depth-1 Support Assist Graph validated: Ally B assists A, but Ally C does not chain off B." << std::endl;

        // 25. Test Comprehensive Architectural Fixes (AI Auto-Init, Scaled 10/14 A*, Local Sidestep, Wall Faction)
        std::cout << "[Test] Validating Comprehensive Architectural Solutions (Auto-Init, Scaled A*, Sidestep, Wall Factions)..." << std::endl;

        // 1) Automatic AI Initialization on Map Load (via setOnLoaded hook)
        if(!app.m_mission->loadMission(7, "assets", *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder))
        {
            std::cerr << "[Test] Error: Failed to load Mission 7 (Skirmish map)!" << std::endl;
            return false;
        }
        if(!app.m_aiController->isInitialized())
        {
            std::cerr << "[Test] Error: AIController was not initialized automatically on mission load!" << std::endl;
            return false;
        }
        if(app.m_mission->getEnemyHqX() <= 0 || app.m_mission->getEnemyHqY() <= 0)
        {
            std::cerr << "[Test] Error: Mission enemy HQ position is invalid (0, 0)!" << std::endl;
            return false;
        }
        std::cout << "  - Automatic AI Initialization & Base Coordinates validated: Skirmish/TestMap auto-hooks AIController::init." << std::endl;

        // 2) Scaled 10/14 Octile A* in brain_map: 5-step diagonal path should have exactly 5 diagonal steps (no staircases)
        app.m_map->initBlankArena(40, 40, 0);
        app.m_pathfinder->setMap(app.m_map.get());
        std::vector<PathNode> diagPath;
        bool diagPartial = false;
        if(!app.m_pathfinder->findPath(10, 10, 15, 15, diagPath, 0, &diagPartial, true) || diagPath.empty())
        {
            std::cerr << "[Test] Error: Could not find diagonal path in open arena!" << std::endl;
            return false;
        }
        if(diagPath.size() != 5)
        {
            std::cerr << "[Test] Error: 8-way diagonal path from (10,10) to (15,15) expected 5 steps, got " << diagPath.size() << std::endl;
            return false;
        }
        for(size_t i = 0; i < diagPath.size(); ++i)
        {
            int expectedX = 11 + static_cast<int>(i);
            int expectedY = 11 + static_cast<int>(i);
            if(diagPath[i].x != expectedX || diagPath[i].y != expectedY)
            {
                std::cerr << "[Test] Error: Diagonal path deviated into staircase at step " << i << ": (" << diagPath[i].x << "," << diagPath[i].y << ") vs (" << expectedX << "," << expectedY << ")" << std::endl;
                return false;
            }
        }
        std::cout << "  - Scaled 10/14 Octile A* Navigation validated: smooth 5-step diagonal trajectory without staircases." << std::endl;

        // 3) Wall Faction Breaching Rules:
        // AI unit should breach Resistance walls, but NEVER breach friendly Confederation walls!
        app.m_entities->clear();
        app.m_map->initBlankArena(40, 40, 0);
        app.m_pathfinder->setMap(app.m_map.get());

        // Place a Confederation (friendly) wall at (12, 10)
        app.m_map->setWall(12, 10, Faction::Confederation, 100);
        int aiUnitId = app.m_entities->spawnUnitByArchetype(1, 10, 10, Faction::Confederation);
        Unit *aiUnit = app.m_entities->getUnit(aiUnitId);
        aiUnit->isAttackMoving = true;
        aiUnit->path = {{11, 10}, {12, 10}, {13, 10}};
        aiUnit->pathIndex = 1; // Attempt to enter (12, 10)

        app.m_entities->update(0.1f, *app.m_map, *app.m_pathfinder);
        if(aiUnit->targetWallTileX == 12 && aiUnit->targetWallTileY == 10)
        {
            std::cerr << "[Test] Error: AI attacked its own friendly Confederation wall!" << std::endl;
            return false;
        }

        // Now a hostile Resistance wall seals column X=12 completely: no way around, so it must be breached
        // (a lone wall tile with an open route around it is walked around instead - see the wall detour tests)
        for(int y = 0; y < app.m_map->getHeight(); ++y)
            app.m_map->setWall(12, y, Faction::Resistance, 100);
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

        app.m_entities->update(0.1f, *app.m_map, *app.m_pathfinder);
        if(aiUnit->targetWallTileX != 12 || aiUnit->targetWallTileY != 10)
        {
            std::cerr << "[Test] Error: AI failed to target hostile Resistance wall for breach! targetWall=(" << aiUnit->targetWallTileX << "," << aiUnit->targetWallTileY << ")" << std::endl;
            return false;
        }
        std::cout << "  - Wall Faction Breach Rules validated: hostile walls breached, friendly walls preserved." << std::endl;

        // 4) Instant Local Sidestepping & Non-Crossing Group Movement:
        app.m_entities->clear();
        app.m_map->initBlankArena(40, 40, 0);
        app.m_pathfinder->setMap(app.m_map.get());

        int leadId = app.m_entities->spawnUnitByArchetype(0, 15, 10, Faction::Resistance);
        int followerId = app.m_entities->spawnUnitByArchetype(0, 14, 10, Faction::Resistance);
        Unit *leadU = app.m_entities->getUnit(leadId);
        Unit *followU = app.m_entities->getUnit(followerId);

        app.m_pathfinder->setEntityManager(app.m_entities.get());
        leadU->state = ActionState::Move;
        std::vector<PathNode> independentPath;
        if(!app.m_pathfinder->findPath(14, 10, 16, 10, independentPath, followerId, nullptr, false) || independentPath.empty() || !(independentPath.front() == PathNode {15, 10}))
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

        app.m_entities->update(0.016f, *app.m_map, *app.m_pathfinder);
        // Follower must sidestep into (14, 9) or (14, 11) or (15, 9) or (15, 11) in 1 frame without freezing
        if(followU->path.empty() || (followU->path[followU->pathIndex].x == 15 && followU->path[followU->pathIndex].y == 10))
        {
            std::cerr << "[Test] Error: Follower did not sidestep parked ally! next waypoint=(" << followU->path[followU->pathIndex].x << "," << followU->path[followU->pathIndex].y << ")" << std::endl;
            return false;
        }
        if(followU->waitBlockedTimer > 0.05f)
        {
            std::cerr << "[Test] Error: Follower froze in waitBlockedTimer instead of local sidestep! timer=" << followU->waitBlockedTimer << std::endl;
            return false;
        }
        std::cout << "  - Instant Local Sidestepping validated: moving unit detoured around parked ally on frame 1 without stall." << std::endl;

        // 25b. Construction next to the enemy, demolition, range technologies across siege forms
        std::cout << "[Test] Validating construction rules, demolition and siege research..." << std::endl;
        {
            app.m_entities->clear();
            app.m_map->initBlankArena(40, 40, 0);
            app.m_pathfinder->setMap(app.m_map.get());
            const Faction me = playerFaction();
            const Faction foe = aiFaction();
            app.m_entities->spawnBuildingByIndex(me == Faction::Resistance ? 8 : 0, 5, 5, me, 1.0f);
            if(!app.m_entities->canPlaceBuilding(12, 5, 2, 2, *app.m_map, Players::ownerOf(me)))
            {
                std::cerr << "[Test] Error: free spot next to the HQ must be buildable!" << std::endl;
                return false;
            }
            const int block = app.m_map->getConstructionRules().enemyBlockTiles;
            const int enemyId = app.m_entities->spawnUnitByArchetype(1, 13 + block, 5, foe); // `block` tiles from the footprint
            if(app.m_entities->canPlaceBuilding(12, 5, 2, 2, *app.m_map, Players::ownerOf(me)))
            {
                std::cerr << "[Test] Error: building " << block << " tiles from an enemy must be refused!" << std::endl;
                return false;
            }
            app.m_entities->getUnit(enemyId)->tileX = 14 + block;
            if(!app.m_entities->canPlaceBuilding(12, 5, 2, 2, *app.m_map, Players::ownerOf(me)))
            {
                std::cerr << "[Test] Error: an enemy " << block + 1 << " tiles away must not block construction!" << std::endl;
                return false;
            }

            const int barracksId = app.m_entities->spawnBuildingByIndex(me == Faction::Resistance ? 9 : 1, 20, 20, me, 1.0f);
            Building *barracks = app.m_entities->getBuilding(barracksId);
            barracks->productionQueue.push_back({0, 120, 5.0f, 1.0f});
            const int refund = app.m_entities->demolishBuilding(barracksId, *app.m_map);
            if(refund != 120 || barracks->state != ActionState::Die || !app.m_map->isPassable(21, 21))
            {
                std::cerr << "[Test] Error: demolition must destroy the building, free its tiles and refund its queue! refund=" << refund << std::endl;
                return false;
            }
            std::cout << "  - Construction blocked within " << block << " tiles of an enemy; demolition refunds " << refund << std::endl;

            // Snipers + Hornet-10: 9.5 in normal form, the concealed form keeps its +2 (11.5) after the change
            resetResearch();
            completeResearch(Players::ownerOf(Faction::Resistance), 5, *app.m_entities);
            completeResearch(Players::ownerOf(Faction::Resistance), 6, *app.m_entities);
            const int sniperId = app.m_entities->spawnUnitByArchetype(4, 30, 30, Faction::Resistance);
            Unit *sniper = app.m_entities->getUnit(sniperId);
            const float siegeRange = researchedAttackRange(Players::ownerOf(Faction::Resistance), 14, app.m_entities->getUnitConfig(14)->attackRange);
            if(siegeRange <= sniper->attackRange)
            {
                std::cerr << "[Test] Error: researched siege range " << siegeRange << " must stay above the normal " << sniper->attackRange << std::endl;
                return false;
            }
            app.m_entities->toggleSiegeMode({sniperId});
            for(int i = 0; i < 20 && sniper->archetypeId != 14; ++i)
                app.m_entities->update(0.1f, *app.m_map, *app.m_pathfinder);
            if(sniper->archetypeId != 14 || std::abs(sniper->attackRange - siegeRange) > 0.01f)
            {
                std::cerr << "[Test] Error: deployed sniper lost its technologies! range=" << sniper->attackRange << " expected " << siegeRange << std::endl;
                return false;
            }
            resetResearch();
            std::cout << "  - Sniper range " << siegeRange << " kept in siege form after research" << std::endl;
        }

        // 26. Group movement quality (see runGroupMovementTest)
        if(!runGroupMovementTest(app))
            return false;

        // 27. AI waves, squads, chokepoints and research (see runAIBehaviourTest)
        if(!runAIBehaviourTest(app))
            return false;

        // 28. Lockstep groundwork: same commands, same world (see runDeterminismTest)
        if(!runDeterminismTest(app))
            return false;

        // 29. Several players, teams, per-player economies (see runMatchTest)
        if(!runMatchTest(app))
            return false;

        // 30. Computer players in match slots (see runMatchAiTest)
        if(!runMatchAiTest(app))
            return false;

        std::cout << "[Test] Simulation completed successfully! Stats:" << std::endl;
        std::cout << "  - Player Credits: " << app.m_economy->getCredits() << std::endl;
        std::cout << "  - Player Power: " << app.m_economy->getPowerProduced() << "/" << app.m_economy->getPowerConsumed() << std::endl;
        std::cout << "  - Player Kills: " << app.m_entities->getPlayerKillCount() << std::endl;
        std::cout << "  - Total Entities remaining: " << app.m_entities->getAllEntities().size() << std::endl;
        std::cout << "[Test] Test passed with 0 errors!" << std::endl;
        // 23. A computer player never shuts its own base in (the only passage of a room must stay open)
        std::cout << "[Test] Validating that the AI keeps the exit of its base open..." << std::endl;
        {
            EntityManager &em = *app.m_entities;
            TileMap &map = *app.m_map;
            const int roomW = 9, roomH = 7;
            int ox = -1, oy = -1;
            for(int y = 2; y + roomH + 2 < map.getHeight() && ox < 0; ++y)
            {
                for(int x = 2; x + roomW + 2 < map.getWidth() && ox < 0; ++x)
                {
                    bool clear = true;
                    for(int ty = y - 1; ty <= y + roomH && clear; ++ty)
                    {
                        for(int tx = x - 1; tx <= x + roomW && clear; ++tx)
                        {
                            clear = map.isPassable(tx, ty) && map.getWall(tx, ty) == 0;
                            for(const auto &e : em.getAllEntities())
                            {
                                const Building *b = e.asBuilding();
                                if(b && e.alive && tx >= b->tileX - 1 && tx <= b->tileX + b->buildWidth && ty >= b->tileY - 1 && ty <= b->tileY + b->buildHeight)
                                    clear = false;
                            }
                        }
                    }
                    if(clear)
                    {
                        ox = x;
                        oy = y;
                    }
                }
            }
            if(ox < 0)
            {
                std::cout << "  - (no free 9x7 area on this map: base-exit check skipped)" << std::endl;
            }
            else
            {
                // Walls around the room, one door in the middle of the east side
                const int doorY = oy + roomH / 2;
                std::vector<std::pair<int, int>> walls;
                for(int x = ox - 1; x <= ox + roomW; ++x)
                {
                    walls.push_back({x, oy - 1});
                    walls.push_back({x, oy + roomH});
                }
                for(int y = oy; y < oy + roomH; ++y)
                {
                    walls.push_back({ox - 1, y});
                    if(y != doorY)
                        walls.push_back({ox + roomW, y});
                }
                for(const auto &w : walls)
                    map.setWall(w.first, w.second, Faction::Neutral, 100);
                const PlayerId ai = Players::ownerOf(Faction::Confederation);
                const int hqId = em.spawnBuildingByIndex(0, ox + 2, oy + 2, Faction::Confederation, 1.0f);
                const bool shutsDoor = em.keepsBaseOpen(ox + roomW - 1, doorY, 1, 1, map, ai);
                const bool cornerOk = em.keepsBaseOpen(ox + roomW - 2, oy + roomH - 2, 1, 1, map, ai);
                const bool outsideOk = em.keepsBaseOpen(ox + roomW + 3, doorY + 2, 2, 2, map, ai);
                if(Building *hq = em.getBuilding(hqId))
                    hq->alive = false;
                for(const auto &w : walls)
                    map.setWall(w.first, w.second, Faction::Neutral, 0);
                if(shutsDoor || !cornerOk || !outsideOk)
                {
                    std::cerr << "[Test] Error: base-exit check wrong: door blocked=" << !shutsDoor << " (want 1), corner ok=" << cornerOk << ", outside ok=" << outsideOk << std::endl;
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
            const Faction enemy = Players::local() == Players::ownerOf(Faction::Resistance) ? Faction::Confederation : Faction::Resistance;
            const bool fogWas = map.isFogEnabled();
            map.setFogEnabled(true);
            const int b1 = em.spawnBuildingByIndex(enemy == Faction::Confederation ? 2 : 10, 8, 8, enemy, 0.3f);  // under construction
            const int b2 = em.spawnBuildingByIndex(enemy == Faction::Confederation ? 2 : 10, 20, 8, enemy, 1.0f); // finished
            const Building *under = em.getBuilding(b1);
            const Building *done = em.getBuilding(b2);
            map.resetAllFog(FogState::Explored); // explored ground, nothing in sight, nothing seen
            const bool neverSeenHidden = !em.isHostileBuildingShown(*under, map) && !em.isHostileBuildingShown(*done, map);
            map.resetAllFog(FogState::Visible); // everything in sight
            bool inSight = false;
            const bool shownInSight = em.isHostileBuildingShown(*under, map, &inSight) && inSight && em.isHostileBuildingShown(*done, map);
            map.resetAllFog(FogState::Explored);                                   // out of sight again
            const bool finishedStaysKnown = em.isHostileBuildingShown(*done, map); // seen before and finished: still shown
            const bool progressHidden = !em.isHostileBuildingShown(*under, map);   // construction out of sight: hidden
            map.setFogEnabled(fogWas);
            if(Building *hq = em.getBuilding(b1))
                hq->alive = false;
            if(Building *hq = em.getBuilding(b2))
                hq->alive = false;
            if(!neverSeenHidden || !shownInSight || !finishedStaysKnown || !progressHidden)
            {
                std::cerr << "[Test] Error: enemy structures in the fog: never seen hidden=" << neverSeenHidden << ", shown in sight=" << shownInSight << ", finished stays known=" << finishedStaysKnown << ", construction hidden=" << progressHidden << std::endl;
                return false;
            }
            std::cout << "  - Enemy structures show only after they were seen; construction out of sight stays hidden." << std::endl;
        }

        // 25. A map with buildings of its own: prebuilt bases or a start from scratch
        std::cout << "[Test] Validating prebuilt bases and the start from scratch..." << std::endl;
        {
            const std::string path = "assets/maps/testmap.json";
            bool listed = false;
            for(const SkirmishMapInfo &m : MissionController::listSkirmishMaps())
            {
                if(m.path == path)
                    listed = m.hasPrebuilt;
            }
            auto countBuildings = [&]()
            {
                int n = 0;
                for(const auto &e : app.m_entities->getAllEntities())
                    n += (e.alive && e.asBuilding()) ? 1 : 0;
                return n;
            };
            auto countWalls = [&]()
            {
                int n = 0;
                for(int y = 0; y < app.m_map->getHeight(); ++y)
                    for(int x = 0; x < app.m_map->getWidth(); ++x)
                        n += app.m_map->isWall(x, y) ? 1 : 0;
                return n;
            };
            const bool prebuiltOk = app.m_mission->loadSkirmish(path, *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder, true);
            const int prebuiltBuildings = countBuildings(), prebuiltWalls = countWalls();
            const bool scratchOk = app.m_mission->loadSkirmish(path, *app.m_map, *app.m_entities, *app.m_economy, *app.m_pathfinder, false);
            const int scratchBuildings = countBuildings(), scratchWalls = countWalls();
            if(!listed || !prebuiltOk || !scratchOk || prebuiltBuildings < 40 || prebuiltWalls < 100 || scratchBuildings > 8 || scratchWalls != 0)
            {
                std::cerr << "[Test] Error: prebuilt/scratch start: listed=" << listed << " buildings " << prebuiltBuildings << "/" << scratchBuildings << " walls " << prebuiltWalls << "/" << scratchWalls << std::endl;
                return false;
            }
            // Effects of destruction come from assets/characters/effects.yaml (defaults = the original numbers)
            {
                const EffectsConfig &fx = EffectsConfig::get();
                EffectsConfig custom;
                const bool parsed = custom.loadText("explosions:\n  frame_time: 0.1\n  vehicle:\n    main: [20, 21]\nwrecks:\n  burial_seconds: 60\n  light_archetypes: []\n");
                if(fx.wreckOfVehicle(6).composite != 1759 || fx.wreckOfVehicle(10).stages != 4 || fx.wreckOfVehicle(8).composite != 1764 || fx.wreckOfBuilding(2, 2).composite != 1741 || fx.wreckOfBuilding(4, 3).composite != 1753 || fx.burialSeconds < 100.0f || !parsed ||
                   custom.vehicleMain.size() != 2 || custom.wreckOfVehicle(6).composite != 1764 || custom.burialSeconds != 60.0f || custom.frameTime != 0.1f)
                {
                    std::cerr << "[Test] Error: effects.yaml (wrecks / explosions)" << std::endl;
                    return false;
                }
            }
            std::cout << "  - Prebuilt start: " << prebuiltBuildings << " buildings, " << prebuiltWalls << " wall tiles; from scratch: " << scratchBuildings << " buildings, no walls." << std::endl;
        }

        return true;
    }

} // namespace OpenArtOfWar
