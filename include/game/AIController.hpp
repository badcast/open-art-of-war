#pragma once

#include "core/Config.hpp"
#include "game/Players.hpp"
#include "game/AISquadController.hpp"
#include <vector>
#include <memory>
#include <string>
#include <map>
#include <unordered_set>
#include <deque>

namespace OpenArtOfWar
{

    class TileMap;
    class EntityManager;
    class Pathfinder;
    class Renderer;
    class BitmapFontRenderer;
    class Camera;
    class Unit;
    class Entity;

    enum class AIDifficulty
    {
        Easy = 0,
        Medium = 1,
        Hard = 2,
        Hardcore = 3
    };

    // Escalation curve: waves start with cheap infantry and grow into a full assault force
    enum class AIWaveTier
    {
        Scout = 1,    // waves 1-2: light infantry only
        Vanguard = 2, // waves 3-4: infantry + flamethrowers + 1-2 rocket troopers
        Assault = 3,  // waves 5-6: + tanks, ranged support behind the front line
        Siege = 4     // wave 7+: heavy tanks and MLRS in the middle of the fist
    };

    struct AIWavePlan
    {
        int waveNumber {1};
        AIWaveTier tier {AIWaveTier::Scout};
        std::vector<int> archetypes; // Confederation archetype per unit
    };

    struct AIDifficultyConfig
    {
        std::string id {"medium"};
        std::string name {"Medium"};
        std::string description;
        int startingCredits {1600};
        int creditsPerSec {12};
        float resourceMultiplier {0.80f};
        float productionSpeedMultiplier {1.0f};
        AITacticsConfig tactics;
    };

    std::string difficultyToString(AIDifficulty diff);
    AIDifficulty stringToDifficulty(const std::string &str);

    struct AIStructurePlan
    {
        UnitType type {UnitType::Headquarters};
        int rawBuildingType {0};
        int tileX {0};
        int tileY {0};
        int width {2};
        int height {2};
        int creditCost {300};
        bool destroyed {false};
        float rebuildTimer {0.0f};
    };

    struct AIWallPlan
    {
        int tileX {0};
        int tileY {0};
        Faction faction {Faction::Confederation};
        int creditCost {25};
        bool destroyed {false};
        float rebuildTimer {0.0f};
    };

    // One line of the AI's reasoning log (debug panel "AI thoughts")
    struct AIThought
    {
        float time {0.0f};    // game time in seconds
        std::string category; // WAVE, ATTACK, DEFENSE, RESEARCH, BUILD, ECONOMY
        std::string text;
    };

    // One item of the skirmish base-building plan
    struct AIDefenseSite
    {
        enum Kind
        {
            Tower,
            Bunker,
            Wall,
            Lab,
            PowerPlant
        };
        Kind kind {Tower};
        int tileX {0};
        int tileY {0};
        bool done {false};
    };

    class AIController
    {
    public:
        friend class HeadlessTest;

        AIController();
        ~AIController() = default;

        // enemyHq: this AI's own base (the player's enemy), playerHq: the base it goes for
        void init(int enemyHqX, int enemyHqY, int playerHqX, int playerHqY);
        // The player slot this AI commands (single player: set by init from the factions)
        void setPlayer(PlayerId player);
        PlayerId player() const
        {
            return m_player;
        }
        Faction side() const
        {
            return Players::factionOf(m_player);
        }
        // A living thing of a player hostile to this AI
        bool isFoe(const Entity &e) const;
        void reset();

        // Skirmish: the AI also fortifies and extends its base (walls, towers, bunkers, lab)
        void setSkirmish(bool skirmish)
        {
            m_skirmish = skirmish;
        }
        bool isSkirmish() const
        {
            return m_skirmish;
        }
        const std::vector<AIDefenseSite> &getDefensePlan() const
        {
            return m_defensePlan;
        }
        // Number of groups a wave of readyUnits splits into (money, map size, difficulty)
        int attackGroupCount(int readyUnits, const TileMap &map) const;

