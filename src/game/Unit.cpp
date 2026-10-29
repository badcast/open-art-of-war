#include "game/Unit.hpp"
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

    static float computeDistanceToEntityPixels(const Unit &attacker, const Entity &target)
    {
        float srcX = attacker.getUnitCenterPixelX();
        float srcY = attacker.getUnitCenterPixelY();

        if(target.isBuilding)
        {
            const Building *b = target.asBuilding();
            int w = b ? std::max(1, b->buildWidth) : 1;
            int h = b ? std::max(1, b->buildHeight) : 1;

            float minCenterX = target.tileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
            float maxCenterX = (target.tileX + w - 1) * TILE_WIDTH + (TILE_WIDTH * 0.5f);
            float minCenterY = target.tileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
            float maxCenterY = (target.tileY + h - 1) * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);

            float closestX = std::clamp(srcX, minCenterX, maxCenterX);
            float closestY = std::clamp(srcY, minCenterY, maxCenterY);
            return DetMath::hypot(closestX - srcX, closestY - srcY);
        }

        float dstX = target.getUnitCenterPixelX();
        float dstY = target.getUnitCenterPixelY();
        return DetMath::hypot(dstX - srcX, dstY - srcY);
    }

    Unit::Unit() : Entity(false)
    {
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

    void Unit::getMuzzlePoint(float &outX, float &outY) const
    {
        float cx = getUnitCenterPixelX();
        float cy = getUnitCenterPixelY();

        const float *s_dirDx = Utility::kDir8X;
        const float *s_dirDy = Utility::kDir8Y;

        if(hasRotatingTurret)
        {
            // Physical turret pivot center (elevated on tank hull dome)
            float tOx = 0.0f, tOy = -13.5f;
            if(hasCustomTurretOffset)
            {
                tOx = turretOffsetX;
                tOy = turretOffsetY;
            }
            else
            {
                switch(archetypeId)
                {
                    case 4:
                        tOx = 0.0f;
                        tOy = -11.0f;
                        break; // Coyote
                    case 6:
                        tOx = -2.0f;
                        tOy = -14.0f;
                        break; // Assault Tank
                    case 7:
                    case 18:
                        tOx = 0.0f;
                        tOy = -14.0f;
                        break; // Fortress
                    case 9:
                        tOx = -2.0f;
                        tOy = -14.0f;
                        break; // Hammer
                    case 10:
                        tOx = -2.0f;
                        tOy = -14.0f;
                        break; // Heavy Tank
                    case 11:
                    case 17:
                        tOx = 0.0f;
                        tOy = -14.0f;
                        break; // Zeus
                    case 15:
                        tOx = -2.0f;
                        tOy = -14.0f;
                        break; // Rhino Siege
                    case 16:
                        tOx = -2.0f;
                        tOy = -15.0f;
                        break; // Grad Deployed
                    default:
                        tOx = 0.0f;
                        tOy = -13.5f;
                        break;
                }
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
            float pivotY = isInfantry() ? (cy - 4.0f) : cy;
            outX = cx + s_dirDx[f] * muzzleOffsetX - s_dirDy[f] * muzzleOffsetY;
            outY = pivotY + s_dirDy[f] * muzzleOffsetX + s_dirDx[f] * muzzleOffsetY;
        }
    }

    void Unit::update(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates)
    {
        if(!alive)
            return;

        if(state == ActionState::Die)
        {
            deathTimer -= deltaTime;
            if(isInfantry())
            {
                animTimer += deltaTime;
                if(animTimer >= 0.08f)
                {
                    animTimer = 0.0f;
                    if(animFrame < 25)
                    {
                        animFrame++;
                    }
                }
            }
            else
            {
                // Gradual burial into the ground over the burial time of effects.yaml
                const EffectsConfig &fx = EffectsConfig::get();
                const float elapsed = std::clamp(fx.burialSeconds - deathTimer, 0.0f, fx.burialSeconds);
                const int stages = (wreckStages > 0) ? wreckStages : fx.wreckOfVehicle(archetypeId).stages;
                animFrame = std::min(stages - 1, static_cast<int>((elapsed / fx.burialSeconds) * static_cast<float>(stages)));
            }
            if(deathTimer <= 0.0f)
            {
                alive = false;
            }
            return;
        }

        if(cooldownTimer > 0.0f)
        {
            cooldownTimer -= deltaTime;
        }
        if(secondaryCooldownTimer > 0.0f)
        {
            secondaryCooldownTimer -= deltaTime;
        }

        updateBurst(deltaTime, map, delegates);

        // Muzzle flash / attack animation decay
        if(attackAnimTimer > 0.0f)
        {
            attackAnimTimer -= deltaTime;
            if(attackAnimTimer <= 0.0f && state == ActionState::Attack)
            {
                if(!path.empty() && pathIndex < path.size() && !isSiegeMode && moveSpeed > 0.0f && targetEntityId == 0)
                {
                    state = ActionState::Move;
                }
                else
                {
                    // Infantry keeps its crouched firing pose for as long as it has a target, instead of
                    // standing up after every shot and crouching again for the next one
                    bool hasTarget = (targetEntityId != 0 || targetWallTileX >= 0);
                    if(!isInfantry() || !hasTarget || isSiegeMode)
                    {
                        state = ActionState::Idle;
                        animFrame = 0;
                    }
                }
            }
        }

        // If pursuing a target beyond attack range, compute path (unless in Siege Mode)
        if(targetEntityId != 0 && state != ActionState::Move && !isSiegeMode && moveSpeed > 0.0f)
        {
            Entity *target = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;
            if(target && target->alive && target->state != ActionState::Die)
            {
                float dist = computeDistanceToEntityPixels(*this, *target);
                if(dist > attackRange * TILE_WIDTH && !waitingForPath)
                {
                    bool isPartial = false;
                    if(pathfinder.findPath(tileX, tileY, target->tileX, target->tileY, path, id, &isPartial))
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

        updateMovement(deltaTime, map, pathfinder, delegates);
        updateCombat(deltaTime, map, pathfinder, delegates);

        // Update animation frame timer
        if(state == ActionState::Move)
        {
            animTimer += deltaTime;
            if(animTimer >= 0.09f)
            {
                animTimer = 0.0f;
                animFrame++;
            }
        }
        else if(state == ActionState::Attack)
        {
            animTimer += deltaTime;
            float stepDelay = (archetypeId == 7 || archetypeId == 18) ? 0.04f : 0.07f;
            if(animTimer >= stepDelay)
            {
                animTimer = 0.0f;
                animFrame++;
            }
        }
        else if(state == ActionState::Transform)
        {
            updateTransformation(deltaTime, delegates);
        }
        else
        {
            animFrame = 0;
            animTimer = 0.0f;
        }

        // Sight of the unit's side moves along with it
        if(owner < kMaxPlayers)
        {
            map.revealArea(tileX, tileY, sightRadius, Players::teamOf(owner));
        }
    }

    void Unit::updateBurst(float deltaTime, TileMap &map, const UnitDelegates &delegates)
    {
        if(burstShotsRemaining > 0)
        {
            burstTimer -= deltaTime;
            if(burstTimer <= 0.0f)
            {
                burstShotsRemaining--;
                burstTimer = burstDelay;
                state = ActionState::Attack;
                attackAnimTimer = 0.15f;

                if(targetEntityId != 0)
                {
                    Entity *target = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;
                    if(target && target->alive && target->state != ActionState::Die)
                    {
                        float srcX, srcY;
                        getMuzzlePoint(srcX, srcY);
                        float dstX = target->getUnitCenterPixelX();
                        float dstY = target->getUnitCenterPixelY();
                        if(delegates.spawnProjectile)
                        {
                            delegates.spawnProjectile(*this, srcX, srcY, dstX, dstY, target->id, map);
                        }
                    }
                }
                else if(targetWallTileX >= 0 && targetWallTileY >= 0)
                {
                    if(map.getWall(targetWallTileX, targetWallTileY) > 0)
                    {
                        float srcX, srcY;
                        getMuzzlePoint(srcX, srcY);
                        float dstX = targetWallTileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
                        float dstY = targetWallTileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
                        if(delegates.spawnProjectile)
                        {
                            delegates.spawnProjectile(*this, srcX, srcY, dstX, dstY, 0, map);
                        }
                    }
                }
            }
        }
    }

    void Unit::updateTransformation(float deltaTime, const UnitDelegates &delegates)
    {
        animTimer += deltaTime;
        if(animTimer >= 0.09f)
        {
            animTimer = 0.0f;
            animFrame++;

            int maxFrames = 5; // Default for Rhino (arch 10, 15: 5 steps, seq 115/116)
            if(archetypeId == 13 || archetypeId == 16)
            {
                maxFrames = 6; // Grad MLRS (arch 13, 16: 6 steps, seq 64/65)
            }
            else if(archetypeId == 4 || archetypeId == 14)
            {
                maxFrames = 2; // Sniper prone transition
            }

            if(animFrame >= maxFrames)
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
                if(!isSiegeMode && hasMoveTarget())
                {
                    waitingForPath = true;
                    pathRetryTimer = 0.45f;
                }

                const UnitConfig *cfg = delegates.getUnitConfig ? delegates.getUnitConfig(newArch) : nullptr;
                if(cfg)
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
                    projectile = cfg->projectile;
                    hasRotatingTurret = cfg->hasRotatingTurret;
                    muzzleOffsetX = cfg->muzzleOffsetX;
                    muzzleOffsetY = cfg->muzzleOffsetY;
                    turretOffsetX = cfg->turretOffsetX;
                    turretOffsetY = cfg->turretOffsetY;
                    hasCustomTurretOffset = cfg->hasCustomTurretOffset;
                    // The new form starts from its config: give it the technologies back
                    if(delegates.applyResearch)
                        delegates.applyResearch(*this);
                }
                else
                {
                    if(isSiegeMode)
                    {
                        moveSpeed = 0.0f;
                        attackRange += 3.0f;
                        attackDamage = static_cast<int>(attackDamage * 1.6f);
                        if(newArch == 15)
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

    void Unit::finalizeArrivalIfPathDone()
    {
        if(pathIndex < path.size())
            return;
        state = ActionState::Idle;
        clearPathState();
        if(!waitingForPath)
        {
            if(isAttackMoving && hasMoveTarget() && std::abs(tileX - targetTileX) <= 1 && std::abs(tileY - targetTileY) <= 1)
            {
                isAttackMoving = false;
            }
            clearMoveTarget();
        }
    }

    float Unit::advanceStep(float budget)
    {
        // Slide from the origin tile toward the reserved tile. tileX/tileY flip only once the step is complete,
        // so the logical tile never runs ahead of (or lags behind) the sprite.
        float dx = static_cast<float>((resTileX - tileX) * TILE_WIDTH) - subX;
        float dy = static_cast<float>((resTileY - tileY) * TILE_HEIGHT) - subY;
        float dist = DetMath::hypot(dx, dy);

        if(dist > budget)
        {
            subX += (dx / dist) * budget;
            subY += (dy / dist) * budget;
            return 0.0f;
        }

        tileX = resTileX;
        tileY = resTileY;
        subX = 0.0f;
        subY = 0.0f;
        releaseReservation();

        if(pathIndex < path.size() && path[pathIndex].x == tileX && path[pathIndex].y == tileY)
        {
            pathIndex++;
        }
        if(state == ActionState::Move)
        {
            finalizeArrivalIfPathDone();
        }
        return budget - dist;
    }

    void Unit::updateMovement(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates)
    {
        float budget = std::max(0.0f, moveSpeed) * std::clamp(paceScale, 0.1f, 1.0f) * (deltaTime * 60.0f);

        // A step that is under way is always finished: the unit owns both tiles until it arrives, so it never
        // snaps back to the origin tile and never gets parked between two tiles (whatever its state is now).
        if(hasReservation())
        {
            if(budget <= 0.0f)
            {
                // Cannot move at all any more: settle on whichever tile the sprite is closer to
                if(std::abs(subX) * 2.0f > TILE_WIDTH || std::abs(subY) * 2.0f > TILE_HEIGHT)
                {
                    tileX = resTileX;
                    tileY = resTileY;
                }
                subX = 0.0f;
                subY = 0.0f;
                releaseReservation();
            }
            else
            {
                budget = advanceStep(budget);
                if(hasReservation())
                    return;
            }
        }

        // Immobile, transforming, or Siege Mode units cannot move
        if(state == ActionState::Transform || isSiegeMode || moveSpeed <= 0.0f)
        {
            if(state == ActionState::Move)
            {
                state = ActionState::Idle;
            }
            clearPathState();
            waitingForPath = false;
            return;
        }

        const int mapW = map.getWidth();
        const int mapH = map.getHeight();

        // Sanitize out-of-bounds target coordinates immediately (Invariant enforcement)
        if(targetTileX < 0 || targetTileX >= mapW || targetTileY < 0 || targetTileY >= mapH)
        {
            targetTileX = -1;
            targetTileY = -1;
        }

        // ------------------------------------------------------------------------
        // 1. Handle Idle / Path Exhausted / Waiting-For-Path Retry State
        // ------------------------------------------------------------------------
        if(state != ActionState::Move || path.empty() || pathIndex >= path.size())
        {
            if(state == ActionState::Move)
            {
                state = ActionState::Idle;
            }

            if(waitingForPath && state == ActionState::Idle)
            {
                pathRetryTimer += deltaTime;
                if(pathRetryTimer >= 0.45f)
                {
                    pathRetryTimer = 0.0f;

                    int goalX = -1;
                    int goalY = -1;

                    // Priority 1: Dynamic entity pursuit
                    if(targetEntityId != 0)
                    {
                        const Entity *t = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;
                        if(t && t->alive && t->state != ActionState::Die)
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
                    if(goalX < 0 || goalY < 0)
                    {
                        if(hasMoveTarget())
                        {
                            goalX = targetTileX;
                            goalY = targetTileY;
                        }
                    }

                    // Validate goal and check if we still need to travel
                    if(goalX >= 0 && goalX < mapW && goalY >= 0 && goalY < mapH && (std::abs(tileX - goalX) > 0 || std::abs(tileY - goalY) > 0))
                    {
                        std::vector<PathNode> newPath;
                        bool isPartial = false;
                        if(pathfinder.findPath(tileX, tileY, goalX, goalY, newPath, id, &isPartial) && !newPath.empty())
                        {
                            int curDistSq = (tileX - goalX) * (tileX - goalX) + (tileY - goalY) * (tileY - goalY);
                            int endDistSq = (newPath.back().x - goalX) * (newPath.back().x - goalX) + (newPath.back().y - goalY) * (newPath.back().y - goalY);
                            if(!isPartial || endDistSq < curDistSq)
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
                        if(hasMoveTarget() && std::abs(tileX - targetTileX) <= 1 && std::abs(tileY - targetTileY) <= 1)
                        {
                            clearMoveTarget();
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
        auto tryLocalSidestep = [&](int blockedX, int blockedY) -> bool
        {
            int nextGoalX = (pathIndex + 1 < path.size()) ? path[pathIndex + 1].x : (hasMoveTarget() ? targetTileX : blockedX);
            int nextGoalY = (pathIndex + 1 < path.size()) ? path[pathIndex + 1].y : (hasMoveTarget() ? targetTileY : blockedY);

            int bestSx = -1, bestSy = -1;
            int bestScore = 999999;

            for(int dy = -1; dy <= 1; ++dy)
            {
                for(int dx = -1; dx <= 1; ++dx)
                {
                    if(dx == 0 && dy == 0)
                        continue;

                    int sx = tileX + dx;
                    int sy = tileY + dy;
                    if(sx == blockedX && sy == blockedY)
                        continue;
                    if(sx < 0 || sx >= mapW || sy < 0 || sy >= mapH)
                        continue;
                    if(!map.isPassable(sx, sy) || map.getWall(sx, sy) > 0)
                        continue;
                    if(delegates.isTileOccupied && delegates.isTileOccupied(sx, sy, id))
                        continue;

                    // Prevent diagonal corner-cutting past impassable terrain/walls
                    if(dx != 0 && dy != 0)
                    {
                        if(!map.isPassable(tileX + dx, tileY) || map.getWall(tileX + dx, tileY) > 0 || !map.isPassable(tileX, tileY + dy) || map.getWall(tileX, tileY + dy) > 0)
                        {
                            continue;
                        }
                    }

                    // Sidestep must be close to the original blocked node (Chebyshev <= 1) and progress toward nextGoal
                    if(std::max(std::abs(sx - blockedX), std::abs(sy - blockedY)) > 1)
                        continue;

                    int dGoalSq = (sx - nextGoalX) * (sx - nextGoalX) + (sy - nextGoalY) * (sy - nextGoalY);
                    int curGoalSq = (tileX - nextGoalX) * (tileX - nextGoalX) + (tileY - nextGoalY) * (tileY - nextGoalY);
                    if(!isInfantry())
                    {
                        // Vehicles turn in place before a step: a sidestep that needs more than a 90-degree turn
                        // (or does not even get closer) means turning around and back again - wait / re-plan instead
                        const int stepFacing = Utility::dir8FromVector(static_cast<float>(dx), static_cast<float>(dy));
                        if(Utility::dir8Distance(stepFacing, facingDirection) > 2 || dGoalSq >= curGoalSq)
                            continue;
                    }
                    if(dGoalSq <= curGoalSq + 1)
                    {
                        if(dGoalSq < bestScore)
                        {
                            bestScore = dGoalSq;
                            bestSx = sx;
                            bestSy = sy;
                        }
                    }
                }
            }

            if(bestSx != -1)
            {
                path[pathIndex] = {bestSx, bestSy};
                waitBlockedTimer = 0.0f;
                return true;
            }
            return false;
        };

        // Helper: Nudge an idle friendly unit out of a narrow passage if it is blocking our march
        auto tryNudgeIdleFriendly = [&](Unit *friendlyBlocker) -> bool
        {
            if(!friendlyBlocker || friendlyBlocker->state != ActionState::Idle)
                return false;
            if(friendlyBlocker->isSiegeMode || friendlyBlocker->moveSpeed <= 0.0f)
                return false;
            if(friendlyBlocker->targetEntityId != 0 || friendlyBlocker->isDirectEngaged)
                return false;
            if(friendlyBlocker->hasReservation())
                return false; // still finishing its own step

            int fx = friendlyBlocker->tileX;
            int fy = friendlyBlocker->tileY;
            // The way we are going: the ally steps aside off it, not further along it (where it would block us again)
            const int dirX = fx - tileX;
            const int dirY = fy - tileY;
            const PathNode *after = pathIndex + 1 < path.size() ? &path[pathIndex + 1] : nullptr;

            int bestX = -1, bestY = -1, bestScore = 1 << 20;
            for(int dy = -1; dy <= 1; ++dy)
            {
                for(int dx = -1; dx <= 1; ++dx)
                {
                    if(dx == 0 && dy == 0)
                        continue;
                    int nx = fx + dx;
                    int ny = fy + dy;
                    if(nx == tileX && ny == tileY)
                        continue;
                    if(nx < 0 || nx >= mapW || ny < 0 || ny >= mapH)
                        continue;
                    if(!map.isPassable(nx, ny) || map.getWall(nx, ny) > 0)
                        continue;
                    if(delegates.isTileOccupied && delegates.isTileOccupied(nx, ny, friendlyBlocker->id))
                        continue;
                    if(delegates.getUnitAtTile && delegates.getUnitAtTile(nx, ny, friendlyBlocker->id) != 0)
                        continue;
                    // Sideways first, then back towards us, ahead last; never onto our next tile
                    int score = dx * dirX + dy * dirY;
                    score = score > 0 ? 4 * score : -score;
                    if(after && after->x == nx && after->y == ny)
                        score += 100;
                    if(dx != 0 && dy != 0)
                        score += 1; // a straight sidestep is quicker
                    if(score < bestScore)
                    {
                        bestScore = score;
                        bestX = nx;
                        bestY = ny;
                    }
                }
            }
            if(bestX < 0)
                return false;
            friendlyBlocker->path = {{bestX, bestY}};
            friendlyBlocker->pathIndex = 0;
            friendlyBlocker->state = ActionState::Move;
            return true;
        };

        // Helper: Re-plan from the current tile to the current goal, optionally walking around one tile
        auto replanFromHere = [&](int avoidX, int avoidY) -> bool
        {
            int goalX = hasMoveTarget() ? targetTileX : path.back().x;
            int goalY = hasMoveTarget() ? targetTileY : path.back().y;
            if(targetEntityId != 0 && delegates.getEntity)
            {
                if(const Entity *t = delegates.getEntity(targetEntityId); t && t->alive && t->state != ActionState::Die)
                {
                    goalX = t->tileX;
                    goalY = t->tileY;
                }
            }
            if(goalX < 0 || goalY < 0)
                return false;

            // Moving allies are not locked by syncMap, so explicitly route around the one we bumped into
            if(avoidX >= 0 && avoidY >= 0 && (avoidX != goalX || avoidY != goalY))
            {
                pathfinder.setAvoidTiles({{avoidX, avoidY}});
            }

            std::vector<PathNode> altPath;
            bool isPartial = false;
            if(pathfinder.findPath(tileX, tileY, goalX, goalY, altPath, id, &isPartial) && !altPath.empty() && (!delegates.isTileOccupied || !delegates.isTileOccupied(altPath[0].x, altPath[0].y, id)))
            {
                int curDistSq = (tileX - goalX) * (tileX - goalX) + (tileY - goalY) * (tileY - goalY);
                int endDistSq = (altPath.back().x - goalX) * (altPath.back().x - goalX) + (altPath.back().y - goalY) * (altPath.back().y - goalY);
                if(!isPartial || endDistSq < curDistSq)
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
        auto parkAndRetry = [&]()
        {
            bool hasGoal = hasMoveTarget() || targetEntityId != 0;
            state = ActionState::Idle;
            clearPathState();
            waitingForPath = hasGoal;
            pathRetryTimer = 0.0f;
        };

        // Helper lambda to handle choke-point collisions (Breach & Clear + Dynamic Sidestepping).
        // Returns true when movement is over for this frame, false when path[pathIndex] was replaced and can be retried.
        auto handleTileBlockage = [&](int blockedX, int blockedY) -> bool
        {
            // A. Destructible Wall: breach ONLY if hostile, and only when there is no way around it
            //    (otherwise one wall would stop the whole column while an open route exists)
            if(map.getWall(blockedX, blockedY) > 0 && Players::hostile(map.getWallOwner(blockedX, blockedY), owner) && (isAttackMoving || !Players::slot(owner).human))
            {
                int goalX = hasMoveTarget() ? targetTileX : path.back().x;
                int goalY = hasMoveTarget() ? targetTileY : path.back().y;
                if(hasOpenRouteTo(goalX, goalY, pathfinder))
                {
                    if(replanFromHere(blockedX, blockedY))
                        return false;
                    parkAndRetry();
                    return true;
                }
                targetWallTileX = blockedX;
                targetWallTileY = blockedY;
                state = ActionState::Idle;
                clearPathState();
                waitingForPath = false;
                return true;
            }

            int blockerId = delegates.getUnitAtTile ? delegates.getUnitAtTile(blockedX, blockedY, id) : 0;
            Entity *blocker = (blockerId > 0 && delegates.getEntity) ? delegates.getEntity(blockerId) : nullptr;

            if(blocker && blocker->alive && blocker->state != ActionState::Die)
            {
                // B. Hostile Entity Blocking Choke Point -> "Breach & Clear"
                if(Players::hostile(blocker->owner, owner))
                {
                    targetEntityId = blockerId;
                    targetWallTileX = -1;
                    targetWallTileY = -1;
                    state = ActionState::Idle; // updateCombat will fire immediately on this frame
                    clearPathState();
                    waitingForPath = false;
                    return true;
                }

                // C. Friendly Unit Occupation -> NO single-file column waiting!
                //    1) If blocked tile is destination (or adjacent to destination), arrived at squad cluster!
                if(targetEntityId == 0 && (pathIndex + 1 >= path.size() || (hasMoveTarget() && std::max(std::abs(tileX - targetTileX), std::abs(tileY - targetTileY)) <= 1)))
                {
                    state = ActionState::Idle;
                    clearPathState();
                    if(!waitingForPath)
                    {
                        clearMoveTarget();
                        isAttackMoving = false;
                    }
                    return true;
                }

                //    2) Try instant 1-tile local sidestep into an open adjacent lane!
                //       (not in single-file flow mode: the column follows one lane through a chokepoint)
                if(!flowMode && tryLocalSidestep(blockedX, blockedY))
                {
                    return false; // Path updated in-place; continue moving on this frame!
                }

                //    3) If in a narrow 1-tile bottleneck behind an idle ally, nudge the idle ally to step aside
                if(Unit *friendlyUnit = blocker->asUnit())
                {
                    if(tryNudgeIdleFriendly(friendlyUnit))
                    {
                        return true;
                    }
                    // Brief micro-yield while the friendly unit is actively walking out of our way;
                    // in flow mode simply queue behind it instead of re-pathing around
                    float yieldLimit = flowMode ? 2.0f : 0.18f;
                    if((friendlyUnit->state == ActionState::Move || friendlyUnit->hasReservation()) && waitBlockedTimer < yieldLimit)
                    {
                        waitBlockedTimer += deltaTime;
                        return true;
                    }
                }
            }

            // D. Full Bypass Re-pathing around the blocked tile
            waitBlockedTimer = 0.0f;
            if(replanFromHere(blockedX, blockedY))
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
        for(int guard = 0; guard < 6 && budget > 0.0f; ++guard)
        {
            if(state != ActionState::Move)
                return;
            if(pathIndex >= path.size())
            {
                finalizeArrivalIfPathDone();
                return;
            }

            const PathNode node = path[pathIndex];

            // Skip redundant waypoint equal to the tile we stand on
            if(node.x == tileX && node.y == tileY)
            {
                pathIndex++;
                finalizeArrivalIfPathDone();
                continue;
            }

            // The path was planned from another tile (e.g. order issued mid-step): plan again from here
            if(std::max(std::abs(node.x - tileX), std::abs(node.y - tileY)) != 1)
            {
                if(!replanFromHere(-1, -1))
                {
                    parkAndRetry();
                    return;
                }
                continue;
            }

            bool nextTileBlocked = node.x < 0 || node.x >= mapW || node.y < 0 || node.y >= mapH || !map.isPassable(node.x, node.y) || map.getWall(node.x, node.y) > 0 || (delegates.isTileOccupied && delegates.isTileOccupied(node.x, node.y, id)) ||
                (delegates.getUnitAtTile && delegates.getUnitAtTile(node.x, node.y, id) != 0);
            if(nextTileBlocked)
            {
                if(handleTileBlockage(node.x, node.y))
                {
                    return;
                }
                continue;
            }

            int stepX = node.x - tileX;
            int stepY = node.y - tileY;
            int desiredFacing = Utility::dir8FromVector(static_cast<float>(stepX), static_cast<float>(stepY));

            if(!isInfantry() && facingDirection != desiredFacing && turnStepDelay > 0.0f)
            {
                // Vehicles turn in place on the tile centre before committing to the step

                turnTimer += deltaTime;
                while(turnTimer >= turnStepDelay && facingDirection != desiredFacing)
                {
                    turnTimer -= turnStepDelay;
                    facingDirection = (facingDirection + Utility::turnStep8(facingDirection, desiredFacing) + 8) % 8;
                }

                if(targetEntityId == 0 && targetWallTileX < 0 && moveFireTargetId == 0)
                {
                    turretFacing = facingDirection;
                }
                if(archetypeId == 17)
                {
                    turretFacing = facingDirection;
                }
                return;
            }

            facingDirection = desiredFacing;
            turnTimer = 0.0f;
            if(targetEntityId == 0 && targetWallTileX < 0 && moveFireTargetId == 0)
            {
                turretFacing = facingDirection;
            }
            if(archetypeId == 17)
            {
                turretFacing = facingDirection;
            }

            // Commit: reserve the tile we are entering, then slide into it
            waitBlockedTimer = 0.0f;
            resTileX = node.x;
            resTileY = node.y;
            budget = advanceStep(budget);
            if(hasReservation())
                return;
        }
    }

    void Unit::fireOnTheMove(TileMap &map, const UnitDelegates &delegates)
    {
        // Keep shooting at the same enemy while it stays in range, otherwise pick the next one in range
        Entity *target = (moveFireTargetId != 0 && delegates.getEntity) ? delegates.getEntity(moveFireTargetId) : nullptr;
        if(target && (target->state == ActionState::Die || computeDistanceToEntityPixels(*this, *target) > attackRange * TILE_WIDTH || (delegates.isEntityVisible && !delegates.isEntityVisible(*target, owner))))
        {
            target = nullptr;
        }
        if(!target)
        {
            moveFireTargetId = delegates.findEnemyInAttackRange ? delegates.findEnemyInAttackRange(*this) : 0;
            target = (moveFireTargetId != 0 && delegates.getEntity) ? delegates.getEntity(moveFireTargetId) : nullptr;
        }
        if(!target)
        {
            moveFireTargetId = 0;
            return;
        }

        float sx = getUnitCenterPixelX(), sy = getUnitCenterPixelY();
        float dstX = target->getUnitCenterPixelX(), dstY = target->getUnitCenterPixelY();
        turretFacing = Utility::dir8FromVector(dstX - sx, dstY - sy);

        if(cooldownTimer > 0.0f)
            return;
        cooldownTimer = nextCooldown();
        attackAnimTimer = (projectile == ProjectileType::Bullet) ? 0.12f : 0.22f; // state stays Move: the hull keeps driving
        float mx = 0.0f, my = 0.0f;
        getMuzzlePoint(mx, my);
        if(delegates.spawnProjectile)
            delegates.spawnProjectile(*this, mx, my, dstX, dstY, target->id, map);
    }

    bool Unit::hasOpenRouteTo(int goalX, int goalY, Pathfinder &pathfinder) const
    {
        if(goalX < 0 || goalY < 0)
            return false;
        if(goalX == tileX && goalY == tileY)
            return true;
        std::vector<PathNode> route;
        bool partial = false;
        pathfinder.setIgnoreUnitsOnce();
        bool found = pathfinder.findPath(tileX, tileY, goalX, goalY, route, id, &partial);
        return found && !partial && !route.empty();
    }

    bool Unit::hasOpenRouteToEntity(const Entity &target, Pathfinder &pathfinder) const
    {
        int x0 = target.tileX, y0 = target.tileY, x1 = target.tileX, y1 = target.tileY;
        if(const Building *b = target.asBuilding())
        {
            x1 = b->tileX + b->buildWidth - 1;
            y1 = b->tileY + b->buildHeight - 1;
        }
        auto touches = [&](int x, int y) { return x >= x0 - 1 && x <= x1 + 1 && y >= y0 - 1 && y <= y1 + 1; };
        if(touches(tileX, tileY))
            return true;
        // A building fills its own tiles: the route counts as open when it ends right next to the footprint
        std::vector<PathNode> route;
        bool partial = false;
        pathfinder.setIgnoreUnitsOnce();
        if(!pathfinder.findPath(tileX, tileY, target.tileX, target.tileY, route, id, &partial) || route.empty())
            return false;
        return !partial || touches(route.back().x, route.back().y);
    }

    void Unit::updateCombat(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates)
    {
        if(state == ActionState::Transform || attackDamage <= 0 || state == ActionState::Die)
        {
            isDirectEngaged = false;
            return;
        }

        // Pure retreat / ground move order (!isAttackMoving && targetEntityId == 0 && targetWallTileX < 0)
        // must NOT be interrupted by auto-targeting:
        if(state == ActionState::Move && targetEntityId == 0 && targetWallTileX < 0 && hasRotatingTurret)
        {
            fireOnTheMove(map, delegates);
        }
        else
        {
            moveFireTargetId = 0;
        }
        if(state == ActionState::Move && !isAttackMoving && targetEntityId == 0 && targetWallTileX < 0)
        {
            isDirectEngaged = false;
            return;
        }

        // 1. If attacking an auto-acquired wall, prioritize any enemy unit entering firing range
        if(targetWallTileX >= 0 && !isManualWallTarget)
        {
            if(delegates.findEnemyInAttackRange)
            {
                int enemyInRange = delegates.findEnemyInAttackRange(*this);
                if(enemyInRange != 0)
                {
                    targetWallTileX = -1;
                    targetWallTileY = -1;
                    targetEntityId = enemyInRange;
                }
            }
        }

        // 2. Handle Wall Target Engagement
        if(targetWallTileX >= 0 && targetWallTileY >= 0)
        {
            if(map.getWall(targetWallTileX, targetWallTileY) == 0)
            {
                targetWallTileX = -1;
                targetWallTileY = -1;
                isManualWallTarget = false;
                isDirectEngaged = false;

                if(hasMoveTarget() && !isSiegeMode && moveSpeed > 0.0f)
                {
                    if(std::abs(tileX - targetTileX) > 1 || std::abs(tileY - targetTileY) > 1)
                    {
                        bool isPartial = false;
                        if(pathfinder.findPath(tileX, tileY, targetTileX, targetTileY, path, id, &isPartial))
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
                        clearMoveTarget();
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
            if(isSiegeMode && Players::slot(owner).human && !map.isVisibleTo(targetWallTileX, targetWallTileY, Players::teamOf(owner)))
            {
                targetWallTileX = -1;
                targetWallTileY = -1;
                isManualWallTarget = false;
                isDirectEngaged = false;
                if(state == ActionState::Attack && attackAnimTimer <= 0.0f)
                {
                    state = ActionState::Idle;
                    animFrame = 0;
                }
                return;
            }

            float srcX = getUnitCenterPixelX();
            float srcY = getUnitCenterPixelY();
            float dstX = targetWallTileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
            float dstY = targetWallTileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
            float dx = dstX - srcX;
            float dy = dstY - srcY;
            float dist = DetMath::hypot(dx, dy);

            // Siege Mode / Immobile unit cannot chase an out-of-range wall: drop target immediately
            if(dist > attackRange * TILE_WIDTH)
            {
                isDirectEngaged = false;
                if(isSiegeMode || moveSpeed <= 0.0f)
                {
                    targetWallTileX = -1;
                    targetWallTileY = -1;
                    isManualWallTarget = false;
                    if(state == ActionState::Attack)
                    {
                        state = ActionState::Idle;
                        animFrame = 0;
                    }
                    return;
                }
                if(state == ActionState::Attack && attackAnimTimer <= 0.0f)
                {
                    state = ActionState::Idle;
                    animFrame = 0;
                }
                if(state != ActionState::Move && !waitingForPath)
                {
                    bool isPartial = false;
                    if(pathfinder.findPath(tileX, tileY, targetWallTileX, targetWallTileY, path, id, &isPartial))
                    {
                        pathIndex = 0;
                        state = ActionState::Move;
                        waitingForPath = isPartial;
                    }
                }
                return;
            }

            // Within firing range: orient turret/hull and fire when cooldown is ready
            int targetDir8 = Utility::dir8FromVector(dx, dy);

            turretFacing = targetDir8;
            if(!hasRotatingTurret && state != ActionState::Move && !isSiegeMode)
            {
                if(!isInfantry() && facingDirection != targetDir8)
                {
                    if(turnStepDelay <= 0.0f)
                    {
                        facingDirection = targetDir8;
                    }
                    else
                    {
                        turnTimer += deltaTime;
                        while(turnTimer >= turnStepDelay && facingDirection != targetDir8)
                        {
                            turnTimer -= turnStepDelay;
                            int delta = Utility::turnStep8(facingDirection, targetDir8);
                            facingDirection = (facingDirection + delta + 8) % 8;
                        }
                    }
                    if(facingDirection != targetDir8)
                        return;
                }
                else
                {
                    facingDirection = targetDir8;
                }
            }
            else if(isSiegeMode)
            {
                facingDirection = targetDir8;
            }

            if(!isSiegeMode)
            {
                clearPathState();
                if(state == ActionState::Move)
                    state = ActionState::Idle;
            }

            if(cooldownTimer <= 0.0f)
            {
                cooldownTimer = nextCooldown();

                if(isFortress())
                {
                    int tdx = std::abs(targetWallTileX - tileX);
                    int tdy = std::abs(targetWallTileY - tileY);
                    bool isClose = (tdx <= 1 && tdy <= 1);
                    bool hasUpgrade = delegates.isFortressFlamethrowerUnlocked && delegates.isFortressFlamethrowerUnlocked(owner);
                    if(isClose && hasUpgrade)
                    {
                        projectile = ProjectileType::Flame;
                        if(delegates.spawnProjectile)
                        {
                            float mSrcX, mSrcY;
                            getMuzzlePoint(mSrcX, mSrcY);
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

                attackAnimTimer = isFortress() ? 0.25f : ((projectile == ProjectileType::Bullet) ? 0.12f : 0.22f);
                animFrame = 0;
                animTimer = 0.0f;
                state = ActionState::Attack;

                float mSrcX, mSrcY;
                getMuzzlePoint(mSrcX, mSrcY);
                if(delegates.spawnProjectile)
                {
                    delegates.spawnProjectile(*this, mSrcX, mSrcY, dstX, dstY, 0, map);
                }
                if(burstCount > 1)
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
        if(wallCheckCooldown > 0.0f)
            wallCheckCooldown -= deltaTime;
        if(targetEntityId == 0 && targetWallTileX < 0 && state == ActionState::Idle && path.empty() && waitingForPath && (isAttackMoving || !Players::slot(owner).human) && wallCheckCooldown <= 0.0f)
        {
            wallCheckCooldown = 1.0f;
            bool wallsInTheWay = !hasMoveTarget() || !hasOpenRouteTo(targetTileX, targetTileY, pathfinder);
            if(wallsInTheWay && delegates.findWallTarget)
            {
                delegates.findWallTarget(*this, targetWallTileX, targetWallTileY);
                if(targetWallTileX >= 0)
                    return;
            }
        }

        // 4. Auto-acquire Enemy Target & Support Assist (Depth = 1)
        if(targetEntityId == 0 && (state == ActionState::Idle || isAttackMoving || waitingForPath || !Players::slot(owner).human))
        {
            if(delegates.findTarget)
            {
                int bestTargetId = delegates.findTarget(*this);
                if(bestTargetId != 0)
                {
                    Entity *cand = delegates.getEntity ? delegates.getEntity(bestTargetId) : nullptr;
                    bool validForUnit = (cand != nullptr);
                    if(validForUnit && (isSiegeMode || moveSpeed <= 0.0f))
                    {
                        // Siege mode units ONLY acquire targets strictly within attackRange
                        float d = computeDistanceToEntityPixels(*this, *cand);
                        validForUnit = (d <= attackRange * TILE_WIDTH);
                    }
                    if(validForUnit && !isManualWallTarget)
                    {
                        targetEntityId = bestTargetId;
                        targetWallTileX = -1;
                        targetWallTileY = -1;
                        isSupportAssisting = false;
                        waitingForPath = false;
                    }
                }
                else if(isAttackMoving && hasMoveTarget())
                {
                    if(state == ActionState::Idle && !isSiegeMode && moveSpeed > 0.0f && !waitingForPath)
                    {
                        if(std::abs(tileX - targetTileX) > 1 || std::abs(tileY - targetTileY) > 1)
                        {
                            bool isPartial = false;
                            if(pathfinder.findPath(tileX, tileY, targetTileX, targetTileY, path, id, &isPartial))
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
                            clearMoveTarget();
                        }
                    }
                }
            }

            // Chain Support Assist (strictly depth = 1):
            // Only mobile idle units without a target assist a directly engaged frontline ally in attackRange.
            if(targetEntityId == 0 && state == ActionState::Idle && !isSiegeMode && moveSpeed > 0.0f && !waitingForPath && !isManualWallTarget)
            {
                if(delegates.findSupportAlly)
                {
                    int supportEnemyId = 0;
                    int supportAllyId = delegates.findSupportAlly(*this, supportEnemyId);
                    if(supportAllyId != 0 && supportEnemyId != 0)
                    {
                        Entity *enemy = delegates.getEntity ? delegates.getEntity(supportEnemyId) : nullptr;
                        if(enemy && enemy->alive && enemy->state != ActionState::Die)
                        {
                            targetEntityId = supportEnemyId;
                            targetWallTileX = -1;
                            targetWallTileY = -1;
                            isSupportAssisting = true; // Marks depth = 1 so other units do NOT chain off this unit!
                            waitingForPath = false;
                            bool isPartial = false;
                            if(pathfinder.findPath(tileX, tileY, enemy->tileX, enemy->tileY, path, id, &isPartial))
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
        if(targetEntityId != 0)
        {
            Entity *target = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;

            // 5a. Target destroyed or invalid
            if(!target || !target->alive || target->state == ActionState::Die)
            {
                targetEntityId = 0;
                isDirectEngaged = false;
                isSupportAssisting = false;

                if(delegates.findTarget)
                {
                    int nextTargetId = delegates.findTarget(*this);
                    if(nextTargetId != 0)
                    {
                        Entity *cand = delegates.getEntity ? delegates.getEntity(nextTargetId) : nullptr;
                        if(cand && (!isSiegeMode || computeDistanceToEntityPixels(*this, *cand) <= attackRange * TILE_WIDTH))
                        {
                            targetEntityId = nextTargetId;
                            return;
                        }
                    }
                }

                // Resume original move / attack-move order if a valid ground destination exists
                if(hasMoveTarget() && !isSiegeMode && moveSpeed > 0.0f)
                {
                    if(std::abs(tileX - targetTileX) > 1 || std::abs(tileY - targetTileY) > 1)
                    {
                        bool isPartial = false;
                        if(pathfinder.findPath(tileX, tileY, targetTileX, targetTileY, path, id, &isPartial))
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
                        clearMoveTarget();
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

            // 5b. Fog of War Visibility Gate (multi-tile aware via delegates.isEntityVisible)
            if(delegates.isEntityVisible && !delegates.isEntityVisible(*target, owner))
            {
                targetEntityId = 0;
                isDirectEngaged = false;
                isSupportAssisting = false;
                if(state != ActionState::Move)
                {
                    state = ActionState::Idle;
                    animFrame = 0;
                }
                return;
            }

            float srcX = getUnitCenterPixelX();
            float srcY = getUnitCenterPixelY();
            float dstX = target->getUnitCenterPixelX();
            float dstY = target->getUnitCenterPixelY();
            float dx = dstX - srcX;
            float dy = dstY - srcY;
            float dist = computeDistanceToEntityPixels(*this, *target);

            // 5c. Out-of-Range Check (Crucial for Siege Mode Anti-Vibration!)
            if(dist > attackRange * TILE_WIDTH)
            {
                isDirectEngaged = false;

                if(isSiegeMode || moveSpeed <= 0.0f)
                {
                    // Immobile siege unit MUST drop unreachable target immediately without rotating or flickering
                    targetEntityId = 0;
                    isSupportAssisting = false;
                    if(state == ActionState::Attack)
                    {
                        state = ActionState::Idle;
                        animFrame = 0;
                    }
                    return;
                }

                if(state == ActionState::Attack && attackAnimTimer <= 0.0f)
                {
                    state = ActionState::Idle;
                    animFrame = 0;
                }

                // Stuck in front of the target (a base walled in all around): shoot a way through the wall
                if(state != ActionState::Move && path.empty() && (isAttackMoving || !Players::slot(owner).human) && !isManualWallTarget && wallCheckCooldown <= 0.0f && delegates.findWallTarget)
                {
                    wallCheckCooldown = 1.0f;
                    if(!hasOpenRouteToEntity(*target, pathfinder))
                    {
                        int wx = -1, wy = -1;
                        delegates.findWallTarget(*this, wx, wy);
                        if(wx >= 0)
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

                if(state != ActionState::Move && !waitingForPath)
                {
                    bool isPartial = false;
                    if(pathfinder.findPath(tileX, tileY, target->tileX, target->tileY, path, id, &isPartial))
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
            int targetDir8 = Utility::dir8FromVector(dx, dy);

            turretFacing = targetDir8;
            if(!hasRotatingTurret && state != ActionState::Move && !isSiegeMode)
            {
                if(!isInfantry() && facingDirection != targetDir8)
                {
                    if(turnStepDelay <= 0.0f)
                    {
                        facingDirection = targetDir8;
                    }
                    else
                    {
                        turnTimer += deltaTime;
                        while(turnTimer >= turnStepDelay && facingDirection != targetDir8)
                        {
                            turnTimer -= turnStepDelay;
                            int delta = Utility::turnStep8(facingDirection, targetDir8);
                            facingDirection = (facingDirection + delta + 8) % 8;
                        }
                    }
                    if(facingDirection != targetDir8)
                    {
                        return;
                    }
                }
                else
                {
                    facingDirection = targetDir8;
                }
            }
            else if(isSiegeMode)
            {
                facingDirection = targetDir8;
            }

            // Mark unit as actively engaged in direct firing range
            isDirectEngaged = true;

            if(!isSiegeMode)
            {
                clearPathState();
                if(state == ActionState::Move)
                {
                    state = ActionState::Idle;
                    animFrame = 0;
                }
            }

            if(cooldownTimer <= 0.0f)
            {
                cooldownTimer = nextCooldown();

                if(isFortress())
                {
                    int tdx = std::abs(target->tileX - tileX);
                    int tdy = std::abs(target->tileY - tileY);
                    bool isClose = (tdx <= 1 && tdy <= 1);
                    bool hasUpgrade = delegates.isFortressFlamethrowerUnlocked && delegates.isFortressFlamethrowerUnlocked(owner);
                    if(isClose && hasUpgrade)
                    {
                        projectile = ProjectileType::Flame;
                        if(delegates.spawnProjectile)
                        {
                            float mSrcX, mSrcY;
                            getMuzzlePoint(mSrcX, mSrcY);
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

                attackAnimTimer = isFortress() ? 0.25f : ((projectile == ProjectileType::Bullet) ? 0.12f : 0.22f);
                animFrame = 0;
                animTimer = 0.0f;
                state = ActionState::Attack;

                float mSrcX, mSrcY;
                getMuzzlePoint(mSrcX, mSrcY);
                if(delegates.spawnProjectile)
                {
                    delegates.spawnProjectile(*this, mSrcX, mSrcY, dstX, dstY, target->id, map);
                }
                if(burstCount > 1)
                {
                    burstShotsRemaining = burstCount - 1;
                    burstTimer = burstDelay;
                }
            }
        }
    }

} // namespace OpenArtOfWar
