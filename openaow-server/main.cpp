// openaow-server: the dedicated server of Open Art Of War (rooms, room codes, the public room list).
// No graphics, no game code: it closes the turns of each room, relays the players' commands and
// compares their world checksums. See --help.
#include "openaow/net/Net.hpp"
#include "openaow/net/RoomServer.hpp"
#include "openaow/net/StatusPage.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using namespace OpenArtOfWar::Net;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }

enum class Level { Trace, Debug, Info, Warn, Error };

struct Log {
    Level level{Level::Info};
    std::ofstream file;

    void write(Level l, const std::string& text) {
        if (l < level) return;
        static const char* const kNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        const std::string line = std::string(stamp) + " [" + kNames[static_cast<int>(l)] + "] " + text;
        (l >= Level::Warn ? std::cerr : std::cout) << line << std::endl;
        if (file.is_open()) file << line << std::endl;
    }
};

const char* const kHelp = R"(openaow-server - dedicated server of Open Art Of War

Players open the public room list or enter a room code in the game (Network game -> Internet); the
first player in a room picks the map and starts the match. Every connection is outgoing from the
players, so they need no forwarded ports; the server needs its UDP ports open.

Options (command line wins over --config):
  --config <file.json>        options as JSON keys without the dashes ("port": 38100, ...)
  --bind <address>            listen on this address only (default: all)
  --port <n>                  master UDP port: room list, codes (default 38100)
  --room-ports <first-last>   UDP ports of the rooms (default 38110-38199)
  --tcp-port <n>              HTTP status page with the rooms as JSON (default 38100, 0 = off)
  --max-rooms <n>             rooms at the same time (default 64)
  --max-players <n>           players per room, 2..8 (default 8)
  --max-clients <n>           connections to the master port (default 512)
  --turn-ms <n>               length of a lockstep turn (default 100)
  --input-delay-min <n>       turns between a command and its execution, at least (default 1)
  --input-delay-max <n>       ... at most, however slow the players' connections (default 6)
  --idle-room-timeout-s <n>   a room with a running match nobody is in closes after this (default 120)
  --empty-room-timeout-s <n>  a lobby nobody is in closes after this (default 30)
  --unused-room-timeout-s <n> a room nobody entered closes after this (default 120)
  --name <text>               server name shown to players
  --motd <text>               message of the day shown to players
  --log-level <level>         trace, debug, info, warn, error (default info)
  --log-file <file>           also write the log there
  --stats-interval-s <n>      a line with rooms / players every n seconds (default 60, 0 = off)
  --version                   print the version and exit
  --help                      this text
)";

