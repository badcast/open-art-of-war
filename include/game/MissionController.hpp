#pragma once

#include "core/Config.hpp"
#include "game/Match.hpp"
#include "core/Localization.hpp"
#include "game/Missions.hpp"
#include <string>
#include <vector>
#include <functional>

namespace OpenArtOfWar
{

    class TileMap;
    class EntityManager;
    class Pathfinder;
    class Economy;

    using MissionLoadedFn = std::function<void()>;

    // A map of assets/maps that can be played as a custom game
    struct SkirmishMapInfo
    {
        std::string path;  // "assets/maps/<name>.json"
        std::string name;  // file name without extension
        std::string title; // "title" key: English text that doubles as the gettext msgid, may be empty
        int width {0};
        int height {0};
        int tilesetId {0};
        bool hasPrebuilt {false}; // the map has buildings or units of its own (the start can be "prebuilt" or "from scratch")

        std::string displayName() const
        {
            return title.empty() ? name : _(title);
        }
    };

    struct ScheduledWave
    {
        float triggerTime {0.0f};
        int group {0};
        int x {0};
        int y {0};
        int val {0};
        bool spawned {false};
        int defenseOrder {-1};   // defense mission: n-th starting squad of the enemy (timed by the difficulty)
        float scriptTime {0.0f}; // when the mission script sends it (sum of the command delays before it)
    };

    class MissionController
    {
    public:
        MissionController();

        bool loadMission(int missionId, const std::string &assetRoot, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder);

        // Custom game on any map of assets/maps: both sides get a small starting base next to the open
        // ground nearest to their corner (or to the map's playerPos / enemyPos), the AI builds the rest
        // `usePrebuilt`: the buildings, units and walls the map has are used (a base on the map replaces the generated
        // one of its side); otherwise the map starts from scratch: only the usual starting bases, no walls
        bool loadSkirmish(const std::string &mapPath, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder, bool usePrebuilt = false);

        // Match of several players (network game): every active slot gets the starting base of its faction at
        // its start position; Players is set up from the slots. No AI: every slot is a person.
        bool loadMatch(const MatchSetup &setup, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder);
        static constexpr int kMatchMissionId = 8;
        bool isMatch() const
        {
            return m_currentMissionId == kMatchMissionId;
        }
        const MatchSetup &matchSetup() const
        {
            return m_matchSetup;
        }
        // Where the bases of a map go (up to 8): its "start_positions" (the number of players the map is made for);
        // a map without them has the two classic spots (playerPos / enemyPos)
        static std::vector<std::pair<int, int>> startPositions(const std::string &mapPath);

        // Starts the current mission or custom game map over
        bool restart(const std::string &assetRoot, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder);

        bool loadTestPolygon(TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder);

        bool loadTestMap(const std::string &assetRoot, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder);

        void update(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder);

        void setOnLoaded(MissionLoadedFn fn)
        {
            m_onLoaded = std::move(fn);
        }
        // Defense missions: when the enemy's squads come and how big they are (AI difficulty tactics)
        void setDefensePacing(float firstWaveSeconds, float waveGapSeconds, int squadPercent, float timeScale);
        bool isDefenseMission() const
        {
            return m_defenseMission;
        }
        void setUseBuiltinWaves(bool use)
        {
            m_useBuiltinWaves = use;
        }
        bool getUseBuiltinWaves() const
        {
            return m_useBuiltinWaves;
        }

        int getCurrentMissionId() const
        {
            return m_currentMissionId;
        }
        // Changes on every loadMission (entity ids from the previous game are stale afterwards)
        int getLoadSerial() const
        {
            return m_loadSerial;
        }
        // Open skirmish (main menu "Custom game"): the AI builds and fortifies its base
        static constexpr int kSkirmishMissionId = 7;
        bool isSkirmish() const
        {
            return m_currentMissionId == kSkirmishMissionId;
        }
        int getPlayerHqX() const
        {
            return m_playerHqTileX;
        }
        int getPlayerHqY() const
        {
            return m_playerHqTileY;
        }
        int getEnemyHqX() const
        {
            return m_enemyHqTileX;
        }
        int getEnemyHqY() const
        {
            return m_enemyHqTileY;
        }
        const std::string &getMissionName() const
        {
            return m_missionName;
        }
        const std::string &getBriefingText() const
        {
            return m_briefingText;
        }
        const std::string &getObjectiveText() const
        {
            return m_objectiveText;
        }

        bool isVictory() const
        {
            return m_isVictory;
        }
        bool isDefeat() const
        {
            return m_isDefeat;
        }

        // The missions of the campaign of the side the player commands (Resistance: Liberation of Peru,
        // Confederation: Global Confederation); ids index this list
        static const std::vector<MissionInfo> &getAvailableMissions();
        // The campaign is the first kCampaignMissionCount entries of getAvailableMissions()
        static constexpr int kCampaignMissionCount = 7;
        // Every map of assets/maps, ordered by name (m*, then map_mission_*, then map_skirmish_*, then the rest)
        static std::vector<SkirmishMapInfo> listSkirmishMaps();

    private:
        void spawnSquadFromCmd27(int group, int x, int y, int val, TileMap &map, EntityManager &entities, Pathfinder *pathfinder = nullptr);

        int m_loadSerial {0};
        MatchSetup m_matchSetup;
        int m_currentMissionId {0};
        int m_playerHqTileX {10};
        int m_playerHqTileY {10};
        int m_enemyHqTileX {50};
        int m_enemyHqTileY {50};
        float m_missionTime {0.0f};
        float m_waveTimer {0.0f};
        int m_waveCount {0};

        std::vector<ScheduledWave> m_scheduledWaves;
        bool m_defenseMission {false};
        float m_defenseFirstWave {40.0f};
        float m_defenseWaveGap {20.0f};
        int m_defenseSquadPercent {60};
        float m_defenseTimeScale {1.25f};
        bool m_hasDestObjective {false};
        int m_destX {0};
        int m_destY {0};
        int m_destSize {0};

        bool m_isVictory {false};
        bool m_isDefeat {false};

        std::string m_missionName;
        std::string m_briefingText;
        std::string m_objectiveText;

        std::string m_customMapPath;   // map of the running custom game, empty in a campaign mission
        bool m_customPrebuilt {false}; // ... and whether it was started with the map's own buildings
        MissionLoadedFn m_onLoaded;
        bool m_useBuiltinWaves {false};
    };

} // namespace OpenArtOfWar