        void update(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder);

        // Callbacks for combat reactions
        void onEntityAttacked(int victimId, int attackerId, EntityManager &entities, Pathfinder &pathfinder);
        void onStructureAttacked(int structureId, int attackerId, EntityManager &entities, Pathfinder &pathfinder);

        // Configuration
        void setAggressiveness(float aggro)
        {
            m_aggressiveness = aggro;
        }
        void setEconomyBonus(int creditsPerSec)
        {
            m_creditsPerSec = creditsPerSec;
        }

        // Difficulty system
        bool loadDifficultyConfig(const std::string &path = "assets/characters/difficulty.yaml");
        void setDifficulty(AIDifficulty difficulty);
        AIDifficulty getDifficulty() const
        {
            return m_difficulty;
        }
        // Difficulty actually in effect (the debug override wins over the menu setting)
        AIDifficulty getEffectiveDifficulty() const;
        const AITacticsConfig &getTactics() const
        {
            return m_currentDifficultyConfig.tactics;
        }

        // Debug-mode runtime settings
        const AIDebugConfig &getDebugConfig() const
        {
            return m_debugConfig;
        }
        void setDebugConfig(const AIDebugConfig &config);
        const AIDifficultyConfig &getDifficultyConfig() const
        {
            return m_currentDifficultyConfig;
        }
        const std::map<AIDifficulty, AIDifficultyConfig> &getAllDifficultyConfigs() const
        {
            return m_difficultyConfigs;
        }

        int getEnemyCredits() const
        {
            return m_credits;
        }
        // Debug: set the AI treasury to any value (0 bankrupts it)
        void setEnemyCredits(int credits);

        // Reasoning log, newest last (bounded); current intentions recomputed every frame
        const std::deque<AIThought> &getThoughts() const
        {
            return m_thoughts;
        }
        void clearThoughts()
        {
            m_thoughts.clear();
            m_lastThoughtByKey.clear();
        }
        size_t getThoughtSerial() const
        {
            return m_thoughtSerial;
        } // grows with every new thought
        std::vector<std::string> currentIntentions() const;
        int getWaveCount() const
        {
            return m_waveCount;
        }
        bool isInitialized() const
        {
            return m_initialized;
        }
        void recordStartingBase(const EntityManager &entities, const TileMap &map);

        // Director: wave planning and escalation
        AIWavePlan planWave(int waveNumber, bool hasFactory, bool hasFactoryPlan) const;
        const AIWavePlan &getCurrentWavePlan() const
        {
            return m_currentPlan;
        }
        const std::vector<int> &getInfantryQueue() const
        {
            return m_infantryQueue;
        }
        const std::vector<int> &getVehicleQueue() const
        {
            return m_vehicleQueue;
        }
        int getReadyUnitCount() const
        {
            return m_readyUnits;
        }
        float getWaveCooldown() const
        {
            return m_waveCooldown;
        }
        int getStagingX() const
        {
            return m_stagingX;
        }
        int getStagingY() const
        {
            return m_stagingY;
        }

        // Research Lab: technologies are researched one by one from the first to the last
        int getResearchIndex() const
        {
            return m_researchIndex;
        }
        bool isResearchActive() const
        {
            return m_researchActive;
        }
        float getResearchProgress() const;
        bool isTechnologyResearched(int index) const;

        const AISquadController &getSquadController() const
        {
            return m_squadController;
        }
        const ChokepointMap &getChokepoints() const
        {
            return m_chokes;
        }

        void renderDebug(Renderer &renderer, BitmapFontRenderer &font, const Camera &camera, const EntityManager &entities) const;

