#include "game/Unit.hpp"
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

static float ComputeDistanceToEntityPixels(const Unit &attacker, const Entity &target)
{
    float srcX = attacker.GetUnitCenterPixelX();
    float srcY = attacker.GetUnitCenterPixelY();

    if (target.isBuilding)
    {
        const Building *b = target.AsBuilding();
        int w = b ? std::max(1, b->buildWidth) : 1;
        int h = b ? std::max(1, b->buildHeight) : 1;

        float minCenterX = target.tileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
        float maxCenterX = (target.tileX + w - 1) * TILE_WIDTH + (TILE_WIDTH * 0.5f);
        float minCenterY = target.tileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
        float maxCenterY = (target.tileY + h - 1) * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);

        float closestX = std::clamp(srcX, minCenterX, maxCenterX);
        float closestY = std::clamp(srcY, minCenterY, maxCenterY);
        return DetMath::Hypot(closestX - srcX, closestY - srcY);
    }

    float dstX = target.GetUnitCenterPixelX();
    float dstY = target.GetUnitCenterPixelY();
    return DetMath::Hypot(dstX - srcX, dstY - srcY);
}

Unit::Unit()
    : Entity(false)
{
    // Set as default stats
    maxHp = 100;
    currentHp = 100;
    sightRadius = 6;
    moveSpeed = 1.8f;
    attackDamage = 15;
    attackRange = 3.5f;
    attackCooldown = 1.0f;
    burstCount = 1;
    burstDelay = 0.1f;
}

void Unit::GetMuzzlePoint(float &outX, float &outY) const
{
    float cx = GetUnitCenterPixelX();
    float cy = GetUnitCenterPixelY();

    const float *s_dirDx = Utility::kDir8X;
    const float *s_dirDy = Utility::kDir8Y;

    if (hasRotatingTurret)
    {
        // Physical turret pivot center (elevated on tank hull dome)
        float tOx = 0.0f, tOy = -13.5f;
        if (hasCustomTurretOffset)
        {
            tOx = turretOffsetX;
            tOy = turretOffsetY;
        }
        else
        {
            tOx = 0.0f;
            tOy = 0.0f;
        }

        // The turret sits elsewhere on the hull for every hull facing: follow the sprite's turret layer
        float turretPivotX = cx + tOx + turretShiftX;
        float turretPivotY = cy + tOy + turretShiftY;

        int tf = (turretFacing % 8 + 8) % 8;
        outX = turretPivotX + s_dirDx[tf] * muzzleOffsetX - s_dirDy[tf] * muzzleOffsetY;
        outY = turretPivotY + s_dirDy[tf] * muzzleOffsetX + s_dirDx[tf] * muzzleOffsetY;
    }
    else
    {
        // Non-turret units (infantry, fixed vehicles): localePosition rotates with facingDirection
        int f = (facingDirection % 8 + 8) % 8;
        float pivotY = IsInfantry() ? (cy - 4.0f) : cy;
        outX = cx + s_dirDx[f] * muzzleOffsetX - s_dirDy[f] * muzzleOffsetY;
        outY = pivotY + s_dirDy[f] * muzzleOffsetX + s_dirDx[f] * muzzleOffsetY;
    }
}

void Unit::Update(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates)
{
    // A Fortress with the flamethrower upgrade that knows its target walks up to it (machine gun and flame together
    // work at the flame's reach): its range for chasing, firing and standing off is that short one meanwhile
    const float savedRange = attackRange;
    bool shortened = false;
    if (alive && IsFortress() && !isSiegeMode && moveSpeed > 0.0f && state != ActionState::Die
        && (targetEntityId != 0 || targetWallTileX >= 0) && delegates.isFortressFlamethrowerUnlocked
        && delegates.isFortressFlamethrowerUnlocked(owner) && attackRange > kFortressFlameApproach)
    {
        attackRange = kFortressFlameApproach;
        shortened = true;
    }
    UpdateImpl(deltaTime, map, pathfinder, delegates);
    if (shortened && attackRange == kFortressFlameApproach)
        attackRange = savedRange; // (a transformation inside the update sets its own range: left alone)
}

void Unit::UpdateImpl(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates)
{
    if (!alive)
        return;

    if (state == ActionState::Die)
    {
        deathTimer -= deltaTime;
        if (IsInfantry())
        {
            animTimer += deltaTime;
            if (animTimer >= 0.08f)
            {
                animTimer = 0.0f;
                if (animFrame < 25)
                {
                    animFrame++;
                }
            }
        }
        else
        {
            // Gradual burial into the ground over the burial time of effects.yaml
            const EffectsConfig &fx = EffectsConfig::Get();
            const float elapsed = std::clamp(fx.burialSeconds - deathTimer, 0.0f, fx.burialSeconds);
            const int stages = (wreckStages > 0) ? wreckStages : fx.WreckOfVehicle(archetypeId).stages;
            animFrame
                = std::min(stages - 1, static_cast<int>((elapsed / fx.burialSeconds) * static_cast<float>(stages)));
        }
        if (deathTimer <= -EffectsConfig::Get().fadeSeconds)
        {
            alive = false; // after the burial time and the fade
        }
        return;
    }

    if (cooldownTimer > 0.0f)
    {
        cooldownTimer -= deltaTime;
    }
    if (lastAttackedTimer > 0.0f)
    {
        lastAttackedTimer -= deltaTime;
        if (lastAttackedTimer <= 0.0f)
        {
            lastAttackedTimer = 0.0f;
            lastAttackerId = 0;
        }
    }
    // Queueing time: grows while the unit has somewhere to go but stays on the same tile
    if (HasMoveTarget() && targetWallTileX < 0 && targetEntityId == 0 && tileX == blockedTileX && tileY == blockedTileY)
        blockedTimer += deltaTime;
    else
        blockedTimer = 0.0f;
    if (tileX != blockedTileX || tileY != blockedTileY)
        blockRetries = 0; // it moved on: the next blockage gets its tries again
    if (blockRetryTimer > 0.0f)
        blockRetryTimer -= deltaTime;
    blockedTileX = tileX;
    blockedTileY = tileY;
    if (secondaryCooldownTimer > 0.0f)
    {
        secondaryCooldownTimer -= deltaTime;
    }

    UpdateBurst(deltaTime, map, delegates);

    // Muzzle flash / attack animation decay
    if (attackAnimTimer > 0.0f)
    {
        attackAnimTimer -= deltaTime;
        if (attackAnimTimer <= 0.0f && state == ActionState::Attack)
        {
            if (!path.empty() && pathIndex < path.size() && !isSiegeMode && moveSpeed > 0.0f && targetEntityId == 0)
            {
                state = ActionState::Move;
            }
            else
            {
                // Infantry keeps its crouched firing pose for as long as it has a target, instead of
                // standing up after every shot and crouching again for the next one
                bool hasTarget = (targetEntityId != 0 || targetWallTileX >= 0);
                if (!IsInfantry() || !hasTarget || isSiegeMode)
                {
                    state = ActionState::Idle;
                    animFrame = 0;
                }
            }
        }
    }

    // If pursuing a target beyond attack range, compute path (unless in Siege Mode)
    if (targetEntityId != 0 && state != ActionState::Move && !isSiegeMode && moveSpeed > 0.0f)
    {
        Entity *target = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;
        if (target && target->alive && target->state != ActionState::Die)
        {
            float dist = ComputeDistanceToEntityPixels(*this, *target);
            if (dist > attackRange * TILE_WIDTH && !waitingForPath)
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
        }
    }

    UpdateMovement(deltaTime, map, pathfinder, delegates);
    UpdateCombat(deltaTime, map, pathfinder, delegates);

    // Update animation frame timer
    if (state == ActionState::Move)
    {
        animTimer += deltaTime;
        if (animTimer >= moveFrameTime)
        {
            animTimer = 0.0f;
            animFrame++;
        }
    }
    else if (state == ActionState::Attack)
    {
        animTimer += deltaTime;
        float stepDelay
            = attackFrameTime > 0.0f ? attackFrameTime : ((archetypeId == 7 || archetypeId == 18) ? 0.04f : 0.07f);
        if (animTimer >= stepDelay)
        {
            animTimer = 0.0f;
            animFrame++;
        }
    }
    else if (state == ActionState::Transform)
    {
        UpdateTransformation(deltaTime, delegates);
    }
    else
    {
        animFrame = 0;
        animTimer = 0.0f;
    }

    // Sight of the unit's side moves along with it
    if (owner < kMaxPlayers)
    {
        map.RevealArea(tileX, tileY, sightRadius, Players::TeamOf(owner));
    }
}

