#include "graphics/RenderUtility.hpp"
#include "game/AIController.hpp"
#include "AIControllerInternal.hpp"
#include "game/BuildLimits.hpp"
#include "core/DetMath.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/TileMap.hpp"
#include "game/Pathfinder.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <sstream>
#include <cmath>

namespace OpenArtOfWar
{

void AIController::RenderDebug(
    Renderer &renderer, BitmapFontRenderer &font, const Camera &camera, const EntityManager &entities) const
{
    if (m_debugConfig.visualizeChokepoints && m_chokes.Width() > 0)
    {
        float x0, y0, x1, y1;
        camera.ScreenToWorld(0.0f, 0.0f, x0, y0);
        camera.ScreenToWorld(
            static_cast<float>(camera.GetViewportWidth()), static_cast<float>(camera.GetViewportHeight()), x1, y1);
        int tx0 = std::max(0, static_cast<int>(x0 / TILE_WIDTH));
        int ty0 = std::max(0, static_cast<int>(y0 / TILE_HEIGHT));
        int tx1 = std::min(m_chokes.Width() - 1, static_cast<int>(x1 / TILE_WIDTH) + 1);
        int ty1 = std::min(m_chokes.Height() - 1, static_cast<int>(y1 / TILE_HEIGHT) + 1);
        for (int ty = ty0; ty <= ty1; ++ty)
        {
            for (int tx = tx0; tx <= tx1; ++tx)
            {
                if (!m_chokes.IsChoke(tx, ty))
                    continue;
                renderer.FillRectWorld(
                    tx * TILE_WIDTH + 1.0f,
                    ty * TILE_HEIGHT + 1.0f,
                    TILE_WIDTH - 2.0f,
                    TILE_HEIGHT - 2.0f,
                    camera,
                    {255, 140, 30, 70});
            }
        }
    }

    if (m_debugConfig.visualizeSquadLeash)
    {
        // Staging area marker
        RenderUtility::DrawTileRadius(
            renderer,
            camera,
            m_stagingX * TILE_WIDTH + TILE_WIDTH * 0.5f,
            m_stagingY * TILE_HEIGHT + TILE_HEIGHT * 0.5f,
            7.0f,
            SDL_Color {120, 200, 255, 90},
            1.5f);
        m_squadController.RenderDebug(renderer, font, camera, entities, m_currentDifficultyConfig.tactics.leashTiles);
    }

    // Director summary in the top-left corner of the map view
    std::string line1 = "AI " + DifficultyToString(GetEffectiveDifficulty())
                        + (m_debugConfig.forceDebugOverride ? " (debug override)" : "") + "  |  credits "
                        + std::to_string(m_credits);
    std::string line2
        = "Wave " + std::to_string(m_currentPlan.waveNumber) + " " + TierName(m_currentPlan.tier) + ": ready "
          + std::to_string(m_readyUnits) + "/" + std::to_string(m_currentPlan.archetypes.size())
          + (m_waveCooldown > 0.0f ? "  next in " + std::to_string(static_cast<int>(m_waveCooldown)) + "s" : "")
          + "  |  groups " + std::to_string(m_squadController.GetSquads().size());
    std::string line3 = "Research: ";
    if (m_researchIndex >= kResearchCount)
    {
        line3 += "complete";
    }
    else
    {
        line3 += std::string(researchDef(side(), m_researchIndex).name)
                 + (m_researchActive ? " " + std::to_string(static_cast<int>(GetResearchProgress() * 100.0f)) + "%"
                                     : (m_hasLab ? " (saving credits)" : " (no lab)"));
    }
    int y = 40;
    for (const std::string *l : {&line1, &line2, &line3})
    {
        int w = font.GetTextWidth(*l, 1) + 12;
        renderer.FillRectScreen(6, y, w, 18, {10, 12, 16, 200});
        font.DrawString(*l, 12, y + 2, 1, {226, 231, 238, 255}, false);
        y += 20;
    }
}

void AIController::OnEntityAttacked(int victimId, int attackerId, EntityManager &entities, Pathfinder &pathfinder)
{
    Entity *victim = entities.GetEntity(victimId);
    Entity *attacker = entities.GetEntity(attackerId);
    if (!victim || !attacker || victim->owner != m_player)
        return;

    if (victim->targetEntityId == 0)
    {
        victim->targetEntityId = attackerId;
    }

    // Call for help: alert nearby allies within 14 tiles
    std::vector<int> allies;
    for (auto &ally : entities.GetAllEntities())
    {
        if (!ally.alive || ally.state == ActionState::Die || ally.isBuilding || ally.owner != m_player)
            continue;
        // Members of a squad are not called away: their squad leads them (it fights when attacked on the way)
        if (m_squadController.IsSquadMember(ally.id))
            continue;
        if (ally.state == ActionState::Idle && ally.targetEntityId == 0)
        {
            float dx = ally.GetUnitCenterPixelX() - victim->GetUnitCenterPixelX();
            float dy = ally.GetUnitCenterPixelY() - victim->GetUnitCenterPixelY();
            if (std::sqrt(dx * dx + dy * dy) <= 14.0f * TILE_WIDTH)
            {
                allies.push_back(ally.id);
            }
        }
    }
    if (!allies.empty())
    {
        entities.IssueAttackMoveOrder(allies, attacker->tileX, attacker->tileY, pathfinder);
    }
}

void AIController::OnStructureAttacked(int structureId, int attackerId, EntityManager &entities, Pathfinder &pathfinder)
{
    m_baseQuietSeconds = 0.0f; // no new headquarters while the base is under fire
    OnEntityAttacked(structureId, attackerId, entities, pathfinder);
}

void AIController::RecordStartingBase(const EntityManager &entities, const TileMap &map)
{
    m_startingBuildings.clear();
    m_startingWalls.clear();

    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->owner != m_player)
            continue;

