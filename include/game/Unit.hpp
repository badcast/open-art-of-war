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
    std::function<void(
        Entity &shooter, float srcX, float srcY, float dstX, float dstY, int targetEntityId, TileMap &map)>
        spawnProjectile;
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
    float siegeQuietTimer {0.0f}; // time in auto siege mode without anything to shoot at   // rotating turret shooting
                                  // at this enemy while the hull keeps driving
    float turretShiftX {0.0f};
    float turretShiftY {0.0f};
    float turnTimer {0.0f};
    float turnStepDelay {0.0f};
    float turretTurnTimer {0.0f};
    float turretTurnStepDelay {0.0f};
    bool waitingForPath {false};
    bool flowMode {false}; // Single-file column (AI chokepoint transit): queue behind allies, no sidesteps
    float pathRetryTimer {0.0f};
    float wallCheckCooldown {0.0f}; // throttles the "is a wall really in the way?" route check
    // Wall breaching: a unit of a group order may shoot its way through hostile walls, like an attack-move
    bool groupOrder {false};
    // Hold position: the unit fires from where it stands and never walks off by itself (not even after an attacker)
    bool holdPosition {false};
    // Time spent with a move goal without leaving the current tile (queueing at a gap, stuck behind allies)
    float blockedTimer {0.0f};
    int blockedTileX {-1};
    int blockedTileY {-1};

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

    // Player orders: the clicked point of the current move / attack-move order (world pixels, -1 = none; the order
    // line ends there, not at the unit's own formation tile) and the orders queued behind it with Shift
    struct QueuedOrder
    {
        int kind {0}; // OrderKind
        int x {0};    // world pixels of the click
        int y {0};
    };
    int orderX {-1};
    int orderY {-1};
    std::vector<QueuedOrder> orderQueue;

    // Movement target & path
    int targetTileX {-1};
    int targetTileY {-1};
    bool isManualWallTarget {false};
    float waitBlockedTimer {0.0f}; // Wait before re-pathing behind friendly moving unit

    // Making way for allies (Unit::requestMakeWay): a blocked unit asks the allies in its way to step
    // aside, as a chain when the first one has no free tile next to it
    float allyWaitTimer {0.0f};   // while > 0 a moving ally in the way is waited for (a chain is moving aside)
    float makeWayCooldown {0.0f}; // throttles the make-way requests of this unit
    int yieldHomeX {-1};          // where a unit without orders stood before it stepped aside (it walks back)
    int yieldHomeY {-1};
    float yieldHomeTimer {0.0f};
    int yieldHomeTries {0};
    std::vector<PathNode> path;
    size_t pathIndex {0};

    Unit();
    ~Unit() override = default;

    bool IsUnitEntity() const override
    {
        return true;
    }
    Unit *AsUnit() override
    {
        return this;
    }
    const Unit *AsUnit() const override
    {
        return this;
    }

    bool IsInfantry() const
    {
        return Utility::IsInfantryArchetype(archetypeId);
    }

    bool IsFortress() const noexcept
    {
        return archetypeId == 7 || archetypeId == 18;
    }

    // Nothing left of the current order: no move, no target, no path being looked for (the next queued order may
    // start)
    bool OrderFinished() const noexcept
    {
        return !HasMoveTarget() && targetEntityId == 0 && targetWallTileX < 0 && !waitingForPath
               && state != ActionState::Move;
    }

    bool HasMoveTarget() const noexcept
    {
        return targetTileX >= 0 && targetTileY >= 0;
    }

    void ClearMoveTarget() noexcept
    {
        targetTileX = -1;
        targetTileY = -1;
        waitingForPath = false;
        pathRetryTimer = 0.0f;
    }

    void ClearPathState() noexcept
    {
        path.clear();
        pathIndex = 0;
        turnTimer = 0.0f;
        waitBlockedTimer = 0.0f;
        // Keep the reservation only while the unit physically straddles two tiles
        if (std::abs(subX) <= 0.001f && std::abs(subY) <= 0.001f)
        {
            ReleaseReservation();
        }
    }

    void GetMuzzlePoint(float &outX, float &outY) const override;

    // True when the goal is reachable over terrain, walls and buildings (other units ignored),
    // i.e. a blocked path is only congestion and no wall needs to be breached
    // outRouteLen (optional): the number of steps of that route
    bool HasOpenRouteTo(int goalX, int goalY, Pathfinder &pathfinder, int *outRouteLen = nullptr) const;
    // May this unit pick hostile walls as targets on its own (attack-move, group order, computer player)?
    bool CanBreachWalls() const;
    // True when shooting through a hostile wall beats walking: no open route, a long detour around
    // the wall line, or the unit has been queueing at a narrow gap for too long
    bool ShouldBreachWalls(int goalX, int goalY, Pathfinder &pathfinder) const;
    // Where to walk to shoot `target`: the tile on the line target -> unit a little inside firing range
    // (walking to the target's own tile brought every unit much too close). The target's tile for
    // buildings, and when that tile is not open.
    void ApproachTile(const Entity &target, const TileMap &map, int &outX, int &outY) const;
    // findPath to approachTile, falling back to the target's tile
    bool FindPathToAttack(const Entity &target, Pathfinder &pathfinder, bool *outPartial);
    // Same for reaching firing contact with an entity (next to a building's footprint counts)
    bool HasOpenRouteToEntity(const Entity &target, Pathfinder &pathfinder) const;

    // Who passes first when two allies are in each other's way: group orders and attack-moves before
    // plain moves, any order before none; equal ones by the lower id (a total order, so never a deadlock)
    int MovePriority() const;
    bool PassesBefore(const Unit &other) const;
    // May this (standing) unit step aside for `requester`? Units without orders always do, units with a
    // goal of their own only for a unit that passes before them
    bool CanMakeWayFor(const Unit &requester) const;
    // Moves `blocker` (and the allies behind it, as a chain) aside to the nearest free tile; true when they go
    bool RequestMakeWay(Unit &blocker, const TileMap &map, const UnitDelegates &delegates);
    // findPath to the goal; when allies standing in a passage make the route partial or a long detour, the
    // route through them (they make way once the unit gets there). avoidX/Y: one extra tile to walk around
    bool FindPathThroughAllies(
        int goalX,
        int goalY,
        Pathfinder &pathfinder,
        std::vector<PathNode> &outPath,
        bool *outPartial,
        int avoidX = -1,
        int avoidY = -1) const;

    void Update(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates);
    void UpdateMovement(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates);
    void UpdateCombat(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates);
    // Rotating turret on a driving vehicle: aim at an enemy in range and fire without stopping
    void FireOnTheMove(float deltaTime, TileMap &map, const UnitDelegates &delegates);
    // Steps turretFacing toward targetDir8 by 45 degrees every turretTurnStepDelay seconds; returns true when aligned
    bool StepTurretToward(int targetDir8, float deltaTime);
    void UpdateTransformation(float deltaTime, const UnitDelegates &delegates);
    void UpdateBurst(float deltaTime, TileMap &map, const UnitDelegates &delegates);

private:
    // Slides toward the reserved tile using up to `budget` pixels; returns the unused budget.
    float AdvanceStep(float budget);
    void FinalizeArrivalIfPathDone();
    // One step of a make-way chain: walk to (x, y), keep the own goal and plan again from there
    void MakeWayStep(int x, int y);
    // A unit that stepped aside without orders walks back to where it stood
    void UpdateYieldHome(float deltaTime, Pathfinder &pathfinder, const UnitDelegates &delegates);
};

} // namespace OpenArtOfWar