void Unit::UpdateBurst(float deltaTime, TileMap &map, const UnitDelegates &delegates)
{
    if (burstShotsRemaining > 0)
    {
        burstTimer -= deltaTime;
        if (burstTimer <= 0.0f)
        {
            burstShotsRemaining--;
            burstTimer = burstDelay;
            // The cooldown counts from the last shot of the burst (ticks stretch a burst, so the nominal span is not
            // enough)
            if (burstShotsRemaining == 0)
                cooldownTimer = std::max(cooldownTimer, NextCooldown());
            // Every shot of a burst plays its own firing picture (the flash starts over with the rocket)
            if (state == ActionState::Idle || state == ActionState::Attack)
            {
                state = ActionState::Attack;
                animFrame = 0;
                animTimer = 0.0f;
            }
            attackAnimTimer = burstPoseTime;

            if (targetEntityId != 0)
            {
                Entity *target = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;
                if (target && target->alive && target->state != ActionState::Die)
                {
                    float srcX, srcY;
                    GetMuzzlePoint(srcX, srcY);
                    float dstX = target->GetUnitCenterPixelX();
                    float dstY = target->GetUnitCenterPixelY();
                    if (delegates.spawnProjectile)
                    {
                        delegates.spawnProjectile(*this, srcX, srcY, dstX, dstY, target->id, map);
                    }
                }
            }
            else if (targetWallTileX >= 0 && targetWallTileY >= 0)
            {
                if (map.GetWall(targetWallTileX, targetWallTileY) > 0)
                {
                    float srcX, srcY;
                    GetMuzzlePoint(srcX, srcY);
                    float dstX = targetWallTileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
                    float dstY = targetWallTileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
                    if (delegates.spawnProjectile)
                    {
                        delegates.spawnProjectile(*this, srcX, srcY, dstX, dstY, 0, map);
                    }
                }
            }
        }
    }
}

void Unit::UpdateTransformation(float deltaTime, const UnitDelegates &delegates)
{
    animTimer += deltaTime;
    if (animTimer >= 0.09f)
    {
        animTimer = 0.0f;
        animFrame++;

        int maxFrames = 5; // Default for Rhino (arch 10, 15: 5 steps, seq 115/116)
        if (archetypeId == 13 || archetypeId == 16)
        {
            maxFrames = 6; // Grad MLRS (arch 13, 16: 6 steps, seq 64/65)
        }
        else if (archetypeId == 4 || archetypeId == 14)
        {
            maxFrames = 2; // Sniper prone transition
        }

        if (animFrame >= maxFrames)
        {
            // Transformation complete! Apply new archetype & stats
            int newArch = pendingArchetypeId;
            archetypeId = newArch;
            isSiegeMode = (newArch >= 14 && newArch <= 16);
            state = ActionState::Idle;
            animFrame = 0;
            animTimer = 0.0f;
            pendingArchetypeId = -1;
            // Packed up with an order still open (attack-move): carry on toward it right away
            if (!isSiegeMode && HasMoveTarget())
            {
                waitingForPath = true;
                pathRetryTimer = 0.45f;
            }

            const UnitConfig *cfg = delegates.getUnitConfig ? delegates.getUnitConfig(newArch) : nullptr;
            if (cfg)
            {
                maxHp = cfg->maxHp;
                attackDamage = cfg->attackDamage;
                attackRange = cfg->attackRange;
                attackCooldown = cfg->attackCooldown;
                attackCooldownMax = cfg->attackCooldownMax;
                moveSpeed = cfg->moveSpeed;
                sightRadius = cfg->sightRadius;
                burstCount = cfg->burstCount;
                burstDelay = cfg->burstDelay;
                moveFrameTime = cfg->moveFrameTime;
                attackFrameTime = cfg->attackFrameTime;
                attackPoseTime = cfg->attackPoseTime;
                burstPoseTime = cfg->burstPoseTime;
                flashMode = cfg->flashMode;
                flashFrames = cfg->flashFrames;
                projectile = cfg->projectile;
                hasRotatingTurret = cfg->hasRotatingTurret;
                muzzleOffsetX = cfg->muzzleOffsetX;
                muzzleOffsetY = cfg->muzzleOffsetY;
                turretOffsetX = cfg->turretOffsetX;
                turretOffsetY = cfg->turretOffsetY;
                hasCustomTurretOffset = cfg->hasCustomTurretOffset;
                turnStepDelay = cfg->turnStepDelay;
                turretTurnStepDelay = cfg->turretTurnStepDelay;
                // The new form starts from its config: give it the technologies back
                if (delegates.applyResearch)
                    delegates.applyResearch(*this);
            }
            else
            {
                if (isSiegeMode)
                {
                    moveSpeed = 0.0f;
                    attackRange += 3.0f;
                    attackDamage = static_cast<int>(attackDamage * 1.6f);
                    if (newArch == 15)
                    {
                        burstCount = 2;
                        burstDelay = 0.1f;
                        attackCooldown = 0.9f;
                    }
                }
                else
                {
                    moveSpeed = (newArch == 4) ? 1.3f : 1.35f;
                    attackRange = std::max(5.5f, attackRange - 3.0f);
                    attackDamage = std::max(30, static_cast<int>(attackDamage / 1.6f));
                    burstCount = 1;
                }
            }
        }
    }
}

void Unit::FinalizeArrivalIfPathDone()
{
    if (pathIndex < path.size())
        return;
    state = ActionState::Idle;
    ClearPathState();
    if (!waitingForPath)
    {
        if (isAttackMoving && HasMoveTarget() && std::abs(tileX - targetTileX) <= 1
            && std::abs(tileY - targetTileY) <= 1)
        {
            isAttackMoving = false;
        }
        ClearMoveTarget();
    }
}

float Unit::AdvanceStep(float budget)
{
    // Slide from the origin tile toward the reserved tile. tileX/tileY flip only once the step is complete,
    // so the logical tile never runs ahead of (or lags behind) the sprite.
    float dx = static_cast<float>((resTileX - tileX) * TILE_WIDTH) - subX;
    float dy = static_cast<float>((resTileY - tileY) * TILE_HEIGHT) - subY;
    float dist = DetMath::Hypot(dx, dy);

    if (dist > budget)
    {
        subX += (dx / dist) * budget;
        subY += (dy / dist) * budget;
        return 0.0f;
    }

    tileX = resTileX;
    tileY = resTileY;
    subX = 0.0f;
    subY = 0.0f;
    ReleaseReservation();

    if (pathIndex < path.size() && path[pathIndex].x == tileX && path[pathIndex].y == tileY)
    {
        pathIndex++;
    }
    if (state == ActionState::Move)
    {
        FinalizeArrivalIfPathDone();
    }
    return budget - dist;
}

