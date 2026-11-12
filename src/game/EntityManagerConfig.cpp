#include "game/EntityManager.hpp"
#include "EntityManagerInternal.hpp"
#include "graphics/SpriteRegistry.hpp"
#include "game/Projectiles.hpp"
#include "game/WeaponBalance.hpp"
#include "core/DetMath.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
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

namespace
{

// animations.wreck_composite: "<object>.<family of stages>" (the stage count defaults to the family size) or a
// single composite; animations.wreck_stages overrides the count
void ReadWreck(const YAML::Node &anim, int &composite, int &stages, const std::string &where)
{
    if (!anim || !anim.IsMap())
        return;
    if (anim["wreck_composite"] && anim["wreck_composite"].IsScalar())
    {
        const std::string ref = anim["wreck_composite"].as<std::string>();
        const auto dot = ref.find('.');
        const SpriteFamily *f
            = dot == std::string::npos ? nullptr : Sprites().FindFamily(ref.substr(0, dot), ref.substr(dot + 1));
        if (f && f->count > 0)
        {
            composite = f->start;
            stages = f->count;
        }
        else
        {
            composite = SpriteHandle(anim["wreck_composite"], SpriteKind::Part, where);
        }
    }
    try
    {
        if (anim["wreck_stages"])
            stages = anim["wreck_stages"].as<int>();
    }
    catch (...)
    {}
}
} // namespace

// A key-style msgid is translated when gettext returns something else than the key itself
static bool HasTranslation(const std::string &key)
{
    return Localization::Tr(key) != key;
}

static const char *ProjectileTypeName(ProjectileType type)
{
    return Projectiles::KeyOf(type);
}

inline ProjectileType StringToProjectileType(const std::string &str)
{
    return Projectiles::FromKey(str);
}

bool EntityManager::LoadCharacterConfigs()
{
    namespace fs = std::filesystem;
    // The definition files are listed by name in main.yaml (characters.files)
    const std::vector<std::string> files = LoaderResources::CharacterFiles();
    if (files.empty())
    {
        std::cerr << "[EntityManager] main.yaml lists no character files (characters.files)" << std::endl;
        return false;
    }

    m_unitConfigs.clear();
    m_buildingConfigs.clear();

    for (const std::string &file : files)
    {
        const fs::path entryPath(file);
        if (entryPath.filename().string().rfind("research_", 0) == 0)
            continue; // technology trees, see Research.cpp

        try
        {
            std::string text;
            if (!Assets::ReadText(file, text))
                continue;
            LoadCharacterYaml(YAML::Load(text));
        }
        catch (const std::exception &e)
        {
            std::cerr << "[EntityManager] Error loading YAML " << entryPath << ": " << e.what() << std::endl;
        }
    }

    RebuildBlueprints();

    std::cout << "[EntityManager] Loaded " << m_unitConfigs.size() << " unit configs and " << m_buildingConfigs.size()
              << " building configs from " << files.size() << " YAML files" << std::endl;
    return true;
}

