#pragma once

#include "core/Config.hpp"
#include "game/Players.hpp"

#include <string>
#include <vector>

namespace OpenArtOfWar
{

class EntityManager;

// Tech levels (data/capabilities.yaml): per level and faction, the buildings, units and researches a player may use.
// Each player plays at one level (the match's, or its slot's own); everything outside it cannot be built, trained or
// researched and the interface does not show it. A research that unlocks a unit the level does not list, or that
// needs a research it does not list, is not available either.
// Part of the simulation: the gates of Research.hpp (IsBuildingUnlocked, IsUnitUnlocked, IsResearchAvailable) ask it.
namespace Capabilities
{

// The level names in file order (empty when the file is missing: nothing is restricted)
const std::vector<std::string> &Levels();
// Index into Levels(), -1 when there is no such level
int LevelIndex(const std::string &name);
// The file's `default` level (the last one when it names none; empty without levels)
const std::string &DefaultLevel();

// The level a player plays at: the slot's own when it has one (>= 0), else the match's (>= 0), else the default
std::string ResolveLevel(int matchLevel, int slotLevel);

// Sets a player's level, the names resolved through the character configs; an unknown level restricts nothing
void SetPlayerLevel(PlayerId player, const std::string &level, const EntityManager &entities);
// Every player unrestricted (before a match, and in tools)
void ResetLevels();
// The level a player plays at (empty: unrestricted)
const std::string &PlayerLevel(PlayerId player);

// By building_index (walls by any of their segment indexes)
bool AllowsBuilding(PlayerId player, int buildingIndex);
// By archetype (siege / baseline forms count as their base unit)
bool AllowsUnit(PlayerId player, int archetype);
// By the index in the player's faction research tree
bool AllowsResearch(PlayerId player, int index);

// Reads the file text over "no levels"; false when it cannot be parsed (then nothing is restricted)
bool LoadText(const std::string &yamlText);
// From data/capabilities.yaml again (the host's, while a network session has its files)
void Reload();

} // namespace Capabilities

} // namespace OpenArtOfWar
