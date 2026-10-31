#include "core/Application.hpp"
#include "core/Localization.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/DebugEnv.hpp"
#include "ui/MenuController.hpp"
#include "ui/HUDController.hpp"
#include "ui/UiKit.hpp"
#include "game/GameplayController.hpp"
#include "game/Sides.hpp"
#include "game/Players.hpp"
#if defined(AOW_DEBUG)
#include "test/HeadlessTest.hpp"
#endif
#include "graphics/TextureManager.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/ShaderEffects.hpp"
#include "net/NetMatch.hpp"
#include "ui/NetworkMenu.hpp"
#include "openaow/net/Discovery.hpp"
#include "openaow/net/MasterClient.hpp"
#include "game/Match.hpp"
#include "game/Simulation.hpp"
#include "graphics/Camera.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "input/InputManager.hpp"
#include "audio/SoundEngine.hpp"
#include "audio/GameSounds.hpp"
#include "audio/SoundRequests.hpp"
#include "game/TileMap.hpp"
#include "game/Pathfinder.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/AIController.hpp"
#include "game/MissionController.hpp"
#include "storage/SaveManager.hpp"
#include "debug/DebugOverlay.hpp"

#include <SDL3/SDL.h>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cmath>

namespace OpenArtOfWar
{

    Application::Application()
    {
    }

    Application::~Application()
    {
        shutdown();
    }

    bool Application::init(int windowWidth, int windowHeight)
    {
        m_windowW = windowWidth;
        m_windowH = windowHeight;

        if(!Assets::init())
        {
            constexpr const char *errorMessage = "Required game data was not found. Place the 'assets' directory or 'data.blob' in the game folder.";
            std::cerr << "[Application] " << errorMessage << std::endl;
            if(!m_headless && SDL_Init(SDL_INIT_VIDEO))
            {
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game data not found", errorMessage, nullptr);
                SDL_Quit();
            }
            return false;
        }
        std::cout << "[Application] Assets: " << Assets::describe() << std::endl;
        // Every resource path comes from assets/main.json from here on
        LoaderResources::init();

        // Options and progress live in the per-user directory (Assets::userDataDir)
        m_saveManager = std::make_unique<SaveManager>();
        m_settings.windowW = windowWidth;
        m_settings.windowH = windowHeight;
        if(!m_settings.load(Settings::defaultPath()))
        {
            // First run with this version: take over what the old save file knew
            SaveData old;
            if(m_saveManager->load(old))
            {
                m_settings.language = old.language;
                m_settings.difficulty = old.difficulty;
                m_settings.muted = !old.soundEnabled;
            }
            m_settings.clamp();
        }
        m_confirmedVideo = m_settings;

        if(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS))
        {
            std::cerr << "[Application] SDL_Init failed: " << SDL_GetError() << std::endl;
            return false;
        }

        m_env.load(LoaderResources::environment());

        // Hidden until a renderer is up: trying one API after the other must not flicker on the screen
        m_window = SDL_CreateWindow("Open Art Of War: Freedom for Liberty", m_settings.windowW, m_settings.windowH, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
        if(!m_window)
        {
            std::cerr << "[Application] SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
            return false;
        }

        m_sdlRenderer = m_headless ? SDL_CreateRenderer(m_window, nullptr) : RenderBackend::create(m_window, m_env, m_rendererOverride);
        if(!m_sdlRenderer)
        {
            std::cerr << "[Application] SDL_CreateRenderer failed: " << SDL_GetError() << std::endl;
            return false;
        }
        m_renderFirstFrame = !m_headless;
        SDL_ShowWindow(m_window);
        VideoModes::apply(m_window, m_sdlRenderer, m_settings); // fullscreen mode, vsync
        {
            const Resolution size = VideoModes::currentResolution(m_window);
            if(size.w > 0 && size.h > 0)
            {
                m_windowW = size.w;
                m_windowH = size.h;
            }
        }
        m_resolutions = VideoModes::availableResolutions(m_window);

        // The language and the font first: the loading screen is up before the heavy part of the start-up
        Localization::init(LoaderResources::localeDir(), LoaderResources::defaultLanguage());
        Localization::setLanguage(m_settings.language);
        {
            std::string uiText;
            if(Assets::readText(LoaderResources::uiTheme(), uiText))
                ui::loadTheme(uiText);
        }
        m_texManager = std::make_unique<TextureManager>(m_sdlRenderer);
        m_renderer = std::make_unique<Renderer>(m_sdlRenderer, m_texManager.get());
        {
            // Font and sizes: section "font" of game_environment.json (a path outside assets/ works too)
            const nlohmann::json &f = m_env.section("font");
            FontSizes sizes;
            sizes.small = f.value("size_small", sizes.small);
            sizes.medium = f.value("size_medium", sizes.medium);
            sizes.large = f.value("size_large", sizes.large);
            sizes.title = f.value("size_title", sizes.title);
            sizes.hero = f.value("size_hero", sizes.hero);
            m_fontRenderer = std::make_unique<BitmapFontRenderer>(m_sdlRenderer, f.value("regular", LoaderResources::fontRegular()), f.value("bold", LoaderResources::fontBold()), sizes);
        }
        showLoading(_("Starting the game..."), 0.05f);

        // Initialize core subsystems
        showLoading(_("Loading graphics..."), 0.12f);
        m_texManager->loadAllAssets(LoaderResources::kDefaultRoot);
        showLoading(_("Preparing the renderer..."), 0.55f);
        m_effects = std::make_unique<ShaderEffects>();
        if(!m_headless)
            m_effects->init(m_sdlRenderer);
        m_renderer->setEffects(m_effects.get());
        // The map viewport ends where the command sidebar begins
        m_camera = std::make_unique<Camera>(m_windowW - HUDController::kSidebarWidth, m_windowH);
        m_input = std::make_unique<InputManager>();
        showLoading(_("Loading sound and music..."), 0.65f);
        m_sound = std::make_unique<SoundEngine>();
        m_sound->init(LoaderResources::kDefaultRoot);
        applyAudio();

        m_map = std::make_unique<TileMap>();
        m_map->loadEnvironment(m_env);
        m_pathfinder = std::make_unique<Pathfinder>(m_map.get());
        showLoading(_("Loading units and buildings..."), 0.8f);
        m_entities = std::make_unique<EntityManager>();
        m_entities->loadCharacterConfigs(LoaderResources::kDefaultRoot);
        m_entities->setTextureManager(m_texManager.get());
        m_pathfinder->setEntityManager(m_entities.get());
        m_economy = std::make_unique<Economy>();
        m_aiController = std::make_unique<AIController>();
        m_mission = std::make_unique<MissionController>();
        m_mission->setOnLoaded(
            [this]
            {
                m_pathfinder->setMap(m_map.get());
                m_pathfinder->setEntityManager(m_entities.get());
                if(m_aiController && m_mission)
                {
                    m_aiController->init(m_mission->getEnemyHqX(), m_mission->getEnemyHqY(), m_mission->getPlayerHqX(), m_mission->getPlayerHqY());
                    m_aiController->recordStartingBase(*m_entities, *m_map);
                    m_aiController->setSkirmish(m_mission->isSkirmish());
                    const AITacticsConfig &t = m_aiController->getDifficultyConfig().tactics;
                    m_mission->setDefensePacing(t.defenseFirstWaveSeconds, t.defenseWaveGapSeconds, t.defenseSquadPercent, t.defenseTimeScale);
                }
            });

#if defined(AOW_DEBUG)
        m_debug = std::make_unique<DebugOverlay>(m_window, m_sdlRenderer);
#endif
        // Initialize modular controllers
        m_menuController = std::make_unique<MenuController>();
        m_networkMenu = std::make_unique<NetworkMenu>();
        m_hudController = std::make_unique<HUDController>();
        m_hudController->setRadarAlertConfig(m_env.section("radar_alert"));
        m_gameplayController = std::make_unique<GameplayController>();
        m_gameplayController->loadOverlayStyle(m_env);
        m_menuController->bindLoading([this](const std::string &title, const std::string &path) { beginMapLoading(title, path); }, [this]() { endMapLoading(); });

        // Difficulty from the options, zoom range and graphics quality
        showLoading(_("Preparing the interface..."), 0.92f);
        m_aiController->setDifficulty(static_cast<AIDifficulty>(m_settings.difficulty));
        applyGraphics();
        m_menuController->bindSettings(m_settings, [this](SettingsArea area) { applySettings(area); }, [this]() -> const std::vector<Resolution> & { return m_resolutions; });
        if(!m_headless)
        {
            RenderApiBinding api;
            api.options = [this]() -> const std::vector<RenderApiOption> &
            {
                if(m_renderOptions.empty())
                    m_renderOptions = RenderBackend::options(m_env, true);
                return m_renderOptions;
            };
            api.active = [this]()
            {
                const char *n = SDL_GetRendererName(m_sdlRenderer);
                return std::string(n ? n : "");
            };
            api.configured = [this]() { return RenderBackend::configured(m_env); };
            api.select = [this](const std::string &name) { RenderBackend::select(m_env, name); };
            api.applyNow = [this]() { switchRenderApi(); };
            api.effectsAvailable = [this]() { return m_effects && m_effects->available(); };
            m_menuController->bindRenderApi(std::move(api));
        }

        showLoading(_("Ready"), 1.0f);
        m_state = AppState::SplashLogo;
        m_splashTimer = 0.0f;

        // Back from a window rebuild for another render API: straight to the graphics settings, keep it or go back
        const std::string previousApi = m_env.section("render").value("confirm_from", std::string());
        if(!m_headless && !previousApi.empty())
        {
            const char *active = SDL_GetRendererName(m_sdlRenderer);
            if(active && previousApi != active)
            {
                m_renderConfirm = true;
                m_videoConfirmActive = true;
                m_videoConfirmTimer = 15.0f;
            }
            else
            {
                m_env.edit("render").erase("confirm_from"); // the new API did not come up, nothing to confirm
                m_env.save();
            }
            m_state = AppState::MenuSettings;
            m_menuController->showSettingsTab(1);
        }

        m_running = true;
        std::cout << "[Application] Game engine initialized successfully!" << std::endl;
        return true;
    }

    void Application::shutdown()
    {
        // Quitting the game in a network match is leaving it for good: the others need not wait out the
        // reconnect window before this player's base goes
        if(m_net)
        {
            m_net->leave();
            m_net.reset();
        }
#if defined(AOW_DEBUG)
        if(m_debug)
            m_debug.reset();
#endif
        if(m_effects)
            m_effects->shutdown();
        if(m_sound)
            m_sound->cleanup();
        if(m_lowResTarget)
        {
            SDL_DestroyTexture(m_lowResTarget);
            m_lowResTarget = nullptr;
        }
        if(m_texManager)
            m_texManager->cleanup();

        if(m_sdlRenderer)
        {
            SDL_DestroyRenderer(m_sdlRenderer);
            m_sdlRenderer = nullptr;
        }
        if(m_window)
        {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
        }

        SDL_Quit();
        m_running = false;
    }

    void Application::setDebugMode(bool debug)
    {
#if !defined(AOW_DEBUG)
        debug = false; // cheats, the debug toolbar and full enemy control exist only in a debug build
#endif
        m_debugMode = debug;
        if(m_gameplayController)
        {
            m_gameplayController->setDebugMode(debug);
        }
    }

