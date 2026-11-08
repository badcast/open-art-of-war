// Helpers shared by the EntityManager*.cpp files (not part of the public interface)
#pragma once

#include "game/Entity.hpp"
#include <vector>

namespace OpenArtOfWar
{

// The infantry deaths every infantry character falls back to
std::vector<int> DefaultDeaths(bool flame);

// Starts the death animation of a unit that has just died
void HandleUnitDeathState(Unit *tu, bool isFlameDamage);

// Distance in pixels from the attacker to the nearest point of the target (a building is a rectangle)
float ComputeDistanceToEntityPixels(const Unit &attacker, const Entity &target);

} // namespace OpenArtOfWar
