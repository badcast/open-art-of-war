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
#include <set>

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

std::string DifficultyToString(AIDifficulty diff);
AIDifficulty StringToDifficulty(const std::string &str);

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
        PowerPlant,
        Barracks,
        Factory,
        Headquarters // another headquarters (more income and a wider building zone)
    };
    Kind kind {Tower};
    int tileX {0};
    int tileY {0};
    bool done {false};
    int attempts {0};   // build steps the spot was taken by units (they move on; terrain and buildings do not)
    bool exact {false}; // the spot was chosen with every check already: built right there or not at all
};

// How one computer player builds its base, drawn from the seed of the match and its slot (so two computer players
// of a match, and the same one in two matches, do not build alike). Deterministic: no clock, no shared generator.
struct AIBaseStyle
{
    enum Layout
    {
        Compact, // the buildings pack the rings around the headquarters
        Road,    // they line up along the road out of the base, behind the HQ
        Fan      // they spread to the sides, a little apart
    };
    Layout layout {Compact};
    float turnDeg {0.0f};       // the barracks / factory sides turn by this much around the HQ
    bool mirror {false};        // barracks and factory swap sides
    bool labEarly {false};      // the laboratory comes before the factory
    bool barracksFirst {false}; // a second barracks before the first factory
    int sparePower {40};        // power kept spare before another consumer
    int wallThickness {2};      // rows of a wall across a passage
    int gateWidth {3};          // tiles left open in it for the road
    int lineOffset {0};         // the wall line this many tiles farther out than the nearest place allowed
    bool towersOnly {false};    // hardly any walls: the money goes into towers and bunkers
    bool bunkerHeavy {false};   // more bunkers than towers
    std::string Describe() const;
};

class AIController
{
public:
    friend class HeadlessTest;

    AIController();
    ~AIController() = default;

    // enemyHq: this AI's own base (the player's enemy), playerHq: the base it goes for
    void Init(int enemyHqX, int enemyHqY, int playerHqX, int playerHqY);
    // Match handicap: scales the treasury the computer player starts with
    void ApplyHandicap(int percent)
    {
        m_credits = m_credits * percent / 100;
    }
    // The player slot this AI commands (single player: set by init from the factions)
    void SetPlayer(PlayerId player);
    // The seed of the match (mixed with the slot by the caller): picks the style of the base
    void SetSeed(std::uint32_t seed);
    const AIBaseStyle &BaseStyle() const
    {
        return m_style;
    }
    PlayerId Player() const
    {
        return m_player;
    }
    inline PlayerId player() const
    {
        return Player();
    }
    Faction Side() const
    {
        return Players::FactionOf(m_player);
    }
    inline Faction side() const
    {
        return Side();
    }
    // A living thing of a player hostile to this AI
    bool IsFoe(const Entity &e) const;
    void Reset();

    // Skirmish: the AI also fortifies and extends its base (walls, towers, bunkers, lab)
    void SetSkirmish(bool skirmish)
    {
        m_skirmish = skirmish;
    }
    bool IsSkirmish() const
    {
        return m_skirmish;
    }
    const std::vector<AIDefenseSite> &GetDefensePlan() const
    {
        return m_defensePlan;
    }
    // Number of groups a wave of readyUnits splits into (money, map size, difficulty)
    int AttackGroupCount(int readyUnits, const TileMap &map) const;

    void Update(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder);

    // Callbacks for combat reactions
    void OnEntityAttacked(int victimId, int attackerId, EntityManager &entities, Pathfinder &pathfinder);
    void OnStructureAttacked(int structureId, int attackerId, EntityManager &entities, Pathfinder &pathfinder);

    // Configuration
    void SetAggressiveness(float aggro)
    {
        m_aggressiveness = aggro;
    }
    void SetEconomyBonus(int creditsPerSec)
    {
        m_creditsPerSec = creditsPerSec;
    }

    // Difficulty system
    bool LoadDifficultyConfig(const std::string &path); // LoaderResources::aiDifficulty()
    void SetDifficulty(AIDifficulty difficulty);
    AIDifficulty GetDifficulty() const
    {
        return m_difficulty;
    }
    // Difficulty actually in effect (the debug override wins over the menu setting)
    AIDifficulty GetEffectiveDifficulty() const;
    const AITacticsConfig &GetTactics() const
    {
        return m_currentDifficultyConfig.tactics;
    }

