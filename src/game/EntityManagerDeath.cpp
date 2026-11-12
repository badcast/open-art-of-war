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

// One hit effect for every bullet that hits something - infantry, vehicles, buildings alike:
// the impact sparks (55..58, variant picked by the target). No blood.
static int BulletImpactAnim(int targetId)
{
    return 55 + (targetId % 4);
}

int EntityManager::DemolishBuilding(int buildingId, TileMap &map)
{
    Building *b = GetBuilding(buildingId);
    if (!b || !b->alive || b->state == ActionState::Die)
        return 0;
    int refund = 0;
    for (const auto &item : b->productionQueue)
        refund += item.cost;
    b->productionQueue.clear();
    b->isProducing = false;
    b->currentHp = 0;
    b->state = ActionState::Die;
    b->animFrame = 0;
    b->animTimer = 0.0f;
    b->selected = false;
    b->deathTimer = EffectsConfig::Get().burialSeconds;
    for (int ty = b->tileY; ty < b->tileY + b->buildHeight; ++ty)
    {
        for (int tx = b->tileX; tx < b->tileX + b->buildWidth; ++tx)
        {
            map.SetPassable(tx, ty, true);
        }
    }
    SpawnDeathExplosions(*b);
    return refund;
}

int EntityManager::ValueOf(const Entity &e) const
{
    if (const Unit *u = e.AsUnit())
    {
        const UnitConfig *cfg = GetUnitConfig(u->archetypeId);
        return cfg ? cfg->creditCost : 0;
    }
    if (const Building *b = e.AsBuilding())
    {
        const BuildingConfig *cfg = GetBuildingConfig(b->rawBuildingType);
        return cfg ? cfg->creditCost : 0;
    }
    return 0;
}

void EntityManager::NoteStats(const Entity &victim, PlayerId killer)
{
    const int value = ValueOf(victim);
    if (victim.owner < kMaxPlayers)
    {
        PlayerStats &lost = m_stats[victim.owner];
        (victim.isBuilding ? lost.buildingsLost : lost.unitsLost) += 1;
        lost.valueLost += value;
    }
    if (killer < kMaxPlayers && Players::Hostile(victim.owner, killer))
    {
        PlayerStats &won = m_stats[killer];
        (victim.isBuilding ? won.buildingsDestroyed : won.unitsKilled) += 1;
        won.valueDestroyed += value;
    }
}

void EntityManager::NoteKill(const Entity &victim, PlayerId killer)
{
    NoteKill(victim.owner, killer);
    NoteStats(victim, killer);
}

void EntityManager::KillEntity(Entity &e, TileMap &map)
{
    if (!e.alive || e.state == ActionState::Die)
        return;
    e.currentHp = 0;
    e.state = ActionState::Die;
    e.animFrame = 0;
    e.animTimer = 0.0f;
    e.selected = false;
    e.deathTimer = EffectsConfig::Get().burialSeconds;
    if (Building *b = e.AsBuilding())
    {
        b->productionQueue.clear();
        b->isProducing = false;
        for (int ty = b->tileY; ty < b->tileY + b->buildHeight; ++ty)
        {
            for (int tx = b->tileX; tx < b->tileX + b->buildWidth; ++tx)
                map.SetPassable(tx, ty, true);
        }
        SpawnDeathExplosions(*b);
    }
    else if (Unit *u = e.AsUnit())
    {
        HandleUnitDeathState(u, false);
        if (u->IsInfantry())
            SpawnExplosion(u->GetUnitCenterPixelX(), u->GetUnitCenterPixelY(), BulletImpactAnim(u->id));
        else
            SpawnDeathExplosions(*u);
    }
}

void EntityManager::DestroyPlayer(PlayerId player, TileMap &map)
{
    if (player >= kMaxPlayers)
        return;
    for (Entity &e : m_entities)
    {
        if (e.owner == player)
            KillEntity(e, map);
    }
    // The walls of the player's base go too
    for (int y = 0; y < map.GetHeight(); ++y)
    {
        for (int x = 0; x < map.GetWidth(); ++x)
        {
            if (map.GetWall(x, y) > 0 && map.GetWallOwner(x, y) == player)
            {
                map.DestroyWall(x, y);
                static const CachedSprite collapse("fx_impacts.wall_destroyed", SpriteKind::Anim);
                SpawnExplosion(x * TILE_WIDTH + TILE_WIDTH * 0.5f, y * TILE_HEIGHT + TILE_HEIGHT * 0.5f, collapse);
            }
        }
    }
    map.ForgetPlayerWalls(player);
    m_mines.erase(
        std::remove_if(m_mines.begin(), m_mines.end(), [player](const Mine &m) { return m.owner == player; }),
        m_mines.end());
}

