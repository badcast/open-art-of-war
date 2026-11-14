#include "core/Application.hpp"
#include "net/ReplayRecorder.hpp"
#include "core/Localization.hpp"
#include "core/AssetStore.hpp"
#include "core/EnvironmentConfig.hpp"
#include "core/LoaderResources.hpp"
#include "core/Mods.hpp"
#include "core/DebugEnv.hpp"
#include "ui/MenuController.hpp"
#include "ui/AboutSimulation.hpp"
#include "ui/TouchHUD.hpp"
#include "core/TouchPlatform.hpp"
#include "ui/UiLayout.hpp"
#include "ui/HUDController.hpp"
#include "ui/UiKit.hpp"
#include "ui/ImGuiBridge.hpp"
#include "game/GameplayController.hpp"
#include "game/Sides.hpp"
#include "game/Players.hpp"
#if defined(AOW_DEBUG)
#endif
#include "graphics/TextureManager.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/ShaderEffects.hpp"
#include "graphics/NightLighting.hpp"
#include "net/NetMatch.hpp"
#include "net/WorldSnapshot.hpp"
#include "ui/NetworkMenu.hpp"
#include "openaow/net/Discovery.hpp"
#include "openaow/net/MasterClient.hpp"
#include "game/Match.hpp"
#include "game/Research.hpp"
#include "game/Simulation.hpp"
#include "graphics/Camera.hpp"
#include "graphics/CameraShake.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "input/InputManager.hpp"
#include "audio/SoundEngine.hpp"
#include "audio/GameSounds.hpp"
#include "audio/SoundRequests.hpp"
#include "replay/ReplayPlayer.hpp"
#include "ui/ReplayMenu.hpp"
#include "ui/ModsMenu.hpp"
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

namespace
{
bool SDLCALL AppLifecycleEventWatch(void *userdata, SDL_Event *event)
{
    if (userdata && event)
        static_cast<Application *>(userdata)->OnLifecycleEvent(static_cast<int>(event->type));
    return true;
}
} // namespace

namespace
{
// OAOW_NET_TRACE=1: every 20th tick the hash of a snapshot, to compare the authority with a replica by eye or script
void SnapshotTrace(const char *who, const WorldSnapshot &snap)
{
    static const bool trace = DebugEnv("OAOW_NET_TRACE") != nullptr;
    if (!trace || snap.tick % 20 != 0)
        return;
    std::uint64_t h = 1469598103934665603ull;
    // Entities and economies only: the replica's own capture carries no shots or events
    WorldSnapshot plain = snap;
    plain.hasWalls = false;
    plain.walls.clear();
    plain.projectiles.clear();
    plain.cues.clear();
    plain.blasts.clear();
    plain.explosions.clear();
    const std::vector<std::uint8_t> bytes = plain.Encode();
    for (std::uint8_t b : bytes)
        h = (h ^ b) * 1099511628211ull;
    std::printf(
        "[Snap] %s tick %u entities %zu bytes %zu hash %016llx\n",
        who,
        snap.tick,
        snap.entities.size(),
        bytes.size(),
        static_cast<unsigned long long>(h));
    std::fflush(stdout);
}
} // namespace

Application::Application() {}

Application::~Application()
{
    Shutdown();
}

bool Application::Init(int windowWidth, int windowHeight)
{
    m_windowW = windowWidth;
    m_windowH = windowHeight;

    if (!Assets::Init())
    {
        constexpr const char *errorMessage
            = "Required game data was not found. Place the 'assets' directory or 'assets.blob' in the game folder.";
        std::cerr << "[Application] " << errorMessage << std::endl;
        if (!m_headless && SDL_Init(SDL_INIT_VIDEO))
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game data not found", errorMessage, nullptr);
            if (m_lifecycleWatchAdded)
    {
        SDL_RemoveEventWatch(&AppLifecycleEventWatch, this);
        m_lifecycleWatchAdded = false;
    }

    SDL_Quit();
        }
        return false;
    }
    std::cout << "[Application] Assets: " << Assets::Describe() << std::endl;
    // A blob must match the integrity certificate it carries (certificate.sha256, written into it by
    // tools/pack_assets.py): a missing certificate or a changed, missing or extra file stops the game before anything
    // is loaded
    {
        std::vector<std::string> problems;
        std::string certificate;
        if (!Assets::VerifyCertificate(problems, certificate))
        {
            std::string message = "The game data failed its integrity check (" + certificate + ", "
                                  + std::to_string(problems.size()) + " problem(s)):\n";
            for (std::size_t i = 0; i < problems.size() && i < 15; ++i)
                message += "\n" + problems[i];
            if (problems.size() > 15)
                message += "\n... and " + std::to_string(problems.size() - 15) + " more";
            message += "\n\nReinstall the game data or pack it again (tools/pack_assets.py).";
            std::cerr << "[Application] " << message << std::endl;
            for (std::size_t i = 15; i < problems.size(); ++i)
                std::cerr << "  " << problems[i] << std::endl;
            if (!m_headless && SDL_Init(SDL_INIT_VIDEO))
            {
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game data check failed", message.c_str(), nullptr);
                if (m_lifecycleWatchAdded)
    {
        SDL_RemoveEventWatch(&AppLifecycleEventWatch, this);
        m_lifecycleWatchAdded = false;
    }

    SDL_Quit();
            }
            return false;
        }
    }
    // Every resource path comes from assets/main.yaml from here on
    // main.yaml names every data file: without it the game does not know what to load
    if (!LoaderResources::Init())
    {
        const std::string message
            = std::string(LoaderResources::kManifestPath)
              + " is missing or broken: it lists every game data file.\n\nReinstall the game data.";
        std::cerr << "[Application] " << message << std::endl;
        if (!m_headless && SDL_Init(SDL_INIT_VIDEO))
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game data check failed", message.c_str(), nullptr);
            if (m_lifecycleWatchAdded)
    {
        SDL_RemoveEventWatch(&AppLifecycleEventWatch, this);
        m_lifecycleWatchAdded = false;
    }

    SDL_Quit();
        }
        return false;
    }

    // Options and progress live in the per-user directory (Assets::userDataDir)
    m_saveManager = std::make_unique<SaveManager>();
    m_settings.ApplyFactoryDefaults(EnvironmentConfig::FactorySettings()); // game_environment.yaml, "settings"
    m_settings.windowW = windowWidth;
    m_settings.windowH = windowHeight;
    if (!m_settings.load(Settings::DefaultPath()))
    {
        // First run with this version: take over what the old save file knew
        SaveData old;
        if (m_saveManager->load(old))
        {
            m_settings.language = old.language;
            m_settings.difficulty = old.difficulty;
            m_settings.muted = !old.soundEnabled;
        }
        m_settings.Clamp();
    }
    m_confirmedVideo = m_settings;
    // The player's mods over the installed data, before anything is loaded from it
    if (!m_settings.enabledMods.empty())
    {
        Mods::Activate(m_settings.enabledMods);
        LoaderResources::Reload();
    }

#if OAOW_TOUCH
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    TouchPlatform::ConfigureBackButton();
    m_settings.windowMode = WindowMode::Borderless;
    m_confirmedVideo = m_settings;
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS))
    {
        std::cerr << "[Application] SDL_Init failed: " << SDL_GetError() << std::endl;
        return false;
    }
    m_lifecycleWatchAdded = SDL_AddEventWatch(&AppLifecycleEventWatch, this);

    m_env.load(LoaderResources::environment());

    // Hidden until a renderer is up: trying one API after the other must not flicker on the screen
    m_window = SDL_CreateWindow(
        "Open Art Of War: Resistance Arisen",
        m_settings.windowW,
        m_settings.windowH,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!m_window)
    {
        std::cerr << "[Application] SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        return false;
    }

    m_sdlRenderer = m_headless ? SDL_CreateRenderer(m_window, nullptr)
                               : RenderBackend::Create(m_window, m_env, m_rendererOverride);
    if (!m_sdlRenderer)
    {
        std::cerr << "[Application] SDL_CreateRenderer failed: " << SDL_GetError() << std::endl;
        return false;
    }
    m_renderFirstFrame = !m_headless;
    SDL_ShowWindow(m_window);
    VideoModes::Apply(m_window, m_sdlRenderer, m_settings); // fullscreen mode, vsync
    {
        const Resolution size = VideoModes::CurrentResolution(m_window);
        if (size.w > 0 && size.h > 0)
        {
            m_windowW = size.w;
            m_windowH = size.h;
        }
    }
#if OAOW_TOUCH
    SyncWindowSize();
#endif
    m_resolutions = VideoModes::AvailableResolutions(m_window);

    // The language and the font first: the loading screen is up before the heavy part of the start-up
    Localization::Init(LoaderResources::LocaleDir(), LoaderResources::DefaultLanguage(), "openaow");
    Localization::SetLanguage(m_settings.language);
    {
        std::string uiText;
        if (Assets::ReadText(LoaderResources::UiTheme(), uiText))
            ui::LoadTheme(uiText);
    }
    m_resourceManager = std::make_unique<ResourceManager>();
    m_texManager = std::make_unique<TextureManager>(m_sdlRenderer);
    m_texManager->BindResourceManager(m_resourceManager.get());
    m_loadingPreview.BindResourceManager(m_resourceManager.get(), "loading_preview");
    m_renderer = std::make_unique<Renderer>(m_sdlRenderer, m_texManager.get());
    {
        // Font files from assets/main.yaml, their sizes from section "font" of game_environment.yaml
        const nlohmann::json &f = m_env.Section("font");
        FontSizes sizes;
        sizes.small = f.value("size_small", sizes.small);
        sizes.medium = f.value("size_medium", sizes.medium);
        sizes.large = f.value("size_large", sizes.large);
        sizes.title = f.value("size_title", sizes.title);
        sizes.hero = f.value("size_hero", sizes.hero);
        m_fontRenderer = std::make_unique<BitmapFontRenderer>(
            m_sdlRenderer, LoaderResources::FontRegular(), LoaderResources::FontBold(), sizes);
    }
    ShowLoading(_("Starting the game..."), 0.05f);

    // Initialize core subsystems
    ShowLoading(_("Loading graphics..."), 0.12f);
    m_texManager->LoadAllAssets();
    m_texManager->PinUiCommon();
    ShowLoading(_("Preparing the renderer..."), 0.55f);
    m_effects = std::make_unique<ShaderEffects>();
    if (!m_headless)
        m_effects->Init(m_sdlRenderer);
    {
        NightConfig night;
        night.Load(LoaderResources::Nights());
        m_nightLighting = std::make_unique<NightLighting>();
        m_nightLighting->Configure(night);
    }
    m_renderer->SetEffects(m_effects.get());
    // The map viewport ends where the command sidebar begins
    m_camera = std::make_unique<Camera>(m_windowW - HudReservedWidth(), m_windowH);
    m_input = std::make_unique<InputManager>();
#if OAOW_TOUCH
    m_input->SetRenderer(m_sdlRenderer);
