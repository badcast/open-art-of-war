#include "game/MissionController.hpp"
#include <array>
#include <functional>
#include "game/Players.hpp"
#include "core/DetMath.hpp"
#include "core/AssetStore.hpp"
#include "core/Utility.hpp"
#include "game/Missions.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include <utility>
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/Pathfinder.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <queue>
#include <tuple>

namespace OpenArtOfWar
{

    const std::vector<MissionInfo> &MissionController::getAvailableMissions()
    {
        static const auto infosOf = [](const std::vector<MissionDef> &defs)
        {
            std::vector<MissionInfo> infos;
            for(const auto &def : defs)
                infos.push_back(def.info);
            return infos;
        };
        static const std::vector<MissionInfo> s_resistance = infosOf(Missions::resistance());
        static const std::vector<MissionInfo> s_confederation = infosOf(Missions::confederation());
        return playerFaction() == Faction::Confederation ? s_confederation : s_resistance;
    }

    MissionController::MissionController()
    {
    }

    void MissionController::spawnSquadFromCmd27(int group, int x, int y, int val, TileMap &map, EntityManager &entities, Pathfinder *pathfinder)
    {
        // Groups 0..2 are the computer's squads, 3 and up the player's - in the Peru campaign as well as in the
        // Confederation one (there the player commands the Confederation)
        Faction fac = (group < 3) ? aiFaction() : playerFaction();
        std::vector<int> spawnedIds;

        auto spawnArch = [&](int arch, int idx)
        {
            int ox = (idx % 4) - 1;
            int oy = (idx / 4) - 1;
            int tx = std::clamp(x + ox, 1, std::max(1, map.getWidth() - 2));
            int ty = std::clamp(y + oy, 1, std::max(1, map.getHeight() - 2));
            int uid = entities.spawnUnitByArchetype(arch, tx, ty, fac);
            if(uid > 0)
            {
                spawnedIds.push_back(uid);
            }
        };

        // Defense missions send the enemy's squads trimmed to the difficulty (an Easy squad of 10 brings 4)
        const int squadPercent = (m_defenseMission && fac == aiFaction()) ? m_defenseSquadPercent : 100;
        auto squadSize = [&](int cnt) { return std::max(1, (cnt * squadPercent + 50) / 100); };

        int spawnIdx = 0;
        if(val >= 72)
        {
            int cnt = squadSize((val == 72) ? 1 : (val == 73 ? 3 : 6));
            int arch = (fac == Faction::Confederation) ? 18 : 8; // Fortress (18) or Armadillo (8)
            for(int k = 0; k < cnt; ++k)
            {
                spawnArch(arch, spawnIdx++);
            }
        }
        else
        {
            int i2 = val % 3;
            int i3 = (val % 30) / 3;
            int i4 = val / 30;
            int nStart = i4 / 2;
            int nEnd = 1 - (i4 & 1);

            for(int N = nStart; N <= nEnd; ++N)
            {
                if(N == 0)
                {
                    // Infantry squad (i.java:3955-3970)
                    int cnt = squadSize((i2 == 0) ? 3 : (i2 == 1 ? 6 : 10));
                    for(int k = 0; k < cnt; ++k)
                    {
                        int arch = 0;
                        if(i3 == 0)
                        {
                            arch = (fac == Faction::Confederation) ? 1 : 0; // Rifleman
                        }
                        else if(i3 == 1)
                        {
                            arch = (fac == Faction::Confederation) ? 3 : 2; // HeavyAssault / Grenadier
                        }
                        else if(i3 == 2)
                        {
                            arch = (fac == Faction::Confederation) ? 5 : 4; // Flamethrower / Sniper
                        }
                        else
                        {
                            int r = k % 3;
                            arch = (fac == Faction::Confederation) ? (r * 2 + 1) : (r * 2);
                        }
                        spawnArch(arch, spawnIdx++);
                    }
                }
                else
                {
                    // Vehicle squad (i.java:3971-3984)
                    int cnt = squadSize((i2 == 0) ? 2 : (i2 == 1 ? 4 : 6));
                    for(int k = 0; k < cnt; ++k)
                    {
                        int arch = 6;
                        if(i3 == 0)
                        {
                            arch = (fac == Faction::Confederation) ? 9 : 6; // Hammer (9) / Coyote (6)
                        }
                        else if(i3 == 1)
                        {
                            if(fac == Faction::Confederation)
                            {
                                arch = (k % 2 == 0) ? 18 : 17; // Fortress (18) / Zeus (17)
                            }
                            else
                            {
                                arch = (k % 2 == 0) ? 8 : 10; // Armadillo (8) / Rhino (10)
                            }
                        }
                        else if(i3 == 2)
                        {
                            if(fac == Faction::Confederation)
                            {
                                arch = (k % 2 == 0) ? 18 : 13; // Fortress (18) / Grad (13)
                            }
                            else
                            {
                                arch = (k % 2 == 0) ? 10 : 12; // Rhino (10) / Porcupine (12)
                            }
                        }
                        else
                        {
                            if(fac == Faction::Confederation)
                            {
                                const int confVeh[] = {9, 18, 17, 13};
                                arch = confVeh[k % 4];
                            }
                            else
                            {
                                const int resVeh[] = {6, 8, 10, 12};
                                arch = resVeh[k % 4];
                            }
                        }
                        spawnArch(arch, spawnIdx++);
                    }
                }
            }
        }

        // Reinforcements of the computer's side march on the player's base right away
        if(pathfinder && fac == aiFaction() && !spawnedIds.empty())
        {
            entities.issueAttackMoveOrder(spawnedIds, m_playerHqTileX, m_playerHqTileY, *pathfinder);
        }
    }

    void MissionController::setDefensePacing(float firstWaveSeconds, float waveGapSeconds, int squadPercent, float timeScale)
    {
        m_defenseFirstWave = std::max(0.0f, firstWaveSeconds);
        m_defenseWaveGap = std::max(1.0f, waveGapSeconds);
        m_defenseSquadPercent = std::clamp(squadPercent, 10, 100);
        m_defenseTimeScale = std::clamp(timeScale, 0.25f, 4.0f);
        if(!m_defenseMission)
            return;
        for(auto &wave : m_scheduledWaves)
        {
            if(wave.spawned || wave.group >= 3)
                continue;
            if(wave.defenseOrder >= 0)
            {
                wave.triggerTime = m_defenseFirstWave + m_defenseWaveGap * static_cast<float>(wave.defenseOrder);
            }
            else
            {
                wave.triggerTime = std::max(m_defenseFirstWave, wave.scriptTime * m_defenseTimeScale);
            }
        }
    }

