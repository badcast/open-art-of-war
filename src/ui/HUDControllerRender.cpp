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

const char *const kTabNames[HUDController::TabCount] = {"BUILD", "INFANTRY", "VEHICLES", "ORDERS"};

// Tab icons per faction: HQ, rifleman, main tank, a research cameo; Orders draws a crosshair
const char *const kResistanceTabIcons[HUDController::TabCount]
    = {"icons_res.headquarters", "icons_res.rifleman", "icons_res.assault_tank", ""};

const char *const kConfederationTabIcons[HUDController::TabCount]
    = {"icons_conf.command_center", "icons_conf.rifleman", "icons_conf.hammer", ""};

int TabIcon(int tab)
{
    const char *name = (PlayerFaction() == Faction::Confederation ? kConfederationTabIcons : kResistanceTabIcons)[tab];
    return SpriteHandle(name, SpriteKind::Part, "sidebar tab icon");
}

constexpr int kCameoW = 60; // native cameo composite size

// A true circle outline in screen pixels (64 segments)
void DrawCircleScreen(Renderer &renderer, float cx, float cy, float radius, SDL_Color color)
{
    constexpr int kSegments = 64;
    float px = cx + radius, py = cy;
    for (int i = 1; i <= kSegments; ++i)
    {
        const float a = 6.28318530718f * static_cast<float>(i) / kSegments;
        const float nx = cx + radius * std::cos(a), ny = cy + radius * std::sin(a);
        renderer.DrawLineScreen(
            static_cast<int>(std::lround(px)),
            static_cast<int>(std::lround(py)),
            static_cast<int>(std::lround(nx)),
            static_cast<int>(std::lround(ny)),
            color);
        px = nx;
        py = ny;
    }
}

// A filled disc, one horizontal span per pixel row
void FillCircleScreen(Renderer &renderer, float cx, float cy, float radius, SDL_Color color)
{
    const int r = static_cast<int>(std::ceil(radius));
    for (int dy = -r; dy <= r; ++dy)
    {
        const float span = std::sqrt(std::max(0.0f, radius * radius - static_cast<float>(dy * dy)));
        const int x0 = static_cast<int>(std::lround(cx - span)), x1 = static_cast<int>(std::lround(cx + span));
        renderer.FillRectScreen(x0, static_cast<int>(std::lround(cy)) + dy, std::max(1, x1 - x0), 1, color);
    }
}

SDL_Color RadarFactionColor(Faction f, bool unit);

// A player's own army colour on the radar when it has one, else the classic colour of its faction
SDL_Color RadarOwnerColor(PlayerId owner, Faction f, bool unit)
{
    if (owner < kMaxPlayers && Players::IsActive(owner) && Players::Slot(owner).color > 0)
    {
        SDL_Color c = ArmyColor(Players::Slot(owner).color);
        if (unit) // units a little lighter than buildings, as with the classic colours
            c
                = {static_cast<Uint8>(c.r + (255 - c.r) / 3),
                   static_cast<Uint8>(c.g + (255 - c.g) / 3),
                   static_cast<Uint8>(c.b + (255 - c.b) / 3),
                   255};
        return c;
    }
    return RadarFactionColor(f, unit);
}

SDL_Color RadarFactionColor(Faction f, bool unit)
{
    switch (f)
    {
        case Faction::Resistance:
            return unit ? SDL_Color {255, 160, 140, 255} : SDL_Color {236, 92, 76, 255};
        case Faction::Confederation:
            return unit ? SDL_Color {150, 215, 255, 255} : SDL_Color {72, 170, 236, 255};
        default:
            return unit ? SDL_Color {190, 230, 150, 255} : SDL_Color {120, 190, 90, 255};
    }
}

std::string UnitName(const UnitCameoDef &def, const EntityManager &entities)
{
    const auto *cfg = entities.GetUnitConfig(def.archetype);
    return (cfg && !cfg->name.empty()) ? cfg->name : std::string(def.name);
}

int UnitCost(const UnitCameoDef &def, const EntityManager &entities)
{
    const auto *cfg = entities.GetUnitConfig(def.archetype);
    return cfg ? cfg->creditCost : def.cost;
}

std::string UnitDesc(const UnitCameoDef &def, const EntityManager &entities)
{
    const auto *cfg = entities.GetUnitConfig(def.archetype);
    return (cfg && !cfg->description.empty()) ? cfg->description : std::string(def.desc);
}

void DrawInfinity(Renderer &r, const UiRect &b, bool on, bool hovered)
{
    r.FillRectScreen(
        b.x, b.y, b.w, b.h, on ? SDL_Color {60, 44, 20, 235} : SDL_Color {6, 8, 12, hovered ? Uint8(235) : Uint8(190)});
    r.DrawRectScreen(b.x, b.y, b.w, b.h, on ? col::Accent : (hovered ? col::BorderStrong : col::Border));
    // two rings side by side: the infinity sign
    const SDL_Color c = on ? col::Accent : col::TextDim;
    r.DrawRingScreen(static_cast<float>(b.x + 7), static_cast<float>(b.CenterY()), 4.0f, 3.5f, 1.6f, c, 24);
    r.DrawRingScreen(static_cast<float>(b.x + 17), static_cast<float>(b.CenterY()), 4.0f, 3.5f, 1.6f, c, 24);
}

void DrawCostTag(Renderer &r, BitmapFontRenderer &font, const UiRect &cell, const std::string &text, SDL_Color color)
{
    int tw = font.GetTextWidth(text, ui::FONT_SMALL_BOLD) + 10;
    UiRect tag {cell.Right() - tw - 3, cell.y + 3, tw, 18};
    r.FillRectScreen(tag.x, tag.y, tag.w, tag.h, {6, 8, 12, 215});
    font.DrawString(
        text, tag.CenterX(), tag.CenterY(), ui::FONT_SMALL_BOLD, color, false, Anchor::HCENTER | Anchor::VCENTER);
}

