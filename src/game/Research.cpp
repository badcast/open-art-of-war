#include "game/Research.hpp"
#include "game/Capabilities.hpp"
#include "graphics/SpriteRegistry.hpp"
#include "game/EntityManager.hpp"
#include "game/Sides.hpp"
#include "core/Utility.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"

#include <yaml-cpp/yaml.h>
#include <array>
#include <iostream>
#include <string>
#include <algorithm>

namespace OpenArtOfWar
{

namespace
{

#include "ResearchCatalog.inc"

bool g_completed[kMaxPlayers][kResearchCount] = {};

bool Valid(PlayerId p)
{
    return p < kMaxPlayers;
}

constexpr const char *kEffectNames[]
    = {"infantry_armor",
       "infantry_regen",
       "fire_rate",
       "damage",
       "infantry_speed",
       "vehicle_speed",
       "vision_range",
       "range_percent",
       "vehicle_armor",
       "repair_speed",
       "reload",
       "unlock_unit",
       "fortress_flamethrower",
       "siege_mode",
       "rocket_turret",
       "power_bonus",
       "building_armor",
       "wall_strength",
       "unlock_radar",
       "train_time",
       "vehicle_time",
       "income",
       "kill_reward",
       "build_radius",
       "mine"};
static_assert(
    sizeof(kEffectNames) / sizeof(kEffectNames[0]) == static_cast<std::size_t>(ResearchEffect::Count),
    "every effect needs a YAML name");

// A faction's catalog: the built-in defaults overlaid with its YAML. The strings the defs point to live here.
struct LoadedCatalog
{
    std::array<ResearchDef, kResearchCount> defs {};
    std::array<std::string, kResearchCount> names, descs;
};

void ApplyYaml(LoadedCatalog &c, const std::string &text)
{
    const YAML::Node list = YAML::Load(text)["research"];
    if (!list || !list.IsSequence())
        return;
    auto num = [](const YAML::Node &n, const char *key, int fallback) {
        try
        {
            return n[key] ? n[key].as<int>() : fallback;
        }
        catch (...)
        {
            return fallback;
        }
    };
    for (std::size_t i = 0; i < list.size() && i < static_cast<std::size_t>(kResearchCount); ++i)
    {
        const YAML::Node n = list[i];
        ResearchDef &d = c.defs[i];
        try
        {
            if (n["name"])
                c.names[i] = n["name"].as<std::string>();
            if (n["description"])
                c.descs[i] = n["description"].as<std::string>();
        }
        catch (...)
        {}
        d.name = c.names[i].c_str();
        d.desc = c.descs[i].c_str();
        if (n["icon"])
            d.compId = SpriteHandle(n["icon"], SpriteKind::Part, "research icon");
        try
        {
            if (n["research_seconds"])
                d.seconds = std::clamp(n["research_seconds"].as<float>(), 1.0f, 3600.0f);
        }
        catch (...)
        {}
        d.cost = num(n, "cost", d.cost);
        d.value = num(n, "value", d.value);
        d.value2 = num(n, "value2", d.value2);
        d.archA = num(n, "arch_a", d.archA);
        d.archB = num(n, "arch_b", d.archB);
        const int needs = num(n, "needs", d.needs);
        d.needs = (needs >= 0 && needs < kResearchCount && needs != static_cast<int>(i)) ? needs : -1;
        if (n["player_only"])
        {
            try
            {
                d.playerOnly = n["player_only"].as<bool>();
            }
            catch (...)
            {}
        }
        ResearchEffect e;
        if (n["effect"] && ParseResearchEffect(n["effect"].as<std::string>(""), e))
            d.effect = e;
    }
}

bool g_catalogsReady = false; // false: the research files are read (again) on the next use

const LoadedCatalog &loadedCatalog(Faction faction)
{
    static LoadedCatalog catalogs[2];
    const bool conf = faction == Faction::Confederation;
    if (!g_catalogsReady)
    {
        g_catalogsReady = true;
        for (int f = 0; f < 2; ++f)
        {
            const Faction side = f == 0 ? Faction::Confederation : Faction::Resistance;
            const ResearchDef *builtIn = side == Faction::Confederation ? kConfederationResearch : kResistanceResearch;
            LoadedCatalog &c = catalogs[f];
            for (int i = 0; i < kResearchCount; ++i)
            {
                c.defs[i] = builtIn[i];
                c.names[i] = builtIn[i].name;
                c.descs[i] = builtIn[i].desc;
                c.defs[i].name = c.names[i].c_str();
                c.defs[i].desc = c.descs[i].c_str();
                c.defs[i].compId = -1; // the icon is a sprite named in the faction's research file
            }
            std::string text;
            const std::string file
                = LoaderResources::research(side == Faction::Confederation ? "confederation" : "resistance");
            if (!Assets::ReadText(file, text))
                continue;
            try
            {
                ApplyYaml(c, text);
            }
            catch (const std::exception &e)
            {
                std::cerr << "[Research] " << file << ": " << e.what() << std::endl;
            }
        }
    }
    return catalogs[conf ? 0 : 1];
}

const ResearchDef *CatalogOf(Faction faction)
{
    return loadedCatalog(faction).defs.data();
}

// Siege and baseline forms count as the unit they belong to
int BaseArchetype(int archetypeId)
{
    const Utility::ArchetypeInfo *info = Utility::archetypeInfo(archetypeId);
    return info && info->baseFormId >= 0 ? info->baseFormId : archetypeId;
}

bool AboutUnit(const ResearchDef &def, const Unit &u)
{
    const int base = BaseArchetype(u.archetypeId);
    return (def.archA >= 0 && base == def.archA) || (def.archB >= 0 && base == def.archB);
}

// Speed points: one point is +20% of the speed
constexpr float kSpeedPoint = 0.2f;
// Armor points: one point is -8% of the damage taken
constexpr int kArmorPointPercent = 8;

void ApplyEffectToUnit(const ResearchDef &def, Unit &u)
{
    const bool infantry = u.IsInfantry();
    auto scaleDamage = [&](int percent) {
        u.attackDamage = static_cast<int>(u.attackDamage * (100 + percent) / 100.0f);
        u.secondaryDamage = static_cast<int>(u.secondaryDamage * (100 + percent) / 100.0f); // the second gun too
    };
    switch (def.effect)
    {
        case ResearchEffect::InfantrySpeed:
            if (infantry)
                u.moveSpeed *= 1.0f + kSpeedPoint * def.value;
            break;
        case ResearchEffect::VehicleSpeed:
            if (!infantry)
                u.moveSpeed *= 1.0f + kSpeedPoint * def.value;
            break;
        case ResearchEffect::FireRate:
            if (AboutUnit(def, u))
            {
                const float f = 100.0f / (100 + def.value);
                u.attackCooldown *= f;
                u.attackCooldownMax *= f;
                u.secondaryCooldown *= f;
            }
            break;
        case ResearchEffect::Reload:
            if (AboutUnit(def, u))
            {
                const float f = (100 - def.value) / 100.0f;
                u.attackCooldown *= f;
                u.attackCooldownMax *= f;
                u.secondaryCooldown *= f;
            }
            break;
        case ResearchEffect::Damage:
            if (AboutUnit(def, u))
                scaleDamage(def.value);
            break;
        case ResearchEffect::VisionRange:
            if (AboutUnit(def, u))
            {
                u.attackRange += static_cast<float>(def.value);
                u.sightRadius += def.value;
                scaleDamage(def.value2);
            }
            break;
        case ResearchEffect::RangePercent:
            if (AboutUnit(def, u))
            {
                u.attackRange *= 1.0f + def.value / 100.0f;
                scaleDamage(def.value2);
            }
            break;
        case ResearchEffect::RocketTurret:
            // Nothing to scale: the rocket launcher is a gun of its own (weapons[1] of the Zeus' second form)
            break;
        default:
            break;
    }
}

} // namespace

const char *ResearchEffectName(ResearchEffect effect)
{
    const int i = static_cast<int>(effect);
    return i >= 0 && i < static_cast<int>(ResearchEffect::Count) ? kEffectNames[i] : "";
}

bool ParseResearchEffect(const std::string &name, ResearchEffect &out)
{
    for (int i = 0; i < static_cast<int>(ResearchEffect::Count); ++i)
    {
        if (name == kEffectNames[i])
        {
            out = static_cast<ResearchEffect>(i);
            return true;
        }
    }
    return false;
}

void ReloadResearchCatalogs()
{
    g_catalogsReady = false;
}

const ResearchDef &researchDef(Faction faction, int index)
{
    return CatalogOf(faction)[std::clamp(index, 0, kResearchCount - 1)];
}

bool HasResearchBuilding(PlayerId player, const EntityManager &entities)
{
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->state != ActionState::Die && b->owner == player && b->type == UnitType::ResearchLab
            && b->buildProgress >= 1.0f)
        {
            return true;
        }
    }
    return false;
}

