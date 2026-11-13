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

// Distance in tiles from tile (px, py) to the segment (ax, ay) - (bx, by)
static float DistanceToSegment(int px, int py, int ax, int ay, int bx, int by)
{
    const float dx = static_cast<float>(bx - ax), dy = static_cast<float>(by - ay);
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return DetMath::Hypot(px - (ax + t * dx), py - (ay + t * dy));
}

void EntityManager::Update(float deltaTime, TileMap &map, Pathfinder &pathfinder)
{
    for (auto &p : m_revealPings)
        p.timeLeft -= deltaTime;
    m_revealPings.erase(
        std::remove_if(
            m_revealPings.begin(), m_revealPings.end(), [](const RevealPing &p) { return p.timeLeft <= 0.0f; }),
        m_revealPings.end());
    pathfinder.SetEntityManager(this);
    pathfinder.BeginTick();

    // Initial fog update for this tick
    UpdateFogOfWar(map);

    // Power balance of every player
    std::array<int, kMaxPlayers> prod {}, cons {};
    for (const auto &e : m_entities)
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->buildProgress < 1.0f || b->owner >= kMaxPlayers)
            continue;
        int pwr = b->GetPowerOutput();
        if (pwr > 0)
            prod[b->owner] += pwr;
        else
            cons[b->owner] += (-pwr);
    }
    std::array<bool, kMaxPlayers> powered {};
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        prod[p] += prod[p] * ResearchTotal(p, ResearchEffect::PowerBonus) / 100;
        powered[p] = m_unlimitedPower || cons[p] <= prod[p];
    }
    UpdateRegeneration(deltaTime);
    UpdateMines(deltaTime, map);

    // Build Delegates for Units
    UnitDelegates uDelegates;
    uDelegates.getEntity = [this](int id) -> Entity * {
        return GetEntity(id);
    };
    uDelegates.spawnProjectile
        = [this](Entity &shooter, float srcX, float srcY, float dstX, float dstY, int targetId, TileMap &m) {
              SpawnProjectileFromEntity(shooter, srcX, srcY, dstX, dstY, targetId, m);
          };
    uDelegates.spawnExplosion = [this](float x, float y, int animId) {
        SpawnExplosion(x, y, animId);
    };
    uDelegates.isTileOccupied = [this](int tx, int ty, int ignoreId) {
        return IsTileOccupiedByUnit(tx, ty, ignoreId);
    };
    uDelegates.getUnitAtTile = [this](int tx, int ty, int ignoreId) {
        return GetUnitIdAtTile(tx, ty, ignoreId);
    };
    uDelegates.onKill = [this](PlayerId victim, PlayerId killer) {
        NoteKill(victim, killer);
    };
    uDelegates.isFortressFlamethrowerUnlocked = [this](PlayerId p) {
        return IsFortressFlamethrowerUnlocked(p);
    };
    uDelegates.getUnitConfig = [this](int archId) {
        return GetUnitConfig(archId);
    };
    uDelegates.applyResearch = [](Unit &u) {
        ApplyCompletedResearch(u);
    };
    uDelegates.isEntityVisible = [this, &map](const Entity &entity, PlayerId observer) {
        return IsEntityVisibleToPlayer(entity, observer, map);
    };
    // Both pick by priority first (armed units, then other units and armed buildings, then the rest), then by
    // distance; m_entities is a vector, so the order (and the result) is the same on every peer
    auto bestTargetWithin = [this, &map](const Unit &u, float rangePixels) -> int {
        int bestId = 0;
        int bestTier = 3;
        float bestDist = rangePixels;
        for (const auto &other : m_entities)
        {
            if (!other.alive || other.state == ActionState::Die || !Players::Hostile(other.owner, u.owner))
                continue;
            const float d = ComputeDistanceToEntityPixels(u, other);
            if (d > rangePixels)
                continue;
            const int tier = TargetPriorityTier(other);
            if (tier > bestTier || (tier == bestTier && d > bestDist))
                continue;
            if (!IsEntityVisibleToPlayer(other, u.owner, map))
                continue;
            bestTier = tier;
            bestDist = d;
            bestId = other.id;
        }
        return bestId;
    };
    uDelegates.findEnemyInAttackRange = [bestTargetWithin](const Unit &u) -> int {
        return bestTargetWithin(u, u.attackRange * TILE_WIDTH);
    };
    uDelegates.findTarget = [bestTargetWithin](const Unit &u) -> int {
        // Moving units on an attack-move look as far as they see; so do idle units whose stance chases
        const bool mobile = !u.isSiegeMode && u.moveSpeed > 0.0f;
        const bool scansSight = mobile
                                && (u.isAttackMoving || u.waitingForPath
                                    || (u.state == ActionState::Idle && !u.HoldsGround() && !u.HasMoveTarget()));
        const float scanDist
            = (scansSight ? std::max(static_cast<float>(u.sightRadius), u.attackRange) : u.attackRange) * TILE_WIDTH;
        return bestTargetWithin(u, scanDist);
    };
    uDelegates.findWallTarget = [this, &map](const Unit &u, int &outWallX, int &outWallY) {
        outWallX = -1;
        outWallY = -1;
        // Something to shoot right now comes first; enemies seen behind the wall do not stop the breach
        // (a base walled in all around would never be attacked otherwise)
        bool enemyInRange = false;
        const float rangePixels = u.attackRange * TILE_WIDTH;
        for (const auto &other : m_entities)
        {
            if (!other.alive || other.state == ActionState::Die || !Players::Hostile(other.owner, u.owner))
                continue;
            if (other.faction == Faction::Neutral)
                continue;
            if (ComputeDistanceToEntityPixels(u, other) <= rangePixels && IsEntityVisibleToPlayer(other, u.owner, map))
            {
                enemyInRange = true;
                break;
            }
        }
        // Walls are picked toward where the unit is heading: its target, else its order's destination
        int goalX = u.HasMoveTarget() ? u.targetTileX : -1;
        int goalY = u.HasMoveTarget() ? u.targetTileY : -1;
        if (const Entity *t = (u.targetEntityId != 0) ? GetEntity(u.targetEntityId) : nullptr)
        {
            goalX = static_cast<int>(t->GetUnitCenterPixelX() / TILE_WIDTH);
            goalY = static_cast<int>(t->GetUnitCenterPixelY() / TILE_HEIGHT);
        }
        if (!enemyInRange)
        {
            // Walls an ally of the same player already shoots at come first: one tile down opens the way
            std::vector<std::pair<int, int>> allyWalls;
            for (const auto &other : m_entities)
            {
                if (other.alive && other.id != u.id && other.owner == u.owner && other.targetWallTileX >= 0)
                    allyWalls.emplace_back(other.targetWallTileX, other.targetWallTileY);
            }
            int scanR = 6;
            int minWX = std::max(0, u.tileX - scanR);
            int maxWX = std::min(map.GetWidth() - 1, u.tileX + scanR);
            int minWY = std::max(0, u.tileY - scanR);
            int maxWY = std::min(map.GetHeight() - 1, u.tileY + scanR);
            float bestWDist = 1e9f;
            for (int wy = minWY; wy <= maxWY; ++wy)
            {
                for (int wx = minWX; wx <= maxWX; ++wx)
                {
                    std::uint16_t w = map.GetWall(wx, wy);
                    if (w == 0)
                        continue;
                    // Only walls toward the destination; never turn back to shoot a wall behind
                    if (goalX >= 0
                        && DetMath::Hypot(wx - goalX, wy - goalY)
                               >= DetMath::Hypot(u.tileX - goalX, u.tileY - goalY) + 1.0f)
                    {
                        continue;
                    }
                    // ... and only the ones in the way: near the straight line to the goal (a wall off to
                    // the side hinders nobody)
                    if (goalX >= 0 && DistanceToSegment(wx, wy, u.tileX, u.tileY, goalX, goalY) > 2.0f)
                    {
                        continue;
                    }
                    // Hostile by owner (two players of one faction can be enemies)
                    if (Players::Hostile(map.GetWallOwner(wx, wy), u.owner))
                    {
                        float wd = DetMath::Hypot((wx - u.tileX) * TILE_WIDTH, (wy - u.tileY) * TILE_HEIGHT);
                        if (std::find(allyWalls.begin(), allyWalls.end(), std::make_pair(wx, wy)) != allyWalls.end())
                            wd -= 2.0f * TILE_WIDTH;
                        if (wd < bestWDist)
                        {
                            bestWDist = wd;
                            outWallX = wx;
                            outWallY = wy;
                        }
                    }
                }
            }
        }
    };
    uDelegates.findSupportAlly = [this, &map](const Unit &u, int &outTargetEnemyId) -> int {
        outTargetEnemyId = 0;
        float maxDist = u.attackRange * TILE_WIDTH;
        float bestDist = maxDist;
        int bestAllyId = 0;
        for (const auto &other : m_entities)
        {
            if (!other.alive || other.state == ActionState::Die || Players::Hostile(other.owner, u.owner))
                continue;
            const Unit *allyUnit = other.AsUnit();
            if (!allyUnit || allyUnit->id == u.id)
                continue;
            // STRICT DEPTH = 1 INVARIANT:
            // Ally must be in direct combat (within its firing range) AND must NOT be a secondary support unit itself!
            if (!allyUnit->isDirectEngaged || allyUnit->isSupportAssisting || allyUnit->targetEntityId == 0)
                continue;

            const Entity *enemy = GetEntity(allyUnit->targetEntityId);
            if (!enemy || !enemy->alive || enemy->state == ActionState::Die)
                continue;
            if (!IsEntityVisibleToPlayer(*enemy, u.owner, map))
                continue;

            float dx = other.GetUnitCenterPixelX() - u.GetUnitCenterPixelX();
            float dy = other.GetUnitCenterPixelY() - u.GetUnitCenterPixelY();
            float d = DetMath::Hypot(dx, dy);
            if (d <= bestDist)
            {
                bestDist = d;
                bestAllyId = other.id;
                outTargetEnemyId = allyUnit->targetEntityId;
            }
        }
        return bestAllyId;
    };

    // Build Delegates for Buildings
    BuildingDelegates bDelegates;
    bDelegates.getEntity = [this](int id) -> Entity * {
        return GetEntity(id);
    };
    bDelegates.spawnProjectile
        = [this](Entity &shooter, float srcX, float srcY, float dstX, float dstY, int targetId, TileMap &m) {
              SpawnProjectileFromEntity(shooter, srcX, srcY, dstX, dstY, targetId, m);
          };
    bDelegates.spawnExplosion = [this](float x, float y, int animId) {
        SpawnExplosion(x, y, animId);
    };
    bDelegates.onKill = [this](PlayerId victim, PlayerId killer) {
        NoteKill(victim, killer);
    };
    bDelegates.isEntityVisible = [this, &map](const Entity &entity, PlayerId observer) {
        return IsEntityVisibleToPlayer(entity, observer, map);
    };
    bDelegates.findTarget = [this, &map](const Building &b) -> int {
        float closestDist = b.attackRange * TILE_WIDTH;
        int bestId = 0;
        float bx = b.GetUnitCenterPixelX();
        float by = b.GetUnitCenterPixelY();
        for (const auto &other : m_entities)
        {
            if (!other.alive || other.state == ActionState::Die || !Players::Hostile(other.owner, b.owner))
                continue;
            if (!IsEntityVisibleToPlayer(other, b.owner, map))
                continue;
            float dx = other.GetUnitCenterPixelX() - bx;
            float dy = other.GetUnitCenterPixelY() - by;
            float d = DetMath::Hypot(dx, dy);
            if (d < closestDist)
            {
                closestDist = d;
                bestId = other.id;
            }
        }
        return bestId;
    };

    // 1. Update entities through polymorphic delegates
    for (auto &entity : m_entities)
    {
        if (!entity.alive)
            continue;

        if (Unit *unit = entity.AsUnit())
        {
            UpdateTurretShift(*unit);
            UpdateAutoSiege(*unit, map, deltaTime);
            unit->Update(deltaTime, map, pathfinder, uDelegates);
            UpdateTurretShift(*unit);
        }
        else if (Building *bldg = entity.AsBuilding())
        {
            bldg->isPowered = bldg->owner >= kMaxPlayers || powered[bldg->owner];
            bldg->Update(deltaTime, map, bDelegates);
        }
    }

    // Resolve any stacked units sharing the exact same tile
    ResolveUnitStacking(map);

    // 2. Update projectiles
    for (auto &proj : m_projectiles)
    {
        if (!proj.active)
            continue;

        float dx = proj.targetX - proj.x;
        float dy = proj.targetY - proj.y;
        float dist = std::sqrt(dx * dx + dy * dy);

        const ProjectileSpec &spec = Projectiles::of(proj.type);
        if (spec.smokeTrail)
        {
            proj.smokeTimer += deltaTime;
            if (proj.smokeTimer >= 0.030f)
            {
                proj.smokeTimer = 0.0f;
                SmokePuff puff;
                puff.x = proj.x;
                puff.y = proj.y;
                puff.lifetime = 0.0f;
                puff.maxLifetime = 0.35f;
                puff.active = true;
                m_smokePuffs.push_back(puff);
            }
        }

        if (dist <= proj.speed)
        {
            if (proj.damage <= 0)
            {
                proj.active = false;
                continue;
            }

            Entity *target = (proj.targetId > 0) ? GetEntity(proj.targetId) : nullptr;
            bool targetAlive = (target && target->alive && target->state != ActionState::Die);
            bool isTargetInfantry = (target && target->AsUnit() && target->AsUnit()->IsInfantry());

            // Hit animation of the projectile kind, picked by the target
            const int impactAnim = spec.PickImpact(proj.targetId);
            if (proj.type == ProjectileType::Shell || proj.type == ProjectileType::Rocket)
                Cue(SoundCue::ExplodeSmall, proj.targetX, proj.targetY, proj.owner, proj.impactSound, proj.soundGain);
            else if (proj.impactSound != 0) // a flame / bullet with a hit sound of its own
                Cue(isTargetInfantry ? SoundCue::HitBullet : SoundCue::HitArmor,
                    proj.targetX,
                    proj.targetY,
                    proj.owner,
                    proj.impactSound,
                    proj.soundGain);
            // A small crater where a shell / rocket lands (on the ground or the unit it hit)
            if (const int mark = spec.PickGroundMark(
                    proj.shooterId + proj.targetId + static_cast<int>(proj.targetX) + static_cast<int>(proj.targetY));
                mark >= 0)
            {
                // At the point of contact (on a building the side it came down on), whatever it hit
                AddGroundMark(proj.targetX, proj.targetY, mark, spec.groundMarkSeconds);
            }

            if (targetAlive)
            {
                // Units blow up at their center, buildings where the projectile came down
                const bool hitUnit = target->AsUnit() != nullptr;
                float hitCenterX = hitUnit ? target->GetUnitCenterPixelX() : proj.targetX;
                float hitCenterY = hitUnit ? target->GetUnitCenterPixelY() : proj.targetY;
                NoteEntityHit(*target, proj.shooterId);
                target->currentHp -= ApplyMovingUnitDamage(proj.damage, target, proj.type);
                if (target->currentHp <= 0)
                {
                    target->currentHp = 0;
                    target->state = ActionState::Die;
                    target->animFrame = 0;
                    target->animTimer = 0.0f;
                    target->selected = false;
                    if (Unit *tu = target->AsUnit())
                    {
                        HandleUnitDeathState(tu, proj.type == ProjectileType::Flame);
                    }

                    if (Building *tb = target->AsBuilding())
                    {
                        tb->deathTimer = EffectsConfig::Get().burialSeconds;
                        for (int ty = tb->tileY; ty < tb->tileY + tb->buildHeight; ++ty)
                        {
                            for (int tx = tb->tileX; tx < tb->tileX + tb->buildWidth; ++tx)
                            {
                                map.SetPassable(tx, ty, true);
                            }
                        }
                        SpawnDeathExplosions(*tb);
                    }
                    else if (!isTargetInfantry)
                    {
                        target->deathTimer = EffectsConfig::Get().burialSeconds;
                        SpawnDeathExplosions(*target);
                    }
                    else
                    {
                        target->deathTimer = EffectsConfig::Get().burialSeconds;
                        SpawnExplosion(hitCenterX, hitCenterY, impactAnim);
                    }

                    NoteKill(*target, proj.owner);
                }
                else
                {
                    SpawnExplosion(hitCenterX, hitCenterY, impactAnim);
                }
            }
            else
            {
                int hitTileX = static_cast<int>(proj.targetX / TILE_WIDTH);
                int hitTileY = static_cast<int>(proj.targetY / TILE_HEIGHT);
                // The wall the shooter was ordered to destroy takes the hit, whoever owns it
                const bool forced = proj.forcedWallX >= 0 && map.GetWall(proj.forcedWallX, proj.forcedWallY) > 0;
                if (forced)
                {
                    hitTileX = proj.forcedWallX;
                    hitTileY = proj.forcedWallY;
                }
                if (map.GetWall(hitTileX, hitTileY) > 0
                    && (forced || Players::Hostile(map.GetWallOwner(hitTileX, hitTileY), proj.owner)))
                {
                    NoteHit(map.GetWallOwner(hitTileX, hitTileY), proj.shooterId);
                    map.DamageWall(hitTileX, hitTileY, WeaponBalance::Scale(proj.damage, proj.type, TargetClass::Wall));
                    if (map.GetWall(hitTileX, hitTileY) == 0)
                    {
                        SpawnExplosion(
                            hitTileX * TILE_WIDTH + TILE_WIDTH * 0.5f,
                            hitTileY * TILE_HEIGHT + TILE_HEIGHT * 0.5f,
                            spec.wallDestroyed);
                        const CharacterSounds *wallSounds = WallSoundsOf(map.GetWallFaction(hitTileX, hitTileY));
                        Cue(SoundCue::WallDestroyed,
                            proj.targetX,
                            proj.targetY,
                            proj.owner,
                            wallSounds ? wallSounds->deathId : 0,
                            wallSounds ? wallSounds->volume : 1.0f);
                    }
                    else
                    {
                        SpawnExplosion(proj.targetX, proj.targetY, spec.impactWall);
                    }
                }
                else
                {
                    SpawnExplosion(proj.targetX, proj.targetY, impactAnim);
                }
            }

            // Splash / AoE Damage (3x3 tiles around impact) for Shell, Rocket, Flame
            if (proj.splashPercent > 0 && !spec.hitscan)
            {
                float impactX = targetAlive ? target->GetUnitCenterPixelX() : proj.targetX;
                float impactY = targetAlive ? target->GetUnitCenterPixelY() : proj.targetY;
                int primaryHitId = targetAlive ? target->id : 0;
                int splashDmg = std::max(1, static_cast<int>(std::round(proj.damage * (proj.splashPercent / 100.0f))));
                // The splash covers 3x3 tiles around the tile of the impact (a building when any tile of it is inside)
                const int splashTileX = static_cast<int>(std::floor(impactX / TILE_WIDTH));
                const int splashTileY = static_cast<int>(std::floor(impactY / TILE_HEIGHT));

                for (auto &other : m_entities)
                {
                    if (!other.alive || other.state == ActionState::Die)
                        continue;
                    if (other.id == primaryHitId)
                        continue;
                    if (!Players::Hostile(other.owner, proj.owner))
                        continue;

                    bool inSplash = false;
                    if (const Building *ob = other.AsBuilding())
                    {
                        inSplash = ob->tileX <= splashTileX + 1 && ob->tileX + ob->buildWidth - 1 >= splashTileX - 1
                                   && ob->tileY <= splashTileY + 1
                                   && ob->tileY + ob->buildHeight - 1 >= splashTileY - 1;
                    }
                    else
                    {
                        const int otx = static_cast<int>(std::floor(other.GetUnitCenterPixelX() / TILE_WIDTH));
                        const int oty = static_cast<int>(std::floor(other.GetUnitCenterPixelY() / TILE_HEIGHT));
                        inSplash = std::abs(otx - splashTileX) <= 1 && std::abs(oty - splashTileY) <= 1;
                    }
                    if (inSplash)
                    {
                        NoteEntityHit(other, proj.shooterId);
                        other.currentHp -= ApplyMovingUnitDamage(splashDmg, &other, proj.type);
                        if (other.currentHp <= 0)
                        {
                            other.currentHp = 0;
                            other.state = ActionState::Die;
                            other.animFrame = 0;
                            other.animTimer = 0.0f;
                            other.selected = false;
                            if (Unit *tu = other.AsUnit())
                            {
                                HandleUnitDeathState(tu, proj.type == ProjectileType::Flame);
                            }

                            if (Building *tb = other.AsBuilding())
                            {
                                tb->deathTimer = EffectsConfig::Get().burialSeconds;
                                for (int ty = tb->tileY; ty < tb->tileY + tb->buildHeight; ++ty)
                                {
                                    for (int tx = tb->tileX; tx < tb->tileX + tb->buildWidth; ++tx)
                                    {
                                        map.SetPassable(tx, ty, true);
                                    }
                                }
                                SpawnDeathExplosions(*tb);
                            }
                            else if (other.AsUnit() && other.AsUnit()->IsInfantry())
                            {
                                other.deathTimer = EffectsConfig::Get().burialSeconds;
                            }
                            else
                            {
                                other.deathTimer = EffectsConfig::Get().burialSeconds;
                                SpawnDeathExplosions(other);
                            }
                            NoteStats(other, proj.owner);
                        }
                    }
                }

                int cTileX = static_cast<int>(impactX / TILE_WIDTH);
                int cTileY = static_cast<int>(impactY / TILE_HEIGHT);
                for (int wy = cTileY - 1; wy <= cTileY + 1; ++wy)
                {
                    for (int wx = cTileX - 1; wx <= cTileX + 1; ++wx)
                    {
                        if (wx < 0 || wy < 0 || wx >= map.GetWidth() || wy >= map.GetHeight())
                            continue;
                        if (map.GetWall(wx, wy) > 0
                            && (Players::Hostile(map.GetWallOwner(wx, wy), proj.owner)
                                || (wx == proj.forcedWallX && wy == proj.forcedWallY)))
                        {
                            NoteHit(map.GetWallOwner(wx, wy), proj.shooterId);
                            map.DamageWall(wx, wy, WeaponBalance::Scale(splashDmg, proj.type, TargetClass::Wall));
                            if (map.GetWall(wx, wy) == 0)
                            {
                                SpawnExplosion(
                                    wx * TILE_WIDTH + TILE_WIDTH * 0.5f,
                                    wy * TILE_HEIGHT + TILE_HEIGHT * 0.5f,
                                    spec.wallDestroyed);
                                const CharacterSounds *wallSounds = WallSoundsOf(map.GetWallFaction(wx, wy));
                                Cue(SoundCue::WallDestroyed,
                                    wx * TILE_WIDTH + TILE_WIDTH * 0.5f,
                                    wy * TILE_HEIGHT + TILE_HEIGHT * 0.5f,
                                    proj.owner,
                                    wallSounds ? wallSounds->deathId : 0,
                                    wallSounds ? wallSounds->volume : 1.0f);
                            }
                        }
                    }
                }
            }
            proj.active = false;
        }
        else
        {
            proj.x += (dx / dist) * proj.speed;
            proj.y += (dy / dist) * proj.speed;
        }
    }

    // Finished shots are not part of simulation state or snapshots. Keep live shots in their existing order.
    m_projectiles.erase(
        std::remove_if(m_projectiles.begin(), m_projectiles.end(), [](const Projectile &p) { return !p.active; }),
        m_projectiles.end());

    // Craters fade away after their time
    for (GroundMark &m : m_groundMarks)
        m.timeLeft -= deltaTime;
    m_groundMarks.erase(
        std::remove_if(
            m_groundMarks.begin(), m_groundMarks.end(), [](const GroundMark &m) { return m.timeLeft <= 0.0f; }),
        m_groundMarks.end());

    // 3. Update smoke puffs
    for (auto &puff : m_smokePuffs)
    {
        if (!puff.active)
            continue;
        puff.lifetime += deltaTime;
        if (puff.lifetime >= puff.maxLifetime)
        {
            puff.active = false;
        }
    }
    m_smokePuffs.erase(
        std::remove_if(m_smokePuffs.begin(), m_smokePuffs.end(), [](const SmokePuff &p) { return !p.active; }),
        m_smokePuffs.end());

    UpdateVisualEffects(deltaTime);

    // 5. Wrecks stay for minutes; a big battle leaves more of them than living entities, and every one costs time
    // in each pass over the list. Beyond MAX_WRECKS the oldest go straight to their fade.
    {
        std::vector<Entity *> wrecks;
        for (Entity &e : m_entities)
            if (e.alive && e.state == ActionState::Die)
                wrecks.push_back(&e);
        if (wrecks.size() > static_cast<std::size_t>(MAX_WRECKS))
        {
            const std::size_t excess = wrecks.size() - static_cast<std::size_t>(MAX_WRECKS);
            std::sort(wrecks.begin(), wrecks.end(), [](const Entity *a, const Entity *b) {
                return a->deathTimer != b->deathTimer ? a->deathTimer < b->deathTimer : a->id < b->id;
            });
            for (std::size_t i = 0; i < excess; ++i)
                wrecks[i]->deathTimer = std::min(wrecks[i]->deathTimer, 0.0f);
        }
    }

    // 6. Clean up dead entities
    m_entities.Raw().erase(
        std::remove_if(
            m_entities.Raw().begin(),
            m_entities.Raw().end(),
            [](const std::unique_ptr<Entity> &e) { return !e->alive; }),
        m_entities.Raw().end());

    // 7. Update Fog of War post-movement
    UpdateFogOfWar(map);
}