// Common cameo frame: icon, bottom name band, hover/selection border
void DrawCameo(
    Renderer &r,
    BitmapFontRenderer &font,
    const UiRect &cell,
    int compId,
    const std::string &name,
    bool hovered,
    bool selected,
    bool locked)
{
    r.FillRectScreen(cell.x, cell.y, cell.w, cell.h, col::PanelSunken);
    if (compId >= 0)
    {
        int iconX = cell.x + (cell.w - kCameoW * 2) / 2;
        int iconY = cell.y + 3;
        // The icons wear the player's army colour, like its buildings and units
        r.SetDrawOwner(Players::Local());
        r.SetStructureFaction(PlayerFaction());
        r.DrawCompositeScreen(compId, iconX, iconY, 2.0f, PlayerFaction());
        r.SetStructureFaction(Faction::Neutral);
        r.SetDrawOwner(kNoPlayer);
    }
    if (locked)
    {
        r.FillRectScreen(cell.x, cell.y, cell.w, cell.h, {10, 12, 16, 170});
    }
    UiRect band {cell.x, cell.Bottom() - 20, cell.w, 20};
    r.FillRectScreen(band.x, band.y, band.w, band.h, {6, 8, 12, 225});
    font.DrawString(
        ui::FitText(font, name, ui::FONT_SMALL_BOLD, band.w - 10),
        band.CenterX(),
        band.CenterY(),
        ui::FONT_SMALL_BOLD,
        locked ? col::TextMuted : (selected ? col::Accent : col::Text),
        false,
        Anchor::HCENTER | Anchor::VCENTER);

    SDL_Color border = selected ? col::Accent : (hovered ? col::BorderStrong : col::Border);
    r.DrawRectScreen(cell.x, cell.y, cell.w, cell.h, border);
    if (selected)
        r.DrawRectScreen(cell.x + 1, cell.y + 1, cell.w - 2, cell.h - 2, ui::WithAlpha(col::Accent, 120));
}

// Progress "shutter" that uncovers the cameo from the top while an item is being produced
void DrawProgressShutter(Renderer &r, BitmapFontRenderer &font, const UiRect &cell, float progress, SDL_Color tint)
{
    int coverH = static_cast<int>((cell.h - 20) * (1.0f - std::clamp(progress, 0.0f, 1.0f)));
    if (coverH > 0)
        r.FillRectScreen(cell.x + 1, cell.Bottom() - 20 - coverH, cell.w - 2, coverH, {6, 8, 12, 160});
    r.FillRectScreen(cell.x + 1, cell.Bottom() - 22, static_cast<int>((cell.w - 2) * progress), 2, tint);
    std::string pct = std::to_string(static_cast<int>(progress * 100.0f)) + "%";
    font.DrawString(
        pct,
        cell.CenterX(),
        cell.y + (cell.h - 20) / 2,
        ui::FONT_H2,
        ui::color::Text,
        true,
        Anchor::HCENTER | Anchor::VCENTER);
}

void DrawLockBadge(Renderer &r, BitmapFontRenderer &font, const UiRect &cell)
{
    font.DrawString(
        _("LOCK"),
        cell.CenterX(),
        cell.y + (cell.h - 20) / 2,
        ui::FONT_SMALL_BOLD,
        col::Danger,
        true,
        Anchor::HCENTER | Anchor::VCENTER);
    (void) r;
}

// Crosshair glyph used as the Orders tab icon
void DrawCrosshair(Renderer &r, int cx, int cy, int size, SDL_Color c)
{
    r.DrawRectScreen(cx - size / 2, cy - size / 2, size, size, c);
    r.FillRectScreen(cx - size / 2 - 4, cy, size + 9, 1, c);
    r.FillRectScreen(cx, cy - size / 2 - 4, 1, size + 9, c);
}

} // namespace