bool ResearchBuildingPowered(PlayerId player, const EntityManager &entities)
{
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->state != ActionState::Die && b->owner == player && b->type == UnitType::ResearchLab
            && b->buildProgress >= 1.0f && b->isPowered)
        {
            return true;
        }
    }
    return false;
}

void SetResearchAnimation(PlayerId player, bool researching, EntityManager &entities)
{
    for (auto &e : entities.GetAllEntities())
    {
        Building *b = e.AsBuilding();
        if (b && b->owner == player && b->type == UnitType::ResearchLab)
            b->isResearching = researching && b->alive && b->buildProgress >= 1.0f;
    }
}

void ResetResearch()
{
    for (auto &side : g_completed)
        std::fill(std::begin(side), std::end(side), false);
}

void ResetResearch(PlayerId player)
{
    if (!Valid(player))
        return;
    std::fill(std::begin(g_completed[player]), std::end(g_completed[player]), false);
}

bool IsResearched(PlayerId player, int index)
{
    return Valid(player) && index >= 0 && index < kResearchCount && g_completed[player][index];
}

bool IsResearchAvailable(PlayerId player, int index)
{
    // The player's tech level (data/capabilities.yaml) may leave it out
    if (!Capabilities::AllowsResearch(player, index))
        return false;
    const int req = researchDef(Players::FactionOf(player), index).needs;
    return req < 0 || IsResearched(player, req);
}