    bool MissionController::loadMission(int missionId, const std::string &assetRoot, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder)
    {
        ++m_loadSerial;
        resetResearch();
        m_customMapPath.clear();
        const auto &mlist = getAvailableMissions();
        if(missionId < 0 || missionId >= static_cast<int>(mlist.size()))
        {
            missionId = 0;
        }
        m_currentMissionId = missionId;
        const auto &minfo = mlist[missionId];
        m_missionName = minfo.name;
        m_briefingText = minfo.briefing;
        m_objectiveText = minfo.objective;
        m_missionTime = 0.0f;
        m_waveTimer = 0.0f;
        m_waveCount = 0;
        m_scheduledWaves.clear();
        m_defenseMission = false;
        m_hasDestObjective = false;
        m_isVictory = false;
        m_isDefeat = false;

        // Reset HQ coordinates before parsing mission entities
        m_playerHqTileX = 8;
        m_playerHqTileY = 8;
        m_enemyHqTileX = 48;
        m_enemyHqTileY = 32;

        entities.clear();
        economy.reset(1500);

        // 1. Mission definition of the campaign the player's side belongs to (Missions*.cpp)
        const MissionData &jmi = Missions::campaign(playerFaction())[missionId].data;
        const int mapIndex = jmi.mapIndex;
        bool foundPlayerBase = false;
        bool foundPlayerHq = false;
        bool foundEnemyBase = false;
        bool foundEnemyHq = false;
        bool hadMissionScript = false;

        // Walls take the side of the nearer base
        auto setWallZone = [&](const Rect &r)
        {
            float distP = DetMath::hypot(static_cast<float>(r.x - m_playerHqTileX), static_cast<float>(r.y - m_playerHqTileY));
            float distE = DetMath::hypot(static_cast<float>(r.x - m_enemyHqTileX), static_cast<float>(r.y - m_enemyHqTileY));
            Faction wallFac = (distE <= distP) ? Faction::Confederation : Faction::Resistance;
            for(int ty = r.y; ty < r.y + r.h; ++ty)
            {
                for(int tx = r.x; tx < r.x + r.w; ++tx)
                {
                    map.setWall(tx, ty, wallFac, 100);
                }
            }
        };

        // 2. Load corresponding map
        std::string mapPath = assetRoot + "/maps/" + jmi.mapFile + ".json";
        if(!map.loadMap(mapPath) && !map.loadMap("maps/" + jmi.mapFile + ".json"))
        {
            std::cerr << "[MissionController] Failed to load map: " << mapPath << std::endl;
            return false;
        }
        pathfinder.setMap(&map);

        // 3. Spawn preplaced authentic structures and walls from the mission
        for(const auto &item : jmi.entities)
        {
            const int etype = item.type, ex = item.x, ey = item.y;
            if(etype < 15)
            {
                // Authentic faction mapping: 0..7 = Confederation (Blue, Enemy), 8..14 = Resistance (Red, Player)
                Faction faction = (etype >= 8) ? Faction::Resistance : Faction::Confederation;
                int bid = entities.spawnBuildingByIndex(etype, ex, ey, faction, 1.0f);
                if(bid > 0)
                {
                    const Building *bEnt = entities.getBuilding(bid);
                    if(bEnt)
                    {
                        for(int ty = ey; ty < ey + bEnt->buildHeight && ty < map.getHeight(); ++ty)
                        {
                            for(int tx = ex; tx < ex + bEnt->buildWidth && tx < map.getWidth(); ++tx)
                            {
                                map.setPassable(tx, ty, false);
                            }
                        }
                    }
                    if(faction == Faction::Resistance)
                    {
                        if(!foundPlayerBase || (etype == 8 && !foundPlayerHq))
                        {
                            m_playerHqTileX = ex;
                            m_playerHqTileY = ey;
                            foundPlayerBase = true;
                            if(etype == 8)
                                foundPlayerHq = true;
                        }
                    }
                    else
                    {
                        if(!foundEnemyBase || (etype == 0 && !foundEnemyHq))
                        {
                            m_enemyHqTileX = ex;
                            m_enemyHqTileY = ey;
                            foundEnemyBase = true;
                            if(etype == 0)
                                foundEnemyHq = true;
                        }
                    }
                }
            }
            else
            {
                setWallZone({ex, ey, 1, 1});
            }
        }

        // 3b. Pre-configured mission wall zones with base proximity faction assignment
        for(const auto &grp : jmi.wallZones)
        {
            for(const auto &r : grp)
                setWallZone(r);
        }

        // 4. Process authentic mission scripts (credits, wall zones, initial squads, scheduled waves, objectives)
        int defenseWaves = 0;
        m_defenseMission = jmi.defense;
        // The original runs the commands one after another: each delay counts from the previous command
        // (i.java:3848), so a squad's time is the sum of all delays before it, not its own delay
        float scriptClock = 0.0f;
        if(!jmi.scripts.empty())
        {
            hadMissionScript = true;
            for(const auto &sc : jmi.scripts)
            {
                const int cmd = sc.cmd, group = sc.group, delay = sc.delay;
                const int sx = sc.x, sy = sc.y, val = sc.val;
                scriptClock += static_cast<float>(std::max(0, delay));

                if(cmd == 25)
                {
                    // Starting credits (i.java:3938)
                    int startCreds = std::max(1000, sy * 10);
                    economy.reset(startCreds);
                }
                else if(cmd == 33 && val == 1)
                {
                    // Activate wall zone (i.java:3996)
                    if(group >= 0 && group < static_cast<int>(jmi.wallZones.size()))
                    {
                        for(const auto &r : jmi.wallZones[group])
                            setWallZone(r);
                    }
                }
                else if(cmd == 40 && group >= 3)
                {
                    // Destination zone objective for player units (i.java:4030)
                    m_hasDestObjective = true;
                    m_destX = sx;
                    m_destY = sy;
                    m_destSize = std::max(3, val);
                }
                else if(cmd == 27)
                {
                    // Unit squad spawn (i.java:3945-3985)
                    // Defense missions: the enemy's starting squads arrive as waves from their spawn points
                    const bool atStart = scriptClock <= 0.0f;
                    if(atStart)
                    {
                        if(jmi.defense && group < 3)
                        {
                            // Timed by setDefensePacing (difficulty); these defaults hold until it is called
                            const int order = defenseWaves++;
                            m_scheduledWaves.push_back({m_defenseFirstWave + m_defenseWaveGap * order, group, sx, sy, val, false, order, 0.0f});
                        }
                        else
                        {
                            spawnSquadFromCmd27(group, sx, sy, val, map, entities, nullptr);
                        }
                    }
                    else
                    {
                        const float at = (jmi.defense && group < 3) ? std::max(m_defenseFirstWave, scriptClock * m_defenseTimeScale) : scriptClock;
                        m_scheduledWaves.push_back({at, group, sx, sy, val, false, -1, scriptClock});
                    }
                    if(delay == 0)
                    {
                        // Bases are collected with the Resistance as "player" and swapped at the end when the
                        // player commands the Confederation, so the player's squads go to the other slot then
                        const bool playerSlot = (group >= 3) == (playerFaction() == Faction::Resistance);
                        if(playerSlot && !foundPlayerBase)
                        {
                            m_playerHqTileX = sx;
                            m_playerHqTileY = sy;
                            foundPlayerBase = true;
                        }
                        else if(!playerSlot && !foundEnemyBase)
                        {
                            m_enemyHqTileX = sx;
                            m_enemyHqTileY = sy;
                            foundEnemyBase = true;
                        }
                    }
                }
            }
        }

        // Fallback only if mission had neither preplaced entities nor scripts
        if(!foundPlayerBase && !hadMissionScript)
        {
            m_playerHqTileX = 8;
            m_playerHqTileY = 8;
            entities.spawnBuildingByIndex(8, 8, 8, Faction::Resistance, 1.0f);   // Штаб Сопротивления (3x4)
            entities.spawnBuildingByIndex(9, 13, 8, Faction::Resistance, 1.0f);  // Генератор (3x3)
            entities.spawnBuildingByIndex(10, 8, 13, Faction::Resistance, 1.0f); // Казарма (3x3)
        }

        if(!foundEnemyBase && !hadMissionScript)
        {
            m_enemyHqTileX = map.getWidth() - 10;
            m_enemyHqTileY = map.getHeight() - 10;
            entities.spawnBuildingByIndex(0, m_enemyHqTileX, m_enemyHqTileY, Faction::Confederation, 1.0f);     // Штаб Конфедерации (4x4)
            entities.spawnBuildingByIndex(6, m_enemyHqTileX - 3, m_enemyHqTileY, Faction::Confederation, 1.0f); // Локатор (2x2)
        }

        // Add initial guard squads around bases if no script units were spawned for that side
        int playerUnitCount = 0;
        int enemyUnitCount = 0;
        for(const auto &e : entities.getAllEntities())
        {
            if(!e.alive || e.isBuilding)
                continue;
            if(e.faction == Faction::Resistance)
                playerUnitCount++;
            else
                enemyUnitCount++;
        }

        if(playerUnitCount == 0)
        {
            int px = std::clamp(m_playerHqTileX + 4, 2, map.getWidth() - 3);
            int py = std::clamp(m_playerHqTileY + 2, 2, map.getHeight() - 3);
            entities.spawnUnitByArchetype(0, px, py, Faction::Resistance);         // Пехотинец
            entities.spawnUnitByArchetype(0, px, py + 1, Faction::Resistance);     // Пехотинец
            entities.spawnUnitByArchetype(2, px - 1, py + 1, Faction::Resistance); // Гренадер
            entities.spawnUnitByArchetype(6, px + 1, py + 1, Faction::Resistance); // Койот
        }

        if(enemyUnitCount == 0 && foundEnemyBase && !jmi.defense)
        {
            int ex = std::clamp(m_enemyHqTileX - 2, 2, map.getWidth() - 3);
            int ey = std::clamp(m_enemyHqTileY + 1, 2, map.getHeight() - 3);
            entities.spawnUnitByArchetype(1, ex, ey, Faction::Confederation);          // Стрелок
            entities.spawnUnitByArchetype(3, ex, ey + 1, Faction::Confederation);      // Штурмовик
            entities.spawnUnitByArchetype(18, ex - 1, ey + 1, Faction::Confederation); // Крепость
        }

        // The map describes the Resistance base as "player" and the Confederation base as "enemy";
        // playing the Confederation simply swaps the roles
        if(playerFaction() == Faction::Confederation)
        {
            std::swap(m_playerHqTileX, m_enemyHqTileX);
            std::swap(m_playerHqTileY, m_enemyHqTileY);
        }

        // Initialize fog of war for player base and starting units
        map.setViewerTeam(Players::teamOf(Players::local())); // the drawn fog is the local player's from the first frame on
        entities.updateFogOfWar(map);

        std::cout << "[MissionController] Initialized " << m_missionName << " on Map m" << mapIndex << " | Player Pos: (" << m_playerHqTileX << "," << m_playerHqTileY << ")"
                  << " | Enemy Pos: (" << m_enemyHqTileX << "," << m_enemyHqTileY << ")"
                  << " | Scheduled Waves: " << m_scheduledWaves.size() << std::endl;

        if(m_onLoaded)
        {
            m_onLoaded();
        }
        return true;
    }

