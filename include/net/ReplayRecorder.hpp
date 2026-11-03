#pragma once

#include "game/Match.hpp"
#include "openaow/net/Protocol.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace OpenArtOfWar
{

// Records the network match this computer plays, for a replay: the setup, every turn the host closed, the world
// checksums, and (so the file plays on its own) the map and the simulation data files. Written to the user's
// own directory, never next to the game: <user dir>/replays/open-art-of-war-YYYYMMDD-HHMMSS_<map>.awr
class ReplayRecorder
{
public:
    struct Info
    {
        std::vector<uint8_t> setupBytes; // the StartMatch setup as the room sent it
        MatchSetup setup;                // the same, decoded (local slot set)
        std::vector<std::string> names;  // by slot; empty for a computer player
        uint32_t ticksPerTurn {2};
        uint32_t turnMs {100};
        uint64_t assetsHash {0};
        // What was played: "network", "custom" (a custom game of this computer) or "campaign" (then the mission,
        // the side of the player and the difficulty of the computer set it up instead of the setup)
        std::string mode {"network"};
        int mission {-1};
        int playerFaction {0};
        int difficulty {1};
    };

    bool Active() const
    {
        return m_active;
    }
    // A match begins (also after a reconnect, which plays the same turns again: they are kept once)
    void Begin(Info info);
    void OnBundle(const Net::TurnBundle &bundle);
    void OnChecksum(uint32_t tick, uint64_t hash);
    // Writes the file; one call per match (later calls do nothing). `outcome`: "victory", "defeat", "left",
    // "disconnected". Returns the path, empty when nothing was written.
    std::string Finish(const std::string &outcome, uint32_t tick);

    // The folder of the user's replays
    static std::string Directory();

private:
    bool m_active {false};
    Info m_info;
    std::map<uint32_t, std::vector<uint8_t>> m_turns;
    std::map<uint32_t, uint64_t> m_checksums;
    int64_t m_startedAt {0};
};

} // namespace OpenArtOfWar
