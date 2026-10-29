#pragma once

#include <SDL3/SDL.h>
#include <random>
#include <vector>

namespace OpenArtOfWar
{

    class Renderer;
    class TextureManager;

    // Slow, soft explosions on the dark left side of the main menu: every blast fades in and out, its frames
    // are cross-faded so the animation flows instead of stepping. Purely decorative: it has its own random
    // numbers and never touches the game's.
    class MenuBackdropFx
    {
    public:
        // `areaW`: width of the dark area the blasts stay in (they are clipped to it)
        void render(Renderer &renderer, TextureManager &textures, int areaW, int windowW, int windowH, Uint32 ticks);

    private:
        struct Blast
        {
            float x {0.0f}, y {0.0f};
            float zoom {4.0f};
            int anim {11};
            float age {0.0f};
            float life {3.0f};
            float peak {0.4f};
        };
        void spawn(TextureManager &textures, int areaW, int windowH);

        std::vector<Blast> m_blasts;
        std::mt19937 m_rng {0x0AA5u};
        Uint32 m_lastTicks {0};
        float m_nextSpawn {0.0f};
    };

} // namespace OpenArtOfWar