void Unit::UpdateMovement(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates)
{
    float budget = std::max(0.0f, moveSpeed) * std::clamp(paceScale, 0.1f, 1.0f) * (deltaTime * 60.0f);

    if (allyWaitTimer > 0.0f)
        allyWaitTimer = std::max(0.0f, allyWaitTimer - deltaTime);
    if (makeWayCooldown > 0.0f)
        makeWayCooldown = std::max(0.0f, makeWayCooldown - deltaTime);
    // Any order (or a target of its own) and the spot it stepped away from no longer matters
    if (yieldHomeX >= 0 && (HasMoveTarget() || targetEntityId != 0 || targetWallTileX >= 0 || isSiegeMode))
    {
        yieldHomeX = yieldHomeY = -1;
        yieldHomeTries = 0;
    }

    // A step that is under way is always finished: the unit owns both tiles until it arrives, so it never
    // snaps back to the origin tile and never gets parked between two tiles (whatever its state is now).
    if (HasReservation())
    {
        if (budget <= 0.0f)
        {
            // Cannot move at all any more: settle on whichever tile the sprite is closer to
            if (std::abs(subX) * 2.0f > TILE_WIDTH || std::abs(subY) * 2.0f > TILE_HEIGHT)
            {
                tileX = resTileX;
                tileY = resTileY;
            }
            subX = 0.0f;
            subY = 0.0f;
            ReleaseReservation();
        }
        else
        {
            if (targetEntityId == 0 && targetWallTileX < 0 && moveFireTargetId == 0)
            {
                if (hasRotatingTurret && archetypeId != 17)
                    StepTurretToward(facingDirection, deltaTime);
                else
                    turretFacing = facingDirection;
            }
            budget = AdvanceStep(budget);
            if (HasReservation())
                return;
        }
    }

    // Immobile, transforming, or Siege Mode units cannot move
    if (state == ActionState::Transform || isSiegeMode || moveSpeed <= 0.0f)
    {
        if (state == ActionState::Move)
        {
            state = ActionState::Idle;
        }
        ClearPathState();
        waitingForPath = false;
        return;
    }

    // Closing in on something to shoot: stop on the first tile from which it is in firing range, before
    // committing to the next step (a started step is always finished, so checking only in updateCombat
    // walked every unit a tile or more too close)
    if (state == ActionState::Move && !HasReservation())
    {
        bool inRange = false;
        if (targetEntityId != 0 && delegates.getEntity)
        {
            const Entity *t = delegates.getEntity(targetEntityId);
            inRange = t && t->alive && t->state != ActionState::Die
                      && ComputeDistanceToEntityPixels(*this, *t) <= attackRange * TILE_WIDTH
                      && (!delegates.isEntityVisible || delegates.isEntityVisible(*t, owner));
        }
        else if (targetWallTileX >= 0 && map.GetWall(targetWallTileX, targetWallTileY) > 0)
        {
            const float wx = targetWallTileX * TILE_WIDTH + TILE_WIDTH * 0.5f,
                        wy = targetWallTileY * TILE_HEIGHT + TILE_HEIGHT * 0.5f;
            inRange
                = DetMath::Hypot(wx - GetUnitCenterPixelX(), wy - GetUnitCenterPixelY()) <= attackRange * TILE_WIDTH;
        }
        if (inRange)
        {
            ClearPathState();
            state = ActionState::Idle;
            animFrame = 0;
            waitingForPath = false;
            return;
        }
    }

    const int mapW = map.GetWidth();
    const int mapH = map.GetHeight();

    // Sanitize out-of-bounds target coordinates immediately (Invariant enforcement)
    if (targetTileX < 0 || targetTileX >= mapW || targetTileY < 0 || targetTileY >= mapH)
    {
        targetTileX = -1;
        targetTileY = -1;
    }

    // ------------------------------------------------------------------------
    // 1. Handle Idle / Path Exhausted / Waiting-For-Path Retry State
    // ------------------------------------------------------------------------
    if (state != ActionState::Move || path.empty() || pathIndex >= path.size())
    {
        if (state == ActionState::Move)
        {
            state = ActionState::Idle;
        }

        if (yieldHomeX >= 0 && state == ActionState::Idle && !waitingForPath)
        {
            UpdateYieldHome(deltaTime, pathfinder, delegates);
            if (state == ActionState::Move)
                return; // walks back from the next frame on
        }

        if (waitingForPath && state == ActionState::Idle)
        {
            pathRetryTimer += deltaTime;
            if (pathRetryTimer >= 0.45f)
            {
                pathRetryTimer = 0.0f;

                int goalX = -1;
                int goalY = -1;

                // Priority 1: Dynamic entity pursuit
                if (targetEntityId != 0)
                {
                    const Entity *t = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;
                    if (t && t->alive && t->state != ActionState::Die)
                    {
                        goalX = t->tileX;
                        goalY = t->tileY;
                    }
                    else
                    {
                        targetEntityId = 0;
                    }
                }

                // Priority 2: Explicit ground destination (ONLY if valid >= 0)
                if (goalX < 0 || goalY < 0)
                {
                    if (HasMoveTarget())
                    {
                        goalX = targetTileX;
                        goalY = targetTileY;
                    }
                }

                // Validate goal and check if we still need to travel
                if (goalX >= 0 && goalX < mapW && goalY >= 0 && goalY < mapH
                    && (std::abs(tileX - goalX) > 0 || std::abs(tileY - goalY) > 0))
                {
                    std::vector<PathNode> newPath;
                    bool isPartial = false;
                    if (FindPathThroughAllies(goalX, goalY, pathfinder, newPath, &isPartial) && !newPath.empty())
                    {
                        int curDistSq = (tileX - goalX) * (tileX - goalX) + (tileY - goalY) * (tileY - goalY);
                        int endDistSq = (newPath.back().x - goalX) * (newPath.back().x - goalX)
                                        + (newPath.back().y - goalY) * (newPath.back().y - goalY);
                        if (!isPartial || endDistSq < curDistSq)
                        {
                            path = std::move(newPath);
                            pathIndex = 0;
                            state = ActionState::Move;
                            waitingForPath = isPartial;
                            waitBlockedTimer = 0.0f;
                        }
                    }
                }
                else
                {
                    // No valid goal or already arrived: clear waiting state to prevent (0,0) ghost marches
                    waitingForPath = false;
                    if (HasMoveTarget() && std::abs(tileX - targetTileX) <= 1 && std::abs(tileY - targetTileY) <= 1)
                    {
                        ClearMoveTarget();
                        isAttackMoving = false;
                    }
                }
            }
        }
        return;
    }

    // ------------------------------------------------------------------------
    // 2. Choke-Point Interactions (only ever evaluated while standing exactly on a tile)
    // ------------------------------------------------------------------------

    // Helper: Try an instant 1-tile local sidestep around a friendly unit occupying (blockedX, blockedY)
    auto tryLocalSidestep = [&](int blockedX, int blockedY) -> bool {
        int nextGoalX
            = (pathIndex + 1 < path.size()) ? path[pathIndex + 1].x : (HasMoveTarget() ? targetTileX : blockedX);
        int nextGoalY
            = (pathIndex + 1 < path.size()) ? path[pathIndex + 1].y : (HasMoveTarget() ? targetTileY : blockedY);

        int bestSx = -1, bestSy = -1;
        int bestScore = 999999;

        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0)
                    continue;

                int sx = tileX + dx;
                int sy = tileY + dy;
                if (sx == blockedX && sy == blockedY)
                    continue;
                if (sx < 0 || sx >= mapW || sy < 0 || sy >= mapH)
                    continue;
                if (!map.IsPassable(sx, sy) || map.GetWall(sx, sy) > 0)
                    continue;
                if (delegates.isTileOccupied && delegates.isTileOccupied(sx, sy, id))
                    continue;

                // Sidestep must be close to the original blocked node (Chebyshev <= 1) and progress toward nextGoal
                if (std::max(std::abs(sx - blockedX), std::abs(sy - blockedY)) > 1)
                    continue;

                int dGoalSq = (sx - nextGoalX) * (sx - nextGoalX) + (sy - nextGoalY) * (sy - nextGoalY);
                int curGoalSq = (tileX - nextGoalX) * (tileX - nextGoalX) + (tileY - nextGoalY) * (tileY - nextGoalY);
                if (!IsInfantry())
                {
                    // Vehicles turn in place before a step: a sidestep that needs more than a 90-degree turn
                    // (or does not even get closer) means turning around and back again - wait / re-plan instead
                    const int stepFacing = Utility::Dir8FromVector(static_cast<float>(dx), static_cast<float>(dy));
                    if (Utility::Dir8Distance(stepFacing, facingDirection) > 2 || dGoalSq >= curGoalSq)
                        continue;
                }
                if (dGoalSq <= curGoalSq + 1)
                {
                    if (dGoalSq < bestScore)
                    {
                        bestScore = dGoalSq;
                        bestSx = sx;
                        bestSy = sy;
                    }
                }
            }
        }

        if (bestSx != -1)
        {
            path[pathIndex] = {bestSx, bestSy};
            waitBlockedTimer = 0.0f;
            return true;
        }
        return false;
    };

    // Helper: Re-plan from the current tile to the current goal, optionally walking around one tile
    auto replanFromHere = [&](int avoidX, int avoidY) -> bool {
        int goalX = HasMoveTarget() ? targetTileX : path.back().x;
        int goalY = HasMoveTarget() ? targetTileY : path.back().y;
        if (targetEntityId != 0 && delegates.getEntity)
        {
            if (const Entity *t = delegates.getEntity(targetEntityId); t && t->alive && t->state != ActionState::Die)
            {
                goalX = t->tileX;
                goalY = t->tileY;
            }
        }
        if (goalX < 0 || goalY < 0)
            return false;

        // Moving allies are not locked by syncMap, so explicitly route around the one we bumped into
        if (avoidX == goalX && avoidY == goalY)
            avoidX = avoidY = -1;

        std::vector<PathNode> altPath;
        bool isPartial = false;
        if (FindPathThroughAllies(goalX, goalY, pathfinder, altPath, &isPartial, avoidX, avoidY) && !altPath.empty()
            && (!delegates.isTileOccupied || !delegates.isTileOccupied(altPath[0].x, altPath[0].y, id)))
        {
            int curDistSq = (tileX - goalX) * (tileX - goalX) + (tileY - goalY) * (tileY - goalY);
            int endDistSq = (altPath.back().x - goalX) * (altPath.back().x - goalX)
                            + (altPath.back().y - goalY) * (altPath.back().y - goalY);
            if (!isPartial || endDistSq < curDistSq)
            {
                path = std::move(altPath);
                pathIndex = 0;
                waitingForPath = isPartial;
                return true;
            }
        }
        return false;
    };

    // Helper: Passage completely sealed -> enter Idle and schedule a retry only if the goal is valid
    auto parkAndRetry = [&]() {
        bool hasGoal = HasMoveTarget() || targetEntityId != 0;
        state = ActionState::Idle;
        ClearPathState();
        waitingForPath = hasGoal;
        pathRetryTimer = 0.0f;
    };

    // Helper lambda to handle choke-point collisions (Breach & Clear + Dynamic Sidestepping).
    // Returns true when movement is over for this frame, false when path[pathIndex] was replaced and can be retried.
    auto handleTileBlockage = [&](int blockedX, int blockedY) -> bool {
        // A. Destructible Wall: breach ONLY if hostile, and only when there is no way around it
        //    (otherwise one wall would stop the whole column while an open route exists)
        if (map.GetWall(blockedX, blockedY) > 0 && Players::Hostile(map.GetWallOwner(blockedX, blockedY), owner)
            && CanBreachWalls())
        {
            int goalX = HasMoveTarget() ? targetTileX : path.back().x;
            int goalY = HasMoveTarget() ? targetTileY : path.back().y;
            if (!ShouldBreachWalls(goalX, goalY, pathfinder))
            {
                if (replanFromHere(blockedX, blockedY))
                    return false;
                parkAndRetry();
                return true;
            }
            targetWallTileX = blockedX;
            targetWallTileY = blockedY;
            state = ActionState::Idle;
            ClearPathState();
            waitingForPath = false;
            return true;
        }

        int blockerId = delegates.getUnitAtTile ? delegates.getUnitAtTile(blockedX, blockedY, id) : 0;
        Entity *blocker = (blockerId > 0 && delegates.getEntity) ? delegates.getEntity(blockerId) : nullptr;

        if (blocker && blocker->alive && blocker->state != ActionState::Die)
        {
            // B. Hostile Entity Blocking Choke Point -> "Breach & Clear"
            if (Players::Hostile(blocker->owner, owner))
            {
                targetEntityId = blockerId;
                targetWallTileX = -1;
                targetWallTileY = -1;
                state = ActionState::Idle; // updateCombat will fire immediately on this frame
                ClearPathState();
                waitingForPath = false;
                return true;
            }

            // C. Friendly Unit Occupation -> NO single-file column waiting!
            Unit *friendlyUnit = blocker->AsUnit();
            const bool allyLeaving
                = friendlyUnit && (friendlyUnit->state == ActionState::Move || friendlyUnit->HasReservation());
            //    1) If blocked tile is destination (or adjacent to destination), arrived at squad cluster!
            //       (not while a make-way chain is moving: the ally in front is about to free the tile)
            if (targetEntityId == 0 && !(allyWaitTimer > 0.0f && allyLeaving)
                && (pathIndex + 1 >= path.size()
                    || (HasMoveTarget()
                        && std::max(std::abs(tileX - targetTileX), std::abs(tileY - targetTileY)) <= 1)))
            {
                state = ActionState::Idle;
                ClearPathState();
                if (!waitingForPath)
                {
                    ClearMoveTarget();
                    isAttackMoving = false;
                }
                return true;
            }

            //    2) Ask the ally to make way: it steps aside, together with the allies behind it when it has
            //       no free tile of its own (a packed gap in a wall). Head-on in a narrow passage, the one
            //       that passes first (Unit::passesBefore) moves the other one off the road.
            const bool headOn = friendlyUnit && friendlyUnit->state == ActionState::Move
                                && !friendlyUnit->HasReservation()
                                && friendlyUnit->pathIndex < friendlyUnit->path.size()
                                && friendlyUnit->path[friendlyUnit->pathIndex].x == tileX
                                && friendlyUnit->path[friendlyUnit->pathIndex].y == tileY;
            const bool standing = friendlyUnit && friendlyUnit->CanMakeWayFor(*this);
            auto askToMakeWay = [&]() -> bool {
                if (makeWayCooldown > 0.0f)
                    return false;
                makeWayCooldown = 0.5f;
                if (!RequestMakeWay(*friendlyUnit, map, delegates))
                    return false;
                allyWaitTimer = 2.5f; // vehicles turn on the spot first: give the chain time
                waitBlockedTimer = 0.0f;
                return true;
            };
            //       A standing ally in a passage (both tiles beside it across our way are closed), or one we
            //       have been stuck behind for a while, is asked first: walking around it only leads in circles
            if (standing)
            {
                auto closed = [&](int x, int y) {
                    return x < 0 || y < 0 || x >= mapW || y >= mapH || !map.IsPassable(x, y) || map.GetWall(x, y) > 0;
                };
                const int sx = blockedX - tileX, sy = blockedY - tileY;
                const bool inPassage = closed(blockedX - sy, blockedY + sx) && closed(blockedX + sy, blockedY - sx);
                if ((inPassage || blockedTimer >= 1.5f) && askToMakeWay())
                    return true;
            }

            //    3) Try instant 1-tile local sidestep into an open adjacent lane!
            //       (not in single-file flow mode: the column follows one lane through a chokepoint)
            if (!flowMode && tryLocalSidestep(blockedX, blockedY))
            {
                return false; // Path updated in-place; continue moving on this frame!
            }

            if (friendlyUnit)
            {
                if ((standing || (headOn && PassesBefore(*friendlyUnit))) && askToMakeWay())
                    return true;
                // Brief micro-yield while the friendly unit is actively walking out of our way;
                // in flow mode simply queue behind it instead of re-pathing around. Longer while a chain
                // makes way, and head-on for the one that gives way (the other one moves it aside).
                float yieldLimit = flowMode ? 2.0f : 0.18f;
                if (headOn && !PassesBefore(*friendlyUnit))
                    yieldLimit = std::max(yieldLimit, 1.0f);
                if (allyLeaving && (waitBlockedTimer < yieldLimit || allyWaitTimer > 0.0f))
                {
                    waitBlockedTimer += deltaTime;
                    return true;
                }
                // The ally is still standing there, but it was asked to go a moment ago
                if (allyWaitTimer > 0.0f && makeWayCooldown > 0.0f)
                {
                    waitBlockedTimer += deltaTime;
                    return true;
                }
            }
        }

        // D0. A crowd of friends shuts the tile for the moment: ask the same road again a few times (a short pause
        //     each) before planning a way around, which can lead the unit in a big circle
        if (blocker && blocker->alive && blocker->state != ActionState::Die && !Players::Hostile(blocker->owner, owner)
            && blockRetries < kBlockRetries && targetEntityId == 0) // (a unit closing in on a target keeps looking for its firing spot)
        {
            if (blockRetryTimer <= 0.0f)
            {
                ++blockRetries;
                blockRetryTimer = kBlockRetryPause;
            }
            return true;
        }

        // D. Full Bypass Re-pathing around the blocked tile
        waitBlockedTimer = 0.0f;
        if (replanFromHere(blockedX, blockedY))
        {
            return false;
        }

        // E. Passage completely sealed
        parkAndRetry();
        return true;
    };

    // ------------------------------------------------------------------------
    // 3. Step Loop: start a new tile step, rotate the hull, then slide with the remaining budget
    // ------------------------------------------------------------------------
    for (int guard = 0; guard < 6 && budget > 0.0f; ++guard)
    {
        if (state != ActionState::Move)
            return;
        if (pathIndex >= path.size())
        {
            FinalizeArrivalIfPathDone();
            return;
        }

        const PathNode node = path[pathIndex];

        // Skip redundant waypoint equal to the tile we stand on
        if (node.x == tileX && node.y == tileY)
        {
            pathIndex++;
            FinalizeArrivalIfPathDone();
            continue;
        }

        // The path was planned from another tile (e.g. order issued mid-step): plan again from here
        if (std::max(std::abs(node.x - tileX), std::abs(node.y - tileY)) != 1)
        {
            if (!replanFromHere(-1, -1))
            {
                parkAndRetry();
                return;
            }
            continue;
        }

        bool nextTileBlocked = node.x < 0 || node.x >= mapW || node.y < 0 || node.y >= mapH
                               || !map.IsPassable(node.x, node.y) || map.GetWall(node.x, node.y) > 0
                               || (delegates.isTileOccupied && delegates.isTileOccupied(node.x, node.y, id))
                               || (delegates.getUnitAtTile && delegates.getUnitAtTile(node.x, node.y, id) != 0);
        if (nextTileBlocked)
        {
            if (handleTileBlockage(node.x, node.y))
            {
                return;
            }
            continue;
        }

        int stepX = node.x - tileX;
        int stepY = node.y - tileY;
        int desiredFacing = Utility::Dir8FromVector(static_cast<float>(stepX), static_cast<float>(stepY));

        if (!IsInfantry() && facingDirection != desiredFacing && turnStepDelay > 0.0f)
        {
            // Vehicles turn in place on the tile centre before committing to the step

            turnTimer += deltaTime;
            while (turnTimer >= turnStepDelay && facingDirection != desiredFacing)
            {
                turnTimer -= turnStepDelay;
                facingDirection = (facingDirection + Utility::TurnStep8(facingDirection, desiredFacing) + 8) % 8;
            }

            if (targetEntityId == 0 && targetWallTileX < 0 && moveFireTargetId == 0)
            {
                if (hasRotatingTurret && archetypeId != 17)
                    StepTurretToward(facingDirection, deltaTime);
                else
                    turretFacing = facingDirection;
            }
            if (archetypeId == 17)
            {
                turretFacing = facingDirection;
            }
            return;
        }

        facingDirection = desiredFacing;
        turnTimer = 0.0f;
        if (targetEntityId == 0 && targetWallTileX < 0 && moveFireTargetId == 0)
        {
            if (hasRotatingTurret && archetypeId != 17)
                StepTurretToward(facingDirection, deltaTime);
            else
                turretFacing = facingDirection;
        }
        if (archetypeId == 17)
        {
            turretFacing = facingDirection;
        }

        // Commit: reserve the tile we are entering, then slide into it
        waitBlockedTimer = 0.0f;
        resTileX = node.x;
        resTileY = node.y;
        budget = AdvanceStep(budget);
        if (HasReservation())
            return;
    }
}

