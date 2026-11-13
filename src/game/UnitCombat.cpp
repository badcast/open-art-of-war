#include "game/Unit.hpp"
#include "EntityManagerInternal.hpp"
#include "game/Projectiles.hpp"
#include "core/DetMath.hpp"
#include "core/Utility.hpp"
#include "game/Effects.hpp"
#include "game/Sides.hpp"
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include "game/Building.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>
namespace OpenArtOfWar
{

namespace
{
// Queue at a gap in a hostile wall: the gap ahead (toward the goal) is shut by an ally that stands there (fighting,
// or queueing itself). Returns the hostile wall segment beside that gap which the unit can shoot from where it
// stands (the nearest one), so the ones held up widen the gap instead of waiting for it to clear.
bool FindJammedGapWall(const Unit &unit, const TileMap &map, const UnitDelegates &delegates, int &outX, int &outY)
{
    outX = outY = -1;
    if (!delegates.getUnitAtTile || !delegates.getEntity || !unit.HasMoveTarget())
        return false;
    const int mapWidth = map.GetWidth(), mapHeight = map.GetHeight();
    auto inside = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < mapWidth && y < mapHeight;
    };
    auto closed = [&](int x, int y) {
        return !inside(x, y) || !map.IsPassable(x, y) || map.GetWall(x, y) > 0;
    };
    const float range = unit.attackRange * TILE_WIDTH;
    const float unitCenterX = unit.GetUnitCenterPixelX(), unitCenterY = unit.GetUnitCenterPixelY();
    const int ownGoalDist = std::max(std::abs(unit.tileX - unit.targetTileX), std::abs(unit.tileY - unit.targetTileY));
    const int reach = std::clamp(static_cast<int>(std::ceil(unit.attackRange)) + 1, 2, 8);
    float bestDist = 1e9f;
    for (int gapY = unit.tileY - reach; gapY <= unit.tileY + reach; ++gapY)
    {
        for (int gapX = unit.tileX - reach; gapX <= unit.tileX + reach; ++gapX)
        {
            if ((gapX == unit.tileX && gapY == unit.tileY) || closed(gapX, gapY))
                continue;
            // a gap: shut on both sides across it
            if (!((closed(gapX - 1, gapY) && closed(gapX + 1, gapY))
                  || (closed(gapX, gapY - 1) && closed(gapX, gapY + 1))))
                continue;
            // ahead of the unit, on its way to the goal
            if (std::max(std::abs(gapX - unit.targetTileX), std::abs(gapY - unit.targetTileY)) >= ownGoalDist)
                continue;
            const int blockerId = delegates.getUnitAtTile(gapX, gapY, unit.id);
            const Entity *blockerEntity = blockerId > 0 ? delegates.getEntity(blockerId) : nullptr;
            const Unit *blocker = blockerEntity ? blockerEntity->AsUnit() : nullptr;
            if (!blocker || !blocker->alive || blocker->state == ActionState::Die
                || Players::Hostile(blocker->owner, unit.owner) || blocker->HasReservation()
                || (blocker->state == ActionState::Move && !blocker->groupHold))
                continue; // free, or the ally there is walking on
            for (int wallY = gapY - 1; wallY <= gapY + 1; ++wallY)
            {
                for (int wallX = gapX - 1; wallX <= gapX + 1; ++wallX)
                {
                    if (!inside(wallX, wallY) || map.GetWall(wallX, wallY) <= 0
                        || !Players::Hostile(map.GetWallOwner(wallX, wallY), unit.owner))
                        continue;
                    const float dist = DetMath::Hypot(
                        wallX * TILE_WIDTH + TILE_WIDTH * 0.5f - unitCenterX,
                        wallY * TILE_HEIGHT + TILE_HEIGHT * 0.5f - unitCenterY);
                    if (dist <= range && dist < bestDist)
                    {
                        bestDist = dist;
                        outX = wallX;
                        outY = wallY;
                    }
                }
            }
        }
    }
    return outX >= 0;
}
} // namespace

bool Unit::FindPathToAttack(const Entity &target, Pathfinder &pathfinder, bool *outPartial)
{
    int goalX = target.tileX, goalY = target.tileY;
    if (const TileMap *map = pathfinder.GetMap())
        ApproachTile(target, *map, goalX, goalY);
    if (pathfinder.FindPath(tileX, tileY, goalX, goalY, path, id, outPartial))
        return true;
    return (goalX != target.tileX || goalY != target.tileY)
           && pathfinder.FindPath(tileX, tileY, target.tileX, target.tileY, path, id, outPartial);
}

bool Unit::CanBreachWalls() const
{
    return isAttackMoving || groupOrder || !Players::Slot(owner).human;
}

bool Unit::ShouldBreachWalls(int goalX, int goalY, Pathfinder &pathfinder) const
{
    // Tuning: a detour longer than twice the straight way plus 10 tiles goes through the wall instead;
    // so does a unit that has been waiting in front of a gap for 3 seconds
    constexpr int kDetourFactor = 2;
    constexpr int kDetourSlack = 10;
    constexpr float kQueueSeconds = 3.0f;
    int routeLen = 0;
    if (!HasOpenRouteTo(goalX, goalY, pathfinder, &routeLen))
        return true;
    const int straightDist = std::max(std::abs(goalX - tileX), std::abs(goalY - tileY));
    if (routeLen > straightDist * kDetourFactor + kDetourSlack)
        return true;
    // (not right at the goal: there the unit only waits for allies to make room; and only a unit standing
    // at the wall itself - one queueing further back is held up by its allies, not by the wall)
    if (blockedTimer < kQueueSeconds || straightDist <= 3)
        return false;
    const TileMap *map = pathfinder.GetMap();
    if (!map)
        return false;
    constexpr int kAtWall = 1;
    for (int y = tileY - kAtWall; y <= tileY + kAtWall; ++y)
    {
        for (int x = tileX - kAtWall; x <= tileX + kAtWall; ++x)
        {
            if (map->GetWall(x, y) > 0 && Players::Hostile(map->GetWallOwner(x, y), owner))
                return true;
        }
    }
    return false;
}

