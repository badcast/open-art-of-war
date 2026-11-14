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

// A true circle outline in screen pixels (64 segments), clipped to `clip`
void DrawCircleScreen(
    Renderer &renderer, float centerX, float centerY, float radius, SDL_Color color, const SDL_Rect &clip)
{
    if (clip.w <= 0 || clip.h <= 0)
        return;
    const float xmin = static_cast<float>(clip.x);
    const float ymin = static_cast<float>(clip.y);
    const float xmax = static_cast<float>(clip.x + clip.w - 1);
    const float ymax = static_cast<float>(clip.y + clip.h - 1);
    auto clipAndDraw = [&](float x0, float y0, float x1, float y1) {
        float t0 = 0.0f, t1 = 1.0f;
        const float dx = x1 - x0, dy = y1 - y0;
        auto clipEdge = [&](float p, float q) {
            if (std::abs(p) < 1e-6f)
                return q >= 0.0f;
            const float r = q / p;
            if (p < 0.0f)
            {
                if (r > t1)
                    return false;
                if (r > t0)
                    t0 = r;
            }
            else
            {
                if (r < t0)
                    return false;
                if (r < t1)
                    t1 = r;
            }
            return true;
        };
        if (!clipEdge(-dx, x0 - xmin) || !clipEdge(dx, xmax - x0) || !clipEdge(-dy, y0 - ymin)
            || !clipEdge(dy, ymax - y0))
            return;
        renderer.DrawLineScreen(
            static_cast<int>(std::lround(x0 + t0 * dx)),
            static_cast<int>(std::lround(y0 + t0 * dy)),
            static_cast<int>(std::lround(x0 + t1 * dx)),
            static_cast<int>(std::lround(y0 + t1 * dy)),
            color);
    };
    constexpr int kSegments = 64;
    float prevX = centerX + radius;
    float prevY = centerY;
    for (int i = 1; i <= kSegments; ++i)
    {
        const float angle = 6.28318530718f * static_cast<float>(i) / kSegments;
        const float nextX = centerX + radius * std::cos(angle);
        const float nextY = centerY + radius * std::sin(angle);
        clipAndDraw(prevX, prevY, nextX, nextY);
        prevX = nextX;
        prevY = nextY;
    }
}

// A filled disc, one horizontal span per pixel row, clipped to `clip`
void FillCircleScreen(
    Renderer &renderer, float centerX, float centerY, float radius, SDL_Color color, const SDL_Rect &clip)
{
    if (clip.w <= 0 || clip.h <= 0)
        return;
    const int intRadius = static_cast<int>(std::ceil(radius));
    const int cy = static_cast<int>(std::lround(centerY));
    for (int offsetY = -intRadius; offsetY <= intRadius; ++offsetY)
    {
        const int py = cy + offsetY;
        if (py < clip.y || py >= clip.y + clip.h)
            continue;
        const float span = std::sqrt(std::max(0.0f, radius * radius - static_cast<float>(offsetY * offsetY)));
        const int leftX = std::max(clip.x, static_cast<int>(std::lround(centerX - span)));
        const int rightX = std::min(clip.x + clip.w, static_cast<int>(std::lround(centerX + span)));
        if (rightX > leftX)
            renderer.FillRectScreen(leftX, py, rightX - leftX, 1, color);
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
    const SDL_Color topFill = active
                                  ? ui::Mix(col::Accent, col::White, 0.14f)
                                  : (hovered ? ui::Mix(ui::color::AccentDeep, col::Accent, 0.40f)
                                             : ui::Mix(col::PanelRaised, ui::color::AccentDeep, 0.52f));
    const SDL_Color botFill = active ? col::Accent : ui::color::AccentDeep;
    const SDL_Color border = active ? ui::Mix(col::Accent, col::White, 0.45f)
                                    : (hovered ? col::Accent : col::BorderStrong);
    ui::DrawButtonChrome(renderer, buttonRect, topFill, botFill, border, {0, 0, 0, 0}, {0, 0, 0, 0}, 235);
    // two rings side by side: the infinity sign (kept crisp white at opacity 200)
    const SDL_Color ringColor {255, 255, 255, 200};
    renderer.DrawRingScreen(
        static_cast<float>(buttonRect.x + 7),
        static_cast<float>(buttonRect.CenterY()),
        4.0f,
        3.5f,
        1.6f,
        ringColor,
        24);
    renderer.DrawRingScreen(
        static_cast<float>(buttonRect.x + 17),
        static_cast<float>(buttonRect.CenterY()),
        4.0f,
        3.5f,
        1.6f,
        ringColor,
        24);
}

} // namespace

