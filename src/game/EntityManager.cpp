#include "game/EntityManager.hpp"
#include "core/DetMath.hpp"
#include "core/AssetStore.hpp"
#include "core/Localization.hpp"
#include "core/Utility.hpp"
#include "game/Effects.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "game/TileMap.hpp"
#include "graphics/TextureManager.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <yaml-cpp/yaml.h>

namespace OpenArtOfWar
{

    std::vector<BuildingBlueprint> EntityManager::s_blueprints;

    std::string EntityManager::descriptionKey(const std::string &characterId)
    {
        return characterId + ".description";
    }

    // A key-style msgid is translated when gettext returns something else than the key itself
    static bool hasTranslation(const std::string &key)
    {
        return Localization::tr(key) != key;
    }

    static const char *projectileTypeName(ProjectileType type)
    {
        switch(type)
        {
            case ProjectileType::Shell:
                return "shell";
            case ProjectileType::Rocket:
                return "rocket";
            case ProjectileType::Flame:
                return "flame";
            default:
                return "bullet";
        }
    }

    static ProjectileType stringToProjectileType(const std::string &str)
    {
        if(str == "rocket")
            return ProjectileType::Rocket;
        if(str == "shell")
            return ProjectileType::Shell;
        if(str == "flame")
            return ProjectileType::Flame;
        return ProjectileType::Bullet;
    }

    int EntityManager::applyMovingUnitDamage(int baseDamage, const Entity *target)
    {
        if(!target || baseDamage <= 0)
            return baseDamage;
        const Unit *u = target->asUnit();
        if(u && (u->state == ActionState::Move || (!u->path.empty() && u->pathIndex < u->path.size())))
        {
            // A moving target takes 50..100% of the damage
            const uint32_t roll = static_cast<uint32_t>(Utility::randomInt(50, 100));
            int finalDamage = static_cast<int>((static_cast<uint64_t>(baseDamage) * roll + 50u) / 100u);
            return reduceDamageByArmor((finalDamage < 1) ? 1 : finalDamage, *target);
        }
        return reduceDamageByArmor(baseDamage, *target);
    }

    // Infantry heals by itself; vehicles are repaired close to their faction's Factory. The technologies of the
    // faction make both 3 times faster.
    void EntityManager::updateRegeneration(float deltaTime)
    {
        constexpr float kInfantryRegen = 0.3f; // HP per second
        constexpr float kRepair = 1.0f;        // HP per second near a Factory
        constexpr float kRepairRadius = 5.0f;  // tiles
        std::vector<const Building *> factories;
        for(const auto &e : m_entities)
        {
            const Building *b = e.asBuilding();
            if(b && b->alive && b->state != ActionState::Die && b->type == UnitType::Factory && b->buildProgress >= 1.0f)
            {
                factories.push_back(b);
            }
        }
        for(auto &e : m_entities)
        {
            Unit *u = e.asUnit();
            if(!u || !u->alive || u->state == ActionState::Die || u->currentHp >= u->maxHp)
                continue;
            float rate = 0.0f;
            if(u->isInfantry())
            {
                rate = kInfantryRegen * (isEffectResearched(u->owner, ResearchEffect::InfantryRegen) ? 3.0f : 1.0f);
                if(u->state != ActionState::Idle)
                    rate *= 0.5f; // walking wounded recover slower
            }
            else
            {
                for(const Building *f : factories)
                {
                    if(f->owner != u->owner)
                        continue;
                    if(DetMath::hypot(static_cast<float>(f->tileX - u->tileX), static_cast<float>(f->tileY - u->tileY)) <= kRepairRadius)
                    {
                        rate = kRepair * (isEffectResearched(u->owner, ResearchEffect::RepairSpeed) ? 3.0f : 1.0f);
                        break;
                    }
                }
            }
            if(rate <= 0.0f)
                continue;
            u->regenAccum += rate * deltaTime;
            if(u->regenAccum >= 1.0f)
            {
                const int heal = static_cast<int>(u->regenAccum);
                u->regenAccum -= static_cast<float>(heal);
                u->currentHp = std::min(u->maxHp, u->currentHp + heal);
            }
        }
    }

    // One hit effect for every bullet that hits something - infantry, vehicles, buildings alike:
    // the impact sparks (55..58, variant picked by the target). No blood.
    static int bulletImpactAnim(int targetId)
    {
        return 55 + (targetId % 4);
    }

    static void handleUnitDeathState(Unit *tu, bool isFlameDamage)
    {
        if(!tu)
            return;
        tu->path.clear();
        tu->deathTimer = EffectsConfig::get().burialSeconds;
        tu->animFrame = 0;
        tu->animTimer = 0.0f;
        if(isFlameDamage || tu->killedByFlame)
        {
            tu->killedByFlame = true;
            if(!tu->flameDeathSequences.empty())
            {
                tu->selectedDeathSeq = Utility::pick(tu->flameDeathSequences);
            }
            else
            {
                tu->selectedDeathSeq = (tu->archetypeId % 2 == 0) ? 259 : 260;
            }
        }
        else if(tu->isInfantry())
        {
            if(!tu->deathSequences.empty())
            {
                tu->selectedDeathSeq = Utility::pick(tu->deathSequences);
            }
            else
            {
                tu->selectedDeathSeq = Utility::randomInt(247, 248);
            }
        }
    }

    EntityManager::EntityManager()
    {
        m_entities.reserve(MAX_UNITS);
        loadCharacterConfigs("assets");
    }

    bool EntityManager::loadCharacterConfigs(const std::string &assetRoot)
    {
        namespace fs = std::filesystem;
        fs::path charDir = fs::path(assetRoot) / "characters";
        std::vector<std::string> files = Assets::list(charDir.generic_string(), ".yaml");
        for(const std::string &f : Assets::list(charDir.generic_string(), ".yml"))
            files.push_back(f);
        if(files.empty())
        {
            charDir = fs::path("../") / assetRoot / "characters";
            files = Assets::list(charDir.generic_string(), ".yaml");
            for(const std::string &f : Assets::list(charDir.generic_string(), ".yml"))
                files.push_back(f);
        }

        if(files.empty())
        {
            std::cerr << "[EntityManager] Characters directory not found: " << charDir << std::endl;
            return false;
        }

        m_unitConfigs.clear();
        m_buildingConfigs.clear();

        for(const std::string &file : files)
        {
            const fs::path entryPath(file);
            if(entryPath.filename() == "difficulty.yaml" || entryPath.filename() == "ai_difficulty.yaml")
                continue;
            if(entryPath.filename().string().rfind("research_", 0) == 0)
                continue; // technology trees, see Research.cpp

            try
            {
                std::string text;
                if(!Assets::readText(file, text))
                    continue;
                loadCharacterYaml(YAML::Load(text));
            }
            catch(const std::exception &e)
            {
                std::cerr << "[EntityManager] Error loading YAML " << entryPath << ": " << e.what() << std::endl;
            }
        }

        rebuildBlueprints();

        std::cout << "[EntityManager] Loaded " << m_unitConfigs.size() << " unit configs and " << m_buildingConfigs.size() << " building configs from YAML in " << charDir.string() << std::endl;
        return true;
    }

    bool EntityManager::loadCharacterYaml(YAML::Node root)
    {
        auto getStr = [](const YAML::Node &node, const char *key, const std::string &def = "") -> std::string
        {
            if(node && node[key])
            {
                try
                {
                    return node[key].as<std::string>();
                }
                catch(...)
                {
                }
            }
            return def;
        };
        auto getInt = [](const YAML::Node &node, const char *key, int def = 0) -> int
        {
            if(node && node[key])
            {
                try
                {
                    return node[key].as<int>();
                }
                catch(...)
                {
                }
            }
            return def;
        };
        auto getFloat = [](const YAML::Node &node, const char *key, float def = 0.0f) -> float
        {
            if(node && node[key])
            {
                try
                {
                    return node[key].as<float>();
                }
                catch(...)
                {
                }
            }
            return def;
        };
        auto getBool = [](const YAML::Node &node, const char *key, bool def = false) -> bool
        {
            if(node && node[key])
            {
                try
                {
                    return node[key].as<bool>();
                }
                catch(...)
                {
                }
            }
            return def;
        };

        if(!root.IsDefined() || !root.IsMap())
            return false;
        if(root["default_difficulty"].IsDefined())
            return false;

        std::string id = getStr(root, "id");
        std::string type = getStr(root, "type");
        const int archId = getInt(root, "archetype_id", -1);
        const int bldgIdx = getInt(root, "building_index", -1);

        // Faction, internal type and class follow from archetype_id / building_index alone (Utility tables);
        // they are not read from the file, so a file can never contradict the game about them
        Faction faction = Faction::Resistance;
        UnitType uType = UnitType::Unknown;
        if(const Utility::ArchetypeInfo *a = (type == "building") ? nullptr : Utility::archetypeInfo(archId))
        {
            faction = a->faction;
            uType = a->type;
        }
        else if(const Utility::BuildingKindInfo *k = Utility::buildingKindInfo(bldgIdx))
        {
            faction = k->faction;
            uType = k->type;
        }
        else
        {
            std::cerr << "[EntityManager] Unknown archetype_id / building_index in character '" << id << "'" << std::endl;
            return false;
        }

        std::string name = getStr(root, "name");
        // Descriptions are not stored in the character files: the text is the translation of "<id>.description"
        // (po/en.po, po/ru.po). cfg.description holds that key; the UI shows _(key).
        std::string desc = descriptionKey(id);
        std::string uClass = Utility::unitClassName(archId);

        YAML::Node statsNode = root["stats"];
        int maxHp = getInt(statsNode, "max_hp", 100);
        int sightRadius = getInt(statsNode, "sight_radius", 5);
        float moveSpeed = getFloat(statsNode, "move_speed", 1.4f);

        YAML::Node sizeNode = root["size"];
        int width = getInt(sizeNode, "width", 1);
        int height = getInt(sizeNode, "height", 1);

        YAML::Node prodNode = root["production"];
        int creditCost = getInt(prodNode, "cost", 100);
        int powerCost = getInt(prodNode, "power", 0);
        float buildTime = getFloat(prodNode, "build_time", 4.0f);
        int iconComp = getInt(prodNode, "icon_composite", 0);
        std::string producedIn = Utility::producedIn(archId);

        YAML::Node combatNode = root["combat"];
        bool canAttack = getBool(combatNode, "can_attack", type == "unit");
        bool hasRotTurret = getBool(combatNode, "has_rotating_turret", false);
        bool hasRotRadar = getBool(combatNode, "has_rotating_radar", false);

        auto getSeqFloat = [](const YAML::Node &node, size_t idx, float def = 0.0f) -> float
        {
            if(node && node.IsSequence() && idx < node.size())
            {
                try
                {
                    return node[idx].as<float>();
                }
                catch(...)
                {
                }
            }
            return def;
        };

        // Weapons live only in combat.weapons; weapons[0] is the main gun the unit / building fights with.
        // Fields a weapon leaves out come from `inherit` (the parent form's main gun for siege / upgrade forms).
        auto parseWeapon = [&](const YAML::Node &w, const WeaponConfig &inherit, size_t index)
        {
            WeaponConfig wc = inherit;
            wc.name = getStr(w, "name", index == 0 ? inherit.name : "Weapon " + std::to_string(index + 1));
            wc.projectile = stringToProjectileType(getStr(w, "projectile", projectileTypeName(inherit.projectile)));
            wc.damage = getInt(w, "damage", inherit.damage);
            wc.range = getFloat(w, "range", inherit.range);
            wc.minRange = getFloat(w, "min_range", inherit.minRange);
            // Delay between attack cycles: fixed (`cooldown`) or random within [cooldown_min, cooldown_max]
            const std::string mode = getStr(w, "cooldown_mode", inherit.cooldownMax > 0.0f ? "random" : "fixed");
            if(mode == "random")
            {
                wc.cooldown = getFloat(w, "cooldown_min", getFloat(w, "cooldown", inherit.cooldown));
                wc.cooldownMax = std::max(wc.cooldown, getFloat(w, "cooldown_max", std::max(inherit.cooldownMax, wc.cooldown)));
            }
            else
            {
                wc.cooldown = getFloat(w, "cooldown", inherit.cooldown);
                wc.cooldownMax = 0.0f;
            }
            wc.burstCount = getInt(w, "burst_count", inherit.burstCount);
            wc.burstDelay = getFloat(w, "burst_delay", inherit.burstDelay);
            wc.splashPercent = getInt(w, "splash_percent", inherit.splashPercent);
            wc.enabled = getBool(w, "enabled", true);
            wc.requiresResearch = getBool(w, "requires_research", false);
            wc.maxDistance = getFloat(w, "max_distance", 0.0f);
            if(w["localePosition"] && w["localePosition"].IsSequence())
            {
                wc.muzzleOffsetX = getSeqFloat(w["localePosition"], 0, inherit.muzzleOffsetX);
                wc.muzzleOffsetY = getSeqFloat(w["localePosition"], 1, inherit.muzzleOffsetY);
            }
            return wc;
        };
        auto parseWeapons = [&](const YAML::Node &combat, const WeaponConfig &inherit)
        {
            std::vector<WeaponConfig> list;
            const YAML::Node ws = combat ? combat["weapons"] : YAML::Node();
            if(ws && ws.IsSequence())
            {
                for(size_t wi = 0; wi < ws.size(); ++wi)
                {
                    if(ws[wi].IsMap())
                        list.push_back(parseWeapon(ws[wi], wi == 0 ? inherit : list.front(), wi));
                }
            }
            return list;
        };

        WeaponConfig noWeapon;
        noWeapon.damage = 0;
        noWeapon.range = 0.0f;
        noWeapon.cooldown = 1.0f;
        const std::vector<WeaponConfig> weapons = parseWeapons(combatNode, noWeapon);
        const WeaponConfig primary = weapons.empty() ? noWeapon : weapons.front();
        int damage = primary.damage;
        float range = primary.range;
        float cooldown = primary.cooldown;
        float cooldownMax = primary.cooldownMax;
        int burstCount = primary.burstCount;
        float burstDelay = primary.burstDelay;
        int splashPercent = primary.splashPercent;
        ProjectileType projectile = primary.projectile;
        float muzzleLocX = primary.muzzleOffsetX;
        float muzzleLocY = primary.muzzleOffsetY;

        float turretOffX = 0.0f;
        float turretOffY = 0.0f;
        bool hasCustomTurretOff = false;
        if(combatNode["turret_offset"] && combatNode["turret_offset"].IsSequence() && combatNode["turret_offset"].size() >= 2)
        {
            turretOffX = getSeqFloat(combatNode["turret_offset"], 0, 0.0f);
            turretOffY = getSeqFloat(combatNode["turret_offset"], 1, 0.0f);
            hasCustomTurretOff = true;
        }
        else if(combatNode["dome_offset"] && combatNode["dome_offset"].IsSequence() && combatNode["dome_offset"].size() >= 2)
        {
            turretOffX = getSeqFloat(combatNode["dome_offset"], 0, 0.0f);
            turretOffY = getSeqFloat(combatNode["dome_offset"], 1, 0.0f);
            hasCustomTurretOff = true;
        }
        else if(combatNode["turretOffset"] && combatNode["turretOffset"].IsSequence() && combatNode["turretOffset"].size() >= 2)
        {
            turretOffX = getSeqFloat(combatNode["turretOffset"], 0, 0.0f);
            turretOffY = getSeqFloat(combatNode["turretOffset"], 1, 0.0f);
            hasCustomTurretOff = true;
        }
        else if(combatNode["turret_pivot"] && combatNode["turret_pivot"].IsSequence() && combatNode["turret_pivot"].size() >= 2)
        {
            turretOffX = getSeqFloat(combatNode["turret_pivot"], 0, 0.0f);
            turretOffY = getSeqFloat(combatNode["turret_pivot"], 1, 0.0f);
            hasCustomTurretOff = true;
        }
        else if(combatNode["turret_offset_x"].IsDefined() || combatNode["turret_offset_y"].IsDefined() || combatNode["dome_offset_x"].IsDefined() || combatNode["dome_offset_y"].IsDefined())
        {
            turretOffX = getFloat(combatNode, "turret_offset_x", getFloat(combatNode, "dome_offset_x", 0.0f));
            turretOffY = getFloat(combatNode, "turret_offset_y", getFloat(combatNode, "dome_offset_y", 0.0f));
            hasCustomTurretOff = true;
        }

        float spreadAngleVal = -1.0f;
        if(combatNode["spread_angle"].IsDefined())
        {
            float sDeg = getFloat(combatNode, "spread_angle", 0.0f);
            spreadAngleVal = std::max(0.0f, sDeg * (static_cast<float>(M_PI) / 180.0f));
        }
        else if(combatNode["spread"].IsDefined())
        {
            float sVal = getFloat(combatNode, "spread", 0.0f);
            spreadAngleVal = std::max(0.0f, (sVal > 0.5f) ? (sVal * static_cast<float>(M_PI) / 180.0f) : sVal);
        }

        YAML::Node animNode = root["animations"];

        YAML::Node abNode = root["abilities"];
        bool isSiege = getBool(abNode, "siege_mode", false);
        int siegeArch = getInt(abNode, "siege_archetype_id", -1);
        int parentArch = getInt(abNode, "parent_archetype_id", -1);

        if(type == "unit" || archId >= 0)
        {
            UnitConfig cfg;
            cfg.archetypeId = (archId >= 0) ? archId : 0;
            cfg.id = id;
            cfg.name = name;
            cfg.type = uType;
            cfg.faction = faction;
            cfg.unitClass = uClass;
            cfg.maxHp = maxHp;
            cfg.moveSpeed = moveSpeed;
            cfg.sightRadius = sightRadius;
            cfg.attackDamage = damage;
            cfg.attackRange = range;
            cfg.attackCooldown = cooldown;
            cfg.attackCooldownMax = cooldownMax;
            cfg.burstCount = burstCount;
            cfg.burstDelay = burstDelay;
            cfg.splashPercent = splashPercent;
            cfg.projectile = projectile;
            cfg.creditCost = creditCost;
            cfg.buildTime = buildTime;
            cfg.producedIn = producedIn;
            cfg.isSiegeMode = isSiege;
            cfg.siegeArchetypeId = siegeArch;
            cfg.parentArchetypeId = parentArch;
            cfg.hasRotatingTurret = hasRotTurret;
            cfg.muzzleOffsetX = muzzleLocX;
            cfg.muzzleOffsetY = muzzleLocY;
            cfg.turretOffsetX = turretOffX;
            cfg.turretOffsetY = turretOffY;
            cfg.hasCustomTurretOffset = hasCustomTurretOff;
            cfg.spreadAngle = spreadAngleVal;

            const bool isInf = Utility::isInfantryArchetype(cfg.archetypeId);
            cfg.turnStepDelay = getFloat(root, "turn_step_delay", Utility::defaultTurnStepDelay(cfg.archetypeId));

            cfg.description = desc;

            cfg.wreckType = isInf ? "infantry" : "vehicle";
            const EffectsConfig::Wreck defaultWreck = EffectsConfig::get().wreckOfVehicle(cfg.archetypeId);
            const int defaultWreckComp = defaultWreck.composite;
            const int defaultWreckStages = defaultWreck.stages;
            cfg.wreckComposite = getInt(animNode, "wreck_composite", defaultWreckComp);
            cfg.wreckStages = getInt(animNode, "wreck_stages", defaultWreckStages);

            if(animNode["death_sequences"] && animNode["death_sequences"].IsSequence())
            {
                for(const auto &item : animNode["death_sequences"])
                {
                    try
                    {
                        cfg.deathSequences.push_back(item.as<int>());
                    }
                    catch(...)
                    {
                    }
                }
            }
            else if(animNode["death_variants"] && animNode["death_variants"].IsSequence())
            {
                for(const auto &item : animNode["death_variants"])
                {
                    try
                    {
                        cfg.deathSequences.push_back(item.as<int>());
                    }
                    catch(...)
                    {
                    }
                }
            }
            else if(isInf)
            {
                cfg.deathSequences = {247, 248};
            }

            if(animNode["flame_death_sequences"] && animNode["flame_death_sequences"].IsSequence())
            {
                for(const auto &item : animNode["flame_death_sequences"])
                {
                    try
                    {
                        cfg.flameDeathSequences.push_back(item.as<int>());
                    }
                    catch(...)
                    {
                    }
                }
            }
            else if(animNode["flame_death_variants"] && animNode["flame_death_variants"].IsSequence())
            {
                for(const auto &item : animNode["flame_death_variants"])
                {
                    try
                    {
                        cfg.flameDeathSequences.push_back(item.as<int>());
                    }
                    catch(...)
                    {
                    }
                }
            }
            else if(isInf)
            {
                cfg.flameDeathSequences = {259, 260};
            }

            cfg.weapons = weapons;
            if(cfg.weapons.empty() && canAttack)
                cfg.weapons.push_back(primary);

            // Siege / baseline forms bring their own weapons; anything they leave out stays as in the main form
            auto applyForm = [&](UnitConfig &form, const YAML::Node &formCombat)
            {
                std::vector<WeaponConfig> formWeapons = parseWeapons(formCombat, primary);
                if(!formWeapons.empty())
                {
                    const WeaponConfig &w = formWeapons.front();
                    form.attackDamage = w.damage;
                    form.attackRange = w.range;
                    form.attackCooldown = w.cooldown;
                    form.attackCooldownMax = w.cooldownMax;
                    form.burstCount = w.burstCount;
                    form.burstDelay = w.burstDelay;
                    form.splashPercent = w.splashPercent;
                    form.projectile = w.projectile;
                    form.weapons = formWeapons;
                }
                form.hasRotatingTurret = getBool(formCombat, "has_rotating_turret", form.hasRotatingTurret);
            };

            // Check for embedded siege mode configuration (consolidated)
            YAML::Node sNode = abNode["siege"].IsDefined() ? abNode["siege"] : root["siege"];
            if(sNode.IsDefined() && (getBool(sNode, "enabled", true) || sNode["archetype_id"].IsDefined()))
            {
                int sArch = getInt(sNode, "archetype_id", -1);
                if(sArch >= 0)
                {
                    UnitConfig siegeCfg = cfg;
                    siegeCfg.archetypeId = sArch;
                    siegeCfg.id = id + "_siege";
                    siegeCfg.name = getStr(sNode, "name", name + " (Siege Mode)");
                    // The siege form has its own text when "<id>_siege.description" is translated, else the unit's
                    siegeCfg.description = hasTranslation(descriptionKey(siegeCfg.id)) ? descriptionKey(siegeCfg.id) : desc;
                    siegeCfg.isSiegeMode = true;
                    siegeCfg.parentArchetypeId = cfg.archetypeId;
                    cfg.siegeArchetypeId = sArch;
                    cfg.siegeRequiresResearch = getBool(sNode, "requires_research", false);

                    if(sNode["stats"])
                    {
                        siegeCfg.maxHp = getInt(sNode["stats"], "max_hp", cfg.maxHp);
                        siegeCfg.sightRadius = getInt(sNode["stats"], "sight_radius", cfg.sightRadius);
                        siegeCfg.moveSpeed = getFloat(sNode["stats"], "move_speed", 0.0f);
                    }
                    else
                    {
                        siegeCfg.moveSpeed = 0.0f;
                    }

                    if(sNode["combat"])
                    {
                        applyForm(siegeCfg, sNode["combat"]);
                    }

                    m_unitConfigs[siegeCfg.archetypeId] = siegeCfg;
                }
            }

            // Check for embedded modernization upgrade configuration (consolidated)
            YAML::Node upNode = abNode["upgrade"].IsDefined() ? abNode["upgrade"] : root["upgrade"];
            if(upNode.IsDefined() && (getBool(upNode, "has_baseline", true) || upNode["baseline_archetype_id"].IsDefined()))
            {
                int bArch = getInt(upNode, "baseline_archetype_id", -1);
                if(bArch >= 0)
                {
                    UnitConfig baseCfg = cfg;
                    baseCfg.archetypeId = bArch;
                    baseCfg.id = id + "_base";
                    baseCfg.name = getStr(upNode, "baseline_name", name + " (Base)");
                    if(upNode["combat"])
                    {
                        applyForm(baseCfg, upNode["combat"]);
                    }
                    m_unitConfigs[baseCfg.archetypeId] = baseCfg;
                }
            }

            m_unitConfigs[cfg.archetypeId] = cfg;
        }
        else
        {
            BuildingConfig cfg;
            cfg.buildingIndex = (bldgIdx >= 0) ? bldgIdx : 0;
            cfg.id = id;
            cfg.name = name;
            cfg.type = uType;
            cfg.faction = faction;
            cfg.width = width;
            cfg.height = height;
            cfg.maxHp = maxHp;
            cfg.sightRadius = sightRadius;
            cfg.creditCost = creditCost;
            cfg.powerCost = powerCost;
            cfg.buildTime = buildTime;
            cfg.iconCompId = iconComp;
            cfg.canAttack = canAttack;
            cfg.attackDamage = damage;
            cfg.attackRange = range;
            cfg.attackCooldown = cooldown;
            cfg.attackCooldownMax = cooldownMax;
            cfg.splashPercent = splashPercent;
            cfg.projectile = projectile;
            cfg.muzzleOffsetX = muzzleLocX;
            cfg.muzzleOffsetY = muzzleLocY;
            cfg.turretOffsetX = turretOffX;
            cfg.turretOffsetY = turretOffY;
            cfg.hasCustomTurretOffset = hasCustomTurretOff;
            cfg.spreadAngle = spreadAngleVal;
            cfg.hasRotatingTurret = hasRotTurret;
            cfg.hasRotatingRadar = hasRotRadar;
            int bRadius = getInt(root, "build_radius", 16);
            if(root["stats"] && root["stats"]["build_radius"])
            {
                bRadius = getInt(root["stats"], "build_radius", bRadius);
            }
            cfg.buildRadius = bRadius;
            if(cfg.type == UnitType::Headquarters && faction == Faction::Resistance)
            {
                m_baseBuildRadius = bRadius;
                m_buildRadiusExtra = {};
            }
            cfg.wreckComposite = getInt(animNode, "wreck_composite", 0);
            cfg.wreckStages = getInt(animNode, "wreck_stages", 0);
            cfg.description = desc;

            m_buildingConfigs[cfg.buildingIndex] = cfg;
        }
        return true;
    }

