#pragma once

#include "game/Players.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace OpenArtOfWar
{

    // Everything a match of several players starts from. A network host decides it (random factions and
    // start positions already resolved with the match seed) and sends it to every peer, so all of them
    // build the same world.
    struct MatchSetup
    {
        std::string mapPath;                              // assets/maps/<name>.json
        uint64_t mapHash {0};                             // of the map file everybody plays on
        uint32_t seed {0};                                // gameplay random numbers (Simulation::reset)
        std::array<PlayerSlot, kMaxPlayers> slots {};     // active ones take part
        std::array<int8_t, kMaxPlayers> startPosition {}; // index into the map's start positions per slot
        PlayerId local {0};                               // this computer's slot
        int startingCredits {1500};
        bool fogOfWar {true};
        bool prebuilt {false}; // use the buildings and units the map itself has (else everybody starts from the usual base)
    };

} // namespace OpenArtOfWar