void DrawCostTag(
    Renderer &renderer, BitmapFontRenderer &fontRenderer, const UiRect &cell, std::string_view text, SDL_Color color)
{
    int tagWidth = fontRenderer.GetTextWidth(text, ui::FONT_SMALL_BOLD) + 10;
    const int tagHeight = std::clamp(cell.h * 18 / 84, 15, 28);
    UiRect tagRect {cell.Right() - tagWidth - 3, cell.y + 3, tagWidth, tagHeight};
    renderer.FillRectScreen(tagRect.x, tagRect.y, tagRect.w, tagRect.h, {6, 8, 12, 215});
    fontRenderer.DrawString(
        text,
        tagRect.CenterX(),
        tagRect.CenterY(),
        ui::FONT_SMALL_BOLD,
        color,
        false,
        Anchor::HCENTER | Anchor::VCENTER);
}

void DrawPowerTag(Renderer &renderer, BitmapFontRenderer &fontRenderer, const UiRect &cell, int powerCost, int iconSpriteId)
{
    const std::string text = (powerCost > 0 ? "+" : "") + std::to_string(powerCost);
    const SDL_Color color = powerCost >= 0 ? col::Ok : SDL_Color {255, 170, 80, 255};
    const int tagHeight = std::clamp(cell.h * 18 / 84, 15, 28);
    const float iconScale = static_cast<float>(std::max(11, tagHeight - 5)) / 19.0f;
    const int iconW = iconSpriteId >= 0 ? static_cast<int>(std::lround(23.0f * iconScale)) : 0;
    const int iconH = iconSpriteId >= 0 ? static_cast<int>(std::lround(19.0f * iconScale)) : 0;
    const int gap = iconSpriteId >= 0 ? 3 : 0;
    const int padX = 5;
    const int textW = fontRenderer.GetTextWidth(text, ui::FONT_SMALL_BOLD);
    const int tagWidth = padX + iconW + gap + textW + padX;
    const UiRect tagRect {cell.x + 3, cell.y + 3, tagWidth, tagHeight};
    renderer.FillRectScreen(tagRect.x, tagRect.y, tagRect.w, tagRect.h, {6, 8, 12, 215});
    if (iconSpriteId >= 0)
        renderer.DrawRawSpriteScreen(
            iconSpriteId, tagRect.x + padX, tagRect.y + (tagRect.h - iconH) / 2, iconScale);
    fontRenderer.DrawString(
        text,
        tagRect.x + padX + iconW + gap,
        tagRect.CenterY(),
        ui::FONT_SMALL_BOLD,
        color,
        false,
        Anchor::LEFT | Anchor::VCENTER);
}

