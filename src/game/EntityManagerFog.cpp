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

void EntityManager::UpdateFogOfWar(TileMap &map)
{
    if (!map.IsFogEnabled())
        return;

    // Reset currently visible tiles to Explored (dimmed darkness shroud)
    map.ResetVisibility();

    // Enemies that just hit something of a side: exactly the tiles they occupy become visible to it
    for (const auto &p : m_revealPings)
    {
        const Entity *e = GetEntity(p.entityId);
        if (!e)
            continue;
        if (const Building *b = e->AsBuilding())
        {
            map.RevealRect(b->tileX, b->tileY, b->buildWidth, b->buildHeight, Players::TeamOf(p.observer));
        }
        else
        {
            map.RevealRect(e->tileX, e->tileY, 1, 1, Players::TeamOf(p.observer));
        }
    }

    // Every team sees around the living units and buildings of its players (the same on every peer)
    for (const auto &entity : m_entities)
    {
        if (!entity.alive || entity.state == ActionState::Die)
            continue;
        if (entity.owner >= kMaxPlayers)
            continue;
        const std::uint8_t team = Players::TeamOf(entity.owner);

        int sight = entity.sightRadius;
        if (sight <= 0)
            sight = 4;
        // A finished Radar Locator sees far beyond its sight radius
        if (const Building *rb = entity.AsBuilding(); rb && rb->radarRange > 0 && rb->buildProgress >= 1.0f)
            sight += rb->radarRange;

        if (const Building *b = entity.AsBuilding())
        {
            int cx = b->tileX + (b->buildWidth / 2);
            int cy = b->tileY + (b->buildHeight / 2);
            map.RevealArea(cx, cy, sight, team);
        }
        else if (const Unit *u = entity.AsUnit())
        {
            map.RevealArea(u->tileX, u->tileY, sight, team);
        }
    }
}

bool EntityManager::IsEntityVisibleToAi(const Entity &entity) const
{
    return IsEntityVisibleToAi(entity, Players::OwnerOf(AiFaction()));
}

bool EntityManager::IsEntityVisibleToAi(const Entity &entity, PlayerId observerPlayer) const
{
    float ex = entity.GetUnitCenterPixelX();
    float ey = entity.GetUnitCenterPixelY();
    for (const auto &observer : m_entities)
    {
        if (!observer.alive || observer.state == ActionState::Die || observer.owner != observerPlayer)
            continue;
        float ox = observer.GetUnitCenterPixelX();
        float oy = observer.GetUnitCenterPixelY();
        float sightPx = static_cast<float>(observer.sightRadius * TILE_WIDTH);
        float dx = ex - ox;
        float dy = ey - oy;
        if ((dx * dx + dy * dy) <= (sightPx * sightPx))
        {
            return true;
        }
    }
    return false;
}

bool EntityManager::IsEntityVisibleToPlayer(const Entity &entity, PlayerId observer, const TileMap &map) const
{
    if (!entity.alive || entity.state == ActionState::Die)
        return false;
    if (!map.IsFogEnabled())
        return true;
    if (!Players::Hostile(entity.owner, observer))
        return true;

    int w = 1;
    int h = 1;
    if (entity.isBuilding)
    {
        if (const Building *b = entity.AsBuilding())
        {
            w = std::max(1, b->buildWidth);
            h = std::max(1, b->buildHeight);
        }
    }

    if (observer >= kMaxPlayers)
        return true;
    if (Players::Slot(observer).human)
    {
        // Visible if ANY tile of the entity's WxH footprint is in the team's sight this tick
        const std::uint8_t team = Players::TeamOf(observer);
        for (int dy = 0; dy < h; ++dy)
        {
            for (int dx = 0; dx < w; ++dx)
            {
                if (map.IsVisibleTo(entity.tileX + dx, entity.tileY + dy, team))
                    return true;
            }
        }
        return false;
    }
    return IsEntityVisibleToAi(entity, observer);
}

} // namespace OpenArtOfWar
