#pragma once

#include "game/Match.hpp"

#include "core/Config.hpp"
#include "core/EnvironmentConfig.hpp"
#include "ui/MapPreview.hpp"
#include "core/Settings.hpp"
#include "core/VideoModes.hpp"
#include "graphics/RenderBackend.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace OpenArtOfWar
{
class GameSounds;
class ReplayPlayer;
class ReplayMenu;

class TextureManager;
class Renderer;
class Camera;
class BitmapFontRenderer;
class InputManager;
class SoundEngine;
class TileMap;
class Pathfinder;
class EntityManager;
class Economy;
class AIController;
class MissionController;
class SaveManager;
class DebugOverlay;
class MenuController;
class HUDController;
class GameplayController;
class ShaderEffects;
class NetMatch;
namespace Net
{
class LanBrowser;
class MasterClient;
} // namespace Net
class NetworkMenu;
struct LanGame;

class Application
{
public:
    static constexpr int kGlowDivisor = 4; // the glow is worked out at a quarter of the view size

    friend class HeadlessTest;

    Application();
    ~Application();

    bool Init(int windowWidth = DEFAULT_WINDOW_WIDTH, int windowHeight = DEFAULT_WINDOW_HEIGHT);
    void run();
#if defined(AOW_DEBUG)
    bool RunHeadlessTest(int numTicks = 200); // debug builds only (HeadlessTest.cpp)
#endif
    void Shutdown();
    void SetDebugMode(bool debug);
    bool IsDebugMode() const
    {
        return m_debugMode;
    }
    // Opens a recorded network game (.awr) and plays it back; false (with the reason on the replay screen) when it
    // cannot be played
    bool OpenReplay(const std::string &path);
    void LaunchTestMap();
    // Single-player game on a map straight from the command line (--map=<name|path>, --from-scratch):
    // the same start as a custom game picked in the menu. False when the map is unknown or fails to load.
    bool LaunchMap(const std::string &nameOrPath, bool fromScratch);

    const Settings &settings() const
    {
        return m_settings;
    }
    // Before init: render API from the command line (one-off, not stored)
    void SetRendererOverride(const std::string &name)
    {
        m_rendererOverride = name;
    }
    // Before init: self-test run, the renderer is whatever SDL picks (no probing, no config changes)
    void SetHeadless(bool headless)
    {
        m_headless = headless;
    }
    // run() returned because the window has to be built anew (another render API): start a new Application
    bool RestartRequested() const
    {
        return m_restartRequested;
    }
    // Network game from the command line (until the lobby screen exists): host on `port` and start the
    // match on `mapPath` once `players` have joined, or join address:port
    void HostNetworkGame(
        uint16_t port, int players, const std::string &mapPath, const std::string &name, bool fromScratch = false);
    void JoinNetworkGame(const std::string &address, uint16_t port, const std::string &name);
    // The hosted game is listed on the server (reachable by its code through NAT hole punching)
    void AnnounceNetworkGame(const std::string &server, uint16_t port);
    // Through a dedicated server (host:port): create a room (code empty) or join the room with `code`;
    // `players` / `mapPath` as for hostNetworkGame (the creator owns the room)
    void ServerNetworkGame(
        const std::string &server,
        uint16_t port,
        const std::string &code,
        int players,
        const std::string &mapPath,
        const std::string &name,
        bool fromScratch = false);
    // Applies the options of one area to the running game and stores them in settings.json
    void ApplySettings(SettingsArea area);

private:
    void HandleEvents();
    void Update(float deltaTime);
    void render();

    void ApplyGraphics();
    // Graphics quality: the game world is drawn at 1/3 (low), 1/2 (medium) or the full (high) resolution
    int RenderDivisor() const;
    void RenderMapEdgeFade(const Camera &camera);
    void ApplyAudio();
    void SaveSettings();
    void ToggleFullscreen();
    // Window mode / size changed: keep it only when confirmed in time, else go back to the previous one
    void BeginVideoConfirm(const Settings &previous);
    void UpdateVideoConfirm(float deltaTime);
    void RenderVideoConfirm();
    void SyncWindowSize(int w = 0, int h = 0);
    void RenderFpsCounter();
    // Settings "Network statistics": ping, jitter, loss, traffic, turn buffer (network lobby and match)
    void RenderNetStats();
    // Network match: services the session, starts the match when its setup arrives
    void UpdateNetwork(uint32_t nowMs);
    bool InNetworkMatch() const;
    void DriveNetworkBot(uint32_t tick);
    void RenderNetworkBanner();
    // Network match: notices who drops out / comes back / leaves (from the room's roster)
    void TrackNetPlayers(uint32_t nowMs);
    // Short messages over the battlefield ("Player X has disconnected"), they fade after a few seconds
    void PushNotice(const std::string &text, uint32_t nowMs);
    void RenderNotices();
    void RenderMenuFade();
    // Tab held in a match: every player with faction, colour, name and efficiency points
    void RenderScoreboard();
    // Sound effects of the frame: the interface's requests, the world, unit replies, victory / defeat
    void UpdateSounds();
    std::unique_ptr<GameSounds> m_gameSounds;
    float m_endScreenTimer {0.0f}; // seconds on the Victory / Defeat screen
    static constexpr float kEndScreenInputDelay = 2.0f; // input is ignored this long after the match ends
    AppState m_soundState {AppState::SplashLogo}; // the state the last updateSounds saw
    // Draws the loading screen right now (for work that blocks the main loop: start-up, loading a match); the window
    // stays alive. `progress` 0..1, negative for an unknown length. Nothing in headless mode.
    void ShowLoading(const std::string &status, float progress);
    // A map is being loaded: the loading screen with the map's picture stays up for at least three seconds, so it can
    // be read (begin before the work, end after it)
    void BeginMapLoading(const std::string &title, const std::string &mapPath);
    void EndMapLoading();
    void DrawLoadingFrame(
        const std::string &title, const std::string &status, float progress, SDL_Texture *preview, int pw, int ph);
    // Render API chosen in the settings: rebuild the window now, confirm or go back after the restart
    void SwitchRenderApi();
    void RequestRestart();
    void KeepRenderApi();
    void RevertRenderApi();

    bool m_running {false};
    MapPreview m_loadingPreview; // the picture of the map on the loading screen
    bool m_mapLoading {false};
    Uint32 m_mapLoadStartMs {0};
    std::string m_mapLoadTitle;
    SDL_Texture *m_mapLoadTexture {nullptr};
    int m_mapLoadW {0}, m_mapLoadH {0};
    bool m_debugMode {false};
    AppState m_state {AppState::SplashLogo};
    // Fade of the main menu out of black (RenderMenuFade)
    static constexpr uint64_t kMenuFadeMs = 2000;
    uint64_t m_menuFadeStartMs {0};
    bool m_menuFadeWasMenu {false};
    float m_splashTimer {0.0f};

    Settings m_settings;
    Settings m_confirmedVideo; // video options of the last confirmed window setup
    bool m_videoConfirmActive {false};
    bool m_renderConfirm {false}; // the confirm dialog asks about the render API, not the window
    float m_videoConfirmTimer {0.0f};
    std::vector<Resolution> m_resolutions;
    SDL_Texture *m_lowResTarget {nullptr}; // the world at reduced resolution (low / medium quality)
    int m_lowResW {0};
    int m_lowResH {0};

    float m_fps {60.0f};
    float m_globalAnimTimer {0.0f};
    int m_windowW {DEFAULT_WINDOW_WIDTH};
    int m_windowH {DEFAULT_WINDOW_HEIGHT};

    EnvironmentConfig m_env; // assets/data/game_environment.json
    std::string m_rendererOverride;
    bool m_headless {false};
    bool m_restartRequested {false};
    bool m_renderFirstFrame {false};              // the next present is the first one of a new renderer
    std::vector<RenderApiOption> m_renderOptions; // probed once, when the graphics settings show them

    SDL_Window *m_window {nullptr};
    SDL_Renderer *m_sdlRenderer {nullptr};

    std::unique_ptr<ShaderEffects> m_effects; // SDL GPU renderer only (available() false otherwise)
    std::unique_ptr<NetMatch> m_net;
    std::unique_ptr<NetworkMenu> m_networkMenu;
    std::vector<LanGame> m_lanGames;
    std::unique_ptr<Net::LanBrowser> m_lanBrowser; // while the network screen is open and not in a game
    std::unique_ptr<Net::MasterClient> m_master;   // the dedicated server (internet tab)
    std::vector<LanGame> m_serverRooms;
    std::string m_serverStatus;
    uint32_t m_serverListMs {0};
    uint32_t m_serverFailedMs {0};                   // when the server connection failed (retried after kServerRetryMs)
    static constexpr uint32_t kServerRetryMs = 3000; // room list refresh and server retry period
    bool m_serverJoinPending {false};                // waiting for the server's answer to create / find
    std::string m_serverOverride;                    // --server: instead of the environment's master host
    uint16_t m_serverOverridePort {0};
    int m_cliServerAction {0}; // 1 create, 2 find by code (from the command line)
    std::string m_cliServerCode;
    bool m_netAuto {false}; // command-line game: ready by itself, the room starts by itself
    uint32_t m_netAutoMs {0};
    void UpdateServer();
    // --- Replay (.awr): a recorded network game played back through the simulation (ApplicationReplay.cpp)
    std::unique_ptr<ReplayPlayer> m_replay;
    std::unique_ptr<ReplayMenu> m_replayMenu;
    MatchSetup m_replaySetup; // the recorded match, `local` = who is watched
    struct ReplayUi
    {
        bool paused {false};
        int speed {1}; // index into the speeds
        int watch {-1}; // slot whose view is shown; -1: the observer sees everything
        bool follow {false};
        bool finished {false};
        double accumulator {0.0};
        float lastCamX {0.0f}, lastCamY {0.0f}, lastCamZoom {0.0f};
        bool cameraSet {false};
        bool interfaceSound {false}; // "Voice the interface": off at the start of every replay, the interface is silent
        int comboOpen {-1};          // the view combobox: -1 closed, else it is open
        int comboScroll {0};
    } m_replayUi;
    bool m_replayMenuFresh {false}; // the list of the open replay screen was read since the screen opened
    std::string m_pendingReplay; // a file dropped on the window / given on the command line, opened when possible
    bool StartReplay(int watchSlot);
    void CloseReplay();
    void ReplayWatch(int slot);
    void UpdateReplay(double frameTime);
    void UpdateReplayInput(double frameTime);
    void RenderReplayBar();
    // The observer of a replay sees the whole map (drawing only: the simulation's fog is part of the match)
    bool ReplayShowsAll() const
    {
        return m_replay && m_replayUi.watch < 0;
    }
    bool m_textInput {false};
    void UpdateNetworkMenu();
    void LeaveNetworkGame();
    // Reads the simulation data (characters, research, projectiles, ...) again: a network game plays with the host's
    // files
    void ReloadGameData();
    int m_netWantedPlayers {2};
    std::string m_netMapPath;
    bool m_netMatchStarted {false};
    bool m_netMatchLoaded {false}; // the match world is set up and playing (or was)
    // The match's players as the roster last showed them (by slot), to notice changes and to name them on the
    // scoreboard after they have gone
    struct NetPlayer
    {
        std::string name;
        bool present {false}; // in the roster
        bool connected {false};
        bool left {false}; // was in the match and is gone for good
    };
    std::array<NetPlayer, 8> m_netPlayers {};
    struct Notice
    {
        std::string text;
        uint32_t untilMs {0};
    };
    std::vector<Notice> m_notices;
    std::unique_ptr<TextureManager> m_texManager;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<Camera> m_camera;
    std::unique_ptr<BitmapFontRenderer> m_fontRenderer;
    std::unique_ptr<InputManager> m_input;
    std::unique_ptr<SoundEngine> m_sound;

    std::unique_ptr<TileMap> m_map;
    std::unique_ptr<Pathfinder> m_pathfinder;
    std::unique_ptr<EntityManager> m_entities;
    std::unique_ptr<Economy> m_economy;
    std::unique_ptr<AIController> m_aiController;
    std::unique_ptr<MissionController> m_mission;
    std::unique_ptr<SaveManager> m_saveManager;
#if defined(AOW_DEBUG)
    std::unique_ptr<DebugOverlay> m_debug; // Dear ImGui debug overlay: debug builds only
#endif

    // Subsystem Controllers
    std::unique_ptr<MenuController> m_menuController;
    std::unique_ptr<HUDController> m_hudController;
    std::unique_ptr<GameplayController> m_gameplayController;
};

} // namespace OpenArtOfWar
