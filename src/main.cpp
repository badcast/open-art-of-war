#include "core/Application.hpp"
#include "core/LoaderResources.hpp"
#include <iostream>
#include <string>

#if OAOW_TOUCH
// Renames main() to SDL_main(): SDLActivity starts the game through it
#include <SDL3/SDL_main.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#endif

namespace
{
const char *const kHelp = R"(Usage: openaow [options]

Open Art Of War: Resistance Arisen. Run it from the folder that holds ./assets.

Single player:
  --map=<name|path>             start a custom game on this map (a name from assets/maps or a .json path)
  --from-scratch                start from scratch (only HQ and power plant, no units)
  --touch-ui                    the touch interface (bottom panels, tap to move, double tap to attack-move)
  --testmap, --test-map, -tm    open the test polygon (assets/maps/testmap.json)
  --replay=<file.awr>           watch a recorded network game (a bare .awr path works too; replays are saved in
                                the user's folder replays/ after every network game)

Network game (the same --map picks the match map):
  --host[=port]                 host a LAN game (default port 38100)
  --players=N                   with --host / --server-create: start once N players are in (default 2)
  --join=<address>[:port]       join a LAN game
  --server=<host>[:port]        play through a dedicated server (openaow-server)
  --server-create               with --server: create a room
  --server-code=<code>          with --server: join the room with this code
  --name=<player>               player name (default "Player")

Graphics:
  --renderer=<name>             render API for this run: gpu, vulkan, opengl, direct3d12, direct3d11,
                                direct3d, metal, software (what this system offers)

Debug builds only (cmake -DAOW_DEBUG=ON):
  --debug, -d                   debug overlay, navigation inspection, full control over the enemy

  --help, -h                    this text
  --version                     print the version
)";
} // namespace

int main(int argc, char *argv[])
{
#if defined(_WIN32)
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut == NULL || hOut == INVALID_HANDLE_VALUE || GetFileType(hOut) == FILE_TYPE_UNKNOWN)
        {
            FILE *fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
        }
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        if (hErr == NULL || hErr == INVALID_HANDLE_VALUE || GetFileType(hErr) == FILE_TYPE_UNKNOWN)
        {
            FILE *fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stderr);
        }
    }
#endif
    bool debugMode = false;
    bool testMapMode = false;
    bool touchUi = false;     // --touch-ui: the touch interface (the default on touch devices) with the mouse
    bool fromScratch = false; // --map without a network game: a custom game, --from-scratch ignores the map's own bases
    std::string rendererOverride;
    // Network game from the command line: --host[=port] --players=N --map=<name> | --join=<address>[:port],
    // --name=<player> Single player: --map=<name|path> [--from-scratch] starts a custom game on that map
    bool netHost = false;
    std::string netJoin;
    std::uint16_t netPort = 38100;
    int netPlayers = 2;
    std::string netMapName; // resolved through LoaderResources once the assets are found
    std::string netName = "Player";
    std::string netServer;
    std::string replayFile; // --replay=<file.awr>: watch a recorded network game
    bool netServerCreate = false;
    std::string netServerCode;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            std::cout << kHelp;
            return 0;
        }
        if (arg == "--version")
        {
            std::cout << "openaow " << OAOW_VERSION << std::endl;
            return 0;
        }
        if (arg == "--host" || arg.rfind("--host=", 0) == 0)
        {
            netHost = true;
            if (arg.size() > 7)
                netPort = static_cast<std::uint16_t>(std::stoi(arg.substr(7)));
        }
        if (arg.rfind("--join=", 0) == 0)
        {
            netJoin = arg.substr(7);
            const std::size_t colon = netJoin.rfind(':');
            if (colon != std::string::npos)
            {
                netPort = static_cast<std::uint16_t>(std::stoi(netJoin.substr(colon + 1)));
                netJoin.resize(colon);
            }
        }
        if (arg.rfind("--players=", 0) == 0)
            netPlayers = std::stoi(arg.substr(10));
        // Through a dedicated server: --server=host[:port] with --server-create or --server-code=<code>
        if (arg.rfind("--server=", 0) == 0)
            netServer = arg.substr(9);
        if (arg == "--server-create")
            netServerCreate = true;
        if (arg.rfind("--server-code=", 0) == 0)
            netServerCode = arg.substr(14);
        if (arg.rfind("--map=", 0) == 0)
            netMapName = arg.substr(6);
        if (arg.rfind("--replay=", 0) == 0)
            replayFile = arg.substr(9);
        else if (arg.size() > 4 && arg.compare(arg.size() - 4, 4, ".awr") == 0)
            replayFile = arg; // a replay file given as it is (opened with the game)
        if (arg == "--from-scratch")
            fromScratch = true;
        if (arg.rfind("--name=", 0) == 0)
            netName = arg.substr(7);
        if (arg.rfind("--renderer=", 0) == 0)
        {
            rendererOverride = arg.substr(11);
        }
