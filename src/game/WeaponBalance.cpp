#include "game/WeaponBalance.hpp"
#include "core/AssetStore.hpp"
#include "core/JsonFile.hpp"
#include "core/LoaderResources.hpp"
#include "game/Projectiles.hpp"
#include "game/Unit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

namespace OpenArtOfWar
{

namespace
{

using Table = std::array<std::array<int, WeaponBalance::kClasses>, WeaponBalance::kKinds>;

// Percent; index [ProjectileType][TargetClass]: infantry, vehicle, building, wall
Table Defaults()
{
    Table t;
    for (auto &row : t)
        row.fill(100);
    auto set = [&t](ProjectileType type, int inf, int veh, int bld, int wall) {
        t[static_cast<std::size_t>(type)] = {inf, veh, bld, wall};
    };
    set(ProjectileType::Bullet, 125, 100, 80, 80);
    set(ProjectileType::Sniper, 150, 50, 40, 40);
    set(ProjectileType::Shell, 80, 100, 125, 125);
    set(ProjectileType::Rocket, 75, 125, 100, 100);
    set(ProjectileType::Flame, 150, 60, 100, 80);
    return t;
}

Table &Storage()
{
    static Table t = Defaults();
    return t;
}

// Share of the damage a moving unit takes (percent, [min, max])
std::array<int, 2> &MovingShare()
{
    static std::array<int, 2> share {50, 100};
    return share;
}

const char *const kClassKeys[WeaponBalance::kClasses] = {"infantry", "vehicle", "building", "wall"};

} // namespace

namespace WeaponBalance
{

bool LoadText(std::string_view jsonText)
{
    const nlohmann::json doc = JsonFile::ReadFromString(jsonText);
    Table t = Defaults();
    MovingShare() = {50, 100};
    if (doc.contains("moving_target") && doc["moving_target"].is_object())
    {
        const nlohmann::json &m = doc["moving_target"];
        int lo = 50, hi = 100;
        if (m.contains("min") && m["min"].is_number())
            lo = static_cast<int>(std::lround(std::clamp(m["min"].get<double>(), 0.0, 1.0) * 100.0));
        if (m.contains("max") && m["max"].is_number())
            hi = static_cast<int>(std::lround(std::clamp(m["max"].get<double>(), 0.0, 1.0) * 100.0));
        MovingShare() = {std::min(lo, hi), std::max(lo, hi)};
    }
    if (doc.contains("multipliers") && doc["multipliers"].is_object())
    {
        for (int kind = 0; kind < kKinds; ++kind)
        {
            const char *key = Projectiles::KeyOf(static_cast<ProjectileType>(kind));
            if (!doc["multipliers"].contains(key) || !doc["multipliers"][key].is_object())
                continue;
            const nlohmann::json &row = doc["multipliers"][key];
            for (int c = 0; c < kClasses; ++c)
            {
                if (!row.contains(kClassKeys[c]) || !row[kClassKeys[c]].is_number())
                    continue;
                const double m = std::clamp(row[kClassKeys[c]].get<double>(), 0.0, 10.0);
                t[static_cast<std::size_t>(kind)][static_cast<std::size_t>(c)]
                    = static_cast<int>(std::lround(m * 100.0));
            }
        }
    }
    Storage() = t;
    return true;
}

void Reload()
{
    Storage() = Defaults();
    MovingShare() = {50, 100};
    std::string text;
    if (Assets::ReadText(LoaderResources::weaponBalance(), text))
        LoadText(text);
}

int Percent(ProjectileType type, TargetClass target)
{
    static const bool loaded = (Reload(), true); // the file is read on first use
    (void) loaded;
    return Storage()[static_cast<std::size_t>(type)][static_cast<std::size_t>(target)];
}

int Scale(int damage, ProjectileType type, TargetClass target)
{
    if (damage <= 0)
        return damage;
    const int percent = Percent(type, target);
    if (percent == 100)
        return damage;
    return std::max(1, (damage * percent + 50) / 100);
}

void MovingTargetPercent(int &minPercent, int &maxPercent)
{
    Percent(ProjectileType::Bullet, TargetClass::Infantry); // makes sure the file has been read
    minPercent = MovingShare()[0];
    maxPercent = MovingShare()[1];
}

TargetClass ClassOf(const Entity &target)
{
    if (target.isBuilding)
        return TargetClass::Building;
    const Unit *u = target.AsUnit();
    return u && u->IsInfantry() ? TargetClass::Infantry : TargetClass::Vehicle;
}

} // namespace WeaponBalance

} // namespace OpenArtOfWar
