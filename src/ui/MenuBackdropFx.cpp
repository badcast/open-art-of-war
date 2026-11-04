#include "ui/MenuBackdropFx.hpp"

#include "game/Effects.hpp"
#include "graphics/Camera.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/TextureManager.hpp"

#include <algorithm>
#include <cmath>

namespace OpenArtOfWar
{

namespace
{

constexpr size_t kMaxBlasts = 6;
constexpr float kSlowdown = 2.6f; // the blast plays this many times slower than in the game

float Uniform(std::mt19937 &rng, float lo, float hi)
{
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}

} // namespace

void MenuBackdropFx::Spawn(TextureManager &textures, int areaW, int windowH)
{
    // The blasts of the game: vehicles, the chain of a building and its big main blasts
    const EffectsConfig &fx = EffectsConfig::Get();
    std::vector<int> pool = fx.vehicleMain;
    pool.insert(pool.end(), fx.buildingExtraAnims.begin(), fx.buildingExtraAnims.end());
    pool.insert(pool.end(), {fx.buildingSmallMain, fx.buildingPowerMain, fx.buildingLargeMain, fx.buildingBigAnim});
    pool.erase(
        std::remove_if(
            pool.begin(),
            pool.end(),
            [&](int a) { return textures.GetAnimRange(a).end < textures.GetAnimRange(a).start; }),
        pool.end());
    if (pool.empty())
        return;
    Blast b;
    b.anim = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(m_rng)];
    const IndexRange r = textures.GetAnimRange(b.anim);
    const int frames = std::max(1, r.end - r.start + 1);
    b.zoom = Uniform(m_rng, 4.0f, 8.0f);
    b.life = std::max(2.0f, static_cast<float>(frames) * fx.frameTime * kSlowdown);
    b.peak = Uniform(m_rng, 0.5f, 0.85f);
    b.x = Uniform(m_rng, areaW * 0.08f, areaW * 0.92f);
    b.y = Uniform(m_rng, windowH * 0.12f, windowH * 0.88f);
    m_blasts.push_back(b);
}

void MenuBackdropFx::Render(
    Renderer &renderer, TextureManager &textures, int areaW, int windowW, int windowH, Uint32 ticks)
{
    if (areaW <= 0)
        return;
    float dt = m_lastTicks ? static_cast<float>(ticks - m_lastTicks) / 1000.0f : 0.0f;
    m_lastTicks = ticks;
    dt = std::clamp(dt, 0.0f, 0.1f);

    for (Blast &b : m_blasts)
        b.age += dt;
    m_blasts.erase(
        std::remove_if(m_blasts.begin(), m_blasts.end(), [](const Blast &b) { return b.age >= b.life; }),
        m_blasts.end());
    m_nextSpawn -= dt;
    if (m_nextSpawn <= 0.0f && m_blasts.size() < kMaxBlasts)
    {
        Spawn(textures, areaW, windowH);
        m_nextSpawn = Uniform(m_rng, 0.5f, 1.6f);
    }

    const float frameTime = EffectsConfig::Get().frameTime * kSlowdown;
    Camera cam(windowW, windowH);
    cam.SetZoomLimits(0.5f, 12.0f);
    renderer.SetClipRect({0, 0, areaW, windowH});
    for (const Blast &b : m_blasts)
    {
        const IndexRange r = textures.GetAnimRange(b.anim);
        const int frames = std::max(1, r.end - r.start + 1);
        const float t = std::clamp(b.age / b.life, 0.0f, 1.0f);
        const float envelope = std::pow(std::sin(t * 3.14159265f), 1.4f) * b.peak; // soft in and out
        const float pos = b.age / frameTime;
        const int f0 = std::min(frames - 1, static_cast<int>(pos));
        const int f1 = std::min(frames - 1, f0 + 1);
        const float mix = std::clamp(pos - static_cast<float>(f0), 0.0f, 1.0f);

        cam.SetZoom(b.zoom);
        cam.SetPosition(-b.x / b.zoom, -b.y / b.zoom);
        float ox = 0.0f, oy = 0.0f;
        ExplosionCentering(b.anim, ox, oy);
        renderer.SetDrawAlpha(envelope * (1.0f - mix));
        renderer.DrawAnimSequenceWorld(b.anim, f0, ox, oy, cam);
        if (f1 != f0)
        {
            renderer.SetDrawAlpha(envelope * mix);
            renderer.DrawAnimSequenceWorld(b.anim, f1, ox, oy, cam);
        }
    }
    renderer.SetDrawAlpha(1.0f);
    renderer.ResetClipRect();
}

} // namespace OpenArtOfWar
