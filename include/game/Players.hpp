#pragma once

#include "core/Config.hpp"
#include <array>
#include <cstdint>

namespace OpenArtOfWar
{

    // A player of the match: up to 8 slots. The faction is the side the player plays (units, buildings,
    // technologies, sprites); the team decides who is an enemy, so two players of the same faction can
    // fight each other and players of both factions can be allies.
    using PlayerId = uint8_t;
    constexpr int kMaxPlayers = 8;
    constexpr PlayerId kNoPlayer = 0xFF; // nobody's (neutral)

    struct PlayerSlot
    {
        bool active {false};
        Faction faction {Faction::Resistance};
        uint8_t team {0};
        uint8_t color {0}; // army colour (0 = the faction's classic sheets)
        bool human {false};
        uint8_t aiLevel {1}; // computer player: 0 easy .. 3 hardcore
    };

    namespace Players
    {

        // Single player: slot 0 the person with `human`, slot 1 the computer with the other faction
        void SetupSinglePlayer(Faction human);
        // A network / custom match with the given slots; `local` is the slot of this computer
        void setup(const std::array<PlayerSlot, kMaxPlayers> &slots, PlayerId local);

        const PlayerSlot &Slot(PlayerId id);
        bool IsActive(PlayerId id);
        PlayerId Local();
        int count(); // active slots

        Faction FactionOf(PlayerId id);
        uint8_t TeamOf(PlayerId id);
        // The first active player of a faction (single player: one each); kNoPlayer for Neutral
        PlayerId OwnerOf(Faction faction);
        // Different teams. Nobody's things count as hostile to everybody, as a third faction did before.
        bool Hostile(PlayerId a, PlayerId b);

    } // namespace Players
} // namespace OpenArtOfWar
