#include "core/Application.hpp"
#include "core/Localization.hpp"
#include "core/AssetStore.hpp"
#include "ui/MenuController.hpp"
#include "ui/HUDController.hpp"
#include "ui/UiKit.hpp"
#include "game/GameplayController.hpp"
#include "game/Sides.hpp"
#include "game/Players.hpp"
#include "test/HeadlessTest.hpp"
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

        m_env.load();

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
        Localization::init("assets/locale", "en");
        Localization::setLanguage(m_settings.language);
        {
            std::string uiText;
            if(Assets::readText("assets/characters/game_ui.yaml", uiText))
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
            m_fontRenderer = std::make_unique<BitmapFontRenderer>(m_sdlRenderer, f.value("regular", std::string("assets/fonts/Roboto-Regular.ttf")), f.value("bold", std::string("assets/fonts/Roboto-Bold.ttf")), sizes);
        }
        showLoading(_("Starting the game..."), 0.05f);

        // Initialize core subsystems
        showLoading(_("Loading graphics..."), 0.12f);
        m_texManager->loadAllAssets("assets");
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
        m_sound->init("assets");
        applyAudio();

        m_map = std::make_unique<TileMap>();
        m_map->loadEnvironment(m_env);
        m_pathfinder = std::make_unique<Pathfinder>(m_map.get());
        showLoading(_("Loading units and buildings..."), 0.8f);
        m_entities = std::make_unique<EntityManager>();
        m_entities->loadCharacterConfigs("assets");
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
        m_mission->loadTestMap("assets", *m_map, *m_entities, *m_economy, *m_pathfinder);
        m_state = AppState::CampaignPlaying;
        m_camera->setZoom(m_settings.zoomDefault);
        m_camera->centerOn((m_mission->getPlayerHqX() + 12) * TILE_WIDTH, m_mission->getPlayerHqY() * TILE_HEIGHT);
        std::cout << "[Application] Standalone Test Map (maps/testmap.json) launched directly via --testmap flag!" << std::endl;
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
            updateNetwork(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(currentTime.time_since_epoch()).count()));

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
                        static const long dropAt = SDL_getenv("OAOW_NET_DROP_AT") ? std::atol(SDL_getenv("OAOW_NET_DROP_AT")) : -1;
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
            return SDL_Color {15, 18, 24, static_cast<Uint8>(215.0f * t * t)};
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
        m_confirmedVideo = m_settings;
        saveSettings();
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
        static const bool bot = std::getenv("OAOW_NET_BOT") != nullptr;
        if(!bot || tick % 100 != 10)
            return;
        const PlayerId me = Players::local();
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
        // The list every 10 seconds while the tab is open
        if(m_serverListMs == 0 || now - m_serverListMs > 10000)
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

    bool Application::inNetworkMatch() const
    {
        return m_net && m_netMatchStarted && m_mission && m_mission->isMatch();
    }

    void Application::hostNetworkGame(uint16_t port, int players, const std::string &mapPath, const std::string &name)
    {
        m_net = std::make_unique<NetMatch>();
        std::string error;
        if(!m_net->host(port, name, error))
        {
            std::cerr << "[Net] Cannot host on port " << port << ": " << error << std::endl;
            m_net.reset();
            return;
        }
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

    void Application::serverNetworkGame(const std::string &server, uint16_t port, const std::string &code, int players, const std::string &mapPath, const std::string &name)
    {
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
                    if(s.slot == m_net->localSlot() && !s.choice.ready && m_net->mapReady())
                    {
                        Net::LobbyChoice ready = s.choice;
                        ready.ready = true;
                        m_net->setChoice(ready);
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
            // A fresh simulation now, before the first turn's commands are scheduled into it
            m_gameplayController->resetForMission(*m_mission, *m_hudController, *m_entities, *m_map);
            m_net->attach(m_gameplayController->simulation());
            m_net->setReconnectWindow(static_cast<uint32_t>(m_env.section("network").value("reconnect_window_s", 60)) * 1000u);
            m_state = AppState::SkirmishPlaying;
            m_camera->setZoom(m_settings.zoomDefault);
            m_camera->centerOn(m_mission->getPlayerHqX() * TILE_WIDTH, m_mission->getPlayerHqY() * TILE_HEIGHT);
            endMapLoading(); // not before three seconds of the loading screen are over
            std::cout << "[Net] Match started, local slot " << int(setup.local) << std::endl;
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

    bool Application::runHeadlessTest(int numTicks)
    {
        return HeadlessTest::run(*this, numTicks);
    }

    void Application::handleEvents()
    {
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
            if(event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && event.window.windowID == SDL_GetWindowID(m_window))
            {
                m_windowW = event.window.data1;
                m_windowH = event.window.data2;
                m_camera->setViewport(m_windowW - HUDController::kSidebarWidth, m_windowH);
            }

            m_input->processEvent(event, *m_camera);
        }

        if(m_gameplayController && m_gameplayController->consumeDebugOverlayToggle())
        {
            m_debug->toggle();
        }
        // Refresh mouse position every frame so menus get hover feedback too
        m_input->update(0.0f, *m_camera);
    }

    void Application::update(float deltaTime)
    {
        m_globalAnimTimer += deltaTime;

        // Enter is consumed only together with Alt; a plain Enter belongs to the menus
        if(m_input->isKeyJustPressed(SDL_SCANCODE_F11) || ((SDL_GetModState() & SDL_KMOD_ALT) && m_input->isKeyJustPressed(SDL_SCANCODE_RETURN)))
        {
            toggleFullscreen();
        }
        if(m_videoConfirmActive)
        {
            updateVideoConfirm(deltaTime);
            return;
        }

        // Standalone Test Map (-1) always runs with DEBUG mode active
        if(m_mission && m_mission->getCurrentMissionId() == -1 && (!m_debugMode || (m_gameplayController && !m_gameplayController->isDebugMode())))
        {
            setDebugMode(true);
        }

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

    void Application::render()
    {
        m_renderer->clear({15, 18, 24, 255});

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
                m_renderer->clear({15, 18, 24, 255});
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
                    m_renderer->clear({15, 18, 24, 255});
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
            bool disableFog = (m_debug && m_debug->isFogDisabled()) || m_gameplayController->isRevealFog() || !m_map->isFogEnabled();
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

        renderFpsCounter();
        renderNetworkBanner();
        if(m_videoConfirmActive)
            renderVideoConfirm();

        // Debug Overlay (Dear ImGui)
        if(m_debug)
        {
            m_debug->render(*m_map, *m_entities, *m_economy, *m_mission, *m_sound, *m_camera, *m_aiController, m_fps);
        }

        m_renderer->present();
        if(m_renderFirstFrame)
        {
            m_renderFirstFrame = false;
            RenderBackend::confirmFirstFrame(m_env);
        }
    }

} // namespace OpenArtOfWar
