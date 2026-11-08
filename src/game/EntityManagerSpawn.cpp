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

int EntityManager::SpawnUnit(UnitType type, int tileX, int tileY, Faction faction)
{
    int arch = 0;
    if (faction == Faction::Resistance)
    {
        switch (type)
        {
            case UnitType::ResistanceRifleman:
            case UnitType::ConfederationRifleman:
                arch = 0;
                break;
            case UnitType::HeavyAssault:
            case UnitType::Grenadier:
            case UnitType::Flamethrower:
                arch = 2;
                break;
            case UnitType::Sniper:
                arch = 4;
                break;
            case UnitType::Coyote:
            case UnitType::Fortress:
                arch = 6;
                break;
            case UnitType::Armadillo:
            case UnitType::Hammer:
                arch = 8;
                break;
            case UnitType::Rhino:
            case UnitType::Zeus:
                arch = 10;
                break;
            case UnitType::Porcupine:
            case UnitType::Grad:
                arch = 12;
                break;
            default:
                arch = 0;
                break;
        }
    }
    else
    {
        switch (type)
        {
            case UnitType::ResistanceRifleman:
            case UnitType::ConfederationRifleman:
                arch = 1;
                break;
            case UnitType::HeavyAssault:
                arch = 3;
                break;
            case UnitType::Grenadier:
            case UnitType::Flamethrower:
            case UnitType::Sniper:
                arch = 5;
                break;
            case UnitType::Coyote:
            case UnitType::Fortress:
                arch = 7;
                break;
            case UnitType::Armadillo:
            case UnitType::Hammer:
                arch = 9;
                break;
            case UnitType::Rhino:
            case UnitType::Zeus:
                arch = 11;
                break;
            case UnitType::Porcupine:
            case UnitType::Grad:
                arch = 13;
                break;
            default:
                arch = 1;
                break;
        }
    }
    return SpawnUnitByArchetype(arch, tileX, tileY, faction);
}

// The stats and looks a unit takes from the character file of its archetype
static void CopyUnitConfig(Unit &u, const UnitConfig &cfg)
{
    u.type = cfg.type;
    u.maxHp = cfg.maxHp;
    u.attackDamage = cfg.attackDamage;
    u.attackRange = cfg.attackRange;
    u.attackCooldown = cfg.attackCooldown;
    u.attackCooldownMax = cfg.attackCooldownMax;
    u.burstCount = cfg.burstCount;
    u.burstDelay = cfg.burstDelay;
    u.splashPercent = cfg.splashPercent;
    u.projectile = cfg.projectile;
    u.moveSpeed = cfg.moveSpeed;
    u.sightRadius = cfg.sightRadius;
    u.hasRotatingTurret = cfg.hasRotatingTurret;
    u.muzzleOffsetX = cfg.muzzleOffsetX;
    u.muzzleOffsetY = cfg.muzzleOffsetY;
    u.turretOffsetX = cfg.turretOffsetX;
    u.turretOffsetY = cfg.turretOffsetY;
    u.hasCustomTurretOffset = cfg.hasCustomTurretOffset;
    u.spreadAngle = cfg.spreadAngle;
    u.turnStepDelay = cfg.turnStepDelay;
    u.turnTimer = 0.0f;
    u.turretTurnStepDelay = cfg.turretTurnStepDelay;
    u.turretTurnTimer = 0.0f;
    u.wreckType = cfg.wreckType;
    u.wreckComposite = cfg.wreckComposite;
    u.wreckStages = cfg.wreckStages;
    u.moveFrameTime = cfg.moveFrameTime;
    u.attackFrameTime = cfg.attackFrameTime;
    u.attackPoseTime = cfg.attackPoseTime;
    u.burstPoseTime = cfg.burstPoseTime;
    u.flashMode = cfg.flashMode;
    u.flashFrames = cfg.flashFrames;
    u.deathSequences = cfg.deathSequences;
    u.flameDeathSequences = cfg.flameDeathSequences;
    u.bulletSpeed = cfg.bulletSpeed;
    // weapons[1]: a second gun beside the main one
    u.hasSecondary = false;
    if (cfg.weapons.size() > 1 && cfg.weapons[1].enabled && cfg.weapons[1].damage > 0)
    {
        const WeaponConfig &w = cfg.weapons[1];
        u.hasSecondary = true;
        u.secondaryProjectile = w.projectile;
        u.secondaryDamage = w.damage;
        u.secondaryRange = w.range;
        u.secondaryMinRange = w.minRange;
        u.secondaryCooldown = std::max(0.1f, w.cooldown);
        u.secondarySplash = w.splashPercent;
        u.secondaryMuzzleX = w.muzzleOffsetX;
        u.secondaryMuzzleY = w.muzzleOffsetY;
    }
}