void CompleteResearch(PlayerId player, int index, EntityManager &entities)
{
    if (!Valid(player) || index < 0 || index >= kResearchCount || IsResearched(player, index))
        return;
    g_completed[player][index] = true;
    const ResearchDef &def = researchDef(Players::FactionOf(player), index);
    for (auto &e : entities.GetAllEntities())
    {
        Unit *u = e.AsUnit();
        if (!u || !u->alive || u->owner != player)
            continue;
        ApplyEffectToUnit(def, *u);
    }
    if (def.effect == ResearchEffect::RocketTurret)
    {
        // The Zeus tanks that exist get the rocket turret now
        std::vector<int> baseZeus;
        for (const auto &e : entities.GetAllEntities())
        {
            const Unit *u = e.AsUnit();
            if (u && u->alive && u->owner == player && u->archetypeId == 17)
                baseZeus.push_back(u->id);
        }
        for (int id : baseZeus)
            entities.UpgradeUnitForm(id, 11);
    }
    if (def.effect == ResearchEffect::FortressFlamethrower)
    {
        entities.SetFortressFlamethrowerUnlocked(player, true);
    }
    else if (def.effect == ResearchEffect::SiegeMode)
    {
        entities.SetSiegeUnlocked(player, true);
    }
    else if (def.effect == ResearchEffect::BuildRadius)
    {
        entities.SetBuildRadiusPercent(player, ResearchTotal(player, ResearchEffect::BuildRadius));
    }
}

void ApplyCompletedResearch(Unit &unit)
{
    for (int i = 0; i < kResearchCount; ++i)
    {
        if (IsResearched(unit.owner, i))
            ApplyEffectToUnit(researchDef(Players::FactionOf(unit.owner), i), unit);
    }
}