bool Unit::HasOpenRouteToEntity(const Entity &target, Pathfinder &pathfinder) const
{
    int minX = target.tileX, minY = target.tileY, maxX = target.tileX, maxY = target.tileY;
    if (const Building *building = target.AsBuilding())
    {
        maxX = building->tileX + building->buildWidth - 1;
        maxY = building->tileY + building->buildHeight - 1;
    }
    auto touches = [&](int x, int y) {
        return x >= minX - 1 && x <= maxX + 1 && y >= minY - 1 && y <= maxY + 1;
    };
    if (touches(tileX, tileY))
        return true;
    // A building fills its own tiles: the route counts as open when it ends right next to the footprint
    std::vector<PathNode> route;
    bool partial = false;
    pathfinder.SetIgnoreUnitsOnce();
    if (!pathfinder.FindPath(tileX, tileY, target.tileX, target.tileY, route, id, &partial) || route.empty())
        return false;
    return !partial || touches(route.back().x, route.back().y);
}

bool Unit::StanceAllowsTarget(const Entity &candidate) const
{
    if (stance != UnitStance::PursueAndReturn || isAttackMoving || stanceHomeX < 0)
        return true;
    const int candidateTileX = static_cast<int>(candidate.GetUnitCenterPixelX() / TILE_WIDTH);
    const int candidateTileY = static_cast<int>(candidate.GetUnitCenterPixelY() / TILE_HEIGHT);
    return std::max(std::abs(candidateTileX - stanceHomeX), std::abs(candidateTileY - stanceHomeY))
           <= StanceLeashTiles();
}

void Unit::UpdateStanceHome(Pathfinder &pathfinder)
{
    if (stance != UnitStance::PursueAndReturn || moveSpeed <= 0.0f || isSiegeMode)
    {
        stanceHomeX = stanceHomeY = -1;
        stanceReturning = false;
        return;
    }
    // A player order owns the unit; the home becomes the place where the order ends
    const bool ordered = HasMoveTarget() || isAttackMoving || !orderQueue.empty() || isManualWallTarget || holdPosition
                         || (orderedTargetId != 0 && targetEntityId == orderedTargetId);
    if (ordered)
    {
        stanceHomeX = stanceHomeY = -1;
        stanceReturning = false;
        return;
    }
    const bool settled
        = state == ActionState::Idle && targetEntityId == 0 && targetWallTileX < 0 && path.empty() && !waitingForPath;
    if (stanceHomeX < 0)
    {
        if (settled)
        {
            stanceHomeX = tileX;
            stanceHomeY = tileY;
        }
        return;
    }
    const int distFromHome = std::max(std::abs(tileX - stanceHomeX), std::abs(tileY - stanceHomeY));
    if (stanceReturning)
    {
        if (state != ActionState::Move || path.empty())
        {
            // Back home, or as near as the way allows: that is where it stands now
            stanceReturning = false;
            stanceHomeX = tileX;
            stanceHomeY = tileY;
        }
        return;
    }
    const bool tooFar = targetEntityId != 0 && distFromHome > StanceLeashTiles();
    const bool chaseOver = settled && distFromHome > 1;
    if (!tooFar && !chaseOver)
        return;
    targetEntityId = 0;
    isDirectEngaged = false;
    isSupportAssisting = false;
    lastAttackerId = 0;
    lastAttackedTimer = 0.0f;
    ClearPathState();
    bool partial = false;
    if (pathfinder.FindPath(tileX, tileY, stanceHomeX, stanceHomeY, path, id, &partial) && !path.empty())
    {
        pathIndex = 0;
        state = ActionState::Move;
        animFrame = 0;
        stanceReturning = true;
    }
    else
    {
        // No way back: this is the new home
        path.clear();
        stanceHomeX = tileX;
        stanceHomeY = tileY;
        if (state == ActionState::Attack || state == ActionState::Move)
        {
            state = ActionState::Idle;
            animFrame = 0;
        }
    }
}