    private:
        void refreshDifficulty();
        void preparePlan(int waveNumber, bool hasFactory, bool hasFactoryPlan);
        void updateEconomyAndProduction(float deltaTime, TileMap &map, EntityManager &entities);
        void updateResearch(float deltaTime, EntityManager &entities);
        void updateBaseDefense(EntityManager &entities, Pathfinder &pathfinder);
        void updateWaveLaunch(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder);
        void updateStaging(TileMap &map, EntityManager &entities, Pathfinder &pathfinder);
        void updateStagingPoint(const TileMap &map);
        bool isReserveUnit(const Unit &u) const;
        bool aiSees(const Entity &e, const EntityManager &entities) const;
        void findAttackObjective(const EntityManager &entities, const TileMap &map, int &outX, int &outY) const;
        void updateIdleUnits(EntityManager &entities, Pathfinder &pathfinder);
        void updateRebuilding(float deltaTime, TileMap &map, EntityManager &entities);
        void planBaseDefenses(const TileMap &map, const EntityManager &entities, Pathfinder &pathfinder);
        void updateBaseBuilder(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder);
        bool buildDefenseSite(AIDefenseSite &site, TileMap &map, EntityManager &entities);
        bool findSiteNear(int &x, int &y, int w, int h, int searchRadius, const TileMap &map, const EntityManager &entities) const;
        void flankPoint(int group, int targetX, int targetY, const TileMap &map, int &outX, int &outY) const;
        // Adds a line to the reasoning log (and the console)
        void think(const std::string &category, const std::string &text);
        // Same, but only when the text for that key changed (state transitions, not every tick)
        void thinkOnce(const std::string &key, const std::string &category, const std::string &text);

        PlayerId m_player {1};
        bool m_playerFixed {false};
        int m_enemyHqX {0};
        int m_enemyHqY {0};
        int m_playerHqX {0};
        int m_playerHqY {0};
        bool m_initialized {false};

        float m_gameTime {0.0f};
        int m_credits {2000};
        int m_creditsPerSec {15};
        float m_economyTimer {0.0f};

        float m_defenseCheckTimer {0.0f};
        int m_waveCount {0};
        float m_aggressiveness {1.0f};

        // Director: wave plan, production queues and staging
        AIWavePlan m_currentPlan;
        std::vector<int> m_infantryQueue;
        std::vector<int> m_vehicleQueue;
        float m_waveCooldown {0.0f};
        float m_waveAssemblyTimer {0.0f};
        int m_readyUnits {0};
        bool m_hasProduction {true};
        int m_stagingX {0};
        int m_stagingY {0};

        // Research Lab progress
        int m_researchIndex {0};
        bool m_researchActive {false};
        bool m_hasLab {false};

        // Units placed by the mission (base garrison) never join the waves
        std::unordered_set<int> m_garrison;
        bool m_garrisonRecorded {false};
        float m_researchTimer {0.0f};

        // Squad Controller + chokepoints
        AISquadController m_squadController;
        ChokepointMap m_chokes;
        float m_chokeRebuildTimer {0.0f};
        AIDebugConfig m_debugConfig;

        std::vector<AIStructurePlan> m_startingBuildings;
        std::vector<AIWallPlan> m_startingWalls;
        bool m_startingBaseRecorded {false};

        // Skirmish base builder
        bool m_skirmish {false};
        bool m_defensePlanned {false};
        std::vector<AIDefenseSite> m_defensePlan;
        float m_buildTimer {0.0f};
        float m_expandTimer {0.0f};
        int m_expansions {0};
        int m_gateX[2] {-1, -1}; // the two openings in the wall the attack groups march out of
        int m_gateY[2] {-1, -1};
        int m_lastThreatX {-1}; // where the player last showed up at the base
        int m_lastThreatY {-1};
        int m_lastThreatId {0};

        std::deque<AIThought> m_thoughts;
        size_t m_thoughtSerial {0};
        std::map<std::string, std::string> m_lastThoughtByKey;

        void initDifficultyDefaults();
        AIDifficulty m_difficulty {AIDifficulty::Medium};
        AIDifficultyConfig m_currentDifficultyConfig;
        std::map<AIDifficulty, AIDifficultyConfig> m_difficultyConfigs;
    };

} // namespace OpenArtOfWar
