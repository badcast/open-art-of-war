#include "game/Research.hpp"
#include "game/EntityManager.hpp"
#include "game/Sides.hpp"
#include "core/Utility.hpp"
#include "core/AssetStore.hpp"

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

        bool valid(PlayerId p)
        {
            return p < kMaxPlayers;
        }

        constexpr const char *kEffectNames[] = {"infantry_armor", "infantry_regen", "fire_rate",   "damage",         "infantry_speed", "vehicle_speed", "vision_range", "range_percent", "vehicle_armor", "repair_speed", "reload",       "unlock_unit", "fortress_flamethrower",
                                                "siege_mode",     "rocket_turret",  "power_bonus", "building_armor", "wall_strength",  "unlock_radar",  "train_time",   "vehicle_time",  "income",        "kill_reward",  "build_radius", "mine"};
        static_assert(sizeof(kEffectNames) / sizeof(kEffectNames[0]) == static_cast<size_t>(ResearchEffect::Count), "every effect needs a YAML name");

        // A faction's catalog: the built-in defaults overlaid with its YAML. The strings the defs point to live here.
        struct LoadedCatalog
        {
            std::array<ResearchDef, kResearchCount> defs {};
            std::array<std::string, kResearchCount> names, descs;
        };

        void applyYaml(LoadedCatalog &c, const std::string &text)
        {
            const YAML::Node list = YAML::Load(text)["research"];
            if(!list || !list.IsSequence())
                return;
            auto num = [](const YAML::Node &n, const char *key, int fallback)
            {
                try
                {
                    return n[key] ? n[key].as<int>() : fallback;
                }
                catch(...)
                {
                    return fallback;
                }
            };
            for(size_t i = 0; i < list.size() && i < static_cast<size_t>(kResearchCount); ++i)
            {
                const YAML::Node n = list[i];
                ResearchDef &d = c.defs[i];
                try
                {
                    if(n["name"])
                        c.names[i] = n["name"].as<std::string>();
                    if(n["description"])
                        c.descs[i] = n["description"].as<std::string>();
                }
                catch(...)
                {
                }
                d.name = c.names[i].c_str();
                d.desc = c.descs[i].c_str();
                d.compId = num(n, "icon", d.compId);
                d.cost = num(n, "cost", d.cost);
                d.value = num(n, "value", d.value);
                d.value2 = num(n, "value2", d.value2);
                d.archA = num(n, "arch_a", d.archA);
                d.archB = num(n, "arch_b", d.archB);
                const int needs = num(n, "needs", d.needs);
                d.needs = (needs >= 0 && needs < kResearchCount && needs != static_cast<int>(i)) ? needs : -1;
                if(n["player_only"])
                {
                    try
                    {
                        d.playerOnly = n["player_only"].as<bool>();
                    }
                    catch(...)
                    {
                    }
                }
                ResearchEffect e;
                if(n["effect"] && parseResearchEffect(n["effect"].as<std::string>(""), e))
                    d.effect = e;
            }
        }

        const LoadedCatalog &loadedCatalog(Faction faction)
        {
            static LoadedCatalog catalogs[2];
            static bool ready = false;
            const bool conf = faction == Faction::Confederation;
            if(!ready)
            {
                ready = true;
                for(int f = 0; f < 2; ++f)
                {
                    const Faction side = f == 0 ? Faction::Confederation : Faction::Resistance;
                    const ResearchDef *builtIn = side == Faction::Confederation ? kConfederationResearch : kResistanceResearch;
                    LoadedCatalog &c = catalogs[f];
                    for(int i = 0; i < kResearchCount; ++i)
                    {
                        c.defs[i] = builtIn[i];
                        c.names[i] = builtIn[i].name;
                        c.descs[i] = builtIn[i].desc;
                        c.defs[i].name = c.names[i].c_str();
                        c.defs[i].desc = c.descs[i].c_str();
                    }
                    std::string text;
                    if(!Assets::readText(std::string("assets/") + researchFile(side), text))
                        continue;
                    try
                    {
                        applyYaml(c, text);
                    }
                    catch(const std::exception &e)
                    {
                        std::cerr << "[Research] " << researchFile(side) << ": " << e.what() << std::endl;
                    }
                }
            }
            return catalogs[conf ? 0 : 1];
        }

        const ResearchDef *catalogOf(Faction faction)
        {
            return loadedCatalog(faction).defs.data();
        }

        // Siege and baseline forms count as the unit they belong to
        int baseArchetype(int archetypeId)
        {
            const Utility::ArchetypeInfo *info = Utility::archetypeInfo(archetypeId);
            return info && info->baseFormId >= 0 ? info->baseFormId : archetypeId;
        }

        bool aboutUnit(const ResearchDef &def, const Unit &u)
        {
            const int base = baseArchetype(u.archetypeId);
            return (def.archA >= 0 && base == def.archA) || (def.archB >= 0 && base == def.archB);
        }

        // Speed points: one point is +20% of the speed
        constexpr float kSpeedPoint = 0.2f;
        // Armor points: one point is -8% of the damage taken
        constexpr int kArmorPointPercent = 8;

        void applyEffectToUnit(const ResearchDef &def, Unit &u)
        {
            const bool infantry = u.isInfantry();
            auto scaleDamage = [&](int percent) { u.attackDamage = static_cast<int>(u.attackDamage * (100 + percent) / 100.0f); };
            switch(def.effect)
            {
                case ResearchEffect::InfantrySpeed:
                    if(infantry)
                        u.moveSpeed *= 1.0f + kSpeedPoint * def.value;
                    break;
                case ResearchEffect::VehicleSpeed:
                    if(!infantry)
                        u.moveSpeed *= 1.0f + kSpeedPoint * def.value;
                    break;
                case ResearchEffect::FireRate:
                    if(aboutUnit(def, u))
                    {
                        const float f = 100.0f / (100 + def.value);
                        u.attackCooldown *= f;
                        u.attackCooldownMax *= f;
                    }
                    break;
                case ResearchEffect::Reload:
                    if(aboutUnit(def, u))
                    {
                        const float f = (100 - def.value) / 100.0f;
                        u.attackCooldown *= f;
                        u.attackCooldownMax *= f;
                    }
                    break;
                case ResearchEffect::Damage:
                    if(aboutUnit(def, u))
                        scaleDamage(def.value);
                    break;
                case ResearchEffect::VisionRange:
                    if(aboutUnit(def, u))
                    {
                        u.attackRange += static_cast<float>(def.value);
                        u.sightRadius += def.value;
                        scaleDamage(def.value2);
                    }
                    break;
                case ResearchEffect::RangePercent:
                    if(aboutUnit(def, u))
                    {
                        u.attackRange *= 1.0f + def.value / 100.0f;
                        scaleDamage(def.value2);
                    }
                    break;
                case ResearchEffect::RocketTurret:
                    // The extra rocket launcher adds firepower to the Zeus
                    if(aboutUnit(def, u))
                        scaleDamage(15);
                    break;
                default:
                    break;
            }
        }

    } // namespace

    const char *researchEffectName(ResearchEffect effect)
    {
        const int i = static_cast<int>(effect);
        return i >= 0 && i < static_cast<int>(ResearchEffect::Count) ? kEffectNames[i] : "";
    }

    bool parseResearchEffect(const std::string &name, ResearchEffect &out)
    {
        for(int i = 0; i < static_cast<int>(ResearchEffect::Count); ++i)
        {
            if(name == kEffectNames[i])
            {
                out = static_cast<ResearchEffect>(i);
                return true;
            }
        }
        return false;
    }

    const char *researchFile(Faction faction)
    {
        return faction == Faction::Confederation ? "characters/research_confederation.yaml" : "characters/research_resistance.yaml";
    }

    const ResearchDef &researchDef(Faction faction, int index)
    {
        return catalogOf(faction)[std::clamp(index, 0, kResearchCount - 1)];
    }

    bool hasResearchBuilding(PlayerId player, const EntityManager &entities)
    {
        for(const auto &e : entities.getAllEntities())
        {
            const Building *b = e.asBuilding();
            if(b && b->alive && b->state != ActionState::Die && b->owner == player && b->type == UnitType::ResearchLab && b->buildProgress >= 1.0f)
            {
                return true;
            }
        }
        return false;
    }

    void resetResearch()
    {
        for(auto &side : g_completed)
            std::fill(std::begin(side), std::end(side), false);
    }

    void resetResearch(PlayerId player)
    {
        if(!valid(player))
            return;
        std::fill(std::begin(g_completed[player]), std::end(g_completed[player]), false);
    }

    bool isResearched(PlayerId player, int index)
    {
        return valid(player) && index >= 0 && index < kResearchCount && g_completed[player][index];
    }

    bool isResearchAvailable(PlayerId player, int index)
    {
        const int req = researchDef(Players::factionOf(player), index).needs;
        return req < 0 || isResearched(player, req);
    }

    void completeResearch(PlayerId player, int index, EntityManager &entities)
    {
        if(!valid(player) || index < 0 || index >= kResearchCount || isResearched(player, index))
            return;
        g_completed[player][index] = true;
        const ResearchDef &def = researchDef(Players::factionOf(player), index);
        for(auto &e : entities.getAllEntities())
        {
            Unit *u = e.asUnit();
            if(!u || !u->alive || u->owner != player)
                continue;
            applyEffectToUnit(def, *u);
        }
        if(def.effect == ResearchEffect::FortressFlamethrower)
        {
            entities.setFortressFlamethrowerUnlocked(player, true);
        }
        else if(def.effect == ResearchEffect::SiegeMode)
        {
            entities.setSiegeUnlocked(player, true);
        }
        else if(def.effect == ResearchEffect::BuildRadius)
        {
            entities.setBuildRadiusPercent(player, researchTotal(player, ResearchEffect::BuildRadius));
        }
    }

    void applyCompletedResearch(Unit &unit)
    {
        for(int i = 0; i < kResearchCount; ++i)
        {
            if(isResearched(unit.owner, i))
                applyEffectToUnit(researchDef(Players::factionOf(unit.owner), i), unit);
        }
    }

    float researchedAttackRange(PlayerId player, int archetypeId, float range)
    {
        const int base = baseArchetype(archetypeId);
        const Faction faction = Players::factionOf(player);
        for(int i = 0; i < kResearchCount; ++i)
        {
            const ResearchDef &def = researchDef(faction, i);
            if(!isResearched(player, i) || (def.archA != base && def.archB != base) || base < 0)
                continue;
            if(def.effect == ResearchEffect::VisionRange)
                range += static_cast<float>(def.value);
            else if(def.effect == ResearchEffect::RangePercent)
                range *= 1.0f + def.value / 100.0f;
        }
        return range;
    }

    int researchTotal(PlayerId player, ResearchEffect effect)
    {
        int total = 0;
        const Faction faction = Players::factionOf(player);
        for(int i = 0; i < kResearchCount; ++i)
        {
            const ResearchDef &def = researchDef(faction, i);
            if(def.effect == effect && isResearched(player, i))
                total += def.value;
        }
        return total;
    }

    bool isEffectResearched(PlayerId player, ResearchEffect effect)
    {
        const Faction faction = Players::factionOf(player);
        for(int i = 0; i < kResearchCount; ++i)
        {
            if(researchDef(faction, i).effect == effect && isResearched(player, i))
                return true;
        }
        return false;
    }

    int unitUnlockResearch(Faction faction, int archetype)
    {
        const int base = baseArchetype(archetype);
        for(int i = 0; i < kResearchCount; ++i)
        {
            const ResearchDef &def = researchDef(faction, i);
            if(def.effect == ResearchEffect::UnlockUnit && def.archA == base)
                return i;
        }
        return -1;
    }

    bool isUnitUnlocked(PlayerId player, int archetype)
    {
        const int index = unitUnlockResearch(Players::factionOf(player), archetype);
        return index < 0 || isResearched(player, index);
    }

    bool isRadarUnlocked(PlayerId player)
    {
        const Faction faction = Players::factionOf(player);
        for(int i = 0; i < kResearchCount; ++i)
        {
            if(researchDef(faction, i).effect == ResearchEffect::UnlockRadar)
                return isResearched(player, i);
        }
        return true;
    }

    int reduceDamageByArmor(int damage, const Entity &target)
    {
        if(damage <= 0)
            return damage;
        int percent = 0;
        if(target.isBuilding)
        {
            percent = researchTotal(target.owner, ResearchEffect::BuildingArmor);
        }
        else if(const Unit *u = target.asUnit())
        {
            const int points = researchTotal(u->owner, u->isInfantry() ? ResearchEffect::InfantryArmor : ResearchEffect::VehicleArmor);
            percent = points * kArmorPointPercent;
        }
        if(percent <= 0)
            return damage;
        return std::max(1, damage * 100 / (100 + percent));
    }

    int reduceWallDamage(int damage, PlayerId player)
    {
        const int percent = researchTotal(player, ResearchEffect::WallStrength);
        return percent <= 0 || damage <= 0 ? damage : std::max(1, damage * 100 / (100 + percent));
    }

} // namespace OpenArtOfWar