float ResearchedAttackRange(PlayerId player, int archetypeId, float range)
{
    const int base = BaseArchetype(archetypeId);
    const Faction faction = Players::FactionOf(player);
    for (int i = 0; i < kResearchCount; ++i)
    {
        const ResearchDef &def = researchDef(faction, i);
        if (!IsResearched(player, i) || (def.archA != base && def.archB != base) || base < 0)
            continue;
        if (def.effect == ResearchEffect::VisionRange)
            range += static_cast<float>(def.value);
        else if (def.effect == ResearchEffect::RangePercent)
            range *= 1.0f + def.value / 100.0f;
    }
    return range;
}

int ResearchTotal(PlayerId player, ResearchEffect effect)
{
    int total = 0;
    const Faction faction = Players::FactionOf(player);
    for (int i = 0; i < kResearchCount; ++i)
    {
        const ResearchDef &def = researchDef(faction, i);
        if (def.effect == effect && IsResearched(player, i))
            total += def.value;
    }
    return total;
}

bool IsEffectResearched(PlayerId player, ResearchEffect effect)
{
    const Faction faction = Players::FactionOf(player);
    for (int i = 0; i < kResearchCount; ++i)
    {
        if (researchDef(faction, i).effect == effect && IsResearched(player, i))
            return true;
    }
    return false;
}

int UnitUnlockResearch(Faction faction, int archetype)
{
    const int base = BaseArchetype(archetype);
    for (int i = 0; i < kResearchCount; ++i)
    {
        const ResearchDef &def = researchDef(faction, i);
        if (def.effect == ResearchEffect::UnlockUnit && def.archA == base)
            return i;
    }
    return -1;
}

bool IsUnitUnlocked(PlayerId player, int archetype)
{
    if (!Capabilities::AllowsUnit(player, archetype))
        return false;
    const int index = UnitUnlockResearch(Players::FactionOf(player), archetype);
    return index < 0 || IsResearched(player, index);
}

int ProductionForm(PlayerId player, int archetype)
{
    constexpr int kZeus = 11, kZeusBase = 17;
    if (archetype == kZeus && Players::FactionOf(player) == Faction::Confederation
        && !IsEffectResearched(player, ResearchEffect::RocketTurret))
        return kZeusBase;
    return archetype;
}

bool IsBuildingUnlocked(PlayerId player, int buildingIndex)
{
    if (!Capabilities::AllowsBuilding(player, buildingIndex))
        return false;
    return buildingIndex != 6 || IsRadarUnlocked(player);
}

bool IsRadarUnlocked(PlayerId player)
{
    const Faction faction = Players::FactionOf(player);
    for (int i = 0; i < kResearchCount; ++i)
    {
        if (researchDef(faction, i).effect == ResearchEffect::UnlockRadar)
            return IsResearched(player, i);
    }
    return true;
}

int ReduceDamageByArmor(int damage, const Entity &target)
{
    if (damage <= 0)
        return damage;
bool IsMineResearched(PlayerId player, int mineValue)
{
    const Faction faction = Players::FactionOf(player);
    for (int i = 0; i < kResearchCount; ++i)
    {
        const ResearchDef &def = researchDef(faction, i);
        if (def.effect == ResearchEffect::Mine && def.value == mineValue && IsResearched(player, i))
            return true;
    }
    return false;
}

    int percent = 0;
    if (target.isBuilding)
    {
        percent = ResearchTotal(target.owner, ResearchEffect::BuildingArmor);
    }
    else if (const Unit *u = target.AsUnit())
    {
        const int points
            = ResearchTotal(u->owner, u->IsInfantry() ? ResearchEffect::InfantryArmor : ResearchEffect::VehicleArmor);
        percent = points * kArmorPointPercent;
    }
    if (percent <= 0)
        return damage;
    return std::max(1, damage * 100 / (100 + percent));
}

int ReduceWallDamage(int damage, PlayerId player)
{
    const int percent = ResearchTotal(player, ResearchEffect::WallStrength);
    return percent <= 0 || damage <= 0 ? damage : std::max(1, damage * 100 / (100 + percent));
}

} // namespace OpenArtOfWar
