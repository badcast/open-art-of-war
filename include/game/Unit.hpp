#pragma once

#include "game/Entity.hpp"
#include "core/Utility.hpp"
#include "game/Pathfinder.hpp"
#include <vector>
#include <functional>
#include <cmath>

namespace OpenArtOfWar
{

    class TileMap;
    class EntityManager;
    struct UnitConfig;

    /**
     * @brief Delegates invoked by Unit to interact with EntityManager and the world.
     */
    struct UnitDelegates
    {
        std::function<Entity *(int id)> getEntity;
        std::function<void(Entity &shooter, float srcX, float srcY, float dstX, float dstY, int targetEntityId, TileMap &map)> spawnProjectile;
        std::function<void(float x, float y, int animId)> spawnExplosion;
        std::function<bool(int tileX, int tileY, int ignoreId)> isTileOccupied;
        std::function<int(int tileX, int tileY, int ignoreId)> getUnitAtTile;
        std::function<void(PlayerId victim, PlayerId killer)> onKill;
        std::function<bool(PlayerId)> isFortressFlamethrowerUnlocked;
        std::function<const UnitConfig *(int archetypeId)> getUnitConfig;
        std::function<void(Unit &u)> applyResearch; // the faction's completed technologies, after a change of form
        std::function<bool(const Entity &entity, PlayerId observer)> isEntityVisible;
        std::function<int(const Unit &u)> findEnemyInAttackRange;
        std::function<int(const Unit &u)> findTarget;
        std::function<void(const Unit &u, int &outWallX, int &outWallY)> findWallTarget;
        std::function<int(const Unit &u, int &outTargetEnemyId)> findSupportAlly;
    };

    /**
     * @brief Dynamic entity: walks, attacks, transforms, animates.
     */
    class Unit : public Entity
    {
    public:
        int archetypeId {0};
        int pendingArchetypeId {-1};
        float regenAccum {0.0f}; // fractions of a hit point healed so far
        float moveSpeed {1.8f};
        float paceScale {1.0f}; // set by a marching group: the ones ahead walk slower, so the group stays together
        int facingDirection {0};
        bool isSiegeMode {false};
        bool isAttackMoving {false};
        bool isDirectEngaged {false};    // Actively in direct combat with enemy (within attack range and firing)
        bool isSupportAssisting {false}; // Acquired target via depth=1 support assist (prevents depth>1 chaining)
        float muzzleOffsetX {0.0f};      // localePosition X (Forward along barrel)
        float muzzleOffsetY {0.0f};      // localePosition Y (Lateral offset)
        float turretOffsetX {0.0f};      // turret_offset / dome_offset X
        float turretOffsetY {0.0f};      // turret_offset / dome_offset Y
        bool hasCustomTurretOffset {false};
        // How far the turret layer moves away from where it sits with the hull facing south (the facing the
        // offsets are tuned with in the character editor); kept up to date by EntityManager from sprite data
        int moveFireTargetId {0};
        bool autoDeployed {false};    // went into siege mode by itself (packs up again by itself)
        float siegeQuietTimer {0.0f}; // time in auto siege mode without anything to shoot at   // rotating turret shooting at this enemy while the hull keeps driving
        float turretShiftX {0.0f};
        float turretShiftY {0.0f};
        float turnTimer {0.0f};
        float turnStepDelay {0.0f};
        bool waitingForPath {false};
        bool flowMode {false}; // Single-file column (AI chokepoint transit): queue behind allies, no sidesteps
        float pathRetryTimer {0.0f};
        float wallCheckCooldown {0.0f}; // throttles the "is a wall really in the way?" route check

        // Burst fire attributes
        int burstCount {1};
        float burstDelay {0.1f};
        int burstShotsRemaining {0};
        float burstTimer {0.0f};

        // Multi-weapon attributes (e.g. Fortress secondary Flame Tower)
        float secondaryCooldownTimer {0.0f};
        int secondaryBurstShotsRemaining {0};
        float secondaryBurstTimer {0.0f};
        bool killedByFlame {false};

        // Wreck & Remains attributes (from YAML)
        std::string wreckType {"vehicle"}; // "infantry" or "vehicle"
        int wreckComposite {1764};
        int wreckStages {5};
        std::vector<int> deathSequences;
        std::vector<int> flameDeathSequences;
        int selectedDeathSeq {-1}; // Picked upon death: 247/248 for bullet, 259/260 for flame

        // Movement target & path
        int targetTileX {-1};
        int targetTileY {-1};
        bool isManualWallTarget {false};
        float waitBlockedTimer {0.0f}; // Wait before re-pathing behind friendly moving unit
        std::vector<PathNode> path;
        size_t pathIndex {0};

        Unit();
        ~Unit() override = default;

        bool isUnitEntity() const override
        {
            return true;
        }
        Unit *asUnit() override
        {
            return this;
        }
        const Unit *asUnit() const override
        {
            return this;
        }

        bool isInfantry() const
        {
            return Utility::isInfantryArchetype(archetypeId);
        }

        bool isFortress() const noexcept
        {
            return archetypeId == 7 || archetypeId == 18;
        }

        bool hasMoveTarget() const noexcept
        {
            return targetTileX >= 0 && targetTileY >= 0;
        }

        void clearMoveTarget() noexcept
        {
            targetTileX = -1;
            targetTileY = -1;
            waitingForPath = false;
            pathRetryTimer = 0.0f;
        }

        void clearPathState() noexcept
        {
            path.clear();
            pathIndex = 0;
            turnTimer = 0.0f;
            waitBlockedTimer = 0.0f;
            // Keep the reservation only while the unit physically straddles two tiles
            if(std::abs(subX) <= 0.001f && std::abs(subY) <= 0.001f)
            {
                releaseReservation();
            }
        }

        void getMuzzlePoint(float &outX, float &outY) const override;

        // True when the goal is reachable over terrain, walls and buildings (other units ignored),
        // i.e. a blocked path is only congestion and no wall needs to be breached
        bool hasOpenRouteTo(int goalX, int goalY, Pathfinder &pathfinder) const;
        // Same for reaching firing contact with an entity (next to a building's footprint counts)
        bool hasOpenRouteToEntity(const Entity &target, Pathfinder &pathfinder) const;

        void update(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates);
        void updateMovement(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates);
        void updateCombat(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates);
        // Rotating turret on a driving vehicle: aim at an enemy in range and fire without stopping
        void fireOnTheMove(TileMap &map, const UnitDelegates &delegates);
        void updateTransformation(float deltaTime, const UnitDelegates &delegates);
        void updateBurst(float deltaTime, TileMap &map, const UnitDelegates &delegates);

    private:
        // Slides toward the reserved tile using up to `budget` pixels; returns the unused budget.
        float advanceStep(float budget);
        void finalizeArrivalIfPathDone();
    };

} // namespace OpenArtOfWar
