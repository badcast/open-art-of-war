#pragma once

#include "core/Config.hpp"
#include "game/Players.hpp"
#include <cstdint>
#include <vector>

namespace OpenArtOfWar
{

// Everything a player does to the world goes through a command: the interface issues it, the simulation
// applies it at the start of a tick. A single-player game applies it on the tick it was issued; a network
// game sends it to the host and every peer applies it on the same tick. Recorded, the commands of a
// match replay it exactly.
enum class CommandType : std::uint8_t
{
    OrderAt,       // ids: units; x, y: world pixel; a: OrderKind; b: march as a control group (1)
    AttackWall,    // ids: units; x, y: wall tile
    SetRally,      // ids: Barracks / Factories; x, y: world pixel
    ToggleSiege,   // ids: units
    Stop,          // ids: units
    HoldPosition,  // ids: units (stop moving, keep fighting from where they stand)
    PlaceBuilding, // a: blueprint index; x, y: tile
    Produce,       // a: unit archetype; b: building id; c: how many (0: one)
    CancelProduce, // a: unit archetype; b: building id
    StartResearch, // a: technology index
    Demolish,      // ids: own buildings
    DebugCredits,  // a: amount (debug toolbar)
    DebugSpawn,    // a: archetype / building index; b: DebugSpawnKind; c, d: footprint; x, y: tile
    PlayerLeft,    // the player left a network match: its buildings, walls and units are destroyed
                   // (only the network session issues it, from the host's turn stream)
    SetStance,     // ids: units; a: UnitStance
    ToggleRepair,  // ids: own buildings (repair on when any of them is not repairing, else off)
    View,          // x, y: where the player's camera looks (world pixel); a: zoom x100. Changes nothing in the world:
                   // it only travels with the turns, so a replay can follow a player's camera
    SetRepeat,     // a: unit archetype; b: building id: the building trains that unit again and again (a second
                   // command with the same unit turns it off)
    Count
};

enum class OrderKind : std::uint8_t
{
    Move = 0,
    AttackMove = 1
};
enum class DebugSpawnKind : std::uint8_t
{
    Unit = 0,
    Building = 1,
    Wall = 2
};

struct SimCommand
{
    CommandType type {CommandType::Stop};
    PlayerId player {0};       // who issues it (a network session sets it from the sender)
    bool debug {false};        // debug mode: may command units of any faction
    std::vector<std::uint32_t> ids; // ascending, no duplicates
    std::int32_t x {0};
    std::int32_t y {0};
    std::int32_t a {0};
    std::int32_t b {0};
    std::int32_t c {0};
    std::int32_t d {0};
};

// A command with the tick it is applied on (replays, network turns)
struct TimedCommand
{
    std::uint32_t tick {0};
    SimCommand command;
};

} // namespace OpenArtOfWar