int EntityManager::SpawnUnitByArchetype(int archetypeId, int tileX, int tileY, Faction faction, PlayerId owner)
{
    if (m_entities.size() >= MAX_UNITS)
        return 0;

    archetypeId = std::clamp(archetypeId, 0, 18);

    auto u = std::make_unique<Unit>();
    u->id = m_nextId++;
    u->archetypeId = archetypeId;
    u->faction = faction;
    u->owner = owner == kAutoOwner ? Players::OwnerOf(faction) : owner;
    u->tileX = tileX;
    u->tileY = tileY;
    u->subX = 0.0f;
    u->subY = 0.0f;
    u->state = ActionState::Idle;
    u->facingDirection = (faction == Faction::Resistance) ? 0 : 4;
    u->turretFacing = u->facingDirection;
    u->isSiegeMode = (archetypeId >= 14 && archetypeId <= 16);

    const UnitConfig *cfg = GetUnitConfig(archetypeId);
    if (cfg)
    {
        CopyUnitConfig(*u, *cfg);
    }
    else
    {
        switch (archetypeId)
        {
            case 0:
                u->type = UnitType::ResistanceRifleman;
                u->maxHp = 90;
                u->attackDamage = 12;
                u->attackRange = 4.0f;
                u->attackCooldown = 0.7f;
                u->moveSpeed = 1.4f;
                u->sightRadius = 5;
                break;
            case 1:
                u->type = UnitType::ConfederationRifleman;
                u->maxHp = 90;
                u->attackDamage = 12;
                u->attackRange = 4.0f;
                u->attackCooldown = 0.7f;
                u->moveSpeed = 1.4f;
                u->sightRadius = 5;
                break;
            case 2:
                u->type = UnitType::Grenadier;
                u->maxHp = 120;
                u->attackDamage = 30;
                u->attackRange = 5.0f;
                u->attackCooldown = 1.4f;
                u->moveSpeed = 1.15f;
                u->sightRadius = 5;
                break;
            case 3:
                u->type = UnitType::HeavyAssault;
                u->maxHp = 120;
                u->attackDamage = 32;
                u->attackRange = 5.2f;
                u->attackCooldown = 1.4f;
                u->moveSpeed = 1.15f;
                u->sightRadius = 5;
                break;
            case 4:
                u->type = UnitType::Sniper;
                u->maxHp = 150;
                u->attackDamage = 55;
                u->attackRange = 7.5f;
                u->attackCooldown = 2.0f;
                u->moveSpeed = 1.3f;
                u->sightRadius = 8;
                break;
            case 5:
                u->type = UnitType::Flamethrower;
                u->maxHp = 210;
                u->attackDamage = 40;
                u->attackRange = 3.2f;
                u->attackCooldown = 0.9f;
                u->moveSpeed = 1.25f;
                u->sightRadius = 5;
                break;
            case 6:
                u->type = UnitType::Coyote;
                u->maxHp = 210;
                u->attackDamage = 22;
                u->attackRange = 5.0f;
                u->attackCooldown = 0.6f;
                u->moveSpeed = 2.4f;
                u->sightRadius = 7;
                break;
            case 7:
            case 18:
                u->type = UnitType::Fortress;
                u->maxHp = 480;
                u->attackDamage = (archetypeId == 7 ? 55 : 42);
                u->attackRange = 5.0f;
                u->attackCooldown = 1.1f;
                u->moveSpeed = 1.4f;
                u->sightRadius = 6;
                break;
            case 8:
                u->type = UnitType::Armadillo;
                u->maxHp = 380;
                u->attackDamage = 42;
                u->attackRange = 5.5f;
                u->attackCooldown = 1.3f;
                u->moveSpeed = 1.6f;
                u->sightRadius = 6;
                break;
            case 9:
                u->type = UnitType::Hammer;
                u->maxHp = 360;
                u->attackDamage = 45;
                u->attackRange = 5.5f;
                u->attackCooldown = 1.3f;
                u->moveSpeed = 1.65f;
                u->sightRadius = 6;
                break;
            case 10:
                u->type = UnitType::Rhino;
                u->maxHp = 520;
                u->attackDamage = 65;
                u->attackRange = 6.0f;
                u->attackCooldown = 1.7f;
                u->moveSpeed = 1.35f;
                u->sightRadius = 7;
                break;
            case 11:
            case 17:
                u->type = UnitType::Zeus;
                u->maxHp = 580;
                u->attackDamage = (archetypeId == 11 ? 85 : 65);
                u->attackRange = 6.5f;
                u->attackCooldown = 1.7f;
                u->moveSpeed = 1.3f;
                u->sightRadius = 7;
                break;
            case 12:
                u->type = UnitType::Porcupine;
                u->maxHp = 340;
                u->attackDamage = 80;
                u->attackRange = 8.5f;
                u->attackCooldown = 2.4f;
                u->moveSpeed = 1.4f;
                u->sightRadius = 8;
                break;
            case 13:
                u->type = UnitType::Grad;
                u->maxHp = 340;
                u->attackDamage = 75;
                u->attackRange = 8.0f;
                u->attackCooldown = 2.4f;
                u->moveSpeed = 1.35f;
                u->sightRadius = 8;
                break;
            case 14:
                u->type = UnitType::Sniper;
                u->maxHp = 150;
                u->attackDamage = 85;
                u->attackRange = 9.5f;
                u->attackCooldown = 1.8f;
                u->moveSpeed = 0.0f;
                u->sightRadius = 10;
                break;
            case 15:
                u->type = UnitType::Rhino;
                u->maxHp = 520;
                u->attackDamage = 110;
                u->attackRange = 9.5f;
                u->attackCooldown = 2.2f;
                u->moveSpeed = 0.0f;
                u->sightRadius = 9;
                break;
            case 16:
                u->type = UnitType::Grad;
                u->maxHp = 340;
                u->attackDamage = 120;
                u->attackRange = 11.0f;
                u->attackCooldown = 2.6f;
                u->moveSpeed = 0.0f;
                u->sightRadius = 9;
                break;
            default:
                u->maxHp = 100;
                break;
        }
        u->turnStepDelay = u->IsInfantry() ? 0.0f : Utility::DefaultTurnStepDelay(archetypeId);
        u->turnTimer = 0.0f;
        u->turretTurnStepDelay = u->IsInfantry() ? 0.0f : Utility::DefaultTurretTurnStepDelay(archetypeId);
        u->turretTurnTimer = 0.0f;
        if (u->IsInfantry())
        {
            u->wreckType = "infantry";
            u->deathSequences = DefaultDeaths(false);
            u->flameDeathSequences = DefaultDeaths(true);
        }
        else
        {
            u->wreckType = "vehicle";
            const EffectsConfig::Wreck wreck = EffectsConfig::Get().WreckOfVehicle(archetypeId);
            u->wreckComposite = wreck.composite;
            u->wreckStages = wreck.stages;
        }
    }
    u->currentHp = u->maxHp;

    int newId = u->id;
    m_entities.push_back(std::move(u));
    return newId;
}