#endif
    m_input->SetEdgePanMargin(m_settings.edgePanMargin);
    ShowLoading(_("Loading sound and music..."), 0.65f);
    m_sound = std::make_unique<SoundEngine>();
    m_sound->Init();
    ApplyAudio();

    m_map = std::make_unique<TileMap>();
    m_map->LoadEnvironment(m_env);
    m_pathfinder = std::make_unique<Pathfinder>(m_map.get());
    ShowLoading(_("Loading units and buildings..."), 0.8f);
    m_entities = std::make_unique<EntityManager>();
    m_entities->LoadCharacterConfigs();
    m_entities->SetTextureManager(m_texManager.get());
    m_pathfinder->SetEntityManager(m_entities.get());
    m_economy = std::make_unique<Economy>();
    m_aiController = std::make_unique<AIController>();
    m_mission = std::make_unique<MissionController>();
    m_mission->SetOnLoaded([this] {
        m_pathfinder->SetMap(m_map.get());
        m_pathfinder->SetEntityManager(m_entities.get());
        if (m_aiController && m_mission
            && (!m_gameplayController || m_gameplayController->simulation().AdvancesWorld()))
        {
            m_aiController->Init(
                m_mission->GetEnemyHqX(),
                m_mission->GetEnemyHqY(),
                m_mission->GetPlayerHqX(),
                m_mission->GetPlayerHqY());
            m_aiController->RecordStartingBase(*m_entities, *m_map);
            m_aiController->SetSkirmish(m_mission->IsSkirmish());
            const AITacticsConfig &t = m_aiController->GetDifficultyConfig().tactics;
            m_mission->SetDefensePacing(
                t.defenseFirstWaveSeconds, t.defenseWaveGapSeconds, t.defenseSquadPercent, t.defenseTimeScale);
        }
    });

    ImGuiBridge::Init(m_window, m_sdlRenderer);
#if defined(AOW_DEBUG)
    m_debug = std::make_unique<DebugOverlay>(m_window, m_sdlRenderer);
#endif
    // Initialize modular controllers
    m_menuController = std::make_unique<MenuController>();
    m_networkMenu = std::make_unique<NetworkMenu>();
    m_replayMenu = std::make_unique<ReplayMenu>();
    m_modsMenu = std::make_unique<ModsMenu>();
    if (m_touchUi)
        m_hudController = std::make_unique<TouchHUD>();
    else
        m_hudController = std::make_unique<HUDController>();
    m_hudController->SetRadarAlertConfig(m_env.Section("radar_alert"));
    m_hudController->SetTacticalConfig(m_env.Section("tactical_view"));
    HUDController::SetBuildGridConfig(m_env.Section("build_grid"));
    m_cameraShake.Configure(m_env.Section("camera_shake"));
    HUDController::SetShowBuildGrid(m_settings.showBuildGrid);
    m_gameplayController = std::make_unique<GameplayController>();
    m_gameplayController->SetTouchMode(m_touchUi);
    uilayout::SetMenuProfile(m_touchUi ? "touch" : "pc");
    m_gameplayController->LoadOverlayStyle(m_env);
    m_hudController->SetResearchProbe([this](PlayerId p) { return m_gameplayController->ResearchOf(p); });
    m_menuController->BindLoading(
        [this](std::string_view title, std::string_view path) { BeginMapLoading(title, path); },
        [this]() { EndMapLoading(); });

    // Difficulty from the options, zoom range and graphics quality
    ShowLoading(_("Preparing the interface..."), 0.92f);
    m_aiController->SetDifficulty(static_cast<AIDifficulty>(m_settings.difficulty));
    ApplyGraphics();
    m_menuController->BindSettings(
        m_settings,
        [this](SettingsArea area) { ApplySettings(area); },
        [this]() -> const std::vector<Resolution> & { return m_resolutions; });
    if (!m_headless)
    {
        RenderApiBinding api;
        api.options = [this]() -> const std::vector<RenderApiOption> & {
            if (m_renderOptions.empty())
                m_renderOptions = RenderBackend::options(m_env, true);
            return m_renderOptions;
        };
        api.active = [this]() {
            const char *n = SDL_GetRendererName(m_sdlRenderer);
            return std::string(n ? n : "");
        };
        api.configured = [this]() {
            return RenderBackend::Configured(m_env);
        };
        api.select = [this](std::string_view name) {
            RenderBackend::select(m_env, name);
        };
        api.applyNow = [this]() {
            SwitchRenderApi();
        };
        api.effectsAvailable = [this]() {
            return m_effects && m_effects->Available();
        };
        m_menuController->BindRenderApi(std::move(api));
    }

    ShowLoading(_("Ready"), 1.0f);
    m_state = AppState::SplashLogo;
    m_splashTimer = 0.0f;

    // Back from a window rebuild for another render API: straight to the graphics settings, keep it or go back
    const std::string previousApi = m_env.Section("render").value("confirm_from", std::string());
    if (!m_headless && !previousApi.empty())
    {
        const char *active = SDL_GetRendererName(m_sdlRenderer);
        if (active && previousApi != active)
        {
            m_renderConfirm = true;
            m_videoConfirmActive = true;
            m_videoConfirmTimer = 15.0f;
        }
        else
        {
            m_env.Edit("render").erase("confirm_from"); // the new API did not come up, nothing to confirm
            m_env.Save();
        }
        m_state = AppState::MenuSettings;
        m_menuController->ShowSettingsTab(1);
    }

    m_running = true;
    std::cout << "[Application] Game engine initialized successfully!" << std::endl;
    return true;
}

