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
#include "game/Mines.hpp"
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

const char *const kTabNames[HUDController::TabCount] = {"BUILD", "INFANTRY", "VEHICLES", "ORDERS"};

// Tab icons per faction: HQ, rifleman, main tank, a research cameo; Orders draws a crosshair
const char *const kResistanceTabIcons[HUDController::TabCount]
    = {"icons_res.headquarters", "icons_res.rifleman", "icons_res.assault_tank", ""};

const char *const kConfederationTabIcons[HUDController::TabCount]
    = {"icons_conf.command_center", "icons_conf.rifleman", "icons_conf.hammer", ""};

int TabIcon(int tab)
{
    const char *iconName
        = (PlayerFaction() == Faction::Confederation ? kConfederationTabIcons : kResistanceTabIcons)[tab];
    return SpriteHandle(iconName, SpriteKind::Part, "sidebar tab icon");
}

constexpr int kCameoWidth = 60; // native cameo composite size

// A true circle outline in screen pixels (64 segments)
void DrawCircleScreen(Renderer &renderer, float centerX, float centerY, float radius, SDL_Color color)
{
    constexpr int kSegments = 64;
    float prevX = centerX + radius;
    float prevY = centerY;
    for (int i = 1; i <= kSegments; ++i)
    {
        const float angle = 6.28318530718f * static_cast<float>(i) / kSegments;
        const float nextX = centerX + radius * std::cos(angle);
        const float nextY = centerY + radius * std::sin(angle);
        renderer.DrawLineScreen(
            static_cast<int>(std::lround(prevX)),
            static_cast<int>(std::lround(prevY)),
            static_cast<int>(std::lround(nextX)),
            static_cast<int>(std::lround(nextY)),
            color);
        prevX = nextX;
        prevY = nextY;
    }
}

// A filled disc, one horizontal span per pixel row
void FillCircleScreen(Renderer &renderer, float centerX, float centerY, float radius, SDL_Color color)
{
    const int intRadius = static_cast<int>(std::ceil(radius));
    for (int offsetY = -intRadius; offsetY <= intRadius; ++offsetY)
    {
        const float span = std::sqrt(std::max(0.0f, radius * radius - static_cast<float>(offsetY * offsetY)));
        const int leftX = static_cast<int>(std::lround(centerX - span));
        const int rightX = static_cast<int>(std::lround(centerX + span));
        renderer.FillRectScreen(leftX, static_cast<int>(std::lround(centerY)) + offsetY, std::max(1, rightX - leftX), 1, color);
    }
}

SDL_Color RadarFactionColor(Faction faction, bool isUnit);

// A player's own army colour on the radar when it has one, else the classic colour of its faction
SDL_Color RadarOwnerColor(PlayerId owner, Faction faction, bool isUnit)
{
    if (owner < kMaxPlayers && Players::IsActive(owner) && Players::Slot(owner).color > 0)
    {
        SDL_Color color = ArmyColor(Players::Slot(owner).color);
        if (isUnit) // units a little lighter than buildings, as with the classic colours
            color
                = {static_cast<Uint8>(color.r + (255 - color.r) / 3),
                   static_cast<Uint8>(color.g + (255 - color.g) / 3),
                   static_cast<Uint8>(color.b + (255 - color.b) / 3),
                   255};
        return color;
    }
    return RadarFactionColor(faction, isUnit);
}

SDL_Color RadarFactionColor(Faction faction, bool isUnit)
{
    switch (faction)
    {
        case Faction::Resistance:
            return isUnit ? SDL_Color {255, 160, 140, 255} : SDL_Color {236, 92, 76, 255};
        case Faction::Confederation:
            return isUnit ? SDL_Color {150, 215, 255, 255} : SDL_Color {72, 170, 236, 255};
        default:
            return isUnit ? SDL_Color {190, 230, 150, 255} : SDL_Color {120, 190, 90, 255};
    }
}

std::string UnitName(const UnitCameoDef &unitDef, const EntityManager &entities)
{
    const auto *unitConfig = entities.GetUnitConfig(unitDef.archetype);
    return (unitConfig && !unitConfig->name.empty()) ? unitConfig->name : std::string(unitDef.name);
}

int UnitCost(const UnitCameoDef &unitDef, const EntityManager &entities)
{
    const auto *unitConfig = entities.GetUnitConfig(unitDef.archetype);
    return unitConfig ? unitConfig->creditCost : unitDef.cost;
}

std::string UnitDesc(const UnitCameoDef &unitDef, const EntityManager &entities)
{
    const auto *unitConfig = entities.GetUnitConfig(unitDef.archetype);
    return (unitConfig && !unitConfig->description.empty()) ? unitConfig->description : std::string(unitDef.desc);
}

void DrawInfinity(Renderer &renderer, const UiRect &buttonRect, bool active, bool hovered)
{
    renderer.FillRectScreen(
        buttonRect.x,
        buttonRect.y,
        buttonRect.w,
        buttonRect.h,
        active ? SDL_Color {60, 44, 20, 235} : SDL_Color {6, 8, 12, hovered ? Uint8(235) : Uint8(190)});
    renderer.DrawRectScreen(
        buttonRect.x,
        buttonRect.y,
        buttonRect.w,
        buttonRect.h,
        active ? col::Accent : (hovered ? col::BorderStrong : col::Border));
    // two rings side by side: the infinity sign
    const SDL_Color ringColor = active ? col::Accent : col::TextDim;
    renderer.DrawRingScreen(
        static_cast<float>(buttonRect.x + 7), static_cast<float>(buttonRect.CenterY()), 4.0f, 3.5f, 1.6f, ringColor, 24);
    renderer.DrawRingScreen(
        static_cast<float>(buttonRect.x + 17), static_cast<float>(buttonRect.CenterY()), 4.0f, 3.5f, 1.6f, ringColor, 24);
}

} // namespace

void DrawCostTag(
    Renderer &renderer, BitmapFontRenderer &fontRenderer, const UiRect &cell, const std::string &text, SDL_Color color)
{
    int tagWidth = fontRenderer.GetTextWidth(text, ui::FONT_SMALL_BOLD) + 10;
    const int tagHeight = std::clamp(cell.h * 18 / 84, 15, 28);
    UiRect tagRect {cell.Right() - tagWidth - 3, cell.y + 3, tagWidth, tagHeight};
    renderer.FillRectScreen(tagRect.x, tagRect.y, tagRect.w, tagRect.h, {6, 8, 12, 215});
    fontRenderer.DrawString(
        text, tagRect.CenterX(), tagRect.CenterY(), ui::FONT_SMALL_BOLD, color, false, Anchor::HCENTER | Anchor::VCENTER);
}