    // Debug-mode runtime settings
    const AIDebugConfig &GetDebugConfig() const
    {
        return m_debugConfig;
    }
    void SetDebugConfig(const AIDebugConfig &config);
    const AIDifficultyConfig &GetDifficultyConfig() const
    {
        return m_currentDifficultyConfig;
    }
    const std::map<AIDifficulty, AIDifficultyConfig> &GetAllDifficultyConfigs() const
    {
        return m_difficultyConfigs;
    }

    int GetEnemyCredits() const
    {
        return m_credits;
    }
    // Debug: set the AI treasury to any value (0 bankrupts it)
    void SetEnemyCredits(int credits);

    // Reasoning log, newest last (bounded); current intentions recomputed every frame
    const std::deque<AIThought> &GetThoughts() const
    {
        return m_thoughts;
    }
    void ClearThoughts()
    {
        m_thoughts.clear();
        m_lastThoughtByKey.clear();
    }
    std::size_t GetThoughtSerial() const
    {
        return m_thoughtSerial;
    } // grows with every new thought
    std::vector<std::string> CurrentIntentions() const;
    int GetWaveCount() const
    {
        return m_waveCount;
    }
    bool IsInitialized() const
    {
        return m_initialized;
    }
    void RecordStartingBase(const EntityManager &entities, const TileMap &map);

    // Director: wave planning and escalation
    AIWavePlan PlanWave(int waveNumber, bool hasFactory, bool hasFactoryPlan) const;
    const AIWavePlan &GetCurrentWavePlan() const
    {
        return m_currentPlan;
    }
    const std::vector<int> &GetInfantryQueue() const
    {
        return m_infantryQueue;
    }
    const std::vector<int> &GetVehicleQueue() const
    {
        return m_vehicleQueue;
    }
    int GetReadyUnitCount() const
    {
        return m_readyUnits;
    }
    float GetWaveCooldown() const
    {
        return m_waveCooldown;
    }
    int GetStagingX() const
    {
        return m_stagingX;
    }
    int GetStagingY() const
    {
        return m_stagingY;
    }

    // Research Lab: technologies are researched one by one from the first to the last
    int GetResearchIndex() const
    {
        return m_researchIndex;
    }
    bool IsResearchActive() const
    {
        return m_researchActive;
    }
    float GetResearchProgress() const;
    bool IsTechnologyResearched(int index) const;

    const AISquadController &GetSquadController() const
    {
        return m_squadController;
    }
    const ChokepointMap &GetChokepoints() const
    {
        return m_chokes;
    }

