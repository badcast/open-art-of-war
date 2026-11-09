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

void Unit::UpdateMovement(float deltaTime, TileMap &map, Pathfinder &pathfinder, const UnitDelegates &delegates)
{
    float budget = std::max(0.0f, moveSpeed) * (deltaTime * 60.0f);

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
        if (budget <= 0.0f || state == ActionState::Transform)
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
            // (one waiting for its group stands: it is not about to free the tile)
            const bool allyLeaving
                = friendlyUnit
                  && ((friendlyUnit->state == ActionState::Move && !friendlyUnit->groupHold)
                      || friendlyUnit->HasReservation());
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
            && blockRetries < kBlockRetries
            && targetEntityId == 0) // (a unit closing in on a target keeps looking for its firing spot)
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
    // Too far ahead of its marching group: stands on this tile (the step under way was finished above)
    // with its path and order kept, until the group releases it
    if (groupHold)
        return;

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
        && back.size() <= static_cast<std::size_t>(kMaxDistance + 4))
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

} // namespace OpenArtOfWar
