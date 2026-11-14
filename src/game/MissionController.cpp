#include "game/MissionController.hpp"
#include <array>
#include <functional>
#include "game/Players.hpp"
#include "core/DetMath.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/Utility.hpp"
#include "game/Missions.hpp"
#include "game/Capabilities.hpp"
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
#include <string>
#include <tuple>
#include <unordered_map>

namespace OpenArtOfWar
{

const std::vector<MissionInfo> &MissionController::GetAvailableMissions()
{
    static const auto extractInfos = [](const std::vector<MissionDef> &missionDefs) {
        std::vector<MissionInfo> infos;
        for (const auto &missionDef : missionDefs)
            infos.push_back(missionDef.info);
        return infos;
    };
    static const std::vector<MissionInfo> s_resistance = extractInfos(Missions::resistance());
    static const std::vector<MissionInfo> s_confederation = extractInfos(Missions::confederation());
    return PlayerFaction() == Faction::Confederation ? s_confederation : s_resistance;
}

MissionController::MissionController() {}

void MissionController::SpawnSquadFromCmd27(
    int group, int x, int y, int commandValue, TileMap &map, EntityManager &entities, Pathfinder *pathfinder)
{
    // Groups 0..2 are the computer's squads, 3 and up the player's - in the Peru campaign as well as in the
    // Confederation one (there the player commands the Confederation)
    Faction faction = (group < 3) ? AiFaction() : PlayerFaction();
    std::vector<int> spawnedIds;

    auto spawnArchetype = [&](int archetypeId, int slotIndex) {
        int offsetX = (slotIndex % 4) - 1;
        int offsetY = (slotIndex / 4) - 1;
        int tileX = std::clamp(x + offsetX, 1, std::max(1, map.GetWidth() - 2));
        int tileY = std::clamp(y + offsetY, 1, std::max(1, map.GetHeight() - 2));
        int unitId = entities.SpawnUnitByArchetype(archetypeId, tileX, tileY, faction);
        if (unitId > 0)
        {
            spawnedIds.push_back(unitId);
        }
    };

    // Defense missions send the enemy's squads trimmed to the difficulty (an Easy squad of 10 brings 4)
    const int squadPercent = (m_defenseMission && faction == AiFaction()) ? m_defenseSquadPercent : 100;
    auto squadSize = [&](int baseCount) {
        return std::max(1, (baseCount * squadPercent + 50) / 100);
    };

    int spawnIndex = 0;
    if (commandValue >= 72)
    {
        int unitCount = squadSize((commandValue == 72) ? 1 : (commandValue == 73 ? 3 : 6));
        int archetypeId = (faction == Faction::Confederation) ? 18 : 8; // Fortress (18) or Armadillo (8)
        for (int k = 0; k < unitCount; ++k)
        {
            spawnArchetype(archetypeId, spawnIndex++);
        }
    }
    else
    {
        int sizeTier = commandValue % 3;
        int unitVariant = (commandValue % 30) / 3;
        int branchMode = commandValue / 30;
        int branchStart = branchMode / 2;
        int branchEnd = 1 - (branchMode & 1);

        for (int branch = branchStart; branch <= branchEnd; ++branch)
        {
            if (branch == 0)
            {
                // Infantry squad
                int unitCount = squadSize((sizeTier == 0) ? 3 : (sizeTier == 1 ? 6 : 10));
                for (int k = 0; k < unitCount; ++k)
                {
                    int archetypeId = 0;
                    if (unitVariant == 0)
                    {
                        archetypeId = (faction == Faction::Confederation) ? 1 : 0; // Rifleman
                    }
                    else if (unitVariant == 1)
                    {
                        archetypeId = (faction == Faction::Confederation) ? 3 : 2; // HeavyAssault / Grenadier
                    }
                    else if (unitVariant == 2)
                    {
                        archetypeId = (faction == Faction::Confederation) ? 5 : 4; // Flamethrower / Sniper
                    }
                    else
                    {
                        int variantIndex = k % 3;
                        archetypeId = (faction == Faction::Confederation) ? (variantIndex * 2 + 1) : (variantIndex * 2);
                    }
                    spawnArchetype(archetypeId, spawnIndex++);
                }
            }
            else
            {
                // Vehicle squad
                int unitCount = squadSize((sizeTier == 0) ? 2 : (sizeTier == 1 ? 4 : 6));
                for (int k = 0; k < unitCount; ++k)
                {
                    int archetypeId = 6;
                    if (unitVariant == 0)
                    {
                        archetypeId = (faction == Faction::Confederation) ? 9 : 6; // Hammer (9) / Coyote (6)
                    }
                    else if (unitVariant == 1)
                    {
                        if (faction == Faction::Confederation)
                        {
                            archetypeId = (k % 2 == 0) ? 18 : 17; // Fortress (18) / Zeus (17)
                        }
                        else
                        {
                            archetypeId = (k % 2 == 0) ? 8 : 10; // Armadillo (8) / Rhino (10)
                        }
                    }
                    else if (unitVariant == 2)
                    {
                        if (faction == Faction::Confederation)
                        {
                            archetypeId = (k % 2 == 0) ? 18 : 13; // Fortress (18) / Grad (13)
                        }
                        else
                        {
                            archetypeId = (k % 2 == 0) ? 10 : 12; // Rhino (10) / Porcupine (12)
                        }
                    }
                    else
                    {
                        if (faction == Faction::Confederation)
                        {
                            const int confederationVehicles[] = {9, 18, 17, 13};
                            archetypeId = confederationVehicles[k % 4];
                        }
                        else
                        {
                            const int resistanceVehicles[] = {6, 8, 10, 12};
                            archetypeId = resistanceVehicles[k % 4];
                        }
                    }
                    spawnArchetype(archetypeId, spawnIndex++);
                }
            }
        }
    }

    // Reinforcements of the computer's side march on the player's base right away
    if (pathfinder && faction == AiFaction() && !spawnedIds.empty())
    {
        entities.IssueAttackMoveOrder(spawnedIds, m_playerHqTileX, m_playerHqTileY, *pathfinder);
    }
}

void MissionController::SetDefensePacing(
    float firstWaveSeconds, float waveGapSeconds, int squadPercent, float timeScale)
{
    m_defenseFirstWave = std::max(0.0f, firstWaveSeconds);
    m_defenseWaveGap = std::max(1.0f, waveGapSeconds);
    m_defenseSquadPercent = std::clamp(squadPercent, 10, 100);
    m_defenseTimeScale = std::clamp(timeScale, 0.25f, 4.0f);
    if (!m_defenseMission)
        return;
    for (auto &wave : m_scheduledWaves)
    {
        if (wave.spawned || wave.group >= 3)
            continue;
        if (wave.defenseOrder >= 0)
        {
            wave.triggerTime = m_defenseFirstWave + m_defenseWaveGap * static_cast<float>(wave.defenseOrder);
        }
        else
        {
            wave.triggerTime = std::max(m_defenseFirstWave, wave.scriptTime * m_defenseTimeScale);
        }
    }
}

namespace
{

// The tech level a map or mission sets for its players: "capabilities": "<level>" in its JSON (none: the default)
std::string TechPreset(const nlohmann::json &mapJson)
{
    if (mapJson.is_object() && mapJson.contains("capabilities") && mapJson["capabilities"].is_string())
        return mapJson["capabilities"].get<std::string>();
    return {};
}

} // namespace

bool MissionController::LoadMission(
    int missionId, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder)
{
    // Single player: two slots in the classic colours of the factions, the local player in slot 0 (a network or
    // custom match before left its own slots, colours and local slot behind)
    SetPlayerFaction(PlayerFaction());
    ++m_loadSerial;
    ResetResearch();
    m_customMapPath.clear();
    const auto &missionList = GetAvailableMissions();
    if (missionId < 0 || missionId >= static_cast<int>(missionList.size()))
    {
        missionId = 0;
    }
    m_currentMissionId = missionId;
    const auto &missionInfo = missionList[missionId];
    m_missionName = missionInfo.name;
    m_mapTitle = _(missionInfo.name.c_str());
    m_briefingText = missionInfo.briefing;
    m_objectiveText = missionInfo.objective;
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

    entities.Clear();
    economy.Reset(1500);

    // 1. Mission definition of the campaign the player's side belongs to (Missions*.cpp)
    const MissionData &missionData = Missions::Campaign(PlayerFaction())[missionId].data;
    const int mapIndex = missionData.mapIndex;
    bool foundPlayerBase = false;
    bool foundPlayerHq = false;
    bool foundEnemyBase = false;
    bool foundEnemyHq = false;
    bool hadMissionScript = false;

    // Walls take the side of the nearer base
    auto setWallZone = [&](const Rect &zoneRect) {
        float distToPlayerHq = DetMath::Hypot(
            static_cast<float>(zoneRect.x - m_playerHqTileX), static_cast<float>(zoneRect.y - m_playerHqTileY));
        float distToEnemyHq = DetMath::Hypot(
            static_cast<float>(zoneRect.x - m_enemyHqTileX), static_cast<float>(zoneRect.y - m_enemyHqTileY));
        Faction wallFaction = (distToEnemyHq <= distToPlayerHq) ? Faction::Confederation : Faction::Resistance;
        for (int tileY = zoneRect.y; tileY < zoneRect.y + zoneRect.h; ++tileY)
        {
            for (int tileX = zoneRect.x; tileX < zoneRect.x + zoneRect.w; ++tileX)
            {
                map.SetWall(tileX, tileY, wallFaction, 100);
            }
        }
    };

    // 2. Load corresponding map
    std::string mapPath = LoaderResources::MissionMapPath(missionData.mapFile);
    if (!map.LoadMap(mapPath))
    {
        std::cerr << "[MissionController] Failed to load map: " << mapPath << std::endl;
        return false;
    }
    pathfinder.SetMap(&map);
    {
        // The tech level of the mission (data/capabilities.yaml): its map may name one
        std::string mapText;
        Assets::ReadText(mapPath, mapText);
        Capabilities::ApplyLevels(-1, TechPreset(nlohmann::json::parse(mapText, nullptr, false)), entities);
    }

    // 3. Spawn preplaced authentic structures and walls from the mission
    for (const auto &item : missionData.entities)
    {
        const int entityType = item.type, entityX = item.x, entityY = item.y;
        if (entityType < 15)
        {
            // Authentic faction mapping: 0..7 = Confederation (Blue, Enemy), 8..14 = Resistance (Red, Player)
            Faction faction = (entityType >= 8) ? Faction::Resistance : Faction::Confederation;
            int buildingId = entities.SpawnBuildingByIndex(entityType, entityX, entityY, faction, 1.0f);
            if (buildingId > 0)
            {
                const Building *building = entities.GetBuilding(buildingId);
                if (building)
                {
                    for (int tileY = entityY; tileY < entityY + building->buildHeight && tileY < map.GetHeight();
                         ++tileY)
                    {
                        for (int tileX = entityX; tileX < entityX + building->buildWidth && tileX < map.GetWidth();
                             ++tileX)
                        {
                            map.SetPassable(tileX, tileY, false);
                        }
                    }
                }
                if (faction == Faction::Resistance)
                {
                    if (!foundPlayerBase || (entityType == 8 && !foundPlayerHq))
                    {
                        m_playerHqTileX = entityX;
                        m_playerHqTileY = entityY;
                        foundPlayerBase = true;
                        if (entityType == 8)
                            foundPlayerHq = true;
                    }
                }
                else
                {
                    if (!foundEnemyBase || (entityType == 0 && !foundEnemyHq))
                    {
                        m_enemyHqTileX = entityX;
                        m_enemyHqTileY = entityY;
                        foundEnemyBase = true;
                        if (entityType == 0)
                            foundEnemyHq = true;
                    }
                }
            }
        }
        else
        {
            setWallZone({entityX, entityY, 1, 1});
        }
    }

    // 3b. Pre-configured mission wall zones with base proximity faction assignment
    for (const auto &wallGroup : missionData.wallZones)
    {
        for (const auto &zoneRect : wallGroup)
            setWallZone(zoneRect);
    }

    // 4. Process authentic mission scripts (credits, wall zones, initial squads, scheduled waves, objectives)
    int defenseWaves = 0;
    m_defenseMission = missionData.defense;
    // Each delay counts from the previous command, so a squad's time is the sum of all prior delays.
    float scriptClock = 0.0f;
    if (!missionData.scripts.empty())
    {
        hadMissionScript = true;
        for (const auto &scriptCmd : missionData.scripts)
        {
            const int command = scriptCmd.cmd, group = scriptCmd.group, delay = scriptCmd.delay;
            const int scriptX = scriptCmd.x, scriptY = scriptCmd.y, commandVal = scriptCmd.val;
            scriptClock += static_cast<float>(std::max(0, delay));

            if (command == 25)
            {
                // Starting credits
                int startingCredits = std::max(1000, scriptY * 10);
                economy.Reset(startingCredits);
            }
            else if (command == 33 && commandVal == 1)
            {
                // Activate wall zone
                if (group >= 0 && group < static_cast<int>(missionData.wallZones.size()))
                {
                    for (const auto &zoneRect : missionData.wallZones[group])
                        setWallZone(zoneRect);
                }
            }
            else if (command == 40 && group >= 3)
            {
                // Destination zone objective for player units
                m_hasDestObjective = true;
                m_destX = scriptX;
                m_destY = scriptY;
                m_destSize = std::max(3, commandVal);
            }
            else if (command == 27)
            {
                // Unit squad spawn
                // Defense missions: the enemy's starting squads arrive as waves from their spawn points
                const bool atStart = scriptClock <= 0.0f;
                if (atStart)
                {
                    if (missionData.defense && group < 3)
                    {
                        // Timed by setDefensePacing (difficulty); these defaults hold until it is called
                        const int order = defenseWaves++;
                        m_scheduledWaves.push_back(
                            {m_defenseFirstWave + m_defenseWaveGap * order,
                             group,
                             scriptX,
                             scriptY,
                             commandVal,
                             false,
                             order,
                             0.0f});
                    }
                    else
                    {
                        SpawnSquadFromCmd27(group, scriptX, scriptY, commandVal, map, entities, nullptr);
                    }
                }
                else
                {
                    const float triggerAt = (missionData.defense && group < 3)
                                                ? std::max(m_defenseFirstWave, scriptClock * m_defenseTimeScale)
                                                : scriptClock;
                    m_scheduledWaves.push_back(
                        {triggerAt, group, scriptX, scriptY, commandVal, false, -1, scriptClock});
                }
                if (delay == 0)
                {
                    // Bases are collected with the Resistance as "player" and swapped at the end when the
                    // player commands the Confederation, so the player's squads go to the other slot then
                    const bool playerSlot = (group >= 3) == (PlayerFaction() == Faction::Resistance);
                    if (playerSlot && !foundPlayerBase)
                    {
                        m_playerHqTileX = scriptX;
                        m_playerHqTileY = scriptY;
                        foundPlayerBase = true;
                    }
                    else if (!playerSlot && !foundEnemyBase)
                    {
                        m_enemyHqTileX = scriptX;
                        m_enemyHqTileY = scriptY;
                        foundEnemyBase = true;
                    }
                }
            }
        }
    }

    // Fallback only if mission had neither preplaced entities nor scripts
    if (!foundPlayerBase && !hadMissionScript)
    {
        m_playerHqTileX = 8;
        m_playerHqTileY = 8;
        entities.SpawnBuildingByIndex(8, 8, 8, Faction::Resistance, 1.0f);   // Resistance headquarters (3x4)
        entities.SpawnBuildingByIndex(9, 13, 8, Faction::Resistance, 1.0f);  // Power plant (3x3)
        entities.SpawnBuildingByIndex(10, 8, 13, Faction::Resistance, 1.0f); // Barracks (3x3)
    }

    if (!foundEnemyBase && !hadMissionScript)
    {
        m_enemyHqTileX = map.GetWidth() - 10;
        m_enemyHqTileY = map.GetHeight() - 10;
        entities.SpawnBuildingByIndex(
            0, m_enemyHqTileX, m_enemyHqTileY, Faction::Confederation, 1.0f); // Confederation headquarters (4x4)
        entities.SpawnBuildingByIndex(
            6, m_enemyHqTileX - 3, m_enemyHqTileY, Faction::Confederation, 1.0f); // Radar locator (2x2)
    }

    // Add initial guard squads around bases if no script units were spawned for that side
    int playerUnitCount = 0;
    int enemyUnitCount = 0;
    for (const auto &entity : entities.GetAllEntities())
    {
        if (!entity.alive || entity.isBuilding)
            continue;
        if (entity.faction == Faction::Resistance)
            playerUnitCount++;
        else
            enemyUnitCount++;
    }

    if (playerUnitCount == 0)
    {
        int guardX = std::clamp(m_playerHqTileX + 4, 2, map.GetWidth() - 3);
        int guardY = std::clamp(m_playerHqTileY + 2, 2, map.GetHeight() - 3);
        entities.SpawnUnitByArchetype(0, guardX, guardY, Faction::Resistance);         // Rifleman
        entities.SpawnUnitByArchetype(0, guardX, guardY + 1, Faction::Resistance);     // Rifleman
        entities.SpawnUnitByArchetype(2, guardX - 1, guardY + 1, Faction::Resistance); // Grenadier
        entities.SpawnUnitByArchetype(6, guardX + 1, guardY + 1, Faction::Resistance); // Coyote
    }

    if (enemyUnitCount == 0 && foundEnemyBase && !missionData.defense)
    {
        int enemyGuardX = std::clamp(m_enemyHqTileX - 2, 2, map.GetWidth() - 3);
        int enemyGuardY = std::clamp(m_enemyHqTileY + 1, 2, map.GetHeight() - 3);
        entities.SpawnUnitByArchetype(1, enemyGuardX, enemyGuardY, Faction::Confederation);          // Rifleman
        entities.SpawnUnitByArchetype(3, enemyGuardX, enemyGuardY + 1, Faction::Confederation);      // Assault trooper
        entities.SpawnUnitByArchetype(18, enemyGuardX - 1, enemyGuardY + 1, Faction::Confederation); // Fortress
    }

    // The map describes the Resistance base as "player" and the Confederation base as "enemy";
    // playing the Confederation simply swaps the roles
    if (PlayerFaction() == Faction::Confederation)
    {
        std::swap(m_playerHqTileX, m_enemyHqTileX);
        std::swap(m_playerHqTileY, m_enemyHqTileY);
    }

    // Initialize fog of war for player base and starting units
    map.SetViewerTeam(Players::TeamOf(Players::Local())); // the drawn fog is the local player's from the first frame on
    entities.UpdateFogOfWar(map);

    std::cout << "[MissionController] Initialized " << m_missionName << " on Map m" << mapIndex << " | Player Pos: ("
              << m_playerHqTileX << "," << m_playerHqTileY << ")"
              << " | Enemy Pos: (" << m_enemyHqTileX << "," << m_enemyHqTileY << ")"
              << " | Scheduled Waves: " << m_scheduledWaves.size() << std::endl;

    if (m_onLoaded)
    {
        m_onLoaded();
    }
    return true;
}

void MissionController::Update(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
{
    if (m_isVictory || m_isDefeat)
        return;

    if (!m_calm)
        m_missionTime += deltaTime;

    // 1. Trigger scheduled script reinforcement waves (cmd 27 with delay > 0)
    int pendingEnemyWaves = 0;
    for (auto &wave : m_scheduledWaves)
    {
        if (!wave.spawned)
        {
            if (m_missionTime >= wave.triggerTime)
            {
                wave.spawned = true;
                SpawnSquadFromCmd27(wave.group, wave.x, wave.y, wave.val, map, entities, &pathfinder);
            }
            else if (wave.group < 3)
            {
                pendingEnemyWaves++;
            }
        }
    }

    // 2. Check Win/Loss conditions
    if (IsMatch())
    {
        // A team is out once it has nothing left; the local player wins when only its team remains
        std::array<bool, kMaxPlayers> teamAlive {};
        for (const auto &entity : entities.GetAllEntities())
        {
            if (!entity.alive || entity.state == ActionState::Die || entity.owner >= kMaxPlayers)
                continue;
            teamAlive[Players::TeamOf(entity.owner) % kMaxPlayers] = true;
        }
        bool hasHuman = false;
        for (int playerIndex = 0; playerIndex < kMaxPlayers; ++playerIndex)
            hasHuman = hasHuman || (m_matchSetup.slots[playerIndex].active && m_matchSetup.slots[playerIndex].human);
        if (!hasHuman)
        {
            int aliveTeams = 0;
            for (int teamIndex = 0; teamIndex < kMaxPlayers; ++teamIndex)
                aliveTeams += teamAlive[teamIndex] ? 1 : 0;
            if (aliveTeams <= 1)
                m_isVictory = true;
            return;
        }
        const std::uint8_t localTeam = Players::TeamOf(Players::Local()) % kMaxPlayers;
        int otherAliveTeams = 0;
        for (int teamIndex = 0; teamIndex < kMaxPlayers; ++teamIndex)
            otherAliveTeams += (teamIndex != localTeam && teamAlive[teamIndex]) ? 1 : 0;
        if (!teamAlive[localTeam])
            m_isDefeat = true;
        else if (otherAliveTeams == 0)
            m_isVictory = true;
        return;
    }
    int playerBuildingCount = 0;
    int playerUnitCount = 0;
    int enemyBuildingCount = 0;
    int enemyUnitCount = 0;
    bool reachedDestZone = false;

    for (const auto &entity : entities.GetAllEntities())
    {
        if (!entity.alive || entity.state == ActionState::Die)
            continue;
        if (entity.faction == PlayerFaction())
        {
            if (entity.isBuilding)
                playerBuildingCount++;
            else
            {
                playerUnitCount++;
                if (m_hasDestObjective && std::abs(entity.tileX - m_destX) <= m_destSize
                    && std::abs(entity.tileY - m_destY) <= m_destSize)
                {
                    reachedDestZone = true;
                }
            }
        }
        else if (entity.faction == AiFaction())
        {
            if (entity.isBuilding)
                enemyBuildingCount++;
            else
                enemyUnitCount++;
        }
    }

    if (playerBuildingCount == 0 && playerUnitCount == 0)
    {
        m_isDefeat = true;
        std::cout << "[MissionController] DEFEAT! All player forces destroyed." << std::endl;
        return;
    }

    if ((enemyBuildingCount == 0 && enemyUnitCount == 0 && pendingEnemyWaves == 0)
        || (m_hasDestObjective && reachedDestZone && enemyBuildingCount == 0))
    {
        m_isVictory = true;
        std::cout << "[MissionController] VICTORY! Mission objectives accomplished." << std::endl;
        return;
    }

    // 3. Periodic AI counter-attack wave if enemy has an active base (every 40 seconds)
    // Only used as fallback if AIController is not active (m_useBuiltinWaves == true)
    m_waveTimer += deltaTime;
    if (m_useBuiltinWaves && m_waveTimer >= 40.0f && enemyBuildingCount > 0)
    {
        m_waveTimer = 0.0f;
        m_waveCount++;

        int spawnX = m_enemyHqTileX;
        int spawnY = m_enemyHqTileY;
        for (const auto &entity : entities.GetAllEntities())
        {
            if (entity.alive && entity.state != ActionState::Die && entity.faction == AiFaction() && entity.isBuilding)
            {
                spawnX = std::clamp(entity.tileX - 1, 1, map.GetWidth() - 2);
                spawnY = std::clamp(entity.tileY + 1, 1, map.GetHeight() - 2);
                break;
            }
        }

        // Rifleman, rocket infantry, then a medium and a heavy tank of the computer's faction
        const Faction aiFaction = AiFaction();
        const bool isConfederation = (aiFaction == Faction::Confederation);
        const int rifleArchetype = isConfederation ? 1 : 0, rocketArchetype = isConfederation ? 3 : 2,
                  tankArchetype = isConfederation ? 9 : 8, heavyTankArchetype = isConfederation ? 11 : 10;
        std::vector<int> waveUnitIds;
        int rifleId = entities.SpawnUnitByArchetype(rifleArchetype, spawnX, spawnY, aiFaction);
        int rocketId = entities.SpawnUnitByArchetype(rocketArchetype, spawnX + 1, spawnY, aiFaction);
        if (rifleId > 0)
            waveUnitIds.push_back(rifleId);
        if (rocketId > 0)
            waveUnitIds.push_back(rocketId);
        if (m_waveCount > 1)
        {
            int tankId = entities.SpawnUnitByArchetype(tankArchetype, spawnX, spawnY + 1, aiFaction);
            if (tankId > 0)
                waveUnitIds.push_back(tankId);
        }
        if (m_waveCount > 3)
        {
            int heavyId = entities.SpawnUnitByArchetype(heavyTankArchetype, spawnX + 1, spawnY + 1, aiFaction);
            if (heavyId > 0)
                waveUnitIds.push_back(heavyId);
        }

        if (!waveUnitIds.empty())
        {
            entities.IssueAttackMoveOrder(waveUnitIds, m_playerHqTileX, m_playerHqTileY, pathfinder);
            std::cout << "[MissionController] AI Wave " << m_waveCount << " attacking the player base!" << std::endl;
        }
    }
}

bool MissionController::LoadTestPolygon(TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder)
{
    // Single player: two slots in the classic colours of the factions, the local player in slot 0 (a network or
    // custom match before left its own slots, colours and local slot behind)
    SetPlayerFaction(PlayerFaction());
    return LoadTestMap(map, entities, economy, pathfinder);
}

namespace
{
std::vector<std::pair<int, int>> StartPositionsOf(const nlohmann::json &mapJson, int mapWidth, int mapHeight);
}

std::vector<SkirmishMapInfo> MissionController::ListSkirmishMaps()
{
    std::vector<SkirmishMapInfo> maps;
    for (const std::string &path : LoaderResources::ListMaps())
    {
        std::string text;
        if (!Assets::ReadText(path, text))
            continue;
        const nlohmann::json mapJson = nlohmann::json::parse(text, nullptr, false);
        if (!mapJson.is_object() || !mapJson.contains("terrainRle") || !mapJson.contains("width")
            || !mapJson.contains("height"))
        {
            continue;
        }
        SkirmishMapInfo info;
        info.path = path;
        info.name = std::filesystem::path(path).stem().string();
        if (mapJson.contains("title") && mapJson["title"].is_string())
            info.title = mapJson["title"].get<std::string>();
        info.width = mapJson["width"].get<int>();
        info.height = mapJson["height"].get<int>();
        info.tilesetId = mapJson.value("tilesetId", 0);
        info.players = std::max<int>(2, static_cast<int>(StartPositionsOf(mapJson, info.width, info.height).size()));
        info.hasPrebuilt
            = (mapJson.contains("buildings") && mapJson["buildings"].is_array() && !mapJson["buildings"].empty())
              || (mapJson.contains("units") && mapJson["units"].is_array() && !mapJson["units"].empty());
        maps.push_back(std::move(info));
    }
    // (only skirmish maps: the campaign's are in maps.mission_dir) skirmish_* first, then everything else; numbers
    // compare as numbers
    auto sortKey = [](const SkirmishMapInfo &mapInfo) {
        const std::string &name = mapInfo.name;
        std::size_t digitPos = name.find_first_of("0123456789");
        std::string prefix = digitPos == std::string::npos ? name : name.substr(0, digitPos);
        const int number = digitPos == std::string::npos ? 0 : std::atoi(name.c_str() + digitPos);
        const int group = prefix.rfind("skirmish_", 0) == 0 ? 0 : 1;
        return std::make_tuple(group, prefix, number, name);
    };
    std::sort(maps.begin(), maps.end(), [&](const SkirmishMapInfo &lhs, const SkirmishMapInfo &rhs) {
        return sortKey(lhs) < sortKey(rhs);
    });
    return maps;
}

namespace
{

// Top-left corner of the free size x size square (open ground, no walls) nearest to (targetX, targetY);
// with `reachable` the square's center must lie in that set, and it must keep `minApart` tiles (center to
// center) away from the square at (apartX, apartY). Returns false when there is no such square.
bool FindBaseSite(
    const TileMap &map,
    int size,
    int targetX,
    int targetY,
    const std::vector<char> *reachable,
    int &outX,
    int &outY,
    int apartX = 0,
    int apartY = 0,
    int minApart = 0)
{
    const int mapWidth = map.GetWidth(), mapHeight = map.GetHeight();
    long bestDistSq = -1;
    for (int y = 0; y + size <= mapHeight; ++y)
    {
        for (int x = 0; x + size <= mapWidth; ++x)
        {
            bool isFree = true;
            for (int dy = 0; dy < size && isFree; ++dy)
            {
                for (int dx = 0; dx < size; ++dx)
                {
                    if (!map.IsPassable(x + dx, y + dy) || map.IsWall(x + dx, y + dy))
                    {
                        isFree = false;
                        break;
                    }
                }
            }
            if (!isFree)
                continue;
            if (reachable && !(*reachable)[(y + size / 2) * mapWidth + x + size / 2])
                continue;
            if (minApart > 0
                && DetMath::Hypot(static_cast<float>(x - apartX), static_cast<float>(y - apartY)) < minApart)
                continue;
            const long distSq
                = static_cast<long>(x - targetX) * (x - targetX) + static_cast<long>(y - targetY) * (y - targetY);
            if (bestDistSq < 0 || distSq < bestDistSq)
            {
                bestDistSq = distSq;
                outX = x;
                outY = y;
            }
        }
    }
    return bestDistSq >= 0;
}

// Open ground connected to (startX, startY)
std::vector<char> ReachableFrom(const TileMap &map, int startX, int startY)
{
    const int mapWidth = map.GetWidth(), mapHeight = map.GetHeight();
    std::vector<char> visited(static_cast<std::size_t>(mapWidth) * mapHeight, 0);
    std::queue<std::pair<int, int>> frontier;
    frontier.push({startX, startY});
    visited[startY * mapWidth + startX] = 1;
    while (!frontier.empty())
    {
        const auto [x, y] = frontier.front();
        frontier.pop();
        static const int kDirections[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto &dir : kDirections)
        {
            const int nextX = x + dir[0], nextY = y + dir[1];
            if (nextX < 0 || nextY < 0 || nextX >= mapWidth || nextY >= mapHeight || visited[nextY * mapWidth + nextX])
                continue;
            if (!map.IsPassable(nextX, nextY))
                continue;
            visited[nextY * mapWidth + nextX] = 1;
            frontier.push({nextX, nextY});
        }
    }
    return visited;
}

// Sets starting bases on free ground: buildings with a one tile gap around them (the base stays
// walkable), units on the free tiles next to them
class BasePlanner
{
public:
    BasePlanner(TileMap &map, EntityManager &entities)
        : m_map(map)
        , m_entities(entities)
        , m_mapWidth(map.GetWidth())
        , m_mapHeight(map.GetHeight())
        , m_taken(static_cast<std::size_t>(m_mapWidth) * m_mapHeight, 0)
    {}

    bool Place(int buildingIndex, int centerX, int centerY, Faction faction, PlayerId owner, int &outX, int &outY)
    {
        const BuildingConfig *buildingConfig = m_entities.GetBuildingConfig(buildingIndex);
        const int buildingWidth = buildingConfig ? buildingConfig->width : 3;
        const int buildingHeight = buildingConfig ? buildingConfig->height : 3;
        int x = 0, y = 0;
        if (!FindSpot(buildingWidth + 1, buildingHeight + 1, centerX - 1, centerY - 1, x, y))
            return false;
        x += 1;
        y += 1;
        if (m_entities.SpawnBuildingByIndex(buildingIndex, x, y, faction, 1.0f, owner) <= 0)
            return false;
        for (int tileY = y; tileY < y + buildingHeight && tileY < m_mapHeight; ++tileY)
        {
            for (int tileX = x; tileX < x + buildingWidth && tileX < m_mapWidth; ++tileX)
            {
                m_map.SetPassable(tileX, tileY, false);
                m_taken[tileY * m_mapWidth + tileX] = 1;
            }
        }
        outX = x;
        outY = y;
        return true;
    }

    void PlaceUnits(const int (&archetypes)[4], int centerX, int centerY, Faction faction, PlayerId owner)
    {
        for (int archetypeId : archetypes)
        {
            int x = 0, y = 0;
            if (!FindSpot(1, 1, centerX, centerY, x, y))
                continue;
            m_taken[y * m_mapWidth + x] = 1;
            m_entities.SpawnUnitByArchetype(archetypeId, x, y, faction, owner);
        }
    }

private:
    bool TileFree(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < m_mapWidth && y < m_mapHeight && m_map.IsPassable(x, y) && !m_map.IsWall(x, y)
               && !m_taken[y * m_mapWidth + x];
    }
    // Nearest spot (by ring distance from centerX, centerY) where a width x height rectangle is free
    bool FindSpot(int width, int height, int centerX, int centerY, int &outX, int &outY) const
    {
        for (int ring = 0; ring < 40; ++ring)
        {
            long bestDistSq = -1;
            for (int y = centerY - ring; y <= centerY + ring; ++y)
            {
                for (int x = centerX - ring; x <= centerX + ring; ++x)
                {
                    if (std::max(std::abs(x - centerX), std::abs(y - centerY)) != ring)
                        continue;
                    bool isFree = true;
                    for (int dy = 0; dy < height && isFree; ++dy)
                    {
                        for (int dx = 0; dx < width; ++dx)
                        {
                            if (!TileFree(x + dx, y + dy))
                            {
                                isFree = false;
                                break;
                            }
                        }
                    }
                    if (!isFree)
                        continue;
                    const long distSq = static_cast<long>(x - centerX) * (x - centerX)
                                        + static_cast<long>(y - centerY) * (y - centerY);
                    if (bestDistSq < 0 || distSq < bestDistSq)
                    {
                        bestDistSq = distSq;
                        outX = x;
                        outY = y;
                    }
                }
            }
            if (bestDistSq >= 0)
                return true;
        }
        return false;
    }

    TileMap &m_map;
    EntityManager &m_entities;
    int m_mapWidth, m_mapHeight;
    std::vector<char> m_taken;
};

// The starting base of a faction: headquarters, power plant, barracks (building indices) and four units
struct StartingBase
{
    int hq, power, barracks;
    int units[4];
};
const StartingBase kResistanceBase {8, 9, 10, {0, 0, 0, 2}};
const StartingBase kConfederationBase {0, 1, 2, {1, 1, 1, 3}};
const StartingBase &startingBase(Faction faction)
{
    return faction == Faction::Confederation ? kConfederationBase : kResistanceBase;
}

std::vector<std::pair<int, int>> StartPositionsOf(const nlohmann::json &mapJson, int mapWidth, int mapHeight)
{
    std::vector<std::pair<int, int>> positions;
    auto addPosition = [&](int x, int y) {
        x = std::clamp(x, 0, std::max(0, mapWidth - 1));
        y = std::clamp(y, 0, std::max(0, mapHeight - 1));
        for (const auto &existing : positions)
        {
            if (std::abs(existing.first - x) < 6 && std::abs(existing.second - y) < 6)
                return; // the same spot
        }
        if (positions.size() < static_cast<std::size_t>(kMaxPlayers))
            positions.push_back({x, y});
    };
    if (mapJson.is_object() && mapJson.contains("start_positions") && mapJson["start_positions"].is_array())
    {
        for (const auto &entry : mapJson["start_positions"])
        {
            if (entry.is_object())
                addPosition(entry.value("x", 0), entry.value("y", 0));
        }
    }
    // The map says how many players it has: its own start positions; a map that lists none is a map for two
    // (the classic spots of a custom game: playerPos / enemyPos)
    int playerX = 6, playerY = mapHeight - 14, enemyX = mapWidth - 16, enemyY = 4;
    if (mapJson.is_object() && mapJson.contains("playerPos") && mapJson["playerPos"].is_object())
    {
        playerX = mapJson["playerPos"].value("x", playerX);
        playerY = mapJson["playerPos"].value("y", playerY);
    }
    if (mapJson.is_object() && mapJson.contains("enemyPos") && mapJson["enemyPos"].is_object())
    {
        enemyX = mapJson["enemyPos"].value("x", enemyX);
        enemyY = mapJson["enemyPos"].value("y", enemyY);
    }
    if (positions.empty())
        addPosition(playerX, playerY);
    if (positions.size() < 2)
        addPosition(enemyX, enemyY);
    return positions;
}

// The buildings and units the map itself carries (the lists the map editor writes). `ownerOf(faction, x, y)` says to
// whom one belongs (kNoPlayer: left out); `onHq(owner, faction, x, y)` is told about every headquarters.
void SpawnPrebuilt(
    const nlohmann::json &mapJson,
    TileMap &map,
    EntityManager &entities,
    const std::function<PlayerId(Faction, int, int)> &ownerOf,
    const std::function<void(PlayerId, Faction, int, int)> &onHq)
{
    if (!mapJson.is_object())
        return;
    for (const auto &buildingJson : mapJson.value("buildings", nlohmann::json::array()))
    {
        const Faction faction = Utility::ParseFaction(buildingJson.value("faction", "resistance"));
        const int x = buildingJson.value("x", 0), y = buildingJson.value("y", 0);
        const int buildingIndex = buildingJson.value("buildingIndex", 0);
        const PlayerId owner = ownerOf(faction, x, y);
        if (owner == kNoPlayer)
            continue;
        const int buildingId = entities.SpawnBuildingByIndex(
            buildingIndex, x, y, faction, buildingJson.value("initialProgress", 1.0f), owner);
        const Building *building = buildingId > 0 ? entities.GetBuilding(buildingId) : nullptr;
        if (!building)
            continue;
        for (int tileY = y; tileY < y + building->buildHeight && tileY < map.GetHeight(); ++tileY)
        {
            for (int tileX = x; tileX < x + building->buildWidth && tileX < map.GetWidth(); ++tileX)
                map.SetPassable(tileX, tileY, false);
        }
        if (buildingIndex == 0 || buildingIndex == 8)
            onHq(owner, faction, x, y);
    }
    for (const auto &unitJson : mapJson.value("units", nlohmann::json::array()))
    {
        const Faction faction = Utility::ParseFaction(unitJson.value("faction", "resistance"));
        const int x = unitJson.value("x", 0), y = unitJson.value("y", 0);
        const PlayerId owner = ownerOf(faction, x, y);
        if (owner == kNoPlayer)
            continue;
        entities.SpawnUnitByArchetype(unitJson.value("archetypeId", 0), x, y, faction, owner);
    }
}

} // namespace

bool MissionController::LoadSkirmish(
    std::string_view mapPath,
    TileMap &map,
    EntityManager &entities,
    Economy &economy,
    Pathfinder &pathfinder,
    bool usePrebuilt)
{
    // Single player: two slots in the classic colours of the factions, the local player in slot 0 (a network or
    // custom match before left its own slots, colours and local slot behind)
    SetPlayerFaction(PlayerFaction());
    std::string mapText;
    if (!Assets::ReadText(mapPath, mapText) || !map.LoadMap(mapPath))
    {
        std::cerr << "[MissionController] Failed to load custom game map: " << mapPath << std::endl;
        return false;
    }
    const nlohmann::json mapJson = nlohmann::json::parse(mapText, nullptr, false);
    if (!usePrebuilt)
        map.ClearWalls(); // from scratch: the map's own walls are not part of it
    m_customPrebuilt = usePrebuilt;

    ++m_loadSerial;
    ResetResearch();
    Capabilities::ApplyLevels(-1, TechPreset(mapJson), entities); // tech levels: the map's, else the default
    m_currentMissionId = kSkirmishMissionId;
    m_customMapPath = mapPath;
    const std::string mapName = std::filesystem::path(mapPath).stem().string();
    std::string title = mapName;
    if (mapJson.is_object() && mapJson.contains("title") && mapJson["title"].is_string())
    {
        title = _(mapJson["title"].get<std::string>());
    }
    m_missionName = "Custom game: " + title;
    m_mapTitle = title;
    m_objectiveText = "- Build up your base.\n- Destroy the enemy base and all enemy forces.";
    m_briefingText = "Open skirmish on the map '" + title
                     + "'. The enemy commander builds and fortifies "
                       "his base. Expand, defend and crush the enemy forces.";
    m_missionTime = 0.0f;
    m_waveTimer = 0.0f;
    m_waveCount = 0;
    m_scheduledWaves.clear();
    m_defenseMission = false;
    m_hasDestObjective = false;
    m_isVictory = false;
    m_isDefeat = false;

    entities.Clear();
    economy.Reset(1500);
    pathfinder.SetMap(&map);
    pathfinder.SetEntityManager(&entities);

    // The map's own buildings and units, when the game is to start with them
    bool prebuiltHq[2] = {false, false}; // Resistance, Confederation
    int prebuiltHqX[2] = {0, 0}, prebuiltHqY[2] = {0, 0};
    if (usePrebuilt)
    {
        SpawnPrebuilt(
            mapJson,
            map,
            entities,
            [](Faction, int, int) { return EntityManager::kAutoOwner; },
            [&](PlayerId, Faction faction, int x, int y) {
                const int factionSlot = faction == Faction::Confederation ? 1 : 0;
                prebuiltHq[factionSlot] = true;
                prebuiltHqX[factionSlot] = x;
                prebuiltHqY[factionSlot] = y;
            });
    }

    // Where the bases go: the map's own start positions if it has them, else the bottom-left / top-right corners
    const int mapWidth = map.GetWidth(), mapHeight = map.GetHeight();
    int playerTargetX = 6, playerTargetY = mapHeight - 14;
    int enemyTargetX = mapWidth - 16, enemyTargetY = 4;
    if (mapJson.is_object())
    {
        if (mapJson.contains("playerPos") && mapJson["playerPos"].is_object())
        {
            playerTargetX = mapJson["playerPos"].value("x", playerTargetX);
            playerTargetY = mapJson["playerPos"].value("y", playerTargetY);
        }
        if (mapJson.contains("enemyPos") && mapJson["enemyPos"].is_object())
        {
            enemyTargetX = mapJson["enemyPos"].value("x", enemyTargetX);
            enemyTargetY = mapJson["enemyPos"].value("y", enemyTargetY);
        }
    }
    // Original maps are dense: take the largest open square that exists (10, 8, then 6 tiles) and set the
    // buildings and units on the free ground around its center
    constexpr int kSizes[3] = {10, 8, 6};
    int playerBaseX = 0, playerBaseY = 0, playerSiteSize = 0;
    for (int size : kSizes)
    {
        if (FindBaseSite(map, size, playerTargetX, playerTargetY, nullptr, playerBaseX, playerBaseY))
        {
            playerSiteSize = size;
            break;
        }
    }
    if (playerSiteSize == 0)
    {
        std::cerr << "[MissionController] No room for a base on " << mapPath << std::endl;
        return false;
    }
    if (prebuiltHq[0] && prebuiltHq[1])
    {
        // The map brings both bases: nothing is generated
        m_playerHqTileX = prebuiltHqX[0];
        m_playerHqTileY = prebuiltHqY[0];
        m_enemyHqTileX = prebuiltHqX[1];
        m_enemyHqTileY = prebuiltHqY[1];
        if (PlayerFaction() == Faction::Confederation)
        {
            std::swap(m_playerHqTileX, m_enemyHqTileX);
            std::swap(m_playerHqTileY, m_enemyHqTileY);
        }
        map.SetViewerTeam(Players::TeamOf(Players::Local()));
        entities.UpdateFogOfWar(map);
        std::cout << "[MissionController] Custom game on " << mapPath << " with the bases of the map" << std::endl;
        if (m_onLoaded)
            m_onLoaded();
        return true;
    }
    const std::vector<char> connected
        = ReachableFrom(map, playerBaseX + playerSiteSize / 2, playerBaseY + playerSiteSize / 2);

    // The enemy base is as far from the player as the map allows (at most half of its short side)
    int enemyBaseX = 0, enemyBaseY = 0, enemySiteSize = 0;
    for (int size : kSizes)
    {
        for (int apart = std::min(mapWidth, mapHeight) / 2; apart >= playerSiteSize && enemySiteSize == 0; apart -= 2)
        {
            if (FindBaseSite(
                    map,
                    size,
                    enemyTargetX,
                    enemyTargetY,
                    &connected,
                    enemyBaseX,
                    enemyBaseY,
                    playerBaseX,
                    playerBaseY,
                    apart))
                enemySiteSize = size;
        }
        if (enemySiteSize != 0)
            break;
    }
    if (enemySiteSize == 0)
    {
        std::cerr << "[MissionController] No room for the enemy base on " << mapPath << std::endl;
        return false;
    }

    BasePlanner planner(map, entities);
    const PlayerId autoOwner = EntityManager::kAutoOwner;
    // Resistance: headquarters, power generator, barracks; Confederation: the same three of its own
    const int playerCenterX = playerBaseX + playerSiteSize / 2;
    const int playerCenterY = playerBaseY + playerSiteSize / 2;
    const int enemyCenterX = enemyBaseX + enemySiteSize / 2;
    const int enemyCenterY = enemyBaseY + enemySiteSize / 2;
    int playerHqX = 0, playerHqY = 0, enemyHqX = 0, enemyHqY = 0, unusedX = 0, unusedY = 0;
    // A side whose base the map brought keeps it, the other gets the usual starting base
    if (prebuiltHq[0])
    {
        playerHqX = prebuiltHqX[0];
        playerHqY = prebuiltHqY[0];
    }
    else if (!planner.Place(8, playerCenterX, playerCenterY, Faction::Resistance, autoOwner, playerHqX, playerHqY))
    {
        std::cerr << "[MissionController] Could not set the headquarters on " << mapPath << std::endl;
        return false;
    }
    if (prebuiltHq[1])
    {
        enemyHqX = prebuiltHqX[1];
        enemyHqY = prebuiltHqY[1];
    }
    else if (!planner.Place(0, enemyCenterX, enemyCenterY, Faction::Confederation, autoOwner, enemyHqX, enemyHqY))
    {
        std::cerr << "[MissionController] Could not set the headquarters on " << mapPath << std::endl;
        return false;
    }
    if (!prebuiltHq[0])
    {
        planner.Place(9, playerCenterX, playerCenterY, Faction::Resistance, autoOwner, unusedX, unusedY);
        if (usePrebuilt)
        {
            planner.Place(10, playerCenterX, playerCenterY, Faction::Resistance, autoOwner, unusedX, unusedY);
            planner.PlaceUnits(kResistanceBase.units, playerCenterX, playerCenterY, Faction::Resistance, autoOwner);
        }
    }
    if (!prebuiltHq[1])
    {
        planner.Place(1, enemyCenterX, enemyCenterY, Faction::Confederation, autoOwner, unusedX, unusedY);
        if (usePrebuilt)
        {
            planner.Place(2, enemyCenterX, enemyCenterY, Faction::Confederation, autoOwner, unusedX, unusedY);
            planner.PlaceUnits(kConfederationBase.units, enemyCenterX, enemyCenterY, Faction::Confederation, autoOwner);
        }
    }

    m_playerHqTileX = playerHqX;
    m_playerHqTileY = playerHqY;
    m_enemyHqTileX = enemyHqX;
    m_enemyHqTileY = enemyHqY;
    if (PlayerFaction() == Faction::Confederation)
    {
        std::swap(m_playerHqTileX, m_enemyHqTileX);
        std::swap(m_playerHqTileY, m_enemyHqTileY);
    }

    map.SetViewerTeam(Players::TeamOf(Players::Local())); // the drawn fog is the local player's from the first frame on
    entities.UpdateFogOfWar(map);
    std::cout << "[MissionController] Custom game on " << mapPath << " | Player base: (" << playerBaseX << ","
              << playerBaseY << ") | Enemy base: (" << enemyBaseX << "," << enemyBaseY << ")" << std::endl;
    if (m_onLoaded)
    {
        m_onLoaded();
    }
    return true;
}

std::vector<std::pair<int, int>> MissionController::StartPositions(std::string_view mapPath)
{
    static std::unordered_map<std::string, std::vector<std::pair<int, int>>> s_cache;
    const std::string key(mapPath);
    if (const auto it = s_cache.find(key); it != s_cache.end())
        return it->second;
    std::string text;
    if (!Assets::ReadText(mapPath, text))
        return {};
    const nlohmann::json mapJson = nlohmann::json::parse(text, nullptr, false);
    if (!mapJson.is_object())
        return {};
    auto positions = StartPositionsOf(mapJson, mapJson.value("width", 64), mapJson.value("height", 64));
    s_cache.emplace(key, positions);
    return positions;
}

bool MissionController::LoadMatch(
    const MatchSetup &setup, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder)
{
    Players::setup(setup.slots, setup.local);
    std::string mapText;
    if (!Assets::ReadText(setup.mapPath, mapText) || !map.LoadMap(setup.mapPath))
    {
        std::cerr << "[MissionController] Failed to load match map: " << setup.mapPath << std::endl;
        return false;
    }
    const nlohmann::json mapJson = nlohmann::json::parse(mapText, nullptr, false);
    if (!setup.prebuilt)
        map.ClearWalls();

    ++m_loadSerial;
    ResetResearch();
    // Tech levels: each slot's own, else the match's, else the map's, else the default
    Capabilities::ApplyLevels(setup.techLevel, TechPreset(mapJson), entities);
    m_currentMissionId = kMatchMissionId;
    m_customMapPath.clear();
    m_matchSetup = setup;
    const std::string mapName = std::filesystem::path(setup.mapPath).stem().string();
    std::string title = mapName;
    if (mapJson.is_object() && mapJson.contains("title") && mapJson["title"].is_string())
        title = _(mapJson["title"].get<std::string>());
    m_missionName = (setup.localOnly ? "Custom game: " : "Match: ") + title;
    m_mapTitle = title;
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

    entities.Clear();
    economy.Reset(setup.startingCredits * HandicapPercentOf(setup.slots[setup.local].handicap) / 100);
    economy.SetPlayer(setup.local);
    map.SetFogEnabled(setup.fogOfWar);
    if (!setup.fogOfWar)
    {
        map.ResetAllFog(FogState::Visible);
    }
    pathfinder.SetMap(&map);
    pathfinder.SetEntityManager(&entities);

    const std::vector<std::pair<int, int>> spots = StartPositionsOf(mapJson, map.GetWidth(), map.GetHeight());
    // The buildings and units of the map go to the player of their faction whose start position is the nearest
    std::array<std::pair<int, int>, kMaxPlayers> prebuiltHq {};
    std::array<bool, kMaxPlayers> hasPrebuiltHq {};
    if (setup.prebuilt)
    {
        auto nearestOwner = [&](Faction faction, int x, int y) {
            PlayerId bestOwner = kNoPlayer;
            long bestDistSq = 0;
            for (PlayerId playerIndex = 0; playerIndex < kMaxPlayers; ++playerIndex)
            {
                const PlayerSlot &slot = setup.slots[playerIndex];
                const int spot = setup.startPosition[playerIndex];
                if (!slot.active || slot.faction != faction || spot < 0 || spot >= static_cast<int>(spots.size()))
                    continue;
                const long dx = spots[spot].first - x, dy = spots[spot].second - y;
                const long distSq = dx * dx + dy * dy;
                if (bestOwner == kNoPlayer || distSq < bestDistSq)
                {
                    bestOwner = playerIndex;
                    bestDistSq = distSq;
                }
            }
            return bestOwner;
        };
        SpawnPrebuilt(mapJson, map, entities, nearestOwner, [&](PlayerId owner, Faction, int x, int y) {
            if (owner >= kMaxPlayers)
                return;
            hasPrebuiltHq[owner] = true;
            prebuiltHq[owner] = {x, y};
        });
        for (int tileY = 0; tileY < map.GetHeight(); ++tileY)
        {
            for (int tileX = 0; tileX < map.GetWidth(); ++tileX)
            {
                if (!map.IsWall(tileX, tileY))
                    continue;
                const PlayerId owner = nearestOwner(map.GetWallFaction(tileX, tileY), tileX, tileY);
                if (owner != kNoPlayer)
                    map.SetWallOwner(tileX, tileY, owner);
            }
        }
    }
    BasePlanner planner(map, entities);
    std::vector<char> connected;
    std::vector<std::pair<int, int>> placed; // centers of the bases set so far
    constexpr int kSizes[3] = {10, 8, 6};
    for (PlayerId playerIndex = 0; playerIndex < kMaxPlayers; ++playerIndex)
    {
        const PlayerSlot &slot = setup.slots[playerIndex];
        if (!slot.active)
            continue;
        const int spot = setup.startPosition[playerIndex];
        if (spot < 0 || spot >= static_cast<int>(spots.size()))
        {
            std::cerr << "[MissionController] Player " << int(playerIndex) << " has no start position" << std::endl;
            return false;
        }
        if (hasPrebuiltHq[playerIndex])
        { // the map brought this player's base
            placed.push_back(prebuiltHq[playerIndex]);
            if (playerIndex == setup.local)
            {
                m_playerHqTileX = prebuiltHq[playerIndex].first;
                m_playerHqTileY = prebuiltHq[playerIndex].second;
            }
            else if (Players::Hostile(playerIndex, setup.local))
            {
                m_enemyHqTileX = prebuiltHq[playerIndex].first;
                m_enemyHqTileY = prebuiltHq[playerIndex].second;
            }
            continue;
        }
        // Open ground near the spot, reachable from the first base, away from the bases already set
        int baseX = 0, baseY = 0, siteSize = 0;
        for (int candidateSize : kSizes)
        {
            for (int apart = std::min(map.GetWidth(), map.GetHeight()) / 3; apart >= 0 && siteSize == 0; apart -= 2)
            {
                bool isValidSite = FindBaseSite(
                    map,
                    candidateSize,
                    spots[spot].first,
                    spots[spot].second,
                    connected.empty() ? nullptr : &connected,
                    baseX,
                    baseY);
                for (const auto &placedCenter : placed)
                {
                    if (DetMath::Hypot(
                            static_cast<float>(baseX + candidateSize / 2 - placedCenter.first),
                            static_cast<float>(baseY + candidateSize / 2 - placedCenter.second))
                        < apart)
                        isValidSite = false;
                }
                if (isValidSite)
                    siteSize = candidateSize;
            }
            if (siteSize != 0)
                break;
        }
        if (siteSize == 0)
        {
            std::cerr << "[MissionController] No room for the base of player " << int(playerIndex) << std::endl;
            return false;
        }
        const int centerX = baseX + siteSize / 2, centerY = baseY + siteSize / 2;
        if (connected.empty())
            connected = ReachableFrom(map, centerX, centerY);
        placed.push_back({centerX, centerY});
        const StartingBase &base = startingBase(slot.faction);
        int hqX = 0, hqY = 0, unusedX = 0, unusedY = 0;
        if (!planner.Place(base.hq, centerX, centerY, slot.faction, playerIndex, hqX, hqY))
            return false;
        {
            // The power plant behind the headquarters, away from the nearest enemy start (not the first thing an
            // attack meets); next to the HQ when there is no room back there
            float dirToEnemyX = 0.0f, dirToEnemyY = 0.0f, bestEnemyDist = 1e9f;
            for (PlayerId otherPlayer = 0; otherPlayer < kMaxPlayers; ++otherPlayer)
            {
                const int otherSpot = setup.startPosition[otherPlayer];
                if (otherPlayer == playerIndex || !setup.slots[otherPlayer].active
                    || setup.slots[otherPlayer].team == slot.team || otherSpot < 0
                    || otherSpot >= static_cast<int>(spots.size()))
                    continue;
                const float deltaX = static_cast<float>(spots[otherSpot].first - centerX),
                            deltaY = static_cast<float>(spots[otherSpot].second - centerY);
                const float dist = DetMath::Hypot(deltaX, deltaY);
                if (dist > 0.5f && dist < bestEnemyDist)
                {
                    bestEnemyDist = dist;
                    dirToEnemyX = deltaX / dist;
                    dirToEnemyY = deltaY / dist;
                }
            }
            const int powerX = centerX - static_cast<int>(std::lround(dirToEnemyX * 5.0f)),
                      powerY = centerY - static_cast<int>(std::lround(dirToEnemyY * 5.0f));
            if (!planner.Place(base.power, powerX, powerY, slot.faction, playerIndex, unusedX, unusedY))
                planner.Place(base.power, centerX, centerY, slot.faction, playerIndex, unusedX, unusedY);
        }
        if (setup.prebuilt)
        {
            planner.Place(base.barracks, centerX, centerY, slot.faction, playerIndex, unusedX, unusedY);
            planner.PlaceUnits(base.units, centerX, centerY, slot.faction, playerIndex);
        }
        if (playerIndex == setup.local)
        {
            m_playerHqTileX = hqX;
            m_playerHqTileY = hqY;
        }
        else if (Players::Hostile(playerIndex, setup.local))
        {
            m_enemyHqTileX = hqX;
            m_enemyHqTileY = hqY;
        }
    }

    map.SetViewerTeam(Players::TeamOf(setup.local));
    entities.UpdateFogOfWar(map);
    std::cout << "[MissionController] Match on " << setup.mapPath << " for " << Players::count() << " players"
              << std::endl;
    if (m_onLoaded)
        m_onLoaded();
    return true;
}

bool MissionController::Restart(TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder)
{
    if (IsMatch() && m_matchSetup.localOnly)
    {
        const MatchSetup again = m_matchSetup;
        return LoadMatch(again, map, entities, economy, pathfinder);
    }
    if (!m_customMapPath.empty())
    {
        return LoadSkirmish(m_customMapPath, map, entities, economy, pathfinder, m_customPrebuilt);
    }
    return LoadMission(m_currentMissionId, map, entities, economy, pathfinder);
}

bool MissionController::LoadTestMap(TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder)
{
    // Single player: two slots in the classic colours of the factions, the local player in slot 0 (a network or
    // custom match before left its own slots, colours and local slot behind)
    SetPlayerFaction(PlayerFaction());
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

    const std::string testMapPath = LoaderResources::TestMap();
    std::string mapText;
    bool haveTestMap = Assets::ReadText(testMapPath, mapText);
    if (!haveTestMap)
    {
        std::cerr << "[MissionController] Failed to open test map: " << testMapPath << std::endl;
        return false;
    }

    nlohmann::json mapData = nlohmann::json::parse(mapText);
    if (!map.LoadMap(testMapPath))
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

    entities.Clear();
    economy.Reset(mapData.value("startingCredits", 10000));
    pathfinder.SetMap(&map);
    pathfinder.SetEntityManager(&entities);

    auto parseFaction = [](const nlohmann::json &item) {
        return Utility::ParseFaction(item.value("faction", "resistance"));
    };
    auto placeBuilding = [&](const nlohmann::json &buildingJson) {
        const int buildingIndex = buildingJson.value("buildingIndex", 0);
        const int buildingX = buildingJson.value("x", 0);
        const int buildingY = buildingJson.value("y", 0);
        const Faction faction = parseFaction(buildingJson);
        int buildingId = entities.SpawnBuildingByIndex(
            buildingIndex, buildingX, buildingY, faction, buildingJson.value("initialProgress", 1.0f));
        if (buildingId > 0)
        {
            const Building *building = entities.GetBuilding(buildingId);
            if (building)
            {
                for (int tileY = buildingY; tileY < buildingY + building->buildHeight && tileY < map.GetHeight();
                     ++tileY)
                {
                    for (int tileX = buildingX; tileX < buildingX + building->buildWidth && tileX < map.GetWidth();
                         ++tileX)
                    {
                        map.SetPassable(tileX, tileY, false);
                    }
                }
            }
        }
    };

    for (const auto &buildingJson : mapData.value("buildings", nlohmann::json::array()))
    {
        placeBuilding(buildingJson);
    }
    for (const auto &unitData : mapData.value("units", nlohmann::json::array()))
    {
        const int unitId = entities.SpawnUnitByArchetype(
            unitData.value("archetypeId", 0), unitData.value("x", 0), unitData.value("y", 0), parseFaction(unitData));
        Entity *unit = entities.GetEntity(unitId);
        if (!unit)
            continue;
        if (unitData.contains("maxHp"))
        {
            unit->maxHp = unitData["maxHp"].get<int>();
            unit->currentHp = unit->maxHp;
        }
        if (unitData.contains("attackDamage"))
        {
            unit->attackDamage = unitData["attackDamage"].get<int>();
        }
    }

    std::cout << "[MissionController] Loaded test map from " << testMapPath << " (" << map.GetWidth() << "x"
              << map.GetHeight() << ")." << std::endl;
    if (m_onLoaded)
    {
        m_onLoaded();
    }
    return true;
}

} // namespace OpenArtOfWar
