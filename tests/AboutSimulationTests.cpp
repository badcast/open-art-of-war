#include "audio/SoundEngine.hpp"
#include "audio/GameSounds.hpp"
#include "net/WorldSnapshot.hpp"
#include "graphics/Camera.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/Utility.hpp"
#include "core/EnvironmentConfig.hpp"
#include "core/Settings.hpp"
#include "game/Players.hpp"
#include "game/EntityManager.hpp"
#include "game/Research.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/TextureManager.hpp"
#include "graphics/SpriteRegistry.hpp"
#include "ui/AboutSimulation.hpp"
#include "ui/MenuController.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace OpenArtOfWar;

int main()
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) || !Assets::Init() || !LoaderResources::Init())
        return 1;
    SDL_Window *window = SDL_CreateWindow("about test", 960, 720, SDL_WINDOW_HIDDEN);
    SDL_Renderer *sdl = window ? SDL_CreateRenderer(window, "software") : nullptr;
    if (!sdl)
        return 1;
    bool ok = true;
    {
        TextureManager textures(sdl);
        textures.LoadAllAssets();
        Renderer renderer(sdl, &textures);
        {
            Settings settings;
            settings.ApplyFactoryDefaults(EnvironmentConfig::FactorySettings());
            ok = settings.showShadows && ok;
            const auto settingsPath = std::filesystem::temp_directory_path() / "oaow_shadow_settings_test.json";
            settings.showShadows = false;
            ok = settings.Save(settingsPath.string()) && ok;
            Settings loaded;
            ok = loaded.Load(settingsPath.string()) && !loaded.showShadows && ok;
            std::filesystem::remove(settingsPath);

            const SpriteRef shadow = Sprites().Resolve("shared_shadows.bunker_res_s2174", SpriteKind::Frame);
            Camera camera(960, 720);
            camera.SetPosition(0, 0);
            auto changedPixels = [&](bool show) {
                renderer.SetShowShadows(show);
                renderer.Clear({255, 255, 255, 255});
                renderer.DrawRawSpriteWorld(shadow.handle, 100, 100, camera, Faction::Resistance);
                SDL_Surface *pixels = SDL_RenderReadPixels(sdl, nullptr);
                if (!pixels)
                    return -1;
                int changed = 0;
                const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(pixels->format);
                for (int y = 180; y < 260; ++y)
                    for (int x = 180; x < 260; ++x)
                    {
                        const auto *pixel = reinterpret_cast<const Uint32 *>(
                                                static_cast<const char *>(pixels->pixels) + y * pixels->pitch)
                                            + x;
                        Uint8 r, g, b, a;
                        SDL_GetRGBA(*pixel, format, nullptr, &r, &g, &b, &a);
                        if (r != 255 || g != 255 || b != 255)
                            ++changed;
                    }
                SDL_DestroySurface(pixels);
                return changed;
            };
            ok = shadow.Valid() && changedPixels(true) > 0 && changedPixels(false) == 0 && ok;
            for (const char *uiSprite :
                 {"dialog_items.panel_fill",
                  "dialog_items.top_rail",
                  "dialog_items.bottom_rail",
                  "dialog_items.side_rail",
                  "dialog_items.corner_cap",
                  "dialog_items.tl_rail_v",
                  "dialog_items.tl_rail_h",
                  "dialog_items.tl_corner",
                  "dialog_items.br_rail_v",
                  "dialog_items.br_rail_h",
                  "dialog_items.br_corner",
                  "dialog_items.tab_fill_active",
                  "dialog_items.tab_fill_idle",
                  "dialog_items.tab_shoulder_l",
                  "dialog_items.tab_shoulder_r"})
            {
                ok = Sprites().Resolve(uiSprite, SpriteKind::Frame).Valid() && ok;
            }
            for (const auto &err : Sprites().Errors())
            {
                if (err.find("dialog_items") != std::string::npos || err.find("misc") != std::string::npos)
                    ok = false;
            }
            renderer.SetShowShadows(true);
        }
        BitmapFontRenderer font(sdl);
        SoundEngine sound;
        ok = sound.Init() && ok;
        const auto advance = [](AboutSimulation &scene, int ticks) {
            for (int i = 0; i < ticks; ++i)
                scene.Update(0.05f);
        };
        {
            MenuController menu;
            const auto lines = menu.AboutLines();
            const auto libraries = std::find(lines.begin(), lines.end(), "LIBRARIES");
            const auto brainmap = std::find_if(lines.begin(), lines.end(), [](const std::string &line) {
                return line.find("library-brainmap - ") == 0;
            });
            const auto music = std::find_if(lines.begin(), lines.end(), [](const std::string &line) {
                return line.find("MUSIC") != std::string::npos;
            });
            ok = libraries != lines.end() && brainmap != lines.end() && music != lines.end() && libraries < brainmap
                 && brainmap < music && ok;
        }
        std::array<PlayerSlot, kMaxPlayers> saved {};
        saved[0] = {true, Faction::Confederation, 0, 3, true};
        Players::Setup(saved, 0);
        ResetResearch();
        EntityManager previousWorld;
        CompleteResearch(0, 0, previousWorld);
        const ResearchState savedResearch = CaptureResearchState();
        Utility::Rng().Reseed(123456u);
        const auto savedRng = Utility::Rng().State();
        const auto configPath = std::filesystem::temp_directory_path() / "oaow_about_test.yaml";
        {
            std::ofstream config(configPath);
            config << "map: assets/maps/aboutmap.json\n"
                      "audio_name: Machinimasound - Nightshift\n"
                      "volume: {music: 0.6, sfx: 0.35}\n"
                      "fade_seconds: 0.5\n"
                      "all_technologies: true\n"
                      "production_speed_multiplier: 10\n"
                      "max_units: 1200\n"
                      "camera: {start: {x: 37, y: 37, zoom: 1}, min_zoom: 0.1, max_zoom: 1.25}\n"
                      "shots:\n"
                      "  - {duration: 3, to: {x: 37, y: 37, zoom: 0}}\n"
                      "  - {duration: 3, from: {x: 40, y: 30, zoom: 2}, to: {x: 37, y: 37, zoom: 0}}\n";
        }
        {
            AboutSimulation scene;
            ok = scene.Start(textures, 960, 720, configPath.string()) && ok;
            std::string mapText;
            if (!Assets::ReadText("assets/maps/aboutmap.json", mapText))
                return 1;
            const auto map = nlohmann::json::parse(mapText);
            std::array<int, 2> expectedUnits {};
            for (const auto &unit : map.at("units"))
            {
                const std::string faction = unit.value("faction", "resistance");
                if (faction == "resistance")
                    ++expectedUnits[0];
                if (faction == "confederation")
                    ++expectedUnits[1];
            }
            if (scene.InitialUnits(0) != expectedUnits[0] || scene.InitialUnits(1) != expectedUnits[1])
                std::printf(
                    "Initial units: %d/%d, expected %d/%d\n",
                    scene.InitialUnits(0),
                    scene.InitialUnits(1),
                    expectedUnits[0],
                    expectedUnits[1]);
            ok = scene.InitialUnits(0) == expectedUnits[0] && scene.InitialUnits(1) == expectedUnits[1] && ok;
            ok = scene.MusicName() == "Machinimasound - Nightshift" && ok;
            ok = Players::Slot(0).faction == Faction::Confederation && Players::Slot(0).color == 3
                 && !Players::Slot(1).active && CaptureResearchState() == savedResearch && ok;
            ok = scene.CameraZoom() == 1.0f && scene.AllResearchCompleted() && ok;
            ok = scene.MusicVolume() == 0.6f && scene.SfxVolume() == 0.35f && ok;
            ok = sound.PlayNamedLoop(scene.MusicName()) && ok;
            sound.Update(true, false, true);
            ok = sound.GetCurrentTrackName() == scene.MusicName() && ok;
            if (ok)
            {
                scene.Update(0.05f);
                scene.Update(0.05f);
                renderer.Clear({0, 0, 0, 255});
                scene.Render(renderer, font, {"About Game", "Credits"}, 960, 720);
                SDL_Surface *image = SDL_RenderReadPixels(sdl, nullptr);
                if (!image)
                    ok = false;
                if (image)
                {
                    if (const char *path = SDL_getenv("OAOW_ABOUT_CAPTURE"))
                        SDL_SaveBMP(image, path);
                    int colored = 0;
                    const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(image->format);
                    for (int y = 50; y < 700; y += 30)
                        for (int x = 50; x < 930; x += 30)
                        {
                            const auto *pixel = reinterpret_cast<const Uint32 *>(
                                                    static_cast<const char *>(image->pixels) + y * image->pitch)
                                                + x;
                            Uint8 r, g, b, a;
                            SDL_GetRGBA(*pixel, format, nullptr, &r, &g, &b, &a);
                            if (r || g || b)
                                ++colored;
                        }
                    ok = colored > 20 && ok;
                    SDL_DestroySurface(image);
                }
                const unsigned initialWorld = scene.WorldGeneration();
                advance(scene, 60);
                ok = scene.ProducedUnits(0) > 0 && scene.ProducedUnits(1) > 0 && scene.DistinctProducedTypes() > 1
                     && ok;
                const float beforeFade = scene.SimulationSeconds();
                advance(scene, 20);
                ok = scene.SimulationSeconds() > beforeFade && ok;
                ok = scene.CameraZoom() <= 1.25f && ok;
                advance(scene, 2);
                ok = scene.SimulationSeconds() > beforeFade && ok;
                advance(scene, 80);
                std::printf("Moved initial units: %d, %d\n", scene.MovedInitialUnits(0), scene.MovedInitialUnits(1));
                ok = scene.MovedInitialUnits(0) > 5 && scene.MovedInitialUnits(1) > 5 && ok;
                advance(scene, 240);
                std::printf(
                    "Damaged initial units: %d, %d\n", scene.DamagedInitialUnits(0), scene.DamagedInitialUnits(1));
                ok = scene.DamagedInitialUnits(0) > 0 && scene.DamagedInitialUnits(1) > 0 && ok;
                ok = scene.WorldGeneration() == initialWorld && ok;
                ok = Players::Slot(0).faction == Faction::Confederation && CaptureResearchState() == savedResearch
                     && Utility::Rng().State() == savedRng && ok;
            }
        }
        {
            std::ofstream config(configPath);
            config << "map: assets/maps/aboutmap.json\nmax_units: 1\nproduction_speed_multiplier: 100\n";
        }
        {
            AboutSimulation capped;
            ok = capped.Start(textures, 960, 720, configPath.string()) && ok;
            capped.Update(0.25f);
            ok = capped.SimulationSeconds() <= 0.051f && ok;
            advance(capped, 59);
            ok = capped.InitialUnits(0) > 1 && capped.InitialUnits(1) > 1 && capped.ProducedUnits(0) == 0
                 && capped.ProducedUnits(1) == 0 && ok;
        }
        // Battle cue playback is separate from voices and announcer lines.
        {
            EntityManager effects;
            WorldSnapshot snapshot;
            snapshot.cues
                = {{static_cast<std::uint8_t>(EntityManager::SoundCue::FireRifle), 200.0f, 200.0f, 0, "", 1.0f}};
            ok = effects.ApplySnapshot(snapshot) && ok;
            Camera camera(960, 720);
            camera.CenterOn(200.0f, 200.0f);
            sound.Sfx().StopAll();
            GameSounds::PlayBattleCues(sound, effects, camera, 0.8f);
            ok = sound.Sfx().ActiveVoices() > 0 && sound.Sfx().AnnouncerQueued() == 0 && ok;
        }
        std::filesystem::remove(configPath);
        ok = Players::Slot(0).faction == Faction::Confederation && Players::Slot(0).color == 3
             && !Players::Slot(1).active && Utility::Rng().State() == savedRng
             && CaptureResearchState() == savedResearch && ok;
        sound.Update(true, false);
        ok = sound.GetCurrentTrackName() != "Machinimasound - Nightshift" && ok;
    }
    SDL_DestroyRenderer(sdl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf("About simulation test %s\n", ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}