// Common cameo frame: icon, bottom name band, hover/selection border
void DrawCameo(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const UiRect &cell,
    int compId,
    std::string_view name,
    bool hovered,
    bool selected,
    bool locked)
{
    ui::DrawPanel(renderer, cell, col::PanelSunken, col::Border);
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
    ui::DrawPanelFrame(renderer, cell, border);
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
        renderer.FillRectScreen(
            cell.x + 1, cell.Bottom() - bandHeight - coverHeight, cell.w - 2, coverHeight, {6, 8, 12, 160});
    renderer.FillRectScreen(
        cell.x + 1, cell.Bottom() - bandHeight - 2, static_cast<int>((cell.w - 2) * progress), 2, tint);
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
    bool zoomButtons,
    bool tactical)
{
    // The tactical map is tinted with the local player's army colour: dark ground, darker space round the map
    SDL_Color groundColor {14, 30, 22, 255}, spaceColor {6, 12, 10, 255}, grassColor {30, 54, 40, 255};
    if (tactical)
    {
        const int colorIndex = Players::Slot(Players::Local()).color;
        const SDL_Color army = colorIndex >= 1 ? ArmyColor(colorIndex) : SDL_Color {60, 120, 90, 255};
        auto shade = [&](float k) {
            return SDL_Color {
                static_cast<Uint8>(army.r * k), static_cast<Uint8>(army.g * k), static_cast<Uint8>(army.b * k), 255};
        };
        groundColor = shade(0.22f);
        spaceColor = shade(0.09f);
        grassColor = shade(0.42f); // what is not walkable (grass, trees, cliffs): the same hue, lighter than the ground
    }
    renderer.FillRectScreen(radarRect.x, radarRect.y, radarRect.w, radarRect.h, spaceColor);
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
    if (tactical)
    {
        // The camera's own mapping (tiles are 30 x 20 world pixels), so every click lands where it is drawn
        view = RadarView {};
        view.scale = camera.GetZoom() * TILE_WIDTH;
        view.scaleY = camera.GetZoom() * TILE_HEIGHT;
        view.viewX = camera.GetX() / TILE_WIDTH;
        view.viewY = camera.GetY() / TILE_HEIGHT;
        view.ox = radarRect.x;
        view.oy = radarRect.y;
        view.w = radarRect.w;
        view.h = radarRect.h;
    }
    if (view.w > 0 && view.h > 0)
    {
        const SDL_Rect radarClip {view.ox, view.oy, view.w, view.h};
        renderer.FillRectScreen(radarRect.x, radarRect.y, radarRect.w, radarRect.h, spaceColor);
        auto fillClamped = [&](int x, int y, int w, int h, SDL_Color color) {
            const int x0 = std::max(x, view.ox);
            const int y0 = std::max(y, view.oy);
            const int x1 = std::min(x + w, view.ox + view.w);
            const int y1 = std::min(y + h, view.oy + view.h);
            if (x1 > x0 && y1 > y0)
                renderer.FillRectScreen(x0, y0, x1 - x0, y1 - y0, color);
        };
        if (tactical)
        {
            // The map's own ground is lighter than the space the view shows beyond it
            const int gx0 = static_cast<int>(std::lround(view.ToScreenX(0.0f)));
            const int gy0 = static_cast<int>(std::lround(view.ToScreenY(0.0f)));
            const int gx1 = static_cast<int>(std::lround(view.ToScreenX(static_cast<float>(mapWidth))));
            const int gy1 = static_cast<int>(std::lround(view.ToScreenY(static_cast<float>(mapHeight))));
            fillClamped(gx0, gy0, gx1 - gx0, gy1 - gy0, groundColor);
        }
        else
            renderer.FillRectScreen(view.ox, view.oy, view.w, view.h, {14, 30, 22, 255});
        const bool fogEnabled = m_radarFog && map.IsFogEnabled() && !m_showAll;
        const int minTileX = std::max(0, static_cast<int>(view.viewX));
        const int minTileY = std::max(0, static_cast<int>(view.viewY));
        const int maxTileX = std::min(mapWidth, static_cast<int>(std::ceil(view.viewX + view.w / view.scale)) + 1);
        const int maxTileY = std::min(mapHeight, static_cast<int>(std::ceil(view.viewY + view.h / view.scaleY)) + 1);
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
                    fillClamped(pixelX, pixelY, pixelWidth, pixelHeight, hiddenColor);
                    continue;
                }
                // Walls as the local player knows them: under the fog of war the ones seen there last
                const std::uint16_t wall = fogEnabled ? map.ViewerWall(tileX, tileY) : map.GetWall(tileX, tileY);
                if (wall > 0)
                {
                    const PlayerId owner
                        = fogEnabled ? map.ViewerWallOwner(tileX, tileY) : map.GetWallOwner(tileX, tileY);
                    fillClamped(
                        pixelX,
                        pixelY,
                        pixelWidth,
                        pixelHeight,
                        RadarOwnerColor(owner, wall >= 1101 ? Faction::Resistance : Faction::Confederation, false));
                }
                else if (map.GetWall(tileX, tileY) == 0 && !map.IsPassable(tileX, tileY)) // an unseen wall is no
                                                                                          // terrain
                {
                    fillClamped(pixelX, pixelY, pixelWidth, pixelHeight, grassColor);
                }
            }
        }

        // Tactical map: translucent grid lines over the map, coarser when zoomed far out
        if (tactical && m_tacticalGridAlpha > 0)
        {
            int step = 1;
            while (step < 64 && std::min(view.scale, view.scaleY) * static_cast<float>(step) < m_tacticalGridMinPx)
                step *= 2;
            const SDL_Color lineColor {255, 255, 255, static_cast<Uint8>(m_tacticalGridAlpha)};
            const SDL_Color majorColor {255, 255, 255, static_cast<Uint8>(std::min(255, m_tacticalGridAlpha * 2))};
            const int gy0 = static_cast<int>(std::lround(view.ToScreenY(0.0f)));
            const int gy1 = static_cast<int>(std::lround(view.ToScreenY(static_cast<float>(mapHeight))));
            const int gx0 = static_cast<int>(std::lround(view.ToScreenX(0.0f)));
            const int gx1 = static_cast<int>(std::lround(view.ToScreenX(static_cast<float>(mapWidth))));
            for (int tx = (minTileX / step) * step; tx <= maxTileX && tx <= mapWidth; tx += step)
            {
                const int px = static_cast<int>(std::lround(view.ToScreenX(static_cast<float>(tx))));
                fillClamped(px, gy0, 1, gy1 - gy0, tx % (step * 4) == 0 ? majorColor : lineColor);
            }
            for (int ty = (minTileY / step) * step; ty <= maxTileY && ty <= mapHeight; ty += step)
            {
                const int py = static_cast<int>(std::lround(view.ToScreenY(static_cast<float>(ty))));
                fillClamped(gx0, py, gx1 - gx0, 1, ty % (step * 4) == 0 ? majorColor : lineColor);
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
                if (tactical)
                {
                    // A small building stays a clear square around its middle: it must be easy to hit and to see
                    const int minSide = static_cast<int>(m_tacticalBuildingMinPx);
                    const int side = std::max(pixelWidth, minSide), sideY = std::max(pixelHeight, minSide);
                    pixelX -= (side - pixelWidth) / 2;
                    pixelY -= (sideY - pixelHeight) / 2;
                    pixelWidth = side;
                    pixelHeight = sideY;
                    fillClamped(
                        pixelX, pixelY, pixelWidth, pixelHeight, RadarOwnerColor(entity.owner, entity.faction, false));
                }
                else
                {
                    fillClamped(
                        pixelX,
                        pixelY,
                        std::max(3, pixelWidth),
                        std::max(3, pixelHeight),
                        RadarOwnerColor(entity.owner, entity.faction, false));
                }
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
                const int unitSize = std::max(tactical ? 5 : 2, std::min(pixelWidth, pixelHeight));
                fillClamped(
                    pixelX + (pixelWidth - unitSize) / 2,
                    pixelY + (pixelHeight - unitSize) / 2,
                    unitSize,
                    unitSize,
                    RadarOwnerColor(entity.owner, entity.faction, true));
            }
            if (tactical && entity.selected && isOwn)
            {
                // Selected: a white frame round the dot
                const int cx = pixelX + pixelWidth / 2, cy = pixelY + pixelHeight / 2;
                const int half = std::max(pixelWidth, pixelHeight) / 2 + 2;
                const SDL_Color white {255, 255, 255, 235};
                fillClamped(cx - half, cy - half, 2 * half, 1, white);
                fillClamped(cx - half, cy + half - 1, 2 * half, 1, white);
                fillClamped(cx - half, cy - half, 1, 2 * half, white);
                fillClamped(cx + half - 1, cy - half, 1, 2 * half, white);
            }
            if (!tactical && !enemyOnRadar && !isOwn && entity.faction != Faction::Neutral
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
        if (!tactical)
            m_radarAlert.Update(nowMs, enemyOnRadar, contactX, contactY);
        // (the tactical map only shows the alarm the sidebar radar keeps up to date)
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
                    DrawCircleScreen(renderer, centerX, centerY, radius + static_cast<float>(k), waveColor, radarClip);
            }
            if (m_radarAlert.BlinkOn(nowMs) && alertConfig.dotRadiusPx > 0.0f)
                FillCircleScreen(renderer, centerX, centerY, alertConfig.dotRadiusPx, alertConfig.color, radarClip);
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
        if (!tactical)
        {
            // The frame's four sides, each cut to the radar's picture by hand (no scissor)
            const int fw = std::max(6, frameWidth), fh = std::max(4, frameHeight);
            const SDL_Color white {255, 255, 255, 220};
            auto side = [&](int x, int y, int w, int h) {
                const int x0 = std::max(x, view.ox), y0 = std::max(y, view.oy);
                const int x1 = std::min(x + w, view.ox + view.w), y1 = std::min(y + h, view.oy + view.h);
                if (x1 > x0 && y1 > y0)
                    renderer.FillRectScreen(x0, y0, x1 - x0, y1 - y0, white);
            };
            side(frameX, frameY, fw, 1);
            side(frameX, frameY + fh - 1, fw, 1);
            side(frameX, frameY, 1, fh);
            side(frameX + fw - 1, frameY, 1, fh);
        }
        {
            const SDL_Color back {6, 12, 10, 255};
            const int top = std::max(radarRect.y, view.oy), bottom = std::min(radarRect.Bottom(), view.oy + view.h);
            if (view.oy > radarRect.y)
                renderer.FillRectScreen(radarRect.x, radarRect.y, radarRect.w, view.oy - radarRect.y, back);
            if (view.oy + view.h < radarRect.Bottom())
                renderer.FillRectScreen(
                    radarRect.x, view.oy + view.h, radarRect.w, radarRect.Bottom() - (view.oy + view.h), back);
            if (bottom > top && view.ox > radarRect.x)
                renderer.FillRectScreen(radarRect.x, top, view.ox - radarRect.x, bottom - top, back);
            if (bottom > top && view.ox + view.w < radarRect.Right())
                renderer.FillRectScreen(
                    view.ox + view.w, top, radarRect.Right() - (view.ox + view.w), bottom - top, back);
        }
    }
    if (tactical)
        return;
    ui::DrawPanelFrame(renderer, radarRect, col::BorderStrong);

    // Radar alarm: the radar's sides flash red, the band fading inward
    if (m_radarAlert.BlinkOn(SDL_GetTicks()))
    {
        const RadarAlertConfig &alertConfig = m_radarAlert.config;
        for (int k = 0; k < alertConfig.edgeWidthPx; ++k)
        {
            SDL_Color edgeColor = alertConfig.color;
            edgeColor.a
                = static_cast<Uint8>(alertConfig.edgeAlpha * (alertConfig.edgeWidthPx - k) / alertConfig.edgeWidthPx);
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
        const SDL_Color topFill = hovered ? ui::Mix(ui::color::AccentDeep, col::Accent, 0.38f)
                                          : ui::Mix(col::PanelRaised, ui::color::AccentDeep, 0.52f);
        const SDL_Color botFill = enabled ? ui::color::AccentDeep : col::PanelSunken;
        const SDL_Color border = hovered ? col::Accent : (enabled ? ui::Mix(col::BorderStrong, col::Accent, 0.35f) : col::Border);
        ui::DrawButtonChrome(
            renderer, buttonRect, topFill, botFill, border, {0, 0, 0, 0}, {0, 0, 0, 0}, enabled ? 235 : 170);
        const SDL_Color symbolColor {255, 255, 255, enabled ? Uint8(200) : Uint8(110)};
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

void HUDController::SetTacticalConfig(const nlohmann::json &section)
{
    if (!section.is_object())
        return;
    m_tacticalZoomThreshold = std::clamp(section.value("zoom_threshold", m_tacticalZoomThreshold), 0.1f, 4.0f);
    m_tacticalBuildingMinPx = std::clamp(section.value("building_min_px", m_tacticalBuildingMinPx), 3.0f, 64.0f);
    m_tacticalGridAlpha = std::clamp(section.value("grid_alpha", m_tacticalGridAlpha), 0, 255);
    m_tacticalGridMinPx = std::clamp(section.value("grid_min_px", m_tacticalGridMinPx), 8, 200);
}

void HUDController::SetTactical(bool on)
{
    if (on == m_tactical)
        return;
    m_tactical = on;
    if (on)
    {
        m_buildMode = false; // no building here: a half-placed one goes away with the picture
        if (m_activeTab == TabBuild)
            m_activeTab = TabInfantry;
    }
}

void HUDController::DrawTacticalMap(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const UiRect &area,
    const EntityManager &entities,
    const TileMap &map,
    const Camera &camera)
{
    DrawRadar(renderer, fontRenderer, area, entities, map, camera, -1, -1, false, true);
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
    ui::DrawPanel(renderer, layout.panel, col::Panel, col::BorderStrong);

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
        ui::DrawProgressBar(renderer, {headerRect.x, headerRect.Bottom() - 6, headerRect.w, 6}, powerRatio, powerColor);
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
            ui::DrawTab(renderer, tabRect, active, hovered && !(m_tactical && tabIndex == TabBuild), col::Accent);

            if (TabIcon(tabIndex) >= 0)
            {
                const int insetBottom = active ? 5 : 4;
                renderer.SetClipRect({tabRect.x + 2, tabRect.y + 2, tabRect.w - 4, tabRect.h - insetBottom});
                float iconScale = 0.75f;
                int iconWidth = static_cast<int>(kCameoWidth * iconScale);
                renderer.SetDrawOwner(Players::Local());
                renderer.SetStructureFaction(PlayerFaction());
                renderer.DrawCompositeScreen(
                    TabIcon(tabIndex), tabRect.CenterX() - iconWidth / 2, tabRect.y + 5, iconScale, PlayerFaction());
                renderer.SetStructureFaction(Faction::Neutral);
                renderer.SetDrawOwner(kNoPlayer);
                renderer.ResetClipRect();
                if (!active)
                    renderer.FillRectScreen(
                        tabRect.x + 2,
                        tabRect.y + 2,
                        tabRect.w - 4,
                        tabRect.h - 4,
                        {8, 10, 14, (m_tactical && tabIndex == TabBuild) ? Uint8(170) : (hovered ? Uint8(30) : Uint8(65))});
            }
            else
            {
                const SDL_Color crossColor = active ? col::White : (hovered ? col::Text : col::TextDim);
                DrawCrosshair(renderer, tabRect.CenterX(), tabRect.CenterY() - 1, 16, crossColor);
            }
        }

        // --- Strip: tab title, primary building, scroll arrows ---------------------------------------
        const int gridRows = (ItemCountForTab(m_activeTab) + SidebarLayout::kCols - 1) / SidebarLayout::kCols;
        const int gridContentH
            = gridRows > 0 ? gridRows * (SidebarLayout::kCellH + SidebarLayout::kGap) - SidebarLayout::kGap : 0;
        const float maxScrollPx = std::max(0.0f, static_cast<float>(gridContentH - layout.grid.h));
        auto &gridScroll = m_gridScroll[m_activeTab];
        gridScroll.offset = std::clamp(gridScroll.offset, 0.0f, maxScrollPx);
        gridScroll.target = std::clamp(gridScroll.target, 0.0f, maxScrollPx);
        const int scrollPx = gridScroll.PixelOffset();
        {
            ui::DrawCaption(fontRenderer, renderer, layout.strip.x, layout.strip.y + 4, _(kTabNames[m_activeTab]));

            if (m_activeTab == TabInfantry || m_activeTab == TabVehicles)
            {
                bool isInfantry = (m_activeTab == TabInfantry);
                int primaryId = isInfantry ? m_primaryBarracksId : m_primaryFactoryId;
                auto buildings = entities.GetBuildingsByType(
                    isInfantry ? UnitType::Barracks : UnitType::Factory, Players::Local());
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

            if (maxScrollPx > 0.0f)
            {
                auto drawArrow = [&](const UiRect &arrowRect, bool isUp, bool enabled) {
                    bool hovered = enabled && arrowRect.Contains(mouseX, mouseY);
                    ui::DrawPanel(
                        renderer,
                        arrowRect,
                        hovered ? ui::Mix(ui::color::AccentDeep, col::Accent, 0.35f)
                                : ui::Mix(col::PanelRaised, ui::color::AccentDeep, 0.50f),
                        hovered ? col::Accent : ui::Mix(col::BorderStrong, col::Accent, 0.35f));
                    SDL_Color arrowColor {255, 255, 255, enabled ? Uint8(200) : Uint8(110)};
                    for (int i = 0; i < 5; ++i)
                    {
                        int rowY = isUp ? (arrowRect.CenterY() - 2 + i) : (arrowRect.CenterY() + 2 - i);
                        renderer.FillRectScreen(arrowRect.CenterX() - i, rowY, 2 * i + 1, 1, arrowColor);
                    }
                };
                drawArrow(layout.scrollUp, true, scrollPx > 0);
                drawArrow(layout.scrollDown, false, gridScroll.offset < maxScrollPx - 0.5f);
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
            UiRect cellRect = layout.Cell(i, scrollPx);
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
                    const int owned = limit > 0
                                          ? (isMine ? entities.CountMines(Players::Local(), blueprint.mineKind)
                                                    : BuildLimits::CountOwned(
                                                          entities, map, Players::Local(), blueprint.buildingIndex))
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
                            hoverText = std::string(_("[RESEARCH REQUIRED] "))
                                        + _(researchDef(
                                                PlayerFaction(), UnitUnlockResearch(PlayerFaction(), unitDef.archetype))
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
                    const SDL_Color cardTop = active
                                                  ? ui::Mix(col::Accent, col::PanelRaised, 0.58f)
                                                  : (hovered ? ui::Mix(col::Hover, ui::color::AccentDeep, 0.45f)
                                                             : ui::Mix(col::PanelRaised, ui::color::AccentDeep, 0.16f));
                    const SDL_Color cardBot = active ? ui::color::AccentDeep : col::PanelSunken;
                    const SDL_Color cardBorder
                        = active ? col::Accent : (hovered ? ui::Mix(col::Accent, col::White, 0.2f) : col::BorderStrong);
                    const SDL_Color cardCorners = (active || hovered) ? col::Accent : col::BorderStrong;
                    ui::DrawButtonChrome(renderer, cellRect, cardTop, cardBot, cardBorder, {0, 0, 0, 0}, cardCorners);
                    fontRenderer.DrawString(
                        ui::FitText(fontRenderer, _(orderDef.name), ui::FONT_BODY_BOLD, cellRect.w - 16),
                        cellRect.CenterX(),
                        cellRect.CenterY() + 6,
                        ui::FONT_BODY_BOLD,
                        active ? col::Accent : (hovered ? col::White : col::Text),
                        true,
                        Anchor::HCENTER | Anchor::VCENTER);
                    if (orderDef.key[0] != '\0')
                    {
                        UiRect keyChipRect {cellRect.x + 8, cellRect.y + 6, 22, 22};
                        ui::DrawButtonChrome(
                            renderer,
                            keyChipRect,
                            col::PanelRaised,
                            col::PanelSunken,
                            active ? col::Accent : col::BorderStrong,
                            {0, 0, 0, 0},
                            {0, 0, 0, 0});
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
        if (maxScrollPx > 0.0f)
        {
            int trackX = layout.panel.Right() - 5;
            renderer.FillRectScreen(trackX, layout.grid.y, 2, layout.grid.h, col::PanelRaised);
            int thumbHeight = std::max(16, layout.grid.h * layout.grid.h / std::max(1, gridContentH));
            int thumbY
                = layout.grid.y + static_cast<int>((layout.grid.h - thumbHeight) * (gridScroll.offset / maxScrollPx));
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
                    ui::DrawButton(
                        renderer,
                        fontRenderer,
                        repairRect,
                        isRepairing ? _("REPAIRING") : _("REPAIR [R]"),
                        isRepairing ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
                        repairHovered,
                        true,
                        ui::FONT_SMALL_BOLD);
                }
            }
            if (!OwnSelectedBuildings(entities, selected).empty())
            {
                const UiRect destroyRect = DestroyBtn(infoRect);
                const bool armed = m_destroyArmed > 0.0f;
                const bool destroyHovered = destroyRect.Contains(mouseX, mouseY);
                ui::DrawButton(
                    renderer,
                    fontRenderer,
                    destroyRect,
                    armed ? _("CONFIRM?") : _("DESTROY"),
                    ui::ButtonStyle::Danger,
                    destroyHovered || armed,
                    true,
                    ui::FONT_SMALL_BOLD);
            }
        }
    }
}

} // namespace OpenArtOfWar