        AIStructurePlan plan;
        plan.type = b->type;
        plan.rawBuildingType = b->rawBuildingType;
        plan.tileX = b->tileX;
        plan.tileY = b->tileY;
        plan.width = b->buildWidth;
        plan.height = b->buildHeight;
        plan.destroyed = false;
        plan.rebuildTimer = 0.0f;

        // The price is the one in the character file of the building
        const BuildingConfig *bcfg = entities.GetBuildingConfig(b->rawBuildingType);
        plan.creditCost = bcfg ? bcfg->creditCost : 250;

        m_startingBuildings.push_back(plan);
    }

    // Record all walls of the AI side
    for (int y = 0; y < map.GetHeight(); ++y)
    {
        for (int x = 0; x < map.GetWidth(); ++x)
        {
            if (map.GetWall(x, y) > 0 && map.GetWallOwner(x, y) == m_player)
            {
                AIWallPlan wp;
                wp.tileX = x;
                wp.tileY = y;
                wp.faction = side();
                {
                    const BuildingConfig *wcfg = entities.GetBuildingConfig(side() == Faction::Confederation ? 15 : 25);
                    wp.creditCost = wcfg ? wcfg->creditCost : 25;
                }
                wp.destroyed = false;
                wp.rebuildTimer = 0.0f;
                m_startingWalls.push_back(wp);
            }
        }
    }

    m_startingBaseRecorded = true;
    std::cout << "[AIController] Recorded starting base: " << m_startingBuildings.size() << " structures, "
              << m_startingWalls.size() << " walls for rebuilding memory." << std::endl;
}