// Common cameo frame: icon, bottom name band, hover/selection border
void DrawCameo(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const UiRect &cell,
    int compId,
    const std::string &name,
    bool hovered,
    bool selected,
    bool locked)
{
    renderer.FillRectScreen(cell.x, cell.y, cell.w, cell.h, col::PanelSunken);
    const int bandHeight = std::clamp(cell.h * 20 / 84, 16, 36);
    if (compId >= 0)
    {
        const float iconScale
            = 2.0f * std::min(static_cast<float>(cell.w) / 136.0f, static_cast<float>(cell.h) / 84.0f);
        const int iconWidth = static_cast<int>(std::lround(kCameoWidth * iconScale));
        int iconX = cell.x + (cell.w - iconWidth) / 2;
        int iconY = cell.y + 3;
        // The icons wear the player's army colour, like its buildings and units
        renderer.SetDrawOwner(Players::Local());
        renderer.SetStructureFaction(PlayerFaction());
        renderer.DrawCompositeScreen(compId, iconX, iconY, iconScale, PlayerFaction());
        renderer.SetStructureFaction(Faction::Neutral);
        renderer.SetDrawOwner(kNoPlayer);
    }
    if (locked)
    {
        renderer.FillRectScreen(cell.x, cell.y, cell.w, cell.h, {10, 12, 16, 170});
    }
    UiRect bandRect {cell.x, cell.Bottom() - bandHeight, cell.w, bandHeight};
    renderer.FillRectScreen(bandRect.x, bandRect.y, bandRect.w, bandRect.h, {6, 8, 12, 225});
    fontRenderer.DrawString(
        ui::FitText(fontRenderer, name, ui::FONT_SMALL_BOLD, std::max(16, bandRect.w - 10)),
        bandRect.CenterX(),
        bandRect.CenterY(),
        ui::FONT_SMALL_BOLD,
        locked ? col::TextMuted : (selected ? col::Accent : col::Text),
        false,
        Anchor::HCENTER | Anchor::VCENTER);

    SDL_Color border = selected ? col::Accent : (hovered ? col::BorderStrong : col::Border);
    renderer.DrawRectScreen(cell.x, cell.y, cell.w, cell.h, border);
    if (selected)
        renderer.DrawRectScreen(cell.x + 1, cell.y + 1, cell.w - 2, cell.h - 2, ui::WithAlpha(col::Accent, 120));
}

// Progress "shutter" that uncovers the cameo from the top while an item is being produced
void DrawProgressShutter(
    Renderer &renderer, BitmapFontRenderer &fontRenderer, const UiRect &cell, float progress, SDL_Color tint)
{
    const int bandHeight = std::clamp(cell.h * 20 / 84, 16, 36);
    int coverHeight = static_cast<int>((cell.h - bandHeight) * (1.0f - std::clamp(progress, 0.0f, 1.0f)));
    if (coverHeight > 0)
        renderer.FillRectScreen(cell.x + 1, cell.Bottom() - bandHeight - coverHeight, cell.w - 2, coverHeight, {6, 8, 12, 160});
    renderer.FillRectScreen(cell.x + 1, cell.Bottom() - bandHeight - 2, static_cast<int>((cell.w - 2) * progress), 2, tint);
    std::string percentText = std::to_string(static_cast<int>(progress * 100.0f)) + "%";
    fontRenderer.DrawString(
        percentText,
        cell.CenterX(),
        cell.y + (cell.h - bandHeight) / 2,
        ui::FONT_H2,
        ui::color::Text,
        true,
        Anchor::HCENTER | Anchor::VCENTER);
}

namespace
{

void DrawLockBadge(Renderer &renderer, BitmapFontRenderer &fontRenderer, const UiRect &cell)
{
    fontRenderer.DrawString(
        _("LOCK"),
        cell.CenterX(),
        cell.y + (cell.h - 20) / 2,
        ui::FONT_SMALL_BOLD,
        col::Danger,
        true,
        Anchor::HCENTER | Anchor::VCENTER);
    (void) renderer;
}

// Crosshair glyph used as the Orders tab icon
void DrawCrosshair(Renderer &renderer, int centerX, int centerY, int size, SDL_Color color)
{
    renderer.DrawRectScreen(centerX - size / 2, centerY - size / 2, size, size, color);
    renderer.FillRectScreen(centerX - size / 2 - 4, centerY, size + 9, 1, color);
    renderer.FillRectScreen(centerX, centerY - size / 2 - 4, 1, size + 9, color);
}

} // namespace