bool Unit::StepTurretToward(int targetDir8, float deltaTime)
{
    targetDir8 = (targetDir8 % 8 + 8) % 8;
    turretFacing = (turretFacing % 8 + 8) % 8;
    if (turretFacing == targetDir8)
    {
        turretTurnTimer = 0.0f;
        return true;
    }
    const float delay = (turretTurnStepDelay > 0.0f) ? turretTurnStepDelay : turnStepDelay;
    if (IsInfantry() || delay <= 0.0f)
    {
        turretFacing = targetDir8;
        turretTurnTimer = 0.0f;
        return true;
    }
    turretTurnTimer += deltaTime;
    while (turretTurnTimer >= delay && turretFacing != targetDir8)
    {
        turretTurnTimer -= delay;
        int delta = Utility::TurnStep8(turretFacing, targetDir8);
        turretFacing = (turretFacing + delta + 8) % 8;
    }
    if (turretFacing == targetDir8)
    {
        turretTurnTimer = 0.0f;
        return true;
    }
    return false;
}

void Unit::FireOnTheMove(float deltaTime, TileMap &map, const UnitDelegates &delegates)
{
    // Keep shooting at the same enemy while it stays in range, otherwise pick the next one in range
    Entity *target = (moveFireTargetId != 0 && delegates.getEntity) ? delegates.getEntity(moveFireTargetId) : nullptr;
    if (target
        && (target->state == ActionState::Die
            || ComputeDistanceToEntityPixels(*this, *target) > attackRange * TILE_WIDTH
            || (delegates.isEntityVisible && !delegates.isEntityVisible(*target, owner))))
    {
        target = nullptr;
    }
    if (!target)
    {
        moveFireTargetId = delegates.findEnemyInAttackRange ? delegates.findEnemyInAttackRange(*this) : 0;
        target = (moveFireTargetId != 0 && delegates.getEntity) ? delegates.getEntity(moveFireTargetId) : nullptr;
    }
    if (!target)
    {
        moveFireTargetId = 0;
        StepTurretToward(facingDirection, deltaTime);
        return;
    }

    float sx = GetUnitCenterPixelX(), sy = GetUnitCenterPixelY();
    float dstX = target->GetUnitCenterPixelX(), dstY = target->GetUnitCenterPixelY();
    int targetDir8 = Utility::Dir8FromVector(dstX - sx, dstY - sy);
    if (!StepTurretToward(targetDir8, deltaTime))
        return;

    if (cooldownTimer > 0.0f)
        return;
    cooldownTimer = NextCooldown();
    attackAnimTimer = Projectiles::of(projectile).hitscan ? 0.12f : 0.22f; // state stays Move: the hull keeps driving
    float mx = 0.0f, my = 0.0f;
    GetMuzzlePoint(mx, my);
    if (delegates.spawnProjectile)
        delegates.spawnProjectile(*this, mx, my, dstX, dstY, target->id, map);
}

