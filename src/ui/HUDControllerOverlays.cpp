#include "ui/HUDController.hpp"
#include "HUDControllerInternal.hpp"
#include "graphics/SpriteRegistry.hpp"
#include "audio/SoundRequests.hpp"
#include "game/Players.hpp"
#include "game/BuildLimits.hpp"
#include "ui/UiKit.hpp"
#include "core/Localization.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/ShaderEffects.hpp"
#include "graphics/Camera.hpp"
#include "graphics/TextureManager.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/Research.hpp"
#include "graphics/RenderUtility.hpp"
#include "game/Sides.hpp"
#include <algorithm>
#include <cmath>

namespace OpenArtOfWar
{

namespace
{

// Build radius of a headquarters / command center: purple
const SDL_Color kBuildRadiusColor {160, 80, 230, 90};

} // namespace

bool HUDController::PowerIconBlinkOn()
{
    return (SDL_GetTicks() / 400) % 2 == 0; // 0.4 s on, 0.4 s off
}

void HUDController::RenderPowerAlerts(
    Renderer &renderer, const EntityManager &entities, const Economy &economy, const Camera &camera) const
{
    if (!economy.HasPowerDeficit() || !PowerIconBlinkOn())
        return;
    const float zoom = camera.GetZoom();
    const float scale = std::clamp(zoom, 0.75f, 1.5f);
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->owner != Players::Local() || b->buildProgress < 1.0f
            || b->isPowered)
            continue;
        // Only what consumes power (power plants and power-free buildings keep working)
        const BuildingConfig *cfg = entities.GetBuildingConfig(b->rawBuildingType);
        if (!cfg || cfg->powerCost >= 0)
            continue;
        float sx = 0.0f, sy = 0.0f;
        camera.WorldToScreen((b->tileX + b->buildWidth * 0.5f) * TILE_WIDTH, b->tileY * TILE_HEIGHT, sx, sy);
        // Over the roof: the top of the footprint, a little above
        const int w = static_cast<int>(23 * scale), h = static_cast<int>(19 * scale);
        renderer.DrawRawSpriteScreen(
            PowerIconSprite(),
            static_cast<int>(sx) - w / 2,
            static_cast<int>(sy) - h - static_cast<int>(10 * zoom),
            scale);
    }
}

void HUDController::RenderProductionOverlay(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const EntityManager &entities,
    const TileMap &map,
    const Camera &camera) const
{
    const float zoom = camera.GetZoom();

    // Selected headquarters: the zone where buildings and walls may be placed (purple, 70% transparent, 2 px).
    // The radius is measured in tiles and tiles are wider than tall, so on screen it is an ellipse.
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->selected || !b->alive || b->state == ActionState::Die || b->owner != Players::Local()
            || b->type != UnitType::Headquarters || b->buildProgress < 1.0f)
        {
            continue;
        }
        const float cx = (b->tileX + b->buildWidth * 0.5f) * TILE_WIDTH;
        const float cy = (b->tileY + b->buildHeight * 0.5f) * TILE_HEIGHT;
        const float r = static_cast<float>(entities.GetBuildRadius());
        // Counted in tiles (the placement check is a distance in tile units), so it is an ellipse on screen
        RenderUtility::DrawTileRadius(renderer, camera, cx, cy, r, kBuildRadiusColor, 2.0f);
    }

    // Selected defensive structures: their firing range (own green, enemy red)
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->selected || !b->alive || b->state == ActionState::Die || b->attackRange <= 0.0f
            || b->attackDamage <= 0)
        {
            continue;
        }
        // The attack range, red for everybody: the ring the game really measures the firing distance with
        const SDL_Color c {240, 80, 70, 90};
        RenderUtility::DrawRangeRing(
            renderer, camera, b->GetUnitCenterPixelX(), b->GetUnitCenterPixelY(), b->attackRange, c, 2.0f);
    }

    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->owner != Players::Local() || !b->IsUnitProducer())
        {
            continue;
        }
        float sx = 0.0f, sy = 0.0f;
        camera.WorldToScreen(b->tileX * TILE_WIDTH, b->tileY * TILE_HEIGHT, sx, sy);
        const int w = static_cast<int>(b->buildWidth * TILE_WIDTH * zoom);

        // Training progress above the building, with the number of orders waiting behind it
        if (!b->productionQueue.empty())
        {
            const int barW = std::max(36, w - 16);
            const int bx = static_cast<int>(sx) + (w - barW) / 2;
            const int by = static_cast<int>(sy) - 22;
            renderer.FillRectScreen(bx - 1, by - 1, barW + 2, 8, {10, 12, 16, 220});
            renderer.FillRectScreen(bx, by, static_cast<int>(barW * b->CurrentProductionProgress()), 6, col::Accent);
            const int count = static_cast<int>(b->productionQueue.size());
            std::string label = std::to_string(count);
            const int lw = fontRenderer.GetTextWidth(label, ui::FONT_SMALL_BOLD) + 8;
            renderer.FillRectScreen(bx + barW + 3, by - 5, lw, 16, col::Accent);
            fontRenderer.DrawString(
                label,
                bx + barW + 3 + lw / 2,
                by + 3,
                ui::FONT_SMALL_BOLD,
                col::AccentText,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }

        // Rally line from the exit to the rally point, only while the building is selected
        if (!b->selected)
            continue;
        int rx = 0, ry = 0;
        RallyPointOf(*b, map, rx, ry);
        const float x1 = (b->tileX + b->buildWidth * 0.5f) * TILE_WIDTH;
        const float y1 = (b->tileY + b->buildHeight) * TILE_HEIGHT;
        const float x2 = (rx + 0.5f) * TILE_WIDTH;
        const float y2 = (ry + 0.5f) * TILE_HEIGHT;
        const SDL_Color yellow {255, 220, 40, 150};
        float ax = 0, ay = 0, bx2 = 0, by2 = 0;
        camera.WorldToScreen(x1, y1, ax, ay);
        camera.WorldToScreen(x2, y2, bx2, by2);
        // 2 px wide: two parallel 1 px lines offset across the line's main direction
        const bool steep = std::abs(by2 - ay) > std::abs(bx2 - ax);
        for (int k = 0; k < 2; ++k)
        {
            const int ox = steep ? k : 0;
            const int oy = steep ? 0 : k;
            renderer.DrawLineScreen(
                static_cast<int>(ax) + ox,
                static_cast<int>(ay) + oy,
                static_cast<int>(bx2) + ox,
                static_cast<int>(by2) + oy,
                yellow);
        }
        const int m = std::max(4, static_cast<int>(5 * zoom));
        renderer.FillRectScreen(static_cast<int>(bx2) - m / 2, static_cast<int>(by2) - m / 2, m, m, yellow);
        renderer.DrawRectScreen(static_cast<int>(bx2) - m, static_cast<int>(by2) - m, 2 * m, 2 * m, yellow);
    }
}

