#include "graphics/NightLighting.hpp"
#include "core/AssetStore.hpp"
#include "core/Config.hpp"
#include "core/JsonFile.hpp"
#include "game/TileMap.hpp"
#include "graphics/Camera.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>

namespace OpenArtOfWar
{

namespace
{
constexpr int kSpotSize = 128;

SDL_Color ReadColor(const nlohmann::json &j, const char *key, SDL_Color fallback)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 3)
        return fallback;
    auto c = [&](std::size_t i) {
        return static_cast<Uint8>(std::clamp((*it)[i].get<int>(), 0, 255));
    };
    return {c(0), c(1), c(2), 255};
}

void ReadSpot(const nlohmann::json &j, const char *key, NightConfig::Spot &spot)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_object())
        return;
    spot.radius = std::max(0.0f, it->value("radius", spot.radius));
    spot.intensity = std::clamp(it->value("intensity", spot.intensity), 0.0f, 1.0f);
}
} // namespace

bool NightConfig::Load(const std::string &path)
{
    std::string text;
    if (!Assets::ReadText(path, text))
    {
        std::cerr << "[Night] Warning: " << path << " not found, the night uses its defaults" << std::endl;
        return false;
    }
    try
    {
        const nlohmann::json j = JsonFile::ReadFromString(text);
        nightColor = ReadColor(j, "night_color", nightColor);
        gpuBase = std::clamp(j.value("gpu_base", gpuBase), 0.2f, 1.0f);
        if (const auto e = j.find("effects"); e != j.end() && e->is_object())
        {
            effectColor = ReadColor(*e, "color", effectColor);
            effectSoftness = std::clamp(e->value("softness", effectSoftness), 0.25f, 8.0f);
            ReadSpot(*e, "explosion", explosion);
            explosionFade = std::clamp(e->value("explosion_fade", explosionFade), 0.0f, 1.0f);
            ReadSpot(*e, "flame_start", flameStart);
            ReadSpot(*e, "flame_end", flameEnd);
            ReadSpot(*e, "projectile", projectile);
            ReadSpot(*e, "muzzle_flash", muzzleFlash);
            muzzleFlashSeconds = std::clamp(e->value("muzzle_flash_seconds", muzzleFlashSeconds), 0.01f, 0.5f);
        }
        if (const auto b = j.find("buildings"); b != j.end() && b->is_object())
        {
            buildingsEnabled = b->value("enabled", buildingsEnabled);
            buildingSpreadTiles = std::clamp(b->value("spread_tiles", buildingSpreadTiles), 0.0f, 8.0f);
            buildingIntensity = std::clamp(b->value("intensity", buildingIntensity), 0.0f, 1.0f);
            buildingSoftness = std::clamp(b->value("softness", buildingSoftness), 0.25f, 8.0f);
            buildingColor = ReadColor(*b, "color", buildingColor);
            buildingUnpowered = std::clamp(b->value("unpowered", buildingUnpowered), 0.0f, 1.0f);
            buildingUnderConstruction
                = std::clamp(b->value("under_construction", buildingUnderConstruction), 0.0f, 1.0f);
        }
    }
    catch (const std::exception &ex)
    {
        std::cerr << "[Night] Warning: " << path << ": " << ex.what() << std::endl;
        return false;
    }
    return true;
}

NightLighting::~NightLighting()
{
    Shutdown();
}

void NightLighting::Configure(const NightConfig &config)
{
    m_config = config;
    // The fall-off of the pictures follows the softness: made again with it
    if (m_spot)
        SDL_DestroyTexture(m_spot);
    if (m_glowRect)
        SDL_DestroyTexture(m_glowRect);
    m_spot = m_glowRect = nullptr;
}

void NightLighting::Shutdown()
{
    if (m_light)
        SDL_DestroyTexture(m_light);
    if (m_spot)
        SDL_DestroyTexture(m_spot);
    if (m_glowRect)
        SDL_DestroyTexture(m_glowRect);
    m_light = m_spot = m_glowRect = nullptr;
    m_lightW = m_lightH = 0;
}

