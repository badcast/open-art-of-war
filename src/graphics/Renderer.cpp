#include "graphics/Renderer.hpp"
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
    const std::vector<uint8_t> &alpha,
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
    if (w <= 0 || h <= 0 || alpha.size() < static_cast<size_t>(w) * h)
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
    const uint32_t rgb = (static_cast<uint32_t>(color.r) << 16) | (static_cast<uint32_t>(color.g) << 8) | color.b;
    m_maskPixels.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < m_maskPixels.size(); ++i)
        m_maskPixels[i] = (static_cast<uint32_t>(alpha[i]) << 24) | rgb;
    SDL_Rect region {0, 0, w, h};
    SDL_UpdateTexture(m_maskTexture, &region, m_maskPixels.data(), w * static_cast<int>(sizeof(uint32_t)));

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

SDL_Texture *Renderer::BindSheet(int &page, Faction faction)
{
    // Sheets with team armour: Confederation (blue) 5, 7, 8, 10, 11, 14, 15 and Resistance (red) 6, 12, 13, 16, 17, 29,
    // 31; sheet 9 is the red copy of 8 (same layout). An army colour recolours the armour; every other pixel stays.
    (void) faction; // the classic sheet of the faction is chosen by the caller
    const PlayerId owner = m_drawOwner;
    const bool blueSheet = page == 5 || page == 7 || page == 8 || page == 10 || page == 11 || page == 14 || page == 15;
    const bool redSheet = page == 6 || page == 12 || page == 13 || page == 16 || page == 17 || page == 29 || page == 31;
    const bool custom = owner < kMaxPlayers && m_armyCustom[owner];
    int team = 0;
    if (custom)
    {
        if (page == 9)
            page = 8;
        team = (blueSheet || page == 8) ? 1 : (redSheet ? 2 : 0);
        // Buildings, walls and icons: their trims sit on the shared sheets too
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
        const bool tracked = page >= 0 && page < 64;
        if (team != 0 || blueSheet || redSheet || page == 8 || (tracked && m_tintedSheet[page]))
        {
            // The colour mod carries the army colour to the shader; plain draws of the same sheet stay white
            const SDL_Color c = team ? m_armyColor[owner] : SDL_Color {255, 255, 255, 255};
            SDL_SetTextureColorMod(sheet, c.r, c.g, c.b);
            if (tracked)
                m_tintedSheet[page] = team != 0;
        }
    }
    return sheet;
}

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
    if (!frame || frame->width <= 0 || frame->height <= 0)
        return;

    int page = frame->page;
    if (page == 8 && faction == Faction::Resistance)
    {
        page = 9; // Red faction (Resistance) switches blue base sheet 8 to red sheet 9
    }

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

    SDL_Texture *sheet = BindSheet(page, faction);
    if (!sheet)
        return;
    float sScale = m_texManager->GetSheetScale(page);
    SDL_FRect srcRect
        = {std::round(frame->x * sScale),
           std::round(frame->y * sScale),
           std::round(frame->width * sScale),
           std::round(frame->height * sScale)};
    SDL_RenderTexture(m_renderer, sheet, &srcRect, &dstRect);
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
            if (!sf)
                continue;
            int page = sf->page;
            if (faction == Faction::Confederation && page == 9)
                page = 8;
            else if (faction == Faction::Confederation && page == 12)
                page = 10;
            SDL_Texture *sheet = BindSheet(page, faction);
            if (!sheet)
                continue;

            float sx, sy;
            camera.WorldToScreen(worldX, worldY, sx, sy);
            float zoom = camera.GetZoom();
            float drawW = sf->width * zoom;
            float drawH = sf->height * zoom;
            float bleed = (zoom < 1.0f) ? 0.6f : ((zoom < 2.0f) ? 0.25f : 0.0f);
            SDL_FRect dstRect = {sx, sy, drawW + bleed, drawH + bleed};

            float sScale = tm->GetSheetScale(page);
            SDL_FRect srcRect
                = {std::round(sf->x * sScale),
                   std::round(sf->y * sScale),
                   std::round(sf->width * sScale),
                   std::round(sf->height * sScale)};
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
        if (!sf)
            return;
        int page = sf->page;
        if (faction == Faction::Confederation && page == 9)
            page = 8;
        else if (faction == Faction::Confederation && page == 12)
            page = 10;
        SDL_Texture *sheet = BindSheet(page, faction);
        if (!sheet)
            return;

        float sx, sy;
        camera.WorldToScreen(worldX + baseOx, worldY + baseOy, sx, sy);
        float zoom = camera.GetZoom();
        float drawW = sf->width * zoom;
        float drawH = sf->height * zoom;
        float bleed = (zoom < 1.0f) ? 0.6f : ((zoom < 2.0f) ? 0.25f : 0.0f);
        SDL_FRect dstRect = {sx, sy, drawW + bleed, drawH + bleed};

        float sScale = m_texManager->GetSheetScale(page);
        SDL_FRect srcRect
            = {std::round(sf->x * sScale),
               std::round(sf->y * sScale),
               std::round(sf->width * sScale),
               std::round(sf->height * sScale)};
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

    // Tileset decoration entries map to building IDs (31..338: trees, bridges 83..108, cliffs, landmarks 121..132).
    // Do not use m_decorAnims here: those are post-destruction craters and vehicle wrecks.
    int buildingId = m_texManager->GetDecorBuildingId(tilesetId, decorType);
    if (buildingId >= 0)
    {
        if (layer < 0)
        {
            DrawBuildingWorld(buildingId, worldX, worldY, 0, camera, Faction::Neutral, animFrame, 1.0f, false);
            DrawBuildingWorld(buildingId, worldX, worldY, 1, camera, Faction::Neutral, animFrame, 1.0f, false);
        }
        else
        {
            DrawBuildingWorld(buildingId, worldX, worldY, layer, camera, Faction::Neutral, animFrame, 1.0f, false);
        }
    }
}

