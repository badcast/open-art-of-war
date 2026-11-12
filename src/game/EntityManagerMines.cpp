#include "game/EntityManager.hpp"
#include "EntityManagerInternal.hpp"
#include "core/DetMath.hpp"
#include "game/Mines.hpp"
#include "game/Research.hpp"
#include "game/TileMap.hpp"
#include "graphics/Renderer.hpp"

#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <cmath>

namespace OpenArtOfWar
{

int EntityManager::CountMines(PlayerId owner, int kind) const
{
    int n = 0;
    for (const Mine &m : m_mines)
        n += (m.owner == owner && m.kind == kind) ? 1 : 0;
    return n;
}

bool EntityManager::CanPlaceMine(int kind, int tileX, int tileY, const TileMap &map, PlayerId player) const
{
    const std::vector<MineSpec> &specs = Mines::Specs();
    if (kind < 0 || kind >= static_cast<int>(specs.size()) || player >= kMaxPlayers)
        return false;
    if (CountMines(player, kind) >= specs[static_cast<std::size_t>(kind)].maxPerPlayer)
        return false;
    for (const Mine &m : m_mines)
    {
        if (m.tileX == tileX && m.tileY == tileY)
            return false;
    }
    if (tileX < 0 || tileY < 0 || tileX >= map.GetWidth() || tileY >= map.GetHeight() || map.GetWall(tileX, tileY) > 0)
        return false;
    // The rules of a wall segment: within the building radius, on ground the player sees, not under the enemy's nose
    // (the walls' "keep the base open" test does not matter for a mine)
    return CanPlaceBuilding(tileX, tileY, 1, 1, map, player, nullptr, false);
}

bool EntityManager::PlaceMine(int kind, int tileX, int tileY, const TileMap &map, PlayerId player)
{
    if (!CanPlaceMine(kind, tileX, tileY, map, player))
        return false;
    Mine m;
    m.id = m_nextMineId++;
    m.owner = player;
    m.tileX = tileX;
    m.tileY = tileY;
    m.kind = kind;
    m.armLeft = Mines::Specs()[static_cast<std::size_t>(kind)].armSeconds;
    m_mines.push_back(m);
    return true;
}

void EntityManager::UpdateMines(float deltaTime, TileMap &map)
{
    if (m_mines.empty())
        return;
    const std::vector<MineSpec> &specs = Mines::Specs();
    std::vector<std::size_t> fired;
    for (std::size_t i = 0; i < m_mines.size(); ++i)
    {
        Mine &m = m_mines[i];
        if (m.kind < 0 || m.kind >= static_cast<int>(specs.size()))
        {
            fired.push_back(i); // its kind is gone (another data set): the mine goes quietly
            continue;
        }
        if (m.armLeft > 0.0f)
        {
            m.armLeft -= deltaTime;
            continue;
        }
        const MineSpec &spec = specs[static_cast<std::size_t>(m.kind)];
        const float cx = m.tileX + 0.5f, cy = m.tileY + 0.5f;
        for (const Entity &e : m_entities)
        {
            const Unit *u = e.AsUnit();
            if (!u || !u->alive || u->state == ActionState::Die || !Players::Hostile(u->owner, m.owner))
                continue;
            const bool infantry = u->IsInfantry();
            if ((spec.trigger == MineSpec::Trigger::Infantry && !infantry)
                || (spec.trigger == MineSpec::Trigger::Vehicle && infantry))
                continue;
            const float dx = u->GetUnitCenterPixelX() / TILE_WIDTH - cx;
            const float dy = u->GetUnitCenterPixelY() / TILE_HEIGHT - cy;
            if (DetMath::Hypot(dx, dy) <= spec.triggerRadius)
            {
                fired.push_back(i);
                break;
            }
        }
    }
    if (fired.empty())
        return;

    // Blow them up in the order they were laid; then take them out of the list
    for (std::size_t idx : fired)
    {
        const Mine m = m_mines[idx];
        if (m.kind < 0 || m.kind >= static_cast<int>(specs.size()))
            continue;
        const MineSpec &spec = specs[static_cast<std::size_t>(m.kind)];
        const float cx = m.tileX + 0.5f, cy = m.tileY + 0.5f;
        SpawnExplosion(cx * TILE_WIDTH, cy * TILE_HEIGHT, spec.explosion);
        Cue(SoundCue::ExplodeSmall, cx * TILE_WIDTH, cy * TILE_HEIGHT, m.owner);
        for (Entity &other : m_entities)
        {
            Unit *u = other.AsUnit();
            if (!u || !u->alive || u->state == ActionState::Die || !Players::Hostile(u->owner, m.owner))
                continue;
            const float dx = u->GetUnitCenterPixelX() / TILE_WIDTH - cx;
            const float dy = u->GetUnitCenterPixelY() / TILE_HEIGHT - cy;
            if (DetMath::Hypot(dx, dy) > spec.blastRadius)
                continue;
            u->currentHp -= ApplyMovingUnitDamage(spec.damage, u, ProjectileType::Shell);
            if (u->currentHp <= 0)
            {
                KillEntity(*u, map);
                NoteStats(*u, m.owner);
            }
        }
    }
    std::vector<bool> gone(m_mines.size(), false);
    for (std::size_t idx : fired)
        gone[idx] = true;
    std::size_t keep = 0;
    for (std::size_t i = 0; i < m_mines.size(); ++i)
    {
        if (!gone[i])
            m_mines[keep++] = m_mines[i];
    }
    m_mines.resize(keep);
}

void EntityManager::RenderMines(Renderer &renderer, const Camera &camera, const TileMap &map, PlayerId viewer) const
{
    const std::vector<MineSpec> &specs = Mines::Specs();
    const bool blink = (SDL_GetTicks() / 400) % 2 == 0; // only a picture: never part of the simulation
    for (const Mine &m : m_mines)
    {
        if (viewer < kMaxPlayers && Players::Hostile(m.owner, viewer))
            continue; // the other side never sees them
        const float x = static_cast<float>(m.tileX * TILE_WIDTH), y = static_cast<float>(m.tileY * TILE_HEIGHT);
        const float w = static_cast<float>(TILE_WIDTH), h = static_cast<float>(TILE_HEIGHT);
        const bool heavy = m.kind >= 0 && m.kind < static_cast<int>(specs.size())
                           && specs[static_cast<std::size_t>(m.kind)].trigger == MineSpec::Trigger::Vehicle;
        const float pad = heavy ? 4.0f : 7.0f;
        renderer.FillRectWorld(x + pad, y + pad, w - 2 * pad, h - 2 * pad, camera, {28, 30, 26, 235});
        renderer.DrawRectWorld(x + pad, y + pad, w - 2 * pad, h - 2 * pad, camera, {150, 150, 120, 255});
        const bool armed = m.armLeft <= 0.0f;
        const SDL_Color led = !armed ? SDL_Color {230, 190, 40, 255}
                                     : (blink ? SDL_Color {240, 50, 40, 255} : SDL_Color {90, 20, 20, 255});
        renderer.FillRectWorld(x + w * 0.5f - 2.0f, y + h * 0.5f - 2.0f, 4.0f, 4.0f, camera, led);
    }
    (void) map;
}

} // namespace OpenArtOfWar