bool parsePorts(const std::string& text, uint16_t& first, uint16_t& last) {
    const size_t dash = text.find('-');
    if (dash == std::string::npos) return false;
    const int a = std::stoi(text.substr(0, dash)), b = std::stoi(text.substr(dash + 1));
    if (a <= 0 || b > 65535 || a > b) return false;
    first = static_cast<uint16_t>(a);
    last = static_cast<uint16_t>(b);
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    // Options: defaults, then the config file, then the command line
    nlohmann::json options = nlohmann::json::object();
    std::vector<std::pair<std::string, std::string>> cli;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << kHelp;
            return 0;
        }
        if (arg == "--version") {
            std::cout << "openaow-server " << OAOW_VERSION << " (protocol " << kProtocolVersion << ")" << std::endl;
            return 0;
        }
        if (arg.rfind("--", 0) != 0) {
            std::cerr << "Unknown argument: " << arg << " (see --help)" << std::endl;
            return 2;
        }
        std::string key = arg.substr(2), value;
        const size_t eq = key.find('=');
        if (eq != std::string::npos) {
            value = key.substr(eq + 1);
            key.resize(eq);
        } else if (i + 1 < argc) {
            value = argv[++i];
        } else {
            std::cerr << "Missing value for --" << key << std::endl;
            return 2;
        }
        if (key == "config") {
            std::ifstream in(value);
            const nlohmann::json file = nlohmann::json::parse(in, nullptr, false, true);
            if (!file.is_object()) {
                std::cerr << "Cannot read the config file " << value << std::endl;
                return 2;
            }
            for (auto it = file.begin(); it != file.end(); ++it) options[it.key()] = it.value();
        } else {
            cli.push_back({key, value});
        }
    }
    for (const auto& [key, value] : cli) options[key] = value;

    auto text = [&](const char* key, const std::string& fallback) {
        if (!options.contains(key)) return fallback;
        const auto& v = options[key];
        return v.is_string() ? v.get<std::string>() : v.dump();
    };
    auto number = [&](const char* key, long fallback) -> long {
        if (!options.contains(key)) return fallback;
        try {
            return std::stol(text(key, ""));
        } catch (...) {
            std::cerr << "Not a number: --" << key << std::endl;
            std::exit(2);
        }
    };

    static const char* const kKnown[] = {"bind", "port", "room-ports", "tcp-port", "max-rooms", "max-players", "max-clients",
                                         "turn-ms", "input-delay-min", "input-delay-max", "idle-room-timeout-s", "empty-room-timeout-s",
                                         "unused-room-timeout-s", "name", "motd", "log-level", "log-file", "stats-interval-s"};
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (std::find(std::begin(kKnown), std::end(kKnown), it.key()) == std::end(kKnown)) {
            std::cerr << "Unknown option: " << it.key() << " (see --help)" << std::endl;
            return 2;
        }
    }

    Log log;
    const std::string level = text("log-level", "info");
    static const char* const kLevels[] = {"trace", "debug", "info", "warn", "error"};
    for (int i = 0; i < 5; ++i) if (level == kLevels[i]) log.level = static_cast<Level>(i);
    if (options.contains("log-file")) {
        log.file.open(text("log-file", ""), std::ios::app);
        if (!log.file) {
            std::cerr << "Cannot write the log file" << std::endl;
            return 2;
        }
    }

    RoomServerConfig config;
    config.bindAddress = text("bind", "");
    config.port = static_cast<uint16_t>(number("port", kDefaultPort));
    if (options.contains("room-ports") && !parsePorts(text("room-ports", ""), config.roomPortFirst, config.roomPortLast)) {
        std::cerr << "--room-ports wants first-last, e.g. 38110-38199" << std::endl;
        return 2;
    }
    config.maxRooms = static_cast<int>(number("max-rooms", 64));
    config.maxPlayersPerRoom = static_cast<int>(std::clamp<long>(number("max-players", kMaxPlayers), 2, kMaxPlayers));
    config.maxMasterClients = static_cast<int>(number("max-clients", 512));
    config.turnMs = static_cast<uint16_t>(std::clamp<long>(number("turn-ms", 100), 20, 500));
    config.inputDelayMin = static_cast<uint8_t>(std::clamp<long>(number("input-delay-min", 1), 1, 20));
    config.inputDelayMax = static_cast<uint8_t>(std::clamp<long>(number("input-delay-max", 6), config.inputDelayMin, 30));
    config.idleRoomTimeoutMs = static_cast<uint32_t>(number("idle-room-timeout-s", 120) * 1000);
    config.emptyRoomTimeoutMs = static_cast<uint32_t>(number("empty-room-timeout-s", 30) * 1000);
    config.unusedRoomTimeoutMs = static_cast<uint32_t>(number("unused-room-timeout-s", 120) * 1000);
    config.name = text("name", "OpenAOW server");
    config.motd = text("motd", "");
    const long tcpPort = number("tcp-port", kDefaultPort);
    const long statsEvery = number("stats-interval-s", 60);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    RoomServer server(config);
    std::string error;
    if (!server.start(error)) {
        log.write(Level::Error, "cannot open the master port " + std::to_string(config.port) + ": " + error);
        return 1;
    }
    StatusPage status;
    if (tcpPort > 0 && !status.open(static_cast<uint16_t>(tcpPort), config.bindAddress, error)) {
        log.write(Level::Warn, "status page off: " + error);
    }
    log.write(Level::Info, "openaow-server " + std::string(OAOW_VERSION) + " '" + config.name + "' on UDP " +
                               std::to_string(config.port) + ", rooms on " + std::to_string(config.roomPortFirst) + "-" +
                               std::to_string(config.roomPortLast) + (tcpPort > 0 ? ", status on TCP " + std::to_string(tcpPort) : ""));

    const auto start = std::chrono::steady_clock::now();
    auto lastStats = start;
    while (!g_stop) {
        const auto now = std::chrono::steady_clock::now();
        const uint32_t nowMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        server.update(nowMs);
        for (const std::string& line : server.takeLog()) log.write(Level::Info, line);
        status.serve(server.statusJson());
        if (statsEvery > 0 && now - lastStats >= std::chrono::seconds(statsEvery)) {
            lastStats = now;
            const RoomServer::Stats s = server.stats();
            log.write(Level::Info, "rooms " + std::to_string(s.rooms) + ", matches " + std::to_string(s.matches) + ", players " +
                                       std::to_string(s.players) + ", browsing " + std::to_string(s.masterClients));
        }
        // Turns are 100 ms; a millisecond of sleep keeps the CPU idle and the latency invisible
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    log.write(Level::Info, "stopping");
    return 0;
}