    void Application::launchTestMap()
    {
        if(!m_mission || !m_map || !m_entities || !m_economy || !m_pathfinder || !m_camera || !m_sound)
        {
            return;
        }
        setDebugMode(true);
        m_mission->loadTestMap(LoaderResources::kDefaultRoot, *m_map, *m_entities, *m_economy, *m_pathfinder);
        m_state = AppState::CampaignPlaying;
        m_camera->setZoom(m_settings.zoomDefault);
        m_camera->centerOn((m_mission->getPlayerHqX() + 12) * TILE_WIDTH, m_mission->getPlayerHqY() * TILE_HEIGHT);
        std::cout << "[Application] Standalone Test Map (maps/testmap.json) launched directly via --testmap flag!" << std::endl;
    }

    bool Application::launchMap(const std::string &nameOrPath, bool fromScratch)
    {
        if(!m_mission || !m_map || !m_entities || !m_economy || !m_pathfinder || !m_camera)
            return false;
        const std::string path = LoaderResources::mapPath(nameOrPath);
        const std::vector<SkirmishMapInfo> maps = MissionController::listSkirmishMaps();
        const SkirmishMapInfo *info = nullptr;
        for(const SkirmishMapInfo &m : maps)
        {
            if(m.path == path || m.name == nameOrPath)
                info = &m;
        }
        if(!info && !std::filesystem::exists(path))
        {
            std::cerr << "[Application] --map: no map \"" << nameOrPath << "\". Maps:";
            for(const SkirmishMapInfo &m : maps)
                std::cerr << " " << m.name;
            std::cerr << std::endl;
            return false;
        }
        const std::string mapFile = info ? info->path : path;
        beginMapLoading(info ? info->displayName() : std::filesystem::path(mapFile).stem().string(), mapFile);
        const bool started = m_mission->loadSkirmish(mapFile, *m_map, *m_entities, *m_economy, *m_pathfinder, !fromScratch);
        endMapLoading();
        if(!started)
        {
            std::cerr << "[Application] --map: " << mapFile << " could not be loaded" << std::endl;
            return false;
        }
        m_state = AppState::Briefing; // as from the menu: the briefing, then the game
        std::cout << "[Application] Custom game on " << mapFile << " launched via --map" << (fromScratch ? " (from scratch)" : "") << std::endl;
        return true;
    }

    void Application::run()
    {
        using clock = std::chrono::high_resolution_clock;
        auto prevTime = clock::now();
        double accumulator = 0.0;
        int frameCount = 0;
        auto fpsTimer = prevTime;

        while(m_running)
        {
            auto currentTime = clock::now();
            std::chrono::duration<double> elapsed = currentTime - prevTime;
            prevTime = currentTime;

            double frameTime = elapsed.count();
            if(frameTime > 0.25)
                frameTime = 0.25; // Clamp spiral of death
            accumulator += frameTime;

            frameCount++;
            if(std::chrono::duration<double>(currentTime - fpsTimer).count() >= 1.0)
            {
                m_fps = static_cast<float>(frameCount);
                frameCount = 0;
                fpsTimer = currentTime;
            }

            handleEvents();
            // One clock for all network timing: SDL ticks, the same the lobby's Start button uses (epoch
            // milliseconds here made a host started from the menu close thousands of turns at once)
            updateNetwork(static_cast<uint32_t>(SDL_GetTicks()));

            // Simulation / Menu Update
            if(m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying)
            {
                if(m_map)
                {
                    m_input->updateCamera(static_cast<float>(frameTime), *m_camera, m_map->getWidth(), m_map->getHeight());
                    m_hudController->updateRadarDrag(m_input->getMouseScreenX(), m_input->getMouseScreenY(), m_input->isLeftMouseDown(), static_cast<float>(frameTime), m_windowW, m_windowH, *m_map, *m_camera);
                }
                const bool net = inNetworkMatch();
                Simulation &sim = m_gameplayController->simulation();
                // P / Pause: everybody stops (a few times per player), anybody goes on
                if(net && (m_input->isKeyJustPressed(SDL_SCANCODE_P) || m_input->isKeyJustPressed(SDL_SCANCODE_PAUSE)))
                {
                    m_net->togglePause();
                }
                const uint32_t nowMs = SDL_GetTicks();
                auto playing = [&] { return m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying; };
                // One tick of a network match: only when its turn is here; checksums go out after it
                auto netTick = [&]()
                {
                    const uint32_t tick = sim.tick();
                    if(!m_net->canRunTick(tick))
                        return false;
                    driveNetworkBot(tick);
                    update(static_cast<float>(SIMULATION_TIMESTEP));
                    if(sim.tick() != tick)
                    {
                        uint64_t checksum = 0;
                        if(tick % NetMatch::kHashIntervalTicks == 0)
                        {
                            SimContext ctx {*m_entities, *m_map, *m_pathfinder, *m_economy, *m_aiController};
                            checksum = sim.checksum(ctx);
                        }
                        m_net->afterTick(tick, checksum);
                        m_net->noteTickRan(nowMs);
                        // Tests (OAOW_NET_DROP_AT=<tick>): the connection drops once at that tick
                        static const long dropAt = debugEnv("OAOW_NET_DROP_AT") ? std::atol(debugEnv("OAOW_NET_DROP_AT")) : -1;
                        static bool dropped = false;
                        if(!dropped && dropAt >= 0 && tick == static_cast<uint32_t>(dropAt))
                        {
                            dropped = true;
                            std::cerr << "[Net] Test: dropping the connection at tick " << tick << std::endl;
                            m_net->dropConnection();
                        }
                    }
                    return true;
                };
                if(net && m_net->turnsAhead(sim.tick()) > 10)
                {
                    // Far behind (back after a reconnect): play the turns through as fast as a frame allows
                    const auto deadline = clock::now() + std::chrono::milliseconds(25);
                    while(clock::now() < deadline && playing() && m_net->turnsAhead(sim.tick()) > 2 && netTick())
                    {
                    }
                    accumulator = 0.0;
                }
                else if(net)
                {
                    // Slightly behind the host: an extra tick this frame
                    if(m_net->turnsAhead(sim.tick()) > 2)
                        accumulator += SIMULATION_TIMESTEP;
                    while(accumulator >= SIMULATION_TIMESTEP && playing())
                    {
                        if(!netTick())
                        {
                            // Waiting for the host's turn: hold the time back instead of piling it up
                            accumulator = std::min(accumulator, SIMULATION_TIMESTEP);
                            break;
                        }
                        accumulator -= SIMULATION_TIMESTEP;
                    }
                }
                else
                {
                    while(accumulator >= SIMULATION_TIMESTEP)
                    {
                        update(static_cast<float>(SIMULATION_TIMESTEP));
                        accumulator -= SIMULATION_TIMESTEP;
                        if(!playing())
                            break;
                    }
                }
            }
            else
            {
                // Immediate, responsive per-frame UI update for menus & dialogs
                update(static_cast<float>(frameTime));
                accumulator = 0.0;
            }

            updateSounds();
            render();

            // Frame rate cap (also with vsync on, when the limit is below the refresh rate)
            if(m_settings.fpsLimit > 0)
            {
                const double target = 1.0 / m_settings.fpsLimit;
                const double spent = std::chrono::duration<double>(clock::now() - currentTime).count();
                if(spent < target)
                    SDL_DelayNS(static_cast<Uint64>((target - spent) * 1e9));
            }
        }
    }

    void Application::saveSettings()
    {
        m_settings.save(Settings::defaultPath());
    }

    void Application::applyAudio()
    {
        if(!m_sound)
            return;
        m_sound->setMusicVolume(m_settings.musicVolume);
        m_sound->setFxVolume(m_settings.fxVolume);
        m_sound->setVoiceVolume(m_settings.voiceVolume);
        m_sound->setVoiceLanguage(m_settings.language); // voices speak the language of the game
        if(m_sound->isMuted() != m_settings.muted)
            m_sound->toggleMute();
    }

    // Graphics quality presets, only about the picture of the game world:
    //   low     sprites at 1/3 of the resolution (chunky pixels)
    //   medium  sprites at 1/2 of the resolution
    //   high    the original resolution
    // Smoothing (a separate option) scales the sprites, and the reduced picture back up, smoothly instead of in hard pixels.
    int Application::renderDivisor() const
    {
        static const int kDivisors[3] = {3, 2, 1};
        return kDivisors[std::clamp(m_settings.graphicsQuality, 0, 2)];
    }

    void Application::applyGraphics()
    {
        if(m_camera)
            m_camera->setZoomLimits(m_settings.zoomMin, m_settings.zoomMax);
        const bool effects = m_effects && m_effects->available();
        if(m_effects)
            m_effects->configure(static_cast<ColorGrade>(m_settings.colorGrade), m_settings.glow, m_settings.sharpPixels);
        // The pixel-art filter blends texel edges itself and needs linear samples to do it
        if(m_texManager)
            m_texManager->setSpriteFiltering(m_settings.smoothing, effects && m_settings.sharpPixels);
    }

    // The edge of the map melts into the dark background instead of ending in a hard line
    void Application::renderMapEdgeFade(const Camera &camera)
    {
        if(!m_map)
            return;
        float x0, y0, x1, y1;
        camera.worldToScreen(0.0f, 0.0f, x0, y0);
        camera.worldToScreen(static_cast<float>(m_map->getWidth() * TILE_WIDTH), static_cast<float>(m_map->getHeight() * TILE_HEIGHT), x1, y1);
        const int viewW = camera.getViewportWidth(), viewH = camera.getViewportHeight();
        const int depth = std::max(4, static_cast<int>(1.6f * TILE_HEIGHT * camera.getZoom()));
        const int left = static_cast<int>(std::floor(x0)), top = static_cast<int>(std::floor(y0));
        const int right = static_cast<int>(std::ceil(x1)), bottom = static_cast<int>(std::ceil(y1));
        auto shade = [&](int i)
        {
            const float t = 1.0f - static_cast<float>(i) / depth;
            return SDL_Color {0, 0, 0, static_cast<Uint8>(215.0f * t * t)};
        };
        const int yFrom = std::max(0, top), yTo = std::min(viewH - 1, bottom - 1);
        const int xFrom = std::max(0, left), xTo = std::min(viewW - 1, right - 1);
        for(int i = 0; i < depth; ++i)
        {
            const SDL_Color c = shade(i);
            const int lx = left + i, rx = right - 1 - i, ty = top + i, by = bottom - 1 - i;
            if(lx >= 0 && lx < viewW)
                m_renderer->drawLineScreen(lx, yFrom, lx, yTo, c);
            if(rx >= 0 && rx < viewW)
                m_renderer->drawLineScreen(rx, yFrom, rx, yTo, c);
            if(ty >= 0 && ty < viewH)
                m_renderer->drawLineScreen(xFrom, ty, xTo, ty, c);
            if(by >= 0 && by < viewH)
                m_renderer->drawLineScreen(xFrom, by, xTo, by, c);
        }
    }

    void Application::applySettings(SettingsArea area)
    {
        m_settings.clamp();
        switch(area)
        {
            case SettingsArea::Video:
            {
                const Settings before = m_confirmedVideo;
                VideoModes::apply(m_window, m_sdlRenderer, m_settings);
                syncWindowSize();
                const bool windowChanged = before.windowMode != m_settings.windowMode || before.windowW != m_settings.windowW || before.windowH != m_settings.windowH || before.fullscreenW != m_settings.fullscreenW || before.fullscreenH != m_settings.fullscreenH;
                if(windowChanged && !m_videoConfirmActive)
                {
                    beginVideoConfirm(before);
                }
                else if(!windowChanged)
                {
                    m_confirmedVideo = m_settings;
                }
                break;
            }
            case SettingsArea::Graphics:
                applyGraphics();
                break;
            case SettingsArea::Audio:
                applyAudio();
                break;
            case SettingsArea::Game:
                Localization::setLanguage(m_settings.language);
                if(m_sound)
                    m_sound->setVoiceLanguage(m_settings.language);
                if(m_aiController)
                    m_aiController->setDifficulty(static_cast<AIDifficulty>(m_settings.difficulty));
                break;
        }
        saveSettings();
    }

