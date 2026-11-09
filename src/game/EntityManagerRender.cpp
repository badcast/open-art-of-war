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

void EntityManager::AddGroundMark(float x, float y, int composite, float seconds)
{
    if (composite <= 0 || seconds <= 0.0f)
        return;
    constexpr std::size_t kMaxMarks = 400; // the oldest go first
    if (m_groundMarks.size() >= kMaxMarks)
        m_groundMarks.erase(m_groundMarks.begin());
    m_groundMarks.push_back({x, y, composite, seconds});
}

void EntityManager::RenderGroundRemnants(
    Renderer &renderer, const Camera &camera, const TileMap &map, bool fogEnabled) const
{
    // Craters of shells and rockets: under everything else, only where the player can see the ground
    for (const GroundMark &m : m_groundMarks)
    {
        if (fogEnabled
            && map.GetFogState(static_cast<int>(m.x / TILE_WIDTH), static_cast<int>(m.y / TILE_HEIGHT))
                   == FogState::Hidden)
            continue;
        renderer.DrawCompositeWorld(m.composite, m.x - 90.0f, m.y - 60.0f, camera, Faction::Resistance);
    }
    for (const auto &entity : m_entities)
    {
        if (!entity.alive || entity.state != ActionState::Die)
            continue;
        if (fogEnabled && Players::Hostile(entity.owner, Players::Local()))
        {
            if (map.GetFogState(entity.tileX, entity.tileY) == FogState::Hidden)
                continue;
        }
        // Wrecks and rubble replace the destroyed object from the middle of its explosion (elapsed >= 0.25s);
        // until then the building / vehicle is still standing there in its battered state
        float elapsedDeath = EffectsConfig::Get().burialSeconds - entity.deathTimer;
        const bool infantry = entity.AsUnit() && entity.AsUnit()->IsInfantry();
        if (elapsedDeath < 0.25f && !infantry)
        {
            if (const Building *b = entity.AsBuilding())
            {
                const int btype = (b->rawBuildingType >= 0) ? b->rawBuildingType : (static_cast<int>(b->type) % 15);
                for (int layer = 0; layer < 2; ++layer)
                {
                    renderer.SetDrawOwner(b->owner);
                    renderer.DrawBuildingWorld(
                        btype,
                        b->tileX * TILE_WIDTH,
                        b->tileY * TILE_HEIGHT,
                        layer,
                        camera,
                        b->faction,
                        0,
                        1.0f,
                        false,
                        0.0f,
                        b->turretFacing,
                        false,
                        false);
                }
            }
            else if (const Unit *u = entity.AsUnit())
            {
                renderer.SetDrawOwner(u->owner);
                renderer.DrawUnitWorld(
                    u->archetypeId,
                    static_cast<int>(ActionState::Idle),
                    u->facingDirection,
                    0,
                    u->GetWorldPixelX(),
                    u->GetWorldPixelY(),
                    camera,
                    u->faction,
                    u->turretFacing,
                    false);
            }
            continue;
        }

        // After the burial time the remains fade out: linear transparency over fade_seconds of effects.yaml
        {
            const EffectsConfig &fx = EffectsConfig::Get();
            const float over = -entity.deathTimer;
            renderer.SetDrawAlpha(
                over > 0.0f && fx.fadeSeconds > 0.0f ? std::clamp(1.0f - over / fx.fadeSeconds, 0.0f, 1.0f) : 1.0f);
        }
        if (const Building *b = entity.AsBuilding())
        {
            int btype = (b->rawBuildingType >= 0) ? b->rawBuildingType : (static_cast<int>(b->type) % 15);
            float bx = b->tileX * TILE_WIDTH;
            float by = b->tileY * TILE_HEIGHT;
            // The rubble sinks into the ground stage by stage over the burial time the remains stay
            int wreckComp = 0, stages = 0;
            Renderer::DefaultBuildingWreck(b->buildWidth, b->buildHeight, wreckComp, stages);
            if (b->wreckStages > 0)
                stages = b->wreckStages;
            const float burial = EffectsConfig::Get().burialSeconds;
            const int stage
                = std::min(stages - 1, static_cast<int>(std::clamp(elapsedDeath, 0.0f, burial) / burial * stages));
            renderer.SetDrawOwner(b->owner);
            renderer.DrawBuildingWorld(
                btype,
                bx,
                by,
                0,
                camera,
                b->faction,
                stage,
                1.0f,
                true,
                1.0f,
                0,
                false,
                true,
                b->wreckComposite,
                b->wreckStages);
        }
        else if (const Unit *u = entity.AsUnit())
        {
            renderer.SetDrawOwner(u->owner);
            renderer.DrawUnitWorld(
                u->archetypeId,
                static_cast<int>(ActionState::Die),
                u->facingDirection,
                u->animFrame,
                u->GetWorldPixelX(),
                u->GetWorldPixelY(),
                camera,
                u->faction,
                u->turretFacing,
                u->killedByFlame,
                u->wreckComposite,
                u->wreckStages,
                u->selectedDeathSeq);
        }
        renderer.SetDrawAlpha(1.0f);
    }
    renderer.SetDrawOwner(kNoPlayer);
}