void HUDController::DrawRadar(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const UiRect &radarRect,
    const EntityManager &entities,
    const TileMap &map,
    const Camera &camera,
    int mouseX,
    int mouseY,
    bool zoomButtons)
{
    renderer.FillRectScreen(radarRect.x, radarRect.y, radarRect.w, radarRect.h, {6, 12, 10, 255});
    const int mapWidth = map.GetWidth();
    const int mapHeight = map.GetHeight();
    // A zoomed radar follows the camera, except while the player drags on it (that moves the camera)
    const float camCenterX = (camera.GetX() + camera.GetViewportWidth() * 0.5f / camera.GetZoom()) / TILE_WIDTH;
    const float camCenterY = (camera.GetY() + camera.GetViewportHeight() * 0.5f / camera.GetZoom()) / TILE_HEIGHT;
    if (!m_radarDragging || m_radarCenterX < 0.0f)
    {
        m_radarCenterX = camCenterX;
        m_radarCenterY = camCenterY;
    }
    RadarView view = radarView(radarRect, mapWidth, mapHeight, m_radarZoom, m_radarCenterX, m_radarCenterY);
    if (view.w > 0 && view.h > 0)
    {
        renderer.SetClipRect({view.ox, view.oy, view.w, view.h});
        renderer.FillRectScreen(view.ox, view.oy, view.w, view.h, {14, 30, 22, 255});
        const bool fogEnabled = m_radarFog && map.IsFogEnabled() && !m_showAll;
        const int minTileX = std::max(0, static_cast<int>(view.viewX));
        const int minTileY = std::max(0, static_cast<int>(view.viewY));
        const int maxTileX = std::min(mapWidth, static_cast<int>(std::ceil(view.viewX + view.w / view.scale)) + 1);
        const int maxTileY = std::min(mapHeight, static_cast<int>(std::ceil(view.viewY + view.h / view.scale)) + 1);
        // Cells from rounded edges: neighbours meet exactly, nothing shimmers as the view moves
        auto cellRect = [&](float tileX,
                            float tileY,
                            float tileWidth,
                            float tileHeight,
                            int &pixelX,
                            int &pixelY,
                            int &pixelWidth,
                            int &pixelHeight) {
            pixelX = static_cast<int>(std::lround(view.ToScreenX(tileX)));
            pixelY = static_cast<int>(std::lround(view.ToScreenY(tileY)));
            pixelWidth = std::max(1, static_cast<int>(std::lround(view.ToScreenX(tileX + tileWidth))) - pixelX);
            pixelHeight = std::max(1, static_cast<int>(std::lround(view.ToScreenY(tileY + tileHeight))) - pixelY);
        };
        const SDL_Color hiddenColor {4, 6, 8, 255};
        for (int tileY = minTileY; tileY < maxTileY; ++tileY)
        {
            for (int tileX = minTileX; tileX < maxTileX; ++tileX)
            {
                int pixelX, pixelY, pixelWidth, pixelHeight;
                cellRect(
                    static_cast<float>(tileX),
                    static_cast<float>(tileY),
                    1.0f,
                    1.0f,
                    pixelX,
                    pixelY,
                    pixelWidth,
                    pixelHeight);
                const FogState fogState = fogEnabled ? map.GetFogState(tileX, tileY) : FogState::Visible;
                if (fogState == FogState::Hidden)
                {
                    renderer.FillRectScreen(pixelX, pixelY, pixelWidth, pixelHeight, hiddenColor);
                    continue;
                }
                // Walls as the local player knows them: under the fog of war the ones seen there last
                const std::uint16_t wall = fogEnabled ? map.ViewerWall(tileX, tileY) : map.GetWall(tileX, tileY);
                if (wall > 0)
                {
                    const PlayerId owner = fogEnabled ? map.ViewerWallOwner(tileX, tileY) : map.GetWallOwner(tileX, tileY);
                    renderer.FillRectScreen(
                        pixelX,
                        pixelY,
                        pixelWidth,
                        pixelHeight,
                        RadarOwnerColor(owner, wall >= 1101 ? Faction::Resistance : Faction::Confederation, false));
                }
                else if (map.GetWall(tileX, tileY) == 0 && !map.IsPassable(tileX, tileY)) // an unseen wall is no terrain
                {
                    renderer.FillRectScreen(pixelX, pixelY, pixelWidth, pixelHeight, {30, 54, 40, 255});
                }
            }
        }

        // First enemy drawn on the radar this frame: the radar alarm's contact point
        bool enemyOnRadar = false;
        float contactX = 0.0f, contactY = 0.0f;
        for (const auto &entity : entities.GetAllEntities())
        {
            if (!entity.alive || entity.state == ActionState::Die)
                continue;
            const bool isOwn = (entity.owner == Players::Local());
            const Building *building = entity.AsBuilding();
            if (fogEnabled && !isOwn)
            {
                if (building)
                {
                    // Enemy structures appear once they have been seen (never while they are being built out of
                    // sight)
                    if (!entities.IsHostileBuildingShown(*building, map))
                        continue;
                }
                else if (!entities.IsEntityVisibleToPlayer(entity, Players::Local(), map))
                {
                    continue; // units only while in sight
                }
            }
            int pixelX, pixelY, pixelWidth, pixelHeight;
            if (building)
            {
                cellRect(
                    static_cast<float>(building->tileX),
                    static_cast<float>(building->tileY),
                    static_cast<float>(building->buildWidth),
                    static_cast<float>(building->buildHeight),
                    pixelX,
                    pixelY,
                    pixelWidth,
                    pixelHeight);
                renderer.FillRectScreen(
                    pixelX,
                    pixelY,
                    std::max(3, pixelWidth),
                    std::max(3, pixelHeight),
                    RadarOwnerColor(entity.owner, entity.faction, false));
            }
            else
            {
                cellRect(
                    entity.GetUnitCenterPixelX() / TILE_WIDTH - 0.5f,
                    entity.GetUnitCenterPixelY() / TILE_HEIGHT - 0.5f,
                    1.0f,
                    1.0f,
                    pixelX,
                    pixelY,
                    pixelWidth,
                    pixelHeight);
                const int unitSize = std::max(2, std::min(pixelWidth, pixelHeight));
                renderer.FillRectScreen(
                    pixelX + (pixelWidth - unitSize) / 2,
                    pixelY + (pixelHeight - unitSize) / 2,
                    unitSize,
                    unitSize,
                    RadarOwnerColor(entity.owner, entity.faction, true));
            }
            if (!enemyOnRadar && !isOwn && entity.faction != Faction::Neutral
                && Players::Hostile(entity.owner, Players::Local())
                && (!building || m_radarAlert.config.includeBuildings))
            {
                enemyOnRadar = true;
                contactX = entity.GetUnitCenterPixelX() / TILE_WIDTH;
                contactY = entity.GetUnitCenterPixelY() / TILE_HEIGHT;
                if (building)
                {
                    contactX = building->tileX + building->buildWidth * 0.5f;
                    contactY = building->tileY + building->buildHeight * 0.5f;
                }
            }
        }

        // Radar alarm: red waves and a flashing dot at the contact (true circles in screen pixels)
        const std::uint64_t nowMs = SDL_GetTicks();
        m_radarAlert.Update(nowMs, enemyOnRadar, contactX, contactY);
        if (m_radarAlert.Active())
        {
            const RadarAlertConfig &alertConfig = m_radarAlert.config;
            const float centerX = view.ToScreenX(m_radarAlert.TileX());
            const float centerY = view.ToScreenY(m_radarAlert.TileY());
            for (int i = 0; i < alertConfig.waveCount; ++i)
            {
                float radius = 0.0f, opacity = 0.0f;
                if (!m_radarAlert.Wave(i, nowMs, radius, opacity))
                    continue;
                SDL_Color waveColor = alertConfig.color;
                waveColor.a = static_cast<Uint8>(alertConfig.color.a * opacity);
                for (int k = 0; k < alertConfig.waveThicknessPx; ++k)
                    DrawCircleScreen(renderer, centerX, centerY, radius + static_cast<float>(k), waveColor);
            }
            if (m_radarAlert.BlinkOn(nowMs) && alertConfig.dotRadiusPx > 0.0f)
                FillCircleScreen(renderer, centerX, centerY, alertConfig.dotRadiusPx, alertConfig.color);
        }

        // Camera frame from rounded edges (truncating position and size separately made it wobble)
        const float camWorldWidth = camera.GetViewportWidth() / camera.GetZoom();
        const float camWorldHeight = camera.GetViewportHeight() / camera.GetZoom();
        int frameX, frameY, frameWidth, frameHeight;
        cellRect(
            camera.GetX() / TILE_WIDTH,
            camera.GetY() / TILE_HEIGHT,
            camWorldWidth / TILE_WIDTH,
            camWorldHeight / TILE_HEIGHT,
            frameX,
            frameY,
            frameWidth,
            frameHeight);
        renderer.DrawRectScreen(frameX, frameY, std::max(6, frameWidth), std::max(4, frameHeight), {255, 255, 255, 220});
        renderer.ResetClipRect();
    }
    renderer.DrawRectScreen(radarRect.x, radarRect.y, radarRect.w, radarRect.h, col::BorderStrong);

    // Radar alarm: the radar's sides flash red, the band fading inward
    if (m_radarAlert.BlinkOn(SDL_GetTicks()))
    {
        const RadarAlertConfig &alertConfig = m_radarAlert.config;
        for (int k = 0; k < alertConfig.edgeWidthPx; ++k)
        {
            SDL_Color edgeColor = alertConfig.color;
            edgeColor.a = static_cast<Uint8>(alertConfig.edgeAlpha * (alertConfig.edgeWidthPx - k) / alertConfig.edgeWidthPx);
            renderer.DrawRectScreen(
                radarRect.x + k, radarRect.y + k, radarRect.w - 2 * k, radarRect.h - 2 * k, edgeColor);
        }
    }

    // Zoom buttons
    const UiRect zoomRects[2] = {RadarZoomOutBtn(radarRect), RadarZoomInBtn(radarRect)};
    for (int k = 0; k < (zoomButtons ? 2 : 0); ++k)
    {
        const UiRect &buttonRect = zoomRects[k];
        const bool enabled = (k == 0) ? m_radarZoom > 1 : m_radarZoom < kRadarMaxZoom;
        const bool hovered = enabled && buttonRect.Contains(mouseX, mouseY);
        if (hovered)
            SoundRequests::MarkHover();
        renderer.FillRectScreen(
            buttonRect.x,
            buttonRect.y,
            buttonRect.w,
            buttonRect.h,
            hovered ? col::Hover : SDL_Color {10, 16, 14, 210});
        renderer.DrawRectScreen(
            buttonRect.x, buttonRect.y, buttonRect.w, buttonRect.h, enabled ? col::BorderStrong : col::Border);
        const SDL_Color symbolColor = enabled ? col::Text : col::TextMuted;
        renderer.FillRectScreen(buttonRect.x + 5, buttonRect.CenterY() - 1, buttonRect.w - 10, 2, symbolColor);
        if (k == 1)
            renderer.FillRectScreen(buttonRect.CenterX() - 1, buttonRect.y + 5, 2, buttonRect.h - 10, symbolColor);
    }
    if (m_radarZoom > 1)
    {
        fontRenderer.DrawString(
            "x" + std::to_string(m_radarZoom),
            radarRect.x + 6,
            radarRect.Bottom() - 12,
            ui::FONT_SMALL_BOLD,
            col::TextDim,
            false,
            Anchor::LEFT | Anchor::VCENTER);
    }
}