// Muzzle flashes, flame jets and explosions: they age with time only, so a replica runs them too
void EntityManager::UpdateVisualEffects(float deltaTime)
{
    if (m_replicaWorld)
    {
        const EffectsConfig &fx = EffectsConfig::Get();
        for (GroundMark &m : m_groundMarks)
            m.timeLeft -= deltaTime;
        m_groundMarks.erase(
            std::remove_if(
                m_groundMarks.begin(), m_groundMarks.end(), [](const GroundMark &m) { return m.timeLeft <= 0.0f; }),
            m_groundMarks.end());
        for (Entity &e : m_entities)
        {
            if (e.state != ActionState::Die)
                continue;
            e.deathTimer -= deltaTime;
            if (Unit *u = e.AsUnit())
            {
                if (u->IsInfantry())
                {
                    u->animTimer += deltaTime;
                    while (u->animTimer >= 0.08f && u->animFrame < 25)
                    {
                        u->animTimer -= 0.08f;
                        ++u->animFrame;
                    }
                }
                else
                {
                    const int stages = u->wreckStages > 0 ? u->wreckStages : fx.WreckOfVehicle(u->archetypeId).stages;
                    const float elapsed = std::clamp(fx.burialSeconds - e.deathTimer, 0.0f, fx.burialSeconds);
                    e.animFrame = std::min(stages - 1, static_cast<int>(elapsed / fx.burialSeconds * stages));
                }
            }
        }
        m_entities.Raw().erase(
            std::remove_if(
                m_entities.Raw().begin(),
                m_entities.Raw().end(),
                [&](const std::unique_ptr<Entity> &e) {
                    return e->state == ActionState::Die && e->deathTimer <= -fx.fadeSeconds;
                }),
            m_entities.Raw().end());
    }
    // 3b. Flamethrower jets and muzzle flashes (visual only)
    for (auto &flash : m_muzzleFlashes)
        flash.age += deltaTime;
    m_muzzleFlashes.erase(
        std::remove_if(
            m_muzzleFlashes.begin(),
            m_muzzleFlashes.end(),
            [](const MuzzleFlash &f) { return f.age >= kMuzzleFlashKeep; }),
        m_muzzleFlashes.end());
    for (auto &burst : m_flameBursts)
        burst.age += deltaTime;
    m_flameBursts.erase(
        std::remove_if(
            m_flameBursts.begin(), m_flameBursts.end(), [](const FlameBurst &b) { return b.age >= b.duration; }),
        m_flameBursts.end());

    // 4. Update explosions
    for (auto &exp : m_explosions)
    {
        if (!exp.active)
            continue;
        if (exp.delay > 0.0f)
        {
            exp.delay -= deltaTime;
            continue;
        }
        exp.frameTimer += deltaTime;
        if (exp.frameTimer >= EffectsConfig::Get().frameTime)
        {
            exp.frameTimer = 0.0f;
            exp.currentFrame++;
            if (exp.currentFrame >= exp.totalFrames)
            {
                exp.active = false;
            }
        }
    }
    m_explosions.erase(
        std::remove_if(m_explosions.begin(), m_explosions.end(), [](const ExplosionParticle &e) { return !e.active; }),
        m_explosions.end());
}

} // namespace OpenArtOfWar