std::vector<PlayerId> EntityManager::UpdateElimination(TileMap &map)
{
    std::vector<PlayerId> out;
    std::array<bool, kMaxPlayers> hasBuilding {};
    for (const Entity &e : m_entities)
    {
        if (e.alive && e.state != ActionState::Die && e.isBuilding && e.owner < kMaxPlayers)
            hasBuilding[e.owner] = true;
    }
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        if (!Players::IsActive(p) || m_eliminated[p] || hasBuilding[p])
            continue;
        m_eliminated[p] = true;
        DestroyPlayer(p, map);
        out.push_back(p);
    }
    return out;
}

void EntityManager::SpawnDeathExplosions(const Entity &e)
{
    const EffectsConfig &fx = EffectsConfig::Get();
    const float cx = e.GetUnitCenterPixelX();
    const float cy = e.GetUnitCenterPixelY();
    auto pick = [](const std::vector<int> &list, int i) {
        return list.empty() ? -1 : list[static_cast<std::size_t>(i) % list.size()];
    };
    if (const Building *b = e.AsBuilding())
    {
        int main = fx.buildingLargeMain;
        if (b->buildWidth <= 2 && b->buildHeight <= 2)
        {
            main = fx.buildingSmallMain;
        }
        else if (b->rawBuildingType == 2 || b->rawBuildingType == 4 || b->rawBuildingType == 12)
        {
            main = fx.buildingPowerMain;
        }
        SpawnExplosion(cx, cy, main);
        if (m_buildingBlasts.size() < 64) // nobody takes them (headless, editor)
            m_buildingBlasts.push_back({cx, cy, std::sqrt(static_cast<float>(b->buildWidth * b->buildHeight) / 9.0f)});
        if (m_collectEvents && m_eventBlasts.size() < 64)
            m_eventBlasts.push_back({cx, cy, std::sqrt(static_cast<float>(b->buildWidth * b->buildHeight) / 9.0f)});
        // The rest of the structure goes up over the next half second, spread over its whole footprint
        const int extra = std::clamp(b->buildWidth * b->buildHeight / 2, fx.buildingExtraMin, fx.buildingExtraMax);
        for (int i = 0; i < extra; ++i)
        {
            const float x = (b->tileX + Utility::VisualRandom(0.2f, b->buildWidth - 0.2f)) * TILE_WIDTH;
            const float y = (b->tileY + Utility::VisualRandom(0.2f, b->buildHeight - 0.2f)) * TILE_HEIGHT;
            const int anim = (i % fx.buildingBigEvery == 0) ? fx.buildingBigAnim
                                                            : pick(fx.buildingExtraAnims, Utility::VisualRandom(0, 3));
            SpawnExplosion(
                x,
                y,
                anim,
                fx.buildingDelayStart + fx.buildingDelayStep * static_cast<float>(i)
                    + Utility::VisualRandom(0.0f, fx.buildingDelayJitter));
        }
        return;
    }
    // Vehicle: the blast, then the ammunition / fuel a moment later
    SpawnExplosion(cx, cy, pick(fx.vehicleMain, e.id));
    SpawnExplosion(
        cx + Utility::VisualRandom(-fx.vehicleSecondSpreadX, fx.vehicleSecondSpreadX),
        cy + Utility::VisualRandom(fx.vehicleSecondSpreadYMin, fx.vehicleSecondSpreadYMax),
        pick(fx.vehicleSecond, e.id + 2),
        Utility::VisualRandom(fx.vehicleSecondDelayMin, fx.vehicleSecondDelayMax));
}

void EntityManager::SpawnExplosion(float worldX, float worldY, int animId, float delay)
{
    if (animId < 0)
        return;
    if (m_collectEvents && m_eventExplosions.size() < 512)
        m_eventExplosions.push_back({worldX, worldY, animId, delay});
    ExplosionParticle exp;
    exp.x = worldX;
    exp.y = worldY;
    exp.animId = animId;
    exp.delay = delay;
    exp.currentFrame = 0;
    exp.frameTimer = 0.0f;
    exp.active = true;

    if (m_texManager)
    {
        IndexRange r = m_texManager->GetAnimRange(animId);
        if (r.end >= r.start)
        {
            exp.totalFrames = (r.end - r.start) + 1;
        }
        else
        {
            exp.totalFrames = 6;
        }
    }
    else
    {
        exp.totalFrames = 6;
    }

    m_explosions.push_back(exp);
}

} // namespace OpenArtOfWar