int Renderer::TallDecorRows(int tilesetId, int decorType) const
{
    if (!m_texManager)
        return 0;
    const int id = m_texManager->GetDecorBuildingId(tilesetId, decorType);
    if (id < 31 || id >= 83)
        return 0;
    const BuildingDef *def = m_texManager->GetBuildingDef(id);
    return def ? std::max<int>(1, def->height) : 1;
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

    const auto *bdef = m_texManager->GetBuildingDef(buildingType);
    if (!bdef || bdef->end < bdef->start)
        return;

    lastDrawnBuilding = {buildingType, animFrame, m_animatedPartsVisible};

    // 1. Destroyed building remnant
    // Static crater/rubble composite that stays for 2 minutes without looping frames!
    if (isDestroyed)
    {
        if (layer == 0)
        {
            int baseRemnant = 0, stages = 0;
            DefaultBuildingWreck(bdef->width, bdef->height, baseRemnant, stages);
            if (customWreckComp > 0)
                baseRemnant = customWreckComp;
            if (customWreckStages > 0)
                stages = customWreckStages;
            // Gradual burial over 120 seconds: animFrame is the stage
            int stage = std::clamp(animFrame, 0, stages - 1);
            // Remnant composite is centered at (centerX, centerY) of the building
            float centerX = worldX + (bdef->width * OpenArtOfWar::TILE_WIDTH * 0.5f);
            float centerY = worldY + (bdef->height * OpenArtOfWar::TILE_HEIGHT * 0.5f);
            DrawCompositeWorld(baseRemnant + stage, centerX - 90.0f, centerY - 60.0f, camera, faction);
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
    if (buildingType < 30 && !isDestroyed)
        SetStructureFaction(faction);

    // 2. Walls (15..29), construction scaffolding (30), and map decorations (31..338)
    if (buildingType >= 15)
    {
        for (int p = bdef->start; p <= bdef->end; ++p)
        {
            const auto *part = m_texManager->GetBuildingPart(p);
            if (!part)
                continue;
            // Ids from 133 have no layer split: all their parts go with layer 0 (drawn once, not once per layer)
            const bool inLayer = buildingType >= 133 ? layer == 0 : part->layer == layer;
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
                else if (part->isComp == 2 && buildingType < 30)
                {
                    DrawAnimSequenceWorld(part->child, animFrame, px, py, camera, faction);
                }
            }
        }
        return;
    }

    // 3. Active faction buildings 0..14
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

    unitTypeId = std::clamp(unitTypeId, 0, 18);
    bool isInfantry = Utility::IsInfantryArchetype(unitTypeId); // archetypes 0..5 and 14 (prone sniper)

    // 1. Dying / Wrecked state
    if (actionState == static_cast<int>(ActionState::Die))
    {
        if (!isInfantry)
        {
            // Destroyed vehicle remnant - static wreck that fades over 2 minutes!
            const EffectsConfig::Wreck def = EffectsConfig::Get().WreckOfVehicle(unitTypeId);
            const int baseWreck = (customWreckComp > 0) ? customWreckComp : def.composite;
            const int maxStages = (customWreckStages > 0) ? customWreckStages : def.stages;
            int stage = std::clamp(animStep, 0, maxStages - 1);
            // Vehicle wreck composite is centered at (centerX, centerY) of the unit tile
            float centerX = worldX + (static_cast<float>(OpenArtOfWar::TILE_WIDTH) * 0.5f);
            float centerY = worldY + (static_cast<float>(OpenArtOfWar::TILE_HEIGHT) * 0.5f);
            DrawCompositeWorld(baseWreck + stage, centerX - 90.0f, centerY - 60.0f, camera, faction);
            return;
        }
        else
        {
            // Human death and corpse sequences:
            // Bullet death: sequences 247 or 248 (corpse variants)
            // Flame death: sequences 259 or 260 (burning human -> charred remains)
            int deathSeq = customDeathSeq;
            if (deathSeq <= 0)
            {
                if (isFlaming)
                {
                    deathSeq = (unitTypeId % 2 == 0) ? 259 : 260;
                }
                else
                {
                    deathSeq = 247 + (unitTypeId % 2);
                }
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

    int baseSeq = m_texManager->GetUnitBaseSeq(unitTypeId, actionState);

    // Infantry shooting pose offset (+8):
    // archetypes 0..3 (riflemen, heavy assault, grenadiers) switch to their firing pose. The flamethrower (5)
    // fires from its standing sequence: its firing layer is the flame; its +8 sequences are a walk cycle.
    int shootOffset = 0;
    if (((15 & (1 << unitTypeId)) != 0) && actionState == static_cast<int>(ActionState::Attack))
    {
        shootOffset = 8;
    }

    // Deployed siege units use a fixed hull sequence while their turret or gun rotates via tdir8.
    int dir8 = (unitTypeId >= 14 && unitTypeId <= 16) ? 0 : ((facingDir % 8 + 8) % 8);
    int tdir8 = (targetFacing >= 0) ? ((targetFacing % 8 + 8) % 8) : ((facingDir % 8 + 8) % 8);
    int seqIdx = baseSeq;
    if (actionState != static_cast<int>(ActionState::Transform) && baseSeq < 230)
    {
        seqIdx = baseSeq + shootOffset + dir8;
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

    uint8_t bMask = 0x07; // Bits 0, 1, 2 enabled by default
    if (actionState == static_cast<int>(ActionState::Attack))
    {
        bMask |= (1 << 3) | (1 << 4) | (1 << 5);
    }

    for (int lay = br.start; lay <= br.end; ++lay)
    {
        const auto *layer = m_texManager->GetUnitLayer(lay);
        if (!layer)
            continue;

        uint8_t flag = layer->flag;
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
    DrawCompositeWorld(1440 + f, worldX - 90.0f, worldY - 60.0f, camera, Faction::Resistance);
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
        std::vector<uint32_t> px(static_cast<size_t>(kSize) * kSize);
        for (int y = 0; y < kSize; ++y)
        {
            for (int x = 0; x < kSize; ++x)
            {
                const float dx = (x + 0.5f) / kSize * 2.0f - 1.0f, dy = (y + 0.5f) / kSize * 2.0f - 1.0f;
                const float r = std::sqrt(dx * dx + dy * dy);
                float a = 0.0f;
                if (r < 1.0f)
                {
                    a = std::pow(r, 2.2f) * 0.30f + 0.04f;        // clear in the middle, denser toward the rim
                    const float rim = (r - 0.90f) / 0.06f;
                    a += std::exp(-rim * rim) * 0.75f;            // the glowing rim
                    a *= std::clamp((1.0f - r) / 0.10f, 0.0f, 1.0f); // soft outer edge
                }
                const uint8_t v = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
                px[static_cast<size_t>(y) * kSize + x] = (static_cast<uint32_t>(v) << 24) | 0x00FFFFFFu;
            }
        }
        m_glowTexture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kSize, kSize);
        if (!m_glowTexture)
            return;
        SDL_UpdateTexture(m_glowTexture, nullptr, px.data(), kSize * static_cast<int>(sizeof(uint32_t)));
        SDL_SetTextureScaleMode(m_glowTexture, SDL_SCALEMODE_LINEAR);
    }
    // ~1.1 pulses per second
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(SDL_GetTicks() % 100000) * 0.00691f);
    float sx = 0.0f, sy = 0.0f;
    camera.WorldToScreen(centerX, centerY, sx, sy);
    const float zoom = camera.GetZoom();
    const float k = 1.0f + 0.05f * pulse;
    const SDL_FRect dst {sx - halfW * zoom * k, sy - halfH * zoom * k, halfW * 2.0f * zoom * k, halfH * 2.0f * zoom * k};
    SDL_SetTextureColorMod(m_glowTexture, color.r, color.g, color.b);
    SDL_SetTextureBlendMode(m_glowTexture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(m_glowTexture, static_cast<Uint8>(150.0f + 90.0f * pulse));
    SDL_RenderTexture(m_renderer, m_glowTexture, nullptr, &dst);
    if (brighten > 0.0f)
    {
        // A light veil over the object itself: the sprite below seems to shine, breathing with the bubble
        SDL_SetTextureColorMod(m_glowTexture, 255, 255, 255);
        SDL_SetTextureBlendMode(m_glowTexture, SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaMod(m_glowTexture, static_cast<Uint8>(std::clamp(brighten * (60.0f + 110.0f * pulse), 0.0f, 255.0f)));
        const SDL_FRect veil {sx - halfW * zoom * 0.95f, sy - halfH * zoom * 0.95f, halfW * 1.9f * zoom, halfH * 1.9f * zoom};
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
    vertices.reserve(static_cast<size_t>(segments) * 2);
    indices.reserve(static_cast<size_t>(segments) * 6);
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
    if (!frame || frame->width <= 0 || frame->height <= 0)
        return;

    int page = frame->page;
    if (page == 8 && faction == Faction::Resistance)
    {
        page = 9;
    }

    // An owner set (HUD icons of the player): the army colour as in the world; otherwise the plain sheet
    SDL_Texture *sheet = m_drawOwner < kMaxPlayers ? BindSheet(page, faction) : m_texManager->GetSpriteSheet(page);
    if (!sheet)
        return;
    if (m_drawOwner >= kMaxPlayers && page >= 0 && page < 64 && m_tintedSheet[page])
    {
        SDL_SetTextureColorMod(sheet, 255, 255, 255);
        m_tintedSheet[page] = false;
    }

    float sScale = m_texManager->GetSheetScale(page);
    SDL_FRect srcRect
        = {std::round(frame->x * sScale),
           std::round(frame->y * sScale),
           std::round(frame->width * sScale),
           std::round(frame->height * sScale)};
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
