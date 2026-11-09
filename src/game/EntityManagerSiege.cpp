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

bool EntityManager::CanDeploySiege(const Unit &u) const
{
    const UnitConfig *cfg = GetUnitConfig(u.archetypeId);
    if (cfg && cfg->isSiegeMode)
        return true; // packing up is always possible
    if (cfg && cfg->siegeRequiresResearch && !IsSiegeUnlocked(u.owner))
        return false;
    return true;
}

void EntityManager::UpdateAutoSiege(Unit &u, const TileMap &map, float deltaTime)
{
    if (u.state == ActionState::Die || u.state == ActionState::Transform)
        return;
    const UnitConfig *cfg = GetUnitConfig(u.archetypeId);
    if (!cfg)
        return;

    // Who may decide by itself: the AI always, the player's units only while on an attack-move order
    const bool aiSide = !Players::Slot(u.owner).human; // a computer player (the same on every peer)
    auto closestVisibleEnemy = [&](float maxTiles) {
        float best = maxTiles * TILE_WIDTH;
        int bestId = 0;
        for (const auto &other : m_entities)
        {
            if (!other.alive || other.state == ActionState::Die || !Players::Hostile(other.owner, u.owner))
                continue;
            if (!IsEntityVisibleToPlayer(other, u.owner, map))
                continue; // fog / camouflage do not count
            const float d = ComputeDistanceToEntityPixels(u, other);
            if (d <= best)
            {
                best = d;
                bestId = other.id;
            }
        }
        return bestId;
    };

    if (!cfg->isSiegeMode)
    {
        u.siegeQuietTimer = 0.0f;
        if (cfg->siegeArchetypeId < 0 || cfg->autoSiege == UnitConfig::AutoSiege::Off || !CanDeploySiege(u)
            || u.autoSiegeCooldownTimer > 0.0f)
            return;
        if (!aiSide && !u.isAttackMoving)
            return;
        const UnitConfig *siege = GetUnitConfig(cfg->siegeArchetypeId);
        if (!siege)
            return;
        // Both forms with the faction's range technologies (Hornet-10 alone already gives the sniper 9.5)
        const float siegeRange = ResearchedAttackRange(u.owner, siege->archetypeId, siege->attackRange);
        if (cfg->autoSiege == UnitConfig::AutoSiege::Far)
        {
            // Only worth it for the farther reach: an enemy beyond the normal range, within the siege range
            if (siegeRange <= u.attackRange + 0.25f)
                return;
            // Something to shoot in normal range: no need to deploy
            if (u.targetEntityId != 0)
            {
                const Entity *t = GetEntity(u.targetEntityId);
                if (t && ComputeDistanceToEntityPixels(u, *t) <= u.attackRange * TILE_WIDTH)
                    return;
            }
            if (closestVisibleEnemy(u.attackRange) != 0)
                return;
        }
        // Always: the siege form is worth having whatever the range (a salvo, concealment), so any enemy in sight
        // within the siege range deploys it
        if (closestVisibleEnemy(siegeRange) == 0)
            return;
        // The order stays: once nothing is left to shoot the unit packs up and carries on toward it
        const int keepX = u.targetTileX, keepY = u.targetTileY;
        const bool keepAttackMove = u.isAttackMoving;
        ToggleSiegeMode({u.id});
        u.targetTileX = keepX;
        u.targetTileY = keepY;
        u.isAttackMoving = keepAttackMove;
        u.autoDeployed = true;
        return;
    }

    // Auto-deployed and nothing visible within siege range for a while: pack up and carry on
    if (!u.autoDeployed)
        return;
    // (u.attackRange is the siege form's: it stays deployed while an enemy is within the siege range)
    if (closestVisibleEnemy(u.attackRange) != 0)
    {
        u.siegeQuietTimer = 0.0f;
        return;
    }
    u.siegeQuietTimer += deltaTime;
    if (u.siegeQuietTimer >= 4.0f)
    {
        u.siegeQuietTimer = 0.0f;
        u.autoDeployed = false;
        u.autoSiegeCooldownTimer = 3.5f;
        ToggleSiegeMode({u.id});
    }
}