bool Unit::HasOpenRouteTo(int goalX, int goalY, Pathfinder &pathfinder, int *outRouteLen) const
{
    if (outRouteLen)
        *outRouteLen = 0;
    if (goalX < 0 || goalY < 0)
        return false;
    if (goalX == tileX && goalY == tileY)
        return true;
    std::vector<PathNode> route;
    bool partial = false;
    pathfinder.SetIgnoreUnitsOnce();
    bool found = pathfinder.FindPath(tileX, tileY, goalX, goalY, route, id, &partial);
    if (outRouteLen)
        *outRouteLen = static_cast<int>(route.size());
    return found && !partial && !route.empty();
}

// ---------------------------------------------------------------------------
// Making way: allies let each other through narrow passages
// ---------------------------------------------------------------------------

int Unit::MovePriority() const
{
    int p = 0;
    if (groupOrder || isAttackMoving)
        p += 2;
    if (HasMoveTarget() || targetEntityId != 0)
        p += 1;
    return p;
}

bool Unit::PassesBefore(const Unit &other) const
{
    const int a = MovePriority(), b = other.MovePriority();
    return a != b ? a > b : id < other.id;
}

bool Unit::CanMakeWayFor(const Unit &requester) const
{
    if (!alive || state == ActionState::Die || state == ActionState::Transform || state == ActionState::Attack
        || state == ActionState::Move)
        return false;
    if (id == requester.id || moveSpeed <= 0.0f || isSiegeMode || isDirectEngaged || HasReservation()
        || targetWallTileX >= 0)
        return false;
    if (Players::Hostile(owner, requester.owner))
        return false;
    const bool ownGoal = HasMoveTarget() || targetEntityId != 0;
    return !ownGoal || requester.PassesBefore(*this);
}

