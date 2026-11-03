#include "graphics/NightLighting.hpp"
#include "graphics/Camera.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace OpenArtOfWar
{

namespace
{
constexpr SDL_Color kNight {104, 116, 158, 255}; // what is left of the daylight (about the shader's Night grade)
constexpr SDL_Color kLight {255, 196, 130, 255}; // fire light, warm
constexpr int kSpotSize = 128;
} // namespace

NightLighting::~NightLighting()
{
    Shutdown();
}

void NightLighting::Shutdown()
{
    if (m_light)
        SDL_DestroyTexture(m_light);
    if (m_spot)
        SDL_DestroyTexture(m_spot);
    m_light = nullptr;
    m_spot = nullptr;
    m_lightW = m_lightH = 0;
}

bool NightLighting::EnsureTextures(SDL_Renderer *renderer, int w, int h)
{
    if (renderer != m_renderer)
    {
        // A new renderer (the old one took its textures with it)
        m_light = m_spot = nullptr;
        m_lightW = m_lightH = 0;
        m_renderer = renderer;
    }
    if (!m_spot)
    {
        std::vector<uint32_t> pixels(static_cast<size_t>(kSpotSize) * kSpotSize);
        const float c = (kSpotSize - 1) * 0.5f;
        for (int y = 0; y < kSpotSize; ++y)
        {
            for (int x = 0; x < kSpotSize; ++x)
            {
                const float d = std::sqrt((x - c) * (x - c) + (y - c) * (y - c)) / c;
                const float f = std::clamp(1.0f - d, 0.0f, 1.0f);
                const auto a = static_cast<uint32_t>(f * f * 255.0f + 0.5f); // soft towards the edge
                pixels[static_cast<size_t>(y) * kSpotSize + x] = (a << 24) | 0x00FFFFFFu;
            }
        }
        m_spot = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kSpotSize, kSpotSize);
        if (!m_spot)
            return false;
        SDL_UpdateTexture(m_spot, nullptr, pixels.data(), kSpotSize * 4);
        SDL_SetTextureBlendMode(m_spot, SDL_BLENDMODE_ADD);
        SDL_SetTextureScaleMode(m_spot, SDL_SCALEMODE_LINEAR);
    }
    const int lw = std::max(1, w / kDivisor), lh = std::max(1, h / kDivisor);
    if (!m_light || m_lightW != lw || m_lightH != lh)
    {
        if (m_light)
            SDL_DestroyTexture(m_light);
        m_light = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, lw, lh);
        m_lightW = lw;
        m_lightH = lh;
        if (!m_light)
            return false;
        SDL_SetTextureBlendMode(m_light, SDL_BLENDMODE_MOD);
        SDL_SetTextureScaleMode(m_light, SDL_SCALEMODE_LINEAR);
    }
    return true;
}

void NightLighting::Apply(
    SDL_Renderer *renderer, const Camera &camera, const std::vector<EntityManager::LightSpot> &lights, int w, int h)
{
    if (!renderer || w <= 0 || h <= 0 || !EnsureTextures(renderer, w, h))
        return;
    SDL_Texture *target = SDL_GetRenderTarget(renderer);
    SDL_Rect clip;
    const bool clipped = SDL_RenderClipEnabled(renderer) && SDL_GetRenderClipRect(renderer, &clip);
    SDL_SetRenderClipRect(renderer, nullptr);

    // The light map: the night, and every light added onto it
    SDL_SetRenderTarget(renderer, m_light);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, kNight.r, kNight.g, kNight.b, 255);
    SDL_RenderClear(renderer);
    const float scale = camera.GetZoom() / static_cast<float>(kDivisor);
    SDL_SetTextureColorMod(m_spot, kLight.r, kLight.g, kLight.b);
    for (const EntityManager::LightSpot &l : lights)
    {
        float sx, sy;
        camera.WorldToScreen(l.x, l.y, sx, sy);
        sx /= static_cast<float>(kDivisor);
        sy /= static_cast<float>(kDivisor);
        const float r = l.radius * scale;
        if (sx + r < 0.0f || sy + r < 0.0f || sx - r > m_lightW || sy - r > m_lightH)
            continue;
        SDL_SetTextureAlphaModFloat(m_spot, std::clamp(l.intensity, 0.0f, 1.0f));
        const SDL_FRect dst {sx - r, sy - r, 2.0f * r, 2.0f * r};
        SDL_RenderTexture(renderer, m_spot, nullptr, &dst);
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