bool EntityManager::IsHostileBuildingShown(const Building &b, const TileMap &map, bool *outInSight) const
{
    bool inSight = false;
    for (int ty = b.tileY; ty < b.tileY + b.buildHeight && !inSight; ++ty)
    {
        for (int tx = b.tileX; tx < b.tileX + b.buildWidth && !inSight; ++tx)
        {
            inSight = map.GetFogState(tx, ty) == FogState::Visible;
        }
    }
    const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.id)) << 32)
                              | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(b.tileY)) << 16)
                              | static_cast<std::uint16_t>(b.tileX);
    if (inSight)
        m_seenBuildings.insert(key);
    if (outInSight)
        *outInSight = inSight;
    return inSight || (b.buildProgress >= 1.0f && m_seenBuildings.count(key) > 0);
}

namespace
{
// Bubble colour of a selected object by its health: green, yellow, red
SDL_Color SelectionHealthColor(float ratio)
{
    const SDL_Color green {80, 255, 130, 255}, yellow {255, 220, 70, 255}, red {255, 70, 60, 255};
    auto mix = [](SDL_Color a, SDL_Color b, float t) {
        return SDL_Color {
            static_cast<Uint8>(a.r + (b.r - a.r) * t),
            static_cast<Uint8>(a.g + (b.g - a.g) * t),
            static_cast<Uint8>(a.b + (b.b - a.b) * t),
            255};
    };
    return ratio >= 0.5f ? mix(yellow, green, (ratio - 0.5f) * 2.0f) : mix(red, yellow, ratio * 2.0f);
}
} // namespace

namespace
{
// A jump farther than this in one tick is no movement (unloaded from a transport, spawned, a slot reused):
// it is shown where it is now instead of sliding there
constexpr float kMaxRenderGlide = 2.0f * TILE_WIDTH;

bool Glides(float fromX, float fromY, float toX, float toY)
{
    const float dx = toX - fromX, dy = toY - fromY;
    return dx * dx + dy * dy <= kMaxRenderGlide * kMaxRenderGlide;
}
} // namespace

void EntityManager::SnapshotRenderState()
{
    for (auto &entity : m_entities)
    {
        if (!entity.AsUnit())
            continue; // buildings never move
        entity.prevRenderX = entity.GetWorldPixelX();
        entity.prevRenderY = entity.GetWorldPixelY();
        entity.hasPrevRender = true;
    }
    for (auto &proj : m_projectiles)
    {
        proj.prevRenderX = proj.x;
        proj.prevRenderY = proj.y;
        proj.hasPrevRender = true;
    }
}

void EntityManager::RenderPosition(const Entity &entity, float &x, float &y) const
{
    x = entity.GetWorldPixelX();
    y = entity.GetWorldPixelY();
    if (entity.hasPrevRender && m_renderAlpha < 1.0f && Glides(entity.prevRenderX, entity.prevRenderY, x, y))
    {
        x = entity.prevRenderX + (x - entity.prevRenderX) * m_renderAlpha;
        y = entity.prevRenderY + (y - entity.prevRenderY) * m_renderAlpha;
    }
}