void Unit::MakeWayStep(int x, int y)
{
    const bool ownGoal = HasMoveTarget() || targetEntityId != 0;
    if (!ownGoal && yieldHomeX < 0)
    {
        yieldHomeX = tileX;
        yieldHomeY = tileY;
        yieldHomeTries = 0;
    }
    yieldHomeTimer = 3.0f;
    path = {{x, y}};
    pathIndex = 0;
    state = ActionState::Move;
    waitBlockedTimer = 0.0f;
    allyWaitTimer = 2.5f;
    // A unit with somewhere to go plans its own route again once it stands aside
    waitingForPath = ownGoal;
    pathRetryTimer = 0.0f;
}

bool Unit::RequestMakeWay(Unit &blocker, const TileMap &map, const UnitDelegates &delegates)
{
    if (!delegates.getUnitAtTile || !delegates.getEntity)
        return false;
    // Tuning: how far the chain may reach, how many tiles the search looks at, how long a chain may be
    constexpr int kRadius = 6;
    constexpr int kMaxNodes = 96;
    constexpr int kMaxChain = 6;

    const int mapW = map.GetWidth(), mapH = map.GetHeight();
    auto open = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < mapW && y < mapH && map.IsPassable(x, y) && map.GetWall(x, y) == 0;
    };

    // Our road ahead: the chain moves off it, not along it (where it would be in the way again)
    auto roadCost = [&](int x, int y) {
        for (size_t i = pathIndex; i < path.size() && i < pathIndex + 6; ++i)
        {
            if (path[i].x == x && path[i].y == y)
                return i == pathIndex + 1 ? 20 : 8;
        }
        return 0;
    };

    struct Node
    {
        int x, y, parent, cost, depth;
        bool done;
    };
    std::vector<Node> nodes;
    nodes.reserve(kMaxNodes);
    nodes.push_back({blocker.tileX, blocker.tileY, -1, 0, 0, false});
    auto known = [&](int x, int y) {
        for (const Node &n : nodes)
        {
            if (n.x == x && n.y == y)
                return true;
        }
        return false;
    };

    // Dijkstra over a handful of tiles (a linear scan for the cheapest open node keeps it deterministic)
    int found = -1;
    for (;;)
    {
        int cur = -1;
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
        {
            if (!nodes[i].done && (cur < 0 || nodes[i].cost < nodes[cur].cost))
                cur = i;
        }
        if (cur < 0)
            break;
        nodes[cur].done = true;
        if (cur > 0)
        {
            const int occupant = delegates.getUnitAtTile(nodes[cur].x, nodes[cur].y, 0);
            if (occupant == 0)
            {
                found = cur;
                break;
            }
        }
        if (nodes[cur].depth >= kMaxChain)
            continue;
        const int cx = nodes[cur].x, cy = nodes[cur].y;
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0)
                    continue;
                const int nx = cx + dx, ny = cy + dy;
                if (!open(nx, ny) || (nx == tileX && ny == tileY) || known(nx, ny))
                    continue;
                if (std::max(std::abs(nx - blocker.tileX), std::abs(ny - blocker.tileY)) > kRadius)
                    continue;
                const int occupant = delegates.getUnitAtTile(nx, ny, 0);
                if (occupant != 0)
                {
                    // Through an ally only when it can step on as well
                    const Entity *e = delegates.getEntity(occupant);
                    const Unit *u = e ? e->AsUnit() : nullptr;
                    if (!u || u->tileX != nx || u->tileY != ny || !u->CanMakeWayFor(*this))
                        continue;
                }
                if (static_cast<int>(nodes.size()) >= kMaxNodes)
                    continue;
                const int cost = nodes[cur].cost + ((dx != 0 && dy != 0) ? 3 : 2) + roadCost(nx, ny);
                nodes.push_back({nx, ny, cur, cost, nodes[cur].depth + 1, false});
            }
        }
    }
    if (found < 0)
        return false;

    // Shift the chain: every unit steps into the tile of the next one, the last one into the free tile
    std::vector<int> chain; // node indices from the free tile back to the blocker
    for (int i = found; i >= 0; i = nodes[i].parent)
        chain.push_back(i);
    for (size_t k = 1; k < chain.size(); ++k)
    {
        const Node &from = nodes[chain[k]];
        const Node &to = nodes[chain[k - 1]];
        Unit *u = &blocker;
        if (chain[k] != 0)
        {
            Entity *e = delegates.getEntity(delegates.getUnitAtTile(from.x, from.y, 0));
            u = e ? e->AsUnit() : nullptr;
        }
        if (u)
            u->MakeWayStep(to.x, to.y);
    }
    return true;
}

bool Unit::FindPathThroughAllies(
    int goalX,
    int goalY,
    Pathfinder &pathfinder,
    std::vector<PathNode> &outPath,
    bool *outPartial,
    int avoidX,
    int avoidY) const
{
    bool partial = false;
    if (avoidX >= 0 && avoidY >= 0)
        pathfinder.SetAvoidTiles({{avoidX, avoidY}});
    const bool found = pathfinder.FindPath(tileX, tileY, goalX, goalY, outPath, id, &partial);
    if (outPartial)
        *outPartial = partial;
    // Same rule as for walls: a detour longer than twice the straight way plus 10 tiles is not a way around
    const int straight = std::max(std::abs(goalX - tileX), std::abs(goalY - tileY));
    if (found && !partial && static_cast<int>(outPath.size()) <= straight * 2 + 10)
        return true;

    // Allies standing in a passage are no wall: plan through them, they make way when we get there.
    // Allies that cannot step aside (deployed, firing, immobile, busy with a goal that goes first) stay obstacles.
    std::vector<PathNode> avoid;
    if (avoidX >= 0 && avoidY >= 0)
        avoid.push_back({avoidX, avoidY});
    if (const EntityManager *entities = pathfinder.GetEntityManager())
    {
        for (const auto &e : entities->GetAllEntities())
        {
            const Unit *u = e.AsUnit();
            if (!u || u->id == id || !u->alive || u->state == ActionState::Die || u->state == ActionState::Move
                || Players::Hostile(u->owner, owner))
                continue;
            if (!u->CanMakeWayFor(*this))
                avoid.push_back({u->tileX, u->tileY});
        }
    }
    std::vector<PathNode> through;
    bool throughPartial = false;
    pathfinder.SetAvoidTiles(std::move(avoid));
    pathfinder.SetIgnoreUnitsOnce();
    if (pathfinder.FindPath(tileX, tileY, goalX, goalY, through, id, &throughPartial) && !through.empty()
        && !throughPartial && (!found || partial || through.size() < outPath.size()))
    {
        outPath = std::move(through);
        if (outPartial)
            *outPartial = false;
        return true;
    }
    return found;
}