void Application::Shutdown()
{
    FinishLocalReplay("left"); // a match of this computer alone being recorded is saved as it is
    // Quitting the game in a network match is leaving it for good: the others need not wait out the
    // reconnect window before this player's base goes
    if (m_net)
    {
        m_net->leave();
        m_net.reset();
    }
#if defined(AOW_DEBUG)
    if (m_debug)
        m_debug.reset();
#endif
    ImGuiBridge::Shutdown();
    if (m_effects)
        m_effects->Shutdown();
    m_nightLighting.reset();
    if (m_sound)
        m_sound->Cleanup();
    if (m_lowResTarget)
    {
        SDL_DestroyTexture(m_lowResTarget);
        m_lowResTarget = nullptr;
    }
    if (m_texManager)
    {
        m_texManager->Cleanup();
        m_texManager->BindResourceManager(nullptr);
    }
    m_loadingPreview.Clear();
    m_loadingPreview.BindResourceManager(nullptr);
    m_resourceManager.reset();

    if (m_sdlRenderer)
    {
        SDL_DestroyRenderer(m_sdlRenderer);
        m_sdlRenderer = nullptr;
    }
    if (m_window)
    {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    if (m_lifecycleWatchAdded)
    {
        SDL_RemoveEventWatch(&AppLifecycleEventWatch, this);
        m_lifecycleWatchAdded = false;
    }

    SDL_Quit();
    m_running = false;
}

void Application::OnLifecycleEvent(int eventType)
{
    switch (eventType)
    {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_DID_ENTER_BACKGROUND:
            m_inBackground.store(true);
            break;
        case SDL_EVENT_WILL_ENTER_FOREGROUND:
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            m_inBackground.store(false);
            m_renderTargetsLost.store(true);
            break;
        case SDL_EVENT_TERMINATING:
            m_running = false;
            break;
        case SDL_EVENT_RENDER_TARGETS_RESET:
            m_renderTargetsLost.store(true);
            break;
        case SDL_EVENT_RENDER_DEVICE_RESET:
            m_renderDeviceLost.store(true);
            break;
        default:
            break;
    }
}

void Application::ResetRenderTargets(bool deviceLost)
{
    if (m_lowResTarget)
    {
        SDL_DestroyTexture(m_lowResTarget);
        m_lowResTarget = nullptr;
    }
    m_lowResW = 0;
    m_lowResH = 0;
    if (m_renderer)
        m_renderer->ResetRenderTargets();
    if (m_nightLighting)
        m_nightLighting->Shutdown();
    if (m_effects)
        m_effects->ResetTargets();
    if (deviceLost)
    {
        if (m_texManager)
        {
            m_texManager->ReloadSprites();
            m_texManager->PinUiCommon();
        }
        m_loadingPreview.Clear();
    }
}

void Application::SetDebugMode(bool debug)
{
#if !defined(AOW_DEBUG)
    debug = false; // cheats, the debug toolbar and full enemy control exist only in a debug build
#endif
    m_debugMode = debug;
    if (m_gameplayController)
    {
        m_gameplayController->SetDebugMode(debug);
    }
}

void Application::LaunchTestMap()
{
    if (!m_mission || !m_map || !m_entities || !m_economy || !m_pathfinder || !m_camera || !m_sound)
    {
        return;
    }
    SetDebugMode(true);
    m_mission->LoadTestMap(*m_map, *m_entities, *m_economy, *m_pathfinder);
    m_state = AppState::CampaignPlaying;
    m_camera->SetZoom(m_settings.zoomDefault);
    m_camera->CenterOn((m_mission->GetPlayerHqX() + 12) * TILE_WIDTH, m_mission->GetPlayerHqY() * TILE_HEIGHT);
    std::cout << "[Application] Standalone Test Map (maps/testmap.json) launched directly via --testmap flag!"
              << std::endl;
}

bool Application::LaunchMap(std::string_view nameOrPath, bool fromScratch)
{
    if (!m_mission || !m_map || !m_entities || !m_economy || !m_pathfinder || !m_camera)
        return false;
    const std::string path = LoaderResources::MapPath(nameOrPath);
    const std::vector<SkirmishMapInfo> maps = MissionController::ListSkirmishMaps();
    const SkirmishMapInfo *info = nullptr;
    for (const SkirmishMapInfo &m : maps)
    {
        if (m.path == path || m.name == nameOrPath)
            info = &m;
    }
    if (!info && !std::filesystem::exists(path))
    {
        std::cerr << "[Application] --map: no map \"" << nameOrPath << "\". Maps:";
        for (const SkirmishMapInfo &m : maps)
            std::cerr << " " << m.name;
        std::cerr << std::endl;
        return false;
    }
    const std::string mapFile = info ? info->path : path;
    BeginMapLoading(info ? info->DisplayName() : std::filesystem::path(mapFile).stem().string(), mapFile);
    const bool started = m_mission->LoadSkirmish(mapFile, *m_map, *m_entities, *m_economy, *m_pathfinder, !fromScratch);
    EndMapLoading();
    if (!started)
    {
        std::cerr << "[Application] --map: " << mapFile << " could not be loaded" << std::endl;
        return false;
    }
    m_state = AppState::Briefing; // as from the menu: the briefing, then the game
    std::cout << "[Application] Custom game on " << mapFile << " launched via --map"
              << (fromScratch ? " (from scratch)" : "") << std::endl;
    return true;
}

void Application::UpdateRenderAlpha(double accumulator, double frameTime)
{
    if (!m_entities || !m_gameplayController)
        return;
    const bool inWorld = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying
                         || m_state == AppState::Paused || m_state == AppState::Victory || m_state == AppState::Defeat;
    if (!inWorld)
    {
        m_entities->SetRenderAlpha(1.0f);
        m_alphaLast = 1.0f;
        return;
    }
    if (InNetworkMatch() && m_net->IsReplica())
    {
        // Snapshots come every second tick: glide from the last picture to the new one over that time
        m_alphaLast = static_cast<float>(std::clamp(m_replicaSince / (2.0 * SIMULATION_TIMESTEP), 0.0, 1.0));
        m_entities->SetRenderAlpha(m_alphaLast);
        return;
    }
    const std::uint32_t tick = m_gameplayController->simulation().Tick();
    const bool ticked = tick != m_alphaTick;
    if (ticked)
    {
        m_alphaTick = tick;
        m_alphaSince = 0.0;
    }
    else
    {
        m_alphaSince += frameTime;
    }
    // Local clocks already contain scaled simulation time. Mixing in wall time makes
    // slow playback reach the next position too early, then stall until the next tick.
    // Replays and spectators own their accumulator separately from the frame loop.
    const double phase = (m_replay || m_spectating)
                             ? m_replayUi.accumulator
                             : (InNetworkMatch() ? std::max(accumulator, m_alphaSince) : accumulator);
    double alpha = phase / SIMULATION_TIMESTEP;
    if (!ticked)
        alpha = std::max(alpha, static_cast<double>(m_alphaLast));
    m_alphaLast = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    m_entities->SetRenderAlpha(m_alphaLast);
}

void Application::Run()
{
    using clock = std::chrono::high_resolution_clock;
    auto prevTime = clock::now();
    double accumulator = 0.0;
    int frameCount = 0;
    auto fpsTimer = prevTime;

    while (m_running)
    {
        auto currentTime = clock::now();
        std::chrono::duration<double> elapsed = currentTime - prevTime;
        prevTime = currentTime;

        double frameTime = elapsed.count();
        if (frameTime > 0.25)
            frameTime = 0.25; // Clamp spiral of death
        accumulator += frameTime;

        frameCount++;
        if (std::chrono::duration<double>(currentTime - fpsTimer).count() >= 1.0)
        {
            m_fps = static_cast<float>(frameCount);
            frameCount = 0;
            fpsTimer = currentTime;
        }

        HandleEvents();
        if (m_inBackground.load())
        {
            UpdateNetwork(static_cast<std::uint32_t>(SDL_GetTicks()));
            SDL_Delay(50);
            prevTime = clock::now();
            continue;
        }
        if (m_renderDeviceLost.exchange(false))
        {
            m_renderTargetsLost.store(false);
            ResetRenderTargets(true);
        }
        else if (m_renderTargetsLost.exchange(false))
        {
            ResetRenderTargets(false);
        }
        if (m_resourceManager)
            m_resourceManager->Tick(SDL_GetTicks());
        // One clock for all network timing: SDL ticks, the same the lobby's Start button uses (epoch
        // milliseconds here made a host started from the menu close thousands of turns at once)
        UpdateNetwork(static_cast<std::uint32_t>(SDL_GetTicks()));
        UpdateGameChat(static_cast<std::uint32_t>(SDL_GetTicks()));

        // Simulation / Menu Update
        const bool activeGame = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying;
        const bool authorityContinues = (m_state == AppState::Defeat) && AuthorityMatchContinues();
        if (activeGame || authorityContinues)
        {
            const bool menuOpen
                = (m_menuController && m_menuController->IsAnyMenuOpen()) || m_state == AppState::Paused;
            if (m_input)
            {
                m_input->SetMenuOpen(menuOpen || !activeGame);
            }
            if (activeGame && m_map && !menuOpen)
            {
                m_input->UpdateCamera(
                    static_cast<float>(frameTime), *m_camera, m_map->GetWidth(), m_map->GetHeight(), menuOpen);
                m_hudController->UpdateRadarDrag(
                    m_input->GetMouseScreenX(),
                    m_input->GetMouseScreenY(),
                    m_input->IsLeftMouseDown(),
                    static_cast<float>(frameTime),
                    m_windowW,
                    m_windowH,
                    *m_map,
                    *m_camera);
            }
            if (m_replay)
            {
                // A recorded game: the turns of the file drive the world
                UpdateReplay(frameTime);
                accumulator = 0.0;
            }
            else if (m_spectating)
            {
                // A custom match only watched: at the speed of the observer bar
                UpdateSpectator(frameTime);
                accumulator = 0.0;
            }
            else
            {
                const bool net = InNetworkMatch();
                Simulation &sim = m_gameplayController->simulation();
                // P / Pause: everybody stops (a few times per player), anybody goes on
                if (net && activeGame
                    && (m_input->IsKeyJustPressed(SDL_SCANCODE_P) || m_input->IsKeyJustPressed(SDL_SCANCODE_PAUSE)))
                {
                    m_net->TogglePause();
                }
                const std::uint32_t nowMs = SDL_GetTicks();
                auto playing = [&] {
                    return m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying;
                };
                // One tick of a network match: only when its turn is here; checksums go out after it
                auto netTick = [&]() {
                    const std::uint32_t tick = sim.Tick();
                    if (!m_net->CanRunTick(tick))
                        return false;
                    if (playing())
                    {
                        DriveNetworkBot(tick);
                        Update(static_cast<float>(SIMULATION_TIMESTEP));
                    }
                    else
                    {
                        AppState simState = m_state;
                        m_gameplayController->SimulationStep(
                            static_cast<float>(SIMULATION_TIMESTEP),
                            *m_map,
                            *m_pathfinder,
                            *m_entities,
                            *m_economy,
                            *m_aiController,
                            *m_mission,
                            *m_hudController,
                            simState);
                    }
                    if (sim.Tick() != tick)
                    {
                        std::uint64_t checksum = 0;
                        if (tick % NetMatch::kHashIntervalTicks == 0)
                        {
                            SimContext ctx {*m_entities, *m_map, *m_pathfinder, *m_economy, *m_aiController};
                            std::uint64_t sections[Simulation::kChecksumSections];
                            checksum = sim.ChecksumSections(ctx, sections);
                            if (!m_net->Desynced())
                                NoteNetState(tick); // kept for a desync dump of this tick
                            static const bool trace = DebugEnv("OAOW_NET_TRACE") != nullptr;
                            if (trace)
                            {
                                std::printf("[Net] tick %u parts", tick);
                                for (int i = 0; i < Simulation::kChecksumSections; ++i)
                                    std::printf(
                                        " %s %016llx",
                                        Simulation::ChecksumSectionName(i),
                                        static_cast<unsigned long long>(sections[i]));
                                std::printf("\n");
                                std::fflush(stdout);
                            }
                        }
                        if (tick % NetMatch::kViewIntervalTicks == 0 && m_camera)
                        {
                            const float zoom = std::max(0.01f, m_camera->GetZoom());
                            m_net->SendView(
                                m_camera->GetX() + m_camera->GetViewportWidth() / zoom * 0.5f,
                                m_camera->GetY() + m_camera->GetViewportHeight() / zoom * 0.5f,
                                zoom);
                        }
                        m_net->AfterTick(tick, checksum);
                        // Send the terminal result even when it falls between the normal snapshot ticks.
                        const bool matchEnded = !playing() && !AuthorityMatchContinues();
                        if (m_net->AuthorityModeOn() && m_net->IsAuthority() && (tick % 2 == 0 || matchEnded))
                        {
                            SimContext ctx {*m_entities, *m_map, *m_pathfinder, *m_economy, *m_aiController};
                            WorldSnapshot snap = WorldSnapshot::CaptureEntities(*m_entities, tick);
                            snap.CaptureEffects(*m_entities);
                            snap.CaptureWalls(*m_map);
                            sim.FillEconomies(ctx, snap);
                            sim.FillResearch(snap);
                            m_net->PublishSnapshot(snap, *m_entities, *m_map);
                        }
                        m_net->NoteTickRan(nowMs);
                        // Tests (OAOW_NET_DROP_AT=<tick>): the connection drops once at that tick
                        static const long dropAt
                            = DebugEnv("OAOW_NET_DROP_AT") ? std::atol(DebugEnv("OAOW_NET_DROP_AT")) : -1;
                        static bool dropped = false;
                        if (!dropped && dropAt >= 0 && tick == static_cast<std::uint32_t>(dropAt))
                        {
                            dropped = true;
                            std::cerr << "[Net] Test: dropping the connection at tick " << tick << std::endl;
                            m_net->DropConnection();
                        }
                    }
                    return true;
                };
                if (net && !m_net->IsReplica() && m_net->TurnsAhead(sim.Tick()) > 10)
                {
                    // Far behind (back after a reconnect): play the turns through as fast as a frame allows
                    const auto deadline = clock::now() + std::chrono::milliseconds(25);
                    while (clock::now() < deadline && (playing() || AuthorityMatchContinues())
                           && m_net->TurnsAhead(sim.Tick()) > 2 && netTick())
                    {
                    }
                    accumulator = 0.0;
                }
                else if (net && m_net->IsReplica())
                {
                    // A replica never advances the world (state comes from the authority): commands go out, the UI
                    // lives
                    struct ReplicaSyncTracker
                    {
                        int missionSerial {-1};
                        bool researchReady {false};
                        std::uint32_t researchMask {0};
                        std::uint32_t seenBuildings {0};
                        std::uint32_t eliminated {0};
                        std::uint32_t lastViewBucket {0xFFFFFFFFu};
                    };
                    static ReplicaSyncTracker tracker;
                    const int missionSerial = m_mission ? m_mission->GetLoadSerial() : -1;
                    if (tracker.missionSerial != missionSerial)
                    {
                        tracker = ReplicaSyncTracker {};
                        tracker.missionSerial = missionSerial;
                    }

                    const bool paused = m_net->paused();
                    m_net->PumpReplica(nowMs);
                    if (!paused)
                    {
                        m_replicaSince += frameTime;
                        m_entities->UpdateVisualEffects(static_cast<float>(frameTime));
                    }
                    WorldSnapshot snap;
                    while (m_net->TakeSnapshot(snap))
                    {
                        SimContext ctx {*m_entities, *m_map, *m_pathfinder, *m_economy, *m_aiController};
                        if (!m_entities->ApplySnapshot(snap))
                            continue;
                        m_net->AckSnapshot(snap);
                        snap.ApplyWalls(*m_map);
                        // The authority decides how the match ends for this player
                        if (m_state == AppState::SkirmishPlaying && snap.result == 1)
                            m_state = AppState::Victory;
                        else if (m_state == AppState::SkirmishPlaying && snap.result == 2)
                            m_state = AppState::Defeat;
                        m_entities->UpdateFogOfWar(
                            *m_map); // nobody simulates here: the fog follows the replicated entities
                        sim.ApplyEconomies(snap, ctx);
                        sim.ApplyResearch(snap, ctx);

                        const PlayerId localPlayer = Players::Local();
                        if (localPlayer < kMaxPlayers)
                        {
                            std::uint32_t newResearchMask = 0;
                            for (int i = 0; i < kResearchCount; ++i)
                            {
                                if (IsResearched(localPlayer, i))
                                    newResearchMask |= (1u << i);
                            }
                            if (tracker.researchReady || snap.tick <= 2)
                            {
                                const std::uint32_t completedNow = newResearchMask & ~tracker.researchMask;
                                for (int i = 0; i < kResearchCount; ++i)
                                {
                                    if (completedNow & (1u << i))
                                        sim.NoteEvent({SimEvent::Type::ResearchCompleted, localPlayer, i});
                                }
                            }
                            tracker.researchMask = newResearchMask;
                            tracker.researchReady = true;
                        }
                        if (m_mission && m_mission->IsMatch())
                        {
                            std::uint32_t hasBuildings = 0;
                            for (const Entity &e : m_entities->GetAllEntities())
                            {
                                if (e.alive && e.state != ActionState::Die && e.isBuilding && e.owner < kMaxPlayers)
                                    hasBuildings |= (1u << e.owner);
                            }
                            tracker.seenBuildings |= hasBuildings;
                            const bool fogOn = m_map->IsFogEnabled();
                            for (PlayerId p = 0; p < kMaxPlayers; ++p)
                            {
                                if (!Players::IsActive(p) || (tracker.eliminated & (1u << p)))
                                    continue;
                                const bool trackable
                                    = !fogOn || (localPlayer < kMaxPlayers && !Players::Hostile(p, localPlayer));
                                if (trackable && (tracker.seenBuildings & (1u << p)) && !(hasBuildings & (1u << p)))
                                {
                                    tracker.eliminated |= (1u << p);
                                    sim.NoteEvent({SimEvent::Type::PlayerEliminated, p, 0});
                                }
                            }
                        }

                        if (DebugEnv("OAOW_NET_TRACE"))
                        {
                            // The replica's own capture of the world it just built: must equal what was sent
                            WorldSnapshot mine = WorldSnapshot::CaptureEntities(*m_entities, snap.tick);
                            sim.FillEconomies(ctx, mine);
                            sim.FillResearch(mine);
                            mine.research.erase(
                                std::remove_if(
                                    mine.research.begin(),
                                    mine.research.end(),
                                    [&](const ResearchSnap &r) {
                                        return std::none_of(
                                            snap.research.begin(), snap.research.end(), [&](const ResearchSnap &o) {
                                                return o.player == r.player;
                                            });
                                    }),
                                mine.research.end());
                            // Only what the authority sent: the replica also remembers things it can no longer see
                            mine.entities.erase(
                                std::remove_if(
                                    mine.entities.begin(),
                                    mine.entities.end(),
                                    [&](const EntitySnap &e) {
                                        return std::none_of(
                                            snap.entities.begin(), snap.entities.end(), [&](const EntitySnap &o) {
                                                return o.id == e.id;
                                            });
                                    }),
                                mine.entities.end());
                            mine.economies.erase(
                                std::remove_if(
                                    mine.economies.begin(),
                                    mine.economies.end(),
                                    [&](const EconomySnap &e) {
                                        return std::none_of(
                                            snap.economies.begin(), snap.economies.end(), [&](const EconomySnap &o) {
                                                return o.player == e.player;
                                            });
                                    }),
                                mine.economies.end());
                            SnapshotTrace("repl", mine);
                        }
                        const std::uint32_t viewBucket = snap.tick / NetMatch::kViewIntervalTicks;
                        if (m_camera && viewBucket != tracker.lastViewBucket)
                        {
                            tracker.lastViewBucket = viewBucket;
                            const float zoom = std::max(0.01f, m_camera->GetZoom());
                            m_net->SendView(
                                m_camera->GetX() + m_camera->GetViewportWidth() / zoom * 0.5f,
                                m_camera->GetY() + m_camera->GetViewportHeight() / zoom * 0.5f,
                                zoom);
                        }
                        m_lastReplicaTick = snap.tick;
                        m_replicaSince = 0.0;
                        m_net->NoteTickRan(nowMs);
                    }
                    if (!paused)
                        DriveNetworkBot(m_lastReplicaTick);
                    accumulator = 0.0;
                    m_gameplayController->SetSimulationHold(true);
                    Update(static_cast<float>(frameTime));
                    m_gameplayController->SetSimulationHold(false);
                }
                else if (net)
                {
                    // Slightly behind the host: an extra tick this frame
                    if (m_net->TurnsAhead(sim.Tick()) > 2)
                        accumulator += SIMULATION_TIMESTEP;
                    int ran = 0;
                    while (accumulator >= SIMULATION_TIMESTEP && (playing() || AuthorityMatchContinues()))
                    {
                        if (!netTick())
                        {
                            // Waiting for the host's turn: hold the time back instead of piling it up
                            accumulator = std::min(accumulator, SIMULATION_TIMESTEP);
                            break;
                        }
                        accumulator -= SIMULATION_TIMESTEP;
                        ++ran;
                    }
                    if (!playing() || ran == 0)
                    {
                        // No network turn ran this frame (or defeated host keeps ticking): keep UI responsive
                        m_gameplayController->SetSimulationHold(true);
                        Update(static_cast<float>(frameTime));
                        m_gameplayController->SetSimulationHold(false);
                    }
                }
                else
                {
                    const bool allowSpeed = IsSpeedControlAllowed();
                    if (!allowSpeed)
                        m_gameSpeed = 1;
                    if (allowSpeed && m_input && !m_textInput)
                    {
                        if (m_input->IsKeyJustPressed(SDL_SCANCODE_MINUS)
                            || m_input->IsKeyJustPressed(SDL_SCANCODE_KP_MINUS))
                            StepGameSpeed(-1);
                        if (m_input->IsKeyJustPressed(SDL_SCANCODE_EQUALS)
                            || m_input->IsKeyJustPressed(SDL_SCANCODE_KP_PLUS))
                            StepGameSpeed(1);
                    }
                    const double speedMul = allowSpeed ? static_cast<double>(GameSpeedMultiplier()) : 1.0;
                    // frameTime was already added once above; add the remaining (speedMul - 1.0) * frameTime
                    accumulator = std::min(accumulator + frameTime * (speedMul - 1.0), SIMULATION_TIMESTEP * 240.0);
                    const auto stepDeadline = clock::now() + std::chrono::milliseconds(28);
                    int ran = 0;
                    while (accumulator >= SIMULATION_TIMESTEP && ran < 400)
                    {
                        Update(static_cast<float>(SIMULATION_TIMESTEP));
                        accumulator -= SIMULATION_TIMESTEP;
                        ++ran;
                        if (!playing())
                            break;
                        if (ran > 1 && clock::now() >= stepDeadline)
                        {
                            accumulator = 0.0;
                            break;
                        }
                    }
                    if (ran == 0 && playing())
                    {
                        m_gameplayController->SetSimulationHold(true);
                        Update(static_cast<float>(frameTime));
                        m_gameplayController->SetSimulationHold(false);
                    }
                }
            }
        }
        else
        {
            // Immediate, responsive per-frame UI update for menus & dialogs
            Update(static_cast<float>(frameTime));
            accumulator = 0.0;
        }

        UpdateRenderAlpha(accumulator, frameTime);
        UpdateSounds();
        UpdateCameraShake(frameTime);
        Render();

        // Frame rate cap (also with vsync on, when the limit is below the refresh rate)
        if (m_settings.fpsLimit > 0)
        {
            const double target = 1.0 / m_settings.fpsLimit;
            const double spent = std::chrono::duration<double>(clock::now() - currentTime).count();
            if (spent < target)
                SDL_DelayNS(static_cast<Uint64>((target - spent) * 1e9));
        }
    }
}

bool Application::IsSpeedControlAllowed() const
{
    if (m_replay)
        return true;
    if (InNetworkMatch())
        return false;
    if (m_mission && m_mission->IsMatch() && m_mission->matchSetup().localOnly)
        return m_mission->matchSetup().allowSpeed;
    return false;
}

void Application::StepGameSpeed(int delta)
{
    if (!IsSpeedControlAllowed())
        return;
    if (delta == 0)
        m_gameSpeed = 1;
    else
        m_gameSpeed = std::clamp(m_gameSpeed + delta, 0, kGameSpeedCount - 1);
    if (m_hudController)
    {
        char buf[32];
        const float mul = GameSpeedMultiplier();
        if (mul < 1.0f)
            std::snprintf(buf, sizeof(buf), "%s: x%.1f", _("SPEED"), mul);
        else
            std::snprintf(buf, sizeof(buf), "%s: x%d", _("SPEED"), static_cast<int>(mul));
        m_hudController->SetNotification(buf, 1.5f);
    }
}

int Application::HudReservedWidth() const
{
    return m_touchUi ? 0 : HUDController::kSidebarWidth;
}

void Application::HandleEvents()
{
    SyncWindowSize();
    uilayout::Poll(SDL_GetTicks()); // the layout file was edited: the interface follows
    if (m_hudController)
        m_input->SetTouchWheelScroll(false); // every scrollable view follows the finger with kinetic glide
    m_input->ResetFrameEvents();
    if (m_hudController && m_touchUi)
    {
        m_input->SetTouchMode(true);
        m_input->SetTouchPan(m_hudController->TouchPansCamera());
        m_input->SetTouchPlacementGhost(m_hudController->TouchPlacementGhost());
        m_input->SetTouchScrollPanel(m_hudController->TouchScrollPanel(m_windowW, m_windowH));
        {
            std::vector<SDL_Rect> panels = m_hudController->TouchPanels(m_windowW, m_windowH);
            if (m_gameplayController) // the control group column keeps the one-finger pan off it
                panels.push_back(m_gameplayController->GroupPanelRect(m_camera->GetViewportWidth(), m_windowH));
            m_input->SetTouchUiRects(panels);
        }
    }
    const bool playing = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying;
    m_input->SetMenuOpen(
        !playing || (m_menuController && m_menuController->IsAnyMenuOpen())
        || (m_hudController && m_hudController->IsResearchWindowOpen()));
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        ImGuiBridge::ProcessEvent(event);
#if defined(AOW_DEBUG)
        if (m_debug && m_debug->IsVisible())
        {
            m_debug->ProcessEvent(event);
        }
#endif

        if (event.type == SDL_EVENT_QUIT)
        {
            m_running = false;
            return;
        }
        if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
        {
            const std::string dropped = event.drop.data;
            if (dropped.size() > 4 && dropped.compare(dropped.size() - 4, 4, ".awr") == 0)
                m_pendingReplay = dropped;
        }

        // Only the game window: the throw-away windows that probe the render APIs report their own sizes
        if ((event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED
             || event.type == SDL_EVENT_WINDOW_MAXIMIZED || event.type == SDL_EVENT_WINDOW_RESTORED
             || event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
            && event.window.windowID == SDL_GetWindowID(m_window))
        {
            SyncWindowSize(event.window.data1, event.window.data2);
        }
        if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND)
        {
            m_renderTargetsLost.store(true);
            SyncWindowSize();
        }

        // A click on a button, list entry or cameo the last frame drew under the mouse
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT
            && SoundRequests::HoverFlag() && m_sound)
            m_sound->PlaySfx("ui_click");
#if OAOW_TOUCH
        SDL_ConvertEventToRenderCoordinates(m_sdlRenderer, &event);
#endif
        m_input->ProcessEvent(event, *m_camera);
    }