    // F11 / Alt+Enter: windowed <-> borderless fullscreen
    void Application::toggleFullscreen()
    {
        if(m_videoConfirmActive)
            return;
        m_settings.windowMode = m_settings.windowMode == WindowMode::Windowed ? WindowMode::Borderless : WindowMode::Windowed;
        VideoModes::apply(m_window, m_sdlRenderer, m_settings);
        syncWindowSize();
        m_confirmedVideo = m_settings;
        saveSettings();
    }

    void Application::syncWindowSize(int w, int h)
    {
        if(!m_window)
            return;
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
        if(pw <= 0 || ph <= 0)
        {
            pw = w;
            ph = h;
        }
        if(pw <= 0 || ph <= 0)
        {
            SDL_GetWindowSize(m_window, &pw, &ph);
        }
        if(pw <= 0 || ph <= 0)
            return;

        if(m_windowW == pw && m_windowH == ph)
            return;

        m_windowW = pw;
        m_windowH = ph;
        if(m_camera)
        {
            m_camera->setViewport(m_windowW - HUDController::kSidebarWidth, m_windowH);
            if(m_map)
                m_camera->clampToBounds(m_map->getWidth(), m_map->getHeight());
        }
        if(!m_videoConfirmActive)
        {
            if(m_settings.windowMode == WindowMode::Windowed)
            {
                m_settings.windowW = m_windowW;
                m_settings.windowH = m_windowH;
                m_confirmedVideo.windowW = m_windowW;
                m_confirmedVideo.windowH = m_windowH;
            }
        }
    }

    void Application::beginVideoConfirm(const Settings &previous)
    {
        m_videoConfirmActive = true;
        m_videoConfirmTimer = 10.0f;
        m_confirmedVideo = previous;
    }

    namespace
    {

        struct VideoConfirmLayout
        {
            ui::UiRect dialog;
            ui::UiRect keepBtn;
            ui::UiRect revertBtn;
        };

        VideoConfirmLayout videoConfirmLayout(int windowW, int windowH)
        {
            VideoConfirmLayout l;
            l.dialog = ui::centeredRect(windowW, windowH, 520, 220);
            l.keepBtn = {l.dialog.x + 24, l.dialog.bottom() - 64, 220, 40};
            l.revertBtn = {l.dialog.right() - 24 - 220, l.dialog.bottom() - 64, 220, 40};
            return l;
        }

    } // namespace

    void Application::updateVideoConfirm(float deltaTime)
    {
        m_videoConfirmTimer -= deltaTime;
        const VideoConfirmLayout l = videoConfirmLayout(m_windowW, m_windowH);
        float lx, ly;
        const bool clicked = m_input->wasLeftClickIssued(lx, ly);
        const int mx = m_input->getMouseScreenX(), my = m_input->getMouseScreenY();

        if(m_renderConfirm)
        {
            if((clicked && l.keepBtn.contains(mx, my)) || m_input->isConfirmJustPressed())
            {
                keepRenderApi();
            }
            else if((clicked && l.revertBtn.contains(mx, my)) || m_input->isKeyJustPressed(SDL_SCANCODE_ESCAPE) || m_videoConfirmTimer <= 0.0f)
            {
                revertRenderApi();
            }
            return;
        }

        if((clicked && l.keepBtn.contains(mx, my)) || m_input->isConfirmJustPressed())
        {
            m_videoConfirmActive = false;
            m_confirmedVideo = m_settings;
            saveSettings();
        }
        else if((clicked && l.revertBtn.contains(mx, my)) || m_input->isKeyJustPressed(SDL_SCANCODE_ESCAPE) || m_videoConfirmTimer <= 0.0f)
        {
            m_videoConfirmActive = false;
            // Back to the previous window setup, the other video options stay as chosen
            m_settings.windowMode = m_confirmedVideo.windowMode;
            m_settings.windowW = m_confirmedVideo.windowW;
            m_settings.windowH = m_confirmedVideo.windowH;
            m_settings.fullscreenW = m_confirmedVideo.fullscreenW;
            m_settings.fullscreenH = m_confirmedVideo.fullscreenH;
            VideoModes::apply(m_window, m_sdlRenderer, m_settings);
            syncWindowSize();
            m_confirmedVideo = m_settings;
            saveSettings();
        }
    }

    void Application::renderVideoConfirm()
    {
        const VideoConfirmLayout l = videoConfirmLayout(m_windowW, m_windowH);
        m_renderer->fillRectScreen(0, 0, m_windowW, m_windowH, ui::color::Scrim);
        ui::UiRect content = ui::drawDialog(*m_renderer, *m_fontRenderer, m_windowW, m_windowH, l.dialog, m_renderConfirm ? _("KEEP THIS RENDER API?") : _("KEEP THESE DISPLAY SETTINGS?"));
        const int seconds = std::max(0, static_cast<int>(std::ceil(m_videoConfirmTimer)));
        m_fontRenderer->drawString(_("The previous settings come back automatically in") + std::string(" ") + std::to_string(seconds) + " " + _("s"), content.centerX(), content.y + 24, ui::FONT_BODY, ui::color::TextDim, false, Anchor::HCENTER | Anchor::VCENTER);
        const int mx = m_input->getMouseScreenX(), my = m_input->getMouseScreenY();
        ui::drawButton(*m_renderer, *m_fontRenderer, l.keepBtn, _("KEEP"), ui::ButtonStyle::Primary, l.keepBtn.contains(mx, my));
        ui::drawButton(*m_renderer, *m_fontRenderer, l.revertBtn, _("REVERT"), ui::ButtonStyle::Secondary, l.revertBtn.contains(mx, my));
    }

    // Developer aid (OAOW_NET_BOT=1): the local player sends all of its units at the nearest enemy
    // headquarters every 5 seconds, so two headless peers have real command traffic and fights to agree on
    void Application::driveNetworkBot(uint32_t tick)
    {
        static const bool bot = debugEnv("OAOW_NET_BOT") != nullptr;
        if(!bot)
            return;
        const PlayerId me = Players::local();
        const Faction myFaction = Players::factionOf(me);
        // Economy traffic too (the commands that broke the other side's faction and unit ids): riflemen from
        // every barracks, and a building of the own faction next to the headquarters now and then
        if(tick % 100 == 40)
        {
            for(const Entity &e : m_entities->getAllEntities())
            {
                const Building *b = e.asBuilding();
                if(b && b->alive && b->owner == me && b->type == UnitType::Barracks && b->buildProgress >= 1.0f)
                {
                    SimCommand produce;
                    produce.type = CommandType::Produce;
                    produce.player = me;
                    produce.a = myFaction == Faction::Confederation ? 1 : 0; // the faction's rifleman
                    produce.b = b->id;
                    m_gameplayController->simulation().issue(std::move(produce));
                }
            }
        }
        if(tick % 200 == 70)
        {
            const int slot = (tick / 200) % 2 == 0 ? 1 : 5; // power plant, bunker (canonical blueprint slots)
            const BuildingBlueprint *bp = EntityManager::findBlueprint(myFaction, slot);
            const Entity *hq = nullptr;
            for(const Entity &e : m_entities->getAllEntities())
            {
                if(e.alive && e.isBuilding && e.owner == me && e.type == UnitType::Headquarters)
                    hq = &e;
            }
            if(bp && hq)
            {
                bool placed = false;
                for(int r = 4; r < 12 && !placed; ++r)
                {
                    for(int dy = -r; dy <= r && !placed; ++dy)
                    {
                        for(int dx = -r; dx <= r && !placed; ++dx)
                        {
                            const int x = hq->tileX + dx, y = hq->tileY + dy;
                            if(std::max(std::abs(dx), std::abs(dy)) != r || !m_entities->canPlaceBuilding(x, y, bp->width, bp->height, *m_map, me))
                                continue;
                            SimCommand place;
                            place.type = CommandType::PlaceBuilding;
                            place.player = me;
                            place.a = bp->blueprintId;
                            place.x = x;
                            place.y = y;
                            std::cout << "[NetBot] place blueprint " << bp->blueprintId << " (building " << bp->buildingIndex << ") at " << x << "," << y << std::endl;
                            m_gameplayController->simulation().issue(std::move(place));
                            placed = true;
                        }
                    }
                }
            }
        }
        if(tick % 100 != 10)
            return;
        SimCommand order;
        order.type = CommandType::OrderAt;
        order.player = me;
        order.a = static_cast<int32_t>(OrderKind::AttackMove);
        int targetX = -1, targetY = -1;
        for(const Entity &e : m_entities->getAllEntities())
        {
            if(!e.alive || e.state == ActionState::Die)
                continue;
            if(e.owner == me && !e.isBuilding)
                order.ids.push_back(static_cast<uint32_t>(e.id));
            if(targetX < 0 && e.isBuilding && e.type == UnitType::Headquarters && Players::hostile(e.owner, me))
            {
                targetX = e.tileX * TILE_WIDTH + TILE_WIDTH / 2;
                targetY = e.tileY * TILE_HEIGHT + TILE_HEIGHT / 2;
            }
        }
        if(order.ids.empty() || targetX < 0)
            return;
        order.x = targetX;
        order.y = targetY;
        m_gameplayController->simulation().issue(std::move(order));
    }

    void Application::leaveNetworkGame()
    {
        if(m_net)
            m_net->leave();
        m_net.reset();
        m_netMatchStarted = false;
        m_netMatchLoaded = false;
        m_netPlayers = {};
        m_notices.clear();
        m_netMapPath.clear();
        m_netAuto = false;
        m_networkMenu->reset();
    }