SDL_Texture *NightLighting::MakeSpot(SDL_Renderer *renderer, float softness)
{
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(kSpotSize) * kSpotSize);
    const float c = kSpotSize * 0.5f;
    for (int y = 0; y < kSpotSize; ++y)
    {
        for (int x = 0; x < kSpotSize; ++x)
        {
            // (pixel centres: the four middle pixels are all at the full light)
            const float dx = std::max(0.0f, std::fabs(x + 0.5f - c) - 0.5f),
                        dy = std::max(0.0f, std::fabs(y + 0.5f - c) - 0.5f);
            const float d = std::sqrt(dx * dx + dy * dy) / (c - 0.5f);
            const float f = std::pow(std::clamp(1.0f - d, 0.0f, 1.0f), softness);
            const auto a = static_cast<std::uint32_t>(f * 255.0f + 0.5f);
            pixels[static_cast<std::size_t>(y) * kSpotSize + x] = (a << 24) | 0x00FFFFFFu;
        }
    }
    SDL_Texture *t
        = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kSpotSize, kSpotSize);
    if (!t)
        return nullptr;
    SDL_UpdateTexture(t, nullptr, pixels.data(), kSpotSize * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

bool NightLighting::EnsureTextures(SDL_Renderer *renderer, int w, int h)
{
    if (renderer != m_renderer)
    {
        // A new renderer (the old one took its textures with it)
        m_light = m_spot = m_glowRect = nullptr;
        m_lightW = m_lightH = 0;
        m_renderer = renderer;
    }
    if (!m_spot)
        m_spot = MakeSpot(renderer, m_config.effectSoftness);
    if (!m_glowRect)
        m_glowRect = MakeSpot(renderer, m_config.buildingSoftness);
    const int lw = std::max(1, w / kDivisor), lh = std::max(1, h / kDivisor);
    if (!m_light || m_lightW != lw || m_lightH != lh)
    {
        if (m_light)
            SDL_DestroyTexture(m_light);
        m_light = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, lw, lh);
        m_lightW = lw;
        m_lightH = lh;
        if (m_light)
        {
            SDL_SetTextureBlendMode(m_light, SDL_BLENDMODE_MOD);
            SDL_SetTextureScaleMode(m_light, SDL_SCALEMODE_LINEAR);
        }
    }
    return m_light && m_spot && m_glowRect;
}

void NightLighting::DrawSpot(SDL_Renderer *renderer, float sx, float sy, float r, float intensity, SDL_Color color)
{
    if (r <= 0.0f || intensity <= 0.0f || sx + r < 0.0f || sy + r < 0.0f || sx - r > m_lightW || sy - r > m_lightH)
        return;
    SDL_SetTextureColorMod(m_spot, color.r, color.g, color.b);
    SDL_SetTextureAlphaModFloat(m_spot, std::clamp(intensity, 0.0f, 1.0f));
    const SDL_FRect dst {sx - r, sy - r, 2.0f * r, 2.0f * r};
    SDL_RenderTexture(renderer, m_spot, nullptr, &dst);
}

void NightLighting::DrawRect(
    SDL_Renderer *renderer, const SDL_FRect &rect, float spread, float edge, float core, SDL_Color color)
{
    if (edge <= 0.0f || rect.x + rect.w + spread < 0.0f || rect.y + rect.h + spread < 0.0f || rect.x - spread > m_lightW
        || rect.y - spread > m_lightH)
        return;
    // The footprint itself at full light
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
    SDL_SetRenderDrawColor(
        renderer, color.r, color.g, color.b, static_cast<Uint8>(std::clamp(core, 0.0f, 1.0f) * 255.0f));
    SDL_RenderFillRect(renderer, &rect);
    if (spread <= 0.0f)
        return;
    // ... and around it a border that fades out: the quarters of the soft disc at the corners, its middle row and
    // column stretched along the sides
    SDL_SetTextureColorMod(m_glowRect, color.r, color.g, color.b);
    SDL_SetTextureAlphaModFloat(m_glowRect, std::clamp(edge, 0.0f, 1.0f));
    const float h = kSpotSize * 0.5f;
    const float x0 = rect.x, y0 = rect.y, x1 = rect.x + rect.w, y1 = rect.y + rect.h, s = spread;
    struct Piece
    {
        SDL_FRect src, dst;
    };
    const Piece pieces[8] = {
        {{0, 0, h, h}, {x0 - s, y0 - s, s, s}}, // corners
        {{h, 0, h, h}, {x1, y0 - s, s, s}},
        {{0, h, h, h}, {x0 - s, y1, s, s}},
        {{h, h, h, h}, {x1, y1, s, s}},
        {{h - 0.5f, 0, 1, h}, {x0, y0 - s, rect.w, s}}, // top, bottom
        {{h - 0.5f, h, 1, h}, {x0, y1, rect.w, s}},
        {{0, h - 0.5f, h, 1}, {x0 - s, y0, s, rect.h}}, // left, right
        {{h, h - 0.5f, h, 1}, {x1, y0, s, rect.h}},
    };
    for (const Piece &p : pieces)
        SDL_RenderTexture(renderer, m_glowRect, &p.src, &p.dst);
}

