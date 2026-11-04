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
    uint8_t handicap {0}; // start credits: index into kHandicapPercent (0 = none)
    uint8_t armyCap {7};  // most units alive at once: index into kArmyCapUnits (7 = unlimited)
};

// The army size limit of a player: the choices of the lobby / custom game (0 units = unlimited)
constexpr int kArmyCapCount = 8;
constexpr int kArmyCapUnits[kArmyCapCount] = {10, 25, 50, 100, 200, 500, 1000, 0};
inline int ArmyCapUnitsOf(int index)
{
    return kArmyCapUnits[(index >= 0 && index < kArmyCapCount) ? index : kArmyCapCount - 1];
}

// Handicaps ("forms"): the share of the match's starting credits a player gets
constexpr int kHandicapCount = 6;
constexpr int kHandicapPercent[kHandicapCount] = {100, 75, 50, 125, 150, 200};
inline int HandicapPercentOf(int index)
{
    return kHandicapPercent[(index >= 0 && index < kHandicapCount) ? index : 0];
}

namespace Players
{

// Single player: slot 0 the person with `human`, slot 1 the computer with the other faction
void SetupSinglePlayer(Faction human);
// A network / custom match with the given slots; `local` is the slot of this computer
void Setup(const std::array<PlayerSlot, kMaxPlayers> &slots, PlayerId local);
inline void setup(const std::array<PlayerSlot, kMaxPlayers> &slots, PlayerId local)
{
    Setup(slots, local);
}

const PlayerSlot &Slot(PlayerId id);
bool IsActive(PlayerId id);
PlayerId Local();
int Count(); // active slots
inline int count()
{
    return Count();
}

Faction FactionOf(PlayerId id);
uint8_t TeamOf(PlayerId id);
// The first active player of a faction (single player: one each); kNoPlayer for Neutral
PlayerId OwnerOf(Faction faction);
// Different teams. Nobody's things count as hostile to everybody, as a third faction did before.
bool Hostile(PlayerId a, PlayerId b);

} // namespace Players
} // namespace OpenArtOfWar