    // The dedicated server of the internet tab: its room list, and joining a room it created or found
    void Application::updateServer()
    {
        if(m_net || (!m_networkMenu->internetTab() && m_cliServerAction == 0 && !m_serverJoinPending))
            return;
        const nlohmann::json &net = m_env.section("network");
        const std::string host = !m_serverOverride.empty() ? m_serverOverride : net.value("master_host", std::string("openaow.imister.tech"));
        const uint16_t port = m_serverOverridePort ? m_serverOverridePort : static_cast<uint16_t>(net.value("master_port", 38100));
        const uint32_t now = SDL_GetTicks();
        if(!m_master)
        {
            m_master = std::make_unique<Net::MasterClient>(host, port);
            m_serverListMs = 0;
        }
        m_master->update(now);
        if(m_master->state() == Net::MasterClient::State::Failed)
        {
            m_serverStatus = host + ": " + m_master->error();
            m_serverRooms.clear();
            if(m_serverJoinPending)
                m_networkMenu->setNotice(std::string(_("Server: ")) + m_master->error());
            m_serverJoinPending = false;
            return;
        }
        if(m_master->state() == Net::MasterClient::State::Connecting)
        {
            m_serverStatus = host + ": " + _("connecting...");
            return;
        }
        // The command line's request, once the server answers
        if(m_cliServerAction == 1)
            m_master->createRoom(NetMatch::gameIdentity(), m_settings.playerName, true);
        if(m_cliServerAction == 2)
            m_master->findRoom(m_cliServerCode);
        if(m_cliServerAction != 0)
        {
            m_cliServerAction = 0;
            m_serverJoinPending = true;
        }
        m_serverStatus = m_master->serverInfo().name.empty() ? host : m_master->serverInfo().name;
        if(!m_master->serverInfo().motd.empty())
            m_serverStatus += " - " + m_master->serverInfo().motd;
        // The list every 2.5 seconds, only while the internet tab of the network screen is open (no need to press
        // Refresh to see rooms come and go)
        constexpr uint32_t kServerListRefreshMs = 2500;
        if(m_serverListMs == 0 || now - m_serverListMs > kServerListRefreshMs)
        {
            m_serverListMs = now;
            m_master->requestList(OAOW_VERSION);
        }
        if(m_master->hasList())
        {
            m_serverRooms.clear();
            for(const Net::RoomListing &r : m_master->rooms())
            {
                if(r.running)
                    continue;
                const std::string map = r.map.substr(r.map.find_last_of('/') + 1);
                m_serverRooms.push_back({host, r.port, r.name, map.substr(0, map.rfind('.')), r.players, r.maxPlayers, 0, r.code, r.locked});
            }
        }
        Net::RejectReason refusal {};
        if(m_master->takeRefusal(refusal))
        {
            m_networkMenu->setNotice(std::string(_("Server: ")) + Net::rejectReasonText(refusal));
            m_serverJoinPending = false;
        }
        Net::RoomAddress room;
        if(m_serverJoinPending && m_master->takeRoom(room))
        {
            m_serverJoinPending = false;
            m_net = std::make_unique<NetMatch>();
            std::string error;
            m_net->setPassword(m_networkMenu->password());
            // Port 0: a game hosted by a player, reached through the server's introduction
            const bool ok = room.port == 0 ? m_net->joinHosted(host, port, room.code, m_settings.playerName, error) : m_net->join(host, room.port, m_settings.playerName, error, true);
            if(!ok)
            {
                m_networkMenu->setNotice(std::string(_("Cannot connect: ")) + error);
                m_net.reset();
            }
            else
            {
                std::cout << "[Net] Room " << room.code << " on " << host << ":" << room.port << std::endl;
            }
        }
    }

    void Application::updateNetworkMenu()
    {
        // SDL delivers typed text only while text input is on
        const bool wantText = m_networkMenu->wantsText();
        if(wantText != m_textInput)
        {
            if(wantText)
                SDL_StartTextInput(m_window);
            else
                SDL_StopTextInput(m_window);
            m_textInput = wantText;
        }
        const uint16_t port = static_cast<uint16_t>(m_env.section("network").value("udp_port", 38100));
        // Games on the local network while browsing (games of another version are left out)
        if(!m_net)
        {
            if(!m_lanBrowser)
                m_lanBrowser = std::make_unique<Net::LanBrowser>(port);
            m_lanBrowser->update(SDL_GetTicks());
            m_lanGames.clear();
            for(const Net::FoundRoom &r : m_lanBrowser->rooms())
            {
                if(r.info.gameVersion != OAOW_VERSION || r.info.running)
                    continue;
                const std::string map = r.info.map.substr(r.info.map.find_last_of('/') + 1);
                m_lanGames.push_back({r.address, r.port, r.info.name, map.substr(0, map.rfind('.')), r.info.players, r.info.maxPlayers, r.pingMs, std::string(), r.info.locked});
            }
        }
        else
        {
            m_lanBrowser.reset();
        }
        updateServer();
        switch(m_networkMenu->update(*m_input, m_windowW, m_windowH, m_net.get(), m_settings, m_lanGames, m_serverRooms))
        {
            case NetworkAction::Back:
                saveSettings();
                m_lanBrowser.reset();
                m_master.reset();
                m_state = AppState::MainMenu;
                break;
            case NetworkAction::Host:
            {
                saveSettings();
                m_net = std::make_unique<NetMatch>();
                std::string error;
                m_net->setPassword(m_networkMenu->password());
                if(!m_net->host(port, m_settings.playerName, error))
                {
                    m_networkMenu->setNotice(std::string(_("Cannot create the game: ")) + error);
                    m_net.reset();
                }
                else if(m_networkMenu->internetTab())
                {
                    // From the internet tab: also listed on the server, reachable by its code without
                    // forwarded ports (NAT hole punching)
                    const nlohmann::json &netEnv = m_env.section("network");
                    m_net->announce(m_serverOverride.empty() ? netEnv.value("master_host", std::string("openaow.imister.tech")) : m_serverOverride, m_serverOverridePort ? m_serverOverridePort : static_cast<uint16_t>(netEnv.value("master_port", 38100)));
                }
                break;
            }
            case NetworkAction::Join:
            {
                saveSettings();
                m_net = std::make_unique<NetMatch>();
                std::string error;
                m_net->setPassword(m_networkMenu->password());
                if(!m_net->join(m_networkMenu->joinAddress(), m_networkMenu->joinPort(), m_settings.playerName, error))
                {
                    m_networkMenu->setNotice(std::string(_("Cannot connect: ")) + error);
                    m_net.reset();
                }
                break;
            }
            case NetworkAction::Leave:
                leaveNetworkGame();
                break;
            case NetworkAction::ServerRefresh:
                m_serverListMs = 0; // updateServer asks again
                m_master.reset();   // and reconnects if the last attempt failed
                break;
            case NetworkAction::ServerCreate:
                saveSettings();
                updateServer();
                if(m_master)
                {
                    m_master->createRoom(NetMatch::gameIdentity(), m_settings.playerName, true, m_networkMenu->password());
                    m_serverJoinPending = true;
                }
                break;
            case NetworkAction::ServerJoin:
                saveSettings();
                updateServer();
                if(m_master)
                {
                    m_master->findRoom(m_networkMenu->joinCode());
                    m_serverJoinPending = true;
                }
                break;
            case NetworkAction::Start:
                if(m_net)
                {
                    m_net->startFromLobby(SDL_GetTicks(), 1500, m_networkMenu->prebuilt());
                    m_netMatchStarted = true;
                }
                break;
            case NetworkAction::None:
                break;
        }
        // The connection ended while in the menu
        if(m_net && !m_net->connected() && m_net->statusText() == "disconnected")
        {
            m_networkMenu->setNotice(_("The connection was lost"));
            leaveNetworkGame();
        }
    }

    // What a network match wants the player to know: catching up, waiting, reconnecting, out of sync
    namespace
    {
        // The name of a map for its loading screen
        std::string mapTitleOf(const std::string &path)
        {
            for(const SkirmishMapInfo &m : MissionController::listSkirmishMaps())
            {
                if(m.path == path)
                    return m.displayName();
            }
            return std::filesystem::path(path).stem().string();
        }
    } // namespace

    void Application::drawLoadingFrame(const std::string &title, const std::string &status, float progress, SDL_Texture *preview, int pw, int ph)
    {
        // Keep the window answering while the main loop is busy
        SDL_Event event;
        while(SDL_PollEvent(&event))
        {
            if(event.type == SDL_EVENT_QUIT)
            {
                SDL_PushEvent(&event); // the main loop handles it once it runs
                break;
            }
        }
        int w = 0, h = 0;
        SDL_GetRenderOutputSize(m_sdlRenderer, &w, &h);
        SDL_SetRenderTarget(m_sdlRenderer, nullptr);
        SDL_SetRenderClipRect(m_sdlRenderer, nullptr);
        SDL_SetRenderDrawColor(m_sdlRenderer, 8, 10, 14, 255);
        SDL_RenderClear(m_sdlRenderer);
        ui::drawLoadingScreen(*m_renderer, *m_fontRenderer, w, h, title, status, progress, SDL_GetTicks(), preview, pw, ph);
        SDL_RenderPresent(m_sdlRenderer);
    }

    void Application::showLoading(const std::string &status, float progress)
    {
        if(m_headless || !m_sdlRenderer || !m_renderer || !m_fontRenderer)
            return;
        drawLoadingFrame("OPEN ART OF WAR", status, progress, nullptr, 0, 0);
    }

    void Application::beginMapLoading(const std::string &title, const std::string &mapPath)
    {
        if(m_headless || !m_sdlRenderer || !m_renderer || !m_fontRenderer)
            return;
        m_mapLoading = true;
        m_mapLoadStartMs = SDL_GetTicks();
        m_mapLoadTitle = title;
        m_mapLoadTexture = m_loadingPreview.get(m_sdlRenderer, *m_texManager, "loading:" + mapPath, mapPath, {}, m_mapLoadW, m_mapLoadH);
        drawLoadingFrame(m_mapLoadTitle, _("Loading the map..."), 0.05f, m_mapLoadTexture, m_mapLoadW, m_mapLoadH);
    }

    void Application::endMapLoading()
    {
        if(!m_mapLoading)
            return;
        constexpr Uint32 kMinMs = 3000;
        // The work is done; the screen stays until the minimum time is over, the bar going on meanwhile
        for(Uint32 elapsed = SDL_GetTicks() - m_mapLoadStartMs; elapsed < kMinMs; elapsed = SDL_GetTicks() - m_mapLoadStartMs)
        {
            drawLoadingFrame(m_mapLoadTitle, _("Loading the map..."), 0.05f + 0.9f * static_cast<float>(elapsed) / kMinMs, m_mapLoadTexture, m_mapLoadW, m_mapLoadH);
            // The connection keeps being served behind the loading screen (a silent peer times out on a slow
            // link); only the transport, this runs inside updateNetwork when a match starts
            if(m_net)
                m_net->update(static_cast<uint32_t>(SDL_GetTicks()));
            SDL_Delay(16);
        }
        drawLoadingFrame(m_mapLoadTitle, _("Ready"), 1.0f, m_mapLoadTexture, m_mapLoadW, m_mapLoadH);
        m_mapLoading = false;
    }

    void Application::renderNetworkBanner()
    {
        if(!inNetworkMatch() || !(m_state == AppState::SkirmishPlaying || m_state == AppState::Victory || m_state == AppState::Defeat))
            return;
        const uint32_t tick = m_gameplayController->simulation().tick();
        std::string text;
        SDL_Color color = ui::color::Accent;
        if(m_net->desynced())
        {
            text = std::string(_("OUT OF SYNC at tick ")) + std::to_string(m_net->desyncTick()) + _(": the players' games differ, the result is not reliable");
            color = ui::color::Danger;
        }
        else if(m_net->paused())
        {
            std::string who = "?";
            if(const Net::Roster *roster = m_net->roster())
            {
                for(const Net::RosterEntry &e : roster->players)
                    if(e.slot == m_net->pausedBy())
                        who = e.name;
            }
            text = std::string(_("PAUSED by ")) + who + " - " + _("P to go on");
        }
        else if(m_net->reconnecting())
        {
            text = _("Connection lost: reconnecting...");
            color = ui::color::Danger;
        }
        else if(m_net->turnsAhead(tick) > 10)
        {
            const uint32_t total = std::max<uint32_t>(1, m_net->ticksReceived());
            text = std::string(_("Catching up with the match: ")) + std::to_string(std::min<uint32_t>(100, tick * 100 / total)) + "%";
        }
        else if(m_net->stalledSeconds(SDL_GetTicks()) > 1.5f)
        {
            text = _("Waiting for the host...");
        }
        if(text.empty())
            return;
        const int w = m_fontRenderer->getTextWidth(text, ui::FONT_BODY_BOLD) + 40;
        const int x = (m_camera->getViewportWidth() - w) / 2, y = 44;
        m_renderer->fillRectScreen(x, y, w, 34, ui::color::Panel);
        m_renderer->drawRectScreen(x, y, w, 34, color);
        m_fontRenderer->drawString(text, x + w / 2, y + 17, ui::FONT_BODY_BOLD, color, false, Anchor::HCENTER | Anchor::VCENTER);
    }

