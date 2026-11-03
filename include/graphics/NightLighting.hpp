#pragma once

#include "game/EntityManager.hpp"
#include <SDL3/SDL.h>
#include <vector>

namespace OpenArtOfWar
{

class Camera;

// The night of a match on render APIs without shader effects (everything but SDL GPU): a light map the size of the
// view (a fraction of it), filled with the night's colour, with a soft spot added for every light (explosions,
// flames, rockets), multiplied onto the world picture. Only render targets and the ADD / MOD blend modes, which every
// SDL renderer has. The SDL GPU renderer draws the night with its colour grading shader instead.
class NightLighting
{
public:
    NightLighting() = default;
    ~NightLighting();
    NightLighting(const NightLighting &) = delete;
    NightLighting &operator=(const NightLighting &) = delete;

    // Darkens what is drawn on the current render target over w x h (the world's view, `camera` drew it)
    void Apply(
        SDL_Renderer *renderer, const Camera &camera, const std::vector<EntityManager::LightSpot> &lights, int w, int h);
    void Shutdown();
    // The list of this frame's lights, kept between frames (no allocation each frame)
    std::vector<EntityManager::LightSpot> &Lights()
    {
        return m_lights;
    }

private:
    bool EnsureTextures(SDL_Renderer *renderer, int w, int h);

    SDL_Renderer *m_renderer {nullptr}; // the textures belong to it
    static constexpr int kDivisor = 2; // the light map at half the view: it is smooth anyway
    SDL_Texture *m_light {nullptr};
    int m_lightW {0}, m_lightH {0};
    SDL_Texture *m_spot {nullptr}; // a white disc whose alpha falls off to the edge
    std::vector<EntityManager::LightSpot> m_lights;
};

} // namespace OpenArtOfWar