void Unit::UpdateYieldHome(float deltaTime, Pathfinder &pathfinder, const UnitDelegates &delegates)
{
    if (HasMoveTarget() || targetEntityId != 0 || !path.empty())
        return;
    yieldHomeTimer -= deltaTime;
    if (yieldHomeTimer > 0.0f)
        return;
    // Tuning: give up after a few tries or when it ended up far away
    constexpr int kMaxTries = 20; // every 2 s: a long column may need a while to pass
    constexpr int kMaxDistance = 8;
    if ((tileX == yieldHomeX && tileY == yieldHomeY) || ++yieldHomeTries > kMaxTries
        || std::max(std::abs(tileX - yieldHomeX), std::abs(tileY - yieldHomeY)) > kMaxDistance)
    {
        yieldHomeX = yieldHomeY = -1;
        yieldHomeTries = 0;
        return;
    }
    yieldHomeTimer = 2.0f;
    if (delegates.isTileOccupied && delegates.isTileOccupied(yieldHomeX, yieldHomeY, id))
        return; // still taken: try again later
    std::vector<PathNode> back;
    bool partial = false;
    if (pathfinder.FindPath(tileX, tileY, yieldHomeX, yieldHomeY, back, id, &partial) && !back.empty() && !partial
        && back.size() <= static_cast<size_t>(kMaxDistance + 4))
    {
        path = std::move(back);
        pathIndex = 0;
        state = ActionState::Move;
    }
}

void Unit::ApproachTile(const Entity &target, const TileMap &map, int &outX, int &outY) const
{
    outX = target.tileX;
    outY = target.tileY;
    if (target.isBuilding)
        return;
    const float tx = target.GetUnitCenterPixelX(), ty = target.GetUnitCenterPixelY();
    const float dx = GetUnitCenterPixelX() - tx, dy = GetUnitCenterPixelY() - ty;
    const float len = DetMath::Hypot(dx, dy);
    const float stand = std::max(0.0f, attackRange - 0.6f) * TILE_WIDTH;
    if (len <= stand || len <= 0.0f)
        return;
    const int cx = static_cast<int>((tx + dx / len * stand) / TILE_WIDTH);
    const int cy = static_cast<int>((ty + dy / len * stand) / TILE_HEIGHT);
    // Around that point: the open tile farthest from the target whose centre is still in range (ties: the
    // one nearest to the unit), so the unit stands at the edge of its range, not a tile inside it
    const float range = attackRange * TILE_WIDTH;
    float bestTargetDist = -1.0f, bestUnitDist = 0.0f;
    for (int y = cy - 1; y <= cy + 1; ++y)
    {
        for (int x = cx - 1; x <= cx + 1; ++x)
        {
            if (x < 0 || y < 0 || x >= map.GetWidth() || y >= map.GetHeight() || !map.IsPassable(x, y)
                || map.GetWall(x, y) > 0)
                continue;
            const float px = x * TILE_WIDTH + TILE_WIDTH * 0.5f, py = y * TILE_HEIGHT + TILE_HEIGHT * 0.5f;
            const float dTarget = DetMath::Hypot(px - tx, py - ty);
            if (dTarget > range)
                continue;
            const float dUnit = DetMath::Hypot(px - GetUnitCenterPixelX(), py - GetUnitCenterPixelY());
            if (dTarget > bestTargetDist + 0.01f
                || (std::abs(dTarget - bestTargetDist) <= 0.01f && dUnit < bestUnitDist))
            {
                bestTargetDist = dTarget;
                bestUnitDist = dUnit;
                outX = x;
                outY = y;
            }
        }
    }
}