#if defined(AOW_DEBUG)
        if (arg == "--debug" || arg == "-d")
        {
            debugMode = true;
        }
#endif
        if (arg == "--touch-ui")
            touchUi = true;
        if (arg == "--testmap" || arg == "--test-map" || arg == "-tm")
        {
            testMapMode = true;
#if defined(AOW_DEBUG)
            debugMode = true; // Test map runs with DEBUG mode active (debug builds only)
#endif
        }
    }

    std::cout << "========================================================" << std::endl;
    std::cout << "Open Art Of War: Resistance Arisen" << std::endl;
    if (debugMode)
    {
        std::cout << "[DEBUG MODE ENABLED: Navigation Inspection & Full Enemy Control]" << std::endl;
    }
    if (testMapMode)
    {
        std::cout << "[DIRECT TEST MAP LAUNCH: Test Polygon]" << std::endl;
    }
    std::cout << "========================================================" << std::endl;

    // A changed render API rebuilds the whole window: run() returns and a fresh Application takes over
    bool restart = false;
    do
    {
        OpenArtOfWar::Application app;
        app.SetRendererOverride(restart ? std::string() : rendererOverride);
        if (touchUi)
            app.SetTouchUi(true);
        if (!app.Init(1280, 720))
        {
            std::cerr << "Engine initialization failed." << std::endl;
            return 1;
        }
        app.SetDebugMode(debugMode);
        if (testMapMode && !restart)
        {
            app.LaunchTestMap();
        }
        // --map=<name> alone: a single-player custom game on that map (with --host / --server it is the match map)
        const bool netGame
            = netHost || !netJoin.empty() || (!netServer.empty() && (netServerCreate || !netServerCode.empty()));
        if (!restart && !testMapMode && !netGame && !netMapName.empty())
            app.LaunchMap(netMapName, fromScratch);
        if (!restart && netHost)
        {
            app.HostNetworkGame(
                netPort,
                netPlayers,
                netMapName.empty() ? OpenArtOfWar::LoaderResources::NetDefaultMap()
                                   : OpenArtOfWar::LoaderResources::MapPath(netMapName),
                netName,
                fromScratch);
            // --server too: the hosted game is listed there and reachable by its code
            if (!netServer.empty())
            {
                std::string host = netServer;
                std::uint16_t port = 38100;
                const std::size_t colon = host.rfind(':');
                if (colon != std::string::npos)
                {
                    port = static_cast<std::uint16_t>(std::stoi(host.substr(colon + 1)));
                    host.resize(colon);
                }
                app.AnnounceNetworkGame(host, port);
            }
        }
        if (!restart && !replayFile.empty())
            app.OpenReplay(replayFile);
        if (!restart && !netJoin.empty())
            app.JoinNetworkGame(netJoin, netPort, netName);
        if (!restart && !netServer.empty() && (netServerCreate || !netServerCode.empty()))
        {
            std::string host = netServer;
            std::uint16_t port = 38100;
            const std::size_t colon = host.rfind(':');
            if (colon != std::string::npos)
            {
                port = static_cast<std::uint16_t>(std::stoi(host.substr(colon + 1)));
                host.resize(colon);
            }
            app.ServerNetworkGame(
                host,
                port,
                netServerCreate ? std::string() : netServerCode,
                netPlayers,
                netMapName.empty() ? OpenArtOfWar::LoaderResources::NetDefaultMap()
                                   : OpenArtOfWar::LoaderResources::MapPath(netMapName),
                netName,
                fromScratch);
        }

        app.Run();
        restart = app.RestartRequested();
    } while (restart);
    std::cout << "Engine shut down cleanly." << std::endl;
    return 0;
}
