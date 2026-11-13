#pragma once

#include "game/Match.hpp"
#include "openaow/net/Protocol.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace OpenArtOfWar
{

class Simulation;

// A recorded network match (.awr) opened for watching: what the file says about the match, and the means to play it
// back through the simulation exactly as the network ran it (the turns are scheduled turn by turn, the world
// checksums of the file are compared with the replayed ones).
class ReplayPlayer
{
public:
    struct Player
    {
        int slot {0};
        std::string name;
        Faction faction {Faction::Resistance};
        int team {0};
        int color {0};
        bool human {false};
        int aiLevel {1};
        std::vector<std::string> addresses; // a server's replay only
    };
    struct Info
    {
        std::string file;   // path on disk
        std::string source; // "client" or "server"
        std::string gameVersion;
        std::vector<std::string> mods; // "name version" of the mods it was recorded with
        std::string map;               // the map's path as the room named it
        std::string outcome;           // "victory", "defeat", "left", ... (a client's replay)
        std::int64_t recordedAt {0};
        std::uint64_t mapHash {0};
        std::uint32_t turns {0};
        std::uint32_t ticks {0}; // length of the playback
        std::uint32_t ticksPerTurn {2};
        int localSlot {-1};
        int protocol {0};
        std::uint64_t simBuildId {0}; // the simulation that recorded it (0: an older file that does not say)
        // Its checksums were computed the way this build computes them: only then are they compared
        bool checksumsComparable {false};
        bool carriesData {false};     // the file holds its map and simulation data
        std::string mode {"network"}; // "network", "custom" or "campaign"
        int mission {-1};             // campaign: the mission, the side of the player, the computer's difficulty
        int playerFaction {0};
        int difficulty {1};
        std::uint64_t fileBytes {0}; // size of the file on disk
        std::vector<Player> players;
    };

    ~ReplayPlayer();

    // Reads and checks a file. False with `error` (translated) for anything that is not a replay this game can play.
    bool Load(std::string_view path, std::string &error);
    // Only what a list shows
    static bool ReadInfo(std::string_view path, Info &out, std::string &error);

    const Info &info() const
    {
        return m_info;
    }
    // The match to set up: the recorded one with `local` = the player to watch. Puts the file's simulation data and
    // map in place (a session overlay; the caller reloads the game data). False with `error` when the map is missing.
    bool PrepareMatch(int watchSlot, MatchSetup &out, std::string &error);
    // The turn that starts on `tick` goes into the simulation (call before the tick runs)
    void ScheduleDue(Simulation &sim, std::uint32_t tick);
    bool Finished(std::uint32_t tick) const
    {
        return tick >= m_info.ticks;
    }
    // After a tick: compares the world's checksum with the recorded one (at the ticks that have one)
    void Verify(std::uint32_t tick, std::uint64_t checksum);
    bool NeedsChecksum(std::uint32_t tick) const
    {
        return m_checksums.count(tick) != 0;
    }
    // First tick whose checksum differed from the recording; -1 while all matched
    std::int64_t MismatchTick() const
    {
        return m_mismatchTick;
    }
    // Where the player's camera was at `tick` (centre in world pixels, zoom); false when the file has no view of him
    bool CameraOf(int slot, std::uint32_t tick, float &x, float &y, float &zoom) const;
    // Back to the first turn (the world is set up again by the caller)
    void Rewind();

private:
    struct View
    {
        std::uint32_t tick {0};
        float x {0.0f}, y {0.0f}, zoom {2.0f};
    };
    Info m_info;
    std::vector<std::vector<std::uint8_t>> m_turns;
    std::vector<std::uint8_t> m_setup;
    std::map<std::uint32_t, std::uint64_t> m_checksums;
    std::map<std::string, std::vector<std::uint8_t>> m_files;
    std::map<int, std::vector<View>> m_views; // by slot, ascending ticks
    std::int64_t m_mismatchTick {-1};
    std::int64_t m_lastScheduledTurn {-1};
    bool m_overlayInstalled {false};
};

} // namespace OpenArtOfWar