void EntityManager::ToggleSiegeMode(const std::vector<int> &unitIds)
{
    for (int uid : unitIds)
    {
        Unit *u = GetUnit(uid);
        if (!u || u->state == ActionState::Die || u->state == ActionState::Transform)
            continue;
        if (!CanDeploySiege(*u))
            continue;            // technology not researched yet
        u->autoDeployed = false; // a manual [G] keeps the unit where the player put it

        int newArch = -1;
        const UnitConfig *curCfg = GetUnitConfig(u->archetypeId);
        if (curCfg && curCfg->isSiegeMode && curCfg->parentArchetypeId >= 0)
        {
            newArch = curCfg->parentArchetypeId;
        }
        else if (curCfg && curCfg->siegeArchetypeId >= 0)
        {
            newArch = curCfg->siegeArchetypeId;
        }
        else
        {
            switch (u->archetypeId)
            {
                case 4:
                    newArch = 14;
                    break;
                case 14:
                    newArch = 4;
                    break;
                case 10:
                    newArch = 15;
                    break;
                case 15:
                    newArch = 10;
                    break;
                case 13:
                    newArch = 16;
                    break;
                case 16:
                    newArch = 13;
                    break;
                default:
                    break;
            }
        }
        if (newArch >= 0)
        {
            // Settle any active tile reservation before transforming so the unit doesn't glide or lock tiles
            if (u->HasReservation())
            {
                if (std::abs(u->subX) * 2.0f > TILE_WIDTH || std::abs(u->subY) * 2.0f > TILE_HEIGHT)
                {
                    u->tileX = u->resTileX;
                    u->tileY = u->resTileY;
                }
                u->subX = 0.0f;
                u->subY = 0.0f;
                u->ReleaseReservation();
            }

            // Deployed by hand: the unit stays where the player put it
            if (!(curCfg && curCfg->isSiegeMode))
            {
                u->ClearMoveTarget();
                u->isAttackMoving = false;
            }
            u->state = ActionState::Transform;
            u->pendingArchetypeId = newArch;
            u->path.clear();
            u->pathIndex = 0;
            u->animFrame = 0;
            u->animTimer = 0.0f;
            u->burstShotsRemaining = 0;
            u->burstTimer = 0.0f;
            u->targetEntityId = 0;
            u->targetWallTileX = -1;
            u->targetWallTileY = -1;
            u->waitingForPath = false;
        }
    }
}

