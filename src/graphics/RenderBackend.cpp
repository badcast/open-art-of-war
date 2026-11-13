#include "graphics/RenderBackend.hpp"
#include "core/EnvironmentConfig.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <map>
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
    {"gpu", "SDL GPU"},
    {"vulkan", "Vulkan"},
    {"direct3d12", "Direct3D 12"},
    {"metal", "Metal"},
    {"opengl", "OpenGL"},
    {"direct3d11", "Direct3D 11"},
    {"opengles2", "OpenGL ES 2"},
    {"direct3d", "Direct3D 9"},
    {"software", "Software"},
};

std::string ProbeKey()
{
    const int v = SDL_GetVersion();
    const char *video = SDL_GetCurrentVideoDriver();
    return "sdl-" + std::to_string(SDL_VERSIONNUM_MAJOR(v)) + "." + std::to_string(SDL_VERSIONNUM_MINOR(v)) + "."
           + std::to_string(SDL_VERSIONNUM_MICRO(v)) + "|" + (video ? video : "none");
}

// The "render" section of the environment: only the choice (api, fallback) and the init_pending marker are stored.
// What was found out about an API (works / fails) is never written down: a failure kept on disk once hid a working
// API (SDL GPU) for good after an SDL update. Old stored results are dropped.
nlohmann::json &RenderSection(EnvironmentConfig &env)
{
    nlohmann::json &r = env.Edit("render");
    r.erase("probed");
    r.erase("probe_key");
    return r;
}

// What this run found out (in memory only)
std::map<std::string, bool, std::less<>> &Probed()
{
    static std::map<std::string, bool, std::less<>> probed;
    return probed;
}

void MarkProbed(std::string_view name, bool works)
{
    Probed()[std::string(name)] = works;
}

bool KnownToFail(std::string_view name)
{
    auto it = Probed().find(name);
    return it != Probed().end() && !it->second;
}

// Checks what the game needs from a renderer; logs the reason when it falls short
bool Usable(SDL_Renderer *renderer, std::string_view name)
{
    const char *actual = SDL_GetRendererName(renderer);
    if (!actual || name != actual)
    {
        std::cerr << "[Render] " << name << ": SDL gave " << (actual ? actual : "nothing") << " instead" << std::endl;
        return false;
    }
    const Sint64 maxSize
        = SDL_GetNumberProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0);
    if (maxSize > 0 && maxSize < kMinTextureSize)
    {
        std::cerr << "[Render] " << name << ": textures limited to " << maxSize << " px" << std::endl;
        return false;
    }
    // The reduced-resolution picture of low / medium quality is drawn into a target texture
    SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 64, 64);
    if (!target)
    {
        std::cerr << "[Render] " << name << ": no render targets: " << SDL_GetError() << std::endl;
        return false;
    }
    SDL_DestroyTexture(target);
    return true;
}

SDL_Renderer *TryCreate(SDL_Window *window, std::string_view name)
{
#ifdef _WIN32
    // The shader effects are built as SPIR-V (and Metal source), not DXIL: SDL GPU on Vulkan here
    if (name == "gpu")
        SDL_SetHint(SDL_HINT_GPU_DRIVER, "vulkan");
#endif
    const std::string nameStr(name);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, nameStr.c_str());
    if (!renderer)
    {
        std::cerr << "[Render] " << name << " failed: " << SDL_GetError() << std::endl;
        return nullptr;
    }
    if (!Usable(renderer, name))
    {
        SDL_DestroyRenderer(renderer);
        return nullptr;
    }
    return renderer;
}

} // namespace

std::vector<std::string> Candidates()
{
    std::vector<std::string> compiled;
    const int n = SDL_GetNumRenderDrivers();
    for (int i = 0; i < n; ++i)
    {
        if (const char *d = SDL_GetRenderDriver(i))
            compiled.emplace_back(d);
    }
    std::vector<std::string> out;
    for (const DriverName &d : kPreference)
    {
        if (std::find(compiled.begin(), compiled.end(), d.name) != compiled.end())
            out.emplace_back(d.name);
    }
    return out;
}

std::string Label(std::string_view name)
{
    for (const DriverName &d : kPreference)
    {
        if (name == d.name)
            return d.label;
    }
    return std::string(name);
}

std::string Configured(const EnvironmentConfig &env)
{
    return env.Section("render").value("api", std::string());
}

void Select(EnvironmentConfig &env, std::string_view name)
{
    nlohmann::json &r = RenderSection(env);
    const std::string previous = r.value("api", std::string());
    if (previous == name)
        return;
    r["api"] = std::string(name);
    // The one that ran so far is a good fallback for the new choice
    if (!previous.empty() && !KnownToFail(previous))
        r["fallback"] = previous;
    env.Save();
}

