#pragma once

#include "core/Settings.hpp"
#include <vector>

struct SDL_Window;
struct SDL_Renderer;

namespace OpenArtOfWar
{

    struct Resolution
    {
        int w {0};
        int h {0};
        bool operator==(const Resolution &o) const
        {
            return w == o.w && h == o.h;
        }
    };

    namespace VideoModes
    {

        // Window sizes and display modes to pick from: the modes of the monitor the window is on plus common window
        // sizes, from kMinWindowW x kMinWindowH up to the desktop size, ascending
        std::vector<Resolution> availableResolutions(SDL_Window *window);

        // Resolution the window shows the game at right now (pixels)
        Resolution currentResolution(SDL_Window *window);

        // Puts the window into settings.windowMode / size and sets vsync on the renderer
        void apply(SDL_Window *window, SDL_Renderer *renderer, const Settings &settings);

    } // namespace VideoModes

} // namespace OpenArtOfWar
