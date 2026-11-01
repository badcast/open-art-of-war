#include "core/Application.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
    const char *const kHelp = R"(Usage: openaow [options]

Open Art Of War: Freedom for Liberty. Run it from the folder that holds ./assets.

Single player:
  --map=<name|path>             start a custom game on this map (a name from assets/maps or a .json path)
  --from-scratch                start from scratch (only HQ and power plant, no units)
  --testmap, --test-map, -tm    open the test polygon (assets/maps/testmap.json)

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
  --test, --headless, -t        run the headless self-tests and exit (0 = success)

  --help, -h                    this text
  --version                     print the version
)";
} // namespace

int main(int argc, char *argv[])
{
    bool testMode = false;
    bool debugMode = false;
    bool testMapMode = false;
    bool fromScratch = false; // --map without a network game: a custom game, --from-scratch ignores the map's own bases
    std::string rendererOverride;
    // Network game from the command line: --host[=port] --players=N --map=<name> | --join=<address>[:port], --name=<player>
    // Single player: --map=<name|path> [--from-scratch] starts a custom game on that map
    bool netHost = false;
    std::string netJoin;
    uint16_t netPort = 38100;
    int netPlayers = 2;
    std::string netMapName; // resolved through LoaderResources once the assets are found
    std::string netName = "Player";
    std::string netServer;
    bool netServerCreate = false;
    std::string netServerCode;
    for(int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if(arg == "--help" || arg == "-h")
        {
            std::cout << kHelp;
            return 0;
        }
        if(arg == "--version")
        {
            std::cout << "openaow " << OAOW_VERSION << std::endl;
            return 0;
        }
        if(arg == "--test" || arg == "--headless" || arg == "--headless-test" || arg == "-t")
        {
#if defined(AOW_DEBUG)
            testMode = true;
#else
            std::cerr << "[Main] " << arg << " needs a debug build (cmake -DAOW_DEBUG=ON); ignored" << std::endl;
#endif
        }
        if(arg == "--host" || arg.rfind("--host=", 0) == 0)
        {
            netHost = true;
            if(arg.size() > 7)
                netPort = static_cast<uint16_t>(std::stoi(arg.substr(7)));
        }
        if(arg.rfind("--join=", 0) == 0)
        {
            netJoin = arg.substr(7);
            const size_t colon = netJoin.rfind(':');
            if(colon != std::string::npos)
            {
                netPort = static_cast<uint16_t>(std::stoi(netJoin.substr(colon + 1)));
                netJoin.resize(colon);
            }
        }
        if(arg.rfind("--players=", 0) == 0)
            netPlayers = std::stoi(arg.substr(10));
        // Through a dedicated server: --server=host[:port] with --server-create or --server-code=<code>
        if(arg.rfind("--server=", 0) == 0)
            netServer = arg.substr(9);
        if(arg == "--server-create")
            netServerCreate = true;
        if(arg.rfind("--server-code=", 0) == 0)
            netServerCode = arg.substr(14);
        if(arg.rfind("--map=", 0) == 0)
            netMapName = arg.substr(6);
        if(arg == "--from-scratch")
            fromScratch = true;
        if(arg.rfind("--name=", 0) == 0)
            netName = arg.substr(7);
        if(arg.rfind("--renderer=", 0) == 0)
        {
            rendererOverride = arg.substr(11);
        }
        if(arg == "--debug" || arg == "-d")
        {
#if defined(AOW_DEBUG)
            debugMode = true;
#else
            std::cerr << "[Main] " << arg << " needs a debug build (cmake -DAOW_DEBUG=ON); ignored" << std::endl;
#endif
        }
        if(arg == "--testmap" || arg == "--test-map" || arg == "-tm")
        {
            testMapMode = true;
#if defined(AOW_DEBUG)
            debugMode = true; // Test map runs with DEBUG mode active (debug builds only)
#endif
        }
    }

    std::cout << "========================================================" << std::endl;
    std::cout << "Open Art Of War: Freedom for Liberty" << std::endl;
    if(debugMode)
    {
        std::cout << "[DEBUG MODE ENABLED: Navigation Inspection & Full Enemy Control]" << std::endl;
    }
    if(testMapMode)
    {
        std::cout << "[DIRECT TEST MAP LAUNCH: Test Polygon]" << std::endl;
    }
    std::cout << "========================================================" << std::endl;

    // The self-test works in a throw-away user directory: it must neither read nor change the player's files
    std::filesystem::path testUserDir;
    if(testMode)
    {
        testUserDir = std::filesystem::temp_directory_path() / "openaow-test-user";
        std::error_code ec;
        std::filesystem::remove_all(testUserDir, ec);
        OpenArtOfWar::Assets::SetUserDataDir(testUserDir.string());
    }

    // A changed render API rebuilds the whole window: run() returns and a fresh Application takes over
    bool restart = false;
    do
    {
        OpenArtOfWar::Application app;
        app.SetRendererOverride(restart ? std::string() : rendererOverride);
        app.SetHeadless(testMode);
        if(!app.Init(1280, 720))
        {
            std::cerr << "Engine initialization failed." << std::endl;
            return 1;
        }
        app.SetDebugMode(debugMode);
        if(testMapMode && !restart)
        {
            app.LaunchTestMap();
        }
        // --map=<name> alone: a single-player custom game on that map (with --host / --server it is the match map)
        const bool netGame = netHost || !netJoin.empty() || (!netServer.empty() && (netServerCreate || !netServerCode.empty()));
        if(!restart && !testMapMode && !testMode && !netGame && !netMapName.empty())
            app.LaunchMap(netMapName, fromScratch);
        if(!restart && netHost)
        {
            app.HostNetworkGame(netPort, netPlayers, netMapName.empty() ? OpenArtOfWar::LoaderResources::NetDefaultMap() : OpenArtOfWar::LoaderResources::MapPath(netMapName), netName, fromScratch);
            // --server too: the hosted game is listed there and reachable by its code
            if(!netServer.empty())
            {
                std::string host = netServer;
                uint16_t port = 38100;
                const size_t colon = host.rfind(':');
                if(colon != std::string::npos)
                {
                    port = static_cast<uint16_t>(std::stoi(host.substr(colon + 1)));
                    host.resize(colon);
                }
                app.AnnounceNetworkGame(host, port);
            }
        }
        if(!restart && !netJoin.empty())
            app.JoinNetworkGame(netJoin, netPort, netName);
        if(!restart && !netServer.empty() && (netServerCreate || !netServerCode.empty()))
        {
            std::string host = netServer;
            uint16_t port = 38100;
            const size_t colon = host.rfind(':');
            if(colon != std::string::npos)
            {
                port = static_cast<uint16_t>(std::stoi(host.substr(colon + 1)));
                host.resize(colon);
            }
            app.ServerNetworkGame(host, port, netServerCreate ? std::string() : netServerCode, netPlayers, netMapName.empty() ? OpenArtOfWar::LoaderResources::NetDefaultMap() : OpenArtOfWar::LoaderResources::MapPath(netMapName), netName, fromScratch);
        }

#if defined(AOW_DEBUG)
        if(testMode)
        {
            bool ok = app.RunHeadlessTest(200);
            std::cout << "Headless test result: " << (ok ? "SUCCESS" : "FAILED") << std::endl;
            std::error_code ec;
            std::filesystem::remove_all(testUserDir, ec);
            return ok ? 0 : 1;
        }
#endif

        app.run();
        restart = app.RestartRequested();
    } while(restart);
    std::cout << "Engine shut down cleanly." << std::endl;
    return 0;
}
