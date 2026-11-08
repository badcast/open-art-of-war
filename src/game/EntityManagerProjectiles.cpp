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

void EntityManager::SpawnProjectileFromEntity(
    Entity &entity, float srcX, float srcY, float dstX, float dstY, int targetEntityId, TileMap &map)
{
    // Shots at a building land on the side facing the shooter, not in its middle (units are hit at their center)
    if (targetEntityId > 0)
    {
        if (const Building *tb = GetBuilding(targetEntityId))
        {
            const float inset = 3.0f;
            const float minX = tb->tileX * TILE_WIDTH + inset;
            const float minY = tb->tileY * TILE_HEIGHT + inset;
            const float maxX = (tb->tileX + tb->buildWidth) * TILE_WIDTH - inset;
            const float maxY = (tb->tileY + tb->buildHeight) * TILE_HEIGHT - inset;
            dstX = std::clamp(srcX, minX, maxX) + Utility::Random(-8.0f, 8.0f);
            dstY = std::clamp(srcY, minY, maxY) + Utility::Random(-6.0f, 6.0f);
            dstX = std::clamp(dstX, minX, maxX);
            dstY = std::clamp(dstY, minY, maxY);
            // ... a tenth of the way in towards its middle: shots, shells and rockets land on the building, not on
            // its very edge
            constexpr float kInwards = 0.10f;
            const float cx = (tb->tileX + tb->buildWidth * 0.5f) * TILE_WIDTH;
            const float cy = (tb->tileY + tb->buildHeight * 0.5f) * TILE_HEIGHT;
            dstX += (cx - dstX) * kInwards;
            dstY += (cy - dstY) * kInwards;
        }
    }

    float dx = dstX - srcX;
    float dy = dstY - srcY;
    float dist = std::max(1.0f, DetMath::Hypot(dx, dy));

    // Randomized shot dispersion
    const float normRng = Utility::RandomSigned(); // [-1 .. +1]

    // Authentic dispersion defaults per projectile type (or override from entity.spreadAngle)
    float maxSpread = 0.0f;
    if (entity.spreadAngle >= 0.0f)
    {
        // Explicit configuration: if spread == 0.0f, maxSpread is 0.0f (high precision pinpoint accuracy)
        maxSpread = entity.spreadAngle;
    }
    else
    {
        // Unconfigured default (-1.0f): the spread of the projectile kind (projectiles.yaml)
        maxSpread = Projectiles::of(entity.projectile).spread;
    }

    float spreadDstX = dstX;
    float spreadDstY = dstY;
    float spreadDx = dx;
    float spreadDy = dy;

    if (maxSpread > 0.0001f)
    {
        float spreadAngle = normRng * maxSpread;
        float baseAng = DetMath::Atan2(dy, dx);
        float spreadAng = baseAng + spreadAngle;

        spreadDstX = srcX + DetMath::Cos(spreadAng) * dist;
        spreadDstY = srcY + DetMath::Sin(spreadAng) * dist;
        spreadDx = spreadDstX - srcX;
        spreadDy = spreadDstY - srcY;
    }

    const int dir16 = Utility::DirFromNorth(spreadDx, spreadDy, 16);
    const int dir32 = Utility::DirFromNorth(spreadDx, spreadDy, 32);

    const ProjectileSpec &spec = Projectiles::of(entity.projectile);
    // The sounds of the gun firing (its character file may name its own) and how loud this character is
    const WeaponConfig *gun = FiringWeapon(entity);
    const CharacterSounds *charSounds = SoundsOf(entity);
    const float soundGain = (gun ? gun->soundVolume : 1.0f) * (charSounds ? charSounds->volume : 1.0f);
    {
        // The sound of the shot by weapon kind
        const Unit *shooter = entity.AsUnit();
        SoundCue fire = SoundCue::FireMg;
        switch (entity.projectile)
        {
            case ProjectileType::Flame:
                fire = SoundCue::FireFlame;
                break;
            case ProjectileType::Sniper:
                fire = SoundCue::FireSniper;
                break;
            case ProjectileType::Shell:
            {
                // The heavy tanks (T-22 Zeus, Rhino, in any of their forms) have a deeper gun than the rest
                const Utility::ArchetypeInfo *info
                    = shooter ? Utility::GetArchetypeInfo(shooter->archetypeId) : nullptr;
                const int base
                    = info && info->baseFormId >= 0 ? info->baseFormId : (shooter ? shooter->archetypeId : -1);
                fire = (base == 10 || base == 11) ? SoundCue::FireCannonHigh : SoundCue::FireCannon;
                break;
            }
            case ProjectileType::Rocket:
                fire = (shooter && shooter->burstCount >= 3) ? SoundCue::FireMlrs : SoundCue::FireRocket;
                break;
            default:
                fire = (shooter && shooter->IsInfantry()) ? SoundCue::FireRifle : SoundCue::FireMg;
                break;
        }
        Cue(fire, srcX, srcY, entity.owner, gun ? gun->soundId : 0, soundGain);
        if (m_muzzleFlashes.size() < 512) // its light at night (a flame jet lights itself)
            if (entity.projectile != ProjectileType::Flame)
                m_muzzleFlashes.push_back({srcX, srcY, 0.0f});
    }
    if (spec.hitscan)
    {
        float finalHitX = spreadDstX;
        float finalHitY = spreadDstY;

        if (targetEntityId > 0)
        {
            Entity *target = GetEntity(targetEntityId);
            if (target && target->alive && target->state != ActionState::Die)
            {
                bool isTargetInfantry = (target->AsUnit() && target->AsUnit()->IsInfantry());
                int impactAnim = spec.PickImpact(target->id);
                // The gun's own hit sound, else the damage sound of the one hit (at its own volume)
                const CharacterSounds *hurt = SoundsOf(*target);
                const bool gunHit = gun && gun->impactSoundId != 0;
                const bool ownHit = !gunHit && hurt && hurt->damageId != 0;
                Cue(isTargetInfantry ? SoundCue::HitBullet : SoundCue::HitArmor,
                    finalHitX,
                    finalHitY,
                    entity.owner,
                    gunHit ? gun->impactSoundId : (ownHit ? hurt->damageId : 0),
                    ownHit ? hurt->volume : soundGain);

                NoteEntityHit(*target, entity.id);
                target->currentHp -= ApplyMovingUnitDamage(entity.attackDamage, target, entity.projectile);
                if (target->currentHp <= 0)
                {
                    target->currentHp = 0;
                    target->state = ActionState::Die;
                    target->animFrame = 0;
                    target->animTimer = 0.0f;
                    target->selected = false;
                    if (Unit *tu = target->AsUnit())
                    {
                        HandleUnitDeathState(tu, entity.projectile == ProjectileType::Flame);
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
                        SpawnExplosion(finalHitX, finalHitY, impactAnim);
                    }

                    NoteKill(*target, entity.owner);
                }
                else
                {
                    SpawnExplosion(finalHitX, finalHitY, impactAnim);
                }
            }
        }
        else if (entity.targetWallTileX >= 0 && entity.targetWallTileY >= 0)
        {
            int wx = entity.targetWallTileX;
            int wy = entity.targetWallTileY;
            if (map.GetWall(wx, wy) > 0)
            {
                NoteHit(map.GetWallOwner(wx, wy), entity.id);
                map.DamageWall(wx, wy, WeaponBalance::Scale(entity.attackDamage, entity.projectile, TargetClass::Wall));
                finalHitX = wx * TILE_WIDTH + (TILE_WIDTH * 0.5f);
                finalHitY = wy * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
                // (the wall character of the side names how its segments sound)
                const CharacterSounds *wallSounds = WallSoundsOf(map.GetWallFaction(wx, wy));
                const float wallGain = wallSounds ? wallSounds->volume : 1.0f;
                if (map.GetWall(wx, wy) == 0)
                {
                    SpawnExplosion(finalHitX, finalHitY, spec.wallDestroyed);
                    Cue(SoundCue::WallDestroyed,
                        finalHitX,
                        finalHitY,
                        entity.owner,
                        wallSounds ? wallSounds->deathId : 0,
                        wallGain);
                }
                else
                {
                    SpawnExplosion(finalHitX, finalHitY, spec.impactWall);
                    const bool gunHit = gun && gun->impactSoundId != 0;
                    Cue(SoundCue::HitArmor,
                        finalHitX,
                        finalHitY,
                        entity.owner,
                        gunHit ? gun->impactSoundId : (wallSounds ? wallSounds->damageId : 0),
                        gunHit ? soundGain : wallGain);
                }
            }
        }

        Projectile p;
        p.x = srcX;
        p.y = srcY;
        p.startX = srcX;
        p.startY = srcY;
        p.targetX = spreadDstX;
        p.targetY = spreadDstY;
        p.damage = 0;
        p.faction = entity.faction;
        p.owner = entity.owner;
        p.shooterId = entity.id;
        p.targetId = 0;
        p.active = true;
        p.smokeTimer = 0.0f;
        p.direction = dir16;
        p.dir32 = dir32;
        p.type = entity.projectile; // the tracer of its own kind (a sniper's round looks different)
        p.speed = spec.speed;
        m_projectiles.push_back(p);
        return;
    }

    // Physical projectile (Rocket, Shell, Flame)
    float projTargetX = spreadDstX;
    float projTargetY = spreadDstY;
    int projTargetId = targetEntityId;

    Projectile p;
    p.impactSound = gun ? gun->impactSoundId : 0;
    p.soundGain = soundGain;
    p.x = srcX;
    p.y = srcY;
    p.startX = p.x;
    p.startY = p.y;
    p.targetX = projTargetX;
    p.targetY = projTargetY;
    if (const Unit *su = entity.AsUnit(); su && su->isManualWallTarget && entity.targetWallTileX >= 0)
    {
        p.forcedWallX = entity.targetWallTileX;
        p.forcedWallY = entity.targetWallTileY;
    }
    p.damage = entity.attackDamage;
    p.splashPercent = entity.splashPercent;
    p.faction = entity.faction;
    p.owner = entity.owner;
    p.shooterId = entity.id;
    p.targetId = projTargetId;
    p.active = true;
    p.smokeTimer = 0.0f;
    p.direction = dir16;
    p.dir32 = dir32;
    p.type = entity.projectile;
    p.speed = spec.speed;
    // A shooter with a flight speed of its own (combat.bullet_speed, px/s) sends its rockets and shells that fast
    if (entity.bulletSpeed > 0.0f && (p.type == ProjectileType::Rocket || p.type == ProjectileType::Shell))
        p.speed = std::max(0.5f, entity.bulletSpeed * static_cast<float>(SIMULATION_TIMESTEP));
    if (p.type == ProjectileType::Flame)
    {
        // The jet is drawn by its own effect, as long as the projectile would fly (22 px per tick) but never
        // shorter than the animation needs: a target close by got no visible flame at all before
        constexpr float kMinFlameSeconds = 0.35f;
        const float flight = DetMath::Hypot(p.targetX - p.startX, p.targetY - p.startY) / p.speed
                             * static_cast<float>(SIMULATION_TIMESTEP);
        FlameBurst burst;
        burst.startX = p.startX;
        burst.startY = p.startY;
        burst.targetX = p.targetX;
        burst.targetY = p.targetY;
        burst.duration = std::max(kMinFlameSeconds, flight);
        burst.faction = p.faction;
        burst.owner = p.owner;
        m_flameBursts.push_back(burst);
    }
    m_projectiles.push_back(p);
}

} // namespace OpenArtOfWar