    void RenderDebug(
        Renderer &renderer, BitmapFontRenderer &font, const Camera &camera, const EntityManager &entities) const;

private:
    void RefreshDifficulty();
    void PreparePlan(int waveNumber, bool hasFactory, bool hasFactoryPlan);
    void UpdateEconomyAndProduction(float deltaTime, TileMap &map, EntityManager &entities);
    void UpdateResearch(float deltaTime, EntityManager &entities);
    void UpdateBaseDefense(EntityManager &entities, Pathfinder &pathfinder);
    void UpdateWaveLaunch(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder);
    void UpdateStaging(TileMap &map, EntityManager &entities, Pathfinder &pathfinder);
    void UpdateStagingPoint(const TileMap &map, Pathfinder &pathfinder, float deltaTime);
    bool IsReserveUnit(const Unit &u) const;
    void UpdateWaveRoster(EntityManager &entities);
    // Stances by role: wave squads pursue, artillery holds its ground, the rest guards the base and returns
    void UpdateUnitStances(EntityManager &entities);
    // Core build order, ahead of the defense plan: a power plant when power runs short, the barracks, a
    // spare power plant when credits allow, the factory. True when something was started this step
    bool UpdateCoreBuildOrder(TileMap &map, EntityManager &entities, bool full = true);
    // --- Base layout (AIBaseLayout.cpp) ---
    // Roads between all headquarters of the map (kept free of buildings) and the ground of this player (nearer to
    // its headquarters than to an ally's): worked out once, when the base is planned
    void PlanTerritory(const TileMap &map, const EntityManager &entities, Pathfinder &pathfinder);
    bool IsRoad(int x, int y) const;     // a road or the band along it
    bool IsRoadCore(int x, int y) const; // the road itself (where the gates go)
    bool IsOwnGround(int x, int y) const;
    // Would a structure on (x, y, w, h) still leave the headquarters, the barracks and the factories a way out (two
    // tiles wide) to the roads leading away from the base?
    // `needsExit`: the structure placed there trains troops or is a headquarters: its own sides must lead out too
    bool KeepsWayOut(
        int x, int y, int w, int h, const TileMap &map, const EntityManager &entities, bool needsExit = false) const;
    // A free place for a structure of the core build order: ring by ring outward from the HQ, the best of a ring by
    // the preferred direction (dx, dy); `rearOnly`: never on the side of the enemy (power plants). False: none
    bool FindCoreSite(
        int &outX,
        int &outY,
        int w,
        int h,
        float dirX,
        float dirY,
        bool rearOnly,
        int minRing,
        const TileMap &map,
        const EntityManager &entities,
        int crowding = 0, // 0: a gap around it, 1: side by side, 2: also on the band along a road
        bool needsExit = false) const;
    // Walls across the narrowest passage on each road to an enemy, gates on the road, towers behind them; true
    // when at least one passage was closed (else the caller falls back to an arc in front of the base)
    bool PlanPassageWalls(
        int cx,
        int cy,
        int hqRadius,
        const TileMap &map,
        const EntityManager &entities,
        std::vector<std::pair<int, int>> &wallTiles);
    // Hard / Hardcore: repairs buildings that are not under fire, paid from the AI treasury
    void UpdateRepairs(float deltaTime, EntityManager &entities);
    // What the computer knows of the enemy: structures and units it has seen (Hardcore: everything)
    void UpdateIntel(EntityManager &entities);
    // A cheap unit looks for the enemy at the start and again every while (not on Hardcore: it sees all)
    void UpdateScouting(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder);
    // Badly hurt expensive units (as dear as a Zeus / Rhino or dearer) leave their squad for repairs,
    // unless the squad is down to a few units; the HP share depends on the difficulty
    void UpdateValuableRetreat(EntityManager &entities, Pathfinder &pathfinder);
    bool IsValuable(const Unit &u, const EntityManager &entities) const;
    // Share of the reserve that stays home when a wave leaves (Easy 40% ... Hardcore 10%)
    float HomeGuardShare() const;
    int HomeGuardCount() const;
    bool AiSees(const Entity &e, const EntityManager &entities) const;
    void FindAttackObjective(const EntityManager &entities, const TileMap &map, int &outX, int &outY) const;
    void UpdateIdleUnits(EntityManager &entities, Pathfinder &pathfinder);
    void UpdateRebuilding(float deltaTime, TileMap &map, EntityManager &entities);
    void PlanBaseDefenses(const TileMap &map, const EntityManager &entities, Pathfinder &pathfinder);
    void UpdateBaseBuilder(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder);
    bool BuildDefenseSite(AIDefenseSite &site, TileMap &map, EntityManager &entities);
    // The power of the base as the economy will count it (finished buildings, the power research bonus), looking
    // ahead: `plannedPlants` also counts the power plants still being built, every consumer counts already (it takes
    // its power the moment it is finished)
    int PowerBalance(const EntityManager &entities, bool plannedPlants) const;
    int m_powerNeeded {0}; // a building that waits for power: what it takes (the next build step raises a plant)
    // unitBlocked (optional): set when only units stood on the free spots, so a later try may succeed
    bool FindSiteNear(
        int &x,
        int &y,
        int w,
        int h,
        int searchRadius,
        const TileMap &map,
        const EntityManager &entities,
        bool *unitBlocked = nullptr,
        bool needsExit = false) const;
    // Units a squad let go far from the base (stragglers, stuck in a passage, retreat timeout, assault over)
    // walk back to the staging area and rejoin the reserve
    void RecallReleasedUnits(EntityManager &entities);
    void FlankPoint(int group, int targetX, int targetY, const TileMap &map, int &outX, int &outY) const;
    // Adds a line to the reasoning log (and the console)
    void Think(const std::string &category, const std::string &text);
    // Same, but only when the text for that key changed (state transitions, not every tick)
    void ThinkOnce(const std::string &key, const std::string &category, const std::string &text);

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
    bool m_factoryReady {false};       // a finished factory was seen (the wave plan is made again once, with vehicles)
    float m_vehicleHoldSeconds {0.0f}; // how long the barracks have been saving credits for the next vehicle
    int m_readyUnits {0};
    bool m_hasProduction {true};
    int m_stagingX {0};
    int m_stagingY {0};
    float m_stagingTimer {0.0f}; // the staging point is placed again every few seconds (the road out may change)

