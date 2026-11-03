#pragma once

#include "game/Entity.hpp"
#include "core/Utility.hpp"
#include "game/Pathfinder.hpp"
#include <algorithm>
#include <cstdint>
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

// What a unit without orders does about enemies (the player sets it per unit, the computer by the unit's role)
enum class UnitStance : uint8_t
{
    PursueAndReturn = 0, // engages what it sees, chases a limited distance, then walks back to where it stood
    HoldGround = 1,      // fires only at what is in range and never walks off by itself
    Pursue = 2,          // engages what it sees and chases it until it dies or is lost (default)
    Count
};

// Target preference: 0 armed units, 1 other units and armed buildings, 2 other buildings (lower comes first)
inline int TargetPriorityTier(const Entity &e)
{
    if (!e.isBuilding)
        return e.attackDamage > 0 ? 0 : 1;
    return e.attackDamage > 0 ? 1 : 2;
}

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
    float siegeQuietTimer {0.0f}; // time in auto siege mode without anything to shoot at
    float autoSiegeCooldownTimer {0.0f}; // cooldown after auto-packing before auto-deploying again
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
    // Stance (see UnitStance) and, for PursueAndReturn, the tile it walks back to after a chase
    UnitStance stance {UnitStance::Pursue};
    int stanceHomeX {-1};
    int stanceHomeY {-1};
    bool stanceReturning {false};
    int orderedTargetId {0};       // target of the player's explicit attack order: a stance never drops it
    // Where the current target was last in sight of the team (tiles): an ordered target that drops out of sight
    // (the rest of the group died, the enemy moved off) is looked for there instead of being forgotten
    int lastSeenTargetX {-1};
    int lastSeenTargetY {-1};
    float retargetCooldown {0.0f}; // throttles the look for a more dangerous enemy while fighting
    // Holding ground right now: the hold order, or the HoldGround stance outside attack-moves and ordered attacks
    bool HoldsGround() const noexcept
    {
        return holdPosition
               || (stance == UnitStance::HoldGround && !isAttackMoving
                   && (targetEntityId == 0 || targetEntityId != orderedTargetId));
    }
    // How far (tiles, from home) a PursueAndReturn unit chases before it turns back
    int StanceLeashTiles() const noexcept
    {
        return std::max(sightRadius, static_cast<int>(attackRange)) + 3;
    }
    // Time spent with a move goal without leaving the current tile (queueing at a gap, stuck behind allies)
    float blockedTimer {0.0f};
    int blockedTileX {-1};
    int blockedTileY {-1};

    // Burst fire attributes
    int burstCount {1};
    float burstDelay {0.1f};
    int burstShotsRemaining {0};
    float burstTimer {0.0f};

    // A second gun that fires on its own beside the main one (the Zeus rocket launcher, weapons[1] of the character
    // file): own projectile, damage, range and reload, at the same target
    bool hasSecondary {false};
    bool firingSecondary {false}; // the second gun is the one shooting right now (its sounds are heard)
    ProjectileType secondaryProjectile {ProjectileType::Rocket};
    int secondaryDamage {0};
    float secondaryRange {0.0f};
    float secondaryMinRange {0.0f};
    float secondaryCooldown {2.0f};
    int secondarySplash {0};
    float secondaryMuzzleX {0.0f};
    float secondaryMuzzleY {0.0f};

    // Multi-weapon attributes (e.g. Fortress secondary Flame Tower)
    float secondaryCooldownTimer {0.0f};
    int secondaryBurstShotsRemaining {0};
    float secondaryBurstTimer {0.0f};
    bool killedByFlame {false};

    // Wreck & Remains attributes (from YAML)
    std::string wreckType {"vehicle"}; // "infantry" or "vehicle"
    int wreckComposite {1764};
    int wreckStages {5};
    // animations: block (timing of the pictures; the character editor's Animator edits it)
    float moveFrameTime {0.09f};   // seconds per walking frame
    float attackFrameTime {-1.0f}; // seconds per firing-pose frame (< 0: 0.07, or 0.04 for archetypes 7 and 18)
    float attackPoseTime {
        -1.0f};                  // how long the firing pose lasts after the first shot of a volley (< 0: by projectile)
    float burstPoseTime {0.15f}; // the same after every later shot of a burst
    int flashMode {0};   // muzzle flash layer: 0 loops while firing, 1 plays once per shot, 2 plays once and holds
    int flashFrames {0}; // frames the flash lasts (0 = its whole animation)
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
    // A friendly crowd shuts the next tile: the same road is tried again kBlockRetries times (a short pause each)
    // before a way around is planned, which led across half the map
    int blockRetries {0};
    float blockRetryTimer {0.0f};
    static constexpr int kBlockRetries = 3;
    static constexpr float kBlockRetryPause = 0.25f;
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
    // start). Units transforming or dead are not finished with orders.
    bool OrderFinished() const noexcept
    {
        return !HasMoveTarget() && targetEntityId == 0 && targetWallTileX < 0 && !waitingForPath
               && state != ActionState::Move && state != ActionState::Transform && state != ActionState::Die;
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
    // The tiles a Fortress with the flamethrower upgrade closes in to on a known target (its flame needs it close)
    static constexpr float kFortressFlameApproach = 1.6f;
    void UpdateMovement(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates);
    void UpdateCombat(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates);
    // Rotating turret on a driving vehicle: aim at an enemy in range and fire without stopping
    void FireOnTheMove(float deltaTime, TileMap &map, const UnitDelegates &delegates);
    // Steps turretFacing toward targetDir8 by 45 degrees every turretTurnStepDelay seconds; returns true when aligned
    bool StepTurretToward(int targetDir8, float deltaTime);
    void UpdateImpl(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates);
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
    // PursueAndReturn: keeps the home tile, turns back when a chase goes beyond the leash or ends far away
    void UpdateStanceHome(Pathfinder &pathfinder);
    // May this stance pick `candidate` on its own (PursueAndReturn: only enemies near home)?
    bool StanceAllowsTarget(const Entity &candidate) const;
};

} // namespace OpenArtOfWar