bool Unit::FindPathToAttack(const Entity &target, Pathfinder &pathfinder, bool *outPartial)
{
    int gx = target.tileX, gy = target.tileY;
    if (const TileMap *map = pathfinder.GetMap())
        ApproachTile(target, *map, gx, gy);
    if (pathfinder.FindPath(tileX, tileY, gx, gy, path, id, outPartial))
        return true;
    return (gx != target.tileX || gy != target.tileY)
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
    const int straight = std::max(std::abs(goalX - tileX), std::abs(goalY - tileY));
    if (routeLen > straight * kDetourFactor + kDetourSlack)
        return true;
    // (not right at the goal: there the unit only waits for allies to make room; and only a unit standing
    // at the wall itself - one queueing further back is held up by its allies, not by the wall)
    if (blockedTimer < kQueueSeconds || straight <= 3)
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
    int x0 = target.tileX, y0 = target.tileY, x1 = target.tileX, y1 = target.tileY;
    if (const Building *b = target.AsBuilding())
    {
        x1 = b->tileX + b->buildWidth - 1;
        y1 = b->tileY + b->buildHeight - 1;
    }
    auto touches = [&](int x, int y) {
        return x >= x0 - 1 && x <= x1 + 1 && y >= y0 - 1 && y <= y1 + 1;
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
    const int cx = static_cast<int>(candidate.GetUnitCenterPixelX() / TILE_WIDTH);
    const int cy = static_cast<int>(candidate.GetUnitCenterPixelY() / TILE_HEIGHT);
    return std::max(std::abs(cx - stanceHomeX), std::abs(cy - stanceHomeY)) <= StanceLeashTiles();
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
    const int away = std::max(std::abs(tileX - stanceHomeX), std::abs(tileY - stanceHomeY));
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
    const bool tooFar = targetEntityId != 0 && away > StanceLeashTiles();
    const bool chaseOver = settled && away > 1;
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

    // 1. If attacking an auto-acquired wall, prioritize any enemy unit entering firing range
    if (targetWallTileX >= 0 && !isManualWallTarget)
    {
        if (delegates.findEnemyInAttackRange)
        {
            int enemyInRange = delegates.findEnemyInAttackRange(*this);
            if (enemyInRange != 0)
            {
                targetWallTileX = -1;
                targetWallTileY = -1;
                targetEntityId = enemyInRange;
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
            ClearPathState();
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
                int tdx = std::abs(targetWallTileX - tileX);
                int tdy = std::abs(targetWallTileY - tileY);
                bool isClose = (tdx <= 1 && tdy <= 1);
                bool hasUpgrade
                    = delegates.isFortressFlamethrowerUnlocked && delegates.isFortressFlamethrowerUnlocked(owner);
                if (isClose && hasUpgrade)
                {
                    projectile = ProjectileType::Flame;
                    if (delegates.spawnProjectile)
                    {
                        float mSrcX, mSrcY;
                        GetMuzzlePoint(mSrcX, mSrcY);
                        ProjectileType origProj = projectile;
                        projectile = ProjectileType::Bullet;
                        delegates.spawnProjectile(*this, mSrcX, mSrcY, dstX, dstY, 0, map);
                        projectile = origProj;
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

            float mSrcX, mSrcY;
            GetMuzzlePoint(mSrcX, mSrcY);
            if (delegates.spawnProjectile)
            {
                delegates.spawnProjectile(*this, mSrcX, mSrcY, dstX, dstY, 0, map);
            }
            if (burstCount > 1)
            {
                burstShotsRemaining = burstCount - 1;
                burstTimer = burstDelay;
            }
        }
        return;
    }

    // 3. Auto-acquire blocking wall if path is blocked and no enemy is in sight.
    //    A partial path is often just congestion (allies standing in the way): only breach when
    //    the goal is unreachable over terrain and walls, checked at most once per second.
    if (wallCheckCooldown > 0.0f)
        wallCheckCooldown -= deltaTime;
    //    Only a unit the wall actually hinders shoots at it: one whose goal is walled off, whose way around
    //    is a long detour, or that has been queueing in front of a gap for a while. Units that can walk
    //    on through an open route leave the wall alone.
    const bool stuckWithGoal = (state == ActionState::Idle && path.empty() && waitingForPath)
                               || (HasMoveTarget() && blockedTimer >= 3.0f && !isSiegeMode && moveSpeed > 0.0f);
    // On auto-attack, a hostile wall beside the way is shot down with a 50% chance (decided once per unit and wall,
    // deterministically). Low priority: this only runs without a target, and any enemy in sight drops the wall.
    if (isAttackMoving && HasMoveTarget() && !isSiegeMode && moveSpeed > 0.0f && attackDamage > 0 && targetEntityId == 0
        && targetWallTileX < 0 && wallCheckCooldown <= 0.0f)
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
                const uint32_t h = static_cast<uint32_t>(id) * 73856093u ^ static_cast<uint32_t>(x) * 19349663u
                                   ^ static_cast<uint32_t>(y) * 83492791u;
                if (((h >> 7) & 1u) == 0u)
                    continue; // this one is left alone
                const int d = std::abs(x - tileX) + std::abs(y - tileY);
                if (d < bestDist)
                {
                    bestDist = d;
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
    if (targetEntityId == 0 && targetWallTileX < 0 && stuckWithGoal && HasMoveTarget() && CanBreachWalls()
        && wallCheckCooldown <= 0.0f)
    {
        wallCheckCooldown = 1.0f;
        const bool wallsInTheWay = ShouldBreachWalls(targetTileX, targetTileY, pathfinder);
        if (wallsInTheWay && delegates.findWallTarget)
        {
            int wx = -1, wy = -1;
            delegates.findWallTarget(*this, wx, wy);
            if (wx >= 0)
            {
                targetWallTileX = wx;
                targetWallTileY = wy;
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
    }

    // A plain move (group or not) got here only for the breach check above: it picks no other fights
    if (state == ActionState::Move && !isAttackMoving && targetEntityId == 0 && targetWallTileX < 0)
    {
        isDirectEngaged = false;
        return;
    }

    // 3b. Retaliation: a unit shot at from beyond its reach goes after the shooter. Not while holding
    //     position, deployed (siege), immobile, breaching a wall, or fighting something it can hit already.
    //     A plain move order never gets here (see the early return above), so explicit moves are kept.
    if (lastAttackedTimer > 0.0f && lastAttackerId != 0 && lastAttackerId != targetEntityId
        && !(state == ActionState::Move && !isAttackMoving) && !HoldsGround() && !isSiegeMode && moveSpeed > 0.0f
        && targetWallTileX < 0 && attackDamage > 0 && state != ActionState::Transform
        && (targetEntityId == 0 || targetEntityId != orderedTargetId))
    {
        Entity *attacker = delegates.getEntity ? delegates.getEntity(lastAttackerId) : nullptr;
        if (attacker && attacker->alive && attacker->state != ActionState::Die
            && Players::Hostile(attacker->owner, owner))
        {
            bool busyInRange = false;
            if (targetEntityId != 0)
            {
                const Entity *current = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;
                busyInRange = current && current->alive && current->state != ActionState::Die
                              && ComputeDistanceToEntityPixels(*this, *current) <= attackRange * TILE_WIDTH;
            }
            if (!busyInRange)
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

    // 4. Auto-acquire Enemy Target & Support Assist (Depth = 1)
    if (targetEntityId == 0
        && (state == ActionState::Idle || isAttackMoving || waitingForPath || !Players::Slot(owner).human))
    {
        if (delegates.findTarget)
        {
            int bestTargetId = delegates.findTarget(*this);
            if (bestTargetId != 0)
            {
                Entity *cand = delegates.getEntity ? delegates.getEntity(bestTargetId) : nullptr;
                bool validForUnit = (cand != nullptr);
                if (validForUnit && (isSiegeMode || moveSpeed <= 0.0f || HoldsGround()))
                {
                    // Siege mode and holding units ONLY acquire targets strictly within attackRange
                    float d = ComputeDistanceToEntityPixels(*this, *cand);
                    validForUnit = (d <= attackRange * TILE_WIDTH);
                    if (!validForUnit && HoldsGround() && delegates.findEnemyInAttackRange)
                    {
                        bestTargetId = delegates.findEnemyInAttackRange(*this);
                        cand = bestTargetId != 0 && delegates.getEntity ? delegates.getEntity(bestTargetId) : nullptr;
                        validForUnit = cand != nullptr;
                    }
                }
                if (validForUnit && !StanceAllowsTarget(*cand))
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
                    Entity *cand = delegates.getEntity ? delegates.getEntity(nextTargetId) : nullptr;
                    if (cand && StanceAllowsTarget(*cand)
                        && (!(isSiegeMode || HoldsGround())
                            || ComputeDistanceToEntityPixels(*this, *cand) <= attackRange * TILE_WIDTH))
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
            const int betterId = delegates.findEnemyInAttackRange(*this);
            const Entity *better = betterId != 0 && delegates.getEntity ? delegates.getEntity(betterId) : nullptr;
            if (better && betterId != targetEntityId && TargetPriorityTier(*better) < TargetPriorityTier(*target))
            {
                targetEntityId = betterId;
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
            const bool reachedLastSeen
                = !target->isBuilding && std::abs(tileX - lastSeenTargetX) <= 1 && std::abs(tileY - lastSeenTargetY) <= 1;
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
                const int gx = target->isBuilding ? target->tileX : lastSeenTargetX;
                const int gy = target->isBuilding ? target->tileY : lastSeenTargetY;
                if (pathfinder.FindPath(tileX, tileY, gx, gy, path, id, &isPartial))
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
        if (hasSecondary && secondaryCooldownTimer <= 0.0f && delegates.spawnProjectile
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
            float mSrcX, mSrcY;
            GetMuzzlePoint(mSrcX, mSrcY);
            delegates.spawnProjectile(*this, mSrcX, mSrcY, dstX, dstY, target->id, map);
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

            // Stuck in front of the target (a base walled in all around): shoot a way through the wall
            if (state != ActionState::Move && path.empty() && CanBreachWalls() && !isManualWallTarget
                && wallCheckCooldown <= 0.0f && delegates.findWallTarget)
            {
                wallCheckCooldown = 1.0f;
                if (!HasOpenRouteToEntity(*target, pathfinder))
                {
                    int wx = -1, wy = -1;
                    delegates.findWallTarget(*this, wx, wy);
                    if (wx >= 0)
                    {
                        targetEntityId = 0;
                        isSupportAssisting = false;
                        targetWallTileX = wx;
                        targetWallTileY = wy;
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
                int tdx = std::abs(target->tileX - tileX);
                int tdy = std::abs(target->tileY - tileY);
                bool isClose = (tdx <= 1 && tdy <= 1);
                bool hasUpgrade
                    = delegates.isFortressFlamethrowerUnlocked && delegates.isFortressFlamethrowerUnlocked(owner);
                if (isClose && hasUpgrade)
                {
                    projectile = ProjectileType::Flame;
                    if (delegates.spawnProjectile)
                    {
                        float mSrcX, mSrcY;
                        GetMuzzlePoint(mSrcX, mSrcY);
                        ProjectileType origProj = projectile;
                        projectile = ProjectileType::Bullet;
                        delegates.spawnProjectile(*this, mSrcX, mSrcY, dstX, dstY, target->id, map);
                        projectile = origProj;
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

            float mSrcX, mSrcY;
            GetMuzzlePoint(mSrcX, mSrcY);
            if (delegates.spawnProjectile)
            {
                delegates.spawnProjectile(*this, mSrcX, mSrcY, dstX, dstY, target->id, map);
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
