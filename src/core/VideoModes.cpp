#include "core/VideoModes.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace OpenArtOfWar::VideoModes
{

    namespace
    {

        SDL_DisplayID displayOf(SDL_Window *window)
        {
            SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : 0;
            return display ? display : SDL_GetPrimaryDisplay();
        }

    } // namespace

    std::vector<Resolution> availableResolutions(SDL_Window *window)
    {
        const SDL_DisplayID display = displayOf(window);
        std::vector<Resolution> list;
        int desktopW = 0, desktopH = 0;
        if(const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display))
        {
            desktopW = desktop->w;
            desktopH = desktop->h;
        }

        int count = 0;
        if(SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &count))
        {
            for(int i = 0; i < count; ++i)
                list.push_back({modes[i]->w, modes[i]->h});
            SDL_free(modes);
        }
        static const Resolution kCommon[] = {{1024, 576}, {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}};
        for(const Resolution &r : kCommon)
        {
            if(desktopW == 0 || (r.w <= desktopW && r.h <= desktopH))
                list.push_back(r);
        }
        if(desktopW > 0)
            list.push_back({desktopW, desktopH});

        list.erase(std::remove_if(list.begin(), list.end(), [](const Resolution &r) { return r.w < Settings::kMinWindowW || r.h < Settings::kMinWindowH; }), list.end());
        std::sort(list.begin(), list.end(), [](const Resolution &a, const Resolution &b) { return a.w != b.w ? a.w < b.w : a.h < b.h; });
        list.erase(std::unique(list.begin(), list.end()), list.end());
        if(list.empty())
            list.push_back({1280, 720});
        return list;
    }

    Resolution currentResolution(SDL_Window *window)
    {
        Resolution r;
        if(window)
            SDL_GetWindowSizeInPixels(window, &r.w, &r.h);
        return r;
    }

    void apply(SDL_Window *window, SDL_Renderer *renderer, const Settings &settings)
    {
        if(!window)
            return;
        const SDL_DisplayID display = displayOf(window);

        switch(settings.windowMode)
        {
            case WindowMode::Windowed:
                SDL_SetWindowFullscreen(window, false);
                SDL_SetWindowBordered(window, true);
                SDL_SetWindowSize(window, settings.windowW, settings.windowH);
                SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
                break;
            case WindowMode::Borderless:
                SDL_SetWindowFullscreenMode(window, nullptr); // desktop resolution
                SDL_SetWindowFullscreen(window, true);
                break;
            case WindowMode::Exclusive:
            {
                // The monitor's mode nearest to the chosen size, the highest refresh rate among equals
                SDL_DisplayMode best {};
                bool found = false;
                int count = 0;
                if(SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &count))
                {
                    long bestScore = 0;
                    for(int i = 0; i < count; ++i)
                    {
                        const long dw = modes[i]->w - settings.fullscreenW, dh = modes[i]->h - settings.fullscreenH;
                        const long score = dw * dw + dh * dh;
                        if(!found || score < bestScore || (score == bestScore && modes[i]->refresh_rate > best.refresh_rate))
                        {
                            best = *modes[i];
                            bestScore = score;
                            found = true;
                        }
                    }
                    SDL_free(modes);
                }
                SDL_SetWindowFullscreenMode(window, found ? &best : nullptr);
                SDL_SetWindowFullscreen(window, true);
                break;
            }
        }
        if(renderer)
            SDL_SetRenderVSync(renderer, settings.vsync ? 1 : 0);
        SDL_SyncWindow(window);
    }

} // namespace OpenArtOfWar::VideoModes
