#include "graphics/Renderer.hpp"
#include "graphics/SpriteRegistry.hpp"
#include "graphics/ShaderEffects.hpp"
#include "core/Utility.hpp"
#include "game/Effects.hpp"
#include <cmath>
#include <iostream>
#include <algorithm>

namespace OpenArtOfWar
{

Renderer::Renderer(SDL_Renderer *sdlRenderer, TextureManager *textureManager)
    : m_renderer(sdlRenderer)
    , m_texManager(textureManager)
{}

Renderer::~Renderer()
{
    if (m_maskTexture)
        SDL_DestroyTexture(m_maskTexture);
    if (m_glowTexture)
        SDL_DestroyTexture(m_glowTexture);
}

void Renderer::DrawAlphaMaskWorld(
    const std::vector<std::uint8_t> &alpha,
    int w,
    int h,
    float worldX,
    float worldY,
    float worldW,
    float worldH,
    const Camera &camera,
    SDL_Color color,
    bool smooth)
{
    PlainState();
    if (w <= 0 || h <= 0 || alpha.size() < static_cast<std::size_t>(w) * h)
        return;
    // The streaming texture only grows, so panning the camera does not reallocate it every frame
    if (!m_maskTexture || w > m_maskW || h > m_maskH)
    {
        if (m_maskTexture)
            SDL_DestroyTexture(m_maskTexture);
        m_maskW = std::max(w, m_maskW);
        m_maskH = std::max(h, m_maskH);
        m_maskTexture
            = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, m_maskW, m_maskH);
        if (!m_maskTexture)
            return;
        SDL_SetTextureBlendMode(m_maskTexture, SDL_BLENDMODE_BLEND);
    }
    SDL_SetTextureScaleMode(m_maskTexture, smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    const std::uint32_t rgb = (static_cast<std::uint32_t>(color.r) << 16) | (static_cast<std::uint32_t>(color.g) << 8) | color.b;
    m_maskPixels.resize(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < m_maskPixels.size(); ++i)
        m_maskPixels[i] = (static_cast<std::uint32_t>(alpha[i]) << 24) | rgb;
    SDL_Rect region {0, 0, w, h};
    SDL_UpdateTexture(m_maskTexture, &region, m_maskPixels.data(), w * static_cast<int>(sizeof(std::uint32_t)));

    float sx = 0.0f, sy = 0.0f;
    camera.WorldToScreen(worldX, worldY, sx, sy);
    const float zoom = camera.GetZoom();
    if (!smooth)
    {
        SDL_FRect src {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)};
        SDL_FRect dst {sx, sy, worldW * zoom, worldH * zoom};
        SDL_RenderTexture(m_renderer, m_maskTexture, &src, &dst);
        return;
    }
    // Half a texel inset keeps the linear filter from sampling outside the updated region
    SDL_FRect src {0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f};
    const float texelW = worldW / static_cast<float>(w);
    const float texelH = worldH / static_cast<float>(h);
    SDL_FRect dst {
        sx + texelW * 0.5f * zoom, sy + texelH * 0.5f * zoom, (worldW - texelW) * zoom, (worldH - texelH) * zoom};
    SDL_RenderTexture(m_renderer, m_maskTexture, &src, &dst);
}

void Renderer::SetArmyColor(PlayerId player, bool custom, SDL_Color color)
{
    if (player >= kMaxPlayers)
        return;
    m_armyCustom[player] = custom;
    m_armyColor[player] = color;
}

void Renderer::EndWorldPass()
{
    m_drawOwner = kNoPlayer;
    m_structureTeam = 0;
    PlainState();
}

void Renderer::PlainState()
{
    if (m_effects)
        m_effects->UseDefaultState();
}

SDL_Texture *Renderer::BindSheet(int &page, Faction faction, int recolor)
{
    // `recolor`: the team armour on the frame (1 blue, 2 red, 0 none; SpriteFrame::recolor). An army colour recolours
    // the armour; every other pixel stays.
    (void) faction; // the image of the faction is chosen by the caller
    const PlayerId owner = m_drawOwner;
    const bool custom = owner < kMaxPlayers && m_armyCustom[owner];
    int team = 0;
    if (custom)
    {
        // An army colour recolours the plain image: the Resistance image is only the classic red
        if (page >= m_texManager->ImageCount())
            page -= m_texManager->ImageCount();
        team = recolor;
        // Buildings, walls and icons: their trims sit on the shared images too
        if (m_structureTeam != 0)
            team = m_structureTeam;
    }
    const bool gpuEffects = m_effects && m_effects->Available();
    SDL_Texture *sheet = (team != 0 && !gpuEffects)
                             ? m_texManager->GetRecoloredSpriteSheet(page, team, m_armyColor[owner])
                             : m_texManager->GetSpriteSheet(page);
    if (!sheet)
        return nullptr;
    SDL_SetTextureAlphaMod(sheet, static_cast<Uint8>(m_drawAlpha * 255.0f + 0.5f));
    if (gpuEffects)
    {
        m_effects->UseSpriteState(team);
        const bool tracked = page >= 0;
        if (tracked && page >= static_cast<int>(m_tintedSheet.size()))
            m_tintedSheet.resize(page + 1, false);
        if (team != 0 || recolor != 0 || (tracked && m_tintedSheet[page]))
        {
            // The colour mod carries the army colour to the shader; plain draws of the same image stay white
            const SDL_Color c = team ? m_armyColor[owner] : SDL_Color {255, 255, 255, 255};
            SDL_SetTextureColorMod(sheet, c.r, c.g, c.b);
            if (tracked)
                m_tintedSheet[page] = team != 0;
        }
    }
    return sheet;
}