void HUDController::RenderGameHud(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const Economy &economy,
    const EntityManager &entities,
    const TileMap &map,
    const Camera &camera,
    int mx,
    int my,
    int windowW,
    int windowH,
    const std::vector<int> &selected)
{
    SidebarLayout l = sidebarLayout(windowW, windowH, m_topInset, !m_observer && m_speedEnabled);

    // Frame
    ui::FillVerticalGradient(renderer, l.panel, {20, 24, 32, 255}, {12, 15, 20, 255});
    renderer.FillRectScreen(l.panel.x, 0, 1, windowH, col::BorderStrong);
    renderer.FillRectScreen(l.panel.x + 1, 0, 1, windowH, {0, 0, 0, 120});

    // --- Header: credits & power ---------------------------------------------------------
    {
        const UiRect &h = l.header;
        std::string credits = "$ " + std::to_string(economy.GetCredits());
        fontRenderer.DrawString(credits, h.x, h.y - 2, ui::FONT_H2, col::Accent, true);

        int prod = economy.GetPowerProduced();
        int cons = economy.GetPowerConsumed();
        bool deficit = economy.HasPowerDeficit();
        SDL_Color pCol = deficit ? col::Danger : col::Ok;
        std::string pwr = std::to_string(cons) + " / " + std::to_string(prod) + _(" kW");
        fontRenderer.DrawString(pwr, h.Right(), h.y + 2, ui::FONT_SMALL_BOLD, pCol, false, Anchor::RIGHT | Anchor::TOP);
        // The lightning of the game next to the kW figures; it blinks while the power is short
        if (!deficit || PowerIconBlinkOn())
        {
            const int textW = fontRenderer.GetTextWidth(pwr, ui::FONT_SMALL_BOLD);
            renderer.DrawRawSpriteScreen(PowerIconSprite(), h.Right() - textW - 27, h.y, 1.0f);
        }
        fontRenderer.DrawString(
            _("POWER"), h.Right(), h.y + 17, ui::FONT_SMALL, col::TextMuted, false, Anchor::RIGHT | Anchor::TOP);

        float ratio = static_cast<float>(cons) / static_cast<float>(std::max(1, prod));
        ui::DrawProgressBar(renderer, {h.x, h.Bottom() - 6, h.w, 6}, ratio, pCol);
    }

    // --- Radar -----------------------------------------------------------------------------
    {
        const UiRect &rd = l.radar;
        renderer.FillRectScreen(rd.x, rd.y, rd.w, rd.h, {6, 12, 10, 255});
        const int mapW = map.GetWidth();
        const int mapH = map.GetHeight();
        // A zoomed radar follows the camera, except while the player drags on it (that moves the camera)
        const float camCenterX = (camera.GetX() + camera.GetViewportWidth() * 0.5f / camera.GetZoom()) / TILE_WIDTH;
        const float camCenterY = (camera.GetY() + camera.GetViewportHeight() * 0.5f / camera.GetZoom()) / TILE_HEIGHT;
        if (!m_radarDragging || m_radarCenterX < 0.0f)
        {
            m_radarCenterX = camCenterX;
            m_radarCenterY = camCenterY;
        }
        RadarView v = radarView(rd, mapW, mapH, m_radarZoom, m_radarCenterX, m_radarCenterY);
        if (v.w > 0 && v.h > 0)
        {
            renderer.SetClipRect({v.ox, v.oy, v.w, v.h});
            renderer.FillRectScreen(v.ox, v.oy, v.w, v.h, {14, 30, 22, 255});
            const bool fog = m_radarFog && map.IsFogEnabled() && !m_showAll;
            const int tx0 = std::max(0, static_cast<int>(v.viewX));
            const int ty0 = std::max(0, static_cast<int>(v.viewY));
            const int tx1 = std::min(mapW, static_cast<int>(std::ceil(v.viewX + v.w / v.scale)) + 1);
            const int ty1 = std::min(mapH, static_cast<int>(std::ceil(v.viewY + v.h / v.scale)) + 1);
            // Cells from rounded edges: neighbours meet exactly, nothing shimmers as the view moves
            auto cellRect = [&](float x, float y, float w, float h, int &px, int &py, int &pw, int &ph) {
                px = static_cast<int>(std::lround(v.ToScreenX(x)));
                py = static_cast<int>(std::lround(v.ToScreenY(y)));
                pw = std::max(1, static_cast<int>(std::lround(v.ToScreenX(x + w))) - px);
                ph = std::max(1, static_cast<int>(std::lround(v.ToScreenY(y + h))) - py);
            };
            const SDL_Color hiddenCol {4, 6, 8, 255};
            for (int ty = ty0; ty < ty1; ++ty)
            {
                for (int tx = tx0; tx < tx1; ++tx)
                {
                    int px, py, pw, ph;
                    cellRect(static_cast<float>(tx), static_cast<float>(ty), 1.0f, 1.0f, px, py, pw, ph);
                    const FogState fs = fog ? map.GetFogState(tx, ty) : FogState::Visible;
                    if (fs == FogState::Hidden)
                    {
                        renderer.FillRectScreen(px, py, pw, ph, hiddenCol);
                        continue;
                    }
                    // Walls as the local player knows them: under the fog of war the ones seen there last
                    const std::uint16_t wall = fog ? map.ViewerWall(tx, ty) : map.GetWall(tx, ty);
                    if (wall > 0)
                    {
                        const PlayerId owner = fog ? map.ViewerWallOwner(tx, ty) : map.GetWallOwner(tx, ty);
                        renderer.FillRectScreen(
                            px,
                            py,
                            pw,
                            ph,
                            RadarOwnerColor(owner, wall >= 1101 ? Faction::Resistance : Faction::Confederation, false));
                    }
                    else if (map.GetWall(tx, ty) == 0 && !map.IsPassable(tx, ty)) // an unseen wall is no terrain
                    {
                        renderer.FillRectScreen(px, py, pw, ph, {30, 54, 40, 255});
                    }
                }
            }

            // First enemy drawn on the radar this frame: the radar alarm's contact point
            bool enemyOnRadar = false;
            float contactX = 0.0f, contactY = 0.0f;
            for (const auto &e : entities.GetAllEntities())
            {
                if (!e.alive || e.state == ActionState::Die)
                    continue;
                const bool own = (e.owner == Players::Local());
                const Building *b = e.AsBuilding();
                if (fog && !own)
                {
                    if (b)
                    {
                        // Enemy structures appear once they have been seen (never while they are being built out of
                        // sight)
                        if (!entities.IsHostileBuildingShown(*b, map))
                            continue;
                    }
                    else if (!entities.IsEntityVisibleToPlayer(e, Players::Local(), map))
                    {
                        continue; // units only while in sight
                    }
                }
                int px, py, pw, ph;
                if (b)
                {
                    cellRect(
                        static_cast<float>(b->tileX),
                        static_cast<float>(b->tileY),
                        static_cast<float>(b->buildWidth),
                        static_cast<float>(b->buildHeight),
                        px,
                        py,
                        pw,
                        ph);
                    renderer.FillRectScreen(
                        px, py, std::max(3, pw), std::max(3, ph), RadarOwnerColor(e.owner, e.faction, false));
                }
                else
                {
                    cellRect(
                        e.GetUnitCenterPixelX() / TILE_WIDTH - 0.5f,
                        e.GetUnitCenterPixelY() / TILE_HEIGHT - 0.5f,
                        1.0f,
                        1.0f,
                        px,
                        py,
                        pw,
                        ph);
                    const int s = std::max(2, std::min(pw, ph));
                    renderer.FillRectScreen(
                        px + (pw - s) / 2, py + (ph - s) / 2, s, s, RadarOwnerColor(e.owner, e.faction, true));
                }
                if (!enemyOnRadar && !own && e.faction != Faction::Neutral
                    && Players::Hostile(e.owner, Players::Local()) && (!b || m_radarAlert.config.includeBuildings))
                {
                    enemyOnRadar = true;
                    contactX = e.GetUnitCenterPixelX() / TILE_WIDTH;
                    contactY = e.GetUnitCenterPixelY() / TILE_HEIGHT;
                    if (b)
                    {
                        contactX = b->tileX + b->buildWidth * 0.5f;
                        contactY = b->tileY + b->buildHeight * 0.5f;
                    }
                }
            }

            // Radar alarm: red waves and a flashing dot at the contact (true circles in screen pixels)
            const std::uint64_t nowMs = SDL_GetTicks();
            m_radarAlert.Update(nowMs, enemyOnRadar, contactX, contactY);
            if (m_radarAlert.Active())
            {
                const RadarAlertConfig &ac = m_radarAlert.config;
                const float cx = v.ToScreenX(m_radarAlert.TileX());
                const float cy = v.ToScreenY(m_radarAlert.TileY());
                for (int i = 0; i < ac.waveCount; ++i)
                {
                    float radius = 0.0f, opacity = 0.0f;
                    if (!m_radarAlert.Wave(i, nowMs, radius, opacity))
                        continue;
                    SDL_Color wc = ac.color;
                    wc.a = static_cast<Uint8>(ac.color.a * opacity);
                    for (int k = 0; k < ac.waveThicknessPx; ++k)
                        DrawCircleScreen(renderer, cx, cy, radius + static_cast<float>(k), wc);
                }
                if (m_radarAlert.BlinkOn(nowMs) && ac.dotRadiusPx > 0.0f)
                    FillCircleScreen(renderer, cx, cy, ac.dotRadiusPx, ac.color);
            }

            // Camera frame from rounded edges (truncating position and size separately made it wobble)
            const float camWorldW = camera.GetViewportWidth() / camera.GetZoom();
            const float camWorldH = camera.GetViewportHeight() / camera.GetZoom();
            int fx, fy, fw, fh;
            cellRect(
                camera.GetX() / TILE_WIDTH,
                camera.GetY() / TILE_HEIGHT,
                camWorldW / TILE_WIDTH,
                camWorldH / TILE_HEIGHT,
                fx,
                fy,
                fw,
                fh);
            renderer.DrawRectScreen(fx, fy, std::max(6, fw), std::max(4, fh), {255, 255, 255, 220});
            renderer.ResetClipRect();
        }
        renderer.DrawRectScreen(rd.x, rd.y, rd.w, rd.h, col::BorderStrong);

        // Radar alarm: the radar's sides flash red, the band fading inward
        if (m_radarAlert.BlinkOn(SDL_GetTicks()))
        {
            const RadarAlertConfig &ac = m_radarAlert.config;
            for (int k = 0; k < ac.edgeWidthPx; ++k)
            {
                SDL_Color ec = ac.color;
                ec.a = static_cast<Uint8>(ac.edgeAlpha * (ac.edgeWidthPx - k) / ac.edgeWidthPx);
                renderer.DrawRectScreen(rd.x + k, rd.y + k, rd.w - 2 * k, rd.h - 2 * k, ec);
            }
        }

        // Zoom buttons
        const UiRect zoomBtns[2] = {RadarZoomOutBtn(rd), RadarZoomInBtn(rd)};
        for (int k = 0; k < 2; ++k)
        {
            const UiRect &b = zoomBtns[k];
            const bool enabled = (k == 0) ? m_radarZoom > 1 : m_radarZoom < kRadarMaxZoom;
            const bool hot = enabled && b.Contains(mx, my);
            if (hot)
                SoundRequests::MarkHover();
            renderer.FillRectScreen(b.x, b.y, b.w, b.h, hot ? col::Hover : SDL_Color {10, 16, 14, 210});
            renderer.DrawRectScreen(b.x, b.y, b.w, b.h, enabled ? col::BorderStrong : col::Border);
            const SDL_Color sc = enabled ? col::Text : col::TextMuted;
            renderer.FillRectScreen(b.x + 5, b.CenterY() - 1, b.w - 10, 2, sc);
            if (k == 1)
                renderer.FillRectScreen(b.CenterX() - 1, b.y + 5, 2, b.h - 10, sc);
        }
        if (m_radarZoom > 1)
        {
            fontRenderer.DrawString(
                "x" + std::to_string(m_radarZoom),
                rd.x + 6,
                rd.Bottom() - 12,
                ui::FONT_SMALL_BOLD,
                col::TextDim,
                false,
                Anchor::LEFT | Anchor::VCENTER);
        }
    }

    // --- Menu / objectives -------------------------------------------------------------------
    ui::DrawButton(
        renderer,
        fontRenderer,
        l.menuBtn,
        ui::FitText(fontRenderer, _("MENU"), ui::FONT_SMALL, l.menuBtn.w - 8),
        ui::ButtonStyle::Secondary,
        l.menuBtn.Contains(mx, my));
    ui::DrawButton(
        renderer,
        fontRenderer,
        l.objectivesBtn,
        ui::FitText(fontRenderer, _("OBJECTIVES"), ui::FONT_SMALL, l.objectivesBtn.w - 8),
        ui::ButtonStyle::Secondary,
        l.objectivesBtn.Contains(mx, my));
    if (!m_observer && m_speedEnabled)
    {
        char spdBuf[16];
        if (m_speedMultiplier < 1.0f)
            std::snprintf(spdBuf, sizeof(spdBuf), "x%.1f", m_speedMultiplier);
        else
            std::snprintf(spdBuf, sizeof(spdBuf), "x%d", static_cast<int>(m_speedMultiplier));
        ui::DrawButton(
            renderer,
            fontRenderer,
            l.slowerBtn,
            "-",
            ui::ButtonStyle::Secondary,
            l.slowerBtn.Contains(mx, my),
            m_speedMultiplier <= 0.5f);
        ui::DrawButton(
            renderer,
            fontRenderer,
            l.speedBtn,
            spdBuf,
            m_speedMultiplier != 1.0f ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
            l.speedBtn.Contains(mx, my));
        ui::DrawButton(
            renderer,
            fontRenderer,
            l.fasterBtn,
            "+",
            ui::ButtonStyle::Secondary,
            l.fasterBtn.Contains(mx, my),
            m_speedMultiplier >= 32.0f);
    }

    // A replay's observer has no buildings to command: only the radar, the header and the menu stay
    if (!m_observer)
    {
        ui::DrawButton(
            renderer,
            fontRenderer,
            l.researchBtn,
            ui::FitText(fontRenderer, _("RESEARCH NEW TECHNOLOGIES"), ui::FONT_SMALL, l.researchBtn.w - 12),
            ui::ButtonStyle::Secondary,
            l.researchBtn.Contains(mx, my));
        // --- Tabs ----------------------------------------------------------------------------------
        for (int t = 0; t < TabCount; ++t)
        {
            const UiRect &tr = l.tabs[t];
            bool active = (t == m_activeTab);
            bool hovered = tr.Contains(mx, my);
            if (hovered)
                SoundRequests::MarkHover();
            renderer.FillRectScreen(
                tr.x, tr.y, tr.w, tr.h, active ? col::Hover : (hovered ? col::PanelRaised : col::PanelSunken));
            renderer.DrawRectScreen(tr.x, tr.y, tr.w, tr.h, active ? col::BorderStrong : col::Border);
            if (active)
                renderer.FillRectScreen(tr.x, tr.Bottom() - 3, tr.w, 3, col::Accent);

            if (TabIcon(t) >= 0)
            {
                renderer.SetClipRect({tr.x + 1, tr.y + 1, tr.w - 2, tr.h - 4});
                float s = 0.75f;
                int iw = static_cast<int>(kCameoW * s);
                renderer.SetDrawOwner(Players::Local());
                renderer.SetStructureFaction(PlayerFaction());
                renderer.DrawCompositeScreen(TabIcon(t), tr.CenterX() - iw / 2, tr.y + 6, s, PlayerFaction());
                renderer.SetStructureFaction(Faction::Neutral);
                renderer.SetDrawOwner(kNoPlayer);
                renderer.ResetClipRect();
                if (!active)
                    renderer.FillRectScreen(
                        tr.x + 1, tr.y + 1, tr.w - 2, tr.h - 4, {10, 12, 16, hovered ? Uint8(60) : Uint8(110)});
            }
            else
            {
                DrawCrosshair(renderer, tr.CenterX(), tr.CenterY() - 1, 16, active ? col::Accent : col::TextDim);
            }
        }

        // --- Strip: tab title, primary building, scroll arrows ---------------------------------------
        int maxScroll = MaxScrollRows(m_activeTab, l.grid.h);
        int &scroll = m_scrollRow[m_activeTab];
        scroll = std::clamp(scroll, 0, maxScroll);
        {
            ui::DrawCaption(fontRenderer, renderer, l.strip.x, l.strip.y + 4, _(kTabNames[m_activeTab]));

            if (m_activeTab == TabInfantry || m_activeTab == TabVehicles)
            {
                bool inf = (m_activeTab == TabInfantry);
                int primary = inf ? m_primaryBarracksId : m_primaryFactoryId;
                auto list = entities.GetBuildingsByType(inf ? UnitType::Barracks : UnitType::Factory, Players::Local());
                if (!list.empty())
                {
                    std::string label
                        = std::string(inf ? _("Barracks") : _("Factory")) + " #" + std::to_string(primary);
                    if (list.size() > 1)
                        label += "  \xE2\x80\xBA";
                    bool hot = list.size() > 1 && l.primaryBtn.Contains(mx, my);
                    fontRenderer.DrawString(
                        ui::FitText(fontRenderer, label, ui::FONT_SMALL, l.primaryBtn.w),
                        l.primaryBtn.Right(),
                        l.primaryBtn.CenterY(),
                        ui::FONT_SMALL,
                        hot ? col::Accent : col::TextDim,
                        false,
                        Anchor::RIGHT | Anchor::VCENTER);
                }
            }
            else if (m_activeTab == TabOrders)
            {
                int selected = 0;
                for (const auto &e : entities.GetAllEntities())
                {
                    if (e.alive && e.selected && !e.isBuilding)
                        ++selected;
                }
                std::string label = std::string(_("Selected")) + ": " + std::to_string(selected);
                fontRenderer.DrawString(
                    label,
                    l.primaryBtn.Right(),
                    l.primaryBtn.CenterY(),
                    ui::FONT_SMALL,
                    col::TextDim,
                    false,
                    Anchor::RIGHT | Anchor::VCENTER);
            }

            if (maxScroll > 0)
            {
                auto arrow = [&](const UiRect &r, bool up, bool enabled) {
                    bool hot = enabled && r.Contains(mx, my);
                    renderer.FillRectScreen(r.x, r.y, r.w, r.h, hot ? col::Hover : col::PanelRaised);
                    renderer.DrawRectScreen(r.x, r.y, r.w, r.h, hot ? col::BorderStrong : col::Border);
                    SDL_Color c = enabled ? (hot ? col::Accent : col::Text) : col::TextMuted;
                    for (int i = 0; i < 5; ++i)
                    {
                        int yy = up ? (r.CenterY() - 2 + i) : (r.CenterY() + 2 - i);
                        renderer.FillRectScreen(r.CenterX() - i, yy, 2 * i + 1, 1, c);
                    }
                };
                arrow(l.scrollUp, true, scroll > 0);
                arrow(l.scrollDown, false, scroll < maxScroll);
            }
        }

        // --- Cameo grid -----------------------------------------------------------------------------------
        std::string hoverTitle;
        std::string hoverText;
        bool hasBarracks = entities.HasBuildingType(UnitType::Barracks, Players::Local());
        bool hasFactory = entities.HasBuildingType(UnitType::Factory, Players::Local());
        bool hasLab = HasResearchBuilding(Players::Local(), entities);
        const bool noHq
            = !entities.HasBuildingType(UnitType::Headquarters, Players::Local(), true); // nothing can be built
        // Orders shown on unit cards come from the buildings that would train them
        auto queuedAt = [&](UnitType buildingType) {
            std::vector<const Building *> out;
            for (int id : ProductionTargets(buildingType, entities, selected))
                out.push_back(entities.GetBuilding(id));
            return out;
        };
        const std::vector<const Building *> barracksTargets = queuedAt(UnitType::Barracks);
        const std::vector<const Building *> factoryTargets = queuedAt(UnitType::Factory);

        renderer.SetClipRect({l.grid.x, l.grid.y, l.grid.w, l.grid.h});
        int count = ItemCountForTab(m_activeTab);
        for (int i = 0; i < count; ++i)
        {
            UiRect c = l.Cell(i, scroll);
            if (c.Bottom() < l.grid.y || c.y > l.grid.Bottom())
                continue;
            bool hovered = c.Contains(mx, my) && l.grid.Contains(mx, my);
            if (hovered)
                SoundRequests::MarkHover();

            switch (m_activeTab)
            {
                case TabBuild:
                {
                    const auto &bp = EntityManager::GetBlueprints()[i];
                    bool selected = (m_buildMode && m_selectedBlueprint == i);
                    bool canAfford = economy.CanAfford(bp.creditCost);
                    // Hard limits (data/limits.yaml): greyed out at the limit, "n/max" while one is set
                    const int limit = BuildLimits::LimitOf(entities, bp.buildingIndex);
                    const int owned
                        = limit > 0 ? BuildLimits::CountOwned(entities, map, Players::Local(), bp.buildingIndex) : 0;
                    const bool atLimit = limit == 0 || (limit > 0 && owned >= limit);
                    const bool radarLocked
                        = (bp.buildingIndex == 6 && !IsRadarUnlocked(Players::Local())) || noHq || atLimit;
                    DrawCameo(
                        renderer, fontRenderer, c, bp.iconCompId, _(bp.name), hovered, selected && !noHq, radarLocked);
                    if (limit > 0)
                    {
                        fontRenderer.DrawString(
                            std::to_string(owned) + "/" + std::to_string(limit),
                            c.x + c.w - 6,
                            c.y + 5,
                            ui::FONT_SMALL_BOLD,
                            atLimit ? col::Danger : col::TextMuted,
                            true,
                            Anchor::RIGHT | Anchor::TOP);
                    }
                    DrawCostTag(
                        renderer,
                        fontRenderer,
                        c,
                        "$" + std::to_string(bp.creditCost),
                        canAfford ? col::Accent : col::Danger);
                    std::string pw = (bp.powerCost >= 0 ? "+" : "") + std::to_string(bp.powerCost);
                    fontRenderer.DrawString(
                        pw,
                        c.x + 6,
                        c.y + 5,
                        ui::FONT_SMALL_BOLD,
                        bp.powerCost >= 0 ? col::Ok : SDL_Color {255, 170, 80, 255},
                        true);
                    if (hovered)
                    {
                        hoverTitle
                            = std::string(_(bp.name)) + "  $" + std::to_string(bp.creditCost) + ", " + pw + _(" kW");
                        hoverText = _(bp.desc);
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
                    bool inf = (m_activeTab == TabInfantry);
                    const UnitCameoDef &def = unitCatalog(inf).items[i];
                    const auto &producers = inf ? barracksTargets : factoryTargets;
                    const bool unlocked = IsUnitUnlocked(Players::Local(), def.archetype);
                    bool available = (inf ? hasBarracks : hasFactory) && unlocked;
                    int cost = UnitCost(def, entities);
                    std::string name = _(UnitName(def, entities));
                    DrawCameo(
                        renderer,
                        fontRenderer,
                        c,
                        SpriteHandle(def.icon, SpriteKind::Part, "unit cameo"),
                        name,
                        hovered,
                        false,
                        !available);

                    int queued = 0;
                    float progress = -1.0f; // most advanced order of this unit currently in training
                    for (const Building *b : producers)
                    {
                        for (const auto &q : b->productionQueue)
                        {
                            if (q.archetype == def.archetype)
                                ++queued;
                        }
                        if (!b->productionQueue.empty() && b->productionQueue.front().archetype == def.archetype)
                        {
                            progress = std::max(progress, b->CurrentProductionProgress());
                        }
                    }
                    if (!available)
                    {
                        DrawLockBadge(renderer, fontRenderer, c);
                    }
                    else
                    {
                        DrawCostTag(
                            renderer,
                            fontRenderer,
                            c,
                            "$" + std::to_string(cost),
                            economy.CanAfford(cost) ? col::Accent : col::Danger);
                        if (progress >= 0.0f)
                        {
                            DrawProgressShutter(renderer, fontRenderer, c, progress, col::Accent);
                        }
                        if (queued > 0)
                        {
                            std::string badge = "\xC3\x97" + std::to_string(queued);
                            renderer.FillRectScreen(
                                c.x + 3,
                                c.y + 3,
                                fontRenderer.GetTextWidth(badge, ui::FONT_SMALL_BOLD) + 10,
                                18,
                                col::Accent);
                            fontRenderer.DrawString(
                                badge, c.x + 8, c.y + 4, ui::FONT_SMALL_BOLD, col::AccentText, false);
                        }
                    }
                    if (available)
                    {
                        bool repeating = false;
                        for (const Building *pb : producers)
                            repeating = repeating || pb->repeatArchetype == def.archetype;
                        const UiRect inf8 = InfinityBtn(c);
                        DrawInfinity(renderer, inf8, repeating, inf8.Contains(mx, my));
                    }
                    if (hovered)
                    {
                        hoverTitle = name + "  $" + std::to_string(cost);
                        if (!unlocked)
                        {
                            hoverText
                                = std::string(_("[RESEARCH REQUIRED] "))
                                  + _(researchDef(PlayerFaction(), UnitUnlockResearch(PlayerFaction(), def.archetype))
                                          .name);
                        }
                        else if (!available)
                        {
                            hoverText = inf ? _("[BARRACKS REQUIRED] Construct Barracks to recruit ") + name
                                            : _("[FACTORY REQUIRED] Construct Factory to build ") + name;
                        }
                        else
                        {
                            hoverText = _(UnitDesc(def, entities));
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
                    const auto &o = kOrders[i];
                    bool active = (i == 0 && m_attackMoveMode);
                    if (i >= kFirstStanceOrder)
                    {
                        // Lit when every selected own unit has this stance
                        const auto wanted = static_cast<UnitStance>(i - kFirstStanceOrder);
                        bool any = false, all = true;
                        for (const auto &e : entities.GetAllEntities())
                        {
                            const Unit *u = e.AsUnit();
                            if (!u || !u->alive || !u->selected || u->owner != Players::Local())
                                continue;
                            any = true;
                            all = all && u->stance == wanted;
                        }
                        active = any && all;
                    }
                    renderer.FillRectScreen(
                        c.x,
                        c.y,
                        c.w,
                        c.h,
                        active ? SDL_Color {60, 44, 20, 255} : (hovered ? col::Hover : col::PanelRaised));
                    renderer.DrawRectScreen(
                        c.x, c.y, c.w, c.h, active ? col::Accent : (hovered ? col::BorderStrong : col::Border));
                    fontRenderer.DrawString(
                        ui::FitText(fontRenderer, _(o.name), ui::FONT_BODY_BOLD, c.w - 16),
                        c.CenterX(),
                        c.CenterY() + 6,
                        ui::FONT_BODY_BOLD,
                        active ? col::Accent : col::Text,
                        false,
                        Anchor::HCENTER | Anchor::VCENTER);
                    if (o.key[0] != '\0')
                    {
                        UiRect chip {c.x + 6, c.y + 6, 22, 22};
                        renderer.FillRectScreen(chip.x, chip.y, chip.w, chip.h, col::PanelSunken);
                        renderer.DrawRectScreen(chip.x, chip.y, chip.w, chip.h, col::BorderStrong);
                        fontRenderer.DrawString(
                            o.key,
                            chip.CenterX(),
                            chip.CenterY(),
                            ui::FONT_SMALL_BOLD,
                            col::Accent,
                            false,
                            Anchor::HCENTER | Anchor::VCENTER);
                    }
                    if (hovered)
                    {
                        hoverTitle = _(o.name);
                        if (o.key[0] != '\0')
                            hoverTitle += std::string("  [") + o.key + "]";
                        hoverText = _(o.desc);
                    }
                    break;
                }
            }
        }
        renderer.ResetClipRect();

        // Scroll position hint on the right edge of the grid
        if (maxScroll > 0)
        {
            int trackX = l.panel.Right() - 5;
            renderer.FillRectScreen(trackX, l.grid.y, 2, l.grid.h, col::PanelRaised);
            int rows = maxScroll + l.VisibleRows();
            int thumbH = std::max(16, l.grid.h * l.VisibleRows() / rows);
            int thumbY = l.grid.y + (l.grid.h - thumbH) * scroll / maxScroll;
            renderer.FillRectScreen(trackX, thumbY, 2, thumbH, col::Accent);
        }

        // --- Info box -------------------------------------------------------------------------------------
        {
            const UiRect &ib = l.info;
            ui::DrawPanel(renderer, ib, col::PanelSunken, col::Border);
            int textW = ib.w - 20;

            if (!m_hudNotification.empty())
            {
                renderer.FillRectScreen(ib.x, ib.y, 3, ib.h, col::Accent);
                fontRenderer.DrawTextWrapped(
                    m_hudNotification, ib.x + 10, ib.y + 8, textW, ui::FONT_SMALL_BOLD, col::Accent, false);
            }
            else if (!hoverTitle.empty())
            {
                fontRenderer.DrawString(
                    ui::FitText(fontRenderer, hoverTitle, ui::FONT_SMALL_BOLD, textW),
                    ib.x + 10,
                    ib.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Text,
                    false);
                fontRenderer.DrawTextWrapped(
                    hoverText, ib.x + 10, ib.y + 26, textW, ui::FONT_SMALL, col::TextDim, false);
            }
            else if (
                m_buildMode && m_selectedBlueprint >= 0
                && m_selectedBlueprint < static_cast<int>(EntityManager::GetBlueprints().size()))
            {
                fontRenderer.DrawString(
                    _(EntityManager::GetBlueprints()[m_selectedBlueprint].name),
                    ib.x + 10,
                    ib.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Accent,
                    false);
                fontRenderer.DrawTextWrapped(
                    _("LMB - build, RMB / Esc - cancel"),
                    ib.x + 10,
                    ib.y + 26,
                    textW,
                    ui::FONT_SMALL,
                    col::TextDim,
                    false);
            }
            else if (
                const Building *busy = [&]() -> const Building * {
                    const auto &list = (m_activeTab == TabVehicles) ? factoryTargets : barracksTargets;
                    for (const Building *b : list)
                    {
                        if (!b->productionQueue.empty())
                            return b;
                    }
                    return nullptr;
                }();
                busy && (m_activeTab == TabInfantry || m_activeTab == TabVehicles))
            {
                const auto &item = busy->productionQueue.front();
                std::string what = _("Production");
                if (const auto *cfg = entities.GetUnitConfig(item.archetype))
                    what = _(cfg->name);
                float prog = busy->CurrentProductionProgress();
                std::string line = what + "  " + std::to_string(static_cast<int>(prog * 100.0f)) + "%";
                if (busy->productionQueue.size() > 1)
                {
                    line += "   " + std::string(_("queue")) + ": " + std::to_string(busy->productionQueue.size() - 1);
                }
                fontRenderer.DrawString(
                    ui::FitText(fontRenderer, line, ui::FONT_SMALL_BOLD, textW),
                    ib.x + 10,
                    ib.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Text,
                    false);
                ui::DrawProgressBar(renderer, {ib.x + 10, ib.y + 30, textW, 8}, prog, col::Accent);
            }
            else if (const auto own = OwnSelectedBuildings(entities, selected); !own.empty())
            {
                const Building *b = own.front();
                std::string title = _("Structure");
                if (const BuildingConfig *cfg = entities.GetBuildingConfig(b->rawBuildingType))
                    title = _(cfg->name);
                if (own.size() > 1)
                    title += "  x" + std::to_string(own.size());
                fontRenderer.DrawString(
                    ui::FitText(fontRenderer, title, ui::FONT_SMALL_BOLD, textW),
                    ib.x + 10,
                    ib.y + 7,
                    ui::FONT_SMALL_BOLD,
                    col::Text,
                    false);
                const float hp = b->maxHp > 0 ? static_cast<float>(b->currentHp) / b->maxHp : 1.0f;
                ui::DrawProgressBar(renderer, {ib.x + 10, ib.y + 28, textW, 6}, hp, hp > 0.35f ? col::Ok : col::Danger);
                fontRenderer.DrawString(
                    std::to_string(b->currentHp) + " / " + std::to_string(b->maxHp),
                    ib.x + 10,
                    ib.y + 40,
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
                    _(kTabHints[m_activeTab]), ib.x + 10, ib.y + 8, textW, ui::FONT_SMALL, col::TextMuted, false);
            }

            if (const auto ownSel = OwnSelectedBuildings(entities, selected); !ownSel.empty())
            {
                bool anyDamaged = false, allRepairing = true;
                for (const Building *b : ownSel)
                {
                    if (b->CanBeRepaired())
                    {
                        anyDamaged = true;
                        allRepairing = allRepairing && b->repairing;
                    }
                }
                if (anyDamaged)
                {
                    const UiRect rb = RepairBtn(ib);
                    const bool on = allRepairing;
                    const bool hotR = rb.Contains(mx, my);
                    renderer.FillRectScreen(
                        rb.x,
                        rb.y,
                        rb.w,
                        rb.h,
                        on ? SDL_Color {60, 44, 20, 255} : (hotR ? col::Hover : col::PanelRaised));
                    renderer.DrawRectScreen(rb.x, rb.y, rb.w, rb.h, on ? col::Accent : col::BorderStrong);
                    fontRenderer.DrawString(
                        on ? _("REPAIRING") : _("REPAIR [R]"),
                        rb.CenterX(),
                        rb.CenterY(),
                        ui::FONT_SMALL_BOLD,
                        on ? col::Accent : col::Text,
                        false,
                        Anchor::HCENTER | Anchor::VCENTER);
                }
            }
            if (!OwnSelectedBuildings(entities, selected).empty())
            {
                const UiRect db = DestroyBtn(ib);
                const bool armed = m_destroyArmed > 0.0f;
                const bool hot = db.Contains(mx, my);
                renderer.FillRectScreen(
                    db.x,
                    db.y,
                    db.w,
                    db.h,
                    armed ? col::Danger : (hot ? ui::Mix(col::PanelRaised, col::Danger, 0.35f) : col::PanelRaised));
                renderer.DrawRectScreen(db.x, db.y, db.w, db.h, col::Danger);
                fontRenderer.DrawString(
                    armed ? _("CONFIRM?") : _("DESTROY"),
                    db.CenterX(),
                    db.CenterY(),
                    ui::FONT_SMALL_BOLD,
                    armed ? col::Text : col::Danger,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
            }
        }
    }
}

} // namespace OpenArtOfWar