#if defined(AOW_DEBUG)
    if (m_gameplayController && m_gameplayController->ConsumeDebugOverlayToggle())
    {
        if (m_debug)
            m_debug->Toggle();
    }
#endif
    // Refresh mouse position every frame so menus get hover feedback too
    m_input->Update(0.0f, *m_camera);
}

FrameContext Application::MakeFrameContext(float deltaTime)
{
    return FrameContext {
        deltaTime,
        m_windowW,
        m_windowH,
        *m_input,
        *m_camera,
        *m_map,
        *m_pathfinder,
        *m_entities,
        *m_economy,
        *m_aiController,
        *m_mission,
        *m_hudController,
        *m_menuController,
        *m_sound,
        m_state};
}

void Application::Update(float deltaTime)
{
    m_globalAnimTimer += deltaTime;
    const bool showcaseScreen = m_state == AppState::MainMenu || m_state == AppState::MenuAbout;
    if (!showcaseScreen)
    {
        if (m_aboutScene)
            m_sound->SetSceneMusicGain(1.0f);
        m_aboutScene.reset();
        m_aboutCredits.clear();
        m_aboutMusicPlaying = false;
        m_aboutViewActive = false;
        m_aboutAttempted = false;
    }
    else if (!m_aboutAttempted)
    {
        m_aboutAttempted = true;
        auto scene = std::make_unique<AboutSimulation>();
        if (scene->Start(*m_texManager, m_windowW, m_windowH))
        {
            m_sound->SetSceneMusicGain(scene->MusicVolume());
            m_aboutCredits = m_menuController->AboutLines();
            m_aboutScene = std::move(scene);
        }
    }
    // The camera zoom belongs to the map of a running game: wheel steps made anywhere else are dropped
    if (m_state != AppState::CampaignPlaying && m_state != AppState::SkirmishPlaying)
        m_input->ConsumeMapWheel();

    // Enter is consumed only together with Alt; a plain Enter belongs to the menus
    if (((SDL_GetModState() & SDL_KMOD_ALT) && m_input->IsKeyJustPressed(SDL_SCANCODE_RETURN)))
    {
        ToggleFullscreen();
    }
    if (m_videoConfirmActive)
    {
        UpdateVideoConfirm(deltaTime);
        return;
    }
    if (m_state != AppState::MenuReplay)
        m_replayMenuFresh = false;
    if (m_state != AppState::MenuMods)
        m_modsMenuFresh = false;
    // A replay file dropped on the window (or named on the command line): from the menus it is opened at once
    if (!m_pendingReplay.empty() && !m_replay
        && (m_state == AppState::MainMenu || m_state == AppState::MenuReplay || m_state == AppState::MenuSettings
            || m_state == AppState::MenuHelp || m_state == AppState::MenuAbout))
    {
        const std::string path = std::move(m_pendingReplay);
        m_pendingReplay.clear();
        OpenReplay(path);
    }

#if defined(AOW_DEBUG)
    // Standalone Test Map (-1) always runs with DEBUG mode active (debug builds)
    if (m_mission && m_mission->GetCurrentMissionId() == -1
        && (!m_debugMode || (m_gameplayController && !m_gameplayController->IsDebugMode())))
    {
        SetDebugMode(true);
    }
#endif

    // A custom match without a person: watched with the whole map, nothing the viewer does reaches the world
    {
        const bool playing
            = m_state == AppState::SkirmishPlaying || m_state == AppState::Paused || m_state == AppState::Briefing;
        const bool spectate = playing && !m_replay && !m_net && m_menuController->IsSpectatorMatch();
        if (spectate != m_spectating)
        {
            m_spectating = spectate;
            m_hudController->SetObserver(spectate);
            m_hudController->SetShowAll(spectate);
            // The bar of the observer (view, follow, speed, statistics, research), as in a replay
            m_replayUi = ReplayUi {};
            if (spectate)
                m_replaySetup = m_mission->matchSetup();
            if (m_sound)
                m_sound->SetInterfaceMuted(spectate); // the computer players' units do not answer the viewer
            m_gameplayController->simulation().SetCommandSink(
                spectate ? std::function<void(SimCommand)>([](SimCommand) {}) : std::function<void(SimCommand)>());
        }
    }

    // Typed text for the map search of the main menu and the search box of the research window
    {
        const bool wantMenuText = (m_state == AppState::MainMenu && m_menuController->WantsText())
                                  || (m_hudController && m_hudController->WantsText());
        if (wantMenuText != m_menuTextInput && (!wantMenuText || !m_textInput))
        {
            if (wantMenuText)
                SDL_StartTextInput(m_window);
            else
                SDL_StopTextInput(m_window);
            m_textInput = m_menuTextInput = wantMenuText;
        }
    }

    UpdateLocalReplay();
    if (m_state == AppState::MainMenu)
    {
        m_gameSpeed = 1;
        m_speedSerial = -1;
    }
    else if (m_mission && m_mission->GetLoadSerial() != m_speedSerial)
    {
        m_speedSerial = m_mission->GetLoadSerial();
        m_gameSpeed = 1;
    }
    if (m_hudController)
    {
        if (m_state != AppState::CampaignPlaying && m_state != AppState::SkirmishPlaying && m_state != AppState::Victory
            && m_state != AppState::Defeat)
        {
            m_scoreboardOpen = false;
        }
        m_hudController->SetStatsOpen((m_replay || m_spectating) ? m_replayUi.stats : m_scoreboardOpen);
        const bool singlePlayerSpeed
            = !InNetworkMatch() && !m_replay && !m_spectating && IsSpeedControlAllowed()
              && (m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying);
        m_hudController->SetSpeedControl(
            singlePlayerSpeed, GameSpeedMultiplier(), [this](int delta) { StepGameSpeed(delta); });
    }
    if (m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying)
    {
        const std::uint32_t tickBefore = m_gameplayController->simulation().Tick();
        UpdateTacticalView();
        m_gameplayController->Update(MakeFrameContext(deltaTime));
        if (m_gameplayController->simulation().Tick() != tickBefore)
            LocalTickRan(tickBefore);
        if (m_hudController && m_hudController->ConsumeStatsToggle())
        {
            if (m_replay || m_spectating)
                m_replayUi.stats = !m_replayUi.stats;
            else
                m_scoreboardOpen = !m_scoreboardOpen;
        }
        if ((m_menuController
             && (m_menuController->IsInGameMenuOpen() || m_menuController->IsObjectivesDialogOpen()
                 || m_menuController->IsMissionDialogOpen()))
            || (m_hudController && m_hudController->IsResearchWindowOpen()))
        {
            m_scoreboardOpen = false;
        }
    }
    else if (m_state == AppState::SplashLogo)
    {
        m_splashTimer += deltaTime;
        float lx, ly;
        if (m_splashTimer >= 2.0f || m_input->IsConfirmJustPressed() || m_input->IsKeyJustPressed(SDL_SCANCODE_ESCAPE)
            || m_input->WasLeftClickIssued(lx, ly))
        {
            m_state = AppState::SplashTitle;
            m_splashTimer = 0.0f;
        }
    }
    else if (m_state == AppState::SplashTitle)
    {
        m_splashTimer += deltaTime;
        float lx, ly;
        if (m_splashTimer >= 2.2f || m_input->IsConfirmJustPressed() || m_input->IsKeyJustPressed(SDL_SCANCODE_ESCAPE)
            || m_input->WasLeftClickIssued(lx, ly))
        {
            m_state = AppState::MainMenu;
            m_menuController->StartMainMenuIntro();
        }
    }
    else if (m_state == AppState::MainMenu)
    {
        if (m_replay)
            CloseReplay();
        m_menuController->UpdateMainMenu(MakeFrameContext(deltaTime), m_running);
    }
    else if (m_state == AppState::MenuSettings)
    {
        m_menuController->UpdateSettings(*m_input, m_windowW, m_windowH, m_state);
    }
    else if (m_state == AppState::MenuNetwork)
    {
        UpdateNetworkMenu();
    }
    else if (m_state == AppState::MenuReplay)
    {
        if (!m_replayMenuFresh)
        {
            m_replayMenu->Refresh();
            m_replayMenuFresh = true;
        }
        const ReplayMenu::Result r = m_replayMenu->Update(*m_input, m_windowW, m_windowH);
        if (r.back)
            m_state = AppState::MainMenu;
        else if (!r.open.empty())
            OpenReplay(r.open);
    }
    else if (m_state == AppState::MenuMods)
    {
        if (!m_modsMenuFresh)
        {
            m_modsMenu->Refresh(m_settings.enabledMods);
            m_modsMenuFresh = true;
        }
        const ModsMenu::Result r = m_modsMenu->Update(*m_input, m_windowW, m_windowH);
        if (r.back)
            m_state = AppState::MainMenu;
        else if (r.apply)
            ApplyMods(r.enabled);
    }
    else if (m_state == AppState::MenuHelp || m_state == AppState::MenuAbout)
    {
        if (m_state == AppState::MenuAbout && m_menuController->IsAboutOutroActive())
            m_menuController->UpdateAboutOutro(deltaTime);
        m_menuController->UpdateInfoScreen(*m_input, m_state);
    }
    else if (m_state == AppState::Briefing)
    {
        float lx, ly;
        bool clicked = m_input->WasLeftClickIssued(lx, ly);
        if (clicked || m_input->IsConfirmJustPressed())
        {
            m_aiController->Init(
                m_mission->GetEnemyHqX(),
                m_mission->GetEnemyHqY(),
                m_mission->GetPlayerHqX(),
                m_mission->GetPlayerHqY());
            m_state = (m_mission && (m_mission->IsMatch() || m_mission->IsSkirmish())) ? AppState::SkirmishPlaying
                                                                                       : AppState::CampaignPlaying;
            m_camera->SetZoom(m_settings.zoomDefault);
            m_camera->CenterOn(m_mission->GetPlayerHqX() * TILE_WIDTH, m_mission->GetPlayerHqY() * TILE_HEIGHT);
        }
    }
    else if (m_state == AppState::Paused)
    {
        if (m_input->IsKeyJustPressed(SDL_SCANCODE_ESCAPE) || m_input->IsConfirmJustPressed())
        {
            m_state = (m_mission && (m_mission->IsMatch() || m_mission->IsSkirmish())) ? AppState::SkirmishPlaying
                                                                                       : AppState::CampaignPlaying;
        }
    }
    else if (m_state == AppState::Victory || m_state == AppState::Defeat)
    {
        // Clicks and Enter from the last seconds of the battle must not skip the result screen: input is read
        // (and so consumed) from the first frame, but acted on only after kEndScreenInputDelay
        if (m_net && !AuthorityMatchContinues())
            m_net->FinishReplay(m_state == AppState::Victory ? "victory" : "defeat"); // the match is saved
        if (m_endScreenTimer <= 0.0f)
            FinishLocalReplay(m_state == AppState::Victory ? "victory" : "defeat");
        m_endScreenTimer += deltaTime;
        float lx, ly;
        const bool confirm = m_input->IsConfirmJustPressed();
        const bool click = m_input->WasLeftClickIssued(lx, ly);
        if (m_endScreenTimer >= kEndScreenInputDelay && (confirm || click))
        {
            m_state = AppState::MainMenu;
        }
    }
    else
    {
        m_endScreenTimer = 0.0f;
    }

    const bool campaignSelectOpen = m_menuController && m_menuController->IsCampaignSelectOpen();
    if (m_aboutScene && (m_state == AppState::MainMenu || m_state == AppState::MenuAbout))
    {
        if (m_state == AppState::MenuAbout && !m_aboutViewActive)
        {
            m_aboutScene->RestartCredits();
            m_aboutMusicPlaying = m_sound->PlayNamedLoop(m_aboutScene->MusicName());
            m_aboutViewActive = true;
        }
        else if (m_state == AppState::MainMenu && m_aboutViewActive)
        {
            m_aboutViewActive = false;
            m_aboutMusicPlaying = false;
        }
        if (!campaignSelectOpen)
            m_aboutScene->Update(deltaTime, m_sound.get());
    }
    else if (m_aboutScene)
    {
        m_aboutScene.reset();
        m_aboutCredits.clear();
        m_aboutMusicPlaying = m_aboutViewActive = m_aboutAttempted = false;
        m_sound->SetSceneMusicGain(1.0f);
    }

    const bool gameplayMusic = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying
                               || m_state == AppState::Paused || m_state == AppState::Briefing
                               || m_state == AppState::Victory || m_state == AppState::Defeat;
    // The menu theme goes on through every screen of the main menu (help, about, settings, network game)
    const bool menuMusic = m_state == AppState::MainMenu || m_state == AppState::MenuHelp
                           || m_state == AppState::MenuAbout || m_state == AppState::MenuSettings
                           || m_state == AppState::MenuNetwork || m_state == AppState::MenuReplay
                           || m_state == AppState::MenuMods;
    m_sound->Update(menuMusic, gameplayMusic, m_state == AppState::MenuAbout && m_aboutMusicPlaying);

    // The time of day of the match (campaign missions are by day): the picture and the loop of the surroundings
    const bool inMatch = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying
                         || m_state == AppState::Paused || m_state == AppState::Victory || m_state == AppState::Defeat;
    const bool night = inMatch && m_mission && m_mission->IsMatch() && m_mission->matchSetup().night;
    if (night != m_night)
    {
        m_night = night;
        ApplyGraphics();
    }
    if (inMatch)
        m_sound->PlayAmbient(night);
    else
        m_sound->StopAmbient();
}