void HUDController::RenderBuildGhost(
    Renderer &renderer,
    const TileMap &map,
    const Camera &camera,
    int mouseTileX,
    int mouseTileY,
    const Economy &economy,
    const EntityManager &entities)
{
    const auto &blueprints = EntityManager::GetBlueprints();
    if (m_selectedBlueprint < 0 || m_selectedBlueprint >= static_cast<int>(blueprints.size()))
        return;

    const auto &bp = blueprints[m_selectedBlueprint];

    bool canPlace = entities.CanPlaceBuilding(mouseTileX, mouseTileY, bp.width, bp.height, map, Players::Local());
    bool canAfford = economy.CanAfford(bp.creditCost);

    // While a building is picked, the line of the build zone of every finished headquarters shows where it may stand
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *hq = e.AsBuilding();
        if (!hq || !hq->alive || hq->state == ActionState::Die || hq->owner != Players::Local()
            || hq->type != UnitType::Headquarters || hq->buildProgress < 1.0f)
            continue;
        RenderUtility::DrawTileRadius(
            renderer,
            camera,
            (hq->tileX + hq->buildWidth * 0.5f) * TILE_WIDTH,
            (hq->tileY + hq->buildHeight * 0.5f) * TILE_HEIGHT,
            static_cast<float>(entities.GetBuildRadius()),
            kBuildRadiusColor,
            2.0f);
    }

    SDL_Color boxColor = (canPlace && canAfford) ? SDL_Color {0, 255, 0, 80} : SDL_Color {255, 0, 0, 80};
    SDL_Color borderColor = (canPlace && canAfford) ? SDL_Color {0, 255, 0, 220} : SDL_Color {255, 0, 0, 220};

    // Footprint tiles in world space so they scale with the camera zoom like the building itself
    for (int ty = mouseTileY; ty < mouseTileY + bp.height; ++ty)
    {
        for (int tx = mouseTileX; tx < mouseTileX + bp.width; ++tx)
        {
            const float wx = static_cast<float>(tx * TILE_WIDTH);
            const float wy = static_cast<float>(ty * TILE_HEIGHT);
            renderer.FillRectWorld(
                wx, wy, static_cast<float>(TILE_WIDTH), static_cast<float>(TILE_HEIGHT), camera, boxColor);
            renderer.DrawRectWorld(
                wx, wy, static_cast<float>(TILE_WIDTH), static_cast<float>(TILE_HEIGHT), camera, borderColor);
        }
    }

    // A tower / bunker shows where it will reach from this spot
    if (const BuildingConfig *cfg = entities.GetBuildingConfig(bp.buildingIndex))
    {
        if (cfg->canAttack && cfg->attackRange > 0.0f)
        {
            RenderUtility::DrawRangeRing(
                renderer,
                camera,
                (mouseTileX + bp.width * 0.5f) * TILE_WIDTH,
                (mouseTileY + bp.height * 0.5f) * TILE_HEIGHT,
                cfg->attackRange,
                {240, 80, 70, 90},
                2.0f);
        }
    }

    // Walls are drawn exactly like the placed segment (building 15 / 25, the reinforced 20 after Active Wall Armor),
    // not with a stray composite
    const int drawIndex = (bp.buildingIndex == 15 && IsEffectResearched(Players::Local(), ResearchEffect::WallStrength))
                              ? 20
                              : bp.buildingIndex;
    renderer.SetDrawOwner(Players::Local()); // the ghost already wears the player's army colour
    renderer.DrawBuildingWorld(
        drawIndex, mouseTileX * TILE_WIDTH, mouseTileY * TILE_HEIGHT, 0, camera, PlayerFaction(), 0, 1.0f, false);
    renderer.DrawBuildingWorld(
        drawIndex, mouseTileX * TILE_WIDTH, mouseTileY * TILE_HEIGHT, 1, camera, PlayerFaction(), 0, 1.0f, false);
    renderer.SetDrawOwner(kNoPlayer);
}

} // namespace OpenArtOfWar