bool EntityManager::UpgradeUnitForm(int unitId, int newArchetype)
{
    Unit *u = GetUnit(unitId);
    const UnitConfig *cfg = GetUnitConfig(newArchetype);
    if (!u || !u->alive || !cfg || u->archetypeId == newArchetype)
        return false;
    const int oldMax = std::max(1, u->maxHp);
    const int oldHp = u->currentHp;
    u->archetypeId = newArchetype;
    CopyUnitConfig(*u, *cfg);
    u->currentHp = std::max(1, static_cast<int>(static_cast<std::int64_t>(oldHp) * u->maxHp / oldMax));
    u->turnTimer = 0.0f;
    u->turretTurnTimer = 0.0f;
    ApplyCompletedResearch(*u); // the technologies of the player apply to the new form too
    return true;
}

int EntityManager::SpawnBuilding(UnitType type, int tileX, int tileY, Faction faction)
{
    int bIdx = 0;
    switch (type)
    {
        case UnitType::Headquarters:
            bIdx = (faction == Faction::Resistance ? 8 : 0);
            break;
        case UnitType::PowerPlant:
            bIdx = (faction == Faction::Resistance ? 9 : 1);
            break;
        case UnitType::Barracks:
            bIdx = (faction == Faction::Resistance ? 10 : 2);
            break;
        case UnitType::Factory:
            bIdx = (faction == Faction::Resistance ? 11 : 3);
            break;
        case UnitType::ResearchLab:
            bIdx = (faction == Faction::Resistance ? 12 : 4);
            break;
        case UnitType::Bunker:
            bIdx = (faction == Faction::Resistance ? 13 : 5);
            break;
        case UnitType::Tower:
            bIdx = (faction == Faction::Resistance ? 14 : 7);
            break;
        default:
            bIdx = (faction == Faction::Resistance ? 8 : 0);
            break;
    }
    return SpawnBuildingByIndex(bIdx, tileX, tileY, faction, 1.0f);
}

