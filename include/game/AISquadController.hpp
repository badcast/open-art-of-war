#pragma once

#include "core/Config.hpp"
#include "game/Players.hpp"
#include "game/Pathfinder.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace OpenArtOfWar
{

    class TileMap;
    class EntityManager;
    class Renderer;
    class BitmapFontRenderer;
    class Camera;

    /**
     * @brief Per-difficulty army behaviour (loaded from difficulty.yaml "tactics").
     */
    struct AITacticsConfig
    {
        float leashTiles {3.0f};          // max distance of a unit from the group centroid before the group waits
        float leashPerUnit {0.35f};       // extra leash radius per unit beyond the first four (long columns)
        float corridorLeashFactor {1.6f}; // leash multiplier while the leader walks a passage <= 4 tiles wide
        bool waitAtChokepoints {true};    // gather in front of narrow passages before entering them
        float preChokeHoldSeconds {1.5f}; // minimum pause at the pre-choke rally point
        bool syncToSlowest {false};       // march pace set by the slowest unit (it becomes the leader)
        bool armorFront {false};          // tanks lead the formation
        bool meleeRangedPairs {true};     // close-combat units in front, ranged support behind
        float retreatLossRatio {0.0f};    // >0: survivors fall back when this share of the wave died for nothing
        int waveBaseSize {12};            // units in wave 1 (never fewer than ten: the AI does not send a handful)
        int waveGrowth {2};               // extra units per following wave
        int waveMaxSize {18};
        float waveIntervalSeconds {45.0f};    // minimum pause between two waves
        int maxWaveTier {4};                  // strongest composition tier the AI may field (1..4)
        float researchSeconds {30.0f};        // time per Research Lab technology
        float stragglerTimeoutSeconds {6.0f}; // a straggler that cannot close in for this long leaves the group
        int marchStrideTiles {5};             // distance between consecutive march waypoints
        int maxAttackGroups {3};              // groups one wave may split into (1..5), each taking its own route
        int creditsPerExtraGroup {700};       // AI treasury needed for every extra attack group
        bool buildDefenses {true};            // skirmish: wall in the base, towers / bunkers toward the enemy
        // Defense missions: the enemy's starting squads arrive one after another instead of all at once
        float defenseFirstWaveSeconds {40.0f}; // time the player has to prepare before the first squad
        float defenseWaveGapSeconds {20.0f};   // pause between two squads
        int defenseSquadPercent {60};          // share of a squad's units that is actually sent (10..100)
        float defenseTimeScale {1.25f};        // stretches the pauses the mission script puts between later waves
    };

    /**
     * @brief Runtime AI settings editable from the debug overlay.
     */
    struct AIDebugConfig
    {
        bool forceDebugOverride {false};
        int debugDifficultyLevel {1};      // AIDifficulty value used while the override is on
        bool visualizeSquadLeash {true};   // group labels, centroid circle and leash lines
        bool visualizeChokepoints {true};  // highlight narrow passages on the map
        bool disableFogOfWarForAi {false}; // strategic AI sees every player unit
        float waveSpawnMultiplier {1.0f};  // scales the planned wave size
    };

    /**
     * @brief Narrow passages (corridor width <= 2 tiles) on the current map.
     */
    class ChokepointMap
    {
    public:
        void rebuild(const TileMap &map);
        bool isChoke(int x, int y) const;
        // Open width across the passage at a tile (min of horizontal/vertical open run), 0 = blocked
        int openWidth(int x, int y) const;
        int width() const
        {
            return m_width;
        }
        int height() const
        {
            return m_height;
        }

    private:
        int m_width {0};
        int m_height {0};
        std::vector<uint8_t> m_choke;
        std::vector<uint8_t> m_openWidth;
    };

    enum class AISquadState
    {
        Marching,      // advancing waypoint by waypoint along the route
        Regrouping,    // a unit left the leash radius: vanguard holds, stragglers catch up
        ChokeApproach, // gathering at the rally point in front of a narrow passage
        ChokeTransit,  // single-file column through the passage (leash rule suspended)
        Deploying,     // waiting past the passage until the whole group is out
        Engaged,       // fighting; cohesion paused until the area is quiet again
        Assault,       // objective reached: free attack on player structures
        Retreating     // Hard: wave lost without effect, survivors fall back to staging
    };

    const char *squadStateName(AISquadState state);

    struct AISquad
    {
        int id {0};
        int waveNumber {0};
        std::vector<int> unitIds;
        int leaderId {0};
        AISquadState state {AISquadState::Marching};
        AISquadState resumeState {AISquadState::Marching};
        float stateTimer {0.0f};
        float quietTimer {0.0f};
        float orderTimer {0.0f};

        std::vector<PathNode> route; // leader route to the objective
        int progress {0};            // route index the group has reached
        int marchIndex {0};          // route index currently ordered
        int deployIndex {-1};        // route index past the current chokepoint
        int targetX {0};
        int targetY {0};
        int viaX {-1}; // optional flank point the route passes through first
        int viaY {-1};
        int initialSize {0};

        std::unordered_map<int, float> stragglerBest;  // best distance to the centroid while catching up
        std::unordered_map<int, float> stragglerTimer; // time without progress
        std::unordered_map<int, int> playerHpAtLaunch; // for the retreat check

        // Cached for debug drawing
        float centroidX {0.0f};
        float centroidY {0.0f};
        float maxSpread {0.0f};
        float leashRadius {3.0f};
        float stallTimer {0.0f}; // time the centroid has not moved while marching
        float stallX {0.0f};
        float stallY {0.0f};

        PlayerId owner {kNoPlayer}; // whose units (the owner of the first one); its enemies are the targets
        bool attackMove {true};     // legs are attack-move orders (false: plain moves that ignore enemies)
        bool playerSquad {false};   // player control group: dissolves once the destination is reached
    };

    /**
     * @brief Squad Controller + Chokepoint Handler: keeps launched waves together on the march.
     */
    class AISquadController
    {
    public:
        void clear();

        // viaX / viaY (optional): the group first marches through that point, e.g. a second base exit or a flank
        int createSquad(const std::vector<int> &unitIds, int targetX, int targetY, int waveNumber, EntityManager &entities, Pathfinder &pathfinder, const AITacticsConfig &tactics, int viaX = -1, int viaY = -1);

        void update(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder, const ChokepointMap &chokes, const AITacticsConfig &tactics);

        void setStagingPoint(int x, int y)
        {
            m_stagingX = x;
            m_stagingY = y;
        }
        bool isSquadMember(int unitId) const;
        AISquad *findSquad(int squadId);
        // Takes units out of their squads (they got a new order of their own)
        void releaseUnits(const std::vector<int> &unitIds, EntityManager &entities);
        const std::vector<AISquad> &getSquads() const
        {
            return m_squads;
        }

        // Units dropped from groups (stuck stragglers, retreat survivors) since the last call
        std::vector<int> takeReleasedUnits();

        void renderDebug(Renderer &renderer, BitmapFontRenderer &font, const Camera &camera, const EntityManager &entities, float leashTiles) const;

    private:
        void tick(AISquad &sq, float dt, TileMap &map, EntityManager &entities, Pathfinder &pathfinder, const ChokepointMap &chokes, const AITacticsConfig &tactics);
        void advanceMarch(AISquad &sq, TileMap &map, EntityManager &entities, Pathfinder &pathfinder, const ChokepointMap &chokes, const AITacticsConfig &tactics);
        void beginTransit(AISquad &sq, int chokeIndex, EntityManager &entities, Pathfinder &pathfinder, const ChokepointMap &chokes);
        void orderMarch(AISquad &sq, int routeIndex, TileMap &map, EntityManager &entities, Pathfinder &pathfinder, const AITacticsConfig &tactics);
        void paceGroup(const AISquad &sq, EntityManager &entities, float cx, float cy, float leash, bool active);
        void holdUnits(const std::vector<int> &ids, EntityManager &entities);
        void orderLeg(const AISquad &sq, const std::vector<int> &ids, int x, int y, EntityManager &entities, Pathfinder &pathfinder);
        // A member whose path is far longer than the straight way (e.g. to reach a slot behind the crowd
        // or around a wall) stays where it is instead of running across the map; the next leg moves it on
        void capDetours(const AISquad &sq, EntityManager &entities);
        void releaseUnit(AISquad &sq, int unitId, EntityManager &entities);
        void setState(AISquad &sq, AISquadState state);
        void rebuildRoute(AISquad &sq, EntityManager &entities, Pathfinder &pathfinder);
        int pickLeader(const AISquad &sq, EntityManager &entities, const AITacticsConfig &tactics) const;
        int damageDealtSinceLaunch(const AISquad &sq, const EntityManager &entities) const;

        std::vector<AISquad> m_squads;
        std::vector<int> m_released;
        int m_nextSquadId {1};
        int m_stagingX {0};
        int m_stagingY {0};
        float m_tickAccumulator {0.0f};
    };

} // namespace OpenArtOfWar