void NightLighting::Apply(
    SDL_Renderer *renderer,
    const Camera &camera,
    const TileMap &map,
    bool fog,
    bool gpu,
    const std::vector<EntityManager::LightSpot> &lights,
    int w,
    int h)
{
    if (!renderer || w <= 0 || h <= 0 || !EnsureTextures(renderer, w, h))
        return;
    SDL_Texture *target = SDL_GetRenderTarget(renderer);
    SDL_Rect clip;
    const bool clipped = SDL_RenderClipEnabled(renderer) && SDL_GetRenderClipRect(renderer, &clip);
    SDL_SetRenderClipRect(renderer, nullptr);
    const NightConfig &c = m_config;
    const SDL_Color base
        = gpu ? SDL_Color {static_cast<Uint8>(c.gpuBase * 255.0f), static_cast<Uint8>(c.gpuBase * 255.0f), static_cast<Uint8>(c.gpuBase * 255.0f), 255}
              : c.nightColor;

    // The light map: the night, and every light added onto it
    SDL_SetRenderTarget(renderer, m_light);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, base.r, base.g, base.b, 255);
    SDL_RenderClear(renderer);
    const float scale = camera.GetZoom() / static_cast<float>(kDivisor);
    auto toMap = [&](float wx, float wy, float &sx, float &sy) {
        camera.WorldToScreen(wx, wy, sx, sy);
        sx /= static_cast<float>(kDivisor);
        sy /= static_cast<float>(kDivisor);
    };
    using Kind = EntityManager::LightSpot::Kind;
    // Buildings first, the fire of the battle over them
    if (c.buildingsEnabled)
    {
        for (const EntityManager::LightSpot &l : lights)
        {
            if (l.kind != Kind::Building)
                continue;
            float k = c.buildingIntensity * (l.powered ? 1.0f : c.buildingUnpowered)
                      * (l.complete ? 1.0f : c.buildingUnderConstruction);
            if (k <= 0.0f)
                continue;
            float sx, sy;
            toMap(l.x, l.y, sx, sy);
            DrawRect(
                renderer,
                {sx, sy, l.w * scale, l.h * scale},
                c.buildingSpreadTiles * TILE_WIDTH * scale,
                k,
                k,
                c.buildingColor);
        }
    }
    for (const EntityManager::LightSpot &l : lights)
    {
        const NightConfig::Spot *spot = l.kind == Kind::Explosion     ? &c.explosion
                                        : l.kind == Kind::FlameStart  ? &c.flameStart
                                        : l.kind == Kind::FlameEnd    ? &c.flameEnd
                                        : l.kind == Kind::Projectile  ? &c.projectile
                                        : l.kind == Kind::MuzzleFlash ? &c.muzzleFlash
                                                                      : nullptr;
        if (!spot)
            continue;
        float k = spot->intensity;
        if (l.kind == Kind::Explosion)
            k *= 1.0f - c.explosionFade * l.age;
        else if (l.kind == Kind::MuzzleFlash)
            k *= std::max(0.0f, 1.0f - l.age / c.muzzleFlashSeconds); // the flash fades out
        if (k <= 0.0f)
            continue;
        float sx, sy;
        toMap(l.x, l.y, sx, sy);
        DrawSpot(renderer, sx, sy, spot->radius * scale, k, c.effectColor);
    }

    // No light where the player sees nothing now: the tiles out of sight keep the plain night
    if (fog)
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, base.r, base.g, base.b, 255);
        float wx0, wy0, wx1, wy1;
        camera.ScreenToWorld(0.0f, 0.0f, wx0, wy0);
        camera.ScreenToWorld(static_cast<float>(w), static_cast<float>(h), wx1, wy1);
        const int tx0 = std::max(0, static_cast<int>(std::floor(wx0 / TILE_WIDTH)));
        const int ty0 = std::max(0, static_cast<int>(std::floor(wy0 / TILE_HEIGHT)));
        const int tx1 = std::min(map.GetWidth() - 1, static_cast<int>(std::floor(wx1 / TILE_WIDTH)));
        const int ty1 = std::min(map.GetHeight() - 1, static_cast<int>(std::floor(wy1 / TILE_HEIGHT)));
        const float tw = TILE_WIDTH * scale, th = TILE_HEIGHT * scale;
        for (int ty = ty0; ty <= ty1; ++ty)
        {
            for (int tx = tx0; tx <= tx1; ++tx)
            {
                if (map.GetFogState(tx, ty) == FogState::Visible)
                    continue;
                float sx, sy;
                toMap(static_cast<float>(tx * TILE_WIDTH), static_cast<float>(ty * TILE_HEIGHT), sx, sy);
                const SDL_FRect r {std::floor(sx), std::floor(sy), std::ceil(tw) + 1.0f, std::ceil(th) + 1.0f};
                SDL_RenderFillRect(renderer, &r);
            }
        }
    }

    // ... multiplied onto the world
    SDL_SetRenderTarget(renderer, target);
    const SDL_FRect dst {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)};
    SDL_RenderTexture(renderer, m_light, nullptr, &dst);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (clipped)
        SDL_SetRenderClipRect(renderer, &clip);
}

} // namespace OpenArtOfWar