    // Research Lab progress
    int m_researchIndex {0};
    bool m_researchActive {false};
    bool m_hasLab {false};

    // Units placed by the mission (base garrison) never join the waves
    std::unordered_set<int> m_garrison;
    bool m_garrisonRecorded {false};
    std::set<int> m_recalled; // released squad members on their way back to the reserve
    // The computer's own list of the troops for its next wave: a unit joins only explicitly (the computer trained it,
    // a squad released it, or it stood at the home base when the game began), never because it happens to stand
    // in some area. Squads keep their own lists (AISquad::unitIds).
    std::set<int> m_waveRoster;
    bool m_rosterSeeded {false};
    float m_researchTimer {0.0f};

    // Squad Controller + chokepoints
    AISquadController m_squadController;
    ChokepointMap m_chokes;
    float m_chokeRebuildTimer {0.0f};
    AIDebugConfig m_debugConfig;

    std::vector<AIStructurePlan> m_startingBuildings;
    std::vector<AIWallPlan> m_startingWalls;
    bool m_startingBaseRecorded {false};

    // Base layout: the style of this match, the roads and the ground
    std::uint32_t m_seed {0};
    AIBaseStyle m_style;
    bool m_territoryReady {false};
    int m_terrW {0}, m_terrH {0};
    std::vector<std::uint8_t> m_road;      // 2: a road between two headquarters, 1: the band along it, 0: free
    std::vector<std::uint8_t> m_ownGround; // 1: nearer to this player's headquarters than to any ally's
    std::vector<std::vector<std::pair<int, int>>> m_enemyRoads; // from this HQ to every enemy headquarters
    int m_homeX {-1}, m_homeY {-1};                             // centre of the own (first) headquarters
    float m_baseQuietSeconds {0.0f};                            // since a structure of the base was last attacked

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
    std::size_t m_thoughtSerial {0};
    std::map<std::string, std::string> m_lastThoughtByKey;

    // Intel (ordered containers: the same iteration on every peer)
    std::map<int, std::pair<int, int>> m_knownBuildings; // enemy structure id -> centre tile
    std::map<int, bool> m_seenEnemyUnits;                // enemy unit id -> vehicle
    float m_intelTimer {0.0f};
    // Scouting
    int m_scoutId {0};
    int m_scoutX {-1};
    int m_scoutY {-1};
    float m_scoutTimer {0.0f};
    float m_scoutIdleTimer {0.0f};
    int m_scoutTrips {0};
    // Expensive units on their way to repairs (out of the waves until healed)
    std::set<int> m_repairing;
    float m_retreatTimer {0.0f};
    bool m_firstRaidDone {false};
    // Waves that broke nothing: Hard / Hardcore send more groups and more troops after them (as far as the treasury
    // allows), one that destroys a structure resets it
    int m_failedWaves {0};
    int m_trackedWave {0};            // the wave being watched (0: none)
    int m_trackedBuildingsKilled {0}; // enemy structures destroyed before it left
    bool Escalates() const;
    int EscalationTroops() const;
    void UpdateWaveOutcome(const EntityManager &entities);
    int m_trainedUnits {0}; // troops trained since the start (the early raid waits for the first one)

    void InitDifficultyDefaults();
    AIDifficulty m_difficulty {AIDifficulty::Medium};
    AIDifficultyConfig m_currentDifficultyConfig;
    std::map<AIDifficulty, AIDifficultyConfig> m_difficultyConfigs;
};

} // namespace OpenArtOfWar