int EntityManager::SpawnBuildingByIndex(
    int buildingIdx, int tileX, int tileY, Faction faction, float initialProgress, PlayerId owner)
{
    if (m_entities.size() >= MAX_UNITS)
        return 0;

    static const std::uint8_t s_widths[15] = {4, 3, 3, 4, 3, 2, 2, 2, 3, 3, 3, 3, 3, 2, 2};
    static const std::uint8_t s_heights[15] = {4, 3, 3, 3, 3, 2, 2, 2, 4, 3, 3, 3, 3, 2, 2};

    auto b = std::make_unique<Building>();
    b->id = m_nextId++;
    b->faction = faction;
    b->owner = owner == kAutoOwner ? Players::OwnerOf(faction) : owner;
    b->tileX = tileX;
    b->tileY = tileY;
    b->subX = 0.0f;
    b->subY = 0.0f;
    b->isPowered = true;
    b->state = ActionState::Idle;
    b->sightRadius = 7;
    b->rawBuildingType = buildingIdx;
    b->powerOutput = BuildingPower(buildingIdx);
    b->buildProgress = initialProgress;
    b->attackDamage = 0;

    int idxClamp = std::clamp(buildingIdx, 0, 14);
    b->buildWidth = s_widths[idxClamp];
    b->buildHeight = s_heights[idxClamp];

    const BuildingConfig *bcfg = GetBuildingConfig(buildingIdx);
    if (bcfg)
    {
        b->type = bcfg->type;
        b->maxHp = bcfg->maxHp;
        b->sightRadius = bcfg->sightRadius;
        b->radarRange = bcfg->radarRange;
        b->attackDamage = bcfg->attackDamage;
        b->attackRange = bcfg->attackRange;
        b->attackCooldown = bcfg->attackCooldown;
        b->attackCooldownMax = bcfg->attackCooldownMax;
        b->burstCount = bcfg->burstCount;
        b->burstDelay = bcfg->burstDelay;
        b->bulletSpeed = bcfg->bulletSpeed;
        b->splashPercent = bcfg->splashPercent;
        b->projectile = bcfg->projectile;
        b->buildWidth = bcfg->width;
        b->buildHeight = bcfg->height;
        b->muzzleOffsetX = bcfg->muzzleOffsetX;
        b->muzzleOffsetY = bcfg->muzzleOffsetY;
        b->turretOffsetX = bcfg->turretOffsetX;
        b->turretOffsetY = bcfg->turretOffsetY;
        b->hasCustomTurretOffset = bcfg->hasCustomTurretOffset;
        b->hasRotatingTurret = bcfg->hasRotatingTurret;
        b->wreckComposite = bcfg->wreckComposite;
        b->wreckStages = bcfg->wreckStages;
        b->spreadAngle = bcfg->spreadAngle;
    }
    else
    {
        switch (idxClamp)
        {
            case 0:
                b->type = UnitType::Headquarters;
                b->maxHp = 2500;
                b->sightRadius = 8;
                break;
            case 1:
                b->type = UnitType::PowerPlant;
                b->maxHp = 1200;
                break;
            case 2:
                b->type = UnitType::Barracks;
                b->maxHp = 1200;
                break;
            case 3:
                b->type = UnitType::Factory;
                b->maxHp = 1500;
                break;
            case 4:
                b->type = UnitType::ResearchLab;
                b->maxHp = 1000;
                break;
            case 5:
                b->type = UnitType::Bunker;
                b->maxHp = 2500;
                b->attackDamage = 30;
                b->attackRange = 6.0f;
                b->attackCooldown = 0.7f;
                break;
            case 6:
                b->type = UnitType::Tower;
                b->maxHp = 1200;
                b->attackDamage = 0;
                b->sightRadius = 11;
                break;
            case 7:
                b->type = UnitType::Bunker;
                b->maxHp = 1600;
                b->attackDamage = 65;
                b->attackRange = 8.5f;
                b->attackCooldown = 1.3f;
                b->sightRadius = 9;
                break;
            case 8:
                b->type = UnitType::Headquarters;
                b->maxHp = 2500;
                b->attackDamage = 18;
                b->attackRange = 5.5f;
                b->attackCooldown = 0.8f;
                b->sightRadius = 8;
                break;
            case 9:
                b->type = UnitType::PowerPlant;
                b->maxHp = 1200;
                break;
            case 10:
                b->type = UnitType::Barracks;
                b->maxHp = 1500;
                b->attackDamage = 15;
                b->attackRange = 5.0f;
                b->attackCooldown = 0.9f;
                break;
            case 11:
                b->type = UnitType::Factory;
                b->maxHp = 1200;
                break;
            case 12:
                b->type = UnitType::ResearchLab;
                b->maxHp = 1200;
                b->attackDamage = 15;
                b->attackRange = 5.0f;
                b->attackCooldown = 0.9f;
                break;
            case 13:
                b->type = UnitType::Bunker;
                b->maxHp = 1800;
                b->attackDamage = 32;
                b->attackRange = 6.0f;
                b->attackCooldown = 0.65f;
                break;
            case 14:
                b->type = UnitType::Tower;
                b->maxHp = 1600;
                b->attackDamage = 40;
                b->attackRange = 7.5f;
                b->attackCooldown = 0.85f;
                b->sightRadius = 9;
                break;
            default:
                b->type = UnitType::Headquarters;
                b->maxHp = 1000;
                break;
        }
    }

    if (initialProgress < 1.0f)
    {
        b->currentHp = std::max(50, static_cast<int>(b->maxHp * 0.10f));
    }
    else
    {
        b->currentHp = b->maxHp;
    }

    int newId = b->id;
    m_entities.push_back(std::move(b));
    return newId;
}