SDL_Renderer *Create(SDL_Window *window, EnvironmentConfig &env, std::string_view forced)
{
    // Without a real display (dummy / offscreen video: servers, CI, tests) nothing found out here says
    // anything about the player's system: work on a copy, store nothing
    const char *video = SDL_GetCurrentVideoDriver();
    if (video && (std::string_view(video) == "dummy" || std::string_view(video) == "offscreen"))
    {
        const std::string forcedStr(forced);
        SDL_Renderer *renderer = SDL_CreateRenderer(window, forced.empty() ? nullptr : forcedStr.c_str());
        if (!renderer)
            renderer = SDL_CreateRenderer(window, nullptr);
        if (renderer)
            std::cout << "[Render] API: " << Label(SDL_GetRendererName(renderer)) << " (no display)" << std::endl;
        return renderer;
    }
    nlohmann::json &r = RenderSection(env);

    // A start that died while bringing an API up leaves it in init_pending
    std::string skipOnce;
    const std::string crashed = r.value("init_pending", std::string());
    if (!crashed.empty())
    {
        std::cerr << "[Render] WARNING: the last start did not survive the " << Label(crashed)
                  << " initialization, it is skipped this time" << std::endl;
        skipOnce = crashed; // for this start only: nothing is remembered against the API
        r["init_pending"] = "";
    }

    const std::vector<std::string> all = Candidates();
    std::vector<std::string> order;
    auto add = [&](std::string_view name) {
        if (name.empty() || std::find(order.begin(), order.end(), name) != order.end())
            return;
        if (std::find(all.begin(), all.end(), name) == all.end())
        {
            std::cerr << "[Render] " << name << " is not available in this SDL build" << std::endl;
            return;
        }
        order.emplace_back(name);
    };
    add(forced);
    add(r.value("api", std::string()));
    add(r.value("fallback", std::string()));
    for (const std::string &name : all)
        add(name);

    const std::string wanted = order.empty() ? std::string() : order.front();
    SDL_Renderer *renderer = nullptr;
    std::string chosen;
    for (const std::string &name : order)
    {
        // A forced API is tried even when it failed before (that is what forcing is for)
        if (name != forced && (name == skipOnce || KnownToFail(name)))
            continue;
        r["init_pending"] = name;
        env.Save(); // on disk before the driver gets a chance to take the process down
        renderer = TryCreate(window, name);
        MarkProbed(name, renderer != nullptr);
        if (renderer)
        {
            chosen = name;
            break;
        }
    }
    if (!renderer)
    {
        // Last resort: whatever SDL picks by itself
        renderer = SDL_CreateRenderer(window, nullptr);
        if (renderer)
            chosen = SDL_GetRendererName(renderer) ? SDL_GetRendererName(renderer) : "";
    }
    if (!renderer)
    {
        r["init_pending"] = "";
        env.Save();
        return nullptr;
    }

    if (chosen != wanted)
    {
        std::cerr << "[Render] WARNING: " << Label(wanted) << " could not be used, running on " << Label(chosen)
                  << std::endl;
    }
    std::cout << "[Render] API: " << Label(chosen) << std::endl;

    // A forced API is a one-off: the stored choice is left alone
    if (forced.empty())
    {
        r["api"] = chosen;
        // Fallback: the next API after the chosen one that is not known to fail
        std::string fallback;
        for (auto it = std::find(order.begin(), order.end(), chosen); it != order.end(); ++it)
        {
            if (*it != chosen && !KnownToFail(*it))
            {
                fallback = *it;
                break;
            }
        }
        if (fallback.empty() && chosen != "software")
            fallback = "software";
        r["fallback"] = fallback;
    }
    env.Save(); // init_pending stays set until confirmFirstFrame
    return renderer;
}

void ConfirmFirstFrame(EnvironmentConfig &env)
{
    if (env.Section("render").value("init_pending", std::string()).empty())
        return;
    env.Edit("render")["init_pending"] = "";
    env.Save();
}

bool Probe(std::string_view name)
{
    SDL_Window *window = SDL_CreateWindow("probe", 64, 64, SDL_WINDOW_HIDDEN);
    if (!window)
        return false;
    SDL_Renderer *renderer = TryCreate(window, name);
    const bool ok = renderer != nullptr;
    if (renderer)
        SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    return ok;
}

std::vector<RenderApiOption> Options(EnvironmentConfig &env, bool probeUnknown)
{
    const std::string current = RenderSection(env).value("api", std::string());
    std::vector<RenderApiOption> out;
    for (const std::string &name : Candidates())
    {
        RenderApiOption o;
        o.name = name;
        o.label = Label(name);
        if (probeUnknown && name != current && Probed().find(name) == Probed().end())
            MarkProbed(name, Probe(name));
        auto it = Probed().find(name);
        if (name == current && it == Probed().end())
            o.status = RenderApiOption::Status::Works; // it is the one that runs
        else if (it != Probed().end())
            o.status = it->second ? RenderApiOption::Status::Works : RenderApiOption::Status::Fails;
        out.push_back(std::move(o));
    }
    return out;
}

} // namespace OpenArtOfWar::RenderBackend
