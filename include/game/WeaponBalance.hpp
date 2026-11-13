#pragma once

#include "game/Entity.hpp"

#include <string_view>

namespace OpenArtOfWar
{

class Entity;

// Damage multipliers by projectile kind and class of the target: assets/data/weapon_balance.json. Every value has the
// built-in default, so a missing file changes nothing but the defaults. Part of the network assets check.
enum class TargetClass : std::uint8_t
{
    Infantry,
    Vehicle,
    Building,
    Wall
};

namespace WeaponBalance
{
constexpr int kKinds = 5;   // ProjectileType values
constexpr int kClasses = 4; // TargetClass values

// Back to the defaults overlaid with the file
void Reload();
// Defaults overlaid with this JSON text
bool LoadText(std::string_view jsonText);
// The multiplier in percent (100 = unchanged)
int Percent(ProjectileType type, TargetClass target);
// `damage` scaled for the target class; never below 1 for a positive damage
int Scale(int damage, ProjectileType type, TargetClass target);
// Share of the damage a moving unit takes, in percent: a random value in [min, max] (the file's `moving_target`)
void MovingTargetPercent(int &minPercent, int &maxPercent);
// Class of an entity (building, infantry or vehicle)
TargetClass ClassOf(const Entity &target);
} // namespace WeaponBalance

} // namespace OpenArtOfWar