void EntityManager::Render(
    Renderer &renderer,
    const Camera &camera,
    const TileMap &map,
    bool fogEnabled,
    int globalAnimTick,
    bool debugMode,
    bool showAllHealth)
{
    float startWorldX, startWorldY, endWorldX, endWorldY;
    camera.ScreenToWorld(0, 0, startWorldX, startWorldY);
    camera.ScreenToWorld(camera.GetViewportWidth(), camera.GetViewportHeight(), endWorldX, endWorldY);
    int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, map.GetWidth() - 1);
    int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, map.GetWidth());
    int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, map.GetHeight() - 1);
    int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, map.GetHeight());

    // Depth order: everything is drawn by the bottom edge of what stands on the ground (a unit's tile, a
    // building's last footprint row, a wall row), so whatever reaches above its own tiles - the top of a
    // headquarters, a tall wall segment - covers units behind it, and units in front are drawn over it.
    // Health / progress bars come after all of it.
    struct DepthItem
    {
        float footY;
        int order; // equal foot: walls, then buildings and tree crowns, then units
        int row;   // wall row
        const Unit *unit;
        const Building *building;
        bool live;                            // building currently in sight (else drawn still, under the fog shading)
        const MapDecoration *decor = nullptr; // the crown of a tree / rock (its ground layer went with the terrain)
    };
    std::vector<DepthItem> items;
    items.reserve(m_entities.size() + static_cast<std::size_t>(std::max(0, endTileY - startTileY)));
    for (int ty = startTileY; ty < endTileY; ++ty)
    {
        items.push_back({static_cast<float>((ty + 1) * TILE_HEIGHT), 0, ty, nullptr, nullptr, true});
    }
    const float viewTop = startTileY * static_cast<float>(TILE_HEIGHT);
    const float viewBottom = (endTileY + 1) * static_cast<float>(TILE_HEIGHT);
    for (const auto &entity : m_entities)
    {
        if (!entity.alive || entity.state == ActionState::Die)
            continue;
        if (const Unit *u = entity.AsUnit())
        {
            if (fogEnabled && Players::Hostile(u->owner, Players::Local())
                && map.GetFogState(u->tileX, u->tileY) != FogState::Visible)
            {
                continue;
            }
            float shownX, shownY;
            RenderPosition(*u, shownX, shownY);
            const float foot = shownY + TILE_HEIGHT;
            if (foot < viewTop || foot > viewBottom + TILE_HEIGHT)
                continue;
            items.push_back({foot, 2, 0, u, nullptr, true});
        }
        else if (const Building *b = entity.AsBuilding())
        {
            // Enemy structures: in sight -> live; seen before and finished -> drawn still, the fog shading goes on top;
            // never seen, or still under construction out of sight -> hidden (no peeking through the fog)
            bool live = true;
            if (fogEnabled && Players::Hostile(b->owner, Players::Local()))
            {
                if (!IsHostileBuildingShown(*b, map, &live))
                    continue;
            }
            items.push_back({static_cast<float>((b->tileY + b->buildHeight) * TILE_HEIGHT), 1, 0, nullptr, b, live});
        }
    }
    // Crowns of trees and rocks, by the bottom row of their footprint (same view and fog rules as the terrain pass)
    for (const MapDecoration &dec : map.GetDecorations())
    {
        if (dec.x < startTileX - 4 || dec.x > endTileX + 4 || dec.y < startTileY - 4 || dec.y > endTileY + 4)
            continue;
        if (fogEnabled && map.GetFogState(dec.x, dec.y) == FogState::Hidden)
            continue;
        const int rows = renderer.TallDecorRows(map.GetTilesetId(), dec.type);
        if (rows <= 0)
            continue;
        DepthItem item {static_cast<float>((dec.y + rows) * TILE_HEIGHT), 1, 0, nullptr, nullptr, true};
        item.decor = &dec;
        items.push_back(item);
    }
    std::stable_sort(items.begin(), items.end(), [](const DepthItem &l, const DepthItem &r) {
        return l.footY != r.footY ? l.footY < r.footY : l.order < r.order;
    });

    for (const DepthItem &it : items)
    {
        if (it.order == 0)
        {
            map.RenderWallRow(it.row, renderer, camera, fogEnabled, globalAnimTick, startTileX, endTileX);
        }
        else if (const MapDecoration *dec = it.decor)
        {
            renderer.SetDrawOwner(kNoPlayer);
            renderer.DrawDecorWorld(
                map.GetTilesetId(), dec->type, dec->x * TILE_WIDTH, dec->y * TILE_HEIGHT, camera, globalAnimTick, 1);
        }
        else if (const Unit *u = it.unit)
        {
            float wx, wy;
            RenderPosition(*u, wx, wy);
            if (u->selected)
            {
                renderer.SetDrawOwner(kNoPlayer);
                // A glowing bubble at the feet (health as its colour) instead of the ring
                const float ratio
                    = u->maxHp > 0 ? std::clamp(static_cast<float>(u->currentHp) / u->maxHp, 0.0f, 1.0f) : 1.0f;
                const float rx = u->IsInfantry() ? TILE_WIDTH * 0.80f : TILE_WIDTH * 1.05f;
                renderer.DrawSelectionBubble(
                    wx + TILE_WIDTH * 0.5f,
                    wy + TILE_HEIGHT * 0.75f,
                    rx,
                    rx * 0.55f,
                    camera,
                    Players::Hostile(u->owner, Players::Local()) ? SDL_Color {240, 80, 70, 255}
                                                                 : SelectionHealthColor(ratio),
                    0.0f);
                if (u->isSiegeMode || u->state == ActionState::Transform)
                {
                    static const CachedSprite marker("ui_siege_marker.marker", SpriteKind::Part);
                    renderer.DrawCompositeWorld(marker, wx - 90.0f, wy - 60.0f, camera, u->faction);
                }
            }
            bool isFlaming
                = (u->state == ActionState::Die) ? u->killedByFlame : (u->projectile == ProjectileType::Flame);
            renderer.SetDrawOwner(u->owner);
            renderer.DrawUnitWorld(
                u->archetypeId,
                // Waiting for its group: stands, so it is drawn standing
                static_cast<int>(
                    u->groupHold && u->state == ActionState::Move && !u->HasReservation() ? ActionState::Idle
                                                                                          : u->state),
                u->facingDirection,
                u->animFrame,
                wx,
                wy,
                camera,
                u->faction,
                u->turretFacing,
                isFlaming,
                u->wreckComposite,
                u->wreckStages,
                u->selectedDeathSeq,
                u->flashMode,
                u->flashFrames);
        }
        else if (const Building *b = it.building)
        {
            const bool live = it.live;
            int btype = (b->rawBuildingType >= 0) ? b->rawBuildingType : (static_cast<int>(b->type) % 15);
            float bx = b->tileX * TILE_WIDTH;
            float by = b->tileY * TILE_HEIGHT;
            bool isProdBuilding = (btype == 2 || btype == 3 || btype == 10 || btype == 11);
            bool isResearchBuilding = (btype == 4 || btype == 12);
            int bAnim = (!live || (isProdBuilding && !b->isProducing) || (isResearchBuilding && !b->isResearching))
                            ? 0
                            : (globalAnimTick + b->id);
            // Work animations appear only while the building works: the Resistance Factory smokes while it builds a
            // vehicle, and the Confederation Tech Center animates screen lights while its owner researches;
            // otherwise those animated parts are not drawn at all.
            // Note: The Resistance Research Lab (btype == 12) radar head is a physical structure part (anim sequence 6,
            // whose frame 0 is the resting radar dish). It must remain visible in IDLE state (bAnim == 0), stopping
            // rotation, rather than being hidden completely.
            bool workAnimation = true;
            if (btype == 11)
                workAnimation = live && b->isProducing && b->buildProgress >= 1.0f;
            else if (btype == 4)
                workAnimation = live && b->isResearching;
            renderer.SetAnimatedPartsVisible(workAnimation);
            float hpRatio = (b->maxHp > 0) ? (static_cast<float>(b->currentHp) / b->maxHp) : 1.0f;
            bool isAttacking = live && (b->state == ActionState::Attack && b->attackAnimTimer > 0.0f);

            renderer.SetDrawOwner(b->owner);
            if (b->selected && b->buildProgress >= 1.0f)
            {
                // The selected building itself shines, its colour breathing between white and the health colour
                const float t = 0.5f + 0.5f * std::sin(static_cast<float>(SDL_GetTicks() % 100000) * 0.00691f);
                const float hp
                    = b->maxHp > 0 ? std::clamp(static_cast<float>(b->currentHp) / b->maxHp, 0.0f, 1.0f) : 1.0f;
                const SDL_Color hc = Players::Hostile(b->owner, Players::Local())
                                         ? SDL_Color {240, 80, 70, 255}
                                         : SelectionHealthColor(hp); // (an opponent's health is not shown)
                auto mix = [&](std::uint8_t c) {
                    return static_cast<std::uint8_t>(255.0f + (c - 255.0f) * t);
                };
                renderer.SetSpriteGlow({mix(hc.r), mix(hc.g), mix(hc.b), 255}, 0.12f + 0.28f * t);
            }

            // The progress of an enemy construction is not shown: just the scaffolding
            const bool hiddenProgress
                = fogEnabled && b->buildProgress < 1.0f && Players::Hostile(b->owner, Players::Local());
            const float shownProgress = hiddenProgress ? 0.5f : b->buildProgress;
            renderer.DrawBuildingWorld(
                btype,
                bx,
                by,
                0,
                camera,
                b->faction,
                bAnim,
                shownProgress,
                false,
                hpRatio,
                b->turretFacing,
                isAttacking,
                // (an opponent's missing power is not shown: no offline sign, its parts keep working)
                b->isPowered || Players::Hostile(b->owner, Players::Local()));
            renderer.SetDrawOwner(b->owner);
            renderer.DrawBuildingWorld(
                btype,
                bx,
                by,
                1,
                camera,
                b->faction,
                bAnim,
                shownProgress,
                false,
                hpRatio,
                b->turretFacing,
                isAttacking,
                // (an opponent's missing power is not shown: no offline sign, its parts keep working)
                b->isPowered || Players::Hostile(b->owner, Players::Local()));
            renderer.SetAnimatedPartsVisible(true);
            renderer.SetSpriteGlow({255, 255, 255, 255}, 0.0f);
        }
    }

    // Bars on top of every sprite
    for (const DepthItem &it : items)
    {
        // The health and the construction progress of an opponent are not for the player's eyes (not even with Alt)
        if (!debugMode
            && Players::Hostile(
                it.unit ? it.unit->owner : (it.building ? it.building->owner : kNoPlayer), Players::Local()))
            continue;
        if (const Unit *u = it.unit)
        {
            // (the selection bubble shows the health of a selected unit by its colour)
            bool showUnitHp = showAllHealth || (debugMode && (u->currentHp < u->maxHp || u->selected));
            if (showUnitHp)
            {
                float shownX, shownY;
                RenderPosition(*u, shownX, shownY);
                float barW = 24.0f;
                float barH = 3.0f;
                float barX = shownX + (TILE_WIDTH * 0.5f) - barW * 0.5f;
                float barY = shownY - 10.0f;
                float hpPct = std::clamp(static_cast<float>(u->currentHp) / u->maxHp, 0.0f, 1.0f);
                renderer.FillRectWorld(barX, barY, barW, barH, camera, {60, 0, 0, 200});
                renderer.FillRectWorld(barX, barY, barW * hpPct, barH, camera, {0, 230, 0, 255});
            }
            continue;
        }
        const Building *b = it.building;
        if (!b)
            continue;
        if (b->buildProgress < 1.0f)
        {
            // No progress bar over an enemy construction
            if (fogEnabled && Players::Hostile(b->owner, Players::Local()))
                continue;
            float barW = b->buildWidth * TILE_WIDTH;
            float barH = 4.0f;
            float barX = b->tileX * TILE_WIDTH;
            float barY = b->tileY * TILE_HEIGHT - 12.0f;

            float progPct = std::clamp(b->buildProgress, 0.0f, 1.0f);
            renderer.FillRectWorld(barX, barY, barW, barH, camera, {20, 30, 45, 220});
            renderer.FillRectWorld(barX, barY, barW * progPct, barH, camera, {255, 215, 0, 255});
            renderer.DrawRectWorld(barX, barY, barW, barH, camera, {255, 255, 255, 200});

            bool showBuildingHp = showAllHealth || (debugMode && (b->currentHp < b->maxHp || b->selected));
            if (showBuildingHp)
            {
                float hpPct = std::clamp(static_cast<float>(b->currentHp) / b->maxHp, 0.0f, 1.0f);
                renderer.FillRectWorld(barX, barY + 5.0f, barW, 3.0f, camera, {60, 0, 0, 200});
                renderer.FillRectWorld(barX, barY + 5.0f, barW * hpPct, 3.0f, camera, {0, 230, 0, 255});
            }
        }
        else
        {
            bool showBuildingHp = showAllHealth || b->selected || (debugMode && b->currentHp < b->maxHp);
            if (showBuildingHp)
            {
                float barW = b->buildWidth * TILE_WIDTH;
                float barH = b->selected ? 4.0f : 3.0f;
                float barX = b->tileX * TILE_WIDTH;
                float barY = b->tileY * TILE_HEIGHT - (b->selected ? 8.0f : 6.0f);

                float hpPct = std::clamp(static_cast<float>(b->currentHp) / b->maxHp, 0.0f, 1.0f);
                renderer.FillRectWorld(barX, barY, barW, barH, camera, {60, 0, 0, 200});
                renderer.FillRectWorld(
                    barX,
                    barY,
                    barW * hpPct,
                    barH,
                    camera,
                    b->selected ? SelectionHealthColor(hpPct) : SDL_Color {0, 230, 0, 255});
                if (b->selected)
                    renderer.DrawRectWorld(barX, barY, barW, barH, camera, {255, 255, 255, 200});
            }
        }
    }

    // 4. Render Smoke Puffs
    renderer.SetDrawOwner(kNoPlayer);
    for (const auto &puff : m_smokePuffs)
    {
        if (!puff.active)
            continue;
        int frame = static_cast<int>((puff.lifetime / puff.maxLifetime) * 6.0f);
        renderer.DrawSmokePuffWorld(frame, puff.x, puff.y, camera);
    }

    // 5..6. Projectiles and explosions (drawn again by the glow pass of the shader effects)
    RenderEmissive(renderer, camera);
}

