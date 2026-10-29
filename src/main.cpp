#include "core/Application.hpp"
#include "core/AssetStore.hpp"
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char *argv[])
{
    bool testMode = false;
    bool debugMode = false;
    bool testMapMode = false;
    std::string rendererOverride;
    // Network game from the command line: --host[=port] --players=N --map=<name> | --join=<address>[:port], --name=<player>
    bool netHost = false;
    std::string netJoin;
    uint16_t netPort = 38100;
    int netPlayers = 2;
    std::string netMap = "assets/maps/m26.json";
    std::string netName = "Player";
    std::string netServer;
    bool netServerCreate = false;
    std::string netServerCode;
    for(int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if(arg == "--test" || arg == "--headless" || arg == "--headless-test" || arg == "-t")
        {
            testMode = true;
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
            netMap = "assets/maps/" + arg.substr(6) + ".json";
        if(arg.rfind("--name=", 0) == 0)
            netName = arg.substr(7);
        if(arg.rfind("--renderer=", 0) == 0)
        {
            rendererOverride = arg.substr(11);
        }
        if(arg == "--debug" || arg == "-d")
        {
            debugMode = true;
        }
        if(arg == "--testmap" || arg == "--test-map" || arg == "-tm")
        {
            testMapMode = true;
            debugMode = true; // Test map always runs with DEBUG mode active
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
        std::cout << "[DIRECT TEST MAP LAUNCH: Mission 8 - Test Polygon]" << std::endl;
    }
    std::cout << "========================================================" << std::endl;

    // The self-test works in a throw-away user directory: it must neither read nor change the player's files
    std::filesystem::path testUserDir;
    if(testMode)
    {
        testUserDir = std::filesystem::temp_directory_path() / "openaow-test-user";
        std::error_code ec;
        std::filesystem::remove_all(testUserDir, ec);
        OpenArtOfWar::Assets::setUserDataDir(testUserDir.string());
    }

    // A changed render API rebuilds the whole window: run() returns and a fresh Application takes over
    bool restart = false;
    do
    {
        OpenArtOfWar::Application app;
        app.setRendererOverride(restart ? std::string() : rendererOverride);
        app.setHeadless(testMode);
        if(!app.init(1280, 720))
        {
            std::cerr << "Engine initialization failed." << std::endl;
            return 1;
        }
        app.setDebugMode(debugMode);
        if(testMapMode && !restart)
        {
            app.launchTestMap();
        }
        if(!restart && netHost)
        {
            app.hostNetworkGame(netPort, netPlayers, netMap, netName);
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
                app.announceNetworkGame(host, port);
            }
        }
        if(!restart && !netJoin.empty())
            app.joinNetworkGame(netJoin, netPort, netName);
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
            app.serverNetworkGame(host, port, netServerCreate ? std::string() : netServerCode, netPlayers, netMap, netName);
        }

        if(testMode)
        {
            bool ok = app.runHeadlessTest(200);
            std::cout << "Headless test result: " << (ok ? "SUCCESS" : "FAILED") << std::endl;
            std::error_code ec;
            std::filesystem::remove_all(testUserDir, ec);
            return ok ? 0 : 1;
        }

        app.run();
        restart = app.restartRequested();
    } while(restart);
    std::cout << "Engine shut down cleanly." << std::endl;
    return 0;
}
