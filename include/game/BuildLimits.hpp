#pragma once

#include "game/Players.hpp"

#include <string>

namespace OpenArtOfWar
{

class EntityManager;
class TileMap;

// Hard per-player building limits (data/limits.yaml): how many buildings of a kind one player may have, finished or
// under construction. -1: unlimited, 0: may not be built. Kinds are shared by both sides (the Resistance Headquarters
// and the Confederation Command Center are both "headquarters"); an exact building id overrides its kind.
// Part of the simulation: Simulation (PlaceBuilding), the computer players and the interface all ask CanBuildMore.
namespace BuildLimits
{

constexpr int kUnlimited = -1;
constexpr int kDefaultMaxArmy = 10000; // max_army when limits.yaml does not say

enum class Kind : int
{
    Headquarters,
    Power,
    Barracks,
    Factory,
    Research,
    Radar,
    Turret,
    Bunker,
    Wall,
    Other,
    Count
};

// By building_index (the blueprint's / Building::rawBuildingType)
Kind KindOf(int buildingIndex);
const char *KeyOf(Kind kind);

// The limit for that building (its id from the character configs overrides its kind)
int LimitOf(const EntityManager &entities, int buildingIndex);
// What the player has of it now: buildings of the same kind (or id, when the id has its own limit), alive and not
// dying, under construction included; for walls the segments it owns
int CountOwned(const EntityManager &entities, const TileMap &map, PlayerId player, int buildingIndex);
// Another one may be started
bool CanBuildMore(const EntityManager &entities, const TileMap &map, PlayerId player, int buildingIndex);

// Hard army size limit (max_army): most units one player may have alive at once; kUnlimited when switched off
int MaxArmy();
// The limit that counts for a player: the lobby's choice (units, 0 = unlimited) held under max_army.
// 0 means no limit at all.
int EffectiveArmyCap(int lobbyUnits);

// Reads the file text over "no limits"; false when it cannot be parsed (then nothing is limited)
bool LoadText(const std::string &yamlText);
// From data/limits.yaml again (the host's, while a network session has its files)
void Reload();

} // namespace BuildLimits

} // namespace OpenArtOfWar