void Application::UpdateCameraShake(double frameTime)
{
    const bool inMatch = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying;
    if (!inMatch || !m_entities || !m_camera)
    {
        m_cameraShake.Reset();
        if (m_entities)
            m_entities->TakeBuildingBlasts();
        return;
    }
    if (m_net && m_net->paused())
        return;
    const float zoom = std::max(0.01f, m_camera->GetZoom());
    const float midX = m_camera->GetX() + m_camera->GetViewportWidth() / zoom * 0.5f;
    const float midY = m_camera->GetY() + m_camera->GetViewportHeight() / zoom * 0.5f;
    for (const EntityManager::BuildingBlast &b : m_entities->TakeBuildingBlasts())
    {
        if (!m_settings.cameraShake)
            continue;
        const float dx = (b.x - midX) / TILE_WIDTH, dy = (b.y - midY) / TILE_HEIGHT;
        m_cameraShake.AddBlast(std::sqrt(dx * dx + dy * dy), b.size);
    }
    if (!m_settings.cameraShake)
        m_cameraShake.Reset();
    m_cameraShake.Update(static_cast<float>(frameTime));
}

void Application::UpdateSounds()
{
    if (!m_sound)
    {
        SoundRequests::Take();
        return;
    }
    if (!m_gameSounds)
        m_gameSounds = std::make_unique<GameSounds>();
    const bool inMatch = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying
                         || m_state == AppState::Paused || m_state == AppState::Victory || m_state == AppState::Defeat;
    for (const SoundRequests::Request &r : SoundRequests::Take())
    {
        if (!r.event.empty() && r.event[0] == '@')
        {
            UnitReply reply;
            if (inMatch && m_gameplayController && m_entities && SoundIds::ReplyFromRequest(r.event.c_str(), reply))
                m_gameSounds->PlayReply(*m_sound, *m_entities, m_gameplayController->GetSelectedUnits(), reply);
        }
        else if (r.announcer)
        {
            m_sound->Announce(r.event);
        }
        else
        {
            m_sound->PlaySfx(r.event);
        }
    }
    if (inMatch && m_state != AppState::Paused && m_entities && m_map && m_economy && m_camera && m_hudController
        && m_gameplayController)
    {
        const bool fog = m_map->IsFogEnabled() && !m_gameplayController->IsRevealFog() && !ReplayShowsAll();
        m_gameSounds->Update(
            *m_sound, *m_entities, *m_economy, *m_map, *m_camera, fog, m_hudController->radarAlert(), SDL_GetTicks());
        m_gameSounds->SelectionChanged(*m_sound, *m_entities, m_gameplayController->GetSelectedUnits());
    }
    // The end of a match or screen transition
    if (m_state != m_soundState)
    {
        const bool isMenuState
            = (m_state == AppState::MainMenu || m_state == AppState::MenuSettings || m_state == AppState::MenuNetwork
               || m_state == AppState::MenuReplay || m_state == AppState::MenuHelp || m_state == AppState::MenuAbout
               || m_state == AppState::MenuMods);
        if (isMenuState && m_soundState != AppState::SplashLogo)
            m_sound->PlaySfx("ui_show_dialog");

        if (m_state == AppState::Victory)
            m_sound->Announce("ann_victory");
        else if (m_state == AppState::Defeat)
            m_sound->Announce("ann_defeat");
        else if (
            !inMatch
            && (m_soundState == AppState::CampaignPlaying || m_soundState == AppState::SkirmishPlaying
                || m_soundState == AppState::Paused || m_soundState == AppState::Victory
                || m_soundState == AppState::Defeat))
        {
            m_sound->Sfx().StopAll(); // back in the menus: the battle falls silent
            m_gameSounds->Reset();
        }
        m_soundState = m_state;
    }
}