void AIController::UpdateRebuilding(float deltaTime, TileMap &map, EntityManager &entities)
{
    if (!m_startingBaseRecorded)
    {
        RecordStartingBase(entities, map);
        return;
    }

    // Without a finished headquarters nothing is built, exactly as for a person (CanPlaceBuilding says so too)
    const bool canBuild = entities.HasBuildingType(UnitType::Headquarters, m_player, true);

    // 1. Check building status & rebuild destroyed ones in the exact same location
    for (auto &plan : m_startingBuildings)
    {
        if (!plan.destroyed)
        {
            bool stillAlive = false;
            for (const auto &e : entities.GetAllEntities())
            {
                if (e.alive && e.state != ActionState::Die && e.isBuilding && e.owner == m_player
                    && e.tileX == plan.tileX && e.tileY == plan.tileY)
                {
                    stillAlive = true;
                    break;
                }
            }
            if (!stillAlive)
            {
                plan.destroyed = true;
                plan.rebuildTimer = 0.0f;
                Think(
                    "BUILD",
                    "Structure at (" + std::to_string(plan.tileX) + ", " + std::to_string(plan.tileY)
                        + ") destroyed: queued for reconstruction");
            }
        }
        else
        {
            // One laboratory is enough: the core build order may have put up another one elsewhere meanwhile
            if (plan.type == UnitType::ResearchLab && entities.HasBuildingType(UnitType::ResearchLab, m_player, false))
                continue;
            plan.rebuildTimer += deltaTime;
            if (canBuild && plan.rebuildTimer >= 6.0f && m_credits >= plan.creditCost
                && IsBuildingUnlocked(m_player, plan.rawBuildingType)
                // (hard limits: the destroyed one is rebuilt only while the player may have another)
                && BuildLimits::CanBuildMore(entities, map, m_player, plan.rawBuildingType))
            {
                bool siteClear
                    = entities.CanPlaceBuilding(plan.tileX, plan.tileY, plan.width, plan.height, map, m_player);
                // Foresight as for a new building: one that takes power waits for the plants (rebuilt first)
                const int power = entities.BuildingPower(plan.rawBuildingType);
                if (siteClear && power < 0 && PowerBalance(entities, false) + power < 0)
                {
                    m_powerNeeded = std::max(m_powerNeeded, -power);
                    ThinkOnce("rebuildpower", "BUILD", "Rebuilding waits for power: a power plant first");
                    siteClear = false;
                }
                if (siteClear)
                {
                    m_credits -= plan.creditCost;
                    int newId = entities.SpawnBuildingByIndex(
                        plan.rawBuildingType, plan.tileX, plan.tileY, side(), 0.0f, m_player);
                    if (newId > 0)
                    {
                        for (int ty = plan.tileY; ty < plan.tileY + plan.height && ty < map.GetHeight(); ++ty)
                        {
                            for (int tx = plan.tileX; tx < plan.tileX + plan.width && tx < map.GetWidth(); ++tx)
                            {
                                map.SetPassable(tx, ty, false);
                            }
                        }
                        plan.destroyed = false;
                        plan.rebuildTimer = 0.0f;
                        Think(
                            "BUILD",
                            "Rebuilt the structure at (" + std::to_string(plan.tileX) + ", "
                                + std::to_string(plan.tileY) + ") for $" + std::to_string(plan.creditCost));
                    }
                }
            }
        }
    }

    // 2. Check wall status & rebuild destroyed walls in the exact same location
    for (auto &wp : m_startingWalls)
    {
        if (!wp.destroyed)
        {
            if (map.GetWall(wp.tileX, wp.tileY) == 0)
            {
                wp.destroyed = true;
                wp.rebuildTimer = 0.0f;
            }
        }
        else
        {
            wp.rebuildTimer += deltaTime;
            if (canBuild && wp.rebuildTimer >= 8.0f && m_credits >= wp.creditCost
                && BuildLimits::CanBuildMore(
                    entities, map, m_player, wp.faction == Faction::Confederation ? 15 : 25)) // (hard limits)
            {
                // Not on a unit that stands even partly on the tile (it would be walled in)
                if (!entities.IsTileOccupiedByUnit(wp.tileX, wp.tileY))
                {
                    m_credits -= wp.creditCost;
                    map.SetWall(wp.tileX, wp.tileY, wp.faction, 100, m_player);
                    wp.destroyed = false;
                    wp.rebuildTimer = 0.0f;
                    ThinkOnce(
                        "wall",
                        "BUILD",
                        "Patching the wall at (" + std::to_string(wp.tileX) + ", " + std::to_string(wp.tileY) + ")");
                }
            }
        }
    }
}

} // namespace OpenArtOfWar