    void EntityManager::rebuildBlueprints()
    {
        // Blueprints of the player's faction in canonical order: HQ, power, barracks, factory, lab, bunker, tower, wall
        s_blueprints.clear();
        const std::vector<int> bpOrder = (playerFaction() == Faction::Confederation) ? std::vector<int> {0, 1, 2, 3, 4, 5, 7, 15} : std::vector<int> {8, 9, 10, 11, 12, 13, 14, 25};
        int bpId = 0;
        for(int bIdx : bpOrder)
        {
            auto it = m_buildingConfigs.find(bIdx);
            if(it != m_buildingConfigs.end())
            {
                const auto &cfg = it->second;
                BuildingBlueprint bp;
                bp.blueprintId = bpId++;
                bp.buildingIndex = cfg.buildingIndex;
                bp.iconCompId = cfg.iconCompId;
                bp.isWall = (bIdx == 15 || bIdx == 25);
                bp.unitType = cfg.type;
                bp.name = cfg.name;
                bp.width = cfg.width;
                bp.height = cfg.height;
                bp.creditCost = cfg.creditCost;
                bp.powerCost = cfg.powerCost;
                bp.desc = cfg.description;
                s_blueprints.push_back(bp);
            }
        }
    }

    const UnitConfig *EntityManager::getUnitConfig(int archetypeId) const
    {
        auto it = m_unitConfigs.find(archetypeId);
        if(it != m_unitConfigs.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    const BuildingConfig *EntityManager::getBuildingConfig(int buildingIndex) const
    {
        auto it = m_buildingConfigs.find(buildingIndex);
        if(it != m_buildingConfigs.end())
        {
            return &it->second;
        }
        if(buildingIndex >= 15 && buildingIndex <= 24)
        {
            it = m_buildingConfigs.find(15);
            if(it != m_buildingConfigs.end())
                return &it->second;
        }
        else if(buildingIndex >= 25 && buildingIndex <= 29)
        {
            it = m_buildingConfigs.find(25);
            if(it != m_buildingConfigs.end())
                return &it->second;
        }
        return nullptr;
    }

    const UnitConfig *EntityManager::getUnitConfigByUnitType(UnitType type, Faction faction) const
    {
        for(const auto &[arch, cfg] : m_unitConfigs)
        {
            if(cfg.type == type && cfg.faction == faction && !cfg.isSiegeMode)
            {
                return &cfg;
            }
        }
        for(const auto &[arch, cfg] : m_unitConfigs)
        {
            if(cfg.type == type && cfg.faction == faction)
            {
                return &cfg;
            }
        }
        return nullptr;
    }

    void EntityManager::clear()
    {
        m_entities.clear();
        m_seenBuildings.clear();
        m_projectiles.clear();
        m_explosions.clear();
        m_smokePuffs.clear();
        m_nextId = 1;
        m_kills = {};
        // Research effects belong to the game that is being cleared
        m_buildRadiusExtra = {};
        std::fill(std::begin(m_flamethrowerUnlocked), std::end(m_flamethrowerUnlocked), false);
        std::fill(std::begin(m_siegeUnlocked), std::end(m_siegeUnlocked), false);
    }

    Entity *EntityManager::getEntity(int id)
    {
        for(auto &e : m_entities)
        {
            if(e.id == id && e.alive && e.state != ActionState::Die)
                return &e;
        }
        return nullptr;
    }

    const Entity *EntityManager::getEntity(int id) const
    {
        for(const auto &e : m_entities)
        {
            if(e.id == id && e.alive && e.state != ActionState::Die)
                return &e;
        }
        return nullptr;
    }

    Unit *EntityManager::getUnit(int id)
    {
        Entity *e = getEntity(id);
        return e ? e->asUnit() : nullptr;
    }

    const Unit *EntityManager::getUnit(int id) const
    {
        const Entity *e = getEntity(id);
        return e ? e->asUnit() : nullptr;
    }

    Building *EntityManager::getBuilding(int id)
    {
        Entity *e = getEntity(id);
        return e ? e->asBuilding() : nullptr;
    }

    const Building *EntityManager::getBuilding(int id) const
    {
        const Entity *e = getEntity(id);
        return e ? e->asBuilding() : nullptr;
    }

    int EntityManager::spawnUnit(UnitType type, int tileX, int tileY, Faction faction)
    {
        int arch = 0;
        if(faction == Faction::Resistance)
        {
            switch(type)
            {
                case UnitType::ResistanceRifleman:
                case UnitType::ConfederationRifleman:
                    arch = 0;
                    break;
                case UnitType::HeavyAssault:
                case UnitType::Grenadier:
                case UnitType::Flamethrower:
                    arch = 2;
                    break;
                case UnitType::Sniper:
                    arch = 4;
                    break;
                case UnitType::Coyote:
                case UnitType::Fortress:
                    arch = 6;
                    break;
                case UnitType::Armadillo:
                case UnitType::Hammer:
                    arch = 8;
                    break;
                case UnitType::Rhino:
                case UnitType::Zeus:
                    arch = 10;
                    break;
                case UnitType::Porcupine:
                case UnitType::Grad:
                    arch = 12;
                    break;
                default:
                    arch = 0;
                    break;
            }
        }
        else
        {
            switch(type)
            {
                case UnitType::ResistanceRifleman:
                case UnitType::ConfederationRifleman:
                    arch = 1;
                    break;
                case UnitType::HeavyAssault:
                    arch = 3;
                    break;
                case UnitType::Grenadier:
                case UnitType::Flamethrower:
                case UnitType::Sniper:
                    arch = 5;
                    break;
                case UnitType::Coyote:
                case UnitType::Fortress:
                    arch = 7;
                    break;
                case UnitType::Armadillo:
                case UnitType::Hammer:
                    arch = 9;
                    break;
                case UnitType::Rhino:
                case UnitType::Zeus:
                    arch = 11;
                    break;
                case UnitType::Porcupine:
                case UnitType::Grad:
                    arch = 13;
                    break;
                default:
                    arch = 1;
                    break;
            }
        }
        return spawnUnitByArchetype(arch, tileX, tileY, faction);
    }

    int EntityManager::spawnUnitByArchetype(int archetypeId, int tileX, int tileY, Faction faction, PlayerId owner)
    {
        if(m_entities.size() >= MAX_UNITS)
            return 0;

        archetypeId = std::clamp(archetypeId, 0, 18);

        auto u = std::make_unique<Unit>();
        u->id = m_nextId++;
        u->archetypeId = archetypeId;
        u->faction = faction;
        u->owner = owner == kAutoOwner ? Players::ownerOf(faction) : owner;
        u->tileX = tileX;
        u->tileY = tileY;
        u->subX = 0.0f;
        u->subY = 0.0f;
        u->state = ActionState::Idle;
        u->facingDirection = (faction == Faction::Resistance) ? 0 : 4;
        u->turretFacing = u->facingDirection;
        u->isSiegeMode = (archetypeId >= 14 && archetypeId <= 16);

        const UnitConfig *cfg = getUnitConfig(archetypeId);
        if(cfg)
        {
            u->type = cfg->type;
            u->maxHp = cfg->maxHp;
            u->attackDamage = cfg->attackDamage;
            u->attackRange = cfg->attackRange;
            u->attackCooldown = cfg->attackCooldown;
            u->attackCooldownMax = cfg->attackCooldownMax;
            u->burstCount = cfg->burstCount;
            u->burstDelay = cfg->burstDelay;
            u->splashPercent = cfg->splashPercent;
            u->projectile = cfg->projectile;
            u->moveSpeed = cfg->moveSpeed;
            u->sightRadius = cfg->sightRadius;
            u->hasRotatingTurret = cfg->hasRotatingTurret;
            u->muzzleOffsetX = cfg->muzzleOffsetX;
            u->muzzleOffsetY = cfg->muzzleOffsetY;
            u->turretOffsetX = cfg->turretOffsetX;
            u->turretOffsetY = cfg->turretOffsetY;
            u->hasCustomTurretOffset = cfg->hasCustomTurretOffset;
            u->spreadAngle = cfg->spreadAngle;
            u->turnStepDelay = cfg->turnStepDelay;
            u->turnTimer = 0.0f;
            u->wreckType = cfg->wreckType;
            u->wreckComposite = cfg->wreckComposite;
            u->wreckStages = cfg->wreckStages;
            u->deathSequences = cfg->deathSequences;
            u->flameDeathSequences = cfg->flameDeathSequences;
        }
        else
        {
            switch(archetypeId)
            {
                case 0:
                    u->type = UnitType::ResistanceRifleman;
                    u->maxHp = 90;
                    u->attackDamage = 12;
                    u->attackRange = 4.0f;
                    u->attackCooldown = 0.7f;
                    u->moveSpeed = 1.4f;
                    u->sightRadius = 5;
                    break;
                case 1:
                    u->type = UnitType::ConfederationRifleman;
                    u->maxHp = 90;
                    u->attackDamage = 12;
                    u->attackRange = 4.0f;
                    u->attackCooldown = 0.7f;
                    u->moveSpeed = 1.4f;
                    u->sightRadius = 5;
                    break;
                case 2:
                    u->type = UnitType::Grenadier;
                    u->maxHp = 120;
                    u->attackDamage = 30;
                    u->attackRange = 5.0f;
                    u->attackCooldown = 1.4f;
                    u->moveSpeed = 1.15f;
                    u->sightRadius = 5;
                    break;
                case 3:
                    u->type = UnitType::HeavyAssault;
                    u->maxHp = 120;
                    u->attackDamage = 32;
                    u->attackRange = 5.2f;
                    u->attackCooldown = 1.4f;
                    u->moveSpeed = 1.15f;
                    u->sightRadius = 5;
                    break;
                case 4:
                    u->type = UnitType::Sniper;
                    u->maxHp = 150;
                    u->attackDamage = 55;
                    u->attackRange = 7.5f;
                    u->attackCooldown = 2.0f;
                    u->moveSpeed = 1.3f;
                    u->sightRadius = 8;
                    break;
                case 5:
                    u->type = UnitType::Flamethrower;
                    u->maxHp = 210;
                    u->attackDamage = 40;
                    u->attackRange = 3.2f;
                    u->attackCooldown = 0.9f;
                    u->moveSpeed = 1.25f;
                    u->sightRadius = 5;
                    break;
                case 6:
                    u->type = UnitType::Coyote;
                    u->maxHp = 210;
                    u->attackDamage = 22;
                    u->attackRange = 5.0f;
                    u->attackCooldown = 0.6f;
                    u->moveSpeed = 2.4f;
                    u->sightRadius = 7;
                    break;
                case 7:
                case 18:
                    u->type = UnitType::Fortress;
                    u->maxHp = 480;
                    u->attackDamage = (archetypeId == 7 ? 55 : 42);
                    u->attackRange = 5.0f;
                    u->attackCooldown = 1.1f;
                    u->moveSpeed = 1.4f;
                    u->sightRadius = 6;
                    break;
                case 8:
                    u->type = UnitType::Armadillo;
                    u->maxHp = 380;
                    u->attackDamage = 42;
                    u->attackRange = 5.5f;
                    u->attackCooldown = 1.3f;
                    u->moveSpeed = 1.6f;
                    u->sightRadius = 6;
                    break;
                case 9:
                    u->type = UnitType::Hammer;
                    u->maxHp = 360;
                    u->attackDamage = 45;
                    u->attackRange = 5.5f;
                    u->attackCooldown = 1.3f;
                    u->moveSpeed = 1.65f;
                    u->sightRadius = 6;
                    break;
                case 10:
                    u->type = UnitType::Rhino;
                    u->maxHp = 520;
                    u->attackDamage = 65;
                    u->attackRange = 6.0f;
                    u->attackCooldown = 1.7f;
                    u->moveSpeed = 1.35f;
                    u->sightRadius = 7;
                    break;
                case 11:
                case 17:
                    u->type = UnitType::Zeus;
                    u->maxHp = 580;
                    u->attackDamage = (archetypeId == 11 ? 85 : 65);
                    u->attackRange = 6.5f;
                    u->attackCooldown = 1.7f;
                    u->moveSpeed = 1.3f;
                    u->sightRadius = 7;
                    break;
                case 12:
                    u->type = UnitType::Porcupine;
                    u->maxHp = 340;
                    u->attackDamage = 80;
                    u->attackRange = 8.5f;
                    u->attackCooldown = 2.4f;
                    u->moveSpeed = 1.4f;
                    u->sightRadius = 8;
                    break;
                case 13:
                    u->type = UnitType::Grad;
                    u->maxHp = 340;
                    u->attackDamage = 75;
                    u->attackRange = 8.0f;
                    u->attackCooldown = 2.4f;
                    u->moveSpeed = 1.35f;
                    u->sightRadius = 8;
                    break;
                case 14:
                    u->type = UnitType::Sniper;
                    u->maxHp = 150;
                    u->attackDamage = 85;
                    u->attackRange = 9.5f;
                    u->attackCooldown = 1.8f;
                    u->moveSpeed = 0.0f;
                    u->sightRadius = 10;
                    break;
                case 15:
                    u->type = UnitType::Rhino;
                    u->maxHp = 520;
                    u->attackDamage = 110;
                    u->attackRange = 9.5f;
                    u->attackCooldown = 2.2f;
                    u->moveSpeed = 0.0f;
                    u->sightRadius = 9;
                    break;
                case 16:
                    u->type = UnitType::Grad;
                    u->maxHp = 340;
                    u->attackDamage = 120;
                    u->attackRange = 11.0f;
                    u->attackCooldown = 2.6f;
                    u->moveSpeed = 0.0f;
                    u->sightRadius = 9;
                    break;
                default:
                    u->maxHp = 100;
                    break;
            }
            u->turnStepDelay = u->isInfantry() ? 0.0f : 0.10f;
            u->turnTimer = 0.0f;
            if(u->isInfantry())
            {
                u->wreckType = "infantry";
                u->deathSequences = {247, 248};
                u->flameDeathSequences = {259, 260};
            }
            else
            {
                u->wreckType = "vehicle";
                const EffectsConfig::Wreck wreck = EffectsConfig::get().wreckOfVehicle(archetypeId);
                u->wreckComposite = wreck.composite;
                u->wreckStages = wreck.stages;
            }
        }
        u->currentHp = u->maxHp;

        int newId = u->id;
        m_entities.push_back(std::move(u));
        return newId;
    }

    int EntityManager::spawnBuilding(UnitType type, int tileX, int tileY, Faction faction)
    {
        int bIdx = 0;
        switch(type)
        {
            case UnitType::Headquarters:
                bIdx = (faction == Faction::Resistance ? 8 : 0);
                break;
            case UnitType::PowerPlant:
                bIdx = (faction == Faction::Resistance ? 9 : 1);
                break;
            case UnitType::Barracks:
                bIdx = (faction == Faction::Resistance ? 10 : 2);
                break;
            case UnitType::Factory:
                bIdx = (faction == Faction::Resistance ? 11 : 3);
                break;
            case UnitType::ResearchLab:
                bIdx = (faction == Faction::Resistance ? 12 : 4);
                break;
            case UnitType::Bunker:
                bIdx = (faction == Faction::Resistance ? 13 : 5);
                break;
            case UnitType::Tower:
                bIdx = (faction == Faction::Resistance ? 14 : 7);
                break;
            default:
                bIdx = (faction == Faction::Resistance ? 8 : 0);
                break;
        }
        return spawnBuildingByIndex(bIdx, tileX, tileY, faction, 1.0f);
    }

    int EntityManager::spawnBuildingByIndex(int buildingIdx, int tileX, int tileY, Faction faction, float initialProgress, PlayerId owner)
    {
        if(m_entities.size() >= MAX_UNITS)
            return 0;

        static const uint8_t s_widths[15] = {4, 3, 3, 4, 3, 2, 2, 2, 3, 3, 3, 3, 3, 2, 2};
        static const uint8_t s_heights[15] = {4, 3, 3, 3, 3, 2, 2, 2, 4, 3, 3, 3, 3, 2, 2};

        auto b = std::make_unique<Building>();
        b->id = m_nextId++;
        b->faction = faction;
        b->owner = owner == kAutoOwner ? Players::ownerOf(faction) : owner;
        b->tileX = tileX;
        b->tileY = tileY;
        b->subX = 0.0f;
        b->subY = 0.0f;
        b->isPowered = true;
        b->state = ActionState::Idle;
        b->sightRadius = 7;
        b->rawBuildingType = buildingIdx;
        b->buildProgress = initialProgress;
        b->attackDamage = 0;

        int idxClamp = std::clamp(buildingIdx, 0, 14);
        b->buildWidth = s_widths[idxClamp];
        b->buildHeight = s_heights[idxClamp];

        const BuildingConfig *bcfg = getBuildingConfig(buildingIdx);
        if(bcfg)
        {
            b->type = bcfg->type;
            b->maxHp = bcfg->maxHp;
            b->sightRadius = bcfg->sightRadius;
            b->attackDamage = bcfg->attackDamage;
            b->attackRange = bcfg->attackRange;
            b->attackCooldown = bcfg->attackCooldown;
            b->attackCooldownMax = bcfg->attackCooldownMax;
            b->splashPercent = bcfg->splashPercent;
            b->projectile = bcfg->projectile;
            b->buildWidth = bcfg->width;
            b->buildHeight = bcfg->height;
            b->muzzleOffsetX = bcfg->muzzleOffsetX;
            b->muzzleOffsetY = bcfg->muzzleOffsetY;
            b->turretOffsetX = bcfg->turretOffsetX;
            b->turretOffsetY = bcfg->turretOffsetY;
            b->hasCustomTurretOffset = bcfg->hasCustomTurretOffset;
            b->hasRotatingTurret = bcfg->hasRotatingTurret;
            b->wreckComposite = bcfg->wreckComposite;
            b->wreckStages = bcfg->wreckStages;
            b->spreadAngle = bcfg->spreadAngle;
        }
        else
        {
            switch(idxClamp)
            {
                case 0:
                    b->type = UnitType::Headquarters;
                    b->maxHp = 2500;
                    b->sightRadius = 8;
                    break;
                case 1:
                    b->type = UnitType::PowerPlant;
                    b->maxHp = 1200;
                    break;
                case 2:
                    b->type = UnitType::Barracks;
                    b->maxHp = 1200;
                    break;
                case 3:
                    b->type = UnitType::Factory;
                    b->maxHp = 1500;
                    break;
                case 4:
                    b->type = UnitType::ResearchLab;
                    b->maxHp = 1000;
                    break;
                case 5:
                    b->type = UnitType::Bunker;
                    b->maxHp = 2500;
                    b->attackDamage = 30;
                    b->attackRange = 6.0f;
                    b->attackCooldown = 0.7f;
                    break;
                case 6:
                    b->type = UnitType::Tower;
                    b->maxHp = 1200;
                    b->attackDamage = 0;
                    b->sightRadius = 11;
                    break;
                case 7:
                    b->type = UnitType::Bunker;
                    b->maxHp = 1600;
                    b->attackDamage = 65;
                    b->attackRange = 8.5f;
                    b->attackCooldown = 1.3f;
                    b->sightRadius = 9;
                    break;
                case 8:
                    b->type = UnitType::Headquarters;
                    b->maxHp = 2500;
                    b->attackDamage = 18;
                    b->attackRange = 5.5f;
                    b->attackCooldown = 0.8f;
                    b->sightRadius = 8;
                    break;
                case 9:
                    b->type = UnitType::PowerPlant;
                    b->maxHp = 1200;
                    break;
                case 10:
                    b->type = UnitType::Barracks;
                    b->maxHp = 1500;
                    b->attackDamage = 15;
                    b->attackRange = 5.0f;
                    b->attackCooldown = 0.9f;
                    break;
                case 11:
                    b->type = UnitType::Factory;
                    b->maxHp = 1200;
                    break;
                case 12:
                    b->type = UnitType::ResearchLab;
                    b->maxHp = 1200;
                    b->attackDamage = 15;
                    b->attackRange = 5.0f;
                    b->attackCooldown = 0.9f;
                    break;
                case 13:
                    b->type = UnitType::Bunker;
                    b->maxHp = 1800;
                    b->attackDamage = 32;
                    b->attackRange = 6.0f;
                    b->attackCooldown = 0.65f;
                    break;
                case 14:
                    b->type = UnitType::Tower;
                    b->maxHp = 1600;
                    b->attackDamage = 40;
                    b->attackRange = 7.5f;
                    b->attackCooldown = 0.85f;
                    b->sightRadius = 9;
                    break;
                default:
                    b->type = UnitType::Headquarters;
                    b->maxHp = 1000;
                    break;
            }
        }

        if(initialProgress < 1.0f)
        {
            b->currentHp = std::max(50, static_cast<int>(b->maxHp * 0.10f));
        }
        else
        {
            b->currentHp = b->maxHp;
        }

        int newId = b->id;
        m_entities.push_back(std::move(b));
        return newId;
    }

    bool EntityManager::canPlaceBuilding(int tileX, int tileY, int width, int height, const TileMap &map, PlayerId player) const
    {
        if(tileX < 0 || tileX + width > map.getWidth() || tileY < 0 || tileY + height > map.getHeight())
        {
            return false;
        }

        // 1. Fog of War check: building can only be placed on Visible tiles, NOT on Fog
        if(map.isFogEnabled() && Players::slot(player).human)
        {
            const uint8_t team = Players::teamOf(player);
            for(int ty = tileY; ty < tileY + height; ++ty)
            {
                for(int tx = tileX; tx < tileX + width; ++tx)
                {
                    if(!map.isVisibleTo(tx, ty, team))
                    {
                        return false;
                    }
                }
            }
        }

        // 2. Build radius check: must be within build_radius of ANY friendly Headquarters
        bool hasFriendlyHQ = false;
        bool inHQRange = false;
        float buildCenterX = tileX + width * 0.5f;
        float buildCenterY = tileY + height * 0.5f;

        for(const auto &e : m_entities)
        {
            if(!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner != player)
                continue;
            const Building *b = e.asBuilding();
            if(b && b->type == UnitType::Headquarters)
            {
                hasFriendlyHQ = true;
                float hqCenterX = b->tileX + b->buildWidth * 0.5f;
                float hqCenterY = b->tileY + b->buildHeight * 0.5f;
                float dist = DetMath::hypot(buildCenterX - hqCenterX, buildCenterY - hqCenterY);
                if(dist <= static_cast<float>(getBuildRadius(player)))
                {
                    inHQRange = true;
                    break;
                }
            }
        }
        if(hasFriendlyHQ && !inHQRange)
        {
            return false;
        }

        // 3. No construction under the enemy's nose: a visible enemy unit or building near the footprint blocks it
        const int block = map.getConstructionRules().enemyBlockTiles;
        if(block > 0)
        {
            for(const auto &e : m_entities)
            {
                if(!e.alive || e.state == ActionState::Die || e.owner >= kMaxPlayers || !Players::hostile(e.owner, player))
                    continue;
                int ex0 = e.tileX, ey0 = e.tileY, ex1 = e.tileX, ey1 = e.tileY;
                if(const Building *b = e.asBuilding())
                {
                    ex1 = b->tileX + b->buildWidth - 1;
                    ey1 = b->tileY + b->buildHeight - 1;
                }
                const int gapX = std::max({0, ex0 - (tileX + width - 1), tileX - ex1});
                const int gapY = std::max({0, ey0 - (tileY + height - 1), tileY - ey1});
                if(std::max(gapX, gapY) > block)
                    continue;
                if(isEntityVisibleToPlayer(e, player, map))
                    return false;
            }
        }

        for(int ty = tileY; ty < tileY + height; ++ty)
        {
            for(int tx = tileX; tx < tileX + width; ++tx)
            {
                if(!map.isPassable(tx, ty))
                    return false;
                if(map.getWall(tx, ty) > 0)
                    return false;
                for(const auto &e : m_entities)
                {
                    if(!e.alive || e.state == ActionState::Die)
                        continue;
                    if(const Building *b = e.asBuilding())
                    {
                        if(tx >= b->tileX && tx < b->tileX + b->buildWidth && ty >= b->tileY && ty < b->tileY + b->buildHeight)
                        {
                            return false;
                        }
                    }
                    else
                    {
                        if(e.tileX == tx && e.tileY == ty)
                        {
                            return false;
                        }
                    }
                }
            }
        }
        if(!Players::slot(player).human)
            return keepsBaseOpen(tileX, tileY, width, height, map, player);
        return true;
    }

    bool EntityManager::keepsBaseOpen(int tileX, int tileY, int width, int height, const TileMap &map, PlayerId player) const
    {
        const int W = map.getWidth(), H = map.getHeight();
        std::vector<uint8_t> blocked(static_cast<size_t>(W) * H, 0);
        for(int y = 0; y < H; ++y)
        {
            for(int x = 0; x < W; ++x)
            {
                if(!map.isPassable(x, y) || map.getWall(x, y) > 0)
                    blocked[static_cast<size_t>(y) * W + x] = 1;
            }
        }
        // Where the units of the player come out: the headquarters and every unit producer
        std::vector<std::pair<int, int>> anchors;
        for(const auto &e : m_entities)
        {
            if(!e.alive || e.state == ActionState::Die || !e.isBuilding)
                continue;
            const Building *b = e.asBuilding();
            if(!b)
                continue;
            for(int y = b->tileY; y < b->tileY + b->buildHeight; ++y)
            {
                for(int x = b->tileX; x < b->tileX + b->buildWidth; ++x)
                {
                    if(x >= 0 && y >= 0 && x < W && y < H)
                        blocked[static_cast<size_t>(y) * W + x] = 1;
                }
            }
            if(b->owner == player && (b->type == UnitType::Headquarters || b->isUnitProducer()))
            {
                int sx = 0, sy = 0;
                if(getBuildingSpawnPoint(b->id, map, sx, sy) && sx >= 0 && sy >= 0 && sx < W && sy < H)
                    anchors.push_back({sx, sy});
            }
        }
        if(anchors.empty())
            return true;
        const auto idx = [&](int x, int y) { return static_cast<size_t>(y) * W + x; };
        // A producer's exit stands on a free tile; the first anchor (the oldest building) is the one that is followed
        if(blocked[idx(anchors[0].first, anchors[0].second)])
            return true; // nothing to measure from

        auto flood = [&](const std::vector<uint8_t> &block, std::vector<uint8_t> &seen)
        {
            seen.assign(block.size(), 0);
            std::vector<int> stack {anchors[0].second * W + anchors[0].first};
            seen[stack[0]] = 1;
            int count = 0;
            while(!stack.empty())
            {
                const int cur = stack.back();
                stack.pop_back();
                ++count;
                const int x = cur % W, y = cur / W;
                const int nx[4] = {x + 1, x - 1, x, x}, ny[4] = {y, y, y + 1, y - 1};
                for(int k = 0; k < 4; ++k)
                {
                    if(nx[k] < 0 || ny[k] < 0 || nx[k] >= W || ny[k] >= H)
                        continue;
                    const size_t n = idx(nx[k], ny[k]);
                    if(block[n] || seen[n])
                        continue;
                    seen[n] = 1;
                    stack.push_back(ny[k] * W + nx[k]);
                }
            }
            return count;
        };

        std::vector<uint8_t> before, after;
        const int reachBefore = flood(blocked, before);
        std::vector<uint8_t> withNew = blocked;
        int footprintReached = 0;
        for(int y = tileY; y < tileY + height; ++y)
        {
            for(int x = tileX; x < tileX + width; ++x)
            {
                if(x < 0 || y < 0 || x >= W || y >= H)
                    continue;
                if(before[idx(x, y)])
                    ++footprintReached;
                withNew[idx(x, y)] = 1;
            }
        }
        if(withNew[idx(anchors[0].first, anchors[0].second)])
            return false;
        const int reachAfter = flood(withNew, after);

        // A few tiles may end up in dead corners behind the building; a whole area cut off is a closed base
        constexpr int kDeadCornerTiles = 6;
        if(reachBefore - reachAfter - footprintReached > kDeadCornerTiles)
            return false;
        for(const auto &a : anchors)
        {
            if(before[idx(a.first, a.second)] && !after[idx(a.first, a.second)])
                return false;
        }
        // The new building has a free, reachable side of its own (its exit)
        bool hasExit = false;
        for(int x = tileX - 1; x <= tileX + width && !hasExit; ++x)
        {
            for(int y = tileY - 1; y <= tileY + height && !hasExit; ++y)
            {
                const bool inside = x >= tileX && x < tileX + width && y >= tileY && y < tileY + height;
                if(inside || x < 0 || y < 0 || x >= W || y >= H)
                    continue;
                hasExit = after[idx(x, y)] != 0;
            }
        }
        return hasExit;
    }

    bool EntityManager::hasBuildingType(UnitType type, PlayerId owner, bool mustBeComplete) const
    {
        for(const auto &e : m_entities)
        {
            if(!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner != owner)
                continue;
            const Building *b = e.asBuilding();
            if(mustBeComplete && b && b->buildProgress < 1.0f)
                continue;
            if(e.type == type)
                return true;
        }
        return false;
    }

    std::vector<int> EntityManager::getBuildingsByType(UnitType type, PlayerId owner, bool mustBeComplete) const
    {
        std::vector<int> result;
        for(const auto &e : m_entities)
        {
            if(!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner != owner)
                continue;
            const Building *b = e.asBuilding();
            if(mustBeComplete && b && b->buildProgress < 1.0f)
                continue;
            if(e.type == type)
                result.push_back(e.id);
        }
        return result;
    }

    bool EntityManager::getBuildingSpawnPoint(int buildingId, const TileMap &map, int &outTileX, int &outTileY) const
    {
        const Building *b = getBuilding(buildingId);
        if(!b)
            return false;

        auto isFreeSpawnTile = [&](int x, int y) -> bool
        {
            if(!map.isPassable(x, y))
                return false;
            if(map.getWall(x, y) > 0)
                return false;
            if(isTileOccupiedByUnit(x, y))
                return false;
            for(const auto &e : m_entities)
            {
                if(!e.alive || e.state == ActionState::Die || !e.isBuilding)
                    continue;
                const Building *eb = e.asBuilding();
                if(eb && x >= eb->tileX && x < eb->tileX + eb->buildWidth && y >= eb->tileY && y < eb->tileY + eb->buildHeight)
                {
                    return false;
                }
            }
            return true;
        };

        // 1. Bottom side
        for(int x = b->tileX; x < b->tileX + b->buildWidth; ++x)
        {
            int y = b->tileY + b->buildHeight;
            if(isFreeSpawnTile(x, y))
            {
                outTileX = x;
                outTileY = y;
                return true;
            }
        }
        // 2. Right side
        for(int y = b->tileY; y < b->tileY + b->buildHeight; ++y)
        {
            int x = b->tileX + b->buildWidth;
            if(isFreeSpawnTile(x, y))
            {
                outTileX = x;
                outTileY = y;
                return true;
            }
        }
        // 3. Left side
        for(int y = b->tileY; y < b->tileY + b->buildHeight; ++y)
        {
            int x = b->tileX - 1;
            if(isFreeSpawnTile(x, y))
            {
                outTileX = x;
                outTileY = y;
                return true;
            }
        }
        // 4. Top side
        for(int x = b->tileX; x < b->tileX + b->buildWidth; ++x)
        {
            int y = b->tileY - 1;
            if(isFreeSpawnTile(x, y))
            {
                outTileX = x;
                outTileY = y;
                return true;
            }
        }
        // 5. Fallback: search in radius 2..5
        for(int r = 2; r <= 5; ++r)
        {
            for(int dy = -r; dy <= b->buildHeight + r; ++dy)
            {
                for(int dx = -r; dx <= b->buildWidth + r; ++dx)
                {
                    int tx = b->tileX + dx;
                    int ty = b->tileY + dy;
                    if(isFreeSpawnTile(tx, ty))
                    {
                        outTileX = tx;
                        outTileY = ty;
                        return true;
                    }
                }
            }
        }
        outTileX = b->tileX + b->buildWidth;
        outTileY = b->tileY;
        return true;
    }

    bool EntityManager::isTileOccupiedByUnit(int tileX, int tileY, int ignoreEntityId) const
    {
        for(const auto &e : m_entities)
        {
            if(!e.alive || e.state == ActionState::Die || e.isBuilding)
                continue;
            if(e.id == ignoreEntityId)
                continue;
            if(e.occupiesTile(tileX, tileY))
                return true;
        }
        return false;
    }

    int EntityManager::getUnitIdAtTile(int tileX, int tileY, int ignoreEntityId) const
    {
        for(const auto &e : m_entities)
        {
            if(!e.alive || e.state == ActionState::Die || e.isBuilding)
                continue;
            if(e.id == ignoreEntityId)
                continue;
            if(e.occupiesTile(tileX, tileY))
                return e.id;
        }
        return 0;
    }

    void EntityManager::resolveUnitStacking(TileMap &map)
    {
        std::vector<Unit *> livingUnits;
        for(auto &e : m_entities)
        {
            if(!e.alive || e.state == ActionState::Die || e.isBuilding)
                continue;
            if(Unit *u = e.asUnit())
            {
                livingUnits.push_back(u);
            }
        }

        for(size_t i = 0; i < livingUnits.size(); ++i)
        {
            Unit *u1 = livingUnits[i];
            for(size_t j = i + 1; j < livingUnits.size(); ++j)
            {
                Unit *u2 = livingUnits[j];
                if(u1->tileX == u2->tileX && u1->tileY == u2->tileY)
                {
                    // Two units occupy the exact same tile: push u2 to the nearest free, passable, unblocked tile
                    static constexpr int offsets[8][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
                    for(const auto &off : offsets)
                    {
                        int nx = u2->tileX + off[0];
                        int ny = u2->tileY + off[1];
                        if(nx >= 0 && nx < map.getWidth() && ny >= 0 && ny < map.getHeight())
                        {
                            if(map.isPassable(nx, ny) && map.getWall(nx, ny) == 0 && !isTileOccupiedByUnit(nx, ny, u2->id))
                            {
                                u2->tileX = nx;
                                u2->tileY = ny;
                                u2->subX = 0.0f;
                                u2->subY = 0.0f;
                                u2->releaseReservation();
                                if(u2->state == ActionState::Move)
                                {
                                    u2->state = ActionState::Idle;
                                    u2->path.clear();
                                    u2->waitingForPath = true;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    const std::vector<BuildingBlueprint> &EntityManager::getBlueprints()
    {
        if(!s_blueprints.empty())
        {
            return s_blueprints;
        }
        static const std::vector<BuildingBlueprint> defaultBlueprints = {
            {0, 8, 2074, UnitType::Headquarters, "HEADQUARTERS", 3, 4, 800, 50, "Resistance Command Center. Provides radar and logistics."},
            {1, 9, 2075, UnitType::PowerPlant, "POWER PLANT", 3, 3, 300, 100, "Power Plant (+100 kW). Supplies energy to factories and defenses."},
            {2, 10, 2076, UnitType::Barracks, "BARRACKS", 3, 3, 400, -20, "Infantry Training: Rifleman, Grenadier, Sniper."},
            {3, 11, 2077, UnitType::Factory, "FACTORY", 3, 3, 600, -40, "Vehicle Factory: Coyote, Armadillo, Rhino, and Porcupine MLRS."},
            {4, 12, 2078, UnitType::ResearchLab, "RESEARCH LAB", 3, 3, 500, -30, "Scientific center for combat research upgrades (RESEARCH tab)."},
            {5, 13, 2079, UnitType::Bunker, "BUNKER", 2, 2, 350, -15, "Reinforced machine-gun bunker for perimeter defense."},
            {6, 14, 2080, UnitType::Tower, "TURRET", 2, 2, 250, -10, "Watchtower with heavy machine gun and extended vision."},
            {7, 25, 2081, UnitType::Wall, "WALL", 1, 1, 50, 0, "Reinforced concrete wall (1x1) to block enemy armor.", true}};
        return defaultBlueprints;
    }

    static float computeDistanceToEntityPixels(const Unit &attacker, const Entity &target)
    {
        float srcX = attacker.getUnitCenterPixelX();
        float srcY = attacker.getUnitCenterPixelY();

        if(target.isBuilding)
        {
            const Building *b = target.asBuilding();
            int w = b ? std::max(1, b->buildWidth) : 1;
            int h = b ? std::max(1, b->buildHeight) : 1;

            float minCenterX = target.tileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
            float maxCenterX = (target.tileX + w - 1) * TILE_WIDTH + (TILE_WIDTH * 0.5f);
            float minCenterY = target.tileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
            float maxCenterY = (target.tileY + h - 1) * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);

            float closestX = std::clamp(srcX, minCenterX, maxCenterX);
            float closestY = std::clamp(srcY, minCenterY, maxCenterY);
            return DetMath::hypot(closestX - srcX, closestY - srcY);
        }

        float dstX = target.getUnitCenterPixelX();
        float dstY = target.getUnitCenterPixelY();
        return DetMath::hypot(dstX - srcX, dstY - srcY);
    }

    void EntityManager::issueMoveOrder(const std::vector<int> &unitIds, int targetTileX, int targetTileY, Pathfinder &pathfinder, bool attackMove)
    {
        pathfinder.setEntityManager(this);
        const TileMap *map = pathfinder.getMap();
        if(!map || unitIds.empty())
            return;

        int mapW = map->getWidth();
        int mapH = map->getHeight();
        if(targetTileX < 0 || targetTileY < 0 || targetTileX >= mapW || targetTileY >= mapH)
        {
            return; // Reject invalid negative or out-of-bounds ground coordinates
        }

        std::vector<int> validUnits;
        float sumUnitX = 0.0f, sumUnitY = 0.0f;
        for(int uid : unitIds)
        {
            Unit *u = getUnit(uid);
            if(!u || !u->alive || u->state == ActionState::Die || u->state == ActionState::Transform)
                continue;
            if(u->isSiegeMode || u->moveSpeed <= 0.0f)
            {
                u->clearMoveTarget();
                u->clearPathState();
                // Attack-move [Q] to a deployed unit: it keeps firing while it sees enemies, then packs up and follows
                const UnitConfig *cfg = getUnitConfig(u->archetypeId);
                if(attackMove && u->isSiegeMode && cfg && cfg->parentArchetypeId >= 0)
                {
                    u->targetTileX = targetTileX;
                    u->targetTileY = targetTileY;
                    u->isAttackMoving = true;
                    u->autoDeployed = true;
                }
                continue;
            }
            validUnits.push_back(uid);
            sumUnitX += static_cast<float>(u->tileX);
            sumUnitY += static_cast<float>(u->tileY);
        }
        if(validUnits.empty())
            return;

        float unitCentroidX = sumUnitX / static_cast<float>(validUnits.size());
        float unitCentroidY = sumUnitY / static_cast<float>(validUnits.size());

        std::vector<StormcastSlot> slots;
        if(pathfinder.getMap())
        {
            slots = AINavigataionStormcastView::computeGroupSlots(targetTileX, targetTileY, static_cast<int>(validUnits.size()), *pathfinder.getMap(), this, validUnits);
        }

        // Match each unit to the slot that preserves its relative formation offset from the squad centroid
        // (Prevents units from crossing paths and colliding with each other!)
        if(slots.size() == validUnits.size() && validUnits.size() > 1)
        {
            float sumSlotX = 0.0f, sumSlotY = 0.0f;
            for(const auto &s : slots)
            {
                sumSlotX += static_cast<float>(s.x);
                sumSlotY += static_cast<float>(s.y);
            }
            float slotCentroidX = sumSlotX / static_cast<float>(slots.size());
            float slotCentroidY = sumSlotY / static_cast<float>(slots.size());

            std::vector<StormcastSlot> matchedSlots(validUnits.size());
            std::vector<bool> slotUsed(slots.size(), false);

            for(size_t i = 0; i < validUnits.size(); ++i)
            {
                const Unit *u = getUnit(validUnits[i]);
                float relX = u ? (static_cast<float>(u->tileX) - unitCentroidX) : 0.0f;
                float relY = u ? (static_cast<float>(u->tileY) - unitCentroidY) : 0.0f;
                relX = std::clamp(relX, -4.0f, 4.0f);
                relY = std::clamp(relY, -4.0f, 4.0f);
                float idealX = slotCentroidX + relX;
                float idealY = slotCentroidY + relY;

                size_t bestIdx = 0;
                float bestDistSq = 1e18f;
                for(size_t k = 0; k < slots.size(); ++k)
                {
                    if(slotUsed[k])
                        continue;
                    float dx = static_cast<float>(slots[k].x) - idealX;
                    float dy = static_cast<float>(slots[k].y) - idealY;
                    float dsq = dx * dx + dy * dy;
                    if(dsq < bestDistSq)
                    {
                        bestDistSq = dsq;
                        bestIdx = k;
                    }
                }
                slotUsed[bestIdx] = true;
                matchedSlots[i] = slots[bestIdx];
            }
            slots = std::move(matchedSlots);
        }

        // Pre-mark squad units as moving so Pathfinder::syncLocks does not treat them as impassable walls
        for(int uid : validUnits)
        {
            if(Unit *u = getUnit(uid))
            {
                u->isAttackMoving = attackMove;
                u->state = ActionState::Move;
            }
        }

        std::vector<std::pair<int, int>> assignedEndTiles;
        for(size_t i = 0; i < validUnits.size(); ++i)
        {
            int uid = validUnits[i];
            Unit *u = getUnit(uid);
            if(!u)
                continue;

            int destX = (i < slots.size()) ? std::clamp(slots[i].x, 0, mapW - 1) : targetTileX;
            int destY = (i < slots.size()) ? std::clamp(slots[i].y, 0, mapH - 1) : targetTileY;

            u->targetEntityId = 0;
            u->targetWallTileX = -1;
            u->targetWallTileY = -1;
            u->isManualWallTarget = false;
            u->isDirectEngaged = false;
            u->isSupportAssisting = false;
            u->isAttackMoving = attackMove;

            if(u->tileX == destX && u->tileY == destY && std::abs(u->subX) <= 0.001f && std::abs(u->subY) <= 0.001f)
            {
                u->clearMoveTarget();
                u->clearPathState();
                u->state = ActionState::Idle;
                continue;
            }

            u->targetTileX = destX;
            u->targetTileY = destY;
            u->waitingForPath = false;
            u->pathRetryTimer = 0.0f;
            u->attackAnimTimer = 0.0f;
            u->burstShotsRemaining = 0;
            u->burstTimer = 0.0f;

            bool isPartial = false;
            if(pathfinder.findPath(u->tileX, u->tileY, destX, destY, u->path, u->id, &isPartial) && !u->path.empty())
            {
                if(isPartial && !u->path.empty())
                {
                    int endX = u->path.back().x;
                    int endY = u->path.back().y;
                    for(const auto &pt : assignedEndTiles)
                    {
                        if(pt.first == endX && pt.second == endY)
                        {
                            while(u->path.size() > 1)
                            {
                                u->path.pop_back();
                                endX = u->path.back().x;
                                endY = u->path.back().y;
                                bool clash = false;
                                for(const auto &p2 : assignedEndTiles)
                                {
                                    if(p2.first == endX && p2.second == endY)
                                    {
                                        clash = true;
                                        break;
                                    }
                                }
                                if(!clash)
                                    break;
                            }
                            break;
                        }
                    }
                }
                if(!u->path.empty())
                {
                    assignedEndTiles.push_back({u->path.back().x, u->path.back().y});
                }
                u->pathIndex = 0;
                u->state = ActionState::Move;
                u->waitingForPath = isPartial;
                u->waitBlockedTimer = 0.0f;
            }
            else
            {
                u->clearPathState();
                u->state = ActionState::Idle;
                u->waitingForPath = true;
                u->pathRetryTimer = 0.0f;
            }
        }
    }

    void EntityManager::issueAttackMoveOrder(const std::vector<int> &unitIds, int targetTileX, int targetTileY, Pathfinder &pathfinder)
    {
        issueMoveOrder(unitIds, targetTileX, targetTileY, pathfinder, true);
    }

    void EntityManager::issueAttackOrder(const std::vector<int> &unitIds, int targetEntityId, Pathfinder *pathfinder)
    {
        Entity *target = getEntity(targetEntityId);
        if(!target || !target->alive || target->state == ActionState::Die)
            return;

        if(pathfinder)
        {
            pathfinder->setEntityManager(this);
        }

        for(int uid : unitIds)
        {
            Unit *u = getUnit(uid);
            if(!u || !u->alive || u->state == ActionState::Die || u->state == ActionState::Transform || u->id == targetEntityId)
                continue;
            if(!Players::hostile(u->owner, target->owner))
                continue;

            // 1. If already targeting this exact entity, do NOT reset state or animation frame!
            if(u->targetEntityId == targetEntityId && !u->isManualWallTarget)
            {
                float dist = computeDistanceToEntityPixels(*u, *target);
                if(dist <= u->attackRange * TILE_WIDTH)
                {
                    continue; // Already engaged in range; let updateCombat manage cooldown & firing anim
                }
            }

            // 2. Compute distance to target (footprint-aware for multi-tile buildings)
            float dist = computeDistanceToEntityPixels(*u, *target);
            bool inRange = (dist <= u->attackRange * TILE_WIDTH);

            // 3. Siege Mode / Immobile Unit Strict Gate:
            // Cannot move to pursue targets out of range. Reject out-of-range orders cleanly!
            if(u->isSiegeMode || u->moveSpeed <= 0.0f)
            {
                if(!inRange)
                {
                    u->targetEntityId = 0;
                    u->isDirectEngaged = false;
                    u->clearMoveTarget();
                    u->clearPathState();
                    if(u->state == ActionState::Attack && u->attackAnimTimer <= 0.0f)
                    {
                        u->state = ActionState::Idle;
                        u->animFrame = 0;
                    }
                    continue;
                }

                // Target is within Siege firing range: assign targetEntityId and let updateCombat fire
                u->targetEntityId = targetEntityId;
                u->targetWallTileX = -1;
                u->targetWallTileY = -1;
                u->isManualWallTarget = false;
                u->isAttackMoving = false;
                u->clearMoveTarget();
                u->clearPathState();
                continue;
            }

            // 4. Mobile Unit Order Handling
            u->targetEntityId = targetEntityId;
            u->targetWallTileX = -1;
            u->targetWallTileY = -1;
            u->isManualWallTarget = false;
            u->isAttackMoving = false;
            u->isSupportAssisting = false;
            // Pure entity pursuit must NOT store ground move coordinates (prevents (0,0) bug!)
            u->clearMoveTarget();

            if(inRange)
            {
                u->clearPathState();
                u->state = ActionState::Attack;
            }
            else if(pathfinder)
            {
                bool isPartial = false;
                if(pathfinder->findPath(u->tileX, u->tileY, target->tileX, target->tileY, u->path, u->id, &isPartial))
                {
                    u->pathIndex = 0;
                    u->state = ActionState::Move;
                    u->waitingForPath = isPartial;
                }
                else
                {
                    u->state = ActionState::Idle;
                    u->waitingForPath = true;
                }
            }
        }
    }

    void EntityManager::issueAttackWallOrder(const std::vector<int> &unitIds, int wallTileX, int wallTileY, Pathfinder &pathfinder)
    {
        pathfinder.setEntityManager(this);
        const TileMap *map = pathfinder.getMap();
        if(!map || map->getWall(wallTileX, wallTileY) == 0)
            return;

        for(int uid : unitIds)
        {
            Unit *u = getUnit(uid);
            if(!u || !u->alive || u->state == ActionState::Die || u->state == ActionState::Transform || u->attackDamage <= 0)
                continue;

            float srcX = u->getUnitCenterPixelX();
            float srcY = u->getUnitCenterPixelY();
            float dstX = wallTileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
            float dstY = wallTileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
            float dist = DetMath::hypot(dstX - srcX, dstY - srcY);
            bool inRange = (dist <= u->attackRange * TILE_WIDTH);

            if(u->isSiegeMode || u->moveSpeed <= 0.0f)
            {
                if(!inRange)
                {
                    // Reject out-of-range wall target for immobile siege unit
                    u->targetWallTileX = -1;
                    u->targetWallTileY = -1;
                    u->isManualWallTarget = false;
                    u->targetEntityId = 0;
                    if(u->state == ActionState::Attack && u->attackAnimTimer <= 0.0f)
                    {
                        u->state = ActionState::Idle;
                        u->animFrame = 0;
                    }
                    continue;
                }

                u->targetEntityId = 0;
                u->targetWallTileX = wallTileX;
                u->targetWallTileY = wallTileY;
                u->isManualWallTarget = true;
                u->isAttackMoving = false;
                u->clearMoveTarget();
                u->clearPathState();
                continue;
            }

            u->targetEntityId = 0;
            u->targetWallTileX = wallTileX;
            u->targetWallTileY = wallTileY;
            u->isManualWallTarget = true;
            u->isAttackMoving = false;
            u->isSupportAssisting = false;
            u->clearMoveTarget();

            if(inRange)
            {
                u->clearPathState();
                u->state = ActionState::Attack;
            }
            else
            {
                bool isPartial = false;
                if(pathfinder.findPath(u->tileX, u->tileY, wallTileX, wallTileY, u->path, u->id, &isPartial))
                {
                    u->pathIndex = 0;
                    u->state = ActionState::Move;
                    u->waitingForPath = isPartial;
                }
                else
                {
                    u->state = ActionState::Idle;
                    u->waitingForPath = true;
                }
            }
        }
    }

    int EntityManager::getBuildRadius(PlayerId player) const
    {
        return m_baseBuildRadius + (player < kMaxPlayers ? m_buildRadiusExtra[player] : 0);
    }

    void EntityManager::setBuildRadiusPercent(PlayerId player, int percent)
    {
        if(player >= kMaxPlayers || !Players::slot(player).human)
            return;
        m_buildRadiusExtra[player] = m_baseBuildRadius * (100 + percent) / 100 - m_baseBuildRadius;
    }

    bool EntityManager::canDeploySiege(const Unit &u) const
    {
        const UnitConfig *cfg = getUnitConfig(u.archetypeId);
        if(cfg && cfg->isSiegeMode)
            return true; // packing up is always possible
        if(cfg && cfg->siegeRequiresResearch && !isSiegeUnlocked(u.owner))
            return false;
        return true;
    }

    void EntityManager::updateAutoSiege(Unit &u, const TileMap &map, float deltaTime)
    {
        if(u.state == ActionState::Die || u.state == ActionState::Transform)
            return;
        const UnitConfig *cfg = getUnitConfig(u.archetypeId);
        if(!cfg)
            return;

        // Who may decide by itself: the AI always, the player's units only while on an attack-move order
        const bool aiSide = !Players::slot(u.owner).human; // a computer player (the same on every peer)
        auto closestVisibleEnemy = [&](float maxTiles)
        {
            float best = maxTiles * TILE_WIDTH;
            int bestId = 0;
            for(const auto &other : m_entities)
            {
                if(!other.alive || other.state == ActionState::Die || !Players::hostile(other.owner, u.owner))
                    continue;
                if(!isEntityVisibleToPlayer(other, u.owner, map))
                    continue; // fog / camouflage do not count
                const float d = computeDistanceToEntityPixels(u, other);
                if(d <= best)
                {
                    best = d;
                    bestId = other.id;
                }
            }
            return bestId;
        };

        if(!cfg->isSiegeMode)
        {
            u.siegeQuietTimer = 0.0f;
            if(cfg->siegeArchetypeId < 0 || !canDeploySiege(u))
                return;
            if(!aiSide && !u.isAttackMoving)
                return;
            const UnitConfig *siege = getUnitConfig(cfg->siegeArchetypeId);
            if(!siege)
                return;
            // Both forms with the faction's range technologies (Hornet-10 alone already gives the sniper 9.5)
            const float siegeRange = researchedAttackRange(u.owner, siege->archetypeId, siege->attackRange);
            if(siegeRange <= u.attackRange + 0.25f)
                return;
            // Something to shoot in normal range: no need to deploy
            if(u.targetEntityId != 0)
            {
                const Entity *t = getEntity(u.targetEntityId);
                if(t && computeDistanceToEntityPixels(u, *t) <= u.attackRange * TILE_WIDTH)
                    return;
            }
            if(closestVisibleEnemy(u.attackRange) != 0)
                return;
            if(closestVisibleEnemy(siegeRange) == 0)
                return;
            // The order stays: once nothing is left to shoot the unit packs up and carries on toward it
            const int keepX = u.targetTileX, keepY = u.targetTileY;
            const bool keepAttackMove = u.isAttackMoving;
            toggleSiegeMode({u.id});
            u.targetTileX = keepX;
            u.targetTileY = keepY;
            u.isAttackMoving = keepAttackMove;
            u.autoDeployed = true;
            return;
        }

        // Auto-deployed and nothing visible within siege range for a while: pack up and carry on
        if(!u.autoDeployed)
            return;
        if(closestVisibleEnemy(u.attackRange) != 0)
        {
            u.siegeQuietTimer = 0.0f;
            return;
        }
        u.siegeQuietTimer += deltaTime;
        if(u.siegeQuietTimer >= 4.0f)
        {
            u.siegeQuietTimer = 0.0f;
            u.autoDeployed = false;
            toggleSiegeMode({u.id});
        }
    }

    void EntityManager::toggleSiegeMode(const std::vector<int> &unitIds)
    {
        for(int uid : unitIds)
        {
            Unit *u = getUnit(uid);
            if(!u || u->state == ActionState::Die || u->state == ActionState::Transform)
                continue;
            if(!canDeploySiege(*u))
                continue;            // technology not researched yet
            u->autoDeployed = false; // a manual [G] keeps the unit where the player put it

            int newArch = -1;
            const UnitConfig *curCfg = getUnitConfig(u->archetypeId);
            if(curCfg && curCfg->isSiegeMode && curCfg->parentArchetypeId >= 0)
            {
                newArch = curCfg->parentArchetypeId;
            }
            else if(curCfg && curCfg->siegeArchetypeId >= 0)
            {
                newArch = curCfg->siegeArchetypeId;
            }
            else
            {
                switch(u->archetypeId)
                {
                    case 4:
                        newArch = 14;
                        break;
                    case 14:
                        newArch = 4;
                        break;
                    case 10:
                        newArch = 15;
                        break;
                    case 15:
                        newArch = 10;
                        break;
                    case 13:
                        newArch = 16;
                        break;
                    case 16:
                        newArch = 13;
                        break;
                    default:
                        break;
                }
            }
            if(newArch >= 0)
            {
                // Deployed by hand: the unit stays where the player put it
                if(!(curCfg && curCfg->isSiegeMode))
                {
                    u->clearMoveTarget();
                    u->isAttackMoving = false;
                }
                u->state = ActionState::Transform;
                u->pendingArchetypeId = newArch;
                u->path.clear();
                u->pathIndex = 0;
                u->animFrame = 0;
                u->animTimer = 0.0f;
                u->burstShotsRemaining = 0;
                u->burstTimer = 0.0f;
                u->targetEntityId = 0;
                u->targetWallTileX = -1;
                u->targetWallTileY = -1;
                u->waitingForPath = false;
            }
        }
    }

    void EntityManager::updateTurretShift(Unit &u) const
    {
        u.turretShiftX = u.turretShiftY = 0.0f;
        if(!m_texManager || !u.hasRotatingTurret)
            return;
        // Deployed siege units (14..16) keep a single hull sprite; everything else has one per facing
        const bool fixedHull = (u.archetypeId >= 14 && u.archetypeId <= 16);
        float ax = 0.0f, ay = 0.0f, rx = 0.0f, ry = 0.0f;
        if(m_texManager->getTurretAnchor(u.archetypeId, fixedHull ? 0 : u.facingDirection, ax, ay) && m_texManager->getTurretAnchor(u.archetypeId, fixedHull ? 0 : 2, rx, ry))
        {
            u.turretShiftX = ax - rx;
            u.turretShiftY = ay - ry;
        }
    }

    void EntityManager::noteHit(PlayerId victim, int attackerId)
    {
        const Entity *attacker = getEntity(attackerId);
        if(!attacker || victim >= kMaxPlayers || !Players::hostile(attacker->owner, victim))
            return;
        for(auto &p : m_revealPings)
        {
            if(p.entityId == attackerId && p.observer == victim)
            {
                p.timeLeft = 2.5f; // keeps shooting: stays visible
                return;
            }
        }
        m_revealPings.push_back({attackerId, victim, 2.5f});
    }

    void EntityManager::updateFogOfWar(TileMap &map)
    {
        if(!map.isFogEnabled())
            return;

        // Reset currently visible tiles to Explored (dimmed darkness shroud)
        map.resetVisibility();

        // Enemies that just hit something of a side: exactly the tiles they occupy become visible to it
        for(const auto &p : m_revealPings)
        {
            const Entity *e = getEntity(p.entityId);
            if(!e)
                continue;
            if(const Building *b = e->asBuilding())
            {
                map.revealRect(b->tileX, b->tileY, b->buildWidth, b->buildHeight, Players::teamOf(p.observer));
            }
            else
            {
                map.revealRect(e->tileX, e->tileY, 1, 1, Players::teamOf(p.observer));
            }
        }

        // Every team sees around the living units and buildings of its players (the same on every peer)
        for(const auto &entity : m_entities)
        {
            if(!entity.alive || entity.state == ActionState::Die)
                continue;
            if(entity.owner >= kMaxPlayers)
                continue;
            const uint8_t team = Players::teamOf(entity.owner);

            int sight = entity.sightRadius;
            if(sight <= 0)
                sight = 4;

            if(const Building *b = entity.asBuilding())
            {
                int cx = b->tileX + (b->buildWidth / 2);
                int cy = b->tileY + (b->buildHeight / 2);
                map.revealArea(cx, cy, sight, team);
            }
            else if(const Unit *u = entity.asUnit())
            {
                map.revealArea(u->tileX, u->tileY, sight, team);
            }
        }
    }

    bool EntityManager::isEntityVisibleToAi(const Entity &entity) const
    {
        return isEntityVisibleToAi(entity, Players::ownerOf(aiFaction()));
    }

    bool EntityManager::isEntityVisibleToAi(const Entity &entity, PlayerId observerPlayer) const
    {
        float ex = entity.getUnitCenterPixelX();
        float ey = entity.getUnitCenterPixelY();
        for(const auto &observer : m_entities)
        {
            if(!observer.alive || observer.state == ActionState::Die || observer.owner != observerPlayer)
                continue;
            float ox = observer.getUnitCenterPixelX();
            float oy = observer.getUnitCenterPixelY();
            float sightPx = static_cast<float>(observer.sightRadius * TILE_WIDTH);
            float dx = ex - ox;
            float dy = ey - oy;
            if((dx * dx + dy * dy) <= (sightPx * sightPx))
            {
                return true;
            }
        }
        return false;
    }

    bool EntityManager::isEntityVisibleToPlayer(const Entity &entity, PlayerId observer, const TileMap &map) const
    {
        if(!entity.alive || entity.state == ActionState::Die)
            return false;
        if(!map.isFogEnabled())
            return true;
        if(!Players::hostile(entity.owner, observer))
            return true;

        int w = 1;
        int h = 1;
        if(entity.isBuilding)
        {
            if(const Building *b = entity.asBuilding())
            {
                w = std::max(1, b->buildWidth);
                h = std::max(1, b->buildHeight);
            }
        }

        if(observer >= kMaxPlayers)
            return true;
        if(Players::slot(observer).human)
        {
            // Visible if ANY tile of the entity's WxH footprint is in the team's sight this tick
            const uint8_t team = Players::teamOf(observer);
            for(int dy = 0; dy < h; ++dy)
            {
                for(int dx = 0; dx < w; ++dx)
                {
                    if(map.isVisibleTo(entity.tileX + dx, entity.tileY + dy, team))
                        return true;
                }
            }
            return false;
        }
        return isEntityVisibleToAi(entity, observer);
    }

    void EntityManager::update(float deltaTime, TileMap &map, Pathfinder &pathfinder)
    {
        for(auto &p : m_revealPings)
            p.timeLeft -= deltaTime;
        m_revealPings.erase(std::remove_if(m_revealPings.begin(), m_revealPings.end(), [](const RevealPing &p) { return p.timeLeft <= 0.0f; }), m_revealPings.end());
        pathfinder.setEntityManager(this);

        // Initial fog update for this tick
        updateFogOfWar(map);

        // Power balance of every player
        std::array<int, kMaxPlayers> prod {}, cons {};
        for(const auto &e : m_entities)
        {
            const Building *b = e.asBuilding();
            if(!b || !b->alive || b->state == ActionState::Die || b->buildProgress < 1.0f || b->owner >= kMaxPlayers)
                continue;
            int pwr = b->getPowerOutput();
            if(pwr > 0)
                prod[b->owner] += pwr;
            else
                cons[b->owner] += (-pwr);
        }
        std::array<bool, kMaxPlayers> powered {};
        for(PlayerId p = 0; p < kMaxPlayers; ++p)
        {
            prod[p] += prod[p] * researchTotal(p, ResearchEffect::PowerBonus) / 100;
            powered[p] = m_unlimitedPower || cons[p] <= prod[p];
        }
        updateRegeneration(deltaTime);

        // Build Delegates for Units
        UnitDelegates uDelegates;
        uDelegates.getEntity = [this](int id) -> Entity * { return getEntity(id); };
        uDelegates.spawnProjectile = [this](Entity &shooter, float srcX, float srcY, float dstX, float dstY, int targetId, TileMap &m) { spawnProjectileFromEntity(shooter, srcX, srcY, dstX, dstY, targetId, m); };
        uDelegates.spawnExplosion = [this](float x, float y, int animId) { spawnExplosion(x, y, animId); };
        uDelegates.isTileOccupied = [this](int tx, int ty, int ignoreId) { return isTileOccupiedByUnit(tx, ty, ignoreId); };
        uDelegates.getUnitAtTile = [this](int tx, int ty, int ignoreId) { return getUnitIdAtTile(tx, ty, ignoreId); };
        uDelegates.onKill = [this](PlayerId victim, PlayerId killer) { noteKill(victim, killer); };
        uDelegates.isFortressFlamethrowerUnlocked = [this](PlayerId p) { return isFortressFlamethrowerUnlocked(p); };
        uDelegates.getUnitConfig = [this](int archId) { return getUnitConfig(archId); };
        uDelegates.applyResearch = [](Unit &u) { applyCompletedResearch(u); };
        uDelegates.isEntityVisible = [this, &map](const Entity &entity, PlayerId observer) { return isEntityVisibleToPlayer(entity, observer, map); };
        uDelegates.findEnemyInAttackRange = [this, &map](const Unit &u) -> int
        {
            float atkRangePixels = u.attackRange * TILE_WIDTH;
            for(const auto &other : m_entities)
            {
                if(!other.alive || other.state == ActionState::Die || !Players::hostile(other.owner, u.owner))
                    continue;
                if(!isEntityVisibleToPlayer(other, u.owner, map))
                    continue;
                if(computeDistanceToEntityPixels(u, other) <= atkRangePixels)
                {
                    return other.id;
                }
            }
            return 0;
        };
        uDelegates.findTarget = [this, &map](const Unit &u) -> int
        {
            float scanDist = ((u.isAttackMoving || u.waitingForPath) && !u.isSiegeMode && u.moveSpeed > 0.0f ? std::max(static_cast<float>(u.sightRadius), u.attackRange) : u.attackRange) * TILE_WIDTH;
            float closestDist = scanDist;
            int bestId = 0;
            for(const auto &other : m_entities)
            {
                if(!other.alive || other.state == ActionState::Die || !Players::hostile(other.owner, u.owner))
                    continue;
                if(!isEntityVisibleToPlayer(other, u.owner, map))
                    continue;
                float d = computeDistanceToEntityPixels(u, other);
                if(d <= closestDist)
                {
                    closestDist = d;
                    bestId = other.id;
                }
            }
            return bestId;
        };
        uDelegates.findWallTarget = [this, &map](const Unit &u, int &outWallX, int &outWallY)
        {
            outWallX = -1;
            outWallY = -1;
            // Something to shoot right now comes first; enemies seen behind the wall do not stop the breach
            // (a base walled in all around would never be attacked otherwise)
            bool enemyInRange = false;
            const float rangePixels = u.attackRange * TILE_WIDTH;
            for(const auto &other : m_entities)
            {
                if(!other.alive || other.state == ActionState::Die || !Players::hostile(other.owner, u.owner))
                    continue;
                if(other.faction == Faction::Neutral)
                    continue;
                if(computeDistanceToEntityPixels(u, other) <= rangePixels && isEntityVisibleToPlayer(other, u.owner, map))
                {
                    enemyInRange = true;
                    break;
                }
            }
            // Walls are picked toward where the unit is heading: its target, else its order's destination
            int goalX = u.hasMoveTarget() ? u.targetTileX : -1;
            int goalY = u.hasMoveTarget() ? u.targetTileY : -1;
            if(const Entity *t = (u.targetEntityId != 0) ? getEntity(u.targetEntityId) : nullptr)
            {
                goalX = static_cast<int>(t->getUnitCenterPixelX() / TILE_WIDTH);
                goalY = static_cast<int>(t->getUnitCenterPixelY() / TILE_HEIGHT);
            }
            if(!enemyInRange)
            {
                int scanR = 5;
                int minWX = std::max(0, u.tileX - scanR);
                int maxWX = std::min(map.getWidth() - 1, u.tileX + scanR);
                int minWY = std::max(0, u.tileY - scanR);
                int maxWY = std::min(map.getHeight() - 1, u.tileY + scanR);
                float bestWDist = 1e9f;
                for(int wy = minWY; wy <= maxWY; ++wy)
                {
                    for(int wx = minWX; wx <= maxWX; ++wx)
                    {
                        uint16_t w = map.getWall(wx, wy);
                        if(w == 0)
                            continue;
                        Faction wallFac = (w >= 1101) ? Faction::Resistance : Faction::Confederation;
                        // Only walls toward the destination; never turn back to shoot a wall behind
                        if(goalX >= 0 && DetMath::hypot(wx - goalX, wy - goalY) >= DetMath::hypot(u.tileX - goalX, u.tileY - goalY) + 1.0f)
                        {
                            continue;
                        }
                        if(wallFac != u.faction && wallFac != Faction::Neutral)
                        {
                            float wd = DetMath::hypot((wx - u.tileX) * TILE_WIDTH, (wy - u.tileY) * TILE_HEIGHT);
                            if(wd < bestWDist)
                            {
                                bestWDist = wd;
                                outWallX = wx;
                                outWallY = wy;
                            }
                        }
                    }
                }
            }
        };
        uDelegates.findSupportAlly = [this, &map](const Unit &u, int &outTargetEnemyId) -> int
        {
            outTargetEnemyId = 0;
            float maxDist = u.attackRange * TILE_WIDTH;
            float bestDist = maxDist;
            int bestAllyId = 0;
            for(const auto &other : m_entities)
            {
                if(!other.alive || other.state == ActionState::Die || Players::hostile(other.owner, u.owner))
                    continue;
                const Unit *allyUnit = other.asUnit();
                if(!allyUnit || allyUnit->id == u.id)
                    continue;
                // STRICT DEPTH = 1 INVARIANT:
                // Ally must be in direct combat (within its firing range) AND must NOT be a secondary support unit itself!
                if(!allyUnit->isDirectEngaged || allyUnit->isSupportAssisting || allyUnit->targetEntityId == 0)
                    continue;

                const Entity *enemy = getEntity(allyUnit->targetEntityId);
                if(!enemy || !enemy->alive || enemy->state == ActionState::Die)
                    continue;
                if(!isEntityVisibleToPlayer(*enemy, u.owner, map))
                    continue;

                float dx = other.getUnitCenterPixelX() - u.getUnitCenterPixelX();
                float dy = other.getUnitCenterPixelY() - u.getUnitCenterPixelY();
                float d = DetMath::hypot(dx, dy);
                if(d <= bestDist)
                {
                    bestDist = d;
                    bestAllyId = other.id;
                    outTargetEnemyId = allyUnit->targetEntityId;
                }
            }
            return bestAllyId;
        };

        // Build Delegates for Buildings
        BuildingDelegates bDelegates;
        bDelegates.getEntity = [this](int id) -> Entity * { return getEntity(id); };
        bDelegates.spawnProjectile = [this](Entity &shooter, float srcX, float srcY, float dstX, float dstY, int targetId, TileMap &m) { spawnProjectileFromEntity(shooter, srcX, srcY, dstX, dstY, targetId, m); };
        bDelegates.spawnExplosion = [this](float x, float y, int animId) { spawnExplosion(x, y, animId); };
        bDelegates.onKill = [this](PlayerId victim, PlayerId killer) { noteKill(victim, killer); };
        bDelegates.isEntityVisible = [this, &map](const Entity &entity, PlayerId observer) { return isEntityVisibleToPlayer(entity, observer, map); };
        bDelegates.findTarget = [this, &map](const Building &b) -> int
        {
            float closestDist = b.attackRange * TILE_WIDTH;
            int bestId = 0;
            float bx = b.getUnitCenterPixelX();
            float by = b.getUnitCenterPixelY();
            for(const auto &other : m_entities)
            {
                if(!other.alive || other.state == ActionState::Die || !Players::hostile(other.owner, b.owner))
                    continue;
                if(!isEntityVisibleToPlayer(other, b.owner, map))
                    continue;
                float dx = other.getUnitCenterPixelX() - bx;
                float dy = other.getUnitCenterPixelY() - by;
                float d = DetMath::hypot(dx, dy);
                if(d < closestDist)
                {
                    closestDist = d;
                    bestId = other.id;
                }
            }
            return bestId;
        };

        // 1. Update entities through polymorphic delegates
        for(auto &entity : m_entities)
        {
            if(!entity.alive)
                continue;

            if(Unit *unit = entity.asUnit())
            {
                updateTurretShift(*unit);
                updateAutoSiege(*unit, map, deltaTime);
                unit->update(deltaTime, map, pathfinder, uDelegates);
            }
            else if(Building *bldg = entity.asBuilding())
            {
                bldg->isPowered = bldg->owner >= kMaxPlayers || powered[bldg->owner];
                bldg->update(deltaTime, map, bDelegates);
            }
        }

        // Resolve any stacked units sharing the exact same tile
        resolveUnitStacking(map);

        // 2. Update projectiles
        for(auto &proj : m_projectiles)
        {
            if(!proj.active)
                continue;

            float dx = proj.targetX - proj.x;
            float dy = proj.targetY - proj.y;
            float dist = std::sqrt(dx * dx + dy * dy);

            if(proj.type == ProjectileType::Rocket)
            {
                proj.smokeTimer += deltaTime;
                if(proj.smokeTimer >= 0.030f)
                {
                    proj.smokeTimer = 0.0f;
                    SmokePuff puff;
                    puff.x = proj.x;
                    puff.y = proj.y;
                    puff.lifetime = 0.0f;
                    puff.maxLifetime = 0.35f;
                    puff.active = true;
                    m_smokePuffs.push_back(puff);
                }
            }

            if(dist <= proj.speed)
            {
                if(proj.damage <= 0)
                {
                    proj.active = false;
                    continue;
                }

                Entity *target = (proj.targetId > 0) ? getEntity(proj.targetId) : nullptr;
                bool targetAlive = (target && target->alive && target->state != ActionState::Die);
                bool isTargetInfantry = (target && target->asUnit() && target->asUnit()->isInfantry());

                int impactAnim = 42;
                if(proj.type == ProjectileType::Bullet)
                {
                    if(targetAlive)
                    {
                        impactAnim = bulletImpactAnim(proj.targetId);
                    }
                    else
                    {
                        impactAnim = 42;
                    }
                }
                else if(proj.type == ProjectileType::Rocket)
                {
                    impactAnim = 18 + (proj.targetId % 3);
                }
                else if(proj.type == ProjectileType::Shell)
                {
                    impactAnim = 15 + (proj.targetId % 3);
                }
                else if(proj.type == ProjectileType::Flame)
                {
                    impactAnim = -1;
                }

                if(targetAlive)
                {
                    // Units blow up at their center, buildings where the projectile came down
                    const bool hitUnit = target->asUnit() != nullptr;
                    float hitCenterX = hitUnit ? target->getUnitCenterPixelX() : proj.targetX;
                    float hitCenterY = hitUnit ? target->getUnitCenterPixelY() : proj.targetY;
                    noteHit(target->owner, proj.shooterId);
                    target->currentHp -= applyMovingUnitDamage(proj.damage, target);
                    if(target->currentHp <= 0)
                    {
                        target->currentHp = 0;
                        target->state = ActionState::Die;
                        target->animFrame = 0;
                        target->animTimer = 0.0f;
                        target->selected = false;
                        if(Unit *tu = target->asUnit())
                        {
                            handleUnitDeathState(tu, proj.type == ProjectileType::Flame);
                        }

                        if(Building *tb = target->asBuilding())
                        {
                            tb->deathTimer = EffectsConfig::get().burialSeconds;
                            for(int ty = tb->tileY; ty < tb->tileY + tb->buildHeight; ++ty)
                            {
                                for(int tx = tb->tileX; tx < tb->tileX + tb->buildWidth; ++tx)
                                {
                                    map.setPassable(tx, ty, true);
                                }
                            }
                            spawnDeathExplosions(*tb);
                        }
                        else if(!isTargetInfantry)
                        {
                            target->deathTimer = EffectsConfig::get().burialSeconds;
                            spawnDeathExplosions(*target);
                        }
                        else
                        {
                            target->deathTimer = EffectsConfig::get().burialSeconds;
                            spawnExplosion(hitCenterX, hitCenterY, impactAnim);
                        }

                        noteKill(target->owner, proj.owner);
                    }
                    else
                    {
                        spawnExplosion(hitCenterX, hitCenterY, impactAnim);
                    }
                }
                else
                {
                    int hitTileX = static_cast<int>(proj.targetX / TILE_WIDTH);
                    int hitTileY = static_cast<int>(proj.targetY / TILE_HEIGHT);
                    if(map.getWall(hitTileX, hitTileY) > 0 && Players::hostile(map.getWallOwner(hitTileX, hitTileY), proj.owner))
                    {
                        noteHit(map.getWallOwner(hitTileX, hitTileY), proj.shooterId);
                        map.damageWall(hitTileX, hitTileY, proj.damage);
                        if(map.getWall(hitTileX, hitTileY) == 0)
                        {
                            spawnExplosion(hitTileX * TILE_WIDTH + TILE_WIDTH * 0.5f, hitTileY * TILE_HEIGHT + TILE_HEIGHT * 0.5f, 42);
                        }
                        else
                        {
                            spawnExplosion(proj.targetX, proj.targetY, 55);
                        }
                    }
                    else
                    {
                        spawnExplosion(proj.targetX, proj.targetY, impactAnim);
                    }
                }

                // Splash / AoE Damage (2x2 tiles around impact) for Shell, Rocket, Flame
                if(proj.splashPercent > 0 && (proj.type == ProjectileType::Shell || proj.type == ProjectileType::Rocket || proj.type == ProjectileType::Flame))
                {
                    float impactX = targetAlive ? target->getUnitCenterPixelX() : proj.targetX;
                    float impactY = targetAlive ? target->getUnitCenterPixelY() : proj.targetY;
                    int primaryHitId = targetAlive ? target->id : 0;
                    int splashDmg = std::max(1, static_cast<int>(std::round(proj.damage * (proj.splashPercent / 100.0f))));
                    float maxSplashDist = static_cast<float>(TILE_WIDTH) * 1.5f; // 2x2 tile area (~45px radius)

                    for(auto &other : m_entities)
                    {
                        if(!other.alive || other.state == ActionState::Die)
                            continue;
                        if(other.id == primaryHitId)
                            continue;
                        if(!Players::hostile(other.owner, proj.owner))
                            continue;

                        float ox = other.getUnitCenterPixelX();
                        float oy = other.getUnitCenterPixelY();
                        float distFromImpact = DetMath::hypot(ox - impactX, oy - impactY);
                        if(distFromImpact <= maxSplashDist)
                        {
                            noteHit(other.owner, proj.shooterId);
                            other.currentHp -= applyMovingUnitDamage(splashDmg, &other);
                            if(other.currentHp <= 0)
                            {
                                other.currentHp = 0;
                                other.state = ActionState::Die;
                                other.animFrame = 0;
                                other.animTimer = 0.0f;
                                other.selected = false;
                                if(Unit *tu = other.asUnit())
                                {
                                    handleUnitDeathState(tu, proj.type == ProjectileType::Flame);
                                }

                                if(Building *tb = other.asBuilding())
                                {
                                    tb->deathTimer = EffectsConfig::get().burialSeconds;
                                    for(int ty = tb->tileY; ty < tb->tileY + tb->buildHeight; ++ty)
                                    {
                                        for(int tx = tb->tileX; tx < tb->tileX + tb->buildWidth; ++tx)
                                        {
                                            map.setPassable(tx, ty, true);
                                        }
                                    }
                                    spawnDeathExplosions(*tb);
                                }
                                else if(other.asUnit() && other.asUnit()->isInfantry())
                                {
                                    other.deathTimer = EffectsConfig::get().burialSeconds;
                                }
                                else
                                {
                                    other.deathTimer = EffectsConfig::get().burialSeconds;
                                    spawnDeathExplosions(other);
                                }
                            }
                        }
                    }

                    int cTileX = static_cast<int>(impactX / TILE_WIDTH);
                    int cTileY = static_cast<int>(impactY / TILE_HEIGHT);
                    for(int wy = cTileY - 1; wy <= cTileY + 1; ++wy)
                    {
                        for(int wx = cTileX - 1; wx <= cTileX + 1; ++wx)
                        {
                            if(wx < 0 || wy < 0 || wx >= map.getWidth() || wy >= map.getHeight())
                                continue;
                            if(map.getWall(wx, wy) > 0 && Players::hostile(map.getWallOwner(wx, wy), proj.owner))
                            {
                                noteHit(map.getWallOwner(wx, wy), proj.shooterId);
                                map.damageWall(wx, wy, splashDmg);
                                if(map.getWall(wx, wy) == 0)
                                {
                                    spawnExplosion(wx * TILE_WIDTH + TILE_WIDTH * 0.5f, wy * TILE_HEIGHT + TILE_HEIGHT * 0.5f, 42);
                                }
                            }
                        }
                    }
                }
                proj.active = false;
            }
            else
            {
                proj.x += (dx / dist) * proj.speed;
                proj.y += (dy / dist) * proj.speed;
            }
        }

        // 3. Update smoke puffs
        for(auto &puff : m_smokePuffs)
        {
            if(!puff.active)
                continue;
            puff.lifetime += deltaTime;
            if(puff.lifetime >= puff.maxLifetime)
            {
                puff.active = false;
            }
        }
        m_smokePuffs.erase(std::remove_if(m_smokePuffs.begin(), m_smokePuffs.end(), [](const SmokePuff &p) { return !p.active; }), m_smokePuffs.end());

        // 4. Update explosions
        for(auto &exp : m_explosions)
        {
            if(!exp.active)
                continue;
            if(exp.delay > 0.0f)
            {
                exp.delay -= deltaTime;
                continue;
            }
            exp.frameTimer += deltaTime;
            if(exp.frameTimer >= EffectsConfig::get().frameTime)
            {
                exp.frameTimer = 0.0f;
                exp.currentFrame++;
                if(exp.currentFrame >= exp.totalFrames)
                {
                    exp.active = false;
                }
            }
        }
        m_explosions.erase(std::remove_if(m_explosions.begin(), m_explosions.end(), [](const ExplosionParticle &e) { return !e.active; }), m_explosions.end());

        // 5. Clean up dead entities
        m_entities.raw().erase(std::remove_if(m_entities.raw().begin(), m_entities.raw().end(), [](const std::unique_ptr<Entity> &e) { return !e->alive; }), m_entities.raw().end());

        // 6. Update Fog of War post-movement
        updateFogOfWar(map);
    }

    void EntityManager::spawnProjectileFromEntity(Entity &entity, float srcX, float srcY, float dstX, float dstY, int targetEntityId, TileMap &map)
    {
        // Shots at a building land on the side facing the shooter, not in its middle (units are hit at their center)
        if(targetEntityId > 0)
        {
            if(const Building *tb = getBuilding(targetEntityId))
            {
                const float inset = 3.0f;
                const float minX = tb->tileX * TILE_WIDTH + inset;
                const float minY = tb->tileY * TILE_HEIGHT + inset;
                const float maxX = (tb->tileX + tb->buildWidth) * TILE_WIDTH - inset;
                const float maxY = (tb->tileY + tb->buildHeight) * TILE_HEIGHT - inset;
                dstX = std::clamp(srcX, minX, maxX) + Utility::randomFloat(-8.0f, 8.0f);
                dstY = std::clamp(srcY, minY, maxY) + Utility::randomFloat(-6.0f, 6.0f);
                dstX = std::clamp(dstX, minX, maxX);
                dstY = std::clamp(dstY, minY, maxY);
            }
        }

        float dx = dstX - srcX;
        float dy = dstY - srcY;
        float dist = std::max(1.0f, DetMath::hypot(dx, dy));

        // Randomized shot dispersion
        const float normRng = Utility::randomSigned(); // [-1 .. +1]

        // Authentic dispersion defaults per projectile type (or override from entity.spreadAngle)
        float maxSpread = 0.0f;
        if(entity.spreadAngle >= 0.0f)
        {
            // Explicit configuration: if spread == 0.0f, maxSpread is 0.0f (high precision pinpoint accuracy)
            maxSpread = entity.spreadAngle;
        }
        else
        {
            // Unconfigured default (-1.0f): authentic weapon spread per type
            switch(entity.projectile)
            {
                case ProjectileType::Bullet:
                    maxSpread = 0.065f;
                    break; // ~3.7 deg
                case ProjectileType::Shell:
                    maxSpread = 0.035f;
                    break; // ~2.0 deg
                case ProjectileType::Rocket:
                    maxSpread = 0.080f;
                    break; // ~4.6 deg
                case ProjectileType::Flame:
                    maxSpread = 0.120f;
                    break; // ~6.9 deg
                default:
                    maxSpread = 0.055f;
                    break;
            }
        }

        float spreadDstX = dstX;
        float spreadDstY = dstY;
        float spreadDx = dx;
        float spreadDy = dy;

        if(maxSpread > 0.0001f)
        {
            float spreadAngle = normRng * maxSpread;
            float baseAng = DetMath::atan2(dy, dx);
            float spreadAng = baseAng + spreadAngle;

            spreadDstX = srcX + DetMath::cos(spreadAng) * dist;
            spreadDstY = srcY + DetMath::sin(spreadAng) * dist;
            spreadDx = spreadDstX - srcX;
            spreadDy = spreadDstY - srcY;
        }

        const int dir16 = Utility::dirFromNorth(spreadDx, spreadDy, 16);
        const int dir32 = Utility::dirFromNorth(spreadDx, spreadDy, 32);

        if(entity.projectile == ProjectileType::Bullet)
        {
            float finalHitX = spreadDstX;
            float finalHitY = spreadDstY;

            if(targetEntityId > 0)
            {
                Entity *target = getEntity(targetEntityId);
                if(target && target->alive && target->state != ActionState::Die)
                {
                    bool isTargetInfantry = (target->asUnit() && target->asUnit()->isInfantry());
                    int impactAnim = bulletImpactAnim(target->id);

                    noteHit(target->owner, entity.id);
                    target->currentHp -= applyMovingUnitDamage(entity.attackDamage, target);
                    if(target->currentHp <= 0)
                    {
                        target->currentHp = 0;
                        target->state = ActionState::Die;
                        target->animFrame = 0;
                        target->animTimer = 0.0f;
                        target->selected = false;
                        if(Unit *tu = target->asUnit())
                        {
                            handleUnitDeathState(tu, entity.projectile == ProjectileType::Flame);
                        }

                        if(Building *tb = target->asBuilding())
                        {
                            tb->deathTimer = EffectsConfig::get().burialSeconds;
                            for(int ty = tb->tileY; ty < tb->tileY + tb->buildHeight; ++ty)
                            {
                                for(int tx = tb->tileX; tx < tb->tileX + tb->buildWidth; ++tx)
                                {
                                    map.setPassable(tx, ty, true);
                                }
                            }
                            spawnDeathExplosions(*tb);
                        }
                        else if(!isTargetInfantry)
                        {
                            target->deathTimer = EffectsConfig::get().burialSeconds;
                            spawnDeathExplosions(*target);
                        }
                        else
                        {
                            target->deathTimer = EffectsConfig::get().burialSeconds;
                            spawnExplosion(finalHitX, finalHitY, impactAnim);
                        }

                        noteKill(target->owner, entity.owner);
                    }
                    else
                    {
                        spawnExplosion(finalHitX, finalHitY, impactAnim);
                    }
                }
            }
            else if(entity.targetWallTileX >= 0 && entity.targetWallTileY >= 0)
            {
                int wx = entity.targetWallTileX;
                int wy = entity.targetWallTileY;
                if(map.getWall(wx, wy) > 0)
                {
                    noteHit(map.getWallOwner(wx, wy), entity.id);
                    map.damageWall(wx, wy, entity.attackDamage);
                    finalHitX = wx * TILE_WIDTH + (TILE_WIDTH * 0.5f);
                    finalHitY = wy * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
                    if(map.getWall(wx, wy) == 0)
                    {
                        spawnExplosion(finalHitX, finalHitY, 42);
                    }
                    else
                    {
                        spawnExplosion(finalHitX, finalHitY, 55);
                    }
                }
            }

            Projectile p;
            p.x = srcX;
            p.y = srcY;
            p.startX = srcX;
            p.startY = srcY;
            p.targetX = spreadDstX;
            p.targetY = spreadDstY;
            p.damage = 0;
            p.faction = entity.faction;
            p.owner = entity.owner;
            p.shooterId = entity.id;
            p.targetId = 0;
            p.active = true;
            p.smokeTimer = 0.0f;
            p.direction = dir16;
            p.dir32 = dir32;
            p.type = ProjectileType::Bullet;
            p.speed = 36.0f;
            m_projectiles.push_back(p);
            return;
        }

        // Physical projectile (Rocket, Shell, Flame)
        float projTargetX = spreadDstX;
        float projTargetY = spreadDstY;
        int projTargetId = targetEntityId;

        Projectile p;
        p.x = srcX;
        p.y = srcY;
        p.startX = p.x;
        p.startY = p.y;
        p.targetX = projTargetX;
        p.targetY = projTargetY;
        p.damage = entity.attackDamage;
        p.splashPercent = entity.splashPercent;
        p.faction = entity.faction;
        p.owner = entity.owner;
        p.shooterId = entity.id;
        p.targetId = projTargetId;
        p.active = true;
        p.smokeTimer = 0.0f;
        p.direction = dir16;
        p.dir32 = dir32;
        p.type = entity.projectile;
        switch(p.type)
        {
            case ProjectileType::Rocket:
                p.speed = 11.0f;
                break;
            case ProjectileType::Shell:
                p.speed = 13.5f;
                break;
            case ProjectileType::Flame:
                p.speed = 22.0f;
                break;
            default:
                p.speed = 16.0f;
                break;
        }
        m_projectiles.push_back(p);
    }

    int EntityManager::demolishBuilding(int buildingId, TileMap &map)
    {
        Building *b = getBuilding(buildingId);
        if(!b || !b->alive || b->state == ActionState::Die)
            return 0;
        int refund = 0;
        for(const auto &item : b->productionQueue)
            refund += item.cost;
        b->productionQueue.clear();
        b->isProducing = false;
        b->currentHp = 0;
        b->state = ActionState::Die;
        b->animFrame = 0;
        b->animTimer = 0.0f;
        b->selected = false;
        b->deathTimer = EffectsConfig::get().burialSeconds;
        for(int ty = b->tileY; ty < b->tileY + b->buildHeight; ++ty)
        {
            for(int tx = b->tileX; tx < b->tileX + b->buildWidth; ++tx)
            {
                map.setPassable(tx, ty, true);
            }
        }
        spawnDeathExplosions(*b);
        return refund;
    }

    void EntityManager::spawnDeathExplosions(const Entity &e)
    {
        const EffectsConfig &fx = EffectsConfig::get();
        const float cx = e.getUnitCenterPixelX();
        const float cy = e.getUnitCenterPixelY();
        auto pick = [](const std::vector<int> &list, int i) { return list.empty() ? -1 : list[static_cast<size_t>(i) % list.size()]; };
        if(const Building *b = e.asBuilding())
        {
            int main = fx.buildingLargeMain;
            if(b->buildWidth <= 2 && b->buildHeight <= 2)
            {
                main = fx.buildingSmallMain;
            }
            else if(b->rawBuildingType == 2 || b->rawBuildingType == 4 || b->rawBuildingType == 12)
            {
                main = fx.buildingPowerMain;
            }
            spawnExplosion(cx, cy, main);
            // The rest of the structure goes up over the next half second, spread over its whole footprint
            const int extra = std::clamp(b->buildWidth * b->buildHeight / 2, fx.buildingExtraMin, fx.buildingExtraMax);
            for(int i = 0; i < extra; ++i)
            {
                const float x = (b->tileX + Utility::randomFloat(0.2f, b->buildWidth - 0.2f)) * TILE_WIDTH;
                const float y = (b->tileY + Utility::randomFloat(0.2f, b->buildHeight - 0.2f)) * TILE_HEIGHT;
                const int anim = (i % fx.buildingBigEvery == 0) ? fx.buildingBigAnim : pick(fx.buildingExtraAnims, Utility::randomInt(0, 3));
                spawnExplosion(x, y, anim, fx.buildingDelayStart + fx.buildingDelayStep * static_cast<float>(i) + Utility::randomFloat(0.0f, fx.buildingDelayJitter));
            }
            return;
        }
        // Vehicle: the blast, then the ammunition / fuel a moment later
        spawnExplosion(cx, cy, pick(fx.vehicleMain, e.id));
        spawnExplosion(cx + Utility::randomFloat(-fx.vehicleSecondSpreadX, fx.vehicleSecondSpreadX), cy + Utility::randomFloat(fx.vehicleSecondSpreadYMin, fx.vehicleSecondSpreadYMax), pick(fx.vehicleSecond, e.id + 2), Utility::randomFloat(fx.vehicleSecondDelayMin, fx.vehicleSecondDelayMax));
    }

    void EntityManager::spawnExplosion(float worldX, float worldY, int animId, float delay)
    {
        if(animId < 0)
            return;
        ExplosionParticle exp;
        exp.x = worldX;
        exp.y = worldY;
        exp.animId = animId;
        exp.delay = delay;
        exp.currentFrame = 0;
        exp.frameTimer = 0.0f;
        exp.active = true;

        if(m_texManager)
        {
            IndexRange r = m_texManager->getAnimRange(animId);
            if(r.end >= r.start)
            {
                exp.totalFrames = (r.end - r.start) + 1;
            }
            else
            {
                exp.totalFrames = 6;
            }
        }
        else
        {
            exp.totalFrames = 6;
        }

        m_explosions.push_back(exp);
    }

    void EntityManager::getUnitsInBox(float minWorldX, float minWorldY, float maxWorldX, float maxWorldY, PlayerId owner, std::vector<int> &outUnitIds)
    {
        outUnitIds.clear();
        for(auto &e : m_entities)
        {
            if(!e.alive || e.state == ActionState::Die || e.isBuilding || e.owner != owner)
                continue;
            float px = e.getUnitCenterPixelX();
            float py = e.getUnitCenterPixelY();
            if(px >= minWorldX && px <= maxWorldX && py >= minWorldY && py <= maxWorldY)
            {
                outUnitIds.push_back(e.id);
                e.selected = true;
            }
            else
            {
                e.selected = false;
            }
        }
    }

    int EntityManager::getUnitAt(float worldX, float worldY, float tolerance) const
    {
        float bestDist = tolerance;
        int bestId = 0;
        for(const auto &e : m_entities)
        {
            if(!e.alive || e.state == ActionState::Die)
                continue;
            if(const Building *b = e.asBuilding())
            {
                float minX = b->tileX * TILE_WIDTH;
                float maxX = (b->tileX + b->buildWidth) * TILE_WIDTH;
                float minY = b->tileY * TILE_HEIGHT;
                float maxY = (b->tileY + b->buildHeight) * TILE_HEIGHT;
                if(worldX >= minX && worldX <= maxX && worldY >= minY && worldY <= maxY)
                {
                    return b->id;
                }
                continue;
            }
            float dx = e.getUnitCenterPixelX() - worldX;
            float dy = e.getUnitCenterPixelY() - worldY;
            float d = std::sqrt(dx * dx + dy * dy);
            if(d < bestDist)
            {
                bestDist = d;
                bestId = e.id;
            }
        }
        return bestId;
    }

    void EntityManager::renderGroundRemnants(Renderer &renderer, const Camera &camera, const TileMap &map, bool fogEnabled) const
    {
        for(const auto &entity : m_entities)
        {
            if(!entity.alive || entity.state != ActionState::Die)
                continue;
            if(fogEnabled && Players::hostile(entity.owner, Players::local()))
            {
                if(map.getFogState(entity.tileX, entity.tileY) == FogState::Hidden)
                    continue;
            }
            // Wrecks and rubble replace the destroyed object from the middle of its explosion (elapsed >= 0.25s);
            // until then the building / vehicle is still standing there in its battered state
            float elapsedDeath = EffectsConfig::get().burialSeconds - entity.deathTimer;
            const bool infantry = entity.asUnit() && entity.asUnit()->isInfantry();
            if(elapsedDeath < 0.25f && !infantry)
            {
                if(const Building *b = entity.asBuilding())
                {
                    const int btype = (b->rawBuildingType >= 0) ? b->rawBuildingType : (static_cast<int>(b->type) % 15);
                    for(int layer = 0; layer < 2; ++layer)
                    {
                        renderer.setDrawOwner(b->owner);
                        renderer.drawBuildingWorld(btype, b->tileX * TILE_WIDTH, b->tileY * TILE_HEIGHT, layer, camera, b->faction, 0, 1.0f, false, 0.0f, b->turretFacing, false, false);
                    }
                }
                else if(const Unit *u = entity.asUnit())
                {
                    renderer.setDrawOwner(u->owner);
                    renderer.drawUnitWorld(u->archetypeId, static_cast<int>(ActionState::Idle), u->facingDirection, 0, u->getWorldPixelX(), u->getWorldPixelY(), camera, u->faction, u->turretFacing, false);
                }
                continue;
            }

            if(const Building *b = entity.asBuilding())
            {
                int btype = (b->rawBuildingType >= 0) ? b->rawBuildingType : (static_cast<int>(b->type) % 15);
                float bx = b->tileX * TILE_WIDTH;
                float by = b->tileY * TILE_HEIGHT;
                // The rubble sinks into the ground stage by stage over the 120 s the remains stay
                int wreckComp = 0, stages = 0;
                Renderer::defaultBuildingWreck(b->buildWidth, b->buildHeight, wreckComp, stages);
                if(b->wreckStages > 0)
                    stages = b->wreckStages;
                const int stage = std::min(stages - 1, static_cast<int>(std::clamp(elapsedDeath, 0.0f, 120.0f) / 120.0f * stages));
                renderer.setDrawOwner(b->owner);
                renderer.drawBuildingWorld(btype, bx, by, 0, camera, b->faction, stage, 1.0f, true, 1.0f, 0, false, true, b->wreckComposite, b->wreckStages);
            }
            else if(const Unit *u = entity.asUnit())
            {
                renderer.setDrawOwner(u->owner);
                renderer.drawUnitWorld(u->archetypeId, static_cast<int>(ActionState::Die), u->facingDirection, u->animFrame, u->getWorldPixelX(), u->getWorldPixelY(), camera, u->faction, u->turretFacing, u->killedByFlame, u->wreckComposite, u->wreckStages, u->selectedDeathSeq);
            }
        }
    }

    bool EntityManager::isHostileBuildingShown(const Building &b, const TileMap &map, bool *outInSight) const
    {
        bool inSight = false;
        for(int ty = b.tileY; ty < b.tileY + b.buildHeight && !inSight; ++ty)
        {
            for(int tx = b.tileX; tx < b.tileX + b.buildWidth && !inSight; ++tx)
            {
                inSight = map.getFogState(tx, ty) == FogState::Visible;
            }
        }
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(b.id)) << 32) | (static_cast<uint64_t>(static_cast<uint16_t>(b.tileY)) << 16) | static_cast<uint16_t>(b.tileX);
        if(inSight)
            m_seenBuildings.insert(key);
        if(outInSight)
            *outInSight = inSight;
        return inSight || (b.buildProgress >= 1.0f && m_seenBuildings.count(key) > 0);
    }

    void EntityManager::render(Renderer &renderer, const Camera &camera, const TileMap &map, bool fogEnabled, int globalAnimTick, bool debugMode, bool showAllHealth)
    {
        float startWorldX, startWorldY, endWorldX, endWorldY;
        camera.screenToWorld(0, 0, startWorldX, startWorldY);
        camera.screenToWorld(camera.getViewportWidth(), camera.getViewportHeight(), endWorldX, endWorldY);
        int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, map.getWidth() - 1);
        int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, map.getWidth());
        int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, map.getHeight() - 1);
        int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, map.getHeight());

        // Depth order: everything is drawn by the bottom edge of what stands on the ground (a unit's tile, a
        // building's last footprint row, a wall row), so whatever reaches above its own tiles - the top of a
        // headquarters, a tall wall segment - covers units behind it, and units in front are drawn over it.
        // Health / progress bars come after all of it.
        struct DepthItem
        {
            float footY;
            int order; // equal foot: walls, then buildings, then units
            int row;   // wall row
            const Unit *unit;
            const Building *building;
            bool live; // building currently in sight (else drawn still, under the fog shading)
        };
        std::vector<DepthItem> items;
        items.reserve(m_entities.size() + static_cast<size_t>(std::max(0, endTileY - startTileY)));
        for(int ty = startTileY; ty < endTileY; ++ty)
        {
            items.push_back({static_cast<float>((ty + 1) * TILE_HEIGHT), 0, ty, nullptr, nullptr, true});
        }
        const float viewTop = startTileY * static_cast<float>(TILE_HEIGHT);
        const float viewBottom = (endTileY + 1) * static_cast<float>(TILE_HEIGHT);
        for(const auto &entity : m_entities)
        {
            if(!entity.alive || entity.state == ActionState::Die)
                continue;
            if(const Unit *u = entity.asUnit())
            {
                if(fogEnabled && Players::hostile(u->owner, Players::local()) && map.getFogState(u->tileX, u->tileY) != FogState::Visible)
                {
                    continue;
                }
                const float foot = u->getWorldPixelY() + TILE_HEIGHT;
                if(foot < viewTop || foot > viewBottom + TILE_HEIGHT)
                    continue;
                items.push_back({foot, 2, 0, u, nullptr, true});
            }
            else if(const Building *b = entity.asBuilding())
            {
                // Enemy structures: in sight -> live; seen before and finished -> drawn still, the fog shading goes on top;
                // never seen, or still under construction out of sight -> hidden (no peeking through the fog)
                bool live = true;
                if(fogEnabled && Players::hostile(b->owner, Players::local()))
                {
                    if(!isHostileBuildingShown(*b, map, &live))
                        continue;
                }
                items.push_back({static_cast<float>((b->tileY + b->buildHeight) * TILE_HEIGHT), 1, 0, nullptr, b, live});
            }
        }
        std::stable_sort(items.begin(), items.end(), [](const DepthItem &l, const DepthItem &r) { return l.footY != r.footY ? l.footY < r.footY : l.order < r.order; });

        for(const DepthItem &it : items)
        {
            if(it.order == 0)
            {
                map.renderWallRow(it.row, renderer, camera, fogEnabled, globalAnimTick, startTileX, endTileX);
            }
            else if(const Unit *u = it.unit)
            {
                float wx = u->getWorldPixelX();
                float wy = u->getWorldPixelY();
                if(u->selected)
                {
                    int ringComp = u->isInfantry() ? 1811 : (u->archetypeId == 6 ? 1812 : 1813);
                    renderer.drawCompositeWorld(ringComp, wx - 90.0f, wy - 60.0f, camera, u->faction);
                    if(u->isSiegeMode || u->state == ActionState::Transform)
                    {
                        renderer.drawCompositeWorld(1872, wx - 90.0f, wy - 60.0f, camera, u->faction);
                    }
                }
                bool isFlaming = (u->state == ActionState::Die) ? u->killedByFlame : (u->projectile == ProjectileType::Flame);
                renderer.setDrawOwner(u->owner);
                renderer.drawUnitWorld(u->archetypeId, static_cast<int>(u->state), u->facingDirection, u->animFrame, wx, wy, camera, u->faction, u->turretFacing, isFlaming, u->wreckComposite, u->wreckStages, u->selectedDeathSeq);
            }
            else if(const Building *b = it.building)
            {
                const bool live = it.live;
                int btype = (b->rawBuildingType >= 0) ? b->rawBuildingType : (static_cast<int>(b->type) % 15);
                float bx = b->tileX * TILE_WIDTH;
                float by = b->tileY * TILE_HEIGHT;
                bool isProdBuilding = (btype == 2 || btype == 3 || btype == 10 || btype == 11);
                int bAnim = (!live || (isProdBuilding && !b->isProducing)) ? 0 : (globalAnimTick + b->id);
                float hpRatio = (b->maxHp > 0) ? (static_cast<float>(b->currentHp) / b->maxHp) : 1.0f;
                bool isAttacking = live && (b->state == ActionState::Attack && b->attackAnimTimer > 0.0f);

                renderer.setDrawOwner(b->owner);

                // The progress of an enemy construction is not shown: just the scaffolding
                const bool hiddenProgress = fogEnabled && b->buildProgress < 1.0f && Players::hostile(b->owner, Players::local());
                const float shownProgress = hiddenProgress ? 0.5f : b->buildProgress;
                renderer.drawBuildingWorld(btype, bx, by, 0, camera, b->faction, bAnim, shownProgress, false, hpRatio, b->turretFacing, isAttacking, b->isPowered);
                renderer.setDrawOwner(b->owner);
                renderer.drawBuildingWorld(btype, bx, by, 1, camera, b->faction, bAnim, shownProgress, false, hpRatio, b->turretFacing, isAttacking, b->isPowered);

                if(b->selected)
                {
                    float sizeW = b->buildWidth * TILE_WIDTH;
                    float sizeH = b->buildHeight * TILE_HEIGHT;
                    renderer.drawRectWorld(b->tileX * TILE_WIDTH, b->tileY * TILE_HEIGHT, sizeW, sizeH, camera, {0, 255, 0, 255});
                }
            }
        }

        // Bars on top of every sprite
        for(const DepthItem &it : items)
        {
            if(const Unit *u = it.unit)
            {
                bool showUnitHp = showAllHealth || (debugMode ? (u->currentHp < u->maxHp || u->selected) : (u->selected && u->owner == Players::local()));
                if(showUnitHp)
                {
                    float barW = 24.0f;
                    float barH = 3.0f;
                    float barX = u->getWorldPixelX() + (TILE_WIDTH * 0.5f) - barW * 0.5f;
                    float barY = u->getWorldPixelY() - 10.0f;
                    float hpPct = std::clamp(static_cast<float>(u->currentHp) / u->maxHp, 0.0f, 1.0f);
                    renderer.fillRectWorld(barX, barY, barW, barH, camera, {60, 0, 0, 200});
                    renderer.fillRectWorld(barX, barY, barW * hpPct, barH, camera, {0, 230, 0, 255});
                }
                continue;
            }
            const Building *b = it.building;
            if(!b)
                continue;
            if(b->buildProgress < 1.0f)
            {
                // No progress bar over an enemy construction
                if(fogEnabled && Players::hostile(b->owner, Players::local()))
                    continue;
                float barW = b->buildWidth * TILE_WIDTH;
                float barH = 4.0f;
                float barX = b->tileX * TILE_WIDTH;
                float barY = b->tileY * TILE_HEIGHT - 12.0f;

                float progPct = std::clamp(b->buildProgress, 0.0f, 1.0f);
                renderer.fillRectWorld(barX, barY, barW, barH, camera, {20, 30, 45, 220});
                renderer.fillRectWorld(barX, barY, barW * progPct, barH, camera, {255, 215, 0, 255});
                renderer.drawRectWorld(barX, barY, barW, barH, camera, {255, 255, 255, 200});

                bool showBuildingHp = debugMode ? (b->currentHp < b->maxHp || b->selected) : (b->selected && b->owner == Players::local());
                if(showBuildingHp)
                {
                    float hpPct = std::clamp(static_cast<float>(b->currentHp) / b->maxHp, 0.0f, 1.0f);
                    renderer.fillRectWorld(barX, barY + 5.0f, barW, 3.0f, camera, {60, 0, 0, 200});
                    renderer.fillRectWorld(barX, barY + 5.0f, barW * hpPct, 3.0f, camera, {0, 230, 0, 255});
                }
            }
            else
            {
                bool showBuildingHp = debugMode ? (b->currentHp < b->maxHp || b->selected) : (b->selected && b->owner == Players::local());
                if(showBuildingHp)
                {
                    float barW = b->buildWidth * TILE_WIDTH;
                    float barH = 3.0f;
                    float barX = b->tileX * TILE_WIDTH;
                    float barY = b->tileY * TILE_HEIGHT - 6.0f;

                    float hpPct = std::clamp(static_cast<float>(b->currentHp) / b->maxHp, 0.0f, 1.0f);
                    renderer.fillRectWorld(barX, barY, barW, barH, camera, {60, 0, 0, 200});
                    renderer.fillRectWorld(barX, barY, barW * hpPct, barH, camera, {0, 230, 0, 255});
                }
            }
        }

        // 4. Render Smoke Puffs
        for(const auto &puff : m_smokePuffs)
        {
            if(!puff.active)
                continue;
            int frame = static_cast<int>((puff.lifetime / puff.maxLifetime) * 6.0f);
            renderer.drawSmokePuffWorld(frame, puff.x, puff.y, camera);
        }

        // 5..6. Projectiles and explosions (drawn again by the glow pass of the shader effects)
        renderEmissive(renderer, camera);
    }

    void EntityManager::renderEmissive(Renderer &renderer, const Camera &camera) const
    {
        // Projectiles
        for(const auto &proj : m_projectiles)
        {
            if(!proj.active)
                continue;
            if(proj.type == ProjectileType::Flame)
            {
                float angle = DetMath::atan2(proj.targetY - proj.startY, proj.targetX - proj.startX);
                float totalDist = DetMath::hypot(proj.targetX - proj.startX, proj.targetY - proj.startY);
                float curDist = DetMath::hypot(proj.x - proj.startX, proj.y - proj.startY);
                float prog = (totalDist > 0.001f) ? (curDist / totalDist) : 1.0f;
                if(prog > 1.0f)
                    prog = 1.0f;
                int flameFrame = std::clamp(static_cast<int>(prog * 11.0f), 0, 11);
                renderer.setDrawOwner(proj.owner);
                renderer.drawAnimSequenceWorldRotated(59, flameFrame, proj.startX, proj.startY, angle, camera, proj.faction);
            }
            else if(proj.type == ProjectileType::Rocket)
            {
                renderer.setDrawOwner(proj.owner);
                renderer.drawProjectileWorld(1405, proj.direction, proj.x, proj.y, camera, proj.faction, 16);
            }
            else if(proj.type == ProjectileType::Shell)
            {
                renderer.setDrawOwner(proj.owner);
                renderer.drawProjectileWorld(1341, proj.dir32, proj.x, proj.y, camera, proj.faction, 32);
            }
            else
            {
                renderer.setDrawOwner(proj.owner);
                renderer.drawProjectileWorld(1309, proj.dir32, proj.x, proj.y, camera, proj.faction, 32);
            }
        }

        // 6. Render Explosions (with visual centering for bullet hits and impacts)
        for(const auto &exp : m_explosions)
        {
            if(!exp.active || exp.delay > 0.0f)
                continue;
            float ox = 0.0f, oy = 0.0f;
            explosionCentering(exp.animId, ox, oy);
            renderer.drawAnimSequenceWorld(exp.animId, exp.currentFrame, exp.x + ox, exp.y + oy, camera);
        }
    }

} // namespace OpenArtOfWar
