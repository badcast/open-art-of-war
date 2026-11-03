#pragma once

#include "game/EntityManager.hpp"
#include <SDL3/SDL.h>
#include <string>
#include <vector>

namespace OpenArtOfWar
{

class Camera;
class TileMap;

// How the night is lit (assets/data/nights.json, visual only: not part of the network assets check)
struct NightConfig
{
    // What is left of the daylight on render APIs without shader effects (the SDL GPU renderer grades the picture
    // as night itself and keeps the light map at gpuBase)
    SDL_Color nightColor {104, 116, 158, 255};
    float gpuBase {0.5f};
    // A light around a point: radius in world pixels, brightness 0..1, softness = how slowly it fades out towards its
    // edge (1 linear, 2 smooth, more: a small bright core)
    struct Spot
    {
        float radius {100.0f};
        float intensity {1.0f};
    };
    SDL_Color effectColor {255, 196, 130, 255};
    float effectSoftness {2.0f};
    Spot explosion {110.0f, 1.0f};
    float explosionFade {0.8f}; // how much of the explosion's light is gone by its last frame
    Spot flameStart {60.0f, 0.6f};
    Spot flameEnd {80.0f, 0.8f};
    Spot projectile {40.0f, 0.5f};
    // The light of buildings: their whole footprint lit, spreading spreadTiles tiles around it and fading out
    bool buildingsEnabled {true};
    float buildingSpreadTiles {2.0f};
    float buildingIntensity {0.3f};
    float buildingCoreIntensity {0.45f}; // over the footprint itself
    float buildingSoftness {1.6f};
    SDL_Color buildingColor {255, 214, 160, 255};
    float buildingUnpowered {0.35f};    // times the light of a building without power
    float buildingUnderConstruction {0.0f}; // times the light of a building not yet finished

    // Reads the file (missing values keep their defaults); false: not found / not readable
    bool Load(const std::string &path);
};

// The night of a match: a light map the size of the view (a fraction of it), filled with the night's colour, with a
// soft light added for every light source, multiplied onto the world picture. Tiles out of sight drop their light
// (the fog shows none). Only render targets and the ADD / MOD blend modes, which every SDL renderer has. On the SDL
// GPU renderer the colour grading shader makes the night and this map only adds the lights (`base` = gpuBase).
class NightLighting
{
public:
    NightLighting() = default;
    ~NightLighting();
    NightLighting(const NightLighting &) = delete;
    NightLighting &operator=(const NightLighting &) = delete;

    void Configure(const NightConfig &config);
    const NightConfig &Config() const
    {
        return m_config;
    }
    // Darkens what is drawn on the current render target over w x h (the world's view, `camera` drew it). `gpu`: the
    // shader grades the picture (the map stays at gpuBase). `fog`: tiles not in sight keep no light
    void Apply(
        SDL_Renderer *renderer,
        const Camera &camera,
        const TileMap &map,
        bool fog,
        bool gpu,
        const std::vector<EntityManager::LightSpot> &lights,
        int w,
        int h);
    void Shutdown();
    // The list of this frame's lights, kept between frames (no allocation each frame)
    std::vector<EntityManager::LightSpot> &Lights()
    {
        return m_lights;
    }

private:
    bool EnsureTextures(SDL_Renderer *renderer, int w, int h);
    SDL_Texture *MakeSpot(SDL_Renderer *renderer, float softness);
    void DrawSpot(SDL_Renderer *renderer, float sx, float sy, float r, float intensity, SDL_Color color);
    // A lit rectangle (the footprint, screen pixels of the light map) with a soft border of `spread` around it
    void DrawRect(SDL_Renderer *renderer, const SDL_FRect &rect, float spread, float edge, float core, SDL_Color color);

    NightConfig m_config;
    SDL_Renderer *m_renderer {nullptr}; // the textures belong to it
    static constexpr int kDivisor = 2;  // the light map at half the view: it is smooth anyway
    SDL_Texture *m_light {nullptr};
    int m_lightW {0}, m_lightH {0};
    SDL_Texture *m_spot {nullptr};     // a white disc whose alpha falls off to the edge (effects)
    SDL_Texture *m_glowRect {nullptr}; // the same with the buildings' softness (corners and edges of their light)
    std::vector<EntityManager::LightSpot> m_lights;
};

} // namespace OpenArtOfWar
