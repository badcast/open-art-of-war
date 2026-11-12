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
    if (replanCooldown > 0.0f)
        replanCooldown -= deltaTime;
    blockedTileX = tileX;
    blockedTileY = tileY;
    if (secondaryCooldownTimer > 0.0f)
    {
        secondaryCooldownTimer -= deltaTime;
    }
    if (autoSiegeCooldownTimer > 0.0f)
    {
        autoSiegeCooldownTimer = std::max(0.0f, autoSiegeCooldownTimer - deltaTime);
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
            const UnitConfig *cfg = delegates.getUnitConfig ? delegates.getUnitConfig(newArch) : nullptr;
            isSiegeMode = cfg ? cfg->isSiegeMode : (newArch >= 14 && newArch <= 16);
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

            if (cfg)
            {
                const int oldMax = std::max(1, maxHp);
                const int oldHp = currentHp;
                maxHp = cfg->maxHp;
                currentHp = std::clamp(static_cast<int>(static_cast<std::int64_t>(oldHp) * maxHp / oldMax), 1, maxHp);
                attackDamage = cfg->attackDamage;
                attackRange = cfg->attackRange;
                attackCooldown = cfg->attackCooldown;
                attackCooldownMax = cfg->attackCooldownMax;
                moveSpeed = cfg->moveSpeed;
                sightRadius = cfg->sightRadius;
                burstCount = cfg->burstCount;
                burstDelay = cfg->burstDelay;
                splashPercent = cfg->splashPercent;
                bulletSpeed = cfg->bulletSpeed;
                spreadAngle = cfg->spreadAngle;
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
                wreckType = cfg->wreckType;
                wreckComposite = cfg->wreckComposite;
                wreckStages = cfg->wreckStages;
                deathSequences = cfg->deathSequences;
                flameDeathSequences = cfg->flameDeathSequences;
                hasSecondary = false;
                if (cfg->weapons.size() > 1 && cfg->weapons[1].enabled && cfg->weapons[1].damage > 0)
                {
                    const WeaponConfig &w = cfg->weapons[1];
                    hasSecondary = true;
                    secondaryProjectile = w.projectile;
                    secondaryDamage = w.damage;
                    secondaryRange = w.range;
                    secondaryMinRange = w.minRange;
                    secondaryCooldown = std::max(0.1f, w.cooldown);
                    secondarySplash = w.splashPercent;
                    secondaryMuzzleX = w.muzzleOffsetX;
                    secondaryMuzzleY = w.muzzleOffsetY;
                }
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
        // The road is walked and no better one is being looked for: the order is over, also when the road ended
        // short of the destination (a goal inside a building footprint is moved to its edge by the pathfinder).
        // An attack-move left without a destination would mark the unit as busy for ever (the computer never
        // gathers it for a wave again)
        isAttackMoving = false;
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

} // namespace OpenArtOfWar