void HUDController::RenderGameHud(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const Economy &economy,
    const EntityManager &entities,
    const TileMap &map,
    const Camera &camera,
    int mouseX,
    int mouseY,
    int windowWidth,
    int windowHeight,
    const std::vector<int> &selected)
{
    SidebarLayout layout = sidebarLayout(windowWidth, windowHeight, m_topInset, !m_observer && m_speedEnabled);

    // Frame
    ui::FillVerticalGradient(renderer, layout.panel, {20, 24, 32, 255}, {12, 15, 20, 255});
    renderer.FillRectScreen(layout.panel.x, 0, 1, windowHeight, col::BorderStrong);
    renderer.FillRectScreen(layout.panel.x + 1, 0, 1, windowHeight, {0, 0, 0, 120});

    // --- Header: credits & power ---------------------------------------------------------
    {
        const UiRect &headerRect = layout.header;
        std::string credits = "$ " + std::to_string(economy.GetCredits());
        fontRenderer.DrawString(credits, headerRect.x, headerRect.y - 2, ui::FONT_H2, col::Accent, true);

        int powerProduced = economy.GetPowerProduced();
        int powerConsumed = economy.GetPowerConsumed();
        bool hasDeficit = economy.HasPowerDeficit();
        SDL_Color powerColor = hasDeficit ? col::Danger : col::Ok;
        std::string powerText = std::to_string(powerConsumed) + " / " + std::to_string(powerProduced) + _(" kW");
        fontRenderer.DrawString(
            powerText,
            headerRect.Right(),
            headerRect.y + 2,
            ui::FONT_SMALL_BOLD,
            powerColor,
            false,
            Anchor::RIGHT | Anchor::TOP);
        // The lightning of the game next to the kW figures; it blinks while the power is short
        if (!hasDeficit || PowerIconBlinkOn())
        {
            const int textWidth = fontRenderer.GetTextWidth(powerText, ui::FONT_SMALL_BOLD);
            renderer.DrawRawSpriteScreen(PowerIconSprite(), headerRect.Right() - textWidth - 27, headerRect.y, 1.0f);
        }
        fontRenderer.DrawString(
            _("POWER"),
            headerRect.Right(),
            headerRect.y + 17,
            ui::FONT_SMALL,
            col::TextMuted,
            false,
            Anchor::RIGHT | Anchor::TOP);

        float powerRatio = static_cast<float>(powerConsumed) / static_cast<float>(std::max(1, powerProduced));
        ui::DrawProgressBar(
            renderer, {headerRect.x, headerRect.Bottom() - 6, headerRect.w, 6}, powerRatio, powerColor);
    }

    // --- Radar -----------------------------------------------------------------------------
    DrawRadar(renderer, fontRenderer, layout.radar, entities, map, camera, mouseX, mouseY);

    // --- Menu / objectives -------------------------------------------------------------------
    ui::DrawButton(
        renderer,
        fontRenderer,
        layout.menuBtn,
        ui::FitText(fontRenderer, _("MENU"), ui::FONT_SMALL, layout.menuBtn.w - 8),
        ui::ButtonStyle::Secondary,
        layout.menuBtn.Contains(mouseX, mouseY));
    ui::DrawButton(
        renderer,
        fontRenderer,
        layout.objectivesBtn,
        ui::FitText(fontRenderer, _("OBJECTIVES"), ui::FONT_SMALL, layout.objectivesBtn.w - 8),
        ui::ButtonStyle::Secondary,
        layout.objectivesBtn.Contains(mouseX, mouseY));
    if (!m_observer && m_speedEnabled)
    {
        char speedBuffer[16];
        if (m_speedMultiplier < 1.0f)
            std::snprintf(speedBuffer, sizeof(speedBuffer), "x%.1f", m_speedMultiplier);
        else
            std::snprintf(speedBuffer, sizeof(speedBuffer), "x%d", static_cast<int>(m_speedMultiplier));
        ui::DrawButton(
            renderer,
            fontRenderer,
            layout.slowerBtn,
            "-",
            ui::ButtonStyle::Secondary,
            layout.slowerBtn.Contains(mouseX, mouseY),
            m_speedMultiplier <= 0.5f);
        ui::DrawButton(
            renderer,
            fontRenderer,
            layout.speedBtn,
            speedBuffer,
            m_speedMultiplier != 1.0f ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
            layout.speedBtn.Contains(mouseX, mouseY));
        ui::DrawButton(
            renderer,
            fontRenderer,
            layout.fasterBtn,
            "+",
            ui::ButtonStyle::Secondary,
            layout.fasterBtn.Contains(mouseX, mouseY),
            m_speedMultiplier >= 32.0f);
    }

    // A replay's observer has no buildings to command: only the radar, the header and the menu stay
    if (!m_observer)
    {
        ui::DrawButton(
            renderer,
            fontRenderer,
            layout.researchBtn,
            ui::FitText(fontRenderer, _("RESEARCH NEW TECHNOLOGIES"), ui::FONT_SMALL, layout.researchBtn.w - 12),
            ui::ButtonStyle::Secondary,
            layout.researchBtn.Contains(mouseX, mouseY));
        // --- Tabs ----------------------------------------------------------------------------------
        for (int tabIndex = 0; tabIndex < TabCount; ++tabIndex)
        {
            const UiRect &tabRect = layout.tabs[tabIndex];
            bool active = (tabIndex == m_activeTab);
            bool hovered = tabRect.Contains(mouseX, mouseY);
            if (hovered)
                SoundRequests::MarkHover();
            renderer.FillRectScreen(
                tabRect.x,
                tabRect.y,
                tabRect.w,
                tabRect.h,
                active ? col::Hover : (hovered ? col::PanelRaised : col::PanelSunken));
            renderer.DrawRectScreen(
                tabRect.x, tabRect.y, tabRect.w, tabRect.h, active ? col::BorderStrong : col::Border);
            if (active)
                renderer.FillRectScreen(tabRect.x, tabRect.Bottom() - 3, tabRect.w, 3, col::Accent);

            if (TabIcon(tabIndex) >= 0)
            {
                renderer.SetClipRect({tabRect.x + 1, tabRect.y + 1, tabRect.w - 2, tabRect.h - 4});
                float iconScale = 0.75f;
                int iconWidth = static_cast<int>(kCameoWidth * iconScale);
                renderer.SetDrawOwner(Players::Local());
                renderer.SetStructureFaction(PlayerFaction());
                renderer.DrawCompositeScreen(
                    TabIcon(tabIndex), tabRect.CenterX() - iconWidth / 2, tabRect.y + 6, iconScale, PlayerFaction());
                renderer.SetStructureFaction(Faction::Neutral);
                renderer.SetDrawOwner(kNoPlayer);
                renderer.ResetClipRect();
                if (!active)
                    renderer.FillRectScreen(
                        tabRect.x + 1,
                        tabRect.y + 1,
                        tabRect.w - 2,
                        tabRect.h - 4,
                        {10, 12, 16, hovered ? Uint8(60) : Uint8(110)});
            }
            else
            {
                DrawCrosshair(
                    renderer, tabRect.CenterX(), tabRect.CenterY() - 1, 16, active ? col::Accent : col::TextDim);
            }
        }

        // --- Strip: tab title, primary building, scroll arrows ---------------------------------------
        int maxScroll = MaxScrollRows(m_activeTab, layout.grid.h);
        int &scroll = m_scrollRow[m_activeTab];
        scroll = std::clamp(scroll, 0, maxScroll);
        {
            ui::DrawCaption(fontRenderer, renderer, layout.strip.x, layout.strip.y + 4, _(kTabNames[m_activeTab]));

            if (m_activeTab == TabInfantry || m_activeTab == TabVehicles)
            {
                bool isInfantry = (m_activeTab == TabInfantry);
                int primaryId = isInfantry ? m_primaryBarracksId : m_primaryFactoryId;
                auto buildings
                    = entities.GetBuildingsByType(isInfantry ? UnitType::Barracks : UnitType::Factory, Players::Local());
                if (!buildings.empty())
                {
                    std::string label
                        = std::string(isInfantry ? _("Barracks") : _("Factory")) + " #" + std::to_string(primaryId);
                    if (buildings.size() > 1)
                        label += "  \xE2\x80\xBA";
                    bool hovered = buildings.size() > 1 && layout.primaryBtn.Contains(mouseX, mouseY);
                    fontRenderer.DrawString(
                        ui::FitText(fontRenderer, label, ui::FONT_SMALL, layout.primaryBtn.w),
                        layout.primaryBtn.Right(),
                        layout.primaryBtn.CenterY(),
                        ui::FONT_SMALL,
                        hovered ? col::Accent : col::TextDim,
                        false,
                        Anchor::RIGHT | Anchor::VCENTER);
                }
            }
            else if (m_activeTab == TabOrders)
            {
                int selectedCount = 0;
                for (const auto &entity : entities.GetAllEntities())
                {
                    if (entity.alive && entity.selected && !entity.isBuilding)
                        ++selectedCount;
                }
                std::string label = std::string(_("Selected")) + ": " + std::to_string(selectedCount);
                fontRenderer.DrawString(
                    label,
                    layout.primaryBtn.Right(),
                    layout.primaryBtn.CenterY(),
                    ui::FONT_SMALL,
                    col::TextDim,
                    false,
                    Anchor::RIGHT | Anchor::VCENTER);
            }

            if (maxScroll > 0)
            {
                auto drawArrow = [&](const UiRect &arrowRect, bool isUp, bool enabled) {
                    bool hovered = enabled && arrowRect.Contains(mouseX, mouseY);
                    renderer.FillRectScreen(
                        arrowRect.x, arrowRect.y, arrowRect.w, arrowRect.h, hovered ? col::Hover : col::PanelRaised);
                    renderer.DrawRectScreen(
                        arrowRect.x, arrowRect.y, arrowRect.w, arrowRect.h, hovered ? col::BorderStrong : col::Border);
                    SDL_Color arrowColor = enabled ? (hovered ? col::Accent : col::Text) : col::TextMuted;
                    for (int i = 0; i < 5; ++i)
                    {
                        int rowY = isUp ? (arrowRect.CenterY() - 2 + i) : (arrowRect.CenterY() + 2 - i);
                        renderer.FillRectScreen(arrowRect.CenterX() - i, rowY, 2 * i + 1, 1, arrowColor);
                    }
                };
                drawArrow(layout.scrollUp, true, scroll > 0);
                drawArrow(layout.scrollDown, false, scroll < maxScroll);
            }
        }

        // --- Cameo grid -----------------------------------------------------------------------------------
        std::string hoverTitle;
        std::string hoverText;
        bool hasBarracks = entities.HasBuildingType(UnitType::Barracks, Players::Local());
        bool hasFactory = entities.HasBuildingType(UnitType::Factory, Players::Local());
        [[maybe_unused]] bool hasLab = HasResearchBuilding(Players::Local(), entities);
        const bool noHq
            = !entities.HasBuildingType(UnitType::Headquarters, Players::Local(), true); // nothing can be built
        // Orders shown on unit cards come from the buildings that would train them
        auto queuedAt = [&](UnitType buildingType) {
            std::vector<const Building *> targets;
            for (int id : ProductionTargets(buildingType, entities, selected))
                targets.push_back(entities.GetBuilding(id));
            return targets;
        };
        const std::vector<const Building *> barracksTargets = queuedAt(UnitType::Barracks);
        const std::vector<const Building *> factoryTargets = queuedAt(UnitType::Factory);

        renderer.SetClipRect({layout.grid.x, layout.grid.y, layout.grid.w, layout.grid.h});
        int itemCount = ItemCountForTab(m_activeTab);
        for (int i = 0; i < itemCount; ++i)
        {
            UiRect cellRect = layout.Cell(i, scroll);
            if (cellRect.Bottom() < layout.grid.y || cellRect.y > layout.grid.Bottom())
                continue;
            const int itemIndex = ItemAt(m_activeTab, i); // (the tech level hides what it leaves out)
            bool hovered = cellRect.Contains(mouseX, mouseY) && layout.grid.Contains(mouseX, mouseY);
            if (hovered)
                SoundRequests::MarkHover();

            switch (m_activeTab)
            {
                case TabBuild:
                {
                    const auto &blueprint = EntityManager::GetBlueprints()[itemIndex];
                    bool isSelected = (m_buildMode && m_selectedBlueprint == itemIndex);
                    bool canAfford = economy.CanAfford(blueprint.creditCost);
                    // Hard limits (data/limits.yaml): greyed out at the limit, "n/max" while one is set
                    // (a mine has its own count in mines.yaml)
                    const bool isMine = blueprint.mineKind >= 0;
                    const int limit = isMine ? Mines::Specs()[static_cast<std::size_t>(blueprint.mineKind)].maxPerPlayer
                                             : BuildLimits::LimitOf(entities, blueprint.buildingIndex);
                    const int owned
                        = limit > 0
                              ? (isMine ? entities.CountMines(Players::Local(), blueprint.mineKind)
                                        : BuildLimits::CountOwned(entities, map, Players::Local(), blueprint.buildingIndex))
                              : 0;
                    const bool atLimit = limit == 0 || (limit > 0 && owned >= limit);
                    const bool radarLocked
                        = (blueprint.buildingIndex == 6 && !IsRadarUnlocked(Players::Local())) || noHq || atLimit;
                    DrawCameo(
                        renderer,
                        fontRenderer,
                        cellRect,
                        blueprint.iconCompId,
                        _(blueprint.name),
                        hovered,
                        isSelected && !noHq,
                        radarLocked);
                    if (limit > 0)
                    {
                        fontRenderer.DrawString(
                            std::to_string(owned) + "/" + std::to_string(limit),
                            cellRect.x + cellRect.w - 6,
                            cellRect.y + 5,
                            ui::FONT_SMALL_BOLD,
                            atLimit ? col::Danger : col::TextMuted,
                            true,
                            Anchor::RIGHT | Anchor::TOP);
                    }
                    DrawCostTag(
                        renderer,
                        fontRenderer,
                        cellRect,
                        "$" + std::to_string(blueprint.creditCost),
                        canAfford ? col::Accent : col::Danger);
                    std::string powerCostText
                        = (blueprint.powerCost >= 0 ? "+" : "") + std::to_string(blueprint.powerCost);
                    fontRenderer.DrawString(
                        powerCostText,
                        cellRect.x + 6,
                        cellRect.y + 5,
                        ui::FONT_SMALL_BOLD,
                        blueprint.powerCost >= 0 ? col::Ok : SDL_Color {255, 170, 80, 255},
                        true);
                    if (hovered)
                    {
                        hoverTitle = std::string(_(blueprint.name)) + "  $" + std::to_string(blueprint.creditCost)
                                     + ", " + powerCostText + _(" kW");
                        hoverText = _(blueprint.desc);
                        if (limit == 0)
                            hoverText = std::string(_("Not allowed in this game.")) + " " + hoverText;
                        else if (atLimit)
                            hoverText = std::string(_("Building limit reached: ")) + std::to_string(owned) + "/"
                                        + std::to_string(limit) + ". " + hoverText;
                    }
                    break;
                }
                case TabInfantry:
                case TabVehicles:
                {
                    bool isInfantry = (m_activeTab == TabInfantry);
                    const UnitCameoDef &unitDef = unitCatalog(isInfantry).items[itemIndex];
                    const auto &producers = isInfantry ? barracksTargets : factoryTargets;
                    const bool unlocked = IsUnitUnlocked(Players::Local(), unitDef.archetype);
                    bool available = (isInfantry ? hasBarracks : hasFactory) && unlocked;
                    int cost = UnitCost(unitDef, entities);
                    std::string name = _(UnitName(unitDef, entities));
                    DrawCameo(
                        renderer,
                        fontRenderer,
                        cellRect,
                        SpriteHandle(unitDef.icon, SpriteKind::Part, "unit cameo"),
                        name,
                        hovered,
                        false,
                        !available);

                    int queued = 0;
                    float progress = -1.0f; // most advanced order of this unit currently in training
                    for (const Building *building : producers)
                    {
                        for (const auto &order : building->productionQueue)
                        {
                            if (order.archetype == unitDef.archetype)
                                ++queued;
                        }
                        if (!building->productionQueue.empty()
                            && building->productionQueue.front().archetype == unitDef.archetype)
                        {
                            progress = std::max(progress, building->CurrentProductionProgress());
                        }
                    }
                    if (!available)
                    {
                        DrawLockBadge(renderer, fontRenderer, cellRect);
                    }
                    else
                    {
                        DrawCostTag(
                            renderer,
                            fontRenderer,
                            cellRect,
                            "$" + std::to_string(cost),
                            economy.CanAfford(cost) ? col::Accent : col::Danger);
                        if (progress >= 0.0f)
                        {
                            DrawProgressShutter(renderer, fontRenderer, cellRect, progress, col::Accent);
                        }
                        if (queued > 0)
                        {
                            std::string badge = "\xC3\x97" + std::to_string(queued);
                            renderer.FillRectScreen(
                                cellRect.x + 3,
                                cellRect.y + 3,
                                fontRenderer.GetTextWidth(badge, ui::FONT_SMALL_BOLD) + 10,
                                18,
                                col::Accent);
                            fontRenderer.DrawString(
                                badge, cellRect.x + 8, cellRect.y + 4, ui::FONT_SMALL_BOLD, col::AccentText, false);
                        }
                    }
                    if (available)
                    {
                        bool repeating = false;
                        for (const Building *producer : producers)
                            repeating = repeating || producer->repeatArchetype == unitDef.archetype;
                        const UiRect infinityRect = InfinityBtn(cellRect);
                        DrawInfinity(renderer, infinityRect, repeating, infinityRect.Contains(mouseX, mouseY));
                    }
                    if (hovered)
                    {
                        hoverTitle = name + "  $" + std::to_string(cost);
                        if (!unlocked)
                        {
                            hoverText
                                = std::string(_("[RESEARCH REQUIRED] "))
                                  + _(researchDef(PlayerFaction(), UnitUnlockResearch(PlayerFaction(), unitDef.archetype))
                                          .name);
                        }
                        else if (!available)
                        {
                            hoverText = isInfantry ? _("[BARRACKS REQUIRED] Construct Barracks to recruit ") + name
                                                   : _("[FACTORY REQUIRED] Construct Factory to build ") + name;
                        }
                        else
                        {
                            hoverText = _(UnitDesc(unitDef, entities));
                            if (queued > 0)
                                hoverText += std::string("  ") + _("(RMB - cancel one)");
                            hoverText
                                += std::string("  ") + _("(Shift - 5, Ctrl - 10; the infinity button repeats it)");
                        }
                    }
                    break;
                }
                case TabOrders:
                {
                    const auto &orderDef = kOrders[i];
                    bool active = (i == 0 && m_attackMoveMode);
                    if (i >= kFirstStanceOrder)
                    {
                        // Lit when every selected own unit has this stance
                        const auto wantedStance = static_cast<UnitStance>(i - kFirstStanceOrder);
                        bool anySelected = false, allMatch = true;
                        for (const auto &entity : entities.GetAllEntities())
                        {
                            const Unit *unit = entity.AsUnit();
                            if (!unit || !unit->alive || !unit->selected || unit->owner != Players::Local())
                                continue;
                            anySelected = true;
                            allMatch = allMatch && unit->stance == wantedStance;
                        }
                        active = anySelected && allMatch;
                    }
                    renderer.FillRectScreen(
                        cellRect.x,
                        cellRect.y,
                        cellRect.w,
                        cellRect.h,
                        active ? SDL_Color {60, 44, 20, 255} : (hovered ? col::Hover : col::PanelRaised));
                    renderer.DrawRectScreen(
                        cellRect.x,
                        cellRect.y,
                        cellRect.w,
                        cellRect.h,
                        active ? col::Accent : (hovered ? col::BorderStrong : col::Border));
                    fontRenderer.DrawString(
                        ui::FitText(fontRenderer, _(orderDef.name), ui::FONT_BODY_BOLD, cellRect.w - 16),
                        cellRect.CenterX(),
                        cellRect.CenterY() + 6,
                        ui::FONT_BODY_BOLD,
                        active ? col::Accent : col::Text,
                        false,
                        Anchor::HCENTER | Anchor::VCENTER);
                    if (orderDef.key[0] != '\0')
                    {
                        UiRect keyChipRect {cellRect.x + 6, cellRect.y + 6, 22, 22};
                        renderer.FillRectScreen(
                            keyChipRect.x, keyChipRect.y, keyChipRect.w, keyChipRect.h, col::PanelSunken);
                        renderer.DrawRectScreen(
                            keyChipRect.x, keyChipRect.y, keyChipRect.w, keyChipRect.h, col::BorderStrong);
                        fontRenderer.DrawString(
                            orderDef.key,
                            keyChipRect.CenterX(),
                            keyChipRect.CenterY(),
                            ui::FONT_SMALL_BOLD,
                            col::Accent,
                            false,
                            Anchor::HCENTER | Anchor::VCENTER);
                    }
                    if (hovered)
                    {
                        hoverTitle = _(orderDef.name);
                        if (orderDef.key[0] != '\0')
                            hoverTitle += std::string("  [") + orderDef.key + "]";
                        hoverText = _(orderDef.desc);
                    }
                    break;
                }
            }
        }
        renderer.ResetClipRect();

        // Scroll position hint on the right edge of the grid
        if (maxScroll > 0)
        {
            int trackX = layout.panel.Right() - 5;
            renderer.FillRectScreen(trackX, layout.grid.y, 2, layout.grid.h, col::PanelRaised);
            int totalRows = maxScroll + layout.VisibleRows();
            int thumbHeight = std::max(16, layout.grid.h * layout.VisibleRows() / totalRows);
            int thumbY = layout.grid.y + (layout.grid.h - thumbHeight) * scroll / maxScroll;
            renderer.FillRectScreen(trackX, thumbY, 2, thumbHeight, col::Accent);
        }

        // --- Info box -------------------------------------------------------------------------------------
        {
            const UiRect &infoRect = layout.info;
            ui::DrawPanel(renderer, infoRect, col::PanelSunken, col::Border);
            int textWidth = infoRect.w - 20;

            if (!m_hudNotification.empty())
            {
                renderer.FillRectScreen(infoRect.x, infoRect.y, 3, infoRect.h, col::Accent);
                fontRenderer.DrawTextWrapped(
                    m_hudNotification,
                    infoRect.x + 10,
                    infoRect.y + 8,
                    textWidth,
                    ui::FONT_SMALL_BOLD,
                    col::Accent,
                    false);
            }
            else if (!hoverTitle.empty())
            {
                fontRenderer.DrawString(
                    ui::FitText(fontRenderer, hoverTitle, ui::FONT_SMALL_BOLD, textWidth),
                    infoRect.x + 10,
                    infoRect.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Text,
                    false);
                fontRenderer.DrawTextWrapped(
                    hoverText, infoRect.x + 10, infoRect.y + 26, textWidth, ui::FONT_SMALL, col::TextDim, false);
            }
            else if (
                m_buildMode && m_selectedBlueprint >= 0
                && m_selectedBlueprint < static_cast<int>(EntityManager::GetBlueprints().size()))
            {
                fontRenderer.DrawString(
                    _(EntityManager::GetBlueprints()[m_selectedBlueprint].name),
                    infoRect.x + 10,
                    infoRect.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Accent,
                    false);
                fontRenderer.DrawTextWrapped(
                    _("LMB - build, RMB / Esc - cancel"),
                    infoRect.x + 10,
                    infoRect.y + 26,
                    textWidth,
                    ui::FONT_SMALL,
                    col::TextDim,
                    false);
            }
            else if (
                const Building *busyBuilding = [&]() -> const Building * {
                    const auto &targets = (m_activeTab == TabVehicles) ? factoryTargets : barracksTargets;
                    for (const Building *building : targets)
                    {
                        if (!building->productionQueue.empty())
                            return building;
                    }
                    return nullptr;
                }();
                busyBuilding && (m_activeTab == TabInfantry || m_activeTab == TabVehicles))
            {
                const auto &frontOrder = busyBuilding->productionQueue.front();
                std::string unitName = _("Production");
                if (const auto *unitConfig = entities.GetUnitConfig(frontOrder.archetype))
                    unitName = _(unitConfig->name);
                float progress = busyBuilding->CurrentProductionProgress();
                std::string statusLine = unitName + "  " + std::to_string(static_cast<int>(progress * 100.0f)) + "%";
                if (busyBuilding->productionQueue.size() > 1)
                {
                    statusLine += "   " + std::string(_("queue")) + ": "
                                  + std::to_string(busyBuilding->productionQueue.size() - 1);
                }
                fontRenderer.DrawString(
                    ui::FitText(fontRenderer, statusLine, ui::FONT_SMALL_BOLD, textWidth),
                    infoRect.x + 10,
                    infoRect.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Text,
                    false);
                ui::DrawProgressBar(renderer, {infoRect.x + 10, infoRect.y + 30, textWidth, 8}, progress, col::Accent);
            }
            else if (const auto ownBuildings = OwnSelectedBuildings(entities, selected); !ownBuildings.empty())
            {
                const Building *building = ownBuildings.front();
                std::string title = _("Structure");
                if (const BuildingConfig *buildingConfig = entities.GetBuildingConfig(building->rawBuildingType))
                    title = _(buildingConfig->name);
                if (ownBuildings.size() > 1)
                    title += "  x" + std::to_string(ownBuildings.size());
                fontRenderer.DrawString(
                    ui::FitText(fontRenderer, title, ui::FONT_SMALL_BOLD, textWidth),
                    infoRect.x + 10,
                    infoRect.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Text,
                    false);
                const float healthRatio
                    = building->maxHp > 0 ? static_cast<float>(building->currentHp) / building->maxHp : 1.0f;
                ui::DrawProgressBar(
                    renderer,
                    {infoRect.x + 10, infoRect.y + 28, textWidth, 6},
                    healthRatio,
                    healthRatio > 0.35f ? col::Ok : col::Danger);
                fontRenderer.DrawString(
                    std::to_string(building->currentHp) + " / " + std::to_string(building->maxHp),
                    infoRect.x + 10,
                    infoRect.y + 40,
                    ui::FONT_SMALL,
                    col::TextDim,
                    false);
            }
            else
            {
                static const char *const kTabHints[TabCount] = {
                    "Select building to construct",
                    "Recruit infantry at the selected or primary Barracks (RMB - cancel)",
                    "Build vehicles at the selected or primary Factory (RMB - cancel)",
                    "Select units and issue a command",
                };
                fontRenderer.DrawTextWrapped(
                    _(kTabHints[m_activeTab]),
                    infoRect.x + 10,
                    infoRect.y + 8,
                    textWidth,
                    ui::FONT_SMALL,
                    col::TextMuted,
                    false);
            }

            if (const auto ownBuildings = OwnSelectedBuildings(entities, selected); !ownBuildings.empty())
            {
                bool anyDamaged = false, allRepairing = true;
                for (const Building *building : ownBuildings)
                {
                    if (building->CanBeRepaired())
                    {
                        anyDamaged = true;
                        allRepairing = allRepairing && building->repairing;
                    }
                }
                if (anyDamaged)
                {
                    const UiRect repairRect = RepairBtn(infoRect);
                    const bool isRepairing = allRepairing;
                    const bool repairHovered = repairRect.Contains(mouseX, mouseY);
                    renderer.FillRectScreen(
                        repairRect.x,
                        repairRect.y,
                        repairRect.w,
                        repairRect.h,
                        isRepairing ? SDL_Color {60, 44, 20, 255}
                                    : (repairHovered ? col::Hover : col::PanelRaised));
                    renderer.DrawRectScreen(
                        repairRect.x,
                        repairRect.y,
                        repairRect.w,
                        repairRect.h,
                        isRepairing ? col::Accent : col::BorderStrong);
                    fontRenderer.DrawString(
                        isRepairing ? _("REPAIRING") : _("REPAIR [R]"),
                        repairRect.CenterX(),
                        repairRect.CenterY(),
                        ui::FONT_SMALL_BOLD,
                        isRepairing ? col::Accent : col::Text,
                        false,
                        Anchor::HCENTER | Anchor::VCENTER);
                }
            }
            if (!OwnSelectedBuildings(entities, selected).empty())
            {
                const UiRect destroyRect = DestroyBtn(infoRect);
                const bool armed = m_destroyArmed > 0.0f;
                const bool destroyHovered = destroyRect.Contains(mouseX, mouseY);
                renderer.FillRectScreen(
                    destroyRect.x,
                    destroyRect.y,
                    destroyRect.w,
                    destroyRect.h,
                    armed ? col::Danger
                          : (destroyHovered ? ui::Mix(col::PanelRaised, col::Danger, 0.35f) : col::PanelRaised));
                renderer.DrawRectScreen(destroyRect.x, destroyRect.y, destroyRect.w, destroyRect.h, col::Danger);
                fontRenderer.DrawString(
                    armed ? _("CONFIRM?") : _("DESTROY"),
                    destroyRect.CenterX(),
                    destroyRect.CenterY(),
                    ui::FONT_SMALL_BOLD,
                    armed ? col::Text : col::Danger,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
            }
        }
    }
}

} // namespace OpenArtOfWar