void EntityManager::CollectLights(const TileMap &map, bool fog, std::vector<LightSpot> &out) const
{
    using Kind = LightSpot::Kind;
    out.clear();
    auto seen = [&](float x, float y) {
        return !fog
               || map.GetFogState(static_cast<int>(x) / TILE_WIDTH, static_cast<int>(y) / TILE_HEIGHT)
                      == FogState::Visible;
    };
    for (const auto &exp : m_explosions)
    {
        if (!exp.active || exp.delay > 0.0f || !seen(exp.x, exp.y))
            continue;
        LightSpot l;
        l.kind = Kind::Explosion;
        l.x = exp.x;
        l.y = exp.y;
        l.age = exp.totalFrames > 0 ? static_cast<float>(exp.currentFrame) / static_cast<float>(exp.totalFrames) : 0.0f;
        out.push_back(l);
    }
    for (const auto &burst : m_flameBursts)
    {
        if (seen(burst.startX, burst.startY))
            out.push_back({Kind::FlameStart, burst.startX, burst.startY});
        if (seen(burst.targetX, burst.targetY))
            out.push_back({Kind::FlameEnd, burst.targetX, burst.targetY});
    }
    for (const auto &proj : m_projectiles)
    {
        if (proj.active && (proj.type == ProjectileType::Rocket || proj.type == ProjectileType::Shell)
            && seen(proj.x, proj.y))
            out.push_back({Kind::Projectile, proj.x, proj.y});
    }
    for (const auto &flash : m_muzzleFlashes)
    {
        if (!seen(flash.x, flash.y))
            continue;
        LightSpot l;
        l.kind = Kind::MuzzleFlash;
        l.x = flash.x;
        l.y = flash.y;
        l.age = flash.age;
        out.push_back(l);
    }
    // Buildings in sight (one of their tiles is enough: a building hidden in the fog gives itself away by no light)
    for (const auto &ptr : m_entities.Raw())
    {
        const Building *b = ptr ? ptr->AsBuilding() : nullptr;
        if (!b || !b->alive || b->state == ActionState::Die)
            continue;
        bool visible = !fog;
        for (int ty = b->tileY; !visible && ty < b->tileY + b->buildHeight; ++ty)
            for (int tx = b->tileX; !visible && tx < b->tileX + b->buildWidth; ++tx)
                visible = map.GetFogState(tx, ty) == FogState::Visible;
        if (!visible)
            continue;
        LightSpot l;
        l.kind = Kind::Building;
        l.x = static_cast<float>(b->tileX * TILE_WIDTH);
        l.y = static_cast<float>(b->tileY * TILE_HEIGHT);
        l.w = static_cast<float>(b->buildWidth * TILE_WIDTH);
        l.h = static_cast<float>(b->buildHeight * TILE_HEIGHT);
        l.powered = b->isPowered;
        l.complete = b->buildProgress >= 1.0f;
        out.push_back(l);
    }
}

