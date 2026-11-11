// Constants and helpers shared by the AIController*.cpp files (not part of the public interface)
#pragma once

#include "game/AIController.hpp"
#include "game/EntityManager.hpp"
#include <yaml-cpp/yaml.h>

namespace OpenArtOfWar
{

// Confederation archetypes used by the wave planner
inline constexpr int kRifleman = 1;
inline constexpr int kHeavyAssault = 3;   // rocket trooper (ranged support)
inline constexpr int kFlamethrower = 5;   // close combat
inline constexpr int kMinAttackWave = 10; // smallest wave the AI marches with
inline constexpr int kMinAttackGroup = 5; // ... and the smallest group it splits a wave into
inline constexpr int kFortress = 7;
inline constexpr int kHammer = 9;
inline constexpr int kZeus = 11;
inline constexpr int kGrad = 13; // MLRS (artillery)
// Troops of the base follow an enemy this far (tiles) out of the base zone, no farther
inline constexpr float kBaseChaseTiles = 12.0f;
// A wave waits at most this long for an attack on the base to end, then it leaves anyway
inline constexpr float kWaveHoldMaxSeconds = 60.0f;

int SideArchetype(int confederationArch, Faction side);
bool IsVehicleArchetype(int arch);
int FallbackArchetypeCost(int arch);
const char *TierName(AIWaveTier tier);
void ParseTactics(const YAML::Node &node, AITacticsConfig &t);
int FactionBuildingIndex(AIDefenseSite::Kind kind, Faction side);
int DefenseCost(AIDefenseSite::Kind kind, const EntityManager &entities, Faction side);

} // namespace OpenArtOfWar
