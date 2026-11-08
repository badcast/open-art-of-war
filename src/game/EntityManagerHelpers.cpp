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

// The infantry deaths every infantry character falls back to
std::vector<int> DefaultDeaths(bool flame)
{
    const char *names[2]
        = {flame ? "fx_infantry_death.flame_0" : "fx_infantry_death.death_0",
           flame ? "fx_infantry_death.flame_1" : "fx_infantry_death.death_1"};
    std::vector<int> out;
    for (const char *n : names)
    {
        const int h = SpriteHandle(n, SpriteKind::Sequence, "infantry deaths");
        if (h >= 0)
            out.push_back(h);
    }
    return out;
}

void HandleUnitDeathState(Unit *tu, bool isFlameDamage)
{
    if (!tu)
        return;
    tu->path.clear();
    tu->deathTimer = EffectsConfig::Get().burialSeconds;
    tu->animFrame = 0;
    tu->animTimer = 0.0f;
    if (isFlameDamage || tu->killedByFlame)
    {
        tu->killedByFlame = true;
        if (!tu->flameDeathSequences.empty())
        {
            tu->selectedDeathSeq = Utility::VisualPick(tu->flameDeathSequences);
        }
        else
        {
            tu->selectedDeathSeq = SpriteHandle(
                (tu->archetypeId % 2 == 0) ? "fx_infantry_death.flame_0" : "fx_infantry_death.flame_1",
                SpriteKind::Sequence);
        }
    }
    else if (tu->IsInfantry())
    {
        if (!tu->deathSequences.empty())
        {
            tu->selectedDeathSeq = Utility::VisualPick(tu->deathSequences);
        }
        else
        {
            tu->selectedDeathSeq = SpriteHandle(
                Utility::VisualRandom(0, 1) ? "fx_infantry_death.death_1" : "fx_infantry_death.death_0",
                SpriteKind::Sequence);
        }
    }
}

float ComputeDistanceToEntityPixels(const Unit &attacker, const Entity &target)
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

} // namespace OpenArtOfWar