void Unit::UpdateCombat(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates)
{
    if (state == ActionState::Transform || attackDamage <= 0 || state == ActionState::Die)
    {
        isDirectEngaged = false;
        return;
    }
    if (orderedTargetId != 0 && targetEntityId != orderedTargetId)
        orderedTargetId = 0; // the ordered target is gone (or the unit was given something else)
    UpdateStanceHome(pathfinder);

    // Pure retreat / ground move order (!isAttackMoving && targetEntityId == 0 && targetWallTileX < 0)
    // must NOT be interrupted by auto-targeting:
    if (state == ActionState::Move && targetEntityId == 0 && targetWallTileX < 0 && hasRotatingTurret)
    {
        FireOnTheMove(deltaTime, map, delegates);
    }
    else
    {
        moveFireTargetId = 0;
    }
    if (state == ActionState::Move && !isAttackMoving && targetEntityId == 0 && targetWallTileX < 0 && !groupOrder)
    {
        isDirectEngaged = false;
        return;
    }

    // 1. If attacking an auto-acquired wall (attack-move or idle), prioritize any enemy unit or building in
    //    attackRange, or retaliate immediately against whoever is shooting this unit.
    if (targetWallTileX >= 0 && !isManualWallTarget && (isAttackMoving || !HasMoveTarget()))
    {
        if (delegates.findEnemyInAttackRange)
        {
            const int enemyInRange = delegates.findEnemyInAttackRange(*this);
            if (enemyInRange != 0)
            {
                const Entity *candidate = delegates.getEntity ? delegates.getEntity(enemyInRange) : nullptr;
                if (candidate && StanceAllowsTarget(*candidate))
                {
                    targetWallTileX = -1;
                    targetWallTileY = -1;
                    targetEntityId = enemyInRange;
                    isDirectEngaged = false;
                    isSupportAssisting = false;
                    waitingForPath = false;
                }
            }
        }
        if (targetWallTileX >= 0 && lastAttackedTimer > 0.0f && lastAttackerId != 0
            && !(state == ActionState::Move && !isAttackMoving) && !HoldsGround() && !isSiegeMode && moveSpeed > 0.0f)
        {
            const Entity *attacker = delegates.getEntity ? delegates.getEntity(lastAttackerId) : nullptr;
            if (attacker && attacker->alive && attacker->state != ActionState::Die
                && Players::Hostile(attacker->owner, owner))
            {
                if (StanceAllowsTarget(*attacker)
                    && (ComputeDistanceToEntityPixels(*this, *attacker) <= attackRange * TILE_WIDTH
                        || HasOpenRouteToEntity(*attacker, pathfinder)))
                {
                    targetWallTileX = -1;
                    targetWallTileY = -1;
                    targetEntityId = lastAttackerId;
                    isDirectEngaged = false;
                    isSupportAssisting = false;
                    waitingForPath = false;
                    if (state == ActionState::Attack)
                    {
                        state = ActionState::Idle;
                        animFrame = 0;
                    }
                }
            }
            else
            {
                lastAttackerId = 0;
                lastAttackedTimer = 0.0f;
            }
        }
    }

    // 2. Handle Wall Target Engagement
    if (targetWallTileX >= 0 && targetWallTileY >= 0)
    {
        if (map.GetWall(targetWallTileX, targetWallTileY) == 0)
        {
            targetWallTileX = -1;
            targetWallTileY = -1;
            isManualWallTarget = false;
            isDirectEngaged = false;

            if (HasMoveTarget() && !isSiegeMode && moveSpeed > 0.0f)
            {
                if (std::abs(tileX - targetTileX) > 1 || std::abs(tileY - targetTileY) > 1)
                {
                    bool isPartial = false;
                    if (pathfinder.FindPath(tileX, tileY, targetTileX, targetTileY, path, id, &isPartial))
                    {
                        pathIndex = 0;
                        state = ActionState::Move;
                        waitingForPath = isPartial;
                    }
                    else
                    {
                        waitingForPath = isPartial;
                        state = ActionState::Idle;
                    }
                }
                else
                {
                    ClearMoveTarget();
                    isAttackMoving = false;
                    state = ActionState::Idle;
                }
            }
            else
            {
                state = ActionState::Idle;
            }
            return;
        }

        // Fog of War visibility gate for Siege Mode attacking walls
        if (isSiegeMode && Players::Slot(owner).human
            && !map.IsVisibleTo(targetWallTileX, targetWallTileY, Players::TeamOf(owner)))
        {
            targetWallTileX = -1;
            targetWallTileY = -1;
            isManualWallTarget = false;
            isDirectEngaged = false;
            if (state == ActionState::Attack && attackAnimTimer <= 0.0f)
            {
                state = ActionState::Idle;
                animFrame = 0;
            }
            return;
        }

        float srcX = GetUnitCenterPixelX();
        float srcY = GetUnitCenterPixelY();
        float dstX = targetWallTileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
        float dstY = targetWallTileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
        float dx = dstX - srcX;
        float dy = dstY - srcY;
        float dist = DetMath::Hypot(dx, dy);

        // Siege Mode / Immobile unit cannot chase an out-of-range wall: drop target immediately
        if (dist > attackRange * TILE_WIDTH)
        {
            isDirectEngaged = false;
            if (isSiegeMode || moveSpeed <= 0.0f)
            {
                targetWallTileX = -1;
                targetWallTileY = -1;
                isManualWallTarget = false;
                if (state == ActionState::Attack)
                {
                    state = ActionState::Idle;
                    animFrame = 0;
                }
                return;
            }
            if (state == ActionState::Attack && attackAnimTimer <= 0.0f)
            {
                state = ActionState::Idle;
                animFrame = 0;
            }
            if (state != ActionState::Move && !waitingForPath)
            {
                bool isPartial = false;
                if (pathfinder.FindPath(tileX, tileY, targetWallTileX, targetWallTileY, path, id, &isPartial))
                {
                    pathIndex = 0;
                    state = ActionState::Move;
                    waitingForPath = isPartial;
                }
            }
            return;
        }

        // Within firing range: orient turret/hull and fire when cooldown is ready
        int targetDir8 = Utility::Dir8FromVector(dx, dy);

        if (!isSiegeMode)
        {
            // (the turn in place that aims a hull without a turret must keep counting: clearing the path state resets
            // its timer every tick, and a vehicle whose turn step takes longer than a tick - the Zeus - never turned to
            // the wall it was ordered to shoot)
            const float turnProgress = turnTimer;
            ClearPathState();
            turnTimer = turnProgress;
            if (state == ActionState::Move)
                state = ActionState::Idle;
        }

        if (hasRotatingTurret)
        {
            const bool turretAligned = StepTurretToward(targetDir8, deltaTime);
            if (isSiegeMode)
            {
                facingDirection = turretFacing;
            }
            if (!turretAligned)
                return;
        }
        else if (!IsInfantry() && HasReservation())
        {
            // A hull without a turret finishes the step it is taking first: turning it to the target while it still
            // slides to the next tile would show a vehicle driving sideways
            return;
        }
        else if (state != ActionState::Move)
        {
            if (!IsInfantry() && facingDirection != targetDir8)
            {
                if (turnStepDelay <= 0.0f)
                {
                    facingDirection = targetDir8;
                }
                else
                {
                    turnTimer += deltaTime;
                    while (turnTimer >= turnStepDelay && facingDirection != targetDir8)
                    {
                        turnTimer -= turnStepDelay;
                        int delta = Utility::TurnStep8(facingDirection, targetDir8);
                        facingDirection = (facingDirection + delta + 8) % 8;
                    }
                }
                turretFacing = facingDirection;
                if (facingDirection != targetDir8)
                    return;
            }
            else
            {
                facingDirection = targetDir8;
                turretFacing = targetDir8;
            }
        }

        // A new volley waits for the burst in progress; the cooldown counts from the burst's last shot
        if (cooldownTimer <= 0.0f && burstShotsRemaining == 0)
        {
            cooldownTimer = NextCooldown() + burstDelay * static_cast<float>(std::max(0, burstCount - 1));

            if (IsFortress())
            {
                int tileDeltaX = std::abs(targetWallTileX - tileX);
                int tileDeltaY = std::abs(targetWallTileY - tileY);
                bool isClose = (tileDeltaX <= 1 && tileDeltaY <= 1);
                bool hasUpgrade
                    = delegates.isFortressFlamethrowerUnlocked && delegates.isFortressFlamethrowerUnlocked(owner);
                if (isClose && hasUpgrade)
                {
                    projectile = ProjectileType::Flame;
                    if (delegates.spawnProjectile)
                    {
                        float muzzleX, muzzleY;
                        GetMuzzlePoint(muzzleX, muzzleY);
                        ProjectileType originalProjectile = projectile;
                        projectile = ProjectileType::Bullet;
                        delegates.spawnProjectile(*this, muzzleX, muzzleY, dstX, dstY, 0, map);
                        projectile = originalProjectile;
                    }
                }
                else
                {
                    projectile = ProjectileType::Bullet;
                }
            }

            attackAnimTimer = attackPoseTime >= 0.0f
                                  ? attackPoseTime
                                  : (IsFortress() ? 0.25f : (Projectiles::of(projectile).hitscan ? 0.12f : 0.22f));
            animFrame = 0;
            animTimer = 0.0f;
            state = ActionState::Attack;

            float muzzleX, muzzleY;
            GetMuzzlePoint(muzzleX, muzzleY);
            if (delegates.spawnProjectile)
            {
                delegates.spawnProjectile(*this, muzzleX, muzzleY, dstX, dstY, 0, map);
            }
            if (burstCount > 1)
            {
                burstShotsRemaining = burstCount - 1;
                burstTimer = burstDelay;
            }
        }
        return;
    }

    // 3. Retaliation and Auto-acquire Enemy Target (always run BEFORE auto-acquiring any wall so walls stay low
    //    priority; plain manual moves skip combat and only breach blocking walls if stuck).
    const bool plainMove
        = (state == ActionState::Move && !isAttackMoving && targetEntityId == 0 && targetWallTileX < 0);

    // 3a. Retaliation: a unit shot at (whether on attack-move or idle) goes after the shooter. Not while holding
    //     position, deployed (siege), immobile, or already fighting something it can hit in range.
    if (!plainMove && lastAttackedTimer > 0.0f && lastAttackerId != 0 && lastAttackerId != targetEntityId
        && !HoldsGround() && !isSiegeMode && moveSpeed > 0.0f && targetWallTileX < 0 && attackDamage > 0
        && state != ActionState::Transform && (targetEntityId == 0 || targetEntityId != orderedTargetId))
    {
        Entity *attacker = delegates.getEntity ? delegates.getEntity(lastAttackerId) : nullptr;
        if (attacker && attacker->alive && attacker->state != ActionState::Die
            && Players::Hostile(attacker->owner, owner))
        {
            bool busyInRange = false;
            if (targetEntityId != 0)
            {
                const Entity *currentTarget = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;
                busyInRange = currentTarget && currentTarget->alive && currentTarget->state != ActionState::Die
                              && ComputeDistanceToEntityPixels(*this, *currentTarget) <= attackRange * TILE_WIDTH;
            }
            if (!busyInRange && StanceAllowsTarget(*attacker))
            {
                targetEntityId = lastAttackerId;
                isSupportAssisting = false;
                waitingForPath = false;
                if (state == ActionState::Move)
                {
                    ClearPathState();
                    state = ActionState::Idle;
                    animFrame = 0;
                }
            }
        }
        else
        {
            lastAttackerId = 0;
            lastAttackedTimer = 0.0f;
        }
    }

    // 3b. Auto-acquire Enemy Target & Support Assist (Depth = 1)
    if (!plainMove && targetEntityId == 0
        && (state == ActionState::Idle || isAttackMoving || waitingForPath || !Players::Slot(owner).human))
    {
        if (delegates.findTarget)
        {
            int bestTargetId = delegates.findTarget(*this);
            if (bestTargetId != 0)
            {
                Entity *candidate = delegates.getEntity ? delegates.getEntity(bestTargetId) : nullptr;
                bool validForUnit = (candidate != nullptr);
                if (validForUnit && (isSiegeMode || moveSpeed <= 0.0f || HoldsGround()))
                {
                    // Siege mode and holding units ONLY acquire targets strictly within attackRange
                    float targetDist = ComputeDistanceToEntityPixels(*this, *candidate);
                    validForUnit = (targetDist <= attackRange * TILE_WIDTH);
                    if (!validForUnit && HoldsGround() && delegates.findEnemyInAttackRange)
                    {
                        bestTargetId = delegates.findEnemyInAttackRange(*this);
                        candidate
                            = bestTargetId != 0 && delegates.getEntity ? delegates.getEntity(bestTargetId) : nullptr;
                        validForUnit = candidate != nullptr;
                    }
                }
                if (validForUnit && !StanceAllowsTarget(*candidate))
                    validForUnit = false;
                if (validForUnit && !isManualWallTarget)
                {
                    targetEntityId = bestTargetId;
                    targetWallTileX = -1;
                    targetWallTileY = -1;
                    isSupportAssisting = false;
                    waitingForPath = false;
                }
            }
            else if (isAttackMoving && HasMoveTarget())
            {
                if (state == ActionState::Idle && !isSiegeMode && moveSpeed > 0.0f && !waitingForPath)
                {
                    if (std::abs(tileX - targetTileX) > 1 || std::abs(tileY - targetTileY) > 1)
                    {
                        bool isPartial = false;
                        if (pathfinder.FindPath(tileX, tileY, targetTileX, targetTileY, path, id, &isPartial))
                        {
                            pathIndex = 0;
                            state = ActionState::Move;
                            waitingForPath = isPartial;
                        }
                        else
                        {
                            waitingForPath = isPartial;
                        }
                    }
                    else
                    {
                        isAttackMoving = false;
                        ClearMoveTarget();
                    }
                }
            }
        }

        // Chain Support Assist (strictly depth = 1):
        // Only mobile idle units without a target assist a directly engaged frontline ally in attackRange.
        if (targetEntityId == 0 && state == ActionState::Idle && !isSiegeMode && !HoldsGround() && moveSpeed > 0.0f
            && !waitingForPath && !isManualWallTarget)
        {
            if (delegates.findSupportAlly)
            {
                int supportEnemyId = 0;
                int supportAllyId = delegates.findSupportAlly(*this, supportEnemyId);
                if (supportAllyId != 0 && supportEnemyId != 0)
                {
                    Entity *enemy = delegates.getEntity ? delegates.getEntity(supportEnemyId) : nullptr;
                    if (enemy && enemy->alive && enemy->state != ActionState::Die && StanceAllowsTarget(*enemy))
                    {
                        targetEntityId = supportEnemyId;
                        targetWallTileX = -1;
                        targetWallTileY = -1;
                        isSupportAssisting = true; // Marks depth = 1 so other units do NOT chain off this unit!
                        waitingForPath = false;
                        bool isPartial = false;
                        if (pathfinder.FindPath(tileX, tileY, enemy->tileX, enemy->tileY, path, id, &isPartial))
                        {
                            pathIndex = 0;
                            state = ActionState::Move;
                            waitingForPath = isPartial;
                        }
                    }
                }
            }
        }
    }

    // 4. Auto-acquire blocking wall if no enemy target was found and no enemy is within attackRange.
    //    A partial path is often just congestion (allies standing in the way): only breach when
    //    the goal is unreachable over terrain and walls, checked at most once per second.
    if (wallCheckCooldown > 0.0f)
        wallCheckCooldown -= deltaTime;
    const bool enemyInAttackRange = (isAttackMoving || !HasMoveTarget()) && delegates.findEnemyInAttackRange
                                    && delegates.findEnemyInAttackRange(*this) != 0;
    const bool stuckWithGoal = (state == ActionState::Idle && path.empty() && waitingForPath)
                               || (HasMoveTarget() && blockedTimer >= 3.0f && !isSiegeMode && moveSpeed > 0.0f);
    // On auto-attack, a hostile wall beside the way is shot down with a 50% chance (decided once per unit and wall,
    // deterministically). Low priority: runs only without a target, not under fire, and without enemies in range.
    if (isAttackMoving && HasMoveTarget() && !isSiegeMode && moveSpeed > 0.0f && attackDamage > 0 && targetEntityId == 0
        && targetWallTileX < 0 && !enemyInAttackRange && lastAttackedTimer <= 0.0f && wallCheckCooldown <= 0.0f)
    {
        wallCheckCooldown = 1.0f;
        constexpr int kReach = 3;
        int bestX = -1, bestY = -1, bestDist = 1 << 20;
        for (int y = tileY - kReach; y <= tileY + kReach; ++y)
        {
            for (int x = tileX - kReach; x <= tileX + kReach; ++x)
            {
                if (map.GetWall(x, y) <= 0 || !Players::Hostile(map.GetWallOwner(x, y), owner))
                    continue;
                const std::uint32_t wallHash = static_cast<std::uint32_t>(id) * 73856093u
                                               ^ static_cast<std::uint32_t>(x) * 19349663u
                                               ^ static_cast<std::uint32_t>(y) * 83492791u;
                if (((wallHash >> 7) & 1u) == 0u)
                    continue; // this one is left alone
                const int tileDist = std::abs(x - tileX) + std::abs(y - tileY);
                if (tileDist < bestDist)
                {
                    bestDist = tileDist;
                    bestX = x;
                    bestY = y;
                }
            }
        }
        if (bestX >= 0)
        {
            targetWallTileX = bestX;
            targetWallTileY = bestY;
            if (state == ActionState::Move)
            {
                ClearPathState(); // the move goal stays: the way is planned again once the wall is down
                state = ActionState::Idle;
                animFrame = 0;
            }
            waitingForPath = false;
            blockedTimer = 0.0f;
            return;
        }
    }
    // Held up for a while at a gap that an ally standing in it shuts (one fighting from the gap, say)
    constexpr float kGapJamSeconds = 2.0f;
    const bool jammedAtGap
        = HasMoveTarget() && !isSiegeMode && moveSpeed > 0.0f && attackDamage > 0 && blockedTimer >= kGapJamSeconds;
    if (targetEntityId == 0 && targetWallTileX < 0 && !enemyInAttackRange && (stuckWithGoal || jammedAtGap)
        && HasMoveTarget() && CanBreachWalls() && wallCheckCooldown <= 0.0f)
    {
        wallCheckCooldown = 1.0f;
        int wallX = -1, wallY = -1;
        // (no search left this tick: look again soon)
        const bool searchAllowed = pathfinder.TakeBackgroundSearch();
        if (!searchAllowed)
            wallCheckCooldown = 0.1f;
        if (searchAllowed && stuckWithGoal && delegates.findWallTarget
            && ShouldBreachWalls(targetTileX, targetTileY, pathfinder))
            delegates.findWallTarget(*this, wallX, wallY);
        // The way through is open, only jammed: shoot the wall beside the gap, so it widens
        if (wallX < 0 && jammedAtGap)
            FindJammedGapWall(*this, map, delegates, wallX, wallY);
        if (wallX >= 0)
        {
            targetWallTileX = wallX;
            targetWallTileY = wallY;
            if (state == ActionState::Move)
            {
                // Keep the move goal: the route is planned again once the wall is down
                ClearPathState();
                state = ActionState::Idle;
                animFrame = 0;
            }
            waitingForPath = false;
            blockedTimer = 0.0f;
            return;
        }
    }

    // A plain move (group or not) picks no fights on its own
    if (state == ActionState::Move && !isAttackMoving && targetEntityId == 0 && targetWallTileX < 0)
    {
        isDirectEngaged = false;
        return;
    }

    // 5. Handle Active Enemy Entity Target
    if (targetEntityId != 0)
    {
        Entity *target = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;

        // 5a. Target destroyed or invalid
        if (!target || !target->alive || target->state == ActionState::Die)
        {
            targetEntityId = 0;
            isDirectEngaged = false;
            isSupportAssisting = false;

            if (delegates.findTarget)
            {
                int nextTargetId = delegates.findTarget(*this);
                if (nextTargetId != 0)
                {
                    Entity *candidate = delegates.getEntity ? delegates.getEntity(nextTargetId) : nullptr;
                    if (candidate && StanceAllowsTarget(*candidate)
                        && (!(isSiegeMode || HoldsGround())
                            || ComputeDistanceToEntityPixels(*this, *candidate) <= attackRange * TILE_WIDTH))
                    {
                        targetEntityId = nextTargetId;
                        return;
                    }
                }
            }

            // Resume original move / attack-move order if a valid ground destination exists
            if (HasMoveTarget() && !isSiegeMode && moveSpeed > 0.0f)
            {
                if (std::abs(tileX - targetTileX) > 1 || std::abs(tileY - targetTileY) > 1)
                {
                    bool isPartial = false;
                    if (pathfinder.FindPath(tileX, tileY, targetTileX, targetTileY, path, id, &isPartial))
                    {
                        pathIndex = 0;
                        state = ActionState::Move;
                        waitingForPath = isPartial;
                    }
                    else
                    {
                        waitingForPath = isPartial;
                        state = ActionState::Idle;
                    }
                }
                else
                {
                    ClearMoveTarget();
                    isAttackMoving = false;
                    state = ActionState::Idle;
                }
            }
            else
            {
                state = ActionState::Idle;
                animFrame = 0;
            }
            return;
        }

        // 5a'. A more dangerous enemy in range comes before the current one (a building, an unarmed unit);
        //      an ordered target stays
        retargetCooldown -= deltaTime;
        if (retargetCooldown <= 0.0f && targetEntityId != orderedTargetId && TargetPriorityTier(*target) > 0
            && delegates.findEnemyInAttackRange)
        {
            retargetCooldown = 0.5f;
            const int betterTargetId = delegates.findEnemyInAttackRange(*this);
            const Entity *betterTarget
                = betterTargetId != 0 && delegates.getEntity ? delegates.getEntity(betterTargetId) : nullptr;
            if (betterTarget && betterTargetId != targetEntityId
                && TargetPriorityTier(*betterTarget) < TargetPriorityTier(*target))
            {
                targetEntityId = betterTargetId;
                isSupportAssisting = false;
                return;
            }
        }

        // 5b. Fog of War Visibility Gate (multi-tile aware via delegates.isEntityVisible)
        //     A shooter that just hit this unit is chased even unseen (the unit knows where the fire came from)
        const bool chasingAttacker = targetEntityId == lastAttackerId && lastAttackedTimer > 0.0f;
        const bool targetSeen = !delegates.isEntityVisible || delegates.isEntityVisible(*target, owner);
        if (targetSeen)
        {
            lastSeenTargetX = target->tileX;
            lastSeenTargetY = target->tileY;
        }
        else if (!chasingAttacker)
        {
            // An ordered target and any building (it cannot move off) are not forgotten when they leave the
            // team's sight: the unit walks on to where it last saw them and fires only once they are in sight again
            const bool keepTarget = (targetEntityId == orderedTargetId || target->isBuilding) && moveSpeed > 0.0f
                                    && !isSiegeMode && !HoldsGround() && lastSeenTargetX >= 0;
            const bool reachedLastSeen = !target->isBuilding && std::abs(tileX - lastSeenTargetX) <= 1
                                         && std::abs(tileY - lastSeenTargetY) <= 1;
            if (!keepTarget || reachedLastSeen)
            {
                targetEntityId = 0;
                lastSeenTargetX = lastSeenTargetY = -1;
                isDirectEngaged = false;
                isSupportAssisting = false;
                if (state != ActionState::Move)
                {
                    state = ActionState::Idle;
                    animFrame = 0;
                }
                return;
            }
            isDirectEngaged = false;
            if (state != ActionState::Move && !waitingForPath)
            {
                bool isPartial = false;
                const int goalX = target->isBuilding ? target->tileX : lastSeenTargetX;
                const int goalY = target->isBuilding ? target->tileY : lastSeenTargetY;
                if (pathfinder.FindPath(tileX, tileY, goalX, goalY, path, id, &isPartial))
                {
                    pathIndex = 0;
                    state = ActionState::Move;
                }
                waitingForPath = isPartial;
            }
            return;
        }

        float srcX = GetUnitCenterPixelX();
        float srcY = GetUnitCenterPixelY();
        float dstX = target->GetUnitCenterPixelX();
        float dstY = target->GetUnitCenterPixelY();
        float dx = dstX - srcX;
        float dy = dstY - srcY;
        float dist = ComputeDistanceToEntityPixels(*this, *target);

        // 5b'. The second gun fires on its own beside the main one, at the same target, in its own range
        // (a Fortress placed without the base form, a map's own unit, has its flamethrower only once it is researched)
        const bool secondaryLocked
            = IsFortress()
              && !(delegates.isFortressFlamethrowerUnlocked && delegates.isFortressFlamethrowerUnlocked(owner));
        if (hasSecondary && !secondaryLocked && secondaryCooldownTimer <= 0.0f && delegates.spawnProjectile
            && dist <= secondaryRange * TILE_WIDTH && dist >= secondaryMinRange * TILE_WIDTH)
        {
            secondaryCooldownTimer = secondaryCooldown;
            const int mainDamage = attackDamage, mainSplash = splashPercent;
            const ProjectileType mainProjectile = projectile;
            const float mainMuzzleX = muzzleOffsetX, mainMuzzleY = muzzleOffsetY;
            const bool mainCustomTurret = hasCustomTurretOffset;
            attackDamage = secondaryDamage;
            splashPercent = secondarySplash;
            projectile = secondaryProjectile;
            muzzleOffsetX = secondaryMuzzleX;
            muzzleOffsetY = secondaryMuzzleY;
            float muzzleX, muzzleY;
            GetMuzzlePoint(muzzleX, muzzleY);
            firingSecondary = true;
            delegates.spawnProjectile(*this, muzzleX, muzzleY, dstX, dstY, target->id, map);
            firingSecondary = false;
            attackDamage = mainDamage;
            splashPercent = mainSplash;
            projectile = mainProjectile;
            muzzleOffsetX = mainMuzzleX;
            muzzleOffsetY = mainMuzzleY;
            hasCustomTurretOffset = mainCustomTurret;
            // The second gun shows its own firing pose and flash (not while the hull is driving)
            if (state != ActionState::Move)
            {
                state = ActionState::Attack;
                animFrame = 0;
                animTimer = 0.0f;
                attackAnimTimer = std::max(attackAnimTimer, 0.22f);
            }
        }

        // 5c. Out-of-Range Check (Crucial for Siege Mode Anti-Vibration!)
        if (dist > attackRange * TILE_WIDTH)
        {
            isDirectEngaged = false;

            if (isSiegeMode || moveSpeed <= 0.0f || HoldsGround())
            {
                // Immobile siege unit MUST drop unreachable target immediately without rotating or flickering
                targetEntityId = 0;
                isSupportAssisting = false;
                if (state == ActionState::Attack)
                {
                    state = ActionState::Idle;
                    animFrame = 0;
                }
                return;
            }

            if (state == ActionState::Attack && attackAnimTimer <= 0.0f)
            {
                state = ActionState::Idle;
                animFrame = 0;
            }

            // An enemy unit or building strictly inside attackRange comes before breaching a wall for an unreachable
            // target
            if (isAttackMoving && targetEntityId != orderedTargetId && delegates.findEnemyInAttackRange)
            {
                const int enemyInRange = delegates.findEnemyInAttackRange(*this);
                if (enemyInRange != 0 && enemyInRange != targetEntityId)
                {
                    const Entity *candidate = delegates.getEntity ? delegates.getEntity(enemyInRange) : nullptr;
                    if (candidate && StanceAllowsTarget(*candidate))
                    {
                        targetEntityId = enemyInRange;
                        isSupportAssisting = false;
                        waitingForPath = false;
                        return;
                    }
                }
            }

            // Stuck in front of the target (a base walled in all around): shoot a way through the wall
            if (state != ActionState::Move && path.empty() && CanBreachWalls() && !isManualWallTarget
                && wallCheckCooldown <= 0.0f && delegates.findWallTarget)
            {
                wallCheckCooldown = 1.0f;
                if (!HasOpenRouteToEntity(*target, pathfinder))
                {
                    int wallX = -1, wallY = -1;
                    delegates.findWallTarget(*this, wallX, wallY);
                    if (wallX >= 0)
                    {
                        targetEntityId = 0;
                        isSupportAssisting = false;
                        targetWallTileX = wallX;
                        targetWallTileY = wallY;
                        waitingForPath = false;
                        return;
                    }
                }
            }

            if (state != ActionState::Move && !waitingForPath)
            {
                bool isPartial = false;
                if (FindPathToAttack(*target, pathfinder, &isPartial))
                {
                    pathIndex = 0;
                    state = ActionState::Move;
                    waitingForPath = isPartial;
                }
                else
                {
                    waitingForPath = isPartial;
                }
            }
            return;
        }

        // 5d. Target is Visible AND Within Firing Range: Aim and Fire
        int targetDir8 = Utility::Dir8FromVector(dx, dy);

        // Mark unit as actively engaged in direct firing range
        isDirectEngaged = true;

        if (!isSiegeMode)
        {
            // (the turn in place that aims a hull without a turret must keep counting: clearing the path state resets
            // its timer, and a vehicle whose step takes longer than a tick then never turned to its target)
            const float turnProgress = turnTimer;
            ClearPathState();
            turnTimer = turnProgress;
            if (state == ActionState::Move)
            {
                state = ActionState::Idle;
                animFrame = 0;
            }
        }

        if (hasRotatingTurret)
        {
            const bool turretAligned = StepTurretToward(targetDir8, deltaTime);
            if (isSiegeMode)
            {
                facingDirection = turretFacing;
            }
            if (!turretAligned)
            {
                return;
            }
        }
        else if (!IsInfantry() && HasReservation())
        {
            // A hull without a turret finishes the step it is taking first: turning it to the target while it still
            // slides to the next tile would show a vehicle driving sideways
            return;
        }
        else if (state != ActionState::Move)
        {
            if (!IsInfantry() && facingDirection != targetDir8)
            {
                if (turnStepDelay <= 0.0f)
                {
                    facingDirection = targetDir8;
                }
                else
                {
                    turnTimer += deltaTime;
                    while (turnTimer >= turnStepDelay && facingDirection != targetDir8)
                    {
                        turnTimer -= turnStepDelay;
                        int delta = Utility::TurnStep8(facingDirection, targetDir8);
                        facingDirection = (facingDirection + delta + 8) % 8;
                    }
                }
                turretFacing = facingDirection;
                if (facingDirection != targetDir8)
                {
                    return;
                }
            }
            else
            {
                facingDirection = targetDir8;
                turretFacing = targetDir8;
            }
        }

        // A new volley waits for the burst in progress; the cooldown counts from the burst's last shot
        if (cooldownTimer <= 0.0f && burstShotsRemaining == 0)
        {
            cooldownTimer = NextCooldown() + burstDelay * static_cast<float>(std::max(0, burstCount - 1));

            if (IsFortress())
            {
                int tileDeltaX = std::abs(target->tileX - tileX);
                int tileDeltaY = std::abs(target->tileY - tileY);
                bool isClose = (tileDeltaX <= 1 && tileDeltaY <= 1);
                bool hasUpgrade
                    = delegates.isFortressFlamethrowerUnlocked && delegates.isFortressFlamethrowerUnlocked(owner);
                if (isClose && hasUpgrade)
                {
                    projectile = ProjectileType::Flame;
                    if (delegates.spawnProjectile)
                    {
                        float muzzleX, muzzleY;
                        GetMuzzlePoint(muzzleX, muzzleY);
                        ProjectileType originalProjectile = projectile;
                        projectile = ProjectileType::Bullet;
                        delegates.spawnProjectile(*this, muzzleX, muzzleY, dstX, dstY, target->id, map);
                        projectile = originalProjectile;
                    }
                }
                else
                {
                    projectile = ProjectileType::Bullet;
                }
            }

            attackAnimTimer = attackPoseTime >= 0.0f
                                  ? attackPoseTime
                                  : (IsFortress() ? 0.25f : (Projectiles::of(projectile).hitscan ? 0.12f : 0.22f));
            animFrame = 0;
            animTimer = 0.0f;
            state = ActionState::Attack;

            float muzzleX, muzzleY;
            GetMuzzlePoint(muzzleX, muzzleY);
            if (delegates.spawnProjectile)
            {
                delegates.spawnProjectile(*this, muzzleX, muzzleY, dstX, dstY, target->id, map);
            }
            if (burstCount > 1)
            {
                burstShotsRemaining = burstCount - 1;
                burstTimer = burstDelay;
            }
        }
    }
}

} // namespace OpenArtOfWar