    void Application::trackNetPlayers(uint32_t nowMs)
    {
        const Net::Roster *roster = m_net ? m_net->roster() : nullptr;
        // While this computer is the one cut off, the roster is stale: nothing to report about the others
        if(!roster || !m_net->connected())
            return;
        std::array<const Net::RosterEntry *, 8> now {};
        for(const Net::RosterEntry &e : roster->players)
        {
            if(e.slot < now.size())
                now[e.slot] = &e;
        }
        char buf[200];
        for(size_t slot = 0; slot < m_netPlayers.size(); ++slot)
        {
            NetPlayer &p = m_netPlayers[slot];
            const Net::RosterEntry *e = now[slot];
            const bool local = slot == m_net->localSlot();
            if(e)
            {
                if(!e->name.empty())
                    p.name = e->name;
                if(p.present && !local)
                {
                    if(p.connected && !e->connected)
                    {
                        std::snprintf(buf, sizeof buf, _("Player %s has disconnected"), p.name.c_str());
                        pushNotice(buf, nowMs);
                        std::cout << "[Net] Player " << p.name << " (slot " << slot << ") disconnected" << std::endl;
                    }
                    else if(!p.connected && e->connected)
                    {
                        std::snprintf(buf, sizeof buf, _("Player %s is back"), p.name.c_str());
                        pushNotice(buf, nowMs);
                    }
                }
                p.present = true;
                p.connected = e->connected;
            }
            else if(p.present)
            {
                // Out of the roster: left the match, or gone longer than the reconnect window. Its base and
                // units are destroyed by the turn that announces it (see CommandType::PlayerLeft).
                if(!local)
                {
                    std::snprintf(buf, sizeof buf, p.connected ? _("Player %s has disconnected") : _("Player %s did not come back"), p.name.c_str());
                    pushNotice(buf, nowMs);
                    std::snprintf(buf, sizeof buf, _("The base of %s is destroyed"), p.name.c_str());
                    pushNotice(buf, nowMs);
                }
                p.present = false;
                p.connected = false;
                p.left = true;
                std::cout << "[Net] Player " << p.name << " (slot " << slot << ") left the match" << std::endl;
            }
        }
    }

    void Application::pushNotice(const std::string &text, uint32_t nowMs)
    {
        constexpr uint32_t kShowMs = 7000;
        constexpr size_t kMaxNotices = 5;
        m_notices.push_back({text, nowMs + kShowMs});
        if(m_notices.size() > kMaxNotices)
            m_notices.erase(m_notices.begin(), m_notices.end() - kMaxNotices);
    }

    void Application::renderNotices()
    {
        const uint32_t nowMs = SDL_GetTicks();
        m_notices.erase(std::remove_if(m_notices.begin(), m_notices.end(), [&](const Notice &n) { return nowMs >= n.untilMs; }), m_notices.end());
        if(m_notices.empty() || !(m_state == AppState::SkirmishPlaying || m_state == AppState::CampaignPlaying || m_state == AppState::Paused || m_state == AppState::Victory || m_state == AppState::Defeat))
            return;
        const int viewW = m_camera->getViewportWidth();
        int y = 86; // under the network banner
        for(const Notice &n : m_notices)
        {
            // The last second fades out
            const uint32_t left = n.untilMs - nowMs;
            const float alpha = std::min(1.0f, left / 1000.0f);
            auto fade = [&](SDL_Color c) { return ui::withAlpha(c, static_cast<uint8_t>(c.a * alpha)); };
            const int w = m_fontRenderer->getTextWidth(n.text, ui::FONT_BODY_BOLD) + 32;
            const int x = (viewW - w) / 2;
            m_renderer->fillRectScreen(x, y, w, 28, fade(ui::color::Panel));
            m_renderer->fillRectScreen(x, y, 3, 28, fade(ui::color::Danger));
            m_fontRenderer->drawString(n.text, x + w / 2, y + 14, ui::FONT_BODY_BOLD, fade(ui::color::Text), false, Anchor::HCENTER | Anchor::VCENTER);
            y += 32;
        }
    }

