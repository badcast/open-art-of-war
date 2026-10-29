#include "graphics/RenderBackend.hpp"
#include "core/EnvironmentConfig.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <iostream>

namespace OpenArtOfWar::RenderBackend
{

    namespace
    {

        // Smallest texture size the game can live with: the 1200 px title picture and the reduced-resolution
        // world target of a full HD window
        constexpr int kMinTextureSize = 2048;

        struct DriverName
        {
            const char *name;
            const char *label;
        };

        // Order of preference; drivers missing from the SDL build or the platform are simply not listed.
        // SDL GPU comes first: it runs on Vulkan / Direct3D 12 / Metal itself and is the one API with the
        // shader effects (ShaderEffects).
        const DriverName kPreference[] = {
            {"gpu", "SDL GPU (shader effects)"},
            {"vulkan", "Vulkan"},
            {"direct3d12", "Direct3D 12"},
            {"metal", "Metal"},
            {"opengl", "OpenGL"},
            {"direct3d11", "Direct3D 11"},
            {"opengles2", "OpenGL ES 2"},
            {"direct3d", "Direct3D 9"},
            {"software", "Software"},
        };

        std::string probeKey()
        {
            const int v = SDL_GetVersion();
            const char *video = SDL_GetCurrentVideoDriver();
            return "sdl-" + std::to_string(SDL_VERSIONNUM_MAJOR(v)) + "." + std::to_string(SDL_VERSIONNUM_MINOR(v)) + "." + std::to_string(SDL_VERSIONNUM_MICRO(v)) + "|" + (video ? video : "none");
        }

        // The "render" section, reset when it was probed with another SDL / video driver
        nlohmann::json &renderSection(EnvironmentConfig &env)
        {
            nlohmann::json &r = env.edit("render");
            const std::string key = probeKey();
            if(r.value("probe_key", std::string()) != key)
            {
                r["probe_key"] = key;
                r["probed"] = nlohmann::json::object();
            }
            if(!r.contains("probed") || !r["probed"].is_object())
                r["probed"] = nlohmann::json::object();
            return r;
        }

        void markProbed(nlohmann::json &r, const std::string &name, bool works)
        {
            r["probed"][name] = works;
        }

        bool knownToFail(const nlohmann::json &r, const std::string &name)
        {
            auto it = r["probed"].find(name);
            return it != r["probed"].end() && it->is_boolean() && !it->get<bool>();
        }

