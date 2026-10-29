#pragma once

#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;

namespace OpenArtOfWar
{

    class EnvironmentConfig;

    // A render API the player can pick in the graphics settings
    struct RenderApiOption
    {
        enum class Status
        {
            Unknown,
            Works,
            Fails
        };
        std::string name;  // SDL render driver name: "vulkan", "direct3d12", "metal", "opengl", ...
        std::string label; // "Vulkan", "Direct3D 12", ...
        Status status {Status::Unknown};
    };

    // Choice and creation of the SDL render backend. Order of preference:
    // SDL GPU (Vulkan / D3D12 / Metal underneath, with shader effects) -> Vulkan -> Direct3D 12 / Metal ->
    // OpenGL / Direct3D 11 -> other SDL drivers -> software.
    // The choice lives in the "render" section of the environment (defaults: assets/data/game_environment.json,
    // what this system found out: settings.json, key "environment"):
    //   api           the API to start with
    //   fallback      the next one known to work
    //   probed        { name: true/false } what was tried on this system
    //   probe_key     SDL version + video driver the probes belong to (a change starts over)
    //   init_pending  set while an API is being brought up, cleared after its first frame: still set on
    //                 the next start means that attempt took the whole process down, so the API is skipped
    namespace RenderBackend
    {

        // Drivers compiled into SDL, in the order of preference of this platform
        std::vector<std::string> candidates();
        std::string label(const std::string &name);

        // Creates the renderer for `window`: `forced` (command line, not stored) or the configured API first,
        // then the fallback and the rest in order of preference, software last. Failed attempts are logged,
        // marked in `env` and skipped; `env` gets the API that came up and the next working one. Saves `env`.
        SDL_Renderer *create(SDL_Window *window, EnvironmentConfig &env, const std::string &forced = "");

        // The first frame of the new renderer is on the screen: the API is safe, clear init_pending
        void confirmFirstFrame(EnvironmentConfig &env);

        // Brings `name` up on a hidden throw-away window and tears it down again
        bool probe(const std::string &name);

        // The options for the settings screen: every candidate with what is known about it.
        // `probeUnknown` tries the not yet probed ones now (and stores the results in `env`).
        std::vector<RenderApiOption> options(EnvironmentConfig &env, bool probeUnknown);

        // The API to use from the next start (or the next renderer recreation) on
        void select(EnvironmentConfig &env, const std::string &name);
        std::string configured(const EnvironmentConfig &env);

    } // namespace RenderBackend
} // namespace OpenArtOfWar