bool EntityManager::CanPlaceBuilding(
    int tileX, int tileY, int width, int height, const TileMap &map, PlayerId player, const char **reason) const
{
    auto reject = [&](const char *why) {
        if (reason)
            *reason = why;
        return false;
    };
    if (reason)
        *reason = nullptr;
    if (tileX < 0 || tileX + width > map.GetWidth() || tileY < 0 || tileY + height > map.GetHeight())
    {
        return reject("bounds");
    }

    // 1. Fog of War check: building can only be placed on Visible tiles, NOT on Fog
    // (the same for every player, human or computer)
    if (map.IsFogEnabled())
    {
        const std::uint8_t team = Players::TeamOf(player);
        for (int ty = tileY; ty < tileY + height; ++ty)
        {
            for (int tx = tileX; tx < tileX + width; ++tx)
            {
                if (!map.IsVisibleTo(tx, ty, team))
                {
                    return reject("visibility");
                }
            }
        }
    }

    // 2. Without a finished headquarters nothing is built (no building, no wall, no new headquarters); with one,
    //    the place must be within build_radius of ANY friendly finished Headquarters
    bool inHQRange = false;
    float buildCenterX = tileX + width * 0.5f;
    float buildCenterY = tileY + height * 0.5f;

    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner != player)
            continue;
        const Building *b = e.AsBuilding();
        // A headquarters still under construction opens no building zone yet
        if (b && b->type == UnitType::Headquarters && b->buildProgress >= 1.0f)
        {
            float hqCenterX = b->tileX + b->buildWidth * 0.5f;
            float hqCenterY = b->tileY + b->buildHeight * 0.5f;
            float dist = DetMath::Hypot(buildCenterX - hqCenterX, buildCenterY - hqCenterY);
            if (dist <= static_cast<float>(GetBuildRadius(player)))
            {
                inHQRange = true;
                break;
            }
        }
    }
    if (!inHQRange)
    {
        return reject("HQ radius");
    }

    // 3. No construction under the enemy's nose: a visible enemy unit or building near the footprint blocks it
    const int block = map.GetConstructionRules().enemyBlockTiles;
    if (block > 0)
    {
        for (const auto &e : m_entities)
        {
            if (!e.alive || e.state == ActionState::Die || e.owner >= kMaxPlayers || !Players::Hostile(e.owner, player))
                continue;
            int ex0 = e.tileX, ey0 = e.tileY, ex1 = e.tileX, ey1 = e.tileY;
            if (const Building *b = e.AsBuilding())
            {
                ex1 = b->tileX + b->buildWidth - 1;
                ey1 = b->tileY + b->buildHeight - 1;
            }
            const int gapX = std::max({0, ex0 - (tileX + width - 1), tileX - ex1});
            const int gapY = std::max({0, ey0 - (tileY + height - 1), tileY - ey1});
            if (std::max(gapX, gapY) > block)
                continue;
            if (IsEntityVisibleToPlayer(e, player, map))
                return reject("nearby enemy");
        }
    }

    for (int ty = tileY; ty < tileY + height; ++ty)
    {
        for (int tx = tileX; tx < tileX + width; ++tx)
        {
            if (!map.IsPassable(tx, ty))
                return reject("terrain");
            if (map.GetWall(tx, ty) > 0)
                return reject("wall");
            for (const auto &e : m_entities)
            {
                if (!e.alive || e.state == ActionState::Die)
                    continue;
                if (const Building *b = e.AsBuilding())
                {
                    if (tx >= b->tileX && tx < b->tileX + b->buildWidth && ty >= b->tileY
                        && ty < b->tileY + b->buildHeight)
                    {
                        return reject("building");
                    }
                }
                else
                {
                    if (e.tileX == tx && e.tileY == ty)
                    {
                        return reject("unit");
                    }
                }
            }
        }
    }
    if (!Players::Slot(player).human)
        return KeepsBaseOpen(tileX, tileY, width, height, map, player);
    return true;
}