void EntityManager::RenderEmissive(Renderer &renderer, const Camera &camera) const
{
    // Projectiles
    for (const auto &proj : m_projectiles)
    {
        if (!proj.active)
            continue;
        const ProjectileSpec &spec = Projectiles::of(proj.type);
        if (proj.type == ProjectileType::Flame || spec.sprite < 0)
            continue; // a flame is drawn as a FlameBurst below
        float px = proj.x, py = proj.y;
        if (proj.hasPrevRender && m_renderAlpha < 1.0f && Glides(proj.prevRenderX, proj.prevRenderY, px, py))
        {
            px = proj.prevRenderX + (px - proj.prevRenderX) * m_renderAlpha;
            py = proj.prevRenderY + (py - proj.prevRenderY) * m_renderAlpha;
        }
        renderer.SetDrawOwner(proj.owner);
        renderer.DrawProjectileWorld(
            spec.sprite,
            spec.directions == 16 ? proj.direction : proj.dir32,
            px,
            py,
            camera,
            proj.faction,
            spec.directions);
    }

    // Flamethrower jets: the 12 frames of the flame over the jet's time
    for (const auto &burst : m_flameBursts)
    {
        const float angle = DetMath::Atan2(burst.targetY - burst.startY, burst.targetX - burst.startX);
        const float prog = burst.duration > 0.0f ? std::clamp(burst.age / burst.duration, 0.0f, 1.0f) : 1.0f;
        const int flameFrame = std::clamp(static_cast<int>(prog * 11.0f), 0, 11);
        renderer.SetDrawOwner(burst.owner);
        static const CachedSprite jet("fx_flame_jet.jet", SpriteKind::Anim);
        renderer.DrawAnimSequenceWorldRotated(
            jet, flameFrame, burst.startX, burst.startY, angle, camera, burst.faction);
    }

    // 6. Render Explosions (with visual centering for bullet hits and impacts)
    renderer.SetDrawOwner(kNoPlayer);
    for (const auto &exp : m_explosions)
    {
        if (!exp.active || exp.delay > 0.0f)
            continue;
        float ox = 0.0f, oy = 0.0f;
        ExplosionCentering(exp.animId, ox, oy);
        renderer.DrawAnimSequenceWorld(exp.animId, exp.currentFrame, exp.x + ox, exp.y + oy, camera);
    }
}

} // namespace OpenArtOfWar