    void MissionController::update(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
    {
        if(m_isVictory || m_isDefeat)
            return;

        m_missionTime += deltaTime;

        // 1. Trigger scheduled script reinforcement waves (cmd 27 with delay > 0)
        int pendingEnemyWaves = 0;
        for(auto &wave : m_scheduledWaves)
        {
            if(!wave.spawned)
            {
                if(m_missionTime >= wave.triggerTime)
                {
                    wave.spawned = true;
                    spawnSquadFromCmd27(wave.group, wave.x, wave.y, wave.val, map, entities, &pathfinder);
                }
                else if(wave.group < 3)
                {
                    pendingEnemyWaves++;
                }
            }
        }

        // 2. Check Win/Loss conditions
        if(isMatch())
        {
            // A team is out once it has nothing left; the local player wins when only its team remains
            std::array<bool, kMaxPlayers> teamAlive {};
            for(const auto &e : entities.getAllEntities())
            {
                if(!e.alive || e.state == ActionState::Die || e.owner >= kMaxPlayers)
                    continue;
                teamAlive[Players::teamOf(e.owner) % kMaxPlayers] = true;
            }
            const uint8_t mine = Players::teamOf(Players::local()) % kMaxPlayers;
            int others = 0;
            for(int t = 0; t < kMaxPlayers; ++t)
                others += (t != mine && teamAlive[t]) ? 1 : 0;
            if(!teamAlive[mine])
                m_isDefeat = true;
            else if(others == 0)
                m_isVictory = true;
            return;
        }
        int playerBuildingCount = 0;
        int playerUnitCount = 0;
        int enemyBuildingCount = 0;
        int enemyUnitCount = 0;
        bool reachedDestZone = false;

        for(const auto &e : entities.getAllEntities())
        {
            if(!e.alive || e.state == ActionState::Die)
                continue;
            if(e.faction == playerFaction())
            {
                if(e.isBuilding)
                    playerBuildingCount++;
                else
                {
                    playerUnitCount++;
                    if(m_hasDestObjective && std::abs(e.tileX - m_destX) <= m_destSize && std::abs(e.tileY - m_destY) <= m_destSize)
                    {
                        reachedDestZone = true;
                    }
                }
            }
            else if(e.faction == aiFaction())
            {
                if(e.isBuilding)
                    enemyBuildingCount++;
                else
                    enemyUnitCount++;
            }
        }

        if(playerBuildingCount == 0 && playerUnitCount == 0)
        {
            m_isDefeat = true;
            std::cout << "[MissionController] DEFEAT! All player forces destroyed." << std::endl;
            return;
        }

        if((enemyBuildingCount == 0 && enemyUnitCount == 0 && pendingEnemyWaves == 0) || (m_hasDestObjective && reachedDestZone && enemyBuildingCount == 0))
        {
            m_isVictory = true;
            std::cout << "[MissionController] VICTORY! Mission objectives accomplished." << std::endl;
            return;
        }

        // 3. Periodic AI counter-attack wave if enemy has an active base (every 40 seconds)
        // Only used as fallback if AIController is not active (m_useBuiltinWaves == true)
        m_waveTimer += deltaTime;
        if(m_useBuiltinWaves && m_waveTimer >= 40.0f && enemyBuildingCount > 0)
        {
            m_waveTimer = 0.0f;
            m_waveCount++;

            int spawnX = m_enemyHqTileX;
            int spawnY = m_enemyHqTileY;
            for(const auto &e : entities.getAllEntities())
            {
                if(e.alive && e.state != ActionState::Die && e.faction == aiFaction() && e.isBuilding)
                {
                    spawnX = std::clamp(e.tileX - 1, 1, map.getWidth() - 2);
                    spawnY = std::clamp(e.tileY + 1, 1, map.getHeight() - 2);
                    break;
                }
            }

            // Rifleman, rocket infantry, then a medium and a heavy tank of the computer's faction
            const Faction ai = aiFaction();
            const bool conf = (ai == Faction::Confederation);
            const int archRifle = conf ? 1 : 0, archRocket = conf ? 3 : 2, archTank = conf ? 9 : 8, archHeavy = conf ? 11 : 10;
            std::vector<int> waveUnitIds;
            int u1 = entities.spawnUnitByArchetype(archRifle, spawnX, spawnY, ai);
            int u2 = entities.spawnUnitByArchetype(archRocket, spawnX + 1, spawnY, ai);
            if(u1 > 0)
                waveUnitIds.push_back(u1);
            if(u2 > 0)
                waveUnitIds.push_back(u2);
            if(m_waveCount > 1)
            {
                int u3 = entities.spawnUnitByArchetype(archTank, spawnX, spawnY + 1, ai);
                if(u3 > 0)
                    waveUnitIds.push_back(u3);
            }
            if(m_waveCount > 3)
            {
                int u4 = entities.spawnUnitByArchetype(archHeavy, spawnX + 1, spawnY + 1, ai);
                if(u4 > 0)
                    waveUnitIds.push_back(u4);
            }

            if(!waveUnitIds.empty())
            {
                entities.issueAttackMoveOrder(waveUnitIds, m_playerHqTileX, m_playerHqTileY, pathfinder);
                std::cout << "[MissionController] AI Wave " << m_waveCount << " attacking the player base!" << std::endl;
            }
        }
    }

    bool MissionController::loadTestPolygon(TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder)
    {
        return loadTestMap("assets", map, entities, economy, pathfinder);
    }

    std::vector<SkirmishMapInfo> MissionController::listSkirmishMaps()
    {
        std::vector<SkirmishMapInfo> maps;
        for(const std::string &path : Assets::list("assets/maps", ".json"))
        {
            std::string text;
            if(!Assets::readText(path, text))
                continue;
            const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
            if(!doc.is_object() || !doc.contains("terrainRle") || !doc.contains("width") || !doc.contains("height"))
            {
                continue;
            }
            SkirmishMapInfo info;
            info.path = path;
            info.name = std::filesystem::path(path).stem().string();
            if(doc.contains("title") && doc["title"].is_string())
                info.title = doc["title"].get<std::string>();
            info.width = doc["width"].get<int>();
            info.height = doc["height"].get<int>();
            info.tilesetId = doc.value("tilesetId", 0);
            info.hasPrebuilt = (doc.contains("buildings") && doc["buildings"].is_array() && !doc["buildings"].empty()) || (doc.contains("units") && doc["units"].is_array() && !doc["units"].empty());
            maps.push_back(std::move(info));
        }
        // m0, m26, ... first, map_mission_1, ... next, then map_skirmish_1, ... then everything else; numbers compare as numbers
        auto sortKey = [](const SkirmishMapInfo &m)
        {
            const std::string &n = m.name;
            size_t digits = n.find_first_of("0123456789");
            std::string prefix = digits == std::string::npos ? n : n.substr(0, digits);
            const int number = digits == std::string::npos ? 0 : std::atoi(n.c_str() + digits);
            const int group = prefix == "m" ? 0 : (prefix == "map_mission_" ? 1 : (prefix == "map_skirmish_" ? 2 : 3));
            return std::make_tuple(group, prefix, number, n);
        };
        std::sort(maps.begin(), maps.end(), [&](const SkirmishMapInfo &a, const SkirmishMapInfo &b) { return sortKey(a) < sortKey(b); });
        return maps;
    }

    namespace
    {

        // Top-left corner of the free size x size square (open ground, no walls) nearest to (targetX, targetY);
        // with `reachable` the square's center must lie in that set, and it must keep `minApart` tiles (center to
        // center) away from the square at (apartX, apartY). Returns false when there is no such square.
        bool findBaseSite(const TileMap &map, int size, int targetX, int targetY, const std::vector<char> *reachable, int &outX, int &outY, int apartX = 0, int apartY = 0, int minApart = 0)
        {
            const int w = map.getWidth(), h = map.getHeight();
            long best = -1;
            for(int y = 0; y + size <= h; ++y)
            {
                for(int x = 0; x + size <= w; ++x)
                {
                    bool free = true;
                    for(int dy = 0; dy < size && free; ++dy)
                    {
                        for(int dx = 0; dx < size; ++dx)
                        {
                            if(!map.isPassable(x + dx, y + dy) || map.isWall(x + dx, y + dy))
                            {
                                free = false;
                                break;
                            }
                        }
                    }
                    if(!free)
                        continue;
                    if(reachable && !(*reachable)[(y + size / 2) * w + x + size / 2])
                        continue;
                    if(minApart > 0 && DetMath::hypot(static_cast<float>(x - apartX), static_cast<float>(y - apartY)) < minApart)
                        continue;
                    const long dist = static_cast<long>(x - targetX) * (x - targetX) + static_cast<long>(y - targetY) * (y - targetY);
                    if(best < 0 || dist < best)
                    {
                        best = dist;
                        outX = x;
                        outY = y;
                    }
                }
            }
            return best >= 0;
        }

        // Open ground connected to (startX, startY)
        std::vector<char> reachableFrom(const TileMap &map, int startX, int startY)
        {
            const int w = map.getWidth(), h = map.getHeight();
            std::vector<char> seen(static_cast<size_t>(w) * h, 0);
            std::queue<std::pair<int, int>> queue;
            queue.push({startX, startY});
            seen[startY * w + startX] = 1;
            while(!queue.empty())
            {
                const auto [x, y] = queue.front();
                queue.pop();
                static const int kStep[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for(const auto &s : kStep)
                {
                    const int nx = x + s[0], ny = y + s[1];
                    if(nx < 0 || ny < 0 || nx >= w || ny >= h || seen[ny * w + nx])
                        continue;
                    if(!map.isPassable(nx, ny))
                        continue;
                    seen[ny * w + nx] = 1;
                    queue.push({nx, ny});
                }
            }
            return seen;
        }

        // Sets starting bases on free ground: buildings with a one tile gap around them (the base stays
        // walkable), units on the free tiles next to them
        class BasePlanner
        {
        public:
            BasePlanner(TileMap &map, EntityManager &entities) : m_map(map), m_entities(entities), m_w(map.getWidth()), m_h(map.getHeight()), m_taken(static_cast<size_t>(m_w) * m_h, 0)
            {
            }

            bool place(int type, int cx, int cy, Faction faction, PlayerId owner, int &outX, int &outY)
            {
                const BuildingConfig *cfg = m_entities.getBuildingConfig(type);
                const int bw = cfg ? cfg->width : 3, bh = cfg ? cfg->height : 3;
                int x = 0, y = 0;
                if(!findSpot(bw + 1, bh + 1, cx - 1, cy - 1, x, y))
                    return false;
                x += 1;
                y += 1;
                if(m_entities.spawnBuildingByIndex(type, x, y, faction, 1.0f, owner) <= 0)
                    return false;
                for(int ty = y; ty < y + bh && ty < m_h; ++ty)
                {
                    for(int tx = x; tx < x + bw && tx < m_w; ++tx)
                    {
                        m_map.setPassable(tx, ty, false);
                        m_taken[ty * m_w + tx] = 1;
                    }
                }
                outX = x;
                outY = y;
                return true;
            }

            void placeUnits(const int (&archetypes)[4], int cx, int cy, Faction faction, PlayerId owner)
            {
                for(int arch : archetypes)
                {
                    int x = 0, y = 0;
                    if(!findSpot(1, 1, cx, cy, x, y))
                        continue;
                    m_taken[y * m_w + x] = 1;
                    m_entities.spawnUnitByArchetype(arch, x, y, faction, owner);
                }
            }

        private:
            bool tileFree(int x, int y) const
            {
                return x >= 0 && y >= 0 && x < m_w && y < m_h && m_map.isPassable(x, y) && !m_map.isWall(x, y) && !m_taken[y * m_w + x];
            }
            // Nearest spot (by ring distance from cx, cy) where a bw x bh rectangle is free
            bool findSpot(int bw, int bh, int cx, int cy, int &outX, int &outY) const
            {
                for(int r = 0; r < 40; ++r)
                {
                    long best = -1;
                    for(int y = cy - r; y <= cy + r; ++y)
                    {
                        for(int x = cx - r; x <= cx + r; ++x)
                        {
                            if(std::max(std::abs(x - cx), std::abs(y - cy)) != r)
                                continue;
                            bool free = true;
                            for(int dy = 0; dy < bh && free; ++dy)
                            {
                                for(int dx = 0; dx < bw; ++dx)
                                {
                                    if(!tileFree(x + dx, y + dy))
                                    {
                                        free = false;
                                        break;
                                    }
                                }
                            }
                            if(!free)
                                continue;
                            const long dist = static_cast<long>(x - cx) * (x - cx) + static_cast<long>(y - cy) * (y - cy);
                            if(best < 0 || dist < best)
                            {
                                best = dist;
                                outX = x;
                                outY = y;
                            }
                        }
                    }
                    if(best >= 0)
                        return true;
                }
                return false;
            }

            TileMap &m_map;
            EntityManager &m_entities;
            int m_w, m_h;
            std::vector<char> m_taken;
        };

        // The starting base of a faction: headquarters, power plant, barracks (building indices) and four units
        struct StartingBase
        {
            int hq, power, barracks;
            int units[4];
        };
        const StartingBase kResistanceBase {8, 9, 10, {0, 0, 2, 6}};
        const StartingBase kConfederationBase {0, 1, 2, {1, 1, 3, 9}};
        const StartingBase &startingBase(Faction f)
        {
            return f == Faction::Confederation ? kConfederationBase : kResistanceBase;
        }

        std::vector<std::pair<int, int>> startPositionsOf(const nlohmann::json &doc, int w, int h)
        {
            std::vector<std::pair<int, int>> out;
            auto add = [&](int x, int y)
            {
                x = std::clamp(x, 0, std::max(0, w - 1));
                y = std::clamp(y, 0, std::max(0, h - 1));
                for(const auto &p : out)
                {
                    if(std::abs(p.first - x) < 6 && std::abs(p.second - y) < 6)
                        return; // the same spot
                }
                if(out.size() < static_cast<size_t>(kMaxPlayers))
                    out.push_back({x, y});
            };
            if(doc.is_object() && doc.contains("start_positions") && doc["start_positions"].is_array())
            {
                for(const auto &p : doc["start_positions"])
                {
                    if(p.is_object())
                        add(p.value("x", 0), p.value("y", 0));
                }
            }
            // The map says how many players it has: its own start positions; a map that lists none is a map for two
            // (the classic spots of a custom game: playerPos / enemyPos)
            int px = 6, py = h - 14, ex = w - 16, ey = 4;
            if(doc.is_object() && doc.contains("playerPos") && doc["playerPos"].is_object())
            {
                px = doc["playerPos"].value("x", px);
                py = doc["playerPos"].value("y", py);
            }
            if(doc.is_object() && doc.contains("enemyPos") && doc["enemyPos"].is_object())
            {
                ex = doc["enemyPos"].value("x", ex);
                ey = doc["enemyPos"].value("y", ey);
            }
            if(out.empty())
                add(px, py);
            if(out.size() < 2)
                add(ex, ey);
            return out;
        }

        // The buildings and units the map itself carries (the lists the map editor writes). `ownerOf(faction, x, y)` says to whom
        // one belongs (kNoPlayer: left out); `onHq(owner, faction, x, y)` is told about every headquarters.
        void spawnPrebuilt(const nlohmann::json &doc, TileMap &map, EntityManager &entities, const std::function<PlayerId(Faction, int, int)> &ownerOf, const std::function<void(PlayerId, Faction, int, int)> &onHq)
        {
            if(!doc.is_object())
                return;
            for(const auto &b : doc.value("buildings", nlohmann::json::array()))
            {
                const Faction faction = Utility::parseFaction(b.value("faction", "resistance"));
                const int x = b.value("x", 0), y = b.value("y", 0), index = b.value("buildingIndex", 0);
                const PlayerId owner = ownerOf(faction, x, y);
                if(owner == kNoPlayer)
                    continue;
                const int id = entities.spawnBuildingByIndex(index, x, y, faction, b.value("initialProgress", 1.0f), owner);
                const Building *spawned = id > 0 ? entities.getBuilding(id) : nullptr;
                if(!spawned)
                    continue;
                for(int ty = y; ty < y + spawned->buildHeight && ty < map.getHeight(); ++ty)
                {
                    for(int tx = x; tx < x + spawned->buildWidth && tx < map.getWidth(); ++tx)
                        map.setPassable(tx, ty, false);
                }
                if(index == 0 || index == 8)
                    onHq(owner, faction, x, y);
            }
            for(const auto &u : doc.value("units", nlohmann::json::array()))
            {
                const Faction faction = Utility::parseFaction(u.value("faction", "resistance"));
                const int x = u.value("x", 0), y = u.value("y", 0);
                const PlayerId owner = ownerOf(faction, x, y);
                if(owner == kNoPlayer)
                    continue;
                entities.spawnUnitByArchetype(u.value("archetypeId", 0), x, y, faction, owner);
            }
        }

    } // namespace

    bool MissionController::loadSkirmish(const std::string &mapPath, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder, bool usePrebuilt)
    {
        std::string mapText;
        if(!Assets::readText(mapPath, mapText) || !map.loadMap(mapPath))
        {
            std::cerr << "[MissionController] Failed to load custom game map: " << mapPath << std::endl;
            return false;
        }
        const nlohmann::json doc = nlohmann::json::parse(mapText, nullptr, false);
        if(!usePrebuilt)
            map.clearWalls(); // from scratch: the map's own walls are not part of it
        m_customPrebuilt = usePrebuilt;

        ++m_loadSerial;
        resetResearch();
        m_currentMissionId = kSkirmishMissionId;
        m_customMapPath = mapPath;
        const std::string mapName = std::filesystem::path(mapPath).stem().string();
        std::string title = mapName;
        if(doc.is_object() && doc.contains("title") && doc["title"].is_string())
        {
            title = _(doc["title"].get<std::string>());
        }
        m_missionName = "Custom game: " + title;
        m_objectiveText = "- Build up your base.\n- Destroy the enemy base and all enemy forces.";
        m_briefingText = "Open skirmish on the map '" + title +
            "'. The enemy commander builds and fortifies "
            "his base. Expand, defend and crush the enemy forces.";
        m_missionTime = 0.0f;
        m_waveTimer = 0.0f;
        m_waveCount = 0;
        m_scheduledWaves.clear();
        m_defenseMission = false;
        m_hasDestObjective = false;
        m_isVictory = false;
        m_isDefeat = false;

        entities.clear();
        economy.reset(1500);
        pathfinder.setMap(&map);
        pathfinder.setEntityManager(&entities);

        // The map's own buildings and units, when the game is to start with them
        bool prebuiltHq[2] = {false, false}; // Resistance, Confederation
        int prebuiltHqX[2] = {0, 0}, prebuiltHqY[2] = {0, 0};
        if(usePrebuilt)
        {
            spawnPrebuilt(
                doc,
                map,
                entities,
                [](Faction, int, int) { return EntityManager::kAutoOwner; },
                [&](PlayerId, Faction f, int x, int y)
                {
                    const int i = f == Faction::Confederation ? 1 : 0;
                    prebuiltHq[i] = true;
                    prebuiltHqX[i] = x;
                    prebuiltHqY[i] = y;
                });
        }

        // Where the bases go: the map's own start positions if it has them, else the bottom-left / top-right corners
        const int w = map.getWidth(), h = map.getHeight();
        int playerTargetX = 6, playerTargetY = h - 14;
        int enemyTargetX = w - 16, enemyTargetY = 4;
        if(doc.is_object())
        {
            if(doc.contains("playerPos") && doc["playerPos"].is_object())
            {
                playerTargetX = doc["playerPos"].value("x", playerTargetX);
                playerTargetY = doc["playerPos"].value("y", playerTargetY);
            }
            if(doc.contains("enemyPos") && doc["enemyPos"].is_object())
            {
                enemyTargetX = doc["enemyPos"].value("x", enemyTargetX);
                enemyTargetY = doc["enemyPos"].value("y", enemyTargetY);
            }
        }
        // Original maps are dense: take the largest open square that exists (10, 8, then 6 tiles) and set the
        // buildings and units on the free ground around its center
        constexpr int kSizes[3] = {10, 8, 6};
        int px = 0, py = 0, psize = 0;
        for(int size : kSizes)
        {
            if(findBaseSite(map, size, playerTargetX, playerTargetY, nullptr, px, py))
            {
                psize = size;
                break;
            }
        }
        if(psize == 0)
        {
            std::cerr << "[MissionController] No room for a base on " << mapPath << std::endl;
            return false;
        }
        if(prebuiltHq[0] && prebuiltHq[1])
        {
            // The map brings both bases: nothing is generated
            m_playerHqTileX = prebuiltHqX[0];
            m_playerHqTileY = prebuiltHqY[0];
            m_enemyHqTileX = prebuiltHqX[1];
            m_enemyHqTileY = prebuiltHqY[1];
            if(playerFaction() == Faction::Confederation)
            {
                std::swap(m_playerHqTileX, m_enemyHqTileX);
                std::swap(m_playerHqTileY, m_enemyHqTileY);
            }
            map.setViewerTeam(Players::teamOf(Players::local()));
            entities.updateFogOfWar(map);
            std::cout << "[MissionController] Custom game on " << mapPath << " with the bases of the map" << std::endl;
            if(m_onLoaded)
                m_onLoaded();
            return true;
        }
        const std::vector<char> connected = reachableFrom(map, px + psize / 2, py + psize / 2);

        // The enemy base is as far from the player as the map allows (at most half of its short side)
        int ex = 0, ey = 0, esize = 0;
        for(int size : kSizes)
        {
            for(int apart = std::min(w, h) / 2; apart >= psize && esize == 0; apart -= 2)
            {
                if(findBaseSite(map, size, enemyTargetX, enemyTargetY, &connected, ex, ey, px, py, apart))
                    esize = size;
            }
            if(esize != 0)
                break;
        }
        if(esize == 0)
        {
            std::cerr << "[MissionController] No room for the enemy base on " << mapPath << std::endl;
            return false;
        }

        BasePlanner planner(map, entities);
        const PlayerId autoOwner = EntityManager::kAutoOwner;
        // Resistance: headquarters, power generator, barracks; Confederation: the same three of its own
        const int pcx = px + psize / 2, pcy = py + psize / 2;
        const int ecx = ex + esize / 2, ecy = ey + esize / 2;
        int hqX = 0, hqY = 0, ehqX = 0, ehqY = 0, unusedX = 0, unusedY = 0;
        // A side whose base the map brought keeps it, the other gets the usual starting base
        if(prebuiltHq[0])
        {
            hqX = prebuiltHqX[0];
            hqY = prebuiltHqY[0];
        }
        else if(!planner.place(8, pcx, pcy, Faction::Resistance, autoOwner, hqX, hqY))
        {
            std::cerr << "[MissionController] Could not set the headquarters on " << mapPath << std::endl;
            return false;
        }
        if(prebuiltHq[1])
        {
            ehqX = prebuiltHqX[1];
            ehqY = prebuiltHqY[1];
        }
        else if(!planner.place(0, ecx, ecy, Faction::Confederation, autoOwner, ehqX, ehqY))
        {
            std::cerr << "[MissionController] Could not set the headquarters on " << mapPath << std::endl;
            return false;
        }
        if(!prebuiltHq[0])
        {
            planner.place(9, pcx, pcy, Faction::Resistance, autoOwner, unusedX, unusedY);
            planner.place(10, pcx, pcy, Faction::Resistance, autoOwner, unusedX, unusedY);
            planner.placeUnits(kResistanceBase.units, pcx, pcy, Faction::Resistance, autoOwner);
        }
        if(!prebuiltHq[1])
        {
            planner.place(1, ecx, ecy, Faction::Confederation, autoOwner, unusedX, unusedY);
            planner.place(2, ecx, ecy, Faction::Confederation, autoOwner, unusedX, unusedY);
            planner.placeUnits(kConfederationBase.units, ecx, ecy, Faction::Confederation, autoOwner);
        }

        m_playerHqTileX = hqX;
        m_playerHqTileY = hqY;
        m_enemyHqTileX = ehqX;
        m_enemyHqTileY = ehqY;
        if(playerFaction() == Faction::Confederation)
        {
            std::swap(m_playerHqTileX, m_enemyHqTileX);
            std::swap(m_playerHqTileY, m_enemyHqTileY);
        }

        map.setViewerTeam(Players::teamOf(Players::local())); // the drawn fog is the local player's from the first frame on
        entities.updateFogOfWar(map);
        std::cout << "[MissionController] Custom game on " << mapPath << " | Player base: (" << px << "," << py << ") | Enemy base: (" << ex << "," << ey << ")" << std::endl;
        if(m_onLoaded)
        {
            m_onLoaded();
        }
        return true;
    }

    std::vector<std::pair<int, int>> MissionController::startPositions(const std::string &mapPath)
    {
        std::string text;
        if(!Assets::readText(mapPath, text))
            return {};
        const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
        if(!doc.is_object())
            return {};
        return startPositionsOf(doc, doc.value("width", 64), doc.value("height", 64));
    }

    bool MissionController::loadMatch(const MatchSetup &setup, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder)
    {
        std::string mapText;
        if(!Assets::readText(setup.mapPath, mapText) || !map.loadMap(setup.mapPath))
        {
            std::cerr << "[MissionController] Failed to load match map: " << setup.mapPath << std::endl;
            return false;
        }
        const nlohmann::json doc = nlohmann::json::parse(mapText, nullptr, false);
        if(!setup.prebuilt)
            map.clearWalls();
        Players::setup(setup.slots, setup.local);

        ++m_loadSerial;
        resetResearch();
        m_currentMissionId = kMatchMissionId;
        m_customMapPath.clear();
        m_matchSetup = setup;
        const std::string mapName = std::filesystem::path(setup.mapPath).stem().string();
        std::string title = mapName;
        if(doc.is_object() && doc.contains("title") && doc["title"].is_string())
            title = _(doc["title"].get<std::string>());
        m_missionName = "Match: " + title;
        m_objectiveText = "- Destroy every base and force of the enemy teams.";
        m_briefingText = "Multiplayer match on the map '" + title + "'.";
        m_missionTime = 0.0f;
        m_waveTimer = 0.0f;
        m_waveCount = 0;
        m_scheduledWaves.clear();
        m_defenseMission = false;
        m_hasDestObjective = false;
        m_isVictory = false;
        m_isDefeat = false;

        entities.clear();
        economy.reset(setup.startingCredits);
        economy.setPlayer(setup.local);
        map.setFogEnabled(setup.fogOfWar);
        pathfinder.setMap(&map);
        pathfinder.setEntityManager(&entities);

        const std::vector<std::pair<int, int>> spots = startPositionsOf(doc, map.getWidth(), map.getHeight());
        // The buildings and units of the map go to the player of their faction whose start position is the nearest
        std::array<std::pair<int, int>, kMaxPlayers> prebuiltHq {};
        std::array<bool, kMaxPlayers> hasPrebuiltHq {};
        if(setup.prebuilt)
        {
            spawnPrebuilt(
                doc,
                map,
                entities,
                [&](Faction f, int x, int y)
                {
                    PlayerId best = kNoPlayer;
                    long bestDist = 0;
                    for(PlayerId p = 0; p < kMaxPlayers; ++p)
                    {
                        const PlayerSlot &s = setup.slots[p];
                        const int spot = setup.startPosition[p];
                        if(!s.active || s.faction != f || spot < 0 || spot >= static_cast<int>(spots.size()))
                            continue;
                        const long dx = spots[spot].first - x, dy = spots[spot].second - y;
                        const long d = dx * dx + dy * dy;
                        if(best == kNoPlayer || d < bestDist)
                        {
                            best = p;
                            bestDist = d;
                        }
                    }
                    return best;
                },
                [&](PlayerId owner, Faction, int x, int y)
                {
                    if(owner >= kMaxPlayers)
                        return;
                    hasPrebuiltHq[owner] = true;
                    prebuiltHq[owner] = {x, y};
                });
        }
        BasePlanner planner(map, entities);
        std::vector<char> connected;
        std::vector<std::pair<int, int>> placed; // centers of the bases set so far
        constexpr int kSizes[3] = {10, 8, 6};
        for(PlayerId p = 0; p < kMaxPlayers; ++p)
        {
            const PlayerSlot &slot = setup.slots[p];
            if(!slot.active)
                continue;
            const int spot = setup.startPosition[p];
            if(spot < 0 || spot >= static_cast<int>(spots.size()))
            {
                std::cerr << "[MissionController] Player " << int(p) << " has no start position" << std::endl;
                return false;
            }
            if(hasPrebuiltHq[p])
            { // the map brought this player's base
                placed.push_back(prebuiltHq[p]);
                if(p == setup.local)
                {
                    m_playerHqTileX = prebuiltHq[p].first;
                    m_playerHqTileY = prebuiltHq[p].second;
                }
                else if(Players::hostile(p, setup.local))
                {
                    m_enemyHqTileX = prebuiltHq[p].first;
                    m_enemyHqTileY = prebuiltHq[p].second;
                }
                continue;
            }
            // Open ground near the spot, reachable from the first base, away from the bases already set
            int bx = 0, by = 0, size = 0;
            for(int s : kSizes)
            {
                for(int apart = std::min(map.getWidth(), map.getHeight()) / 3; apart >= 0 && size == 0; apart -= 2)
                {
                    bool ok = findBaseSite(map, s, spots[spot].first, spots[spot].second, connected.empty() ? nullptr : &connected, bx, by);
                    for(const auto &c : placed)
                    {
                        if(DetMath::hypot(static_cast<float>(bx + s / 2 - c.first), static_cast<float>(by + s / 2 - c.second)) < apart)
                            ok = false;
                    }
                    if(ok)
                        size = s;
                }
                if(size != 0)
                    break;
            }
            if(size == 0)
            {
                std::cerr << "[MissionController] No room for the base of player " << int(p) << std::endl;
                return false;
            }
            const int cx = bx + size / 2, cy = by + size / 2;
            if(connected.empty())
                connected = reachableFrom(map, cx, cy);
            placed.push_back({cx, cy});
            const StartingBase &base = startingBase(slot.faction);
            int hqX = 0, hqY = 0, unusedX = 0, unusedY = 0;
            if(!planner.place(base.hq, cx, cy, slot.faction, p, hqX, hqY))
                return false;
            planner.place(base.power, cx, cy, slot.faction, p, unusedX, unusedY);
            planner.place(base.barracks, cx, cy, slot.faction, p, unusedX, unusedY);
            planner.placeUnits(base.units, cx, cy, slot.faction, p);
            if(p == setup.local)
            {
                m_playerHqTileX = hqX;
                m_playerHqTileY = hqY;
            }
            else if(Players::hostile(p, setup.local))
            {
                m_enemyHqTileX = hqX;
                m_enemyHqTileY = hqY;
            }
        }

        map.setViewerTeam(Players::teamOf(setup.local));
        entities.updateFogOfWar(map);
        std::cout << "[MissionController] Match on " << setup.mapPath << " for " << Players::count() << " players" << std::endl;
        if(m_onLoaded)
            m_onLoaded();
        return true;
    }

    bool MissionController::restart(const std::string &assetRoot, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder)
    {
        if(!m_customMapPath.empty())
        {
            return loadSkirmish(m_customMapPath, map, entities, economy, pathfinder, m_customPrebuilt);
        }
        return loadMission(m_currentMissionId, assetRoot, map, entities, economy, pathfinder);
    }

    bool MissionController::loadTestMap(const std::string &assetRoot, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder)
    {
        m_currentMissionId = -1; // Standalone test map, not a numbered campaign mission!
        m_customMapPath.clear();
        m_missionTime = 0.0f;
        m_waveTimer = 0.0f;
        m_waveCount = 0;
        m_scheduledWaves.clear();
        m_defenseMission = false;
        m_hasDestObjective = false;
        m_isVictory = false;
        m_isDefeat = false;

        std::string testMapPath = "maps/testmap.json";
        std::string mapText;
        bool haveTestMap = Assets::readText(testMapPath, mapText);
        if(!haveTestMap && !assetRoot.empty())
        {
            testMapPath = assetRoot + "/maps/testmap.json";
            haveTestMap = Assets::readText(testMapPath, mapText);
        }
        if(!haveTestMap)
        {
            std::cerr << "[MissionController] Failed to open test map: " << testMapPath << std::endl;
            return false;
        }

        nlohmann::json mapData = nlohmann::json::parse(mapText);
        if(!map.loadMap(testMapPath))
        {
            return false;
        }

        m_missionName = mapData.value("missionName", "Test Map");
        m_briefingText = mapData.value("briefing", "");
        m_objectiveText = mapData.value("objectives", "");
        const auto headquarters = mapData.value("headquarters", nlohmann::json::object());
        const auto playerHq = headquarters.value("player", nlohmann::json::object());
        const auto enemyHq = headquarters.value("enemy", nlohmann::json::object());
        m_playerHqTileX = playerHq.value("x", 5);
        m_playerHqTileY = playerHq.value("y", 18);
        m_enemyHqTileX = enemyHq.value("x", 46);
        m_enemyHqTileY = enemyHq.value("y", 18);

        entities.clear();
        economy.reset(mapData.value("startingCredits", 10000));
        pathfinder.setMap(&map);
        pathfinder.setEntityManager(&entities);

        auto parseFaction = [](const nlohmann::json &item) { return Utility::parseFaction(item.value("faction", "resistance")); };
        auto placeBuilding = [&](const nlohmann::json &building)
        {
            const int buildingIndex = building.value("buildingIndex", 0);
            const int bx = building.value("x", 0);
            const int by = building.value("y", 0);
            const Faction faction = parseFaction(building);
            int bid = entities.spawnBuildingByIndex(buildingIndex, bx, by, faction, building.value("initialProgress", 1.0f));
            if(bid > 0)
            {
                const Building *bEnt = entities.getBuilding(bid);
                if(bEnt)
                {
                    for(int ty = by; ty < by + bEnt->buildHeight && ty < map.getHeight(); ++ty)
                    {
                        for(int tx = bx; tx < bx + bEnt->buildWidth && tx < map.getWidth(); ++tx)
                        {
                            map.setPassable(tx, ty, false);
                        }
                    }
                }
            }
        };

        for(const auto &building : mapData.value("buildings", nlohmann::json::array()))
        {
            placeBuilding(building);
        }
        for(const auto &unitData : mapData.value("units", nlohmann::json::array()))
        {
            const int unitId = entities.spawnUnitByArchetype(unitData.value("archetypeId", 0), unitData.value("x", 0), unitData.value("y", 0), parseFaction(unitData));
            Entity *unit = entities.getEntity(unitId);
            if(!unit)
                continue;
            if(unitData.contains("maxHp"))
            {
                unit->maxHp = unitData["maxHp"].get<int>();
                unit->currentHp = unit->maxHp;
            }
            if(unitData.contains("attackDamage"))
            {
                unit->attackDamage = unitData["attackDamage"].get<int>();
            }
        }

        std::cout << "[MissionController] Loaded test map from " << testMapPath << " (" << map.getWidth() << "x" << map.getHeight() << ")." << std::endl;
        if(m_onLoaded)
        {
            m_onLoaded();
        }
        return true;
    }

} // namespace OpenArtOfWar