// The main menu comes up out of black: a linear fade over kMenuFadeMs whenever it opens from outside the menus (after
// the splash screens, after a game), not when going back to it from one of its own screens
void Application::RenderMenuFade()
{
    const bool menuScreen = m_state == AppState::MainMenu || m_state == AppState::MenuHelp
                            || m_state == AppState::MenuAbout || m_state == AppState::MenuSettings
                            || m_state == AppState::MenuNetwork || m_state == AppState::MenuReplay
                            || m_state == AppState::MenuMods;
    const std::uint64_t now = SDL_GetTicks();
    if (menuScreen && !m_menuFadeWasMenu)
        m_menuFadeStartMs = m_state == AppState::MainMenu ? now : 0;
    m_menuFadeWasMenu = menuScreen;
    if (!menuScreen || m_menuFadeStartMs == 0)
        return;
    const std::uint64_t elapsed = now - m_menuFadeStartMs;
    if (elapsed >= kMenuFadeMs)
    {
        m_menuFadeStartMs = 0;
        return;
    }
    const float t = static_cast<float>(elapsed) / static_cast<float>(kMenuFadeMs);
    const Uint8 alpha = static_cast<Uint8>(255.0f * (1.0f - t));
    m_renderer->FillRectScreen(0, 0, m_windowW, m_windowH, {0, 0, 0, alpha});
}