        // Checks what the game needs from a renderer; logs the reason when it falls short
        bool usable(SDL_Renderer *renderer, const std::string &name)
        {
            const char *actual = SDL_GetRendererName(renderer);
            if(!actual || name != actual)
            {
                std::cerr << "[Render] " << name << ": SDL gave " << (actual ? actual : "nothing") << " instead" << std::endl;
                return false;
            }
            const Sint64 maxSize = SDL_GetNumberProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0);
            if(maxSize > 0 && maxSize < kMinTextureSize)
            {
                std::cerr << "[Render] " << name << ": textures limited to " << maxSize << " px" << std::endl;
                return false;
            }
            // The reduced-resolution picture of low / medium quality is drawn into a target texture
            SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 64, 64);
            if(!target)
            {
                std::cerr << "[Render] " << name << ": no render targets: " << SDL_GetError() << std::endl;
                return false;
            }
            SDL_DestroyTexture(target);
            return true;
        }

        SDL_Renderer *tryCreate(SDL_Window *window, const std::string &name)
        {
#ifdef _WIN32
            // The shader effects are built as SPIR-V (and Metal source), not DXIL: SDL GPU on Vulkan here
            if(name == "gpu")
                SDL_SetHint(SDL_HINT_GPU_DRIVER, "vulkan");
#endif
            SDL_Renderer *renderer = SDL_CreateRenderer(window, name.c_str());
            if(!renderer)
            {
                std::cerr << "[Render] " << name << " failed: " << SDL_GetError() << std::endl;
                return nullptr;
            }
            if(!usable(renderer, name))
            {
                SDL_DestroyRenderer(renderer);
                return nullptr;
            }
            return renderer;
        }

    } // namespace

    std::vector<std::string> candidates()
    {
        std::vector<std::string> compiled;
        const int n = SDL_GetNumRenderDrivers();
        for(int i = 0; i < n; ++i)
        {
            if(const char *d = SDL_GetRenderDriver(i))
                compiled.emplace_back(d);
        }
        std::vector<std::string> out;
        for(const DriverName &d : kPreference)
        {
            if(std::find(compiled.begin(), compiled.end(), d.name) != compiled.end())
                out.emplace_back(d.name);
        }
        return out;
    }

    std::string label(const std::string &name)
    {
        for(const DriverName &d : kPreference)
        {
            if(name == d.name)
                return d.label;
        }
        return name;
    }

    std::string configured(const EnvironmentConfig &env)
    {
        return env.section("render").value("api", std::string());
    }

    void select(EnvironmentConfig &env, const std::string &name)
    {
        nlohmann::json &r = renderSection(env);
        const std::string previous = r.value("api", std::string());
        if(previous == name)
            return;
        r["api"] = name;
        // The one that ran so far is a good fallback for the new choice
        if(!previous.empty() && !knownToFail(r, previous))
            r["fallback"] = previous;
        env.save();
    }

    SDL_Renderer *create(SDL_Window *window, EnvironmentConfig &env, const std::string &forced)
    {
        // Without a real display (dummy / offscreen video: servers, CI, tests) nothing found out here says
        // anything about the player's system: work on a copy, store nothing
        const char *video = SDL_GetCurrentVideoDriver();
        if(video && (std::string(video) == "dummy" || std::string(video) == "offscreen"))
        {
            SDL_Renderer *renderer = SDL_CreateRenderer(window, forced.empty() ? nullptr : forced.c_str());
            if(!renderer)
                renderer = SDL_CreateRenderer(window, nullptr);
            if(renderer)
                std::cout << "[Render] API: " << label(SDL_GetRendererName(renderer)) << " (no display)" << std::endl;
            return renderer;
        }
        nlohmann::json &r = renderSection(env);

        // A start that died while bringing an API up leaves it in init_pending
        const std::string crashed = r.value("init_pending", std::string());
        if(!crashed.empty())
        {
            std::cerr << "[Render] WARNING: the last start did not survive the " << label(crashed) << " initialization, it is skipped from now on" << std::endl;
            markProbed(r, crashed, false);
            r["init_pending"] = "";
        }

        const std::vector<std::string> all = candidates();
        std::vector<std::string> order;
        auto add = [&](const std::string &name)
        {
            if(name.empty() || std::find(order.begin(), order.end(), name) != order.end())
                return;
            if(std::find(all.begin(), all.end(), name) == all.end())
            {
                std::cerr << "[Render] " << name << " is not available in this SDL build" << std::endl;
                return;
            }
            order.push_back(name);
        };
        add(forced);
        add(r.value("api", std::string()));
        add(r.value("fallback", std::string()));
        for(const std::string &name : all)
            add(name);

        const std::string wanted = order.empty() ? std::string() : order.front();
        SDL_Renderer *renderer = nullptr;
        std::string chosen;
        for(const std::string &name : order)
        {
            // A forced API is tried even when it failed before (that is what forcing is for)
            if(name != forced && knownToFail(r, name))
                continue;
            r["init_pending"] = name;
            env.save(); // on disk before the driver gets a chance to take the process down
            renderer = tryCreate(window, name);
            markProbed(r, name, renderer != nullptr);
            if(renderer)
            {
                chosen = name;
                break;
            }
        }
        if(!renderer)
        {
            // Last resort: whatever SDL picks by itself
            renderer = SDL_CreateRenderer(window, nullptr);
            if(renderer)
                chosen = SDL_GetRendererName(renderer) ? SDL_GetRendererName(renderer) : "";
        }
        if(!renderer)
        {
            r["init_pending"] = "";
            env.save();
            return nullptr;
        }

        if(chosen != wanted)
        {
            std::cerr << "[Render] WARNING: " << label(wanted) << " could not be used, running on " << label(chosen) << std::endl;
        }
        std::cout << "[Render] API: " << label(chosen) << std::endl;

        // A forced API is a one-off: the stored choice is left alone
        if(forced.empty())
        {
            r["api"] = chosen;
            // Fallback: the next API after the chosen one that is not known to fail
            std::string fallback;
            for(auto it = std::find(order.begin(), order.end(), chosen); it != order.end(); ++it)
            {
                if(*it != chosen && !knownToFail(r, *it))
                {
                    fallback = *it;
                    break;
                }
            }
            if(fallback.empty() && chosen != "software")
                fallback = "software";
            r["fallback"] = fallback;
        }
        env.save(); // init_pending stays set until confirmFirstFrame
        return renderer;
    }

    void confirmFirstFrame(EnvironmentConfig &env)
    {
        if(env.section("render").value("init_pending", std::string()).empty())
            return;
        env.edit("render")["init_pending"] = "";
        env.save();
    }

    bool probe(const std::string &name)
    {
        SDL_Window *window = SDL_CreateWindow("probe", 64, 64, SDL_WINDOW_HIDDEN);
        if(!window)
            return false;
        SDL_Renderer *renderer = tryCreate(window, name);
        const bool ok = renderer != nullptr;
        if(renderer)
            SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return ok;
    }

    std::vector<RenderApiOption> options(EnvironmentConfig &env, bool probeUnknown)
    {
        nlohmann::json &r = renderSection(env);
        const std::string current = r.value("api", std::string());
        std::vector<RenderApiOption> out;
        bool probedNow = false;
        for(const std::string &name : candidates())
        {
            RenderApiOption o;
            o.name = name;
            o.label = label(name);
            auto it = r["probed"].find(name);
            if(it == r["probed"].end() && probeUnknown && name != current)
            {
                markProbed(r, name, probe(name));
                it = r["probed"].find(name);
                probedNow = true;
            }
            if(it != r["probed"].end() && it->is_boolean())
            {
                o.status = it->get<bool>() ? RenderApiOption::Status::Works : RenderApiOption::Status::Fails;
            }
            out.push_back(std::move(o));
        }
        if(probedNow)
            env.save();
        return out;
    }

} // namespace OpenArtOfWar::RenderBackend