bool EntityManager::KeepsBaseOpen(
    int tileX, int tileY, int width, int height, const TileMap &map, PlayerId player) const
{
    const int W = map.GetWidth(), H = map.GetHeight();
    std::vector<std::uint8_t> blocked(static_cast<std::size_t>(W) * H, 0);
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            if (!map.IsPassable(x, y) || map.GetWall(x, y) > 0)
                blocked[static_cast<std::size_t>(y) * W + x] = 1;
        }
    }
    // Where the units of the player come out: the headquarters and every unit producer
    std::vector<std::pair<int, int>> anchors;
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding)
            continue;
        const Building *b = e.AsBuilding();
        if (!b)
            continue;
        for (int y = b->tileY; y < b->tileY + b->buildHeight; ++y)
        {
            for (int x = b->tileX; x < b->tileX + b->buildWidth; ++x)
            {
                if (x >= 0 && y >= 0 && x < W && y < H)
                    blocked[static_cast<std::size_t>(y) * W + x] = 1;
            }
        }
        if (b->owner == player && (b->type == UnitType::Headquarters || b->IsUnitProducer()))
        {
            int sx = 0, sy = 0;
            if (GetBuildingSpawnPoint(b->id, map, sx, sy) && sx >= 0 && sy >= 0 && sx < W && sy < H)
                anchors.push_back({sx, sy});
        }
    }
    if (anchors.empty())
        return true;
    const auto idx = [&](int x, int y) {
        return static_cast<std::size_t>(y) * W + x;
    };
    // A producer's exit stands on a free tile; the first anchor (the oldest building) is the one that is followed
    if (blocked[idx(anchors[0].first, anchors[0].second)])
        return true; // nothing to measure from

    auto flood = [&](const std::vector<std::uint8_t> &block, std::vector<std::uint8_t> &seen) {
        seen.assign(block.size(), 0);
        std::vector<int> stack {anchors[0].second * W + anchors[0].first};
        seen[stack[0]] = 1;
        int count = 0;
        while (!stack.empty())
        {
            const int cur = stack.back();
            stack.pop_back();
            ++count;
            const int x = cur % W, y = cur / W;
            const int nx[4] = {x + 1, x - 1, x, x}, ny[4] = {y, y, y + 1, y - 1};
            for (int k = 0; k < 4; ++k)
            {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= W || ny[k] >= H)
                    continue;
                const std::size_t n = idx(nx[k], ny[k]);
                if (block[n] || seen[n])
                    continue;
                seen[n] = 1;
                stack.push_back(ny[k] * W + nx[k]);
            }
        }
        return count;
    };

    std::vector<std::uint8_t> before, after;
    const int reachBefore = flood(blocked, before);
    std::vector<std::uint8_t> withNew = blocked;
    int footprintReached = 0;
    for (int y = tileY; y < tileY + height; ++y)
    {
        for (int x = tileX; x < tileX + width; ++x)
        {
            if (x < 0 || y < 0 || x >= W || y >= H)
                continue;
            if (before[idx(x, y)])
                ++footprintReached;
            withNew[idx(x, y)] = 1;
        }
    }
    if (withNew[idx(anchors[0].first, anchors[0].second)])
        return false;
    const int reachAfter = flood(withNew, after);

    // A few tiles may end up in dead corners behind the building; a whole area cut off is a closed base
    constexpr int kDeadCornerTiles = 6;
    if (reachBefore - reachAfter - footprintReached > kDeadCornerTiles)
        return false;
    for (const auto &a : anchors)
    {
        if (before[idx(a.first, a.second)] && !after[idx(a.first, a.second)])
            return false;
    }
    // The new building has a free, reachable side of its own (its exit)
    bool hasExit = false;
    for (int x = tileX - 1; x <= tileX + width && !hasExit; ++x)
    {
        for (int y = tileY - 1; y <= tileY + height && !hasExit; ++y)
        {
            const bool inside = x >= tileX && x < tileX + width && y >= tileY && y < tileY + height;
            if (inside || x < 0 || y < 0 || x >= W || y >= H)
                continue;
            hasExit = after[idx(x, y)] != 0;
        }
    }
    return hasExit;
}

} // namespace OpenArtOfWar