void EntityManager::TurretShift(int archetypeId, int facingDirection, float &outShiftX, float &outShiftY)
{
    outShiftX = 0.0f;
    outShiftY = 0.0f;
    // Autogenerated deterministic turret shift table [archetype][facing 0..7] relative to reference facing 2 (South)
    static constexpr float kTurretShifts[20][8][2] = {
        /*  0 */ {
            {0.0f, 0.0f},
            {1.0f, 0.0f},
            {0.0f, 0.0f},
            {0.0f, 0.0f},
            {-1.0f, 0.0f},
            {0.0f, 0.0f},
            {0.0f, -1.0f},
            {0.0f, -1.0f}},
        /*  1 */
        {{0.0f, 0.0f},
         {1.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {-1.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, -1.0f},
         {0.0f, -1.0f}},
        /*  2 */
        {{-2.0f, 0.0f},
         {-1.0f, 1.0f},
         {0.0f, 0.0f},
         {-1.0f, 0.0f},
         {-3.0f, 0.0f},
         {-2.0f, 0.0f},
         {-1.0f, -1.0f},
         {0.0f, -1.0f}},
        /*  3 */
        {{-2.0f, 0.0f},
         {-1.0f, 1.0f},
         {0.0f, 0.0f},
         {-1.0f, 0.0f},
         {-3.0f, 0.0f},
         {-2.0f, 0.0f},
         {-1.0f, -1.0f},
         {0.0f, -1.0f}},
        /*  4 */
        {{-2.0f, 0.0f},
         {-2.0f, 0.0f},
         {0.0f, 0.0f},
         {-2.0f, 0.0f},
         {-3.0f, 0.0f},
         {-1.0f, 0.0f},
         {0.0f, 0.0f},
         {-1.0f, 1.0f}},
        /*  5 */
        {{0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f}},
        /*  6 */
        {{-6.0f, 3.0f},
         {-3.0f, 1.0f},
         {0.0f, 0.0f},
         {4.0f, 1.0f},
         {6.0f, 3.0f},
         {6.0f, 6.0f},
         {0.0f, 9.0f},
         {-5.0f, 7.0f}},
        /*  7 */
        {{-6.0f, 3.0f},
         {-3.0f, 0.0f},
         {0.0f, 0.0f},
         {1.0f, 0.0f},
         {4.0f, 3.0f},
         {1.0f, 4.0f},
         {0.0f, 6.0f},
         {-5.0f, 5.0f}},
        /*  8 */
        {{-7.0f, 4.0f},
         {-5.0f, 0.0f},
         {0.0f, 0.0f},
         {5.0f, 1.0f},
         {5.0f, 4.0f},
         {5.0f, 6.0f},
         {0.0f, 9.0f},
         {-5.0f, 7.0f}},
        /*  9 */
        {{-2.0f, 0.0f},
         {-2.0f, -1.0f},
         {0.0f, 0.0f},
         {2.0f, -1.0f},
         {3.0f, 0.0f},
         {1.0f, 3.0f},
         {0.0f, 3.0f},
         {-2.0f, 3.0f}},
        /* 10 */
        {{-2.0f, 2.0f},
         {-2.0f, 1.0f},
         {0.0f, 0.0f},
         {2.0f, 1.0f},
         {4.0f, 2.0f},
         {3.0f, 4.0f},
         {0.0f, 7.0f},
         {-2.0f, 4.0f}},
        /* 11 */
        {{-6.0f, 3.0f},
         {-5.0f, 0.0f},
         {0.0f, 0.0f},
         {4.0f, 0.0f},
         {6.0f, 3.0f},
         {3.0f, 6.0f},
         {0.0f, 6.0f},
         {-4.0f, 6.0f}},
        /* 12 */
        {{-6.0f, 3.0f},
         {-3.0f, 1.0f},
         {0.0f, 0.0f},
         {2.0f, 1.0f},
         {5.0f, 2.0f},
         {4.0f, 5.0f},
         {-1.0f, 7.0f},
         {-3.0f, 5.0f}},
        /* 13 */
        {{0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f}},
        /* 14 */
        {{0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f}},
        /* 15 */
        {{0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f}},
        /* 16 */
        {{0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f}},
        /* 17 */
        {{-6.0f, 3.0f},
         {-5.0f, 0.0f},
         {0.0f, 0.0f},
         {4.0f, 0.0f},
         {6.0f, 3.0f},
         {3.0f, 6.0f},
         {0.0f, 6.0f},
         {-4.0f, 6.0f}},
        /* 18 */
        {{-6.0f, 3.0f},
         {-3.0f, 0.0f},
         {0.0f, 0.0f},
         {1.0f, 0.0f},
         {4.0f, 3.0f},
         {1.0f, 4.0f},
         {0.0f, 6.0f},
         {-5.0f, 5.0f}},
        /* 19 */
        {{0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f},
         {0.0f, 0.0f}},
    };
    if (archetypeId >= 0 && archetypeId < 20)
    {
        const int f = (facingDirection % 8 + 8) % 8;
        outShiftX = kTurretShifts[archetypeId][f][0];
        outShiftY = kTurretShifts[archetypeId][f][1];
    }
}

void EntityManager::UpdateTurretShift(Unit &u) const
{
    u.turretShiftX = u.turretShiftY = 0.0f;
    if (!u.hasRotatingTurret)
        return;
    // Deployed siege units (14..16) keep a single hull sprite; everything else has one per facing
    const bool fixedHull = (u.archetypeId >= 14 && u.archetypeId <= 16);
    // Always the fixed table, never the sprite's turret layer: the shift moves the muzzle, so the shots' start, spread
    // and flight time. Sprites are a player's own business (not in the network hash, not in replays): reading them
    // here put players with different sprite files out of sync and broke replays after a sprite was edited.
    TurretShift(u.archetypeId, fixedHull ? 0 : u.facingDirection, u.turretShiftX, u.turretShiftY);
}

void EntityManager::NoteHit(PlayerId victim, int attackerId)
{
    const Entity *attacker = GetEntity(attackerId);
    if (!attacker || victim >= kMaxPlayers || !Players::Hostile(attacker->owner, victim))
        return;
    for (auto &p : m_revealPings)
    {
        if (p.entityId == attackerId && p.observer == victim)
        {
            p.timeLeft = 2.5f; // keeps shooting: stays visible
            return;
        }
    }
    m_revealPings.push_back({attackerId, victim, 2.5f});
}

void EntityManager::NoteEntityHit(Entity &victim, int attackerId)
{
    NoteHit(victim.owner, attackerId);
    const Entity *attacker = GetEntity(attackerId);
    if (!attacker || !attacker->alive || attacker->state == ActionState::Die
        || !Players::Hostile(attacker->owner, victim.owner))
        return;
    victim.lastAttackerId = attackerId;
    victim.lastAttackedTimer = 6.0f;
}

} // namespace OpenArtOfWar
