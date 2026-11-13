#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace OpenArtOfWar
{

// A kind of mine: assets/characters/mines.yaml (one file for all of them). The Resistance lays them like wall
// segments once the technology (research effect `mine`, its `value` = researchValue) is done.
// The file is part of the network assets check: damage, radii and cost change the simulation.
struct MineSpec
{
    enum class Trigger
    {
        Any,      // infantry and vehicles
        Infantry, // infantry only
        Vehicle   // vehicles only
    };

    std::string key;         // name in the file
    std::string name;        // gettext msgid shown on the build menu
    std::string desc;        // gettext msgid
    int researchValue {0};   // `value` of the research (effect: mine) that unlocks it
    int iconComp {-1};       // cameo of the build menu (sprite handle, -1: none)
    int cost {40};           // credits
    float armSeconds {3.0f}; // after laying, the mine does not fire for this long
    Trigger trigger {Trigger::Any};
    float triggerRadius {0.8f}; // tiles from the centre of the mine's tile: a hostile unit this close fires it
    float blastRadius {1.0f};   // tiles: every hostile unit inside takes the damage
    int damage {120};
    int maxPerPlayer {30}; // mines a player may have laid at once
    int explosion {-1};    // animation of the blast (sprite handle, -1: none)
};

namespace Mines
{
// The kinds in file order (the build menu's order)
const std::vector<MineSpec> &Specs();
// Reads the file again (a game started, the data of a network host arrived)
void Reload();
// Reads the text of a mines.yaml over the built-in defaults; false when it is not valid
bool Apply(std::string_view yamlText);
} // namespace Mines

} // namespace OpenArtOfWar