namespace
{
// The pixels of a frame in its image
SDL_FRect FrameSource(const SpriteFrame &f)
{
    return {std::round(f.srcX), std::round(f.srcY), std::round(f.srcW), std::round(f.srcH)};
}
} // namespace

void Renderer::Clear(SDL_Color clearColor)
{
    SDL_SetRenderDrawColor(m_renderer, clearColor.r, clearColor.g, clearColor.b, clearColor.a);
    SDL_RenderClear(m_renderer);
}

void Renderer::Present()
{
    SDL_RenderPresent(m_renderer);
}

void Renderer::DrawRawSpriteWorld(int spriteId, float worldX, float worldY, const Camera &camera, Faction faction)
{
    if (!m_texManager)
        return;
    const auto *frame = m_texManager->GetSpriteFrame(spriteId);
    if (!frame || frame->width <= 0 || frame->height <= 0 || frame->srcW <= 0 || frame->srcH <= 0)
        return;

    // The Resistance draws from its own image of the object where there is one
    int page = faction == Faction::Resistance ? m_texManager->ResistancePage(frame->page) : frame->page;

    float sx, sy;
    camera.WorldToScreen(worldX, worldY, sx, sy);

    float zoom = camera.GetZoom();
    float drawW = frame->width * zoom;
    float drawH = frame->height * zoom;

    // Seam fix when zooming out / fractional scale:
    // Floating point rasterization on tile grids requires a tiny bleed (+0.6px when zoom < 1.0f)
    // to prevent subpixel seams / 1-pixel dark gap lines between adjacent tiles.
    float bleed = (zoom < 1.0f) ? 0.6f : ((zoom < 2.0f) ? 0.25f : 0.0f);
    SDL_FRect dstRect = {sx, sy, drawW + bleed, drawH + bleed};

    if (dstRect.x + dstRect.w < 0 || dstRect.x > camera.GetViewportWidth() || dstRect.y + dstRect.h < 0
        || dstRect.y > camera.GetViewportHeight())
    {
        return;
    }

    SDL_Texture *sheet = BindSheet(page, faction, frame->recolor);
    if (!sheet)
        return;
    const SDL_FRect srcRect = FrameSource(*frame);
    SDL_RenderTexture(m_renderer, sheet, &srcRect, &dstRect);
    if (m_glowStrength > 0.0f)
    {
        // The same pixels once more, added in the glow colour: only the sprite itself shines
        SDL_SetTextureBlendMode(sheet, SDL_BLENDMODE_ADD);
        SDL_SetTextureColorMod(sheet, m_glowColor.r, m_glowColor.g, m_glowColor.b);
        SDL_SetTextureAlphaMod(sheet, static_cast<Uint8>(std::clamp(m_glowStrength, 0.0f, 1.0f) * 255.0f));
        SDL_RenderTexture(m_renderer, sheet, &srcRect, &dstRect);
        SDL_SetTextureBlendMode(sheet, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(sheet, 255, 255, 255);
        if (page >= 0 && page < static_cast<int>(m_tintedSheet.size()))
            m_tintedSheet[page] = false;
    }
}

void Renderer::DrawCompositeWorld(
    int compId, float worldX, float worldY, const Camera &camera, Faction faction, int depth)
{
    if (depth > 6 || !m_texManager)
        return;

    IndexRange r = m_texManager->GetCompositeRange(compId);
    if (r.end < r.start)
        return;

    for (int p = r.start; p <= r.end; ++p)
    {
        const auto *part = m_texManager->GetCompositePart(p);
        if (!part)
            continue;

        float px = worldX + part->ox;
        float py = worldY + part->oy;

        if (part->isComp == 0)
        {
            DrawRawSpriteWorld(part->child, px, py, camera, faction);
        }
        else
        {
            DrawCompositeWorld(part->child, px, py, camera, faction, depth + 1);
        }
    }
}

namespace
{
// Box (x0, y0, x1, y1) of the sprites of a composite; false when it shows nothing
bool CompositeBox(const TextureManager &tex, int compId, float ox, float oy, int depth, float box[4], bool &any)
{
    if (depth > 6)
        return any;
    const IndexRange r = tex.GetCompositeRange(compId);
    for (int p = r.start; r.end >= r.start && p <= r.end; ++p)
    {
        const auto *part = tex.GetCompositePart(p);
        if (!part)
            continue;
        if (part->isComp != 0)
        {
            CompositeBox(tex, part->child, ox + part->ox, oy + part->oy, depth + 1, box, any);
            continue;
        }
        const auto *frame = tex.GetSpriteFrame(part->child);
        if (!frame || frame->width <= 0 || frame->height <= 0)
            continue;
        const float x0 = ox + part->ox, y0 = oy + part->oy;
        const float x1 = x0 + frame->width, y1 = y0 + frame->height;
        if (!any)
        {
            box[0] = x0;
            box[1] = y0;
            box[2] = x1;
            box[3] = y1;
            any = true;
        }
        else
        {
            box[0] = std::min(box[0], x0);
            box[1] = std::min(box[1], y0);
            box[2] = std::max(box[2], x1);
            box[3] = std::max(box[3], y1);
        }
    }
    return any;
}
} // namespace

void Renderer::CompositeCenterOffset(int compId, float &outDx, float &outDy)
{
    outDx = outDy = 0.0f;
    if (!m_texManager)
        return;
    const auto it = m_compCenterOffset.find(compId);
    if (it != m_compCenterOffset.end())
    {
        outDx = it->second.first;
        outDy = it->second.second;
        return;
    }
    float box[4] {0, 0, 0, 0};
    bool any = false;
    CompositeBox(*m_texManager, compId, 0.0f, 0.0f, 0, box, any);
    if (any)
    {
        outDx = (box[0] + box[2]) * 0.5f - 90.0f;
        outDy = (box[1] + box[3]) * 0.5f - 60.0f;
    }
    m_compCenterOffset[compId] = {outDx, outDy};
}

void Renderer::DrawCompositeCentered(int compId, float centerX, float centerY, const Camera &camera, Faction faction)
{
    float dx = 0.0f, dy = 0.0f;
    CompositeCenterOffset(compId, dx, dy);
    DrawCompositeWorld(compId, centerX - 90.0f - dx, centerY - 60.0f - dy, camera, faction);
}

void Renderer::DrawAnimSequenceWorld(
    int animId, int frame, float worldX, float worldY, const Camera &camera, Faction faction)
{
    if (!m_texManager)
        return;

    IndexRange r = m_texManager->GetAnimRange(animId);
    if (r.end < r.start)
        return;

    int count = (r.end - r.start) + 1;
    int idx = r.start + (std::abs(frame) % count);

    const auto *f = m_texManager->GetAnimFrame(idx);
    if (!f)
        return;

    float px = worldX + f->ox;
    float py = worldY + f->oy;

    if (f->isComp == 0)
    {
        DrawRawSpriteWorld(f->child, px, py, camera, faction);
    }
    else
    {
        DrawCompositeWorld(f->child, px, py, camera, faction);
    }
}

void Renderer::RenderCompositePartsRotated(
    TextureManager *tm,
    SDL_Renderer *sdlRnd,
    int compId,
    float originX,
    float originY,
    float baseOx,
    float baseOy,
    float angleDeg,
    const Camera &camera,
    Faction faction,
    int depth)
{
    if (depth > 6 || !tm)
        return;
    IndexRange r = tm->GetCompositeRange(compId);
    if (r.end < r.start)
        return;

    for (int p = r.start; p <= r.end; ++p)
    {
        const auto *part = tm->GetCompositePart(p);
        if (!part)
            continue;

        float ox = baseOx + part->ox;
        float oy = baseOy + part->oy;

        if (part->isComp == 0)
        {
            // Place the sprite unrotated at origin + offset and rotate it about the origin itself,
            // so every part of the animation turns around the same pivot (e.g. the flamethrower nozzle)
            float worldX = originX + ox;
            float worldY = originY + oy;

            const auto *sf = tm->GetSpriteFrame(part->child);
            if (!sf || sf->srcW <= 0 || sf->srcH <= 0)
                continue;
            int page = faction == Faction::Resistance ? tm->ResistancePage(sf->page) : sf->page;
            SDL_Texture *sheet = BindSheet(page, faction, sf->recolor);
            if (!sheet)
                continue;

            float sx, sy;
            camera.WorldToScreen(worldX, worldY, sx, sy);
            float zoom = camera.GetZoom();
            float drawW = sf->width * zoom;
            float drawH = sf->height * zoom;
            float bleed = (zoom < 1.0f) ? 0.6f : ((zoom < 2.0f) ? 0.25f : 0.0f);
            SDL_FRect dstRect = {sx, sy, drawW + bleed, drawH + bleed};

            const SDL_FRect srcRect = FrameSource(*sf);
            SDL_FPoint center = {-ox * zoom, -oy * zoom};
            SDL_RenderTextureRotated(sdlRnd, sheet, &srcRect, &dstRect, angleDeg, &center, SDL_FLIP_NONE);
        }
        else
        {
            RenderCompositePartsRotated(
                tm, sdlRnd, part->child, originX, originY, ox, oy, angleDeg, camera, faction, depth + 1);
        }
    }
}

void Renderer::DrawAnimSequenceWorldRotated(
    int animId, int frame, float worldX, float worldY, float angleRad, const Camera &camera, Faction faction)
{
    if (!m_texManager)
        return;

    IndexRange r = m_texManager->GetAnimRange(animId);
    if (r.end < r.start)
        return;

    int count = (r.end - r.start) + 1;
    int idx = r.start + (std::abs(frame) % count);

    const auto *f = m_texManager->GetAnimFrame(idx);
    if (!f)
        return;

    float angleDeg = angleRad * (180.0f / static_cast<float>(M_PI));

    if (f->isComp == 0)
    {
        float baseOx = static_cast<float>(f->ox);
        float baseOy = static_cast<float>(f->oy);
        const auto *sf = m_texManager->GetSpriteFrame(f->child);
        if (!sf || sf->srcW <= 0 || sf->srcH <= 0)
            return;
        int page = faction == Faction::Resistance ? m_texManager->ResistancePage(sf->page) : sf->page;
        SDL_Texture *sheet = BindSheet(page, faction, sf->recolor);
        if (!sheet)
            return;

        float sx, sy;
        camera.WorldToScreen(worldX + baseOx, worldY + baseOy, sx, sy);
        float zoom = camera.GetZoom();
        float drawW = sf->width * zoom;
        float drawH = sf->height * zoom;
        float bleed = (zoom < 1.0f) ? 0.6f : ((zoom < 2.0f) ? 0.25f : 0.0f);
        SDL_FRect dstRect = {sx, sy, drawW + bleed, drawH + bleed};

        const SDL_FRect srcRect = FrameSource(*sf);
        SDL_FPoint center = {-baseOx * zoom, -baseOy * zoom}; // rotate about (worldX, worldY)
        SDL_RenderTextureRotated(m_renderer, sheet, &srcRect, &dstRect, angleDeg, &center, SDL_FLIP_NONE);
    }
    else
    {
        float baseOx = static_cast<float>(f->ox);
        float baseOy = static_cast<float>(f->oy);
        RenderCompositePartsRotated(
            m_texManager, m_renderer, f->child, worldX, worldY, baseOx, baseOy, angleDeg, camera, faction, 0);
    }
}

void Renderer::DrawTileWorld(
    int tilesetId, int tileIndex, float worldX, float worldY, const Camera &camera, int /*globalAnimTick*/)
{
    if (!m_texManager)
        return;

    // Ground, cliff, and water tiles are static.
    int compId = m_texManager->GetTileCompositeId(tilesetId, tileIndex);
    DrawCompositeWorld(compId, worldX, worldY, camera, Faction::Resistance);
}

void Renderer::DrawDecorWorld(
    int tilesetId, int decorType, float worldX, float worldY, const Camera &camera, int animFrame, int layer)
{
    if (!m_texManager)
        return;

    // Tileset decorations (trees, bridges, cliffs, landmarks) are buildings of the tileset's "decor" family
    const int sprite = m_texManager->GetDecorBuildingId(tilesetId, decorType);
    if (sprite >= 0)
    {
        for (int l = layer < 0 ? 0 : layer; l <= (layer < 0 ? 1 : layer); ++l)
            DrawBuildingSpriteWorld(sprite, -1, worldX, worldY, l, camera, Faction::Neutral, animFrame, 1.0f, false);
    }
}

int Renderer::TallDecorRows(int tilesetId, int decorType) const
{
    if (!m_texManager)
        return 0;
    const BuildingDef *def = m_texManager->GetBuildingDef(m_texManager->GetDecorBuildingId(tilesetId, decorType));
    if (!def || !def->tall)
        return 0;
    return std::max<int>(1, def->height);
}

void Renderer::DrawBuildingWorld(
    int buildingType,
    float worldX,
    float worldY,
    int layer,
    const Camera &camera,
    Faction faction,
    int animFrame,
    float buildProgress,
    bool isDestroyed,
    float hpRatio,
    int turretFacing,
    bool isAttacking,
    bool isPowered,
    int customWreckComp,
    int customWreckStages)
{
    if (!m_texManager)
        return;
    DrawBuildingSpriteWorld(
        m_texManager->GetBuildingSprite(buildingType),
        buildingType,
        worldX,
        worldY,
        layer,
        camera,
        faction,
        animFrame,
        buildProgress,
        isDestroyed,
        hpRatio,
        turretFacing,
        isAttacking,
        isPowered,
        customWreckComp,
        customWreckStages);
}

void Renderer::DrawBuildingSpriteWorld(
    int sprite,
    int buildingType,
    float worldX,
    float worldY,
    int layer,
    const Camera &camera,
    Faction faction,
    int animFrame,
    float buildProgress,
    bool isDestroyed,
    float hpRatio,
    int turretFacing,
    bool isAttacking,
    bool isPowered,
    int customWreckComp,
    int customWreckStages)
{
    if (!m_texManager)
        return;

    const auto *bdef = m_texManager->GetBuildingDef(sprite);
    if (!bdef || bdef->end < bdef->start)
        return;

    lastDrawnBuilding = {sprite, animFrame, m_animatedPartsVisible};

    // 1. Destroyed building remnant
    // Static crater/rubble composite that stays for 2 minutes without looping frames!
    if (isDestroyed)
    {
        if (layer == 0)
        {
            int baseRemnant = 0, stages = 0;
            DefaultBuildingWreck(bdef->width, bdef->height, baseRemnant, stages);
            if (customWreckComp >= 0)
                baseRemnant = customWreckComp;
            if (customWreckStages > 0)
                stages = customWreckStages;
            // Gradual burial over 120 seconds: animFrame is the stage
            int stage = std::clamp(animFrame, 0, stages - 1);
            // Remnant composite is centered at (centerX, centerY) of the building
            float centerX = worldX + (bdef->width * OpenArtOfWar::TILE_WIDTH * 0.5f);
            float centerY = worldY + (bdef->height * OpenArtOfWar::TILE_HEIGHT * 0.5f);
            DrawCompositeCentered(baseRemnant + stage, centerX, centerY, camera, faction);
        }
        return;
    }

    // Living buildings and walls show the army colour of their owner on every sheet (see setStructureFaction)
    struct StructureScope
    {
        Renderer &r;
        ~StructureScope()
        {
            r.SetStructureFaction(Faction::Neutral);
        }
    } structureScope {*this};
    const BuildingStyle style = bdef->style;
    if ((style == BuildingStyle::Structure || style == BuildingStyle::Wall) && !isDestroyed)
        SetStructureFaction(faction);

    // 2. Walls, construction scaffolding and map decorations: the parts of state 0
    if (style != BuildingStyle::Structure)
    {
        for (int p = bdef->start; p <= bdef->end; ++p)
        {
            const auto *part = m_texManager->GetBuildingPart(p);
            if (!part)
                continue;
            // Flat decorations have no layer split: all their parts go with layer 0 (drawn once, not once per layer)
            const bool inLayer = style == BuildingStyle::DecorFlat ? layer == 0 : part->layer == layer;
            if (inLayer && part->state == 0)
            {
                float px = worldX + part->ox;
                float py = worldY + part->oy;
                if (part->isComp == 0)
                {
                    DrawRawSpriteWorld(part->child, px, py, camera, faction);
                }
                else if (part->isComp == 1)
                {
                    DrawCompositeWorld(part->child, px, py, camera, faction);
                }
                else if (part->isComp == 2 && style == BuildingStyle::Wall)
                {
                    DrawAnimSequenceWorld(part->child, animFrame, px, py, camera, faction);
                }
            }
        }
        return;
    }

    // 3. Active faction buildings
    // Under construction: draw scaffolding / base
    bool underConstruction = (buildProgress < 1.0f);
    int iA = 4;
    if (!underConstruction)
    {
        int hpScaled = static_cast<int>(std::clamp(hpRatio, 0.0f, 1.0f) * 4.0f);
        iA = std::clamp(7 - hpScaled, 4, 7);
    }

    int tdir8 = (turretFacing % 8 + 8) % 8;

    for (int p = bdef->start; p <= bdef->end; ++p)
    {
        const auto *part = m_texManager->GetBuildingPart(p);
        if (!part)
            continue;

        float px = worldX + part->ox;
        float py = worldY + part->oy;

        if (part->layer == 2)
        {
            // Layer 2 parts are drawn during the layer 1 pass.
            if (layer != 1)
                continue;

            if (part->state == 1)
            {
                // Turret / Bunker / HQ firing muzzle flash animation
                bool pwrOk = (buildingType == 5 || buildingType == 13) || isPowered;
                if (!underConstruction && pwrOk && isAttacking && part->isComp == 2)
                {
                    DrawAnimSequenceWorld(part->child + tdir8, animFrame, px, py, camera, faction);
                }
            }
            else if (part->state == 2)
            {
                // Rotatable turret composite (Building 7: Confederation Missile Turret); also on the
                // construction site, else the tower stands headless until it is finished
                if (part->isComp == 1)
                {
                    DrawCompositeWorld(part->child + tdir8, px, py, camera, faction);
                }
            }
            else if (part->state == 10)
            {
                // Offline / No-power indicator icon (Buildings 0, 6, 8, 12)
                bool needsPower = (buildingType != 5 && buildingType != 13);
                if ((underConstruction || (!isPowered && needsPower)) && part->isComp == 1)
                {
                    DrawCompositeWorld(part->child, px, py, camera, faction);
                }
            }
        }
        else if (part->layer == layer)
        {
            // Standard Layer 0 (foundation + damage state == iA) and Layer 1 (superstructure + smoke/fire state <= iA)
            bool stateMatch = (part->state == 0) || (layer == 0 && part->state == iA)
                              || (layer == 1 && part->state > 0 && part->state <= iA);
            if (!stateMatch)
                continue;

            if (part->isComp == 0)
            {
                DrawRawSpriteWorld(part->child, px, py, camera, faction);
            }
            else if (part->isComp == 1)
            {
                DrawCompositeWorld(part->child, px, py, camera, faction);
            }
            else if (part->isComp == 2)
            {
                // Animated building parts (Radar 6 spinning dish, PowerPlant 1/9 glow, Factory/Lab lights)
                if (!underConstruction && isPowered && m_animatedPartsVisible)
                {
                    DrawAnimSequenceWorld(part->child, animFrame, px, py, camera, faction);
                }
            }
        }
    }
}

void Renderer::DefaultBuildingWreck(int width, int height, int &outComposite, int &outStages)
{
    const EffectsConfig::Wreck w = EffectsConfig::Get().WreckOfBuilding(width, height);
    outComposite = w.composite;
    outStages = w.stages;
}

void Renderer::DrawUnitWorld(
    int unitTypeId,
    int actionState,
    int facingDir,
    int animStep,
    float worldX,
    float worldY,
    const Camera &camera,
    Faction faction,
    int targetFacing,
    bool isFlaming,
    int customWreckComp,
    int customWreckStages,
    int customDeathSeq,
    int flashMode,
    int flashFrames)
{
    if (!m_texManager)
        return;

    const UnitVisual *visual = m_texManager->GetUnitVisual(unitTypeId);
    bool isInfantry = Utility::IsInfantryArchetype(unitTypeId); // archetypes 0..5 and 14 (prone sniper)

    // 1. Dying / Wrecked state
    if (actionState == static_cast<int>(ActionState::Die))
    {
        if (!isInfantry)
        {
            // Destroyed vehicle remnant - static wreck that fades over 2 minutes!
            const EffectsConfig::Wreck def = EffectsConfig::Get().WreckOfVehicle(unitTypeId);
            const int baseWreck = (customWreckComp >= 0) ? customWreckComp : def.composite;
            const int maxStages = (customWreckStages > 0) ? customWreckStages : def.stages;
            int stage = std::clamp(animStep, 0, maxStages - 1);
            // Vehicle wreck composite is centered at (centerX, centerY) of the unit tile
            float centerX = worldX + (static_cast<float>(OpenArtOfWar::TILE_WIDTH) * 0.5f);
            float centerY = worldY + (static_cast<float>(OpenArtOfWar::TILE_HEIGHT) * 0.5f);
            DrawCompositeCentered(baseWreck + stage, centerX, centerY, camera, faction);
            return;
        }
        else
        {
            // Human death and corpse sequences:
            // Bullet death: sequences 247 or 248 (corpse variants)
            // Flame death: sequences 259 or 260 (burning human -> charred remains)
            int deathSeq = customDeathSeq;
            if (deathSeq < 0)
            {
                static const CachedSprite bullet[2] = {{"fx_infantry_death.death_0", SpriteKind::Sequence},
                                                       {"fx_infantry_death.death_1", SpriteKind::Sequence}};
                static const CachedSprite flame[2] = {{"fx_infantry_death.flame_0", SpriteKind::Sequence},
                                                      {"fx_infantry_death.flame_1", SpriteKind::Sequence}};
                deathSeq = (isFlaming ? flame : bullet)[(unitTypeId % 2 + 2) % 2];
            }
            IndexRange r = m_texManager->GetUnitSequenceRange(deathSeq);
            if (r.end >= r.start)
            {
                int count = (r.end - r.start) + 1;
                int stepIdx = r.start + std::clamp(animStep, 0, count - 1);
                const auto *step = m_texManager->GetUnitSequenceStep(stepIdx);
                if (step)
                {
                    float baseX = worldX + step->ox;
                    float baseY = worldY + step->oy;
                    IndexRange br = m_texManager->GetUnitBodyRange(step->bodyId);
                    for (int b = br.start; b <= br.end; ++b)
                    {
                        const auto *layer = m_texManager->GetUnitLayer(b);
                        if (!layer)
                            continue;
                        float lx = baseX + layer->ox;
                        float ly = baseY + layer->oy;
                        if (layer->isComp == 0)
                        {
                            DrawRawSpriteWorld(layer->child, lx, ly, camera, faction);
                        }
                        else if (layer->isComp == 1)
                        {
                            DrawCompositeWorld(layer->child, lx, ly, camera, faction);
                        }
                    }
                    return;
                }
            }
        }
    }

    if (!visual)
        return;
    int baseSeq = m_texManager->GetUnitBaseSeq(unitTypeId, actionState);
    // A family of 8 is drawn by facing; a single sequence (a deployed siege hull) always faces the same way
    int baseCount = actionState == static_cast<int>(ActionState::Move) && visual->move >= 0 ? visual->moveCount
                                                                                           : visual->idleCount;
    // Infantry with a firing pose (riflemen, heavy assault, grenadiers) switch to it; the flamethrower fires from
    // its standing sequences (its firing layer is the flame)
    if (actionState == static_cast<int>(ActionState::Attack) && visual->attack >= 0)
        baseSeq = visual->attack;

    // Deployed siege units use a fixed hull sequence while their turret or gun rotates via tdir8.
    const bool fixedHull = visual->idleCount == 1;
    int dir8 = fixedHull ? 0 : ((facingDir % 8 + 8) % 8);
    int tdir8 = (targetFacing >= 0) ? ((targetFacing % 8 + 8) % 8) : ((facingDir % 8 + 8) % 8);
    int seqIdx = baseSeq;
    if (actionState != static_cast<int>(ActionState::Transform) && baseCount >= 8)
    {
        seqIdx = baseSeq + dir8;
    }

    IndexRange r = m_texManager->GetUnitSequenceRange(seqIdx);
    if (r.end < r.start)
        return;

    int count = (r.end - r.start) + 1;
    bool clampAnim
        = (actionState == static_cast<int>(ActionState::Die)
           || actionState == static_cast<int>(ActionState::Transform));
    int stepIdx = r.start + (clampAnim ? std::clamp(animStep, 0, count - 1) : (std::abs(animStep) % count));

    const auto *step = m_texManager->GetUnitSequenceStep(stepIdx);
    if (!step)
        return;

    float baseX = worldX + step->ox;
    float baseY = worldY + step->oy;

    IndexRange br = m_texManager->GetUnitBodyRange(step->bodyId);
    if (br.end < br.start)
        return;

    // Layer visibility bitmask and turret rotation tables:
    // Turret weapon slot per layer flag 0..5.
    // Idle layer flags map to firing layer flags 3..5.
    static const int s_b53[6] = {1, 0, 2, 0, 1, 2};
    static const int s_b54[3] = {1, 0, 2};

    std::uint8_t bMask = 0x07; // Bits 0, 1, 2 enabled by default
    if (actionState == static_cast<int>(ActionState::Attack))
    {
        bMask |= (1 << 3) | (1 << 4) | (1 << 5);
    }

    for (int lay = br.start; lay <= br.end; ++lay)
    {
        const auto *layer = m_texManager->GetUnitLayer(lay);
        if (!layer)
            continue;

        std::uint8_t flag = layer->flag;
        if (flag >= 6 || (bMask & (1 << flag)) == 0)
        {
            continue;
        }

        int childId = layer->child;
        int b2 = s_b53[flag];

        if (b2 > 0)
        {
            // Un-upgraded T-22 Zeus (archetype 17) hides top rocket launcher (b2 > 0) until Lab upgrade -> archetype 11
            if (unitTypeId == 17)
            {
                continue;
            }
            // Rotate turret independently of hull
            if (actionState == static_cast<int>(ActionState::Transform))
            {
                childId += tdir8;
            }
            else
            {
                childId += (tdir8 - dir8);
            }
        }

        float lx = baseX + layer->ox;
        float ly = baseY + layer->oy;

        if (layer->isComp == 0)
        {
            DrawRawSpriteWorld(childId, lx, ly, camera, faction);
        }
        else if (layer->isComp == 1)
        {
            DrawCompositeWorld(childId, lx, ly, camera, faction);
        }
        else if (layer->isComp == 2)
        {
            if (flag <= 2)
            {
                DrawAnimSequenceWorld(childId, animStep, lx, ly, camera, faction);
            }
            else
            {
                // Firing muzzle flash / recoiling turret layer (flag 3..5):
                // Replaces the corresponding idle layer (flag 0..2) while firing!
                IndexRange ar = m_texManager->GetAnimRange(childId);
                int animLen = (ar.end >= ar.start) ? (ar.end - ar.start + 1) : 1;
                const int flashLen = (flashFrames > 0) ? std::min(flashFrames, animLen) : animLen;
                int fireFrame = std::abs(animStep) % std::max(1, animLen);
                if (flashMode != 0)
                {
                    if (animStep >= flashLen && flashMode == 1)
                        continue; // played once: the idle layer shows until the next shot
                    fireFrame = std::clamp(animStep, 0, std::max(0, flashLen - 1));
                }
                bMask &= ~(1 << s_b54[flag - 3]);
                DrawAnimSequenceWorld(childId, fireFrame, lx, ly, camera, faction);
            }
        }
    }
}

void Renderer::DrawProjectileWorld(
    int baseCompId, int dir, float worldX, float worldY, const Camera &camera, Faction faction, int numDirs)
{
    int safeDirs = (numDirs > 0) ? numDirs : 16;
    int d = (dir % safeDirs + safeDirs) % safeDirs;
    DrawCompositeWorld(baseCompId + d, worldX - 90.0f, worldY - 60.0f, camera, faction);
}

void Renderer::DrawSmokePuffWorld(int frame, float worldX, float worldY, const Camera &camera)
{
    int f = std::clamp(frame, 0, 13);
    static const CachedSprite smokeFrames("fx_smoke_puff.frames", SpriteKind::Part);
    const int smoke = smokeFrames;
    if (smoke >= 0)
        DrawCompositeWorld(smoke + f, worldX - 90.0f, worldY - 60.0f, camera, Faction::Resistance);
}

void Renderer::FillRectWorld(float wx, float wy, float ww, float wh, const Camera &camera, SDL_Color color)
{
    PlainState();
    float sx, sy;
    camera.WorldToScreen(wx, wy, sx, sy);
    float zoom = camera.GetZoom();
    float bleed = (zoom < 1.0f) ? 0.6f : ((zoom < 2.0f) ? 0.25f : 0.0f);
    SDL_FRect rect = {sx, sy, ww * zoom + bleed, wh * zoom + bleed};
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(m_renderer, &rect);
}

void Renderer::DrawSelectionBubble(
    float centerX, float centerY, float halfW, float halfH, const Camera &camera, SDL_Color color, float brighten)
{
    PlainState();
    if (!m_glowTexture)
    {
        constexpr int kSize = 128;
        std::vector<std::uint32_t> px(static_cast<std::size_t>(kSize) * kSize);
        for (int y = 0; y < kSize; ++y)
        {
            for (int x = 0; x < kSize; ++x)
            {
                const float dx = (x + 0.5f) / kSize * 2.0f - 1.0f, dy = (y + 0.5f) / kSize * 2.0f - 1.0f;
                const float r = std::sqrt(dx * dx + dy * dy);
                float a = 0.0f;
                if (r < 1.0f)
                {
                    a = std::pow(r, 2.2f) * 0.30f + 0.04f; // clear in the middle, denser toward the rim
                    const float rim = (r - 0.90f) / 0.06f;
                    a += std::exp(-rim * rim) * 0.75f;               // the glowing rim
                    a *= std::clamp((1.0f - r) / 0.10f, 0.0f, 1.0f); // soft outer edge
                }
                const std::uint8_t v = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
                px[static_cast<std::size_t>(y) * kSize + x] = (static_cast<std::uint32_t>(v) << 24) | 0x00FFFFFFu;
            }
        }
        m_glowTexture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kSize, kSize);
        if (!m_glowTexture)
            return;
        SDL_UpdateTexture(m_glowTexture, nullptr, px.data(), kSize * static_cast<int>(sizeof(std::uint32_t)));
        SDL_SetTextureScaleMode(m_glowTexture, SDL_SCALEMODE_LINEAR);
    }
    // ~1.1 pulses per second
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(SDL_GetTicks() % 100000) * 0.00691f);
    float sx = 0.0f, sy = 0.0f;
    camera.WorldToScreen(centerX, centerY, sx, sy);
    const float zoom = camera.GetZoom();
    const float k = 1.0f + 0.05f * pulse;
    const SDL_FRect dst {
        sx - halfW * zoom * k, sy - halfH * zoom * k, halfW * 2.0f * zoom * k, halfH * 2.0f * zoom * k};
    SDL_SetTextureColorMod(m_glowTexture, color.r, color.g, color.b);
    SDL_SetTextureBlendMode(m_glowTexture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(m_glowTexture, static_cast<Uint8>(150.0f + 90.0f * pulse));
    SDL_RenderTexture(m_renderer, m_glowTexture, nullptr, &dst);
    if (brighten > 0.0f)
    {
        // A light veil over the object itself: the sprite below seems to shine, breathing with the bubble
        SDL_SetTextureColorMod(m_glowTexture, 255, 255, 255);
        SDL_SetTextureBlendMode(m_glowTexture, SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaMod(
            m_glowTexture, static_cast<Uint8>(std::clamp(brighten * (60.0f + 110.0f * pulse), 0.0f, 255.0f)));
        const SDL_FRect veil {
            sx - halfW * zoom * 0.95f, sy - halfH * zoom * 0.95f, halfW * 1.9f * zoom, halfH * 1.9f * zoom};
        SDL_RenderTexture(m_renderer, m_glowTexture, nullptr, &veil);
        SDL_SetTextureBlendMode(m_glowTexture, SDL_BLENDMODE_BLEND);
    }
}

void Renderer::DrawRectWorld(float wx, float wy, float ww, float wh, const Camera &camera, SDL_Color color)
{
    PlainState();
    float sx, sy;
    camera.WorldToScreen(wx, wy, sx, sy);
    float zoom = camera.GetZoom();
    SDL_FRect rect = {sx, sy, ww * zoom, wh * zoom};
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_RenderRect(m_renderer, &rect);
}

void Renderer::DrawLineWorld(float wx1, float wy1, float wx2, float wy2, const Camera &camera, SDL_Color color)
{
    PlainState();
    float sx1, sy1, sx2, sy2;
    camera.WorldToScreen(wx1, wy1, sx1, sy1);
    camera.WorldToScreen(wx2, wy2, sx2, sy2);
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_RenderLine(m_renderer, sx1, sy1, sx2, sy2);
}

void Renderer::DrawRingScreen(float cx, float cy, float rx, float ry, float thickness, SDL_Color color, int segments)
{
    if (rx <= 0.5f || ry <= 0.5f || color.a == 0)
        return;
    PlainState();
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    segments = std::clamp(segments, 24, 720);
    const float half = std::max(0.5f, thickness * 0.5f);
    const SDL_FColor c {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    vertices.reserve(static_cast<std::size_t>(segments) * 2);
    indices.reserve(static_cast<std::size_t>(segments) * 6);
    for (int i = 0; i < segments; ++i)
    {
        const float a = 2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / static_cast<float>(segments);
        const float ca = std::cos(a), sa = std::sin(a);
        vertices.push_back({{cx + ca * (rx + half), cy + sa * (ry + half)}, c, {0.0f, 0.0f}});
        vertices.push_back(
            {{cx + ca * std::max(0.0f, rx - half), cy + sa * std::max(0.0f, ry - half)}, c, {0.0f, 0.0f}});
        const int o = i * 2, n = ((i + 1) % segments) * 2;
        indices.insert(indices.end(), {o, o + 1, n, n, o + 1, n + 1});
    }
    SDL_RenderGeometry(
        m_renderer,
        nullptr,
        vertices.data(),
        static_cast<int>(vertices.size()),
        indices.data(),
        static_cast<int>(indices.size()));
}

void Renderer::DrawThickLineScreen(float x0, float y0, float x1, float y1, float width, SDL_Color color)
{
    if (color.a == 0)
        return;
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.01f)
        return;
    PlainState();
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    const float nx = -dy / len * std::max(0.5f, width) * 0.5f, ny = dx / len * std::max(0.5f, width) * 0.5f;
    const SDL_FColor c {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
    const SDL_Vertex v[4]
        = {{{x0 + nx, y0 + ny}, c, {0, 0}},
           {{x0 - nx, y0 - ny}, c, {0, 0}},
           {{x1 + nx, y1 + ny}, c, {0, 0}},
           {{x1 - nx, y1 - ny}, c, {0, 0}}};
    const int idx[6] = {0, 1, 2, 2, 1, 3};
    SDL_RenderGeometry(m_renderer, nullptr, v, 4, idx, 6);
}

void Renderer::FillRectScreen(int x, int y, int w, int h, SDL_Color color)
{
    PlainState();
    SDL_FRect rect = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(m_renderer, &rect);
}

void Renderer::DrawRectScreen(int x, int y, int w, int h, SDL_Color color)
{
    PlainState();
    SDL_FRect rect = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_RenderRect(m_renderer, &rect);
}

void Renderer::DrawLineScreen(int x1, int y1, int x2, int y2, SDL_Color color)
{
    PlainState();
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_RenderLine(
        m_renderer, static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(x2), static_cast<float>(y2));
}

void Renderer::DrawTextureScreen(SDL_Texture *tex, int x, int y, int w, int h, int anchor)
{
    PlainState();
    if (!tex)
        return;
    float texW = 0.0f, texH = 0.0f;
    SDL_GetTextureSize(tex, &texW, &texH);
    if (w <= 0)
        w = static_cast<int>(texW);
    if (h <= 0)
        h = static_cast<int>(texH);

    ApplyAnchor(x, y, w, h, anchor);
    SDL_FRect dstRect = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
    SDL_RenderTexture(m_renderer, tex, nullptr, &dstRect);
}

void Renderer::DrawRawSpriteScreen(int spriteId, int screenX, int screenY, float scale, Faction faction)
{
    PlainState();
    if (!m_texManager)
        return;
    const auto *frame = m_texManager->GetSpriteFrame(spriteId);
    if (!frame || frame->width <= 0 || frame->height <= 0 || frame->srcW <= 0 || frame->srcH <= 0)
        return;

    int page = faction == Faction::Resistance ? m_texManager->ResistancePage(frame->page) : frame->page;

    // An owner set (HUD icons of the player): the army colour as in the world; otherwise the plain image
    SDL_Texture *sheet = m_drawOwner < kMaxPlayers ? BindSheet(page, faction, frame->recolor)
                                                   : m_texManager->GetSpriteSheet(page);
    if (!sheet)
        return;
    if (m_drawOwner >= kMaxPlayers && page >= 0 && page < static_cast<int>(m_tintedSheet.size())
        && m_tintedSheet[page])
    {
        SDL_SetTextureColorMod(sheet, 255, 255, 255);
        m_tintedSheet[page] = false;
    }

    const SDL_FRect srcRect = FrameSource(*frame);
    SDL_FRect dstRect
        = {static_cast<float>(screenX),
           static_cast<float>(screenY),
           std::max(1.0f, std::round(frame->width * scale)),
           std::max(1.0f, std::round(frame->height * scale))};
    SDL_RenderTexture(m_renderer, sheet, &srcRect, &dstRect);
}

void Renderer::DrawCompositeScreen(int compId, int screenX, int screenY, float scale, Faction faction, int depth)
{
    if (depth > 6 || !m_texManager)
        return;

    IndexRange r = m_texManager->GetCompositeRange(compId);
    if (r.end < r.start)
        return;

    for (int p = r.start; p <= r.end; ++p)
    {
        const auto *part = m_texManager->GetCompositePart(p);
        if (!part)
            continue;

        int px = screenX + static_cast<int>(std::round(part->ox * scale));
        int py = screenY + static_cast<int>(std::round(part->oy * scale));

        if (part->isComp == 0)
        {
            DrawRawSpriteScreen(part->child, px, py, scale, faction);
        }
        else
        {
            DrawCompositeScreen(part->child, px, py, scale, faction, depth + 1);
        }
    }
}

void Renderer::ApplyAnchor(int &x, int &y, int w, int h, int anchor) const
{
    if (anchor & Anchor::HCENTER)
    {
        x -= w / 2;
    }
    else if (anchor & Anchor::RIGHT)
    {
        x -= w;
    }

    if (anchor & Anchor::VCENTER)
    {
        y -= h / 2;
    }
    else if (anchor & Anchor::BOTTOM)
    {
        y -= h;
    }
}

void Renderer::SetClipRect(const SDL_Rect &rect)
{
    SDL_SetRenderClipRect(m_renderer, &rect);
}

void Renderer::ResetClipRect()
{
    SDL_SetRenderClipRect(m_renderer, nullptr);
}

} // namespace OpenArtOfWar
