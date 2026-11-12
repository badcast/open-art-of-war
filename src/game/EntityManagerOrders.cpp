#include "game/EntityManager.hpp"
#include "EntityManagerInternal.hpp"
#include "graphics/SpriteRegistry.hpp"
#include "game/Projectiles.hpp"
#include "game/WeaponBalance.hpp"
#include "core/DetMath.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/Localization.hpp"
#include "core/Utility.hpp"
#include "game/Effects.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "game/TileMap.hpp"
#include "graphics/TextureManager.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <yaml-cpp/yaml.h>

namespace OpenArtOfWar
{

void EntityManager::NearestOpenTile(const TileMap &map, int &tileX, int &tileY)
{
    const int w = map.GetWidth(), h = map.GetHeight();
    if (w <= 0 || h <= 0)
        return;
    tileX = std::clamp(tileX, 0, w - 1);
    tileY = std::clamp(tileY, 0, h - 1);
    auto open = [&](int x, int y) {
        return map.IsPassable(x, y) && map.GetWall(x, y) == 0;
    };
    if (open(tileX, tileY))
        return;
    int bestX = tileX, bestY = tileY;
    float best = 1e9f;
    for (int r = 1; r <= 16 && best >= 1e9f; ++r)
    {
        for (int y = tileY - r; y <= tileY + r; ++y)
        {
            for (int x = tileX - r; x <= tileX + r; ++x)
            {
                if (std::max(std::abs(x - tileX), std::abs(y - tileY)) != r || x < 0 || y < 0 || x >= w || y >= h
                    || !open(x, y))
                    continue;
                const float d = static_cast<float>((x - tileX) * (x - tileX) + (y - tileY) * (y - tileY));
                if (d < best)
                {
                    best = d;
                    bestX = x;
                    bestY = y;
                }
            }
        }
    }
    tileX = bestX;
    tileY = bestY;
}

void EntityManager::IssueMoveOrder(
    const std::vector<int> &unitIds, int targetTileX, int targetTileY, Pathfinder &pathfinder, bool attackMove)
{
    pathfinder.SetEntityManager(this);
    const TileMap *map = pathfinder.GetMap();
    if (!map || unitIds.empty())
        return;

    // A click on the edge of the map (or on ground nobody can stand on) sends the units to the nearest place they can
    NearestOpenTile(*map, targetTileX, targetTileY);
    const int mapW = map->GetWidth();
    const int mapH = map->GetHeight();

    std::vector<int> validUnits;
    float sumUnitX = 0.0f, sumUnitY = 0.0f;
    for (int uid : unitIds)
    {
        Unit *u = GetUnit(uid);
        if (!u || !u->alive || u->state == ActionState::Die || u->state == ActionState::Transform)
            continue;
        if (u->isSiegeMode || u->moveSpeed <= 0.0f)
        {
            u->ClearMoveTarget();
            u->ClearPathState();
            // Attack-move [Q] to a deployed unit: it keeps firing while it sees enemies, then packs up and follows
            const UnitConfig *cfg = GetUnitConfig(u->archetypeId);
            if (attackMove && u->isSiegeMode && cfg && cfg->parentArchetypeId >= 0)
            {
                u->targetTileX = targetTileX;
                u->targetTileY = targetTileY;
                u->isAttackMoving = true;
                u->autoDeployed = true;
            }
            continue;
        }
        validUnits.push_back(uid);
        sumUnitX += static_cast<float>(u->tileX);
        sumUnitY += static_cast<float>(u->tileY);
    }
    if (validUnits.empty())
        return;

    float unitCentroidX = sumUnitX / static_cast<float>(validUnits.size());
    float unitCentroidY = sumUnitY / static_cast<float>(validUnits.size());

    std::vector<StormcastSlot> slots;
    if (pathfinder.GetMap())
    {
        slots = AINavigataionStormcastView::ComputeGroupSlots(
            targetTileX, targetTileY, static_cast<int>(validUnits.size()), *pathfinder.GetMap(), this, validUnits);
    }

    // Match each unit to the slot that preserves its relative formation offset from the squad centroid
    // (Prevents units from crossing paths and colliding with each other!)
    if (slots.size() == validUnits.size() && validUnits.size() > 1)
    {
        float sumSlotX = 0.0f, sumSlotY = 0.0f;
        for (const auto &s : slots)
        {
            sumSlotX += static_cast<float>(s.x);
            sumSlotY += static_cast<float>(s.y);
        }
        float slotCentroidX = sumSlotX / static_cast<float>(slots.size());
        float slotCentroidY = sumSlotY / static_cast<float>(slots.size());

        std::vector<StormcastSlot> matchedSlots(validUnits.size());
        std::vector<bool> slotUsed(slots.size(), false);

        for (std::size_t i = 0; i < validUnits.size(); ++i)
        {
            const Unit *u = GetUnit(validUnits[i]);
            float relX = u ? (static_cast<float>(u->tileX) - unitCentroidX) : 0.0f;
            float relY = u ? (static_cast<float>(u->tileY) - unitCentroidY) : 0.0f;
            relX = std::clamp(relX, -4.0f, 4.0f);
            relY = std::clamp(relY, -4.0f, 4.0f);
            float idealX = slotCentroidX + relX;
            float idealY = slotCentroidY + relY;

            std::size_t bestIdx = 0;
            float bestDistSq = 1e18f;
            for (std::size_t k = 0; k < slots.size(); ++k)
            {
                if (slotUsed[k])
                    continue;
                float dx = static_cast<float>(slots[k].x) - idealX;
                float dy = static_cast<float>(slots[k].y) - idealY;
                float dsq = dx * dx + dy * dy;
                if (dsq < bestDistSq)
                {
                    bestDistSq = dsq;
                    bestIdx = k;
                }
            }
            slotUsed[bestIdx] = true;
            matchedSlots[i] = slots[bestIdx];
        }
        slots = std::move(matchedSlots);
    }

    // Pre-mark squad units as moving so Pathfinder::syncLocks does not treat them as impassable walls
    for (int uid : validUnits)
    {
        if (Unit *u = GetUnit(uid))
        {
            u->isAttackMoving = attackMove;
            u->state = ActionState::Move;
        }
    }

    // A group: one flow field toward the order's point gives everybody the road, each unit only searches the last
    // few tiles to its own slot
    const bool flow = validUnits.size() >= 2 && pathfinder.BuildFlowField(targetTileX, targetTileY);

    std::vector<std::pair<int, int>> assignedEndTiles;
    for (std::size_t i = 0; i < validUnits.size(); ++i)
    {
        int uid = validUnits[i];
        Unit *u = GetUnit(uid);
        if (!u)
            continue;

        int destX = (i < slots.size()) ? std::clamp(slots[i].x, 0, mapW - 1) : targetTileX;
        int destY = (i < slots.size()) ? std::clamp(slots[i].y, 0, mapH - 1) : targetTileY;
        NearestOpenTile(*map, destX, destY); // (formation slots at the edge / on blocked ground too)

        u->targetEntityId = 0;
        u->targetWallTileX = -1;
        u->targetWallTileY = -1;
        u->isManualWallTarget = false;
        u->isDirectEngaged = false;
        u->isSupportAssisting = false;
        u->isAttackMoving = attackMove;
        // Several units ordered together may breach hostile walls instead of queueing at one gap
        u->groupOrder = validUnits.size() >= 2;
        u->holdPosition = false;
        u->orderedTargetId = 0;
        u->blockedTimer = 0.0f;
        u->groupHold = false; // a new leg: the group decides again whether this one waits

        if (u->tileX == destX && u->tileY == destY && std::abs(u->subX) <= 0.001f && std::abs(u->subY) <= 0.001f)
        {
            u->ClearMoveTarget();
            u->ClearPathState();
            u->state = ActionState::Idle;
            continue;
        }

        u->targetTileX = destX;
        u->targetTileY = destY;
        u->waitingForPath = false;
        u->pathRetryTimer = 0.0f;
        u->attackAnimTimer = 0.0f;
        u->burstShotsRemaining = 0;
        u->burstTimer = 0.0f;

        bool isPartial = false;
        const bool found = flow ? pathfinder.FlowPath(u->tileX, u->tileY, destX, destY, u->path, u->id, &isPartial)
                                : pathfinder.FindPath(u->tileX, u->tileY, destX, destY, u->path, u->id, &isPartial);
        if (found && !u->path.empty())
        {
            if (isPartial && !u->path.empty())
            {
                int endX = u->path.back().x;
                int endY = u->path.back().y;
                for (const auto &pt : assignedEndTiles)
                {
                    if (pt.first == endX && pt.second == endY)
                    {
                        while (u->path.size() > 1)
                        {
                            u->path.pop_back();
                            endX = u->path.back().x;
                            endY = u->path.back().y;
                            bool clash = false;
                            for (const auto &p2 : assignedEndTiles)
                            {
                                if (p2.first == endX && p2.second == endY)
                                {
                                    clash = true;
                                    break;
                                }
                            }
                            if (!clash)
                                break;
                        }
                        break;
                    }
                }
            }
            if (!u->path.empty())
            {
                assignedEndTiles.push_back({u->path.back().x, u->path.back().y});
            }
            u->pathIndex = 0;
            u->state = ActionState::Move;
            u->waitingForPath = isPartial;
            u->waitBlockedTimer = 0.0f;
        }
        else
        {
            u->ClearPathState();
            u->state = ActionState::Idle;
            u->waitingForPath = true;
            u->pathRetryTimer = 0.0f;
        }
    }
}

void EntityManager::IssueAttackMoveOrder(
    const std::vector<int> &unitIds, int targetTileX, int targetTileY, Pathfinder &pathfinder)
{
    IssueMoveOrder(unitIds, targetTileX, targetTileY, pathfinder, true);
}

void EntityManager::IssueAttackOrder(const std::vector<int> &unitIds, int targetEntityId, Pathfinder *pathfinder)
{
    Entity *target = GetEntity(targetEntityId);
    if (!target || !target->alive || target->state == ActionState::Die)
        return;

    if (pathfinder)
    {
        pathfinder->SetEntityManager(this);
    }

    for (int uid : unitIds)
    {
        Unit *u = GetUnit(uid);
        if (!u || !u->alive || u->state == ActionState::Die || u->state == ActionState::Transform
            || u->id == targetEntityId)
            continue;
        if (!Players::Hostile(u->owner, target->owner))
            continue;
        u->holdPosition = false; // an explicit attack order ends a hold
        u->orderedTargetId = targetEntityId;

        // 1. If already targeting this exact entity, do NOT reset state or animation frame!
        if (u->targetEntityId == targetEntityId && !u->isManualWallTarget)
        {
            float dist = ComputeDistanceToEntityPixels(*u, *target);
            if (dist <= u->attackRange * TILE_WIDTH)
            {
                continue; // Already engaged in range; let updateCombat manage cooldown & firing anim
            }
        }

        // 2. Compute distance to target (footprint-aware for multi-tile buildings)
        float dist = ComputeDistanceToEntityPixels(*u, *target);
        bool inRange = (dist <= u->attackRange * TILE_WIDTH);

        // 3. Siege Mode / Immobile Unit Strict Gate:
        // Cannot move to pursue targets out of range. Reject out-of-range orders cleanly!
        if (u->isSiegeMode || u->moveSpeed <= 0.0f)
        {
            if (!inRange)
            {
                u->targetEntityId = 0;
                u->isDirectEngaged = false;
                u->ClearMoveTarget();
                u->ClearPathState();
                if (u->state == ActionState::Attack && u->attackAnimTimer <= 0.0f)
                {
                    u->state = ActionState::Idle;
                    u->animFrame = 0;
                }
                continue;
            }

            // Target is within Siege firing range: assign targetEntityId and let updateCombat fire
            u->targetEntityId = targetEntityId;
            u->targetWallTileX = -1;
            u->targetWallTileY = -1;
            u->isManualWallTarget = false;
            u->isAttackMoving = false;
            u->ClearMoveTarget();
            u->ClearPathState();
            continue;
        }

        // 4. Mobile Unit Order Handling
        u->targetEntityId = targetEntityId;
        u->targetWallTileX = -1;
        u->targetWallTileY = -1;
        u->isManualWallTarget = false;
        u->isAttackMoving = false;
        u->isSupportAssisting = false;
        // Pure entity pursuit must NOT store ground move coordinates (prevents (0,0) bug!)
        u->ClearMoveTarget();

        if (inRange)
        {
            u->ClearPathState();
            u->state = ActionState::Attack;
        }
        else if (pathfinder)
        {
            bool isPartial = false;
            if (u->FindPathToAttack(*target, *pathfinder, &isPartial))
            {
                u->pathIndex = 0;
                u->state = ActionState::Move;
                u->waitingForPath = isPartial;
            }
            else
            {
                u->state = ActionState::Idle;
                u->waitingForPath = true;
            }
        }
    }
}

void EntityManager::IssueAttackWallOrder(
    const std::vector<int> &unitIds, int wallTileX, int wallTileY, Pathfinder &pathfinder)
{
    pathfinder.SetEntityManager(this);
    const TileMap *map = pathfinder.GetMap();
    if (!map || map->GetWall(wallTileX, wallTileY) == 0)
        return;

    for (int uid : unitIds)
    {
        Unit *u = GetUnit(uid);
        if (!u || !u->alive || u->state == ActionState::Die || u->state == ActionState::Transform
            || u->attackDamage <= 0)
            continue;
        u->holdPosition = false;

        float srcX = u->GetUnitCenterPixelX();
        float srcY = u->GetUnitCenterPixelY();
        float dstX = wallTileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
        float dstY = wallTileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
        float dist = DetMath::Hypot(dstX - srcX, dstY - srcY);
        bool inRange = (dist <= u->attackRange * TILE_WIDTH);

        if (u->isSiegeMode || u->moveSpeed <= 0.0f)
        {
            if (!inRange)
            {
                // Reject out-of-range wall target for immobile siege unit
                u->targetWallTileX = -1;
                u->targetWallTileY = -1;
                u->isManualWallTarget = false;
                u->targetEntityId = 0;
                if (u->state == ActionState::Attack && u->attackAnimTimer <= 0.0f)
                {
                    u->state = ActionState::Idle;
                    u->animFrame = 0;
                }
                continue;
            }

            u->targetEntityId = 0;
            u->targetWallTileX = wallTileX;
            u->targetWallTileY = wallTileY;
            u->isManualWallTarget = true;
            u->isAttackMoving = false;
            u->ClearMoveTarget();
            u->ClearPathState();
            continue;
        }

        u->targetEntityId = 0;
        u->targetWallTileX = wallTileX;
        u->targetWallTileY = wallTileY;
        u->isManualWallTarget = true;
        u->isAttackMoving = false;
        u->isSupportAssisting = false;
        u->ClearMoveTarget();

        if (inRange)
        {
            u->ClearPathState();
            u->state = ActionState::Attack;
        }
        else
        {
            bool isPartial = false;
            if (pathfinder.FindPath(u->tileX, u->tileY, wallTileX, wallTileY, u->path, u->id, &isPartial))
            {
                u->pathIndex = 0;
                u->state = ActionState::Move;
                u->waitingForPath = isPartial;
            }
            else
            {
                u->state = ActionState::Idle;
                u->waitingForPath = true;
            }
        }
    }
}

int EntityManager::BaseBuildRadius(PlayerId player) const
{
    const Faction f = player < kMaxPlayers ? Players::Slot(player).faction : Faction::Resistance;
    return m_factionBuildRadius[f == Faction::Confederation ? 1 : 0];
}

int EntityManager::GetBuildRadius(PlayerId player) const
{
    return BaseBuildRadius(player) + (player < kMaxPlayers ? m_buildRadiusExtra[player] : 0);
}

void EntityManager::SetBuildRadiusPercent(PlayerId player, int percent)
{
    if (player >= kMaxPlayers)
        return;
    const int base = BaseBuildRadius(player);
    m_buildRadiusExtra[player] = base * (100 + percent) / 100 - base;
}

} // namespace OpenArtOfWar