void Application::Render()
{
    ImGuiBridge::BeginFrame();
    SoundRequests::HoverFlag() = false; // the widgets drawn now mark it again
    m_renderer->Clear({0, 0, 0, 255});

    // Game UI wears the player's faction colors; menus stay neutral (the ImGui debug UI has its own style)
    const bool inGame = m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying
                        || m_state == AppState::Paused || m_state == AppState::Victory || m_state == AppState::Defeat
                        || m_state == AppState::Briefing;
    const int localColor = inGame ? Players::Slot(Players::Local()).color : 0;
    const SDL_Color localArmy = ArmyColor(localColor);
    ui::ApplyTheme(inGame ? PlayerFaction() : Faction::Neutral, localColor > 0 ? &localArmy : nullptr);
    ImGuiBridge::ApplyTheme(inGame ? PlayerFaction() : Faction::Neutral, localColor > 0 ? &localArmy : nullptr);

    if (m_state == AppState::SplashLogo)
    {
        m_menuController->RenderSplashLogo(*m_renderer, *m_camera, *m_texManager, m_windowW, m_windowH);
    }
    else if (m_state == AppState::SplashTitle)
    {
        m_menuController->RenderSplashTitle(*m_renderer, *m_camera, *m_texManager, m_windowW, m_windowH);
    }
    else if (m_state == AppState::MainMenu)
    {
        const bool campaignOpen = m_menuController && m_menuController->IsCampaignSelectOpen();
        if (m_aboutScene && !campaignOpen)
            m_aboutScene->Render(*m_renderer, *m_fontRenderer, {}, m_windowW, m_windowH, false);
        m_menuController->RenderMainMenu(
            *m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH, !m_aboutScene);
    }
    else if (m_state == AppState::MenuHelp)
    {
        m_menuController->RenderMainMenu(*m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
        m_menuController->RenderMenuHelp(*m_renderer, *m_fontRenderer, m_windowW, m_windowH);
    }
    else if (m_state == AppState::MenuAbout)
    {
        if (m_aboutScene)
        {
            m_aboutScene->Render(*m_renderer, *m_fontRenderer, m_aboutCredits, m_windowW, m_windowH);
            if (m_menuController->IsAboutOutroActive())
            {
                m_menuController->RenderMainMenu(
                    *m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH, false);
            }
        }
        else
        {
            m_menuController->RenderMainMenu(
                *m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
            m_menuController->RenderMenuAbout(*m_renderer, *m_fontRenderer, m_windowW, m_windowH);
        }
    }
    else if (m_state == AppState::MenuSettings)
    {
        m_menuController->RenderMainMenu(*m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
        m_menuController->RenderMenuSettings(*m_renderer, *m_fontRenderer, m_windowW, m_windowH);
    }
    else if (m_state == AppState::MenuReplay)
    {
        m_menuController->RenderMainMenu(*m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
        m_replayMenu->Render(*m_renderer, *m_fontRenderer, m_windowW, m_windowH);
    }
    else if (m_state == AppState::MenuMods)
    {
        m_menuController->RenderMainMenu(*m_renderer, *m_texManager, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
        m_modsMenu->Render(*m_renderer, *m_fontRenderer, m_windowW, m_windowH);
    }
    else if (m_state == AppState::MenuNetwork)
    {
        m_renderer->FillRectScreen(0, 0, m_windowW, m_windowH, ui::color::Backdrop);
        m_networkMenu->render(
            *m_renderer,
            *m_fontRenderer,
            *m_texManager,
            m_windowW,
            m_windowH,
            m_net.get(),
            m_settings,
            m_lanGames,
            m_serverRooms,
            m_serverStatus);
    }
    else if (m_state == AppState::Briefing)
    {
        m_menuController->RenderBriefing(*m_renderer, *m_fontRenderer, *m_mission, m_windowW, m_windowH);
    }
    else if (
        m_state == AppState::CampaignPlaying || m_state == AppState::SkirmishPlaying || m_state == AppState::Paused
        || m_state == AppState::Victory || m_state == AppState::Defeat)
    {
        // Army colours (shader effects): from the player slots (chosen in the game setup / the network room)
        for (PlayerId p = 0; p < kMaxPlayers; ++p)
        {
            const int color = Players::Slot(p).color;
            m_renderer->SetArmyColor(p, Players::IsActive(p) && color > 0, ArmyColor(color));
        }

        // Camera shake: the world and its overlays are drawn from a shifted view, the HUD and the input are not
        struct ShakeGuard
        {
            Camera *cam {nullptr};
            float x {0.0f}, y {0.0f};
            void Restore()
            {
                if (cam)
                    cam->SetPosition(x, y);
                cam = nullptr;
            }
            ~ShakeGuard()
            {
                Restore();
            }
        } shake;
        if (m_cameraShake.Active() && m_settings.cameraShake)
        {
            float ox = 0.0f, oy = 0.0f;
            m_cameraShake.Offset(ox, oy);
            const float zoom = std::max(0.01f, m_camera->GetZoom());
            shake = {m_camera.get(), m_camera->GetX(), m_camera->GetY()};
            m_camera->SetPosition(shake.x + ox / zoom, shake.y + oy / zoom);
        }

        // Zoomed far out the world is not drawn: the radar map takes its place (after the picture below)
        const bool tactical = UpdateTacticalView();
        // The world is drawn into a smaller target at low / medium quality and scaled up afterwards
        const int divisor = tactical ? 1 : RenderDivisor();
        const Camera lowCam = divisor > 1 ? m_camera->ScaledDown(divisor) : *m_camera;
        const Camera &worldCam = divisor > 1 ? lowCam : *m_camera;
        // Shader effects: the world goes into an offscreen picture, graded and upscaled on the way to the screen
        SDL_Texture *scene = m_effects && m_effects->PostActive() && !tactical
                                 ? m_effects->SceneTarget(worldCam.GetViewportWidth(), worldCam.GetViewportHeight())
                                 : nullptr;
        if (scene)
        {
            SDL_SetRenderTarget(m_sdlRenderer, scene);
            m_renderer->Clear({0, 0, 0, 255});
        }
        else if (divisor > 1)
        {
            const int lowW = worldCam.GetViewportWidth(), lowH = worldCam.GetViewportHeight();
            if (!m_lowResTarget || m_lowResW != lowW || m_lowResH != lowH)
            {
                if (m_lowResTarget)
                    SDL_DestroyTexture(m_lowResTarget);
                m_lowResTarget
                    = SDL_CreateTexture(m_sdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, lowW, lowH);

                m_lowResW = lowW;
                m_lowResH = lowH;
            }
            if (m_lowResTarget)
            {
                SDL_SetTextureScaleMode(
                    m_lowResTarget,
                    m_settings.smoothing ? TextureManager::SmoothPixelScaleMode() : SDL_SCALEMODE_NEAREST);
                SDL_SetRenderTarget(m_sdlRenderer, m_lowResTarget);
                m_renderer->Clear({0, 0, 0, 255});
            }
        }
        bool offscreenWorld = false;
        int edgeLeft = 0, edgeTop = 0, edgeRight = 0, edgeBottom = 0, edgeViewW = 0, edgeViewH = 0;
        // Everything of the world stays inside the map: tiles, cliffs, trees, buildings and the fog no longer
        // stick out over its edge
        {
            float x0, y0, x1, y1;
            worldCam.WorldToScreen(0.0f, 0.0f, x0, y0);
            worldCam.WorldToScreen(
                static_cast<float>(m_map->GetWidth() * TILE_WIDTH),
                static_cast<float>(m_map->GetHeight() * TILE_HEIGHT),
                x1,
                y1);
            const int left = std::max(0, static_cast<int>(std::floor(x0)));
            const int top = std::max(0, static_cast<int>(std::floor(y0)));
            const int right = std::min(worldCam.GetViewportWidth(), static_cast<int>(std::ceil(x1)));
            const int bottom = std::min(worldCam.GetViewportHeight(), static_cast<int>(std::ceil(y1)));
            // An offscreen target is cleared black already; a hardware scissor on it misbehaves behind the logical
            // presentation of the touch build (a dark slab over the picture), so the margins are filled instead
            offscreenWorld = scene || (divisor > 1 && m_lowResTarget) || m_touchUi;
            if (!offscreenWorld)
                m_renderer->SetClipRect({left, top, std::max(0, right - left), std::max(0, bottom - top)});
            else
            {
                const int vw = worldCam.GetViewportWidth(), vh = worldCam.GetViewportHeight();
                edgeLeft = left;
                edgeTop = top;
                edgeRight = right;
                edgeBottom = bottom;
                edgeViewW = vw;
                edgeViewH = vh;
            }
        }

        // Layer 0: Base Terrain
#if defined(AOW_DEBUG)
        const bool overlayNoFog = m_debug && m_debug->IsFogDisabled();
#else
        const bool overlayNoFog = false;
#endif
        bool disableFog
            = overlayNoFog || m_gameplayController->IsRevealFog() || !m_map->IsFogEnabled() || ReplayShowsAll();
        int globalAnimTick = static_cast<int>(m_globalAnimTimer * 8.0f);
        bool isDebug = m_debugMode || (m_gameplayController && m_gameplayController->IsDebugMode());
        if (!tactical)
        {
        m_map->RenderTerrain(*m_renderer, worldCam, !disableFog, globalAnimTick);

        // Layer 1: Ground Remnants (Strictly 1 layer above ground, underneath decorations, walls, buildings & units)
        m_map->RenderWallRubble(*m_renderer, worldCam, !disableFog);
        m_entities->RenderGroundRemnants(*m_renderer, worldCam, *m_map, !disableFog);
        m_entities->RenderMines(*m_renderer, worldCam, *m_map, ReplayShowsAll() ? kNoPlayer : Players::Local());

        // Layer 2: Map Decorations (trees, bridges, cliffs, landmarks)
        m_map->RenderDecorations(*m_renderer, worldCam, !disableFog, globalAnimTick);

        // Layer 3..4: Living Walls, Buildings, Units, FX
        m_entities->render(
            *m_renderer,
            worldCam,
            *m_map,
            !disableFog,
            globalAnimTick,
            isDebug,
            m_input->isKeyPressed(SDL_SCANCODE_LALT) || m_input->isKeyPressed(SDL_SCANCODE_RALT));

        // Layer 5: Fog of War Explored Shading
        if (!disableFog)
        {
            // Low graphics: the fog in plain tiles, without smoothed edges
            m_map->RenderFog(*m_renderer, worldCam, m_settings.graphicsQuality > 0);
        }
        }

        if (offscreenWorld)
        {
            const SDL_Color black {0, 0, 0, 255};
            const int innerTop = std::max(0, edgeTop), innerBottom = std::min(edgeViewH, edgeBottom);
            if (edgeTop > 0)
                m_renderer->FillRectScreen(0, 0, edgeViewW, std::min(edgeViewH, edgeTop), black);
            if (edgeBottom < edgeViewH)
                m_renderer->FillRectScreen(
                    0, std::max(0, edgeBottom), edgeViewW, edgeViewH - std::max(0, edgeBottom), black);
            if (innerBottom > innerTop)
            {
                if (edgeLeft > 0)
                    m_renderer->FillRectScreen(
                        0, innerTop, std::min(edgeViewW, edgeLeft), innerBottom - innerTop, black);
                if (edgeRight < edgeViewW)
                    m_renderer->FillRectScreen(
                        std::max(0, edgeRight),
                        innerTop,
                        edgeViewW - std::max(0, edgeRight),
                        innerBottom - innerTop,
                        black);
            }
        }
        if (!tactical)
            RenderMapEdgeFade(worldCam);
        m_renderer->ResetClipRect();
        m_renderer->EndWorldPass();

        // Night: the light map over the world picture (buildings, explosions, fire); without shader effects it also
        // darkens it, the SDL GPU renderer's Night grade does that itself
        if (m_night && m_nightLighting && !tactical)
        {
            const bool gpu = m_effects && m_effects->Available();
            if (gpu)
                m_effects->UseDefaultState();
            std::vector<EntityManager::LightSpot> &lights = m_nightLighting->Lights();
            m_entities->CollectLights(*m_map, !disableFog, lights);
            m_nightLighting->Apply(
                m_sdlRenderer,
                worldCam,
                *m_map,
                !disableFog,
                gpu,
                lights,
                worldCam.GetViewportWidth(),
                worldCam.GetViewportHeight());
        }

        if (scene)
        {
            // Glow: what shines, again, into a quarter-size target, then blurred
            if (m_effects->Glow())
            {
                const Camera glowCam = m_camera->ScaledDown(kGlowDivisor);
                m_effects->BeginGlow(glowCam.GetViewportWidth(), glowCam.GetViewportHeight());
                m_entities->RenderEmissive(*m_renderer, glowCam, *m_map, !disableFog);
                m_effects->EndGlow();
            }
            SDL_SetRenderTarget(m_sdlRenderer, nullptr);
            const SDL_Rect clip {0, 0, m_camera->GetViewportWidth(), m_camera->GetViewportHeight()};
            SDL_SetRenderClipRect(m_sdlRenderer, &clip);
            const SDL_FRect dst {
                0.0f,
                0.0f,
                static_cast<float>(worldCam.GetViewportWidth() * divisor),
                static_cast<float>(worldCam.GetViewportHeight() * divisor)};
            const Camera glowCam = m_camera->ScaledDown(kGlowDivisor);
            const SDL_FRect glowDst {
                0.0f,
                0.0f,
                static_cast<float>(glowCam.GetViewportWidth() * kGlowDivisor),
                static_cast<float>(glowCam.GetViewportHeight() * kGlowDivisor)};
            m_effects->Composite(dst, glowDst, m_settings.smoothing);
            SDL_SetRenderClipRect(m_sdlRenderer, nullptr);
        }
        else if (divisor > 1 && m_lowResTarget)
        {
            SDL_SetRenderTarget(m_sdlRenderer, nullptr);
            const SDL_Rect clip {0, 0, m_camera->GetViewportWidth(), m_camera->GetViewportHeight()};
            SDL_SetRenderClipRect(m_sdlRenderer, &clip);
            const SDL_FRect dst {
                0.0f, 0.0f, static_cast<float>(m_lowResW * divisor), static_cast<float>(m_lowResH * divisor)};
            SDL_RenderTexture(m_sdlRenderer, m_lowResTarget, nullptr, &dst);
            SDL_SetRenderClipRect(m_sdlRenderer, nullptr);
        }

        // The radar map over the empty picture (orders, selection and production bars are drawn over it below)
        if (tactical)
        {
            m_hudController->SetRadarFog(!disableFog);
            m_hudController->DrawTacticalMap(
                *m_renderer,
                *m_fontRenderer,
                {0, 0, m_camera->GetViewportWidth(), m_camera->GetViewportHeight()},
                *m_entities,
                *m_map,
                *m_camera);
        }

        // Render Debug Passability Grid (70% Transparency Green/Red)
        m_gameplayController->RenderDebugGrid(*m_renderer, *m_camera, *m_entities, *m_map);

        // Order lines of the selected units (of every unit in sight while Alt is held); not for a viewer of computer
        // players: their orders are theirs
        if (!m_spectating)
            m_gameplayController->RenderOrderLines(
                *m_renderer,
                *m_camera,
                *m_entities,
                *m_map,
                m_input->isKeyPressed(SDL_SCANCODE_LALT) || m_input->isKeyPressed(SDL_SCANCODE_RALT));

        // Render Debug Navigation (Paths, Waypoints, Combat/Wall Targets)
        m_gameplayController->RenderDebugNavigation(*m_renderer, *m_camera, *m_entities, *m_map);

        // AI groups (G1..GN), leash lines and chokepoints
        if (isDebug)
        {
            m_aiController->RenderDebug(*m_renderer, *m_fontRenderer, *m_camera, *m_entities);
        }

        // Leaders of the player's control groups
        m_gameplayController->RenderGroupLeaders(*m_renderer, *m_camera, *m_entities);

        // Training progress above Barracks / Factories, rally lines of the selected ones
        m_hudController->RenderProductionOverlay(*m_renderer, *m_fontRenderer, *m_entities, *m_map, *m_camera);
        // Not enough power: blinking lightnings at the center of buildings that stopped
        m_hudController->RenderPowerAlerts(*m_renderer, *m_entities, *m_map, *m_economy, *m_camera);

        shake.Restore();

        // Render Selection Box
        if (!m_hudController->IsBuildMode() && !m_hudController->OwnsMapInput())
        {
            m_gameplayController->RenderSelectionBox(*m_renderer, *m_camera, *m_input);
        }

        // Render Build Ghost in Placement Mode (only while the cursor is over the map)
        if (m_hudController->IsBuildMode()
            && !m_hudController->IsOverHud(
                m_input->GetMouseScreenX(), m_input->GetMouseScreenY(), m_windowW, m_windowH))
        {
            int mouseTileX = static_cast<int>(m_input->GetMouseWorldX() / TILE_WIDTH);
            int mouseTileY = static_cast<int>(m_input->GetMouseWorldY() / TILE_HEIGHT);
            m_hudController->RenderBuildGhost(
                *m_renderer, *m_map, *m_camera, mouseTileX, mouseTileY, *m_economy, *m_entities);
            m_renderer->EndWorldPass();
        }

        // Render Debug Spawn Ghost in Debug Spawn Mode
        if (m_gameplayController->IsSpawnMode())
        {
            m_gameplayController->RenderDebugSpawnGhost(*m_renderer, *m_camera, *m_input, *m_map, *m_fontRenderer);
        }

        // Render Attack-Move Reticle in Attack-Move Mode ('Q' key)
        if (m_hudController->IsAttackMoveMode()
            && !m_hudController->IsOverHud(
                m_input->GetMouseScreenX(), m_input->GetMouseScreenY(), m_windowW, m_windowH))
        {
            int mx = m_input->GetMouseScreenX();
            int my = m_input->GetMouseScreenY();

            // Crosshair
            m_renderer->DrawRectScreen(mx - 14, my - 1, 29, 3, {255, 60, 60, 255});
            m_renderer->DrawRectScreen(mx - 1, my - 14, 3, 29, {255, 60, 60, 255});
            m_renderer->DrawRectScreen(mx - 9, my - 9, 19, 19, {255, 220, 0, 255});

            // Floating tooltip, kept inside the map viewport
            std::string tip = _("ATTACK-MOVE [Q]: select target (LMB to confirm, RMB/ESC to cancel)");
            int tipW = m_fontRenderer->GetTextWidth(tip, ui::FONT_SMALL) + 16;
            int tipX = std::min(mx + 16, m_camera->GetViewportWidth() - tipW - 4);
            int tipY = std::max(4, my - 30);
            m_renderer->FillRectScreen(tipX, tipY, tipW, 24, ui::color::Panel);
            m_renderer->DrawRectScreen(tipX, tipY, tipW, 24, ui::color::Danger);
            m_fontRenderer->DrawString(
                tip, tipX + 8, tipY + 12, ui::FONT_SMALL, ui::color::Text, false, Anchor::LEFT | Anchor::VCENTER);
        }

        // Control groups (Ctrl+1..0) along the bottom of the map view
        m_gameplayController->RenderGroupBar(
            *m_renderer,
            *m_fontRenderer,
            *m_entities,
            m_camera->GetViewportWidth(),
            m_windowH,
            m_input->GetMouseScreenX(),
            m_input->GetMouseScreenY());

        // Command sidebar (right edge)
        m_hudController->SetRadarFog(!disableFog);
        m_hudController->RenderGameHud(
            *m_renderer,
            *m_fontRenderer,
            *m_economy,
            *m_entities,
            *m_map,
            *m_camera,
            m_input->GetMouseScreenX(),
            m_input->GetMouseScreenY(),
            m_windowW,
            m_windowH,
            m_gameplayController->GetSelectedUnits());

        // The technology tree window (over the map and the sidebar)
        m_hudController->RenderResearchWindow(
            *m_renderer,
            *m_fontRenderer,
            *m_economy,
            *m_entities,
            m_input->GetMouseScreenX(),
            m_input->GetMouseScreenY(),
            m_windowW,
            m_windowH);

        // Top Debug Toolbar (--debug mode)
        m_gameplayController->RenderDebugToolbar(*m_renderer, *m_fontRenderer, m_windowW);
        m_gameplayController->RenderDebugSpawnDropdown(*m_renderer, *m_fontRenderer, m_windowH);

        if (m_menuController->IsMissionDialogOpen())
        {
            m_menuController->RenderMissionDialog(*m_renderer, *m_fontRenderer, m_windowW, m_windowH);
        }
        if (m_menuController->IsObjectivesDialogOpen())
        {
            m_menuController->RenderObjectivesDialog(
                *m_renderer,
                *m_fontRenderer,
                *m_mission,
                m_aiController->GetEffectiveDifficulty(),
                m_windowW,
                m_windowH);
        }
        else if (m_menuController->IsInGameMenuOpen() && m_menuController->IsInGameSettingsOpen())
        {
            m_menuController->RenderMenuSettings(*m_renderer, *m_fontRenderer, m_windowW, m_windowH);
        }
        else if (m_menuController->IsInGameMenuOpen() || m_state == AppState::Paused)
        {
            m_menuController->RenderInGamePauseMenu(*m_renderer, *m_fontRenderer, *m_sound, m_windowW, m_windowH);
        }
        else if (m_state == AppState::Victory || m_state == AppState::Defeat)
        {
            bool won = (m_state == AppState::Victory);
            int viewW = m_camera->GetViewportWidth();
            m_renderer->FillRectScreen(0, 0, viewW, m_windowH, ui::color::Scrim);
            ui::UiRect band {0, m_windowH / 2 - 70, viewW, 140};
            m_renderer->FillRectScreen(band.x, band.y, band.w, band.h, ui::color::Panel);
            SDL_Color accent = won ? ui::color::Ok : ui::color::Danger;
            m_renderer->FillRectScreen(band.CenterX() - 120, band.y, 240, 2, accent);
            m_renderer->FillRectScreen(band.CenterX() - 120, band.Bottom() - 2, 240, 2, accent);
            m_fontRenderer->DrawString(
                won ? _("MISSION ACCOMPLISHED!") : _("MISSION FAILED!"),
                band.CenterX(),
                band.CenterY() - 14,
                ui::FONT_HERO,
                accent,
                true,
                Anchor::HCENTER | Anchor::VCENTER);
            if (m_endScreenTimer >= kEndScreenInputDelay)
            {
                m_fontRenderer->DrawString(
                    _("Click or press Enter to return to the main menu"),
                    band.CenterX(),
                    band.CenterY() + 38,
                    ui::FONT_BODY,
                    ui::color::TextDim,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
            }
        }
    }

    RenderMenuFade();
    RenderNotices();
    RenderGameChat();
    RenderScoreboard();
    RenderFpsCounter();
    RenderNetStats();
    RenderNetworkBanner();
    if ((m_replay || m_spectating) && m_state == AppState::SkirmishPlaying)
        RenderReplayBar();
    if (m_videoConfirmActive)
    {
        RenderVideoConfirm();
    }

#if defined(AOW_DEBUG)
    // Debug Overlay (Dear ImGui)
    if (m_debug)
    {
        m_debug->render(
            *m_map, *m_entities, *m_economy, *m_mission, *m_sound, *m_camera, *m_aiController, m_fps, m_resourceManager.get());
    }
#endif

    ImGuiBridge::EndFrame();

    m_renderer->Present();
    if (m_renderFirstFrame)
    {
        m_renderFirstFrame = false;
        RenderBackend::ConfirmFirstFrame(m_env);
    }
}

// The Mods screen applied: the mods go in place in that order, everything read from the data is read again, the choice
// is saved. Music, sounds, fonts and translations were loaded at start: they change with the next start.
void Application::ApplyMods(const std::vector<std::string> &ids)
{
    Mods::ActivateReport report;
    const bool ok = Mods::Activate(ids, &report);
    LoaderResources::Reload();
    ReloadGameData();
    if (m_texManager)
        m_texManager->ReloadSprites();
    m_settings.enabledMods.clear();
    for (const Mods::ModInfo &m : Mods::Active())
        m_settings.enabledMods.push_back(m.id);
    m_settings.Save(Settings::DefaultPath());
    m_modsMenu->Refresh(m_settings.enabledMods);
    std::string notice = ok ? (ids.empty() ? _("No mods: the installed data is in use") : _("Mods applied"))
                            : _("Some mods could not be used");
    notice += std::string(". ") + _("Music, sounds, fonts and translations change after a restart.");
    m_modsMenu->SetReport(report, notice);
}

} // namespace OpenArtOfWar