    void Application::renderScoreboard()
    {
        if(!m_input || !m_input->isKeyPressed(SDL_SCANCODE_TAB) || !m_entities)
            return;
        if(!(m_state == AppState::SkirmishPlaying || m_state == AppState::CampaignPlaying || m_state == AppState::Victory || m_state == AppState::Defeat))
            return;
        if(m_menuController->isInGameMenuOpen())
            return;
        namespace col = ui::color;
        struct Row
        {
            PlayerId id;
            std::string name;
            std::string faction;
            SDL_Color color;
            std::string colorName;
            int points;
            int kills;
            int losses;
            bool gone;
        };
        std::vector<Row> rows;
        for(PlayerId id = 0; id < kMaxPlayers; ++id)
        {
            if(!Players::isActive(id))
                continue;
            const PlayerSlot &slot = Players::slot(id);
            Row r;
            r.id = id;
            r.gone = false;
            if(!slot.human)
            {
                static const char *const kLevels[4] = {"Easy", "Medium", "Hard", "Hardcore"};
                r.name = std::string(_("AI")) + " (" + _(kLevels[slot.aiLevel & 3]) + ")";
            }
            else if(inNetworkMatch() && !m_netPlayers[id].name.empty())
            {
                r.name = m_netPlayers[id].name;
                r.gone = m_netPlayers[id].left;
            }
            else if(id == Players::local())
            {
                r.name = m_settings.playerName;
            }
            else
            {
                r.name = std::string(_("Player")) + " " + std::to_string(id + 1);
            }
            r.faction = slot.faction == Faction::Confederation ? _("Confederation") : _("Resistance");
            r.color = slot.color == 0 ? col::TextDim : armyColor(slot.color);
            r.colorName = _(armyColorName(slot.color));
            const EntityManager::PlayerStats &st = m_entities->getPlayerStats(id);
            r.points = m_entities->getEfficiencyPoints(id);
            r.kills = st.unitsKilled + st.buildingsDestroyed;
            r.losses = st.unitsLost + st.buildingsLost;
            rows.push_back(r);
        }
        if(rows.empty())
            return;
        // Best first; the same points: in slot order
        std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.points > b.points; });

        const int viewW = m_camera->getViewportWidth();
        const int rowH = 30, headH = 64;
        // Columns: colour swatch + name | faction | colour | efficiency | kills | losses
        const int colName = 16, colFaction = 250, colColor = 410, colPoints = 520, colKills = 620, colLosses = 700;
        const int w = std::min(viewW - 32, 780);
        const int h = headH + static_cast<int>(rows.size()) * rowH + 34;
        const int x = (viewW - w) / 2, y = std::max(60, (m_windowH - h) / 3);
        m_renderer->fillRectScreen(x, y, w, h, col::Panel);
        m_renderer->drawRectScreen(x, y, w, h, col::BorderStrong);
        m_renderer->fillRectScreen(x, y, w, 2, col::Accent);
        m_fontRenderer->drawString(_("MATCH STATISTICS"), x + colName, y + 12, ui::FONT_H2, col::Accent, false);
        const int hy = y + 44;
        m_fontRenderer->drawString(_("Player"), x + colName + 22, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
        m_fontRenderer->drawString(_("Faction"), x + colFaction, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
        m_fontRenderer->drawString(_("Color"), x + colColor, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
        m_fontRenderer->drawString(_("Efficiency"), x + colPoints, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
        m_fontRenderer->drawString(_("Kills"), x + colKills, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
        m_fontRenderer->drawString(_("Losses"), x + colLosses, hy, ui::FONT_SMALL_BOLD, col::TextDim, false);
        int ry = y + headH;
        for(const Row &r : rows)
        {
            const bool me = r.id == Players::local();
            if(me)
                m_renderer->fillRectScreen(x + 4, ry, w - 8, rowH - 2, col::Hover);
            const int cy = ry + rowH / 2 - 1;
            const SDL_Color text = r.gone ? col::TextMuted : col::Text;
            m_renderer->fillRectScreen(x + colName, cy - 7, 14, 14, r.color);
            m_renderer->drawRectScreen(x + colName, cy - 7, 14, 14, col::Border);
            std::string name = r.name;
            if(r.gone)
                name += std::string(" (") + _("left") + ")";
            m_fontRenderer->drawString(name, x + colName + 22, cy, me ? ui::FONT_BODY_BOLD : ui::FONT_BODY, text, false, Anchor::LEFT | Anchor::VCENTER);
            m_fontRenderer->drawString(r.faction, x + colFaction, cy, ui::FONT_BODY, text, false, Anchor::LEFT | Anchor::VCENTER);
            m_fontRenderer->drawString(r.colorName, x + colColor, cy, ui::FONT_BODY, text, false, Anchor::LEFT | Anchor::VCENTER);
            m_fontRenderer->drawString(std::to_string(r.points), x + colPoints, cy, ui::FONT_BODY_BOLD, r.gone ? col::TextMuted : col::Accent, false, Anchor::LEFT | Anchor::VCENTER);
            m_fontRenderer->drawString(std::to_string(r.kills), x + colKills, cy, ui::FONT_BODY, text, false, Anchor::LEFT | Anchor::VCENTER);
            m_fontRenderer->drawString(std::to_string(r.losses), x + colLosses, cy, ui::FONT_BODY, text, false, Anchor::LEFT | Anchor::VCENTER);
            ry += rowH;
        }
        m_fontRenderer->drawString(_("Efficiency: a point for every 10 credits of enemy forces destroyed"), x + colName, ry + 10, ui::FONT_SMALL, col::TextMuted, false);
    }

    bool Application::inNetworkMatch() const
    {
        return m_net && m_netMatchStarted && m_mission && m_mission->isMatch();
    }

    void Application::hostNetworkGame(uint16_t port, int players, const std::string &mapPath, const std::string &name, bool fromScratch)
    {
        m_net = std::make_unique<NetMatch>();
        std::string error;
        if(!m_net->host(port, name, error))
        {
            std::cerr << "[Net] Cannot host on port " << port << ": " << error << std::endl;
            m_net.reset();
            return;
        }
        if(m_networkMenu)
            m_networkMenu->setPrebuilt(!fromScratch);
        m_netWantedPlayers = std::clamp(players, 1, kMaxPlayers);
        m_netMapPath = mapPath;
        m_netAuto = true;
        m_state = AppState::MenuNetwork; // the room shows who has joined
        std::cout << "[Net] Hosting on UDP " << port << ", waiting for " << m_netWantedPlayers << " players" << std::endl;
    }

    void Application::announceNetworkGame(const std::string &server, uint16_t port)
    {
        if(m_net)
            m_net->announce(server, port);
    }

    void Application::joinNetworkGame(const std::string &address, uint16_t port, const std::string &name)
    {
        m_net = std::make_unique<NetMatch>();
        std::string error;
        if(!m_net->join(address, port, name, error))
        {
            std::cerr << "[Net] Cannot join " << address << ":" << port << ": " << error << std::endl;
            m_net.reset();
            return;
        }
        std::cout << "[Net] Joining " << address << ":" << port << std::endl;
        m_netAuto = true;
        m_state = AppState::MenuNetwork;
    }

    void Application::serverNetworkGame(const std::string &server, uint16_t port, const std::string &code, int players, const std::string &mapPath, const std::string &name, bool fromScratch)
    {
        if(m_networkMenu && code.empty())
            m_networkMenu->setPrebuilt(!fromScratch);
        m_serverOverride = server;
        m_serverOverridePort = port;
        m_cliServerAction = code.empty() ? 1 : 2;
        m_cliServerCode = code;
        m_netWantedPlayers = std::clamp(players, 1, kMaxPlayers);
        m_netMapPath = mapPath;
        m_netAuto = true;
        m_settings.playerName = name;
        m_state = AppState::MenuNetwork;
        std::cout << "[Net] Server " << server << ":" << port << (code.empty() ? ", creating a room" : ", room " + code) << std::endl;
    }

    void Application::updateNetwork(uint32_t nowMs)
    {
        if(!m_net)
            return;
        m_net->update(nowMs);
        static std::string announcedCode;
        if(m_net->publicCode() != announcedCode)
        {
            announcedCode = m_net->publicCode();
            if(!announcedCode.empty())
                std::cout << "[Net] Listed on the server as " << announcedCode << std::endl;
        }
        // A game from the command line: everybody says ready by itself, the host / room owner sets the map and
        // starts as soon as the wanted number of players is there (factions at random, teams by slot)
        if(m_netAuto && m_net->connected() && !m_netMatchStarted && nowMs - m_netAutoMs > 300)
        {
            m_netAutoMs = nowMs;
            const Net::LobbyState *lobby = m_net->lobby();
            if(m_net->controlsRoom() && !m_netMapPath.empty() && lobby && lobby->map != m_netMapPath)
                m_net->setLobbyMap(m_netMapPath);
            if(lobby)
            {
                for(const Net::LobbySlot &s : lobby->slots)
                {
                    Net::LobbyChoice mine = s.choice;
                    // OAOW_NET_FACTION_BY_SLOT=1 (self-test): even slots Resistance, odd ones Confederation, so
                    // a match always has both factions
                    static const bool factionBySlot = debugEnv("OAOW_NET_FACTION_BY_SLOT") != nullptr;
                    if(s.slot == m_net->localSlot() && m_net->localChoice(mine) && !mine.ready && m_net->mapReady())
                    {
                        if(factionBySlot)
                            mine.faction = static_cast<uint8_t>(m_net->localSlot() % 2 == 0 ? Faction::Resistance : Faction::Confederation);
                        // OAOW_NET_FACTION=0|1: this faction for everybody (players of one faction)
                        if(const char *forced = debugEnv("OAOW_NET_FACTION"); forced && (forced[0] == '0' || forced[0] == '1'))
                            mine.faction = static_cast<uint8_t>(forced[0] - '0');
                        mine.ready = true;
                        m_net->setChoice(mine);
                    }
                }
            }
            if(m_net->controlsRoom() && m_net->canStart() && m_net->playerCount() >= m_netWantedPlayers)
            {
                m_net->startFromLobby(nowMs, 1500, m_networkMenu->prebuilt());
                std::cout << "[Net] Starting the match on " << m_netMapPath << " for " << m_net->playerCount() << " players" << std::endl;
                m_netMatchStarted = true;
            }
        }
        MatchSetup setup;
        if(m_net->takeStartedMatch(setup))
        {
            beginMapLoading(mapTitleOf(setup.mapPath), setup.mapPath);
            if(!m_mission->loadMatch(setup, *m_map, *m_entities, *m_economy, *m_pathfinder))
            {
                endMapLoading();
                std::cerr << "[Net] The match map could not be set up" << std::endl;
                m_net.reset();
                return;
            }
            m_netMatchStarted = true;
            m_netMatchLoaded = true;
            m_netPlayers = {};
            m_notices.clear();
            // A fresh simulation now, before the first turn's commands are scheduled into it
            m_gameplayController->resetForMission(*m_mission, *m_hudController, *m_entities, *m_map);
            m_net->attach(m_gameplayController->simulation());
            m_net->setReconnectWindow(static_cast<uint32_t>(m_env.section("network").value("reconnect_window_s", 60)) * 1000u);
            m_state = AppState::SkirmishPlaying;
            m_camera->setZoom(m_settings.zoomDefault);
            m_camera->centerOn(m_mission->getPlayerHqX() * TILE_WIDTH, m_mission->getPlayerHqY() * TILE_HEIGHT);
            endMapLoading(); // not before three seconds of the loading screen are over
            std::cout << "[Net] Match started, local slot " << int(setup.local) << std::endl;
            // Who plays what, as this computer got it (the self-test compares the peers' lines)
            for(int i = 0; i < kMaxPlayers; ++i)
            {
                if(setup.slots[i].active)
                    std::cout << "[Net] Slot " << i << ": faction " << static_cast<int>(setup.slots[i].faction) << " team " << int(setup.slots[i].team) << " color " << int(setup.slots[i].color) << std::endl;
            }
        }
        if(!m_net->failure().empty())
        {
            std::cerr << "[Net] " << m_net->failure() << std::endl;
            m_networkMenu->setNotice(m_net->failure());
            m_state = AppState::MenuNetwork;
            leaveNetworkGame();
            return;
        }
        if(m_net->rejected())
        {
            std::cerr << "[Net] Rejected: " << m_net->statusText() << std::endl;
            m_networkMenu->setNotice(std::string(_("Not admitted: ")) + m_net->statusText());
            leaveNetworkGame();
            return;
        }
        // Back in the menus after a match: the session is over
        if(m_netMatchLoaded && (m_state == AppState::MainMenu || m_state == AppState::MenuNetwork))
        {
            leaveNetworkGame();
            return;
        }
        if(m_netMatchLoaded)
            trackNetPlayers(nowMs);
        if(m_net->desynced())
        {
            static bool reported = false;
            if(!reported)
                std::cerr << "[Net] DESYNC: the worlds of the players differ" << std::endl;
            reported = true;
        }
    }

    void Application::requestRestart()
    {
        m_restartRequested = true;
        m_running = false;
    }

    void Application::switchRenderApi()
    {
        const char *active = SDL_GetRendererName(m_sdlRenderer);
        if(!active || RenderBackend::configured(m_env) == active)
            return;
        m_env.edit("render")["confirm_from"] = active;
        m_env.save();
        requestRestart();
    }

    void Application::keepRenderApi()
    {
        m_renderConfirm = false;
        m_videoConfirmActive = false;
        m_env.edit("render").erase("confirm_from");
        m_env.save();
    }

    void Application::revertRenderApi()
    {
        const std::string previous = m_env.section("render").value("confirm_from", std::string());
        m_renderConfirm = false;
        m_videoConfirmActive = false;
        m_env.edit("render").erase("confirm_from");
        RenderBackend::select(m_env, previous); // saves
        if(!previous.empty())
            requestRestart();
    }

    void Application::renderFpsCounter()
    {
        if(!m_settings.showFps)
            return;
        m_fontRenderer->drawString("FPS " + std::to_string(static_cast<int>(m_fps + 0.5f)), 10, 8, ui::FONT_SMALL_BOLD, ui::color::Accent, true);
    }

    void Application::renderNetStats()
    {
        if(!m_settings.showNetStats || !m_net)
            return;
        const uint32_t tick = m_netMatchLoaded ? m_gameplayController->simulation().tick() : 0;
        const NetMatch::Stats st = m_net->stats(static_cast<uint32_t>(SDL_GetTicks()), tick);
        if(!st.valid)
            return;
        namespace col = ui::color;
        auto pingColor = [](uint32_t ms) { return ms < 80 ? col::Ok : (ms < 160 ? col::Accent : col::Danger); };
        struct Line
        {
            std::string text;
            SDL_Color color;
        };
        std::vector<Line> lines;
        char buf[160];
        lines.push_back({_("NETWORK"), col::Accent});
        if(st.host)
        {
            lines.push_back({_("Hosting this game"), col::TextDim});
        }
        else
        {
            std::snprintf(buf, sizeof buf, "%s %u %s   %s %u %s", _("Ping"), st.rttMs, _("ms"), _("Jitter"), st.jitterMs, _("ms"));
            lines.push_back({buf, pingColor(st.rttMs + st.jitterMs)});
            std::snprintf(buf, sizeof buf, "%s %.1f%%", _("Packet loss"), st.lossPercent);
            lines.push_back({buf, st.lossPercent < 1.0f ? col::Text : (st.lossPercent < 5.0f ? col::Accent : col::Danger)});
            // (a host's own client talks to its server through memory: no traffic of its own to show)
            std::snprintf(buf, sizeof buf, "%s  %.1f / %.1f %s", _("Traffic out / in"), st.sendKBps, st.recvKBps, _("KB/s"));
            lines.push_back({buf, col::Text});
        }
        if(m_netMatchLoaded)
        {
            std::snprintf(buf, sizeof buf, "%s %d %s  (%s %d %s)", _("Input delay"), st.inputDelayMs, _("ms"), _("turn"), st.turnMs, _("ms"));
            lines.push_back({buf, col::Text});
            std::snprintf(buf, sizeof buf, "%s %d", _("Turns buffered"), st.turnsBuffered);
            lines.push_back({buf, st.turnsBuffered > 2 ? col::Accent : col::Text});
            if(st.stalledSeconds > 0.3f)
            {
                std::snprintf(buf, sizeof buf, "%s %.1f %s", _("Waiting for the next turn"), st.stalledSeconds, _("s"));
                lines.push_back({buf, col::Danger});
            }
        }
        for(const NetMatch::Stats::Peer &p : st.peers)
        {
            if(p.rttMs == 0 && p.connected)
                continue; // the host's own client (memory link)
            std::snprintf(buf, sizeof buf, "%d. %s  %s", p.slot + 1, p.name.c_str(), p.connected ? (std::to_string(p.rttMs) + " " + _("ms")).c_str() : _("disconnected"));
            lines.push_back({buf, p.connected ? pingColor(p.rttMs) : col::Danger});
        }
        if(st.reconnecting)
            lines.push_back({_("Reconnecting..."), col::Danger});
        if(st.desynced)
            lines.push_back({_("DESYNC"), col::Danger});

        int w = 0;
        for(const Line &l : lines)
            w = std::max(w, m_fontRenderer->getTextWidth(l.text, ui::FONT_SMALL));
        const int lineH = 16, x = 8, y = m_settings.showFps ? 28 : 8;
        const int h = static_cast<int>(lines.size()) * lineH + 8;
        m_renderer->fillRectScreen(x, y, w + 16, h, {8, 10, 14, 200});
        m_renderer->drawRectScreen(x, y, w + 16, h, col::Border);
        for(size_t i = 0; i < lines.size(); ++i)
            m_fontRenderer->drawString(lines[i].text, x + 8, y + 4 + static_cast<int>(i) * lineH, i == 0 ? ui::FONT_SMALL_BOLD : ui::FONT_SMALL, lines[i].color, false);
    }

#if defined(AOW_DEBUG)
    bool Application::runHeadlessTest(int numTicks)
    {
        return HeadlessTest::run(*this, numTicks);
    }
#endif

    void Application::handleEvents()
    {
        syncWindowSize();
        m_input->resetFrameEvents();
        SDL_Event event;
        while(SDL_PollEvent(&event))
        {
#if defined(AOW_DEBUG)
            if(m_debug && m_debug->isVisible())
            {
                m_debug->processEvent(event);
            }
#endif

            if(event.type == SDL_EVENT_QUIT)
            {
                m_running = false;
                return;
            }

            // Only the game window: the throw-away windows that probe the render APIs report their own sizes
            if((event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_MAXIMIZED || event.type == SDL_EVENT_WINDOW_RESTORED || event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) && event.window.windowID == SDL_GetWindowID(m_window))
            {
                syncWindowSize(event.window.data1, event.window.data2);
            }

            // A click on a button, list entry or cameo the last frame drew under the mouse
            if(event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && SoundRequests::hoverFlag() && m_sound)
                m_sound->playSfx("ui_click");
            m_input->processEvent(event, *m_camera);
        }

        if(m_gameplayController && m_gameplayController->consumeDebugOverlayToggle())
        {
#if defined(AOW_DEBUG)
            if(m_debug)
                m_debug->toggle();
#endif
        }
        // Refresh mouse position every frame so menus get hover feedback too
        m_input->update(0.0f, *m_camera);
    }

    void Application::update(float deltaTime)
    {
        m_globalAnimTimer += deltaTime;

        // Enter is consumed only together with Alt; a plain Enter belongs to the menus
        if(((SDL_GetModState() & SDL_KMOD_ALT) && m_input->isKeyJustPressed(SDL_SCANCODE_RETURN)))
        {
            toggleFullscreen();
        }
        if(m_videoConfirmActive)
        {
            updateVideoConfirm(deltaTime);
            return;
        }

#if defined(AOW_DEBUG)
        // Standalone Test Map (-1) always runs with DEBUG mode active (debug builds)
        if(m_mission && m_mission->getCurrentMissionId() == -1 && (!m_debugMode || (m_gameplayController && !m_gameplayController->isDebugMode())))
        {
            setDebugMode(true);
        }
#endif

        if(m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying)
        {
            m_gameplayController->update(deltaTime, *m_input, *m_camera, *m_map, *m_pathfinder, *m_entities, *m_economy, *m_aiController, *m_mission, *m_hudController, *m_menuController, *m_sound, m_state, m_windowW, m_windowH);
        }
        else if(m_state == AppState::SplashLogo)
        {
            m_splashTimer += deltaTime;
            float lx, ly;
            if(m_splashTimer >= 2.0f || m_input->isConfirmJustPressed() || m_input->isKeyJustPressed(SDL_SCANCODE_ESCAPE) || m_input->wasLeftClickIssued(lx, ly))
            {
                m_state = AppState::SplashTitle;
                m_splashTimer = 0.0f;
            }
        }
        else if(m_state == AppState::SplashTitle)
        {
            m_splashTimer += deltaTime;
            float lx, ly;
            if(m_splashTimer >= 2.2f || m_input->isConfirmJustPressed() || m_input->isKeyJustPressed(SDL_SCANCODE_ESCAPE) || m_input->wasLeftClickIssued(lx, ly))
            {
                m_state = AppState::MainMenu;
            }
        }
        else if(m_state == AppState::MainMenu)
        {
            m_menuController->updateMainMenu(*m_input, m_windowW, m_windowH, m_state, m_running, *m_sound, *m_mission, *m_map, *m_entities, *m_economy, *m_pathfinder, *m_camera, *m_aiController);
        }
        else if(m_state == AppState::MenuSettings)
        {
            m_menuController->updateSettings(*m_input, m_windowW, m_windowH, m_state);
        }
        else if(m_state == AppState::MenuNetwork)
        {
            updateNetworkMenu();
        }
        else if(m_state == AppState::MenuHelp || m_state == AppState::MenuAbout)
        {
            m_menuController->updateInfoScreen(*m_input, m_state);
        }
        else if(m_state == AppState::Briefing)
        {
            float lx, ly;
            bool clicked = m_input->wasLeftClickIssued(lx, ly);
            if(clicked || m_input->isConfirmJustPressed())
            {
                m_aiController->init(m_mission->getEnemyHqX(), m_mission->getEnemyHqY(), m_mission->getPlayerHqX(), m_mission->getPlayerHqY());
                m_state = AppState::CampaignPlaying;
                m_camera->setZoom(m_settings.zoomDefault);
                m_camera->centerOn(m_mission->getPlayerHqX() * TILE_WIDTH, m_mission->getPlayerHqY() * TILE_HEIGHT);
            }
        }
        else if(m_state == AppState::Paused)
        {
            if(m_input->isKeyJustPressed(SDL_SCANCODE_ESCAPE) || m_input->isConfirmJustPressed())
            {
                m_state = AppState::CampaignPlaying;
            }
        }
        else if(m_state == AppState::Victory || m_state == AppState::Defeat)
        {
            float lx, ly;
            if(m_input->isConfirmJustPressed() || m_input->wasLeftClickIssued(lx, ly))
            {
                m_state = AppState::MainMenu;
            }
        }

        const bool gameplayMusic = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying || m_state == AppState::Paused || m_state == AppState::Briefing || m_state == AppState::Victory || m_state == AppState::Defeat;
        // The menu theme goes on through every screen of the main menu (help, about, settings, network game)
        const bool menuMusic = m_state == AppState::MainMenu || m_state == AppState::MenuHelp || m_state == AppState::MenuAbout || m_state == AppState::MenuSettings || m_state == AppState::MenuNetwork;
        m_sound->update(menuMusic, gameplayMusic);
    }

    void Application::updateSounds()
    {
        if(!m_sound)
        {
            SoundRequests::take();
            return;
        }
        if(!m_gameSounds)
            m_gameSounds = std::make_unique<GameSounds>();
        const bool inMatch = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying || m_state == AppState::Paused || m_state == AppState::Victory || m_state == AppState::Defeat;
        for(const SoundRequests::Request &r : SoundRequests::take())
        {
            if(!r.event.empty() && r.event[0] == '@')
            {
                if(inMatch && m_gameplayController && m_entities)
                    m_gameSounds->playReply(*m_sound, *m_entities, m_gameplayController->getSelectedUnits(), r.event.c_str() + 1);
            }
            else if(r.announcer)
            {
                m_sound->announce(r.event);
            }
            else
            {
                m_sound->playSfx(r.event);
            }
        }
        if(inMatch && m_state != AppState::Paused && m_entities && m_map && m_economy && m_camera && m_hudController && m_gameplayController)
        {
            const bool fog = m_map->isFogEnabled() && !m_gameplayController->isRevealFog();
            m_gameSounds->update(*m_sound, *m_entities, *m_economy, *m_map, *m_camera, fog, m_hudController->radarAlert(), SDL_GetTicks());
            m_gameSounds->selectionChanged(*m_sound, *m_entities, m_gameplayController->getSelectedUnits());
        }
        // The end of a match
        if(m_state != m_soundState)
        {
            if(m_state == AppState::Victory)
                m_sound->announce("ann_victory");
            else if(m_state == AppState::Defeat)
                m_sound->announce("ann_defeat");
            else if(!inMatch && (m_soundState == AppState::CampaignPlaying || m_soundState == AppState::SkirmishPlaying || m_soundState == AppState::Paused ||
                                 m_soundState == AppState::Victory || m_soundState == AppState::Defeat))
            {
                m_sound->sfx().stopAll(); // back in the menus: the battle falls silent
                m_gameSounds->reset();
            }
            m_soundState = m_state;
        }
    }

    void Application::render()
    {
        SoundRequests::hoverFlag() = false; // the widgets drawn now mark it again
        m_renderer->clear({0,0,0,255});

        // Game UI wears the player's faction colors; menus stay neutral (the ImGui debug UI has its own style)
        const bool inGame = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying || m_state == AppState::Paused || m_state == AppState::Victory || m_state == AppState::Defeat || m_state == AppState::Briefing;
        const int localColor = inGame ? Players::slot(Players::local()).color : 0;
        const SDL_Color localArmy = armyColor(localColor);
        ui::applyTheme(inGame ? playerFaction() : Faction::Neutral, localColor > 0 ? &localArmy : nullptr);

        if(m_state == AppState::SplashLogo)
        {
            m_menuController->renderSplashLogo(*m_renderer, *m_camera, *m_texManager, m_windowW, m_windowH);
        }
        else if(m_state == AppState::SplashTitle)
        {
            m_menuController->renderSplashTitle(*m_renderer, *m_camera, *m_texManager, m_windowW, m_windowH);
        }
        else if(m_state == AppState::MainMenu)
        {
            m_menuController->renderMainMenu(*m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
        }
        else if(m_state == AppState::MenuHelp)
        {
            m_menuController->renderMainMenu(*m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
            m_menuController->renderMenuHelp(*m_renderer, *m_fontRenderer, m_windowW, m_windowH);
        }
        else if(m_state == AppState::MenuAbout)
        {
            m_menuController->renderMainMenu(*m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
            m_menuController->renderMenuAbout(*m_renderer, *m_fontRenderer, m_windowW, m_windowH);
        }
        else if(m_state == AppState::MenuSettings)
        {
            m_menuController->renderMainMenu(*m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
            m_menuController->renderMenuSettings(*m_renderer, *m_fontRenderer, m_windowW, m_windowH);
        }
        else if(m_state == AppState::MenuNetwork)
        {
            m_menuController->renderMainMenu(*m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
            m_networkMenu->render(*m_renderer, *m_fontRenderer, *m_texManager, m_windowW, m_windowH, m_net.get(), m_settings, m_lanGames, m_serverRooms, m_serverStatus);
        }
        else if(m_state == AppState::Briefing)
        {
            m_menuController->renderBriefing(*m_renderer, *m_fontRenderer, *m_mission, m_windowW, m_windowH);
        }
        else if(m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying || m_state == AppState::Paused || m_state == AppState::Victory || m_state == AppState::Defeat)
        {
            // Army colours (shader effects): from the player slots (chosen in the game setup / the network room)
            for(PlayerId p = 0; p < kMaxPlayers; ++p)
            {
                const int color = Players::slot(p).color;
                m_renderer->setArmyColor(p, Players::isActive(p) && color > 0, armyColor(color));
            }

            // The world is drawn into a smaller target at low / medium quality and scaled up afterwards
            const int divisor = renderDivisor();
            const Camera lowCam = divisor > 1 ? m_camera->scaledDown(divisor) : *m_camera;
            const Camera &worldCam = divisor > 1 ? lowCam : *m_camera;
            // Shader effects: the world goes into an offscreen picture, graded and upscaled on the way to the screen
            SDL_Texture *scene = m_effects && m_effects->postActive() ? m_effects->sceneTarget(worldCam.getViewportWidth(), worldCam.getViewportHeight()) : nullptr;
            if(scene)
            {
                SDL_SetRenderTarget(m_sdlRenderer, scene);
                m_renderer->clear({0, 0, 0, 255});
            }
            else if(divisor > 1)
            {
                const int lowW = worldCam.getViewportWidth(), lowH = worldCam.getViewportHeight();
                if(!m_lowResTarget || m_lowResW != lowW || m_lowResH != lowH)
                {
                    if(m_lowResTarget)
                        SDL_DestroyTexture(m_lowResTarget);
                    m_lowResTarget = SDL_CreateTexture(m_sdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, lowW, lowH);

                    m_lowResW = lowW;
                    m_lowResH = lowH;
                }
                if(m_lowResTarget)
                {
                    SDL_SetTextureScaleMode(m_lowResTarget, m_settings.smoothing ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
                    SDL_SetRenderTarget(m_sdlRenderer, m_lowResTarget);
                    m_renderer->clear({0, 0, 0, 255});
                }
            }
            // Everything of the world stays inside the map: tiles, cliffs, trees, buildings and the fog no longer
            // stick out over its edge
            {
                float x0, y0, x1, y1;
                worldCam.worldToScreen(0.0f, 0.0f, x0, y0);
                worldCam.worldToScreen(static_cast<float>(m_map->getWidth() * TILE_WIDTH), static_cast<float>(m_map->getHeight() * TILE_HEIGHT), x1, y1);
                const int left = std::max(0, static_cast<int>(std::floor(x0)));
                const int top = std::max(0, static_cast<int>(std::floor(y0)));
                const int right = std::min(worldCam.getViewportWidth(), static_cast<int>(std::ceil(x1)));
                const int bottom = std::min(worldCam.getViewportHeight(), static_cast<int>(std::ceil(y1)));
                m_renderer->setClipRect({left, top, std::max(0, right - left), std::max(0, bottom - top)});
            }

            // Layer 0: Base Terrain
#if defined(AOW_DEBUG)
            const bool overlayNoFog = m_debug && m_debug->isFogDisabled();
#else
            const bool overlayNoFog = false;
#endif
            bool disableFog = overlayNoFog || m_gameplayController->isRevealFog() || !m_map->isFogEnabled();
            int globalAnimTick = static_cast<int>(m_globalAnimTimer * 8.0f);
            m_map->renderTerrain(*m_renderer, worldCam, !disableFog, globalAnimTick);

            // Layer 1: Ground Remnants (Strictly 1 layer above ground, underneath decorations, walls, buildings & units)
            m_map->renderWallRubble(*m_renderer, worldCam, !disableFog);
            m_entities->renderGroundRemnants(*m_renderer, worldCam, *m_map, !disableFog);

            // Layer 2: Map Decorations (trees, bridges, cliffs, landmarks)
            m_map->renderDecorations(*m_renderer, worldCam, !disableFog, globalAnimTick);

            // Layer 3..4: Living Walls, Buildings, Units, FX
            bool isDebug = m_debugMode || (m_gameplayController && m_gameplayController->isDebugMode());
            m_entities->render(*m_renderer, worldCam, *m_map, !disableFog, globalAnimTick, isDebug, m_input->isKeyPressed(SDL_SCANCODE_LALT) || m_input->isKeyPressed(SDL_SCANCODE_RALT));

            // Layer 5: Fog of War Explored Shading
            if(!disableFog)
            {
                // Low graphics: the fog in plain tiles, without smoothed edges
                m_map->renderFog(*m_renderer, worldCam, m_settings.graphicsQuality > 0);
            }

            renderMapEdgeFade(worldCam);
            m_renderer->resetClipRect();
            m_renderer->endWorldPass();

            if(scene)
            {
                // Glow: what shines, again, into a quarter-size target, then blurred
                if(m_effects->glow())
                {
                    const Camera glowCam = m_camera->scaledDown(kGlowDivisor);
                    m_effects->beginGlow(glowCam.getViewportWidth(), glowCam.getViewportHeight());
                    m_entities->renderEmissive(*m_renderer, glowCam);
                    m_effects->endGlow();
                }
                SDL_SetRenderTarget(m_sdlRenderer, nullptr);
                const SDL_Rect clip {0, 0, m_camera->getViewportWidth(), m_camera->getViewportHeight()};
                SDL_SetRenderClipRect(m_sdlRenderer, &clip);
                const SDL_FRect dst {0.0f, 0.0f, static_cast<float>(worldCam.getViewportWidth() * divisor), static_cast<float>(worldCam.getViewportHeight() * divisor)};
                const Camera glowCam = m_camera->scaledDown(kGlowDivisor);
                const SDL_FRect glowDst {0.0f, 0.0f, static_cast<float>(glowCam.getViewportWidth() * kGlowDivisor), static_cast<float>(glowCam.getViewportHeight() * kGlowDivisor)};
                m_effects->composite(dst, glowDst, m_settings.smoothing);
                SDL_SetRenderClipRect(m_sdlRenderer, nullptr);
            }
            else if(divisor > 1 && m_lowResTarget)
            {
                SDL_SetRenderTarget(m_sdlRenderer, nullptr);
                const SDL_Rect clip {0, 0, m_camera->getViewportWidth(), m_camera->getViewportHeight()};
                SDL_SetRenderClipRect(m_sdlRenderer, &clip);
                const SDL_FRect dst {0.0f, 0.0f, static_cast<float>(m_lowResW * divisor), static_cast<float>(m_lowResH * divisor)};
                SDL_RenderTexture(m_sdlRenderer, m_lowResTarget, nullptr, &dst);
                SDL_SetRenderClipRect(m_sdlRenderer, nullptr);
            }

            // Render Debug Passability Grid (70% Transparency Green/Red)
            m_gameplayController->renderDebugGrid(*m_renderer, *m_camera, *m_entities, *m_map);

            // Order lines of the selected units (of every unit in sight while Alt is held)
            m_gameplayController->renderOrderLines(*m_renderer, *m_camera, *m_entities, *m_map, m_input->isKeyPressed(SDL_SCANCODE_LALT) || m_input->isKeyPressed(SDL_SCANCODE_RALT));

            // Render Debug Navigation (Paths, Waypoints, Combat/Wall Targets)
            m_gameplayController->renderDebugNavigation(*m_renderer, *m_camera, *m_entities, *m_map);

            // AI groups (G1..GN), leash lines and chokepoints
            if(isDebug)
            {
                m_aiController->renderDebug(*m_renderer, *m_fontRenderer, *m_camera, *m_entities);
            }

            // Leaders of the player's control groups
            m_gameplayController->renderGroupLeaders(*m_renderer, *m_camera, *m_entities);

            // Training progress above Barracks / Factories, rally lines of the selected ones
            m_hudController->renderProductionOverlay(*m_renderer, *m_fontRenderer, *m_entities, *m_map, *m_camera);
            // Not enough power: blinking lightnings over the buildings that stopped
            m_hudController->renderPowerAlerts(*m_renderer, *m_entities, *m_economy, *m_camera);

            // Render Selection Box
            if(!m_hudController->isBuildMode())
            {
                m_gameplayController->renderSelectionBox(*m_renderer, *m_camera, *m_input);
            }

            // Render Build Ghost in Placement Mode (only while the cursor is over the map)
            if(m_hudController->isBuildMode() && !HUDController::isOverSidebar(m_input->getMouseScreenX(), m_windowW))
            {
                int mouseTileX = static_cast<int>(m_input->getMouseWorldX() / TILE_WIDTH);
                int mouseTileY = static_cast<int>(m_input->getMouseWorldY() / TILE_HEIGHT);
                m_hudController->renderBuildGhost(*m_renderer, *m_map, *m_camera, mouseTileX, mouseTileY, *m_economy, *m_entities);
            }

            // Render Debug Spawn Ghost in Debug Spawn Mode
            if(m_gameplayController->isSpawnMode())
            {
                m_gameplayController->renderDebugSpawnGhost(*m_renderer, *m_camera, *m_input, *m_map, *m_fontRenderer);
            }

            // Render Attack-Move Reticle in Attack-Move Mode ('Q' key)
            if(m_hudController->isAttackMoveMode() && !HUDController::isOverSidebar(m_input->getMouseScreenX(), m_windowW))
            {
                int mx = m_input->getMouseScreenX();
                int my = m_input->getMouseScreenY();

                // Crosshair
                m_renderer->drawRectScreen(mx - 14, my - 1, 29, 3, {255, 60, 60, 255});
                m_renderer->drawRectScreen(mx - 1, my - 14, 3, 29, {255, 60, 60, 255});
                m_renderer->drawRectScreen(mx - 9, my - 9, 19, 19, {255, 220, 0, 255});

                // Floating tooltip, kept inside the map viewport
                std::string tip = _("ATTACK-MOVE [Q]: select target (LMB to confirm, RMB/ESC to cancel)");
                int tipW = m_fontRenderer->getTextWidth(tip, ui::FONT_SMALL) + 16;
                int tipX = std::min(mx + 16, m_camera->getViewportWidth() - tipW - 4);
                int tipY = std::max(4, my - 30);
                m_renderer->fillRectScreen(tipX, tipY, tipW, 24, ui::color::Panel);
                m_renderer->drawRectScreen(tipX, tipY, tipW, 24, ui::color::Danger);
                m_fontRenderer->drawString(tip, tipX + 8, tipY + 12, ui::FONT_SMALL, ui::color::Text, false, Anchor::LEFT | Anchor::VCENTER);
            }

            // Control groups (Ctrl+1..0) along the bottom of the map view
            m_gameplayController->renderGroupBar(*m_renderer, *m_fontRenderer, *m_entities, m_camera->getViewportWidth(), m_windowH, m_input->getMouseScreenX(), m_input->getMouseScreenY());

            // Command sidebar (right edge)
            m_hudController->setRadarFog(!disableFog);
            m_hudController->renderGameHUD(*m_renderer, *m_fontRenderer, *m_economy, *m_entities, *m_map, *m_camera, m_input->getMouseScreenX(), m_input->getMouseScreenY(), m_windowW, m_windowH, m_gameplayController->getSelectedUnits());

            // Top Debug Toolbar (--debug mode)
            m_gameplayController->renderDebugToolbar(*m_renderer, *m_fontRenderer, m_windowW);
            m_gameplayController->renderDebugSpawnDropdown(*m_renderer, *m_fontRenderer, m_windowH);

            if(m_menuController->isObjectivesDialogOpen())
            {
                m_menuController->renderObjectivesDialog(*m_renderer, *m_fontRenderer, *m_mission, m_windowW, m_windowH);
            }
            else if(m_menuController->isInGameMenuOpen() && m_menuController->isInGameSettingsOpen())
            {
                m_menuController->renderMenuSettings(*m_renderer, *m_fontRenderer, m_windowW, m_windowH);
            }
            else if(m_menuController->isInGameMenuOpen() || m_state == AppState::Paused)
            {
                m_menuController->renderInGamePauseMenu(*m_renderer, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
            }
            else if(m_state == AppState::Victory || m_state == AppState::Defeat)
            {
                bool won = (m_state == AppState::Victory);
                int viewW = m_camera->getViewportWidth();
                m_renderer->fillRectScreen(0, 0, viewW, m_windowH, ui::color::Scrim);
                ui::UiRect band {0, m_windowH / 2 - 70, viewW, 140};
                ui::fillHorizontalGradient(*m_renderer, {band.x, band.y, band.w / 2, band.h}, ui::withAlpha(ui::color::Panel, 0), ui::color::Panel);
                ui::fillHorizontalGradient(*m_renderer, {band.centerX(), band.y, band.w - band.w / 2, band.h}, ui::color::Panel, ui::withAlpha(ui::color::Panel, 0));
                SDL_Color accent = won ? ui::color::Ok : ui::color::Danger;
                m_renderer->fillRectScreen(band.centerX() - 120, band.y, 240, 2, accent);
                m_renderer->fillRectScreen(band.centerX() - 120, band.bottom() - 2, 240, 2, accent);
                m_fontRenderer->drawString(won ? _("MISSION ACCOMPLISHED!") : _("MISSION FAILED!"), band.centerX(), band.centerY() - 14, ui::FONT_HERO, accent, true, Anchor::HCENTER | Anchor::VCENTER);
                m_fontRenderer->drawString(_("Click or press Enter to return to the main menu"), band.centerX(), band.centerY() + 38, ui::FONT_BODY, ui::color::TextDim, false, Anchor::HCENTER | Anchor::VCENTER);
            }
        }

        renderNotices();
        renderScoreboard();
        renderFpsCounter();
        renderNetStats();
        renderNetworkBanner();
        if(m_videoConfirmActive)
            renderVideoConfirm();

#if defined(AOW_DEBUG)
        // Debug Overlay (Dear ImGui)
        if(m_debug)
        {
            m_debug->render(*m_map, *m_entities, *m_economy, *m_mission, *m_sound, *m_camera, *m_aiController, m_fps);
        }
#endif

        m_renderer->present();
        if(m_renderFirstFrame)
        {
            m_renderFirstFrame = false;
            RenderBackend::confirmFirstFrame(m_env);
        }
    }

} // namespace OpenArtOfWar