bool EntityManager::LoadCharacterYaml(YAML::Node root)
{
    auto getVal = [](const YAML::Node &node, const char *key, auto def) {
        using T = decltype(def);
        if (node && node[key])
        {
            try
            {
                return node[key].as<T>();
            }
            catch (...)
            {}
        }
        return def;
    };
    auto getStr = [&](const YAML::Node &node, const char *key, const std::string &def = "") -> std::string {
        return getVal(node, key, def);
    };
    auto getInt = [&](const YAML::Node &node, const char *key, int def = 0) -> int {
        return getVal(node, key, def);
    };
    auto getFloat = [&](const YAML::Node &node, const char *key, float def = 0.0f) -> float {
        return getVal(node, key, def);
    };
    auto getBool = [&](const YAML::Node &node, const char *key, bool def = false) -> bool {
        return getVal(node, key, def);
    };

    if (!root.IsDefined() || !root.IsMap())
        return false;
    if (root["default_difficulty"].IsDefined())
        return false;

    std::string id = getStr(root, "id");
    const std::string where = "character " + id; // for messages about its sprites
    std::string type = getStr(root, "type");
    const int archId = getInt(root, "archetype_id", -1);
    const int bldgIdx = getInt(root, "building_index", -1);

    // Faction, internal type and class follow from archetype_id / building_index alone (Utility tables);
    // they are not read from the file, so a file can never contradict the game about them
    Faction faction = Faction::Resistance;
    UnitType uType = UnitType::Unknown;
    if (const Utility::ArchetypeInfo *a = (type == "building") ? nullptr : Utility::GetArchetypeInfo(archId))
    {
        faction = a->faction;
        uType = a->type;
    }
    else if (const Utility::BuildingKindInfo *k = Utility::GetBuildingKindInfo(bldgIdx))
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
    std::string desc = DescriptionKey(id);
    std::string uClass = Utility::UnitClassName(archId);

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
    const int iconComp = SpriteHandle(prodNode ? prodNode["icon_composite"] : YAML::Node(), SpriteKind::Part, where);
    std::string producedIn = Utility::ProducedIn(archId);

    YAML::Node combatNode = root["combat"];
    bool canAttack = getBool(combatNode, "can_attack", type == "unit");
    bool hasRotTurret = getBool(combatNode, "has_rotating_turret", false);
    bool hasRotRadar = getBool(combatNode, "has_rotating_radar", false);

    auto getSeqFloat = [](const YAML::Node &node, std::size_t idx, float def = 0.0f) -> float {
        if (node && node.IsSequence() && idx < node.size())
        {
            try
            {
                return node[idx].as<float>();
            }
            catch (...)
            {}
        }
        return def;
    };

    // Weapons live only in combat.weapons; weapons[0] is the main gun the unit / building fights with.
    // Fields a weapon leaves out come from `inherit` (the parent form's main gun for siege / upgrade forms).
    auto parseWeapon = [&](const YAML::Node &w, const WeaponConfig &inherit, std::size_t index) {
        WeaponConfig wc = inherit;
        wc.name = getStr(w, "name", index == 0 ? inherit.name : "Weapon " + std::to_string(index + 1));
        wc.projectile = StringToProjectileType(getStr(w, "projectile", ProjectileTypeName(inherit.projectile)));
        wc.damage = getInt(w, "damage", inherit.damage);
        wc.range = getFloat(w, "range", inherit.range);
        wc.minRange = getFloat(w, "min_range", inherit.minRange);
        // Delay between attack cycles: fixed (`cooldown`) or random within [cooldown_min, cooldown_max]
        const std::string mode = getStr(w, "cooldown_mode", inherit.cooldownMax > 0.0f ? "random" : "fixed");
        if (mode == "random")
        {
            wc.cooldown = getFloat(w, "cooldown_min", getFloat(w, "cooldown", inherit.cooldown));
            wc.cooldownMax
                = std::max(wc.cooldown, getFloat(w, "cooldown_max", std::max(inherit.cooldownMax, wc.cooldown)));
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
        wc.sound = getStr(w, "fire_sfx", inherit.sound);
        wc.impactSound = getStr(w, "impact_sfx", inherit.impactSound);
        wc.soundVolume = std::max(0.0f, getFloat(w, "sound_volume", inherit.soundVolume));
        wc.soundId = InternSound(wc.sound);
        wc.impactSoundId = InternSound(wc.impactSound);
        if (w["localePosition"] && w["localePosition"].IsSequence())
        {
            wc.muzzleOffsetX = getSeqFloat(w["localePosition"], 0, inherit.muzzleOffsetX);
            wc.muzzleOffsetY = getSeqFloat(w["localePosition"], 1, inherit.muzzleOffsetY);
        }
        return wc;
    };
    auto parseWeapons = [&](const YAML::Node &combat, const WeaponConfig &inherit) {
        std::vector<WeaponConfig> list;
        const YAML::Node ws = combat ? combat["weapons"] : YAML::Node();
        if (ws && ws.IsSequence())
        {
            for (std::size_t wi = 0; wi < ws.size(); ++wi)
            {
                if (ws[wi].IsMap())
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
    // sounds: { death: <event>, damage: <event>, volume: 1.0 } - the character's own sounds (its guns have theirs in
    // combat.weapons: fire_sfx, impact_sfx)
    CharacterSounds sounds;
    if (const YAML::Node sn = root["sounds"]; sn && sn.IsMap())
    {
        sounds.death = getStr(sn, "death", "");
        sounds.damage = getStr(sn, "damage", "");
        sounds.volume = std::max(0.0f, getFloat(sn, "volume", 1.0f));
    }
    sounds.deathId = InternSound(sounds.death);
    sounds.damageId = InternSound(sounds.damage);
    const WeaponConfig primary = weapons.empty() ? noWeapon : weapons.front();
    int damage = primary.damage;
    float range = primary.range;
    float cooldown = primary.cooldown;
    float cooldownMax = primary.cooldownMax;
    // Flight speed of rockets and shells, px/s (editor: Bullet / Projectile Speed)
    const float bulletSpeedPxPerSec
        = std::max(0.0f, getFloat(combatNode, "bullet_speed", getFloat(combatNode, "projectile_speed", 0.0f)));
    int burstCount = primary.burstCount;
    float burstDelay = primary.burstDelay;
    int splashPercent = primary.splashPercent;
    ProjectileType projectile = primary.projectile;
    float muzzleLocX = primary.muzzleOffsetX;
    float muzzleLocY = primary.muzzleOffsetY;

    float turretOffX = 0.0f;
    float turretOffY = 0.0f;
    bool hasCustomTurretOff = false;
    if (combatNode["turret_offset"] && combatNode["turret_offset"].IsSequence()
        && combatNode["turret_offset"].size() >= 2)
    {
        turretOffX = getSeqFloat(combatNode["turret_offset"], 0, 0.0f);
        turretOffY = getSeqFloat(combatNode["turret_offset"], 1, 0.0f);
        hasCustomTurretOff = true;
    }
    else if (
        combatNode["dome_offset"] && combatNode["dome_offset"].IsSequence() && combatNode["dome_offset"].size() >= 2)
    {
        turretOffX = getSeqFloat(combatNode["dome_offset"], 0, 0.0f);
        turretOffY = getSeqFloat(combatNode["dome_offset"], 1, 0.0f);
        hasCustomTurretOff = true;
    }
    else if (
        combatNode["turretOffset"] && combatNode["turretOffset"].IsSequence() && combatNode["turretOffset"].size() >= 2)
    {
        turretOffX = getSeqFloat(combatNode["turretOffset"], 0, 0.0f);
        turretOffY = getSeqFloat(combatNode["turretOffset"], 1, 0.0f);
        hasCustomTurretOff = true;
    }
    else if (
        combatNode["turret_pivot"] && combatNode["turret_pivot"].IsSequence() && combatNode["turret_pivot"].size() >= 2)
    {
        turretOffX = getSeqFloat(combatNode["turret_pivot"], 0, 0.0f);
        turretOffY = getSeqFloat(combatNode["turret_pivot"], 1, 0.0f);
        hasCustomTurretOff = true;
    }
    else if (
        combatNode["turret_offset_x"].IsDefined() || combatNode["turret_offset_y"].IsDefined()
        || combatNode["dome_offset_x"].IsDefined() || combatNode["dome_offset_y"].IsDefined())
    {
        turretOffX = getFloat(combatNode, "turret_offset_x", getFloat(combatNode, "dome_offset_x", 0.0f));
        turretOffY = getFloat(combatNode, "turret_offset_y", getFloat(combatNode, "dome_offset_y", 0.0f));
        hasCustomTurretOff = true;
    }

    float spreadAngleVal = -1.0f;
    if (combatNode["spread_angle"].IsDefined())
    {
        float sDeg = getFloat(combatNode, "spread_angle", 0.0f);
        spreadAngleVal = std::max(0.0f, sDeg * (static_cast<float>(M_PI) / 180.0f));
    }
    else if (combatNode["spread"].IsDefined())
    {
        float sVal = getFloat(combatNode, "spread", 0.0f);
        spreadAngleVal = std::max(0.0f, (sVal > 0.5f) ? (sVal * static_cast<float>(M_PI) / 180.0f) : sVal);
    }

    YAML::Node animNode = root["animations"];

    YAML::Node abNode = root["abilities"];
    bool isSiege = getBool(abNode, "siege_mode", false);
    int siegeArch = getInt(abNode, "siege_archetype_id", -1);
    int parentArch = getInt(abNode, "parent_archetype_id", -1);

    if (type == "unit" || archId >= 0)
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
        cfg.bulletSpeed = bulletSpeedPxPerSec;
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

        const bool isInf = Utility::IsInfantryArchetype(cfg.archetypeId);
        cfg.turnStepDelay = getFloat(root, "turn_step_delay", Utility::DefaultTurnStepDelay(cfg.archetypeId));
        const float defTurretDelay = root["turn_step_delay"].IsDefined()
                                         ? cfg.turnStepDelay
                                         : Utility::DefaultTurretTurnStepDelay(cfg.archetypeId);
        cfg.turretTurnStepDelay
            = getFloat(combatNode, "turret_turn_step_delay", getFloat(root, "turret_turn_step_delay", defTurretDelay));

        cfg.description = desc;

        cfg.wreckType = isInf ? "infantry" : "vehicle";
        const EffectsConfig::Wreck defaultWreck = EffectsConfig::Get().WreckOfVehicle(cfg.archetypeId);
        const int defaultWreckComp = defaultWreck.composite;
        const int defaultWreckStages = defaultWreck.stages;
        cfg.wreckComposite = defaultWreckComp;
        cfg.wreckStages = defaultWreckStages;
        ReadWreck(animNode, cfg.wreckComposite, cfg.wreckStages, where);
        cfg.moveFrameTime = std::clamp(getFloat(animNode, "move_frame_time", cfg.moveFrameTime), 0.02f, 1.0f);
        cfg.attackFrameTime = getFloat(animNode, "attack_frame_time", cfg.attackFrameTime);
        cfg.attackPoseTime = getFloat(animNode, "attack_pose_time", cfg.attackPoseTime);
        cfg.burstPoseTime = std::clamp(getFloat(animNode, "burst_pose_time", cfg.burstPoseTime), 0.02f, 5.0f);
        {
            const std::string mode
                = animNode && animNode["flash_mode"] ? animNode["flash_mode"].as<std::string>("loop") : "loop";
            cfg.flashMode = mode == "once" ? 1 : (mode == "hold" ? 2 : 0);
        }
        cfg.flashFrames = std::max(0, getInt(animNode, "flash_frames", 0));

        // Death sequences are sprites by name (fx_infantry_death.death_0); the older "*_variants" keys too
        for (const char *key : {"death_sequences", "death_variants"})
        {
            if (animNode[key] && animNode[key].IsSequence())
            {
                cfg.deathSequences = SpriteHandles(animNode[key], SpriteKind::Sequence, where);
                break;
            }
        }
        if (cfg.deathSequences.empty() && isInf)
            cfg.deathSequences = DefaultDeaths(false);
        for (const char *key : {"flame_death_sequences", "flame_death_variants"})
        {
            if (animNode[key] && animNode[key].IsSequence())
            {
                cfg.flameDeathSequences = SpriteHandles(animNode[key], SpriteKind::Sequence, where);
                break;
            }
        }
        if (cfg.flameDeathSequences.empty() && isInf)
            cfg.flameDeathSequences = DefaultDeaths(true);

        cfg.weapons = weapons;
        if (cfg.weapons.empty() && canAttack)
            cfg.weapons.push_back(primary);
        cfg.sounds = sounds;

        // Siege / baseline forms bring their own weapons; anything they leave out stays as in the main form
        auto applyForm = [&](UnitConfig &form, const YAML::Node &formCombat) {
            std::vector<WeaponConfig> formWeapons = parseWeapons(formCombat, primary);
            if (!formWeapons.empty())
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
            form.turretTurnStepDelay = getFloat(formCombat, "turret_turn_step_delay", form.turretTurnStepDelay);
        };

        // Check for embedded siege mode configuration (consolidated)
        YAML::Node sNode = abNode["siege"].IsDefined() ? abNode["siege"] : root["siege"];
        if (sNode.IsDefined() && (getBool(sNode, "enabled", true) || sNode["archetype_id"].IsDefined()))
        {
            int sArch = getInt(sNode, "archetype_id", -1);
            if (sArch >= 0)
            {
                UnitConfig siegeCfg = cfg;
                siegeCfg.archetypeId = sArch;
                siegeCfg.id = id + "_siege";
                siegeCfg.name = getStr(sNode, "name", name + " (Siege Mode)");
                // The siege form has its own text when "<id>_siege.description" is translated, else the unit's
                siegeCfg.description = HasTranslation(DescriptionKey(siegeCfg.id)) ? DescriptionKey(siegeCfg.id) : desc;
                siegeCfg.isSiegeMode = true;
                siegeCfg.parentArchetypeId = cfg.archetypeId;
                cfg.siegeArchetypeId = sArch;
                cfg.siegeRequiresResearch = getBool(sNode, "requires_research", false);
                {
                    const std::string mode = getStr(sNode, "auto_deploy", "far");
                    cfg.autoSiege = mode == "off"      ? UnitConfig::AutoSiege::Off
                                    : mode == "always" ? UnitConfig::AutoSiege::Always
                                                       : UnitConfig::AutoSiege::Far;
                }

                // One maximum health for both forms: the unit's own max_hp (a "max_hp" in the siege stats is ignored)
                siegeCfg.maxHp = cfg.maxHp;
                if (sNode["stats"])
                {
                    siegeCfg.sightRadius = getInt(sNode["stats"], "sight_radius", cfg.sightRadius);
                    siegeCfg.moveSpeed = getFloat(sNode["stats"], "move_speed", 0.0f);
                }
                else
                {
                    siegeCfg.moveSpeed = 0.0f;
                }

                if (sNode["combat"])
                {
                    applyForm(siegeCfg, sNode["combat"]);
                }

                m_unitConfigs[siegeCfg.archetypeId] = siegeCfg;
            }
        }

        // Check for embedded modernization upgrade configuration (consolidated)
        YAML::Node upNode = abNode["upgrade"].IsDefined() ? abNode["upgrade"] : root["upgrade"];
        if (upNode.IsDefined()
            && (getBool(upNode, "has_baseline", true) || upNode["baseline_archetype_id"].IsDefined()))
        {
            int bArch = getInt(upNode, "baseline_archetype_id", -1);
            if (bArch >= 0)
            {
                UnitConfig baseCfg = cfg;
                baseCfg.archetypeId = bArch;
                baseCfg.id = id + "_base";
                baseCfg.name = getStr(upNode, "baseline_name", name + " (Base)");
                if (upNode["combat"])
                {
                    applyForm(baseCfg, upNode["combat"]);
                }
                // The base form without a weapon list of its own has the main form's guns, except those that come
                // with the technology (requires_research): the Zeus without its rocket launcher
                if (!(upNode["combat"] && upNode["combat"]["weapons"]))
                {
                    baseCfg.weapons.erase(
                        std::remove_if(
                            baseCfg.weapons.begin(),
                            baseCfg.weapons.end(),
                            [](const WeaponConfig &w) { return w.requiresResearch; }),
                        baseCfg.weapons.end());
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
        cfg.radarRange = std::max(0, getInt(statsNode, "radar_range", 0));
        cfg.creditCost = creditCost;
        cfg.powerCost = powerCost;
        cfg.buildTime = buildTime;
        cfg.iconCompId = iconComp;
        cfg.canAttack = canAttack;
        cfg.burstCount = std::max(1, primary.burstCount);
        cfg.burstDelay = std::max(0.02f, primary.burstDelay);
        cfg.bulletSpeed = bulletSpeedPxPerSec;
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
        if (root["stats"] && root["stats"]["build_radius"])
        {
            bRadius = getInt(root["stats"], "build_radius", bRadius);
        }
        cfg.buildRadius = bRadius;
        if (cfg.type == UnitType::Headquarters && faction != Faction::Neutral)
        {
            m_factionBuildRadius[static_cast<std::size_t>(faction)] = bRadius;
            m_buildRadiusExtra = {};
        }
        cfg.wreckComposite = -1;
        cfg.wreckStages = 0;
        ReadWreck(animNode, cfg.wreckComposite, cfg.wreckStages, where);
        cfg.description = desc;
        cfg.weapons = weapons;
        cfg.sounds = sounds;

        m_buildingConfigs[cfg.buildingIndex] = cfg;
    }
    return true;
}

} // namespace OpenArtOfWar
