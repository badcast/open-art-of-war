#include "game/EntityManager.hpp"
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

std::vector<BuildingBlueprint> EntityManager::s_blueprints;
std::vector<BuildingBlueprint> EntityManager::s_factionBlueprints[2];

std::string EntityManager::DescriptionKey(const std::string &characterId)
{
    return characterId + ".description";
}

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

int EntityManager::ApplyMovingUnitDamage(int baseDamage, const Entity *target, ProjectileType kind)
{
    if (!target || baseDamage <= 0)
        return baseDamage;
    baseDamage = WeaponBalance::Scale(baseDamage, kind, WeaponBalance::ClassOf(*target));
    const Unit *u = target->AsUnit();
    // A unit on the move is harder to hit: it takes a random share of the damage (weapon_balance.json: moving_target),
    // from every kind of damage (direct hit, splash, flame)
    if (u && (u->state == ActionState::Move || (!u->path.empty() && u->pathIndex < u->path.size() && !u->waitingForPath)))
    {
        int lo = 50, hi = 100;
        WeaponBalance::MovingTargetPercent(lo, hi);
        const uint32_t roll = static_cast<uint32_t>(Utility::Random(lo, hi));
        int finalDamage = static_cast<int>((static_cast<uint64_t>(baseDamage) * roll + 50u) / 100u);
        return ReduceDamageByArmor((finalDamage < 1) ? 1 : finalDamage, *target);
    }
    return ReduceDamageByArmor(baseDamage, *target);
}

// Infantry heals by itself; vehicles are repaired close to their faction's Factory. The technologies of the
// faction make both 3 times faster.
void EntityManager::UpdateRegeneration(float deltaTime)
{
    constexpr float kInfantryRegen = 0.3f; // HP per second
    constexpr float kRepair = 1.0f;        // HP per second near a Factory
    constexpr float kRepairRadius = 5.0f;  // tiles
    std::vector<const Building *> factories;
    for (const auto &e : m_entities)
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->state != ActionState::Die && b->type == UnitType::Factory && b->buildProgress >= 1.0f)
        {
            factories.push_back(b);
        }
    }
    for (auto &e : m_entities)
    {
        Unit *u = e.AsUnit();
        if (!u || !u->alive || u->state == ActionState::Die || u->currentHp >= u->maxHp)
            continue;
        float rate = 0.0f;
        if (u->IsInfantry())
        {
            rate = kInfantryRegen * (IsEffectResearched(u->owner, ResearchEffect::InfantryRegen) ? 3.0f : 1.0f);
            if (u->state != ActionState::Idle)
                rate *= 0.5f; // walking wounded recover slower
        }
        else
        {
            for (const Building *f : factories)
            {
                if (f->owner != u->owner)
                    continue;
                if (DetMath::Hypot(static_cast<float>(f->tileX - u->tileX), static_cast<float>(f->tileY - u->tileY))
                    <= kRepairRadius)
                {
                    rate = kRepair * (IsEffectResearched(u->owner, ResearchEffect::RepairSpeed) ? 3.0f : 1.0f);
                    break;
                }
            }
        }
        if (rate <= 0.0f)
            continue;
        u->regenAccum += rate * deltaTime;
        if (u->regenAccum >= 1.0f)
        {
            const int heal = static_cast<int>(u->regenAccum);
            u->regenAccum -= static_cast<float>(heal);
            u->currentHp = std::min(u->maxHp, u->currentHp + heal);
        }
    }
}

// One hit effect for every bullet that hits something - infantry, vehicles, buildings alike:
// the impact sparks (55..58, variant picked by the target). No blood.
static int BulletImpactAnim(int targetId)
{
    return 55 + (targetId % 4);
}

// Distance in tiles from tile (px, py) to the segment (ax, ay) - (bx, by)
static float DistanceToSegment(int px, int py, int ax, int ay, int bx, int by)
{
    const float dx = static_cast<float>(bx - ax), dy = static_cast<float>(by - ay);
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return DetMath::Hypot(px - (ax + t * dx), py - (ay + t * dy));
}

static void HandleUnitDeathState(Unit *tu, bool isFlameDamage)
{
    if (!tu)
        return;
    tu->path.clear();
    tu->deathTimer = EffectsConfig::Get().burialSeconds;
    tu->animFrame = 0;
    tu->animTimer = 0.0f;
    if (isFlameDamage || tu->killedByFlame)
    {
        tu->killedByFlame = true;
        if (!tu->flameDeathSequences.empty())
        {
            tu->selectedDeathSeq = Utility::VisualPick(tu->flameDeathSequences);
        }
        else
        {
            tu->selectedDeathSeq = (tu->archetypeId % 2 == 0) ? 259 : 260;
        }
    }
    else if (tu->IsInfantry())
    {
        if (!tu->deathSequences.empty())
        {
            tu->selectedDeathSeq = Utility::VisualPick(tu->deathSequences);
        }
        else
        {
            tu->selectedDeathSeq = Utility::VisualRandom(247, 248);
        }
    }
}

EntityManager::EntityManager()
{
    m_entities.Reserve(MAX_UNITS);
    LoadCharacterConfigs(LoaderResources::kDefaultRoot);
}

bool EntityManager::LoadCharacterConfigs(const std::string &assetRoot)
{
    namespace fs = std::filesystem;
    // The definition files are listed by name in main.json (characters.files)
    const std::vector<std::string> files = LoaderResources::CharacterFiles(assetRoot);
    if (files.empty())
    {
        std::cerr << "[EntityManager] main.json lists no character files (characters.files)" << std::endl;
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
    int iconComp = getInt(prodNode, "icon_composite", 0);
    std::string producedIn = Utility::ProducedIn(archId);

    YAML::Node combatNode = root["combat"];
    bool canAttack = getBool(combatNode, "can_attack", type == "unit");
    bool hasRotTurret = getBool(combatNode, "has_rotating_turret", false);
    bool hasRotRadar = getBool(combatNode, "has_rotating_radar", false);

    auto getSeqFloat = [](const YAML::Node &node, size_t idx, float def = 0.0f) -> float {
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
    auto parseWeapon = [&](const YAML::Node &w, const WeaponConfig &inherit, size_t index) {
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
            for (size_t wi = 0; wi < ws.size(); ++wi)
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
    const float bulletSpeedPxPerSec = std::max(0.0f, getFloat(combatNode, "bullet_speed", getFloat(combatNode, "projectile_speed", 0.0f)));
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
        cfg.wreckComposite = getInt(animNode, "wreck_composite", defaultWreckComp);
        cfg.wreckStages = getInt(animNode, "wreck_stages", defaultWreckStages);
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

        if (animNode["death_sequences"] && animNode["death_sequences"].IsSequence())
        {
            for (const auto &item : animNode["death_sequences"])
            {
                try
                {
                    cfg.deathSequences.push_back(item.as<int>());
                }
                catch (...)
                {}
            }
        }
        else if (animNode["death_variants"] && animNode["death_variants"].IsSequence())
        {
            for (const auto &item : animNode["death_variants"])
            {
                try
                {
                    cfg.deathSequences.push_back(item.as<int>());
                }
                catch (...)
                {}
            }
        }
        else if (isInf)
        {
            cfg.deathSequences = {247, 248};
        }

        if (animNode["flame_death_sequences"] && animNode["flame_death_sequences"].IsSequence())
        {
            for (const auto &item : animNode["flame_death_sequences"])
            {
                try
                {
                    cfg.flameDeathSequences.push_back(item.as<int>());
                }
                catch (...)
                {}
            }
        }
        else if (animNode["flame_death_variants"] && animNode["flame_death_variants"].IsSequence())
        {
            for (const auto &item : animNode["flame_death_variants"])
            {
                try
                {
                    cfg.flameDeathSequences.push_back(item.as<int>());
                }
                catch (...)
                {}
            }
        }
        else if (isInf)
        {
            cfg.flameDeathSequences = {259, 260};
        }

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

                if (sNode["stats"])
                {
                    siegeCfg.maxHp = getInt(sNode["stats"], "max_hp", cfg.maxHp);
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
            m_factionBuildRadius[static_cast<size_t>(faction)] = bRadius;
            m_buildRadiusExtra = {};
        }
        cfg.wreckComposite = getInt(animNode, "wreck_composite", 0);
        cfg.wreckStages = getInt(animNode, "wreck_stages", 0);
        cfg.description = desc;
        cfg.weapons = weapons;
        cfg.sounds = sounds;

        m_buildingConfigs[cfg.buildingIndex] = cfg;
    }
    return true;
}

void EntityManager::RebuildBlueprints()
{
    // Blueprints of the player's faction in canonical order: HQ, power, barracks, factory, lab, bunker, tower, wall
    // Both factions: commands of other players are looked up in their own faction's list
    for (int side = 0; side < 2; ++side)
    {
        std::vector<BuildingBlueprint> &list = s_factionBlueprints[side];
        list.clear();
        const std::vector<int> bpOrder
            = side == 0 ? std::vector<int> {0, 1, 2, 3, 4, 5, 7, 15} : std::vector<int> {8, 9, 10, 11, 12, 13, 14, 25};
        for (size_t slot = 0; slot < bpOrder.size(); ++slot)
        {
            const int bIdx = bpOrder[slot];
            auto it = m_buildingConfigs.find(bIdx);
            if (it != m_buildingConfigs.end())
            {
                const auto &cfg = it->second;
                BuildingBlueprint bp;
                bp.blueprintId = static_cast<int>(slot);
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
                list.push_back(bp);
            }
        }
    }
    s_blueprints = s_factionBlueprints[PlayerFaction() == Faction::Confederation ? 0 : 1];
}

const std::vector<BuildingBlueprint> &EntityManager::GetBlueprints(Faction faction)
{
    const std::vector<BuildingBlueprint> &list = s_factionBlueprints[faction == Faction::Confederation ? 0 : 1];
    return list.empty() ? GetBlueprints() : list;
}

const BuildingBlueprint *EntityManager::FindBlueprint(Faction faction, int blueprintId)
{
    for (const BuildingBlueprint &bp : GetBlueprints(faction))
    {
        if (bp.blueprintId == blueprintId)
            return &bp;
    }
    return nullptr;
}

const UnitConfig *EntityManager::GetUnitConfig(int archetypeId) const
{
    auto it = m_unitConfigs.find(archetypeId);
    if (it != m_unitConfigs.end())
    {
        return &it->second;
    }
    return nullptr;
}

const BuildingConfig *EntityManager::GetBuildingConfig(int buildingIndex) const
{
    auto it = m_buildingConfigs.find(buildingIndex);
    if (it != m_buildingConfigs.end())
    {
        return &it->second;
    }
    if (buildingIndex >= 15 && buildingIndex <= 24)
    {
        it = m_buildingConfigs.find(15);
        if (it != m_buildingConfigs.end())
            return &it->second;
    }
    else if (buildingIndex >= 25 && buildingIndex <= 29)
    {
        it = m_buildingConfigs.find(25);
        if (it != m_buildingConfigs.end())
            return &it->second;
    }
    return nullptr;
}

const UnitConfig *EntityManager::GetUnitConfigByUnitType(UnitType type, Faction faction) const
{
    for (const auto &[arch, cfg] : m_unitConfigs)
    {
        if (cfg.type == type && cfg.faction == faction && !cfg.isSiegeMode)
        {
            return &cfg;
        }
    }
    for (const auto &[arch, cfg] : m_unitConfigs)
    {
        if (cfg.type == type && cfg.faction == faction)
        {
            return &cfg;
        }
    }
    return nullptr;
}

void EntityManager::Clear()
{
    m_entities.Clear();
    m_seenBuildings.clear();
    m_projectiles.clear();
    m_explosions.clear();
    m_smokePuffs.clear();
    m_flameBursts.clear();
    m_muzzleFlashes.clear();
    m_groundMarks.clear();
    m_soundCues.clear();
    m_eliminated = {};
    // Muzzle-flash reveals name entity ids: the next game reuses them from 1, so old pings would show its units
    m_revealPings.clear();
    ++m_generation;
    m_nextId = 1;
    m_kills = {};
    m_stats = {};
    // Research effects belong to the game that is being cleared
    m_buildRadiusExtra = {};
    std::fill(std::begin(m_flamethrowerUnlocked), std::end(m_flamethrowerUnlocked), false);
    std::fill(std::begin(m_siegeUnlocked), std::end(m_siegeUnlocked), false);
}

Entity *EntityManager::GetEntity(int id)
{
    for (auto &e : m_entities)
    {
        if (e.id == id && e.alive && e.state != ActionState::Die)
            return &e;
    }
    return nullptr;
}

const Entity *EntityManager::GetEntity(int id) const
{
    for (const auto &e : m_entities)
    {
        if (e.id == id && e.alive && e.state != ActionState::Die)
            return &e;
    }
    return nullptr;
}

Unit *EntityManager::GetUnit(int id)
{
    Entity *e = GetEntity(id);
    return e ? e->AsUnit() : nullptr;
}

const Unit *EntityManager::GetUnit(int id) const
{
    const Entity *e = GetEntity(id);
    return e ? e->AsUnit() : nullptr;
}

Building *EntityManager::GetBuilding(int id)
{
    Entity *e = GetEntity(id);
    return e ? e->AsBuilding() : nullptr;
}

const Building *EntityManager::GetBuilding(int id) const
{
    const Entity *e = GetEntity(id);
    return e ? e->AsBuilding() : nullptr;
}

int EntityManager::SpawnUnit(UnitType type, int tileX, int tileY, Faction faction)
{
    int arch = 0;
    if (faction == Faction::Resistance)
    {
        switch (type)
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
        switch (type)
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
    return SpawnUnitByArchetype(arch, tileX, tileY, faction);
}

// The stats and looks a unit takes from the character file of its archetype
static void CopyUnitConfig(Unit &u, const UnitConfig &cfg)
{
    u.type = cfg.type;
    u.maxHp = cfg.maxHp;
    u.attackDamage = cfg.attackDamage;
    u.attackRange = cfg.attackRange;
    u.attackCooldown = cfg.attackCooldown;
    u.attackCooldownMax = cfg.attackCooldownMax;
    u.burstCount = cfg.burstCount;
    u.burstDelay = cfg.burstDelay;
    u.splashPercent = cfg.splashPercent;
    u.projectile = cfg.projectile;
    u.moveSpeed = cfg.moveSpeed;
    u.sightRadius = cfg.sightRadius;
    u.hasRotatingTurret = cfg.hasRotatingTurret;
    u.muzzleOffsetX = cfg.muzzleOffsetX;
    u.muzzleOffsetY = cfg.muzzleOffsetY;
    u.turretOffsetX = cfg.turretOffsetX;
    u.turretOffsetY = cfg.turretOffsetY;
    u.hasCustomTurretOffset = cfg.hasCustomTurretOffset;
    u.spreadAngle = cfg.spreadAngle;
    u.turnStepDelay = cfg.turnStepDelay;
    u.turnTimer = 0.0f;
    u.turretTurnStepDelay = cfg.turretTurnStepDelay;
    u.turretTurnTimer = 0.0f;
    u.wreckType = cfg.wreckType;
    u.wreckComposite = cfg.wreckComposite;
    u.wreckStages = cfg.wreckStages;
    u.moveFrameTime = cfg.moveFrameTime;
    u.attackFrameTime = cfg.attackFrameTime;
    u.attackPoseTime = cfg.attackPoseTime;
    u.burstPoseTime = cfg.burstPoseTime;
    u.flashMode = cfg.flashMode;
    u.flashFrames = cfg.flashFrames;
    u.deathSequences = cfg.deathSequences;
    u.flameDeathSequences = cfg.flameDeathSequences;
    u.bulletSpeed = cfg.bulletSpeed;
    // weapons[1]: a second gun beside the main one
    u.hasSecondary = false;
    if (cfg.weapons.size() > 1 && cfg.weapons[1].enabled && cfg.weapons[1].damage > 0)
    {
        const WeaponConfig &w = cfg.weapons[1];
        u.hasSecondary = true;
        u.secondaryProjectile = w.projectile;
        u.secondaryDamage = w.damage;
        u.secondaryRange = w.range;
        u.secondaryMinRange = w.minRange;
        u.secondaryCooldown = std::max(0.1f, w.cooldown);
        u.secondarySplash = w.splashPercent;
        u.secondaryMuzzleX = w.muzzleOffsetX;
        u.secondaryMuzzleY = w.muzzleOffsetY;
    }
}

int EntityManager::SpawnUnitByArchetype(int archetypeId, int tileX, int tileY, Faction faction, PlayerId owner)
{
    if (m_entities.size() >= MAX_UNITS)
        return 0;

    archetypeId = std::clamp(archetypeId, 0, 18);

    auto u = std::make_unique<Unit>();
    u->id = m_nextId++;
    u->archetypeId = archetypeId;
    u->faction = faction;
    u->owner = owner == kAutoOwner ? Players::OwnerOf(faction) : owner;
    u->tileX = tileX;
    u->tileY = tileY;
    u->subX = 0.0f;
    u->subY = 0.0f;
    u->state = ActionState::Idle;
    u->facingDirection = (faction == Faction::Resistance) ? 0 : 4;
    u->turretFacing = u->facingDirection;
    u->isSiegeMode = (archetypeId >= 14 && archetypeId <= 16);

    const UnitConfig *cfg = GetUnitConfig(archetypeId);
    if (cfg)
    {
        CopyUnitConfig(*u, *cfg);
    }
    else
    {
        switch (archetypeId)
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
        u->turnStepDelay = u->IsInfantry() ? 0.0f : Utility::DefaultTurnStepDelay(archetypeId);
        u->turnTimer = 0.0f;
        u->turretTurnStepDelay = u->IsInfantry() ? 0.0f : Utility::DefaultTurretTurnStepDelay(archetypeId);
        u->turretTurnTimer = 0.0f;
        if (u->IsInfantry())
        {
            u->wreckType = "infantry";
            u->deathSequences = {247, 248};
            u->flameDeathSequences = {259, 260};
        }
        else
        {
            u->wreckType = "vehicle";
            const EffectsConfig::Wreck wreck = EffectsConfig::Get().WreckOfVehicle(archetypeId);
            u->wreckComposite = wreck.composite;
            u->wreckStages = wreck.stages;
        }
    }
    u->currentHp = u->maxHp;

    int newId = u->id;
    m_entities.push_back(std::move(u));
    return newId;
}

bool EntityManager::UpgradeUnitForm(int unitId, int newArchetype)
{
    Unit *u = GetUnit(unitId);
    const UnitConfig *cfg = GetUnitConfig(newArchetype);
    if (!u || !u->alive || !cfg || u->archetypeId == newArchetype)
        return false;
    const int oldMax = std::max(1, u->maxHp);
    const int oldHp = u->currentHp;
    u->archetypeId = newArchetype;
    CopyUnitConfig(*u, *cfg);
    u->currentHp = std::max(1, static_cast<int>(static_cast<int64_t>(oldHp) * u->maxHp / oldMax));
    u->turnTimer = 0.0f;
    u->turretTurnTimer = 0.0f;
    ApplyCompletedResearch(*u); // the technologies of the player apply to the new form too
    return true;
}

int EntityManager::SpawnBuilding(UnitType type, int tileX, int tileY, Faction faction)
{
    int bIdx = 0;
    switch (type)
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
    return SpawnBuildingByIndex(bIdx, tileX, tileY, faction, 1.0f);
}

int EntityManager::SpawnBuildingByIndex(
    int buildingIdx, int tileX, int tileY, Faction faction, float initialProgress, PlayerId owner)
{
    if (m_entities.size() >= MAX_UNITS)
        return 0;

    static const uint8_t s_widths[15] = {4, 3, 3, 4, 3, 2, 2, 2, 3, 3, 3, 3, 3, 2, 2};
    static const uint8_t s_heights[15] = {4, 3, 3, 3, 3, 2, 2, 2, 4, 3, 3, 3, 3, 2, 2};

    auto b = std::make_unique<Building>();
    b->id = m_nextId++;
    b->faction = faction;
    b->owner = owner == kAutoOwner ? Players::OwnerOf(faction) : owner;
    b->tileX = tileX;
    b->tileY = tileY;
    b->subX = 0.0f;
    b->subY = 0.0f;
    b->isPowered = true;
    b->state = ActionState::Idle;
    b->sightRadius = 7;
    b->rawBuildingType = buildingIdx;
    b->powerOutput = BuildingPower(buildingIdx);
    b->buildProgress = initialProgress;
    b->attackDamage = 0;

    int idxClamp = std::clamp(buildingIdx, 0, 14);
    b->buildWidth = s_widths[idxClamp];
    b->buildHeight = s_heights[idxClamp];

    const BuildingConfig *bcfg = GetBuildingConfig(buildingIdx);
    if (bcfg)
    {
        b->type = bcfg->type;
        b->maxHp = bcfg->maxHp;
        b->sightRadius = bcfg->sightRadius;
        b->radarRange = bcfg->radarRange;
        b->attackDamage = bcfg->attackDamage;
        b->attackRange = bcfg->attackRange;
        b->attackCooldown = bcfg->attackCooldown;
        b->attackCooldownMax = bcfg->attackCooldownMax;
        b->burstCount = bcfg->burstCount;
        b->burstDelay = bcfg->burstDelay;
        b->bulletSpeed = bcfg->bulletSpeed;
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
        switch (idxClamp)
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

    if (initialProgress < 1.0f)
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

bool EntityManager::CanPlaceBuilding(
    int tileX, int tileY, int width, int height, const TileMap &map, PlayerId player) const
{
    if (tileX < 0 || tileX + width > map.GetWidth() || tileY < 0 || tileY + height > map.GetHeight())
    {
        return false;
    }

    // 1. Fog of War check: building can only be placed on Visible tiles, NOT on Fog
    // (the same for every player, human or computer)
    if (map.IsFogEnabled())
    {
        const uint8_t team = Players::TeamOf(player);
        for (int ty = tileY; ty < tileY + height; ++ty)
        {
            for (int tx = tileX; tx < tileX + width; ++tx)
            {
                if (!map.IsVisibleTo(tx, ty, team))
                {
                    return false;
                }
            }
        }
    }

    // 2. Without a finished headquarters nothing is built (no building, no wall, no new headquarters); with one,
    //    the place must be within build_radius of ANY friendly finished Headquarters
    bool inHQRange = false;
    float buildCenterX = tileX + width * 0.5f;
    float buildCenterY = tileY + height * 0.5f;

    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner != player)
            continue;
        const Building *b = e.AsBuilding();
        // A headquarters still under construction opens no building zone yet
        if (b && b->type == UnitType::Headquarters && b->buildProgress >= 1.0f)
        {
            float hqCenterX = b->tileX + b->buildWidth * 0.5f;
            float hqCenterY = b->tileY + b->buildHeight * 0.5f;
            float dist = DetMath::Hypot(buildCenterX - hqCenterX, buildCenterY - hqCenterY);
            if (dist <= static_cast<float>(GetBuildRadius(player)))
            {
                inHQRange = true;
                break;
            }
        }
    }
    if (!inHQRange)
    {
        return false;
    }

    // 3. No construction under the enemy's nose: a visible enemy unit or building near the footprint blocks it
    const int block = map.GetConstructionRules().enemyBlockTiles;
    if (block > 0)
    {
        for (const auto &e : m_entities)
        {
            if (!e.alive || e.state == ActionState::Die || e.owner >= kMaxPlayers || !Players::Hostile(e.owner, player))
                continue;
            int ex0 = e.tileX, ey0 = e.tileY, ex1 = e.tileX, ey1 = e.tileY;
            if (const Building *b = e.AsBuilding())
            {
                ex1 = b->tileX + b->buildWidth - 1;
                ey1 = b->tileY + b->buildHeight - 1;
            }
            const int gapX = std::max({0, ex0 - (tileX + width - 1), tileX - ex1});
            const int gapY = std::max({0, ey0 - (tileY + height - 1), tileY - ey1});
            if (std::max(gapX, gapY) > block)
                continue;
            if (IsEntityVisibleToPlayer(e, player, map))
                return false;
        }
    }

    for (int ty = tileY; ty < tileY + height; ++ty)
    {
        for (int tx = tileX; tx < tileX + width; ++tx)
        {
            if (!map.IsPassable(tx, ty))
                return false;
            if (map.GetWall(tx, ty) > 0)
                return false;
            for (const auto &e : m_entities)
            {
                if (!e.alive || e.state == ActionState::Die)
                    continue;
                if (const Building *b = e.AsBuilding())
                {
                    if (tx >= b->tileX && tx < b->tileX + b->buildWidth && ty >= b->tileY
                        && ty < b->tileY + b->buildHeight)
                    {
                        return false;
                    }
                }
                else
                {
                    if (e.tileX == tx && e.tileY == ty)
                    {
                        return false;
                    }
                }
            }
        }
    }
    if (!Players::Slot(player).human)
        return KeepsBaseOpen(tileX, tileY, width, height, map, player);
    return true;
}

bool EntityManager::KeepsBaseOpen(
    int tileX, int tileY, int width, int height, const TileMap &map, PlayerId player) const
{
    const int W = map.GetWidth(), H = map.GetHeight();
    std::vector<uint8_t> blocked(static_cast<size_t>(W) * H, 0);
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            if (!map.IsPassable(x, y) || map.GetWall(x, y) > 0)
                blocked[static_cast<size_t>(y) * W + x] = 1;
        }
    }
    // Where the units of the player come out: the headquarters and every unit producer
    std::vector<std::pair<int, int>> anchors;
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding)
            continue;
        const Building *b = e.AsBuilding();
        if (!b)
            continue;
        for (int y = b->tileY; y < b->tileY + b->buildHeight; ++y)
        {
            for (int x = b->tileX; x < b->tileX + b->buildWidth; ++x)
            {
                if (x >= 0 && y >= 0 && x < W && y < H)
                    blocked[static_cast<size_t>(y) * W + x] = 1;
            }
        }
        if (b->owner == player && (b->type == UnitType::Headquarters || b->IsUnitProducer()))
        {
            int sx = 0, sy = 0;
            if (GetBuildingSpawnPoint(b->id, map, sx, sy) && sx >= 0 && sy >= 0 && sx < W && sy < H)
                anchors.push_back({sx, sy});
        }
    }
    if (anchors.empty())
        return true;
    const auto idx = [&](int x, int y) {
        return static_cast<size_t>(y) * W + x;
    };
    // A producer's exit stands on a free tile; the first anchor (the oldest building) is the one that is followed
    if (blocked[idx(anchors[0].first, anchors[0].second)])
        return true; // nothing to measure from

    auto flood = [&](const std::vector<uint8_t> &block, std::vector<uint8_t> &seen) {
        seen.assign(block.size(), 0);
        std::vector<int> stack {anchors[0].second * W + anchors[0].first};
        seen[stack[0]] = 1;
        int count = 0;
        while (!stack.empty())
        {
            const int cur = stack.back();
            stack.pop_back();
            ++count;
            const int x = cur % W, y = cur / W;
            const int nx[4] = {x + 1, x - 1, x, x}, ny[4] = {y, y, y + 1, y - 1};
            for (int k = 0; k < 4; ++k)
            {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= W || ny[k] >= H)
                    continue;
                const size_t n = idx(nx[k], ny[k]);
                if (block[n] || seen[n])
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
    for (int y = tileY; y < tileY + height; ++y)
    {
        for (int x = tileX; x < tileX + width; ++x)
        {
            if (x < 0 || y < 0 || x >= W || y >= H)
                continue;
            if (before[idx(x, y)])
                ++footprintReached;
            withNew[idx(x, y)] = 1;
        }
    }
    if (withNew[idx(anchors[0].first, anchors[0].second)])
        return false;
    const int reachAfter = flood(withNew, after);

    // A few tiles may end up in dead corners behind the building; a whole area cut off is a closed base
    constexpr int kDeadCornerTiles = 6;
    if (reachBefore - reachAfter - footprintReached > kDeadCornerTiles)
        return false;
    for (const auto &a : anchors)
    {
        if (before[idx(a.first, a.second)] && !after[idx(a.first, a.second)])
            return false;
    }
    // The new building has a free, reachable side of its own (its exit)
    bool hasExit = false;
    for (int x = tileX - 1; x <= tileX + width && !hasExit; ++x)
    {
        for (int y = tileY - 1; y <= tileY + height && !hasExit; ++y)
        {
            const bool inside = x >= tileX && x < tileX + width && y >= tileY && y < tileY + height;
            if (inside || x < 0 || y < 0 || x >= W || y >= H)
                continue;
            hasExit = after[idx(x, y)] != 0;
        }
    }
    return hasExit;
}

bool EntityManager::HasBuildingType(UnitType type, PlayerId owner, bool mustBeComplete) const
{
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner != owner)
            continue;
        const Building *b = e.AsBuilding();
        if (mustBeComplete && b && b->buildProgress < 1.0f)
            continue;
        if (e.type == type)
            return true;
    }
    return false;
}

uint16_t EntityManager::InternSound(const std::string &name)
{
    if (name.empty())
        return 0;
    for (size_t i = 0; i < m_soundNames.size(); ++i)
        if (m_soundNames[i] == name)
            return static_cast<uint16_t>(i + 1);
    if (m_soundNames.size() >= 0xFFFE)
        return 0;
    m_soundNames.push_back(name);
    return static_cast<uint16_t>(m_soundNames.size());
}

const WeaponConfig *EntityManager::FiringWeapon(const Entity &e) const
{
    const std::vector<WeaponConfig> *list = nullptr;
    size_t slot = 0;
    if (const Unit *u = e.AsUnit())
    {
        if (const UnitConfig *cfg = GetUnitConfig(u->archetypeId))
            list = &cfg->weapons;
        slot = u->firingSecondary ? 1 : 0;
    }
    else if (const Building *b = e.AsBuilding())
    {
        if (const BuildingConfig *cfg = GetBuildingConfig(b->rawBuildingType))
            list = &cfg->weapons;
    }
    return list && slot < list->size() ? &(*list)[slot] : nullptr;
}

const CharacterSounds *EntityManager::SoundsOf(const Entity &e) const
{
    if (const Unit *u = e.AsUnit())
    {
        const UnitConfig *cfg = GetUnitConfig(u->archetypeId);
        return cfg ? &cfg->sounds : nullptr;
    }
    if (const Building *b = e.AsBuilding())
    {
        const BuildingConfig *cfg = GetBuildingConfig(b->rawBuildingType);
        return cfg ? &cfg->sounds : nullptr;
    }
    return nullptr;
}

int EntityManager::BuildingPower(int buildingIndex) const
{
    const BuildingConfig *cfg = GetBuildingConfig(buildingIndex);
    return cfg ? cfg->powerCost : Building::DefaultPowerOutput(buildingIndex);
}

const CharacterSounds *EntityManager::WallSoundsOf(Faction faction) const
{
    const BuildingConfig *cfg = GetBuildingConfig(faction == Faction::Resistance ? 25 : 15);
    return cfg ? &cfg->sounds : nullptr;
}

int EntityManager::CountBuildingType(UnitType type, PlayerId owner, bool mustBeComplete) const
{
    int n = 0;
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner != owner || e.type != type)
            continue;
        const Building *b = e.AsBuilding();
        if (!mustBeComplete || !b || b->buildProgress >= 1.0f)
            ++n;
    }
    return n;
}

std::vector<int> EntityManager::GetBuildingsByType(UnitType type, PlayerId owner, bool mustBeComplete) const
{
    std::vector<int> result;
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner != owner)
            continue;
        const Building *b = e.AsBuilding();
        if (mustBeComplete && b && b->buildProgress < 1.0f)
            continue;
        if (e.type == type)
            result.push_back(e.id);
    }
    return result;
}

bool EntityManager::GetBuildingSpawnPoint(int buildingId, const TileMap &map, int &outTileX, int &outTileY) const
{
    const Building *b = GetBuilding(buildingId);
    if (!b)
        return false;

    auto isFreeSpawnTile = [&](int x, int y) -> bool {
        if (!map.IsPassable(x, y))
            return false;
        if (map.GetWall(x, y) > 0)
            return false;
        if (IsTileOccupiedByUnit(x, y))
            return false;
        for (const auto &e : m_entities)
        {
            if (!e.alive || e.state == ActionState::Die || !e.isBuilding)
                continue;
            const Building *eb = e.AsBuilding();
            if (eb && x >= eb->tileX && x < eb->tileX + eb->buildWidth && y >= eb->tileY
                && y < eb->tileY + eb->buildHeight)
            {
                return false;
            }
        }
        return true;
    };

    // 1. Bottom side
    for (int x = b->tileX; x < b->tileX + b->buildWidth; ++x)
    {
        int y = b->tileY + b->buildHeight;
        if (isFreeSpawnTile(x, y))
        {
            outTileX = x;
            outTileY = y;
            return true;
        }
    }
    // 2. Right side
    for (int y = b->tileY; y < b->tileY + b->buildHeight; ++y)
    {
        int x = b->tileX + b->buildWidth;
        if (isFreeSpawnTile(x, y))
        {
            outTileX = x;
            outTileY = y;
            return true;
        }
    }
    // 3. Left side
    for (int y = b->tileY; y < b->tileY + b->buildHeight; ++y)
    {
        int x = b->tileX - 1;
        if (isFreeSpawnTile(x, y))
        {
            outTileX = x;
            outTileY = y;
            return true;
        }
    }
    // 4. Top side
    for (int x = b->tileX; x < b->tileX + b->buildWidth; ++x)
    {
        int y = b->tileY - 1;
        if (isFreeSpawnTile(x, y))
        {
            outTileX = x;
            outTileY = y;
            return true;
        }
    }
    // 5. Fallback: search in radius 2..5
    for (int r = 2; r <= 5; ++r)
    {
        for (int dy = -r; dy <= b->buildHeight + r; ++dy)
        {
            for (int dx = -r; dx <= b->buildWidth + r; ++dx)
            {
                int tx = b->tileX + dx;
                int ty = b->tileY + dy;
                if (isFreeSpawnTile(tx, ty))
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

bool EntityManager::IsTileOccupiedByUnit(int tileX, int tileY, int ignoreEntityId) const
{
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || e.isBuilding)
            continue;
        if (e.id == ignoreEntityId)
            continue;
        if (e.OccupiesTile(tileX, tileY))
            return true;
    }
    return false;
}

int EntityManager::GetUnitIdAtTile(int tileX, int tileY, int ignoreEntityId) const
{
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || e.isBuilding)
            continue;
        if (e.id == ignoreEntityId)
            continue;
        if (e.OccupiesTile(tileX, tileY))
            return e.id;
    }
    return 0;
}

void EntityManager::ResolveUnitStacking(TileMap &map)
{
    std::vector<Unit *> livingUnits;
    for (auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || e.isBuilding)
            continue;
        if (Unit *u = e.AsUnit())
        {
            livingUnits.push_back(u);
        }
    }

    for (size_t i = 0; i < livingUnits.size(); ++i)
    {
        Unit *u1 = livingUnits[i];
        for (size_t j = i + 1; j < livingUnits.size(); ++j)
        {
            Unit *u2 = livingUnits[j];
            if (u1->tileX == u2->tileX && u1->tileY == u2->tileY)
            {
                // Two units occupy the exact same tile: push u2 to the nearest free, passable, unblocked tile
                static constexpr int offsets[8][2]
                    = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
                for (const auto &off : offsets)
                {
                    int nx = u2->tileX + off[0];
                    int ny = u2->tileY + off[1];
                    if (nx >= 0 && nx < map.GetWidth() && ny >= 0 && ny < map.GetHeight())
                    {
                        if (map.IsPassable(nx, ny) && map.GetWall(nx, ny) == 0 && !IsTileOccupiedByUnit(nx, ny, u2->id))
                        {
                            u2->tileX = nx;
                            u2->tileY = ny;
                            u2->subX = 0.0f;
                            u2->subY = 0.0f;
                            u2->ReleaseReservation();
                            if (u2->state == ActionState::Move)
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

const std::vector<BuildingBlueprint> &EntityManager::GetBlueprints()
{
    if (!s_blueprints.empty())
    {
        return s_blueprints;
    }
    static const std::vector<BuildingBlueprint> defaultBlueprints
        = {{0,
            8,
            2074,
            UnitType::Headquarters,
            "HEADQUARTERS",
            3,
            4,
            800,
            50,
            "Resistance Command Center. Provides radar and logistics."},
           {1,
            9,
            2075,
            UnitType::PowerPlant,
            "POWER PLANT",
            3,
            3,
            300,
            100,
            "Power Plant (+100 kW). Supplies energy to factories and defenses."},
           {2,
            10,
            2076,
            UnitType::Barracks,
            "BARRACKS",
            3,
            3,
            400,
            -20,
            "Infantry Training: Rifleman, Grenadier, Sniper."},
           {3,
            11,
            2077,
            UnitType::Factory,
            "FACTORY",
            3,
            3,
            600,
            -40,
            "Vehicle Factory: Coyote, Armadillo, Rhino, and Porcupine MLRS."},
           {4,
            12,
            2078,
            UnitType::ResearchLab,
            "RESEARCH LAB",
            3,
            3,
            500,
            -30,
            "Scientific center for combat research upgrades (RESEARCH tab)."},
           {5,
            13,
            2079,
            UnitType::Bunker,
            "BUNKER",
            2,
            2,
            350,
            -15,
            "Reinforced machine-gun bunker for perimeter defense."},
           {6,
            14,
            2080,
            UnitType::Tower,
            "TURRET",
            2,
            2,
            250,
            -10,
            "Watchtower with heavy machine gun and extended vision."},
           {7,
            25,
            2081,
            UnitType::Wall,
            "WALL",
            1,
            1,
            50,
            0,
            "Reinforced concrete wall (1x1) to block enemy armor.",
            true}};
    return defaultBlueprints;
}

static float ComputeDistanceToEntityPixels(const Unit &attacker, const Entity &target)
{
    float srcX = attacker.GetUnitCenterPixelX();
    float srcY = attacker.GetUnitCenterPixelY();

    if (target.isBuilding)
    {
        const Building *b = target.AsBuilding();
        int w = b ? std::max(1, b->buildWidth) : 1;
        int h = b ? std::max(1, b->buildHeight) : 1;

        float minCenterX = target.tileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
        float maxCenterX = (target.tileX + w - 1) * TILE_WIDTH + (TILE_WIDTH * 0.5f);
        float minCenterY = target.tileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
        float maxCenterY = (target.tileY + h - 1) * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);

        float closestX = std::clamp(srcX, minCenterX, maxCenterX);
        float closestY = std::clamp(srcY, minCenterY, maxCenterY);
        return DetMath::Hypot(closestX - srcX, closestY - srcY);
    }

    float dstX = target.GetUnitCenterPixelX();
    float dstY = target.GetUnitCenterPixelY();
    return DetMath::Hypot(dstX - srcX, dstY - srcY);
}

void EntityManager::NearestOpenTile(const TileMap &map, int &tileX, int &tileY)
{
    const int w = map.GetWidth(), h = map.GetHeight();
    if (w <= 0 || h <= 0)
        return;
    tileX = std::clamp(tileX, 0, w - 1);
    tileY = std::clamp(tileY, 0, h - 1);
    auto open = [&](int x, int y) { return map.IsPassable(x, y) && map.GetWall(x, y) == 0; };
    if (open(tileX, tileY))
        return;
    int bestX = tileX, bestY = tileY;
    float best = 1e9f;
    for (int r = 1; r <= 16 && best >= 1e9f; ++r)
    {
        for (int y = tileY - r; y <= tileY + r; ++y)
        {
            for (int x = tileX - r; x <= tileX + r; ++x)
            {
                if (std::max(std::abs(x - tileX), std::abs(y - tileY)) != r || x < 0 || y < 0 || x >= w || y >= h
                    || !open(x, y))
                    continue;
                const float d = static_cast<float>((x - tileX) * (x - tileX) + (y - tileY) * (y - tileY));
                if (d < best)
                {
                    best = d;
                    bestX = x;
                    bestY = y;
                }
            }
        }
    }
    tileX = bestX;
    tileY = bestY;
}

void EntityManager::IssueMoveOrder(
    const std::vector<int> &unitIds, int targetTileX, int targetTileY, Pathfinder &pathfinder, bool attackMove)
{
    pathfinder.SetEntityManager(this);
    const TileMap *map = pathfinder.GetMap();
    if (!map || unitIds.empty())
        return;

    // A click on the edge of the map (or on ground nobody can stand on) sends the units to the nearest place they can
    NearestOpenTile(*map, targetTileX, targetTileY);
    const int mapW = map->GetWidth();
    const int mapH = map->GetHeight();

    std::vector<int> validUnits;
    float sumUnitX = 0.0f, sumUnitY = 0.0f;
    for (int uid : unitIds)
    {
        Unit *u = GetUnit(uid);
        if (!u || !u->alive || u->state == ActionState::Die || u->state == ActionState::Transform)
            continue;
        if (u->isSiegeMode || u->moveSpeed <= 0.0f)
        {
            u->ClearMoveTarget();
            u->ClearPathState();
            // Attack-move [Q] to a deployed unit: it keeps firing while it sees enemies, then packs up and follows
            const UnitConfig *cfg = GetUnitConfig(u->archetypeId);
            if (attackMove && u->isSiegeMode && cfg && cfg->parentArchetypeId >= 0)
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
    if (validUnits.empty())
        return;

    float unitCentroidX = sumUnitX / static_cast<float>(validUnits.size());
    float unitCentroidY = sumUnitY / static_cast<float>(validUnits.size());

    std::vector<StormcastSlot> slots;
    if (pathfinder.GetMap())
    {
        slots = AINavigataionStormcastView::ComputeGroupSlots(
            targetTileX, targetTileY, static_cast<int>(validUnits.size()), *pathfinder.GetMap(), this, validUnits);
    }

    // Match each unit to the slot that preserves its relative formation offset from the squad centroid
    // (Prevents units from crossing paths and colliding with each other!)
    if (slots.size() == validUnits.size() && validUnits.size() > 1)
    {
        float sumSlotX = 0.0f, sumSlotY = 0.0f;
        for (const auto &s : slots)
        {
            sumSlotX += static_cast<float>(s.x);
            sumSlotY += static_cast<float>(s.y);
        }
        float slotCentroidX = sumSlotX / static_cast<float>(slots.size());
        float slotCentroidY = sumSlotY / static_cast<float>(slots.size());

        std::vector<StormcastSlot> matchedSlots(validUnits.size());
        std::vector<bool> slotUsed(slots.size(), false);

        for (size_t i = 0; i < validUnits.size(); ++i)
        {
            const Unit *u = GetUnit(validUnits[i]);
            float relX = u ? (static_cast<float>(u->tileX) - unitCentroidX) : 0.0f;
            float relY = u ? (static_cast<float>(u->tileY) - unitCentroidY) : 0.0f;
            relX = std::clamp(relX, -4.0f, 4.0f);
            relY = std::clamp(relY, -4.0f, 4.0f);
            float idealX = slotCentroidX + relX;
            float idealY = slotCentroidY + relY;

            size_t bestIdx = 0;
            float bestDistSq = 1e18f;
            for (size_t k = 0; k < slots.size(); ++k)
            {
                if (slotUsed[k])
                    continue;
                float dx = static_cast<float>(slots[k].x) - idealX;
                float dy = static_cast<float>(slots[k].y) - idealY;
                float dsq = dx * dx + dy * dy;
                if (dsq < bestDistSq)
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
    for (int uid : validUnits)
    {
        if (Unit *u = GetUnit(uid))
        {
            u->isAttackMoving = attackMove;
            u->state = ActionState::Move;
        }
    }

    std::vector<std::pair<int, int>> assignedEndTiles;
    for (size_t i = 0; i < validUnits.size(); ++i)
    {
        int uid = validUnits[i];
        Unit *u = GetUnit(uid);
        if (!u)
            continue;

        int destX = (i < slots.size()) ? std::clamp(slots[i].x, 0, mapW - 1) : targetTileX;
        int destY = (i < slots.size()) ? std::clamp(slots[i].y, 0, mapH - 1) : targetTileY;
        NearestOpenTile(*map, destX, destY); // (formation slots at the edge / on blocked ground too)

        u->targetEntityId = 0;
        u->targetWallTileX = -1;
        u->targetWallTileY = -1;
        u->isManualWallTarget = false;
        u->isDirectEngaged = false;
        u->isSupportAssisting = false;
        u->isAttackMoving = attackMove;
        // Several units ordered together may breach hostile walls instead of queueing at one gap
        u->groupOrder = validUnits.size() >= 2;
        u->holdPosition = false;
        u->orderedTargetId = 0;
        u->blockedTimer = 0.0f;

        if (u->tileX == destX && u->tileY == destY && std::abs(u->subX) <= 0.001f && std::abs(u->subY) <= 0.001f)
        {
            u->ClearMoveTarget();
            u->ClearPathState();
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
        if (pathfinder.FindPath(u->tileX, u->tileY, destX, destY, u->path, u->id, &isPartial) && !u->path.empty())
        {
            if (isPartial && !u->path.empty())
            {
                int endX = u->path.back().x;
                int endY = u->path.back().y;
                for (const auto &pt : assignedEndTiles)
                {
                    if (pt.first == endX && pt.second == endY)
                    {
                        while (u->path.size() > 1)
                        {
                            u->path.pop_back();
                            endX = u->path.back().x;
                            endY = u->path.back().y;
                            bool clash = false;
                            for (const auto &p2 : assignedEndTiles)
                            {
                                if (p2.first == endX && p2.second == endY)
                                {
                                    clash = true;
                                    break;
                                }
                            }
                            if (!clash)
                                break;
                        }
                        break;
                    }
                }
            }
            if (!u->path.empty())
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
            u->ClearPathState();
            u->state = ActionState::Idle;
            u->waitingForPath = true;
            u->pathRetryTimer = 0.0f;
        }
    }
}

void EntityManager::IssueAttackMoveOrder(
    const std::vector<int> &unitIds, int targetTileX, int targetTileY, Pathfinder &pathfinder)
{
    IssueMoveOrder(unitIds, targetTileX, targetTileY, pathfinder, true);
}

void EntityManager::IssueAttackOrder(const std::vector<int> &unitIds, int targetEntityId, Pathfinder *pathfinder)
{
    Entity *target = GetEntity(targetEntityId);
    if (!target || !target->alive || target->state == ActionState::Die)
        return;

    if (pathfinder)
    {
        pathfinder->SetEntityManager(this);
    }

    for (int uid : unitIds)
    {
        Unit *u = GetUnit(uid);
        if (!u || !u->alive || u->state == ActionState::Die || u->state == ActionState::Transform
            || u->id == targetEntityId)
            continue;
        if (!Players::Hostile(u->owner, target->owner))
            continue;
        u->holdPosition = false; // an explicit attack order ends a hold
        u->orderedTargetId = targetEntityId;

        // 1. If already targeting this exact entity, do NOT reset state or animation frame!
        if (u->targetEntityId == targetEntityId && !u->isManualWallTarget)
        {
            float dist = ComputeDistanceToEntityPixels(*u, *target);
            if (dist <= u->attackRange * TILE_WIDTH)
            {
                continue; // Already engaged in range; let updateCombat manage cooldown & firing anim
            }
        }

        // 2. Compute distance to target (footprint-aware for multi-tile buildings)
        float dist = ComputeDistanceToEntityPixels(*u, *target);
        bool inRange = (dist <= u->attackRange * TILE_WIDTH);

        // 3. Siege Mode / Immobile Unit Strict Gate:
        // Cannot move to pursue targets out of range. Reject out-of-range orders cleanly!
        if (u->isSiegeMode || u->moveSpeed <= 0.0f)
        {
            if (!inRange)
            {
                u->targetEntityId = 0;
                u->isDirectEngaged = false;
                u->ClearMoveTarget();
                u->ClearPathState();
                if (u->state == ActionState::Attack && u->attackAnimTimer <= 0.0f)
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
            u->ClearMoveTarget();
            u->ClearPathState();
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
        u->ClearMoveTarget();

        if (inRange)
        {
            u->ClearPathState();
            u->state = ActionState::Attack;
        }
        else if (pathfinder)
        {
            bool isPartial = false;
            if (u->FindPathToAttack(*target, *pathfinder, &isPartial))
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

void EntityManager::IssueAttackWallOrder(
    const std::vector<int> &unitIds, int wallTileX, int wallTileY, Pathfinder &pathfinder)
{
    pathfinder.SetEntityManager(this);
    const TileMap *map = pathfinder.GetMap();
    if (!map || map->GetWall(wallTileX, wallTileY) == 0)
        return;

    for (int uid : unitIds)
    {
        Unit *u = GetUnit(uid);
        if (!u || !u->alive || u->state == ActionState::Die || u->state == ActionState::Transform
            || u->attackDamage <= 0)
            continue;
        u->holdPosition = false;

        float srcX = u->GetUnitCenterPixelX();
        float srcY = u->GetUnitCenterPixelY();
        float dstX = wallTileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
        float dstY = wallTileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
        float dist = DetMath::Hypot(dstX - srcX, dstY - srcY);
        bool inRange = (dist <= u->attackRange * TILE_WIDTH);

        if (u->isSiegeMode || u->moveSpeed <= 0.0f)
        {
            if (!inRange)
            {
                // Reject out-of-range wall target for immobile siege unit
                u->targetWallTileX = -1;
                u->targetWallTileY = -1;
                u->isManualWallTarget = false;
                u->targetEntityId = 0;
                if (u->state == ActionState::Attack && u->attackAnimTimer <= 0.0f)
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
            u->ClearMoveTarget();
            u->ClearPathState();
            continue;
        }

        u->targetEntityId = 0;
        u->targetWallTileX = wallTileX;
        u->targetWallTileY = wallTileY;
        u->isManualWallTarget = true;
        u->isAttackMoving = false;
        u->isSupportAssisting = false;
        u->ClearMoveTarget();

        if (inRange)
        {
            u->ClearPathState();
            u->state = ActionState::Attack;
        }
        else
        {
            bool isPartial = false;
            if (pathfinder.FindPath(u->tileX, u->tileY, wallTileX, wallTileY, u->path, u->id, &isPartial))
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

int EntityManager::BaseBuildRadius(PlayerId player) const
{
    const Faction f = player < kMaxPlayers ? Players::Slot(player).faction : Faction::Resistance;
    return m_factionBuildRadius[f == Faction::Confederation ? 1 : 0];
}

int EntityManager::GetBuildRadius(PlayerId player) const
{
    return BaseBuildRadius(player) + (player < kMaxPlayers ? m_buildRadiusExtra[player] : 0);
}

void EntityManager::SetBuildRadiusPercent(PlayerId player, int percent)
{
    if (player >= kMaxPlayers || !Players::Slot(player).human)
        return;
    const int base = BaseBuildRadius(player);
    m_buildRadiusExtra[player] = base * (100 + percent) / 100 - base;
}

bool EntityManager::CanDeploySiege(const Unit &u) const
{
    const UnitConfig *cfg = GetUnitConfig(u.archetypeId);
    if (cfg && cfg->isSiegeMode)
        return true; // packing up is always possible
    if (cfg && cfg->siegeRequiresResearch && !IsSiegeUnlocked(u.owner))
        return false;
    return true;
}

void EntityManager::UpdateAutoSiege(Unit &u, const TileMap &map, float deltaTime)
{
    if (u.state == ActionState::Die || u.state == ActionState::Transform)
        return;
    const UnitConfig *cfg = GetUnitConfig(u.archetypeId);
    if (!cfg)
        return;

    // Who may decide by itself: the AI always, the player's units only while on an attack-move order
    const bool aiSide = !Players::Slot(u.owner).human; // a computer player (the same on every peer)
    auto closestVisibleEnemy = [&](float maxTiles) {
        float best = maxTiles * TILE_WIDTH;
        int bestId = 0;
        for (const auto &other : m_entities)
        {
            if (!other.alive || other.state == ActionState::Die || !Players::Hostile(other.owner, u.owner))
                continue;
            if (!IsEntityVisibleToPlayer(other, u.owner, map))
                continue; // fog / camouflage do not count
            const float d = ComputeDistanceToEntityPixels(u, other);
            if (d <= best)
            {
                best = d;
                bestId = other.id;
            }
        }
        return bestId;
    };

    if (!cfg->isSiegeMode)
    {
        u.siegeQuietTimer = 0.0f;
        if (cfg->siegeArchetypeId < 0 || cfg->autoSiege == UnitConfig::AutoSiege::Off || !CanDeploySiege(u)
            || u.autoSiegeCooldownTimer > 0.0f)
            return;
        if (!aiSide && !u.isAttackMoving)
            return;
        const UnitConfig *siege = GetUnitConfig(cfg->siegeArchetypeId);
        if (!siege)
            return;
        // Both forms with the faction's range technologies (Hornet-10 alone already gives the sniper 9.5)
        const float siegeRange = ResearchedAttackRange(u.owner, siege->archetypeId, siege->attackRange);
        if (cfg->autoSiege == UnitConfig::AutoSiege::Far)
        {
            // Only worth it for the farther reach: an enemy beyond the normal range, within the siege range
            if (siegeRange <= u.attackRange + 0.25f)
                return;
            // Something to shoot in normal range: no need to deploy
            if (u.targetEntityId != 0)
            {
                const Entity *t = GetEntity(u.targetEntityId);
                if (t && ComputeDistanceToEntityPixels(u, *t) <= u.attackRange * TILE_WIDTH)
                    return;
            }
            if (closestVisibleEnemy(u.attackRange) != 0)
                return;
        }
        // Always: the siege form is worth having whatever the range (a salvo, concealment), so any enemy in sight
        // within the siege range deploys it
        if (closestVisibleEnemy(siegeRange) == 0)
            return;
        // The order stays: once nothing is left to shoot the unit packs up and carries on toward it
        const int keepX = u.targetTileX, keepY = u.targetTileY;
        const bool keepAttackMove = u.isAttackMoving;
        ToggleSiegeMode({u.id});
        u.targetTileX = keepX;
        u.targetTileY = keepY;
        u.isAttackMoving = keepAttackMove;
        u.autoDeployed = true;
        return;
    }

    // Auto-deployed and nothing visible within siege range for a while: pack up and carry on
    if (!u.autoDeployed)
        return;
    // (u.attackRange is the siege form's: it stays deployed while an enemy is within the siege range)
    if (closestVisibleEnemy(u.attackRange) != 0)
    {
        u.siegeQuietTimer = 0.0f;
        return;
    }
    u.siegeQuietTimer += deltaTime;
    if (u.siegeQuietTimer >= 4.0f)
    {
        u.siegeQuietTimer = 0.0f;
        u.autoDeployed = false;
        u.autoSiegeCooldownTimer = 3.5f;
        ToggleSiegeMode({u.id});
    }
}

void EntityManager::ToggleSiegeMode(const std::vector<int> &unitIds)
{
    for (int uid : unitIds)
    {
        Unit *u = GetUnit(uid);
        if (!u || u->state == ActionState::Die || u->state == ActionState::Transform)
            continue;
        if (!CanDeploySiege(*u))
            continue;            // technology not researched yet
        u->autoDeployed = false; // a manual [G] keeps the unit where the player put it

        int newArch = -1;
        const UnitConfig *curCfg = GetUnitConfig(u->archetypeId);
        if (curCfg && curCfg->isSiegeMode && curCfg->parentArchetypeId >= 0)
        {
            newArch = curCfg->parentArchetypeId;
        }
        else if (curCfg && curCfg->siegeArchetypeId >= 0)
        {
            newArch = curCfg->siegeArchetypeId;
        }
        else
        {
            switch (u->archetypeId)
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
        if (newArch >= 0)
        {
            // Settle any active tile reservation before transforming so the unit doesn't glide or lock tiles
            if (u->HasReservation())
            {
                if (std::abs(u->subX) * 2.0f > TILE_WIDTH || std::abs(u->subY) * 2.0f > TILE_HEIGHT)
                {
                    u->tileX = u->resTileX;
                    u->tileY = u->resTileY;
                }
                u->subX = 0.0f;
                u->subY = 0.0f;
                u->ReleaseReservation();
            }

            // Deployed by hand: the unit stays where the player put it
            if (!(curCfg && curCfg->isSiegeMode))
            {
                u->ClearMoveTarget();
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

void EntityManager::TurretShift(int archetypeId, int facingDirection, float &outShiftX, float &outShiftY)
{
    outShiftX = 0.0f;
    outShiftY = 0.0f;
    // Autogenerated deterministic turret shift table [archetype][facing 0..7] relative to reference facing 2 (South)
    static constexpr float kTurretShifts[20][8][2] = {
        /*  0 */ {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, -1.0f}},
        /*  1 */ {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, -1.0f}},
        /*  2 */ {{-2.0f, 0.0f}, {-1.0f, 1.0f}, {0.0f, 0.0f}, {-1.0f, 0.0f}, {-3.0f, 0.0f}, {-2.0f, 0.0f}, {-1.0f, -1.0f}, {0.0f, -1.0f}},
        /*  3 */ {{-2.0f, 0.0f}, {-1.0f, 1.0f}, {0.0f, 0.0f}, {-1.0f, 0.0f}, {-3.0f, 0.0f}, {-2.0f, 0.0f}, {-1.0f, -1.0f}, {0.0f, -1.0f}},
        /*  4 */ {{-2.0f, 0.0f}, {-2.0f, 0.0f}, {0.0f, 0.0f}, {-2.0f, 0.0f}, {-3.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 0.0f}, {-1.0f, 1.0f}},
        /*  5 */ {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}},
        /*  6 */ {{-6.0f, 3.0f}, {-3.0f, 1.0f}, {0.0f, 0.0f}, {4.0f, 1.0f}, {6.0f, 3.0f}, {6.0f, 6.0f}, {0.0f, 9.0f}, {-5.0f, 7.0f}},
        /*  7 */ {{-6.0f, 3.0f}, {-3.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}, {4.0f, 3.0f}, {1.0f, 4.0f}, {0.0f, 6.0f}, {-5.0f, 5.0f}},
        /*  8 */ {{-7.0f, 4.0f}, {-5.0f, 0.0f}, {0.0f, 0.0f}, {5.0f, 1.0f}, {5.0f, 4.0f}, {5.0f, 6.0f}, {0.0f, 9.0f}, {-5.0f, 7.0f}},
        /*  9 */ {{-2.0f, 0.0f}, {-2.0f, -1.0f}, {0.0f, 0.0f}, {2.0f, -1.0f}, {3.0f, 0.0f}, {1.0f, 3.0f}, {0.0f, 3.0f}, {-2.0f, 3.0f}},
        /* 10 */ {{-2.0f, 2.0f}, {-2.0f, 1.0f}, {0.0f, 0.0f}, {2.0f, 1.0f}, {4.0f, 2.0f}, {3.0f, 4.0f}, {0.0f, 7.0f}, {-2.0f, 4.0f}},
        /* 11 */ {{-6.0f, 3.0f}, {-5.0f, 0.0f}, {0.0f, 0.0f}, {4.0f, 0.0f}, {6.0f, 3.0f}, {3.0f, 6.0f}, {0.0f, 6.0f}, {-4.0f, 6.0f}},
        /* 12 */ {{-6.0f, 3.0f}, {-3.0f, 1.0f}, {0.0f, 0.0f}, {2.0f, 1.0f}, {5.0f, 2.0f}, {4.0f, 5.0f}, {-1.0f, 7.0f}, {-3.0f, 5.0f}},
        /* 13 */ {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}},
        /* 14 */ {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}},
        /* 15 */ {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}},
        /* 16 */ {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}},
        /* 17 */ {{-6.0f, 3.0f}, {-5.0f, 0.0f}, {0.0f, 0.0f}, {4.0f, 0.0f}, {6.0f, 3.0f}, {3.0f, 6.0f}, {0.0f, 6.0f}, {-4.0f, 6.0f}},
        /* 18 */ {{-6.0f, 3.0f}, {-3.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}, {4.0f, 3.0f}, {1.0f, 4.0f}, {0.0f, 6.0f}, {-5.0f, 5.0f}},
        /* 19 */ {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}},
    };
    if (archetypeId >= 0 && archetypeId < 20)
    {
        const int f = (facingDirection % 8 + 8) % 8;
        outShiftX = kTurretShifts[archetypeId][f][0];
        outShiftY = kTurretShifts[archetypeId][f][1];
    }
}

void EntityManager::UpdateTurretShift(Unit &u) const
{
    u.turretShiftX = u.turretShiftY = 0.0f;
    if (!u.hasRotatingTurret)
        return;
    // Deployed siege units (14..16) keep a single hull sprite; everything else has one per facing
    const bool fixedHull = (u.archetypeId >= 14 && u.archetypeId <= 16);
    float ax = 0.0f, ay = 0.0f, rx = 0.0f, ry = 0.0f;
    if (m_texManager
        && m_texManager->GetTurretAnchor(u.archetypeId, fixedHull ? 0 : u.facingDirection, ax, ay)
        && m_texManager->GetTurretAnchor(u.archetypeId, fixedHull ? 0 : 2, rx, ry))
    {
        u.turretShiftX = ax - rx;
        u.turretShiftY = ay - ry;
    }
    else
    {
        TurretShift(u.archetypeId, fixedHull ? 0 : u.facingDirection, u.turretShiftX, u.turretShiftY);
    }
}

void EntityManager::NoteHit(PlayerId victim, int attackerId)
{
    const Entity *attacker = GetEntity(attackerId);
    if (!attacker || victim >= kMaxPlayers || !Players::Hostile(attacker->owner, victim))
        return;
    for (auto &p : m_revealPings)
    {
        if (p.entityId == attackerId && p.observer == victim)
        {
            p.timeLeft = 2.5f; // keeps shooting: stays visible
            return;
        }
    }
    m_revealPings.push_back({attackerId, victim, 2.5f});
}

void EntityManager::NoteEntityHit(Entity &victim, int attackerId)
{
    NoteHit(victim.owner, attackerId);
    const Entity *attacker = GetEntity(attackerId);
    if (!attacker || !attacker->alive || attacker->state == ActionState::Die
        || !Players::Hostile(attacker->owner, victim.owner))
        return;
    victim.lastAttackerId = attackerId;
    victim.lastAttackedTimer = 6.0f;
}

void EntityManager::UpdateFogOfWar(TileMap &map)
{
    if (!map.IsFogEnabled())
        return;

    // Reset currently visible tiles to Explored (dimmed darkness shroud)
    map.ResetVisibility();

    // Enemies that just hit something of a side: exactly the tiles they occupy become visible to it
    for (const auto &p : m_revealPings)
    {
        const Entity *e = GetEntity(p.entityId);
        if (!e)
            continue;
        if (const Building *b = e->AsBuilding())
        {
            map.RevealRect(b->tileX, b->tileY, b->buildWidth, b->buildHeight, Players::TeamOf(p.observer));
        }
        else
        {
            map.RevealRect(e->tileX, e->tileY, 1, 1, Players::TeamOf(p.observer));
        }
    }

    // Every team sees around the living units and buildings of its players (the same on every peer)
    for (const auto &entity : m_entities)
    {
        if (!entity.alive || entity.state == ActionState::Die)
            continue;
        if (entity.owner >= kMaxPlayers)
            continue;
        const uint8_t team = Players::TeamOf(entity.owner);

        int sight = entity.sightRadius;
        if (sight <= 0)
            sight = 4;
        // A finished Radar Locator sees far beyond its sight radius
        if (const Building *rb = entity.AsBuilding(); rb && rb->radarRange > 0 && rb->buildProgress >= 1.0f)
            sight += rb->radarRange;

        if (const Building *b = entity.AsBuilding())
        {
            int cx = b->tileX + (b->buildWidth / 2);
            int cy = b->tileY + (b->buildHeight / 2);
            map.RevealArea(cx, cy, sight, team);
        }
        else if (const Unit *u = entity.AsUnit())
        {
            map.RevealArea(u->tileX, u->tileY, sight, team);
        }
    }
}

bool EntityManager::IsEntityVisibleToAi(const Entity &entity) const
{
    return IsEntityVisibleToAi(entity, Players::OwnerOf(AiFaction()));
}

bool EntityManager::IsEntityVisibleToAi(const Entity &entity, PlayerId observerPlayer) const
{
    float ex = entity.GetUnitCenterPixelX();
    float ey = entity.GetUnitCenterPixelY();
    for (const auto &observer : m_entities)
    {
        if (!observer.alive || observer.state == ActionState::Die || observer.owner != observerPlayer)
            continue;
        float ox = observer.GetUnitCenterPixelX();
        float oy = observer.GetUnitCenterPixelY();
        float sightPx = static_cast<float>(observer.sightRadius * TILE_WIDTH);
        float dx = ex - ox;
        float dy = ey - oy;
        if ((dx * dx + dy * dy) <= (sightPx * sightPx))
        {
            return true;
        }
    }
    return false;
}

bool EntityManager::IsEntityVisibleToPlayer(const Entity &entity, PlayerId observer, const TileMap &map) const
{
    if (!entity.alive || entity.state == ActionState::Die)
        return false;
    if (!map.IsFogEnabled())
        return true;
    if (!Players::Hostile(entity.owner, observer))
        return true;

    int w = 1;
    int h = 1;
    if (entity.isBuilding)
    {
        if (const Building *b = entity.AsBuilding())
        {
            w = std::max(1, b->buildWidth);
            h = std::max(1, b->buildHeight);
        }
    }

    if (observer >= kMaxPlayers)
        return true;
    if (Players::Slot(observer).human)
    {
        // Visible if ANY tile of the entity's WxH footprint is in the team's sight this tick
        const uint8_t team = Players::TeamOf(observer);
        for (int dy = 0; dy < h; ++dy)
        {
            for (int dx = 0; dx < w; ++dx)
            {
                if (map.IsVisibleTo(entity.tileX + dx, entity.tileY + dy, team))
                    return true;
            }
        }
        return false;
    }
    return IsEntityVisibleToAi(entity, observer);
}

void EntityManager::Update(float deltaTime, TileMap &map, Pathfinder &pathfinder)
{
    for (auto &p : m_revealPings)
        p.timeLeft -= deltaTime;
    m_revealPings.erase(
        std::remove_if(
            m_revealPings.begin(), m_revealPings.end(), [](const RevealPing &p) { return p.timeLeft <= 0.0f; }),
        m_revealPings.end());
    pathfinder.SetEntityManager(this);

    // Initial fog update for this tick
    UpdateFogOfWar(map);

    // Power balance of every player
    std::array<int, kMaxPlayers> prod {}, cons {};
    for (const auto &e : m_entities)
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->buildProgress < 1.0f || b->owner >= kMaxPlayers)
            continue;
        int pwr = b->GetPowerOutput();
        if (pwr > 0)
            prod[b->owner] += pwr;
        else
            cons[b->owner] += (-pwr);
    }
    std::array<bool, kMaxPlayers> powered {};
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        prod[p] += prod[p] * ResearchTotal(p, ResearchEffect::PowerBonus) / 100;
        powered[p] = m_unlimitedPower || cons[p] <= prod[p];
    }
    UpdateRegeneration(deltaTime);

    // Build Delegates for Units
    UnitDelegates uDelegates;
    uDelegates.getEntity = [this](int id) -> Entity * {
        return GetEntity(id);
    };
    uDelegates.spawnProjectile
        = [this](Entity &shooter, float srcX, float srcY, float dstX, float dstY, int targetId, TileMap &m) {
              SpawnProjectileFromEntity(shooter, srcX, srcY, dstX, dstY, targetId, m);
          };
    uDelegates.spawnExplosion = [this](float x, float y, int animId) {
        SpawnExplosion(x, y, animId);
    };
    uDelegates.isTileOccupied = [this](int tx, int ty, int ignoreId) {
        return IsTileOccupiedByUnit(tx, ty, ignoreId);
    };
    uDelegates.getUnitAtTile = [this](int tx, int ty, int ignoreId) {
        return GetUnitIdAtTile(tx, ty, ignoreId);
    };
    uDelegates.onKill = [this](PlayerId victim, PlayerId killer) {
        NoteKill(victim, killer);
    };
    uDelegates.isFortressFlamethrowerUnlocked = [this](PlayerId p) {
        return IsFortressFlamethrowerUnlocked(p);
    };
    uDelegates.getUnitConfig = [this](int archId) {
        return GetUnitConfig(archId);
    };
    uDelegates.applyResearch = [](Unit &u) {
        ApplyCompletedResearch(u);
    };
    uDelegates.isEntityVisible = [this, &map](const Entity &entity, PlayerId observer) {
        return IsEntityVisibleToPlayer(entity, observer, map);
    };
    // Both pick by priority first (armed units, then other units and armed buildings, then the rest), then by
    // distance; m_entities is a vector, so the order (and the result) is the same on every peer
    auto bestTargetWithin = [this, &map](const Unit &u, float rangePixels) -> int {
        int bestId = 0;
        int bestTier = 3;
        float bestDist = rangePixels;
        for (const auto &other : m_entities)
        {
            if (!other.alive || other.state == ActionState::Die || !Players::Hostile(other.owner, u.owner))
                continue;
            const float d = ComputeDistanceToEntityPixels(u, other);
            if (d > rangePixels)
                continue;
            const int tier = TargetPriorityTier(other);
            if (tier > bestTier || (tier == bestTier && d > bestDist))
                continue;
            if (!IsEntityVisibleToPlayer(other, u.owner, map))
                continue;
            bestTier = tier;
            bestDist = d;
            bestId = other.id;
        }
        return bestId;
    };
    uDelegates.findEnemyInAttackRange = [bestTargetWithin](const Unit &u) -> int {
        return bestTargetWithin(u, u.attackRange * TILE_WIDTH);
    };
    uDelegates.findTarget = [bestTargetWithin](const Unit &u) -> int {
        // Moving units on an attack-move look as far as they see; so do idle units whose stance chases
        const bool mobile = !u.isSiegeMode && u.moveSpeed > 0.0f;
        const bool scansSight = mobile
                                && (u.isAttackMoving || u.waitingForPath
                                    || (u.state == ActionState::Idle && !u.HoldsGround() && !u.HasMoveTarget()));
        const float scanDist
            = (scansSight ? std::max(static_cast<float>(u.sightRadius), u.attackRange) : u.attackRange) * TILE_WIDTH;
        return bestTargetWithin(u, scanDist);
    };
    uDelegates.findWallTarget = [this, &map](const Unit &u, int &outWallX, int &outWallY) {
        outWallX = -1;
        outWallY = -1;
        // Something to shoot right now comes first; enemies seen behind the wall do not stop the breach
        // (a base walled in all around would never be attacked otherwise)
        bool enemyInRange = false;
        const float rangePixels = u.attackRange * TILE_WIDTH;
        for (const auto &other : m_entities)
        {
            if (!other.alive || other.state == ActionState::Die || !Players::Hostile(other.owner, u.owner))
                continue;
            if (other.faction == Faction::Neutral)
                continue;
            if (ComputeDistanceToEntityPixels(u, other) <= rangePixels && IsEntityVisibleToPlayer(other, u.owner, map))
            {
                enemyInRange = true;
                break;
            }
        }
        // Walls are picked toward where the unit is heading: its target, else its order's destination
        int goalX = u.HasMoveTarget() ? u.targetTileX : -1;
        int goalY = u.HasMoveTarget() ? u.targetTileY : -1;
        if (const Entity *t = (u.targetEntityId != 0) ? GetEntity(u.targetEntityId) : nullptr)
        {
            goalX = static_cast<int>(t->GetUnitCenterPixelX() / TILE_WIDTH);
            goalY = static_cast<int>(t->GetUnitCenterPixelY() / TILE_HEIGHT);
        }
        if (!enemyInRange)
        {
            // Walls an ally of the same player already shoots at come first: one tile down opens the way
            std::vector<std::pair<int, int>> allyWalls;
            for (const auto &other : m_entities)
            {
                if (other.alive && other.id != u.id && other.owner == u.owner && other.targetWallTileX >= 0)
                    allyWalls.emplace_back(other.targetWallTileX, other.targetWallTileY);
            }
            int scanR = 6;
            int minWX = std::max(0, u.tileX - scanR);
            int maxWX = std::min(map.GetWidth() - 1, u.tileX + scanR);
            int minWY = std::max(0, u.tileY - scanR);
            int maxWY = std::min(map.GetHeight() - 1, u.tileY + scanR);
            float bestWDist = 1e9f;
            for (int wy = minWY; wy <= maxWY; ++wy)
            {
                for (int wx = minWX; wx <= maxWX; ++wx)
                {
                    uint16_t w = map.GetWall(wx, wy);
                    if (w == 0)
                        continue;
                    // Only walls toward the destination; never turn back to shoot a wall behind
                    if (goalX >= 0
                        && DetMath::Hypot(wx - goalX, wy - goalY)
                               >= DetMath::Hypot(u.tileX - goalX, u.tileY - goalY) + 1.0f)
                    {
                        continue;
                    }
                    // ... and only the ones in the way: near the straight line to the goal (a wall off to
                    // the side hinders nobody)
                    if (goalX >= 0 && DistanceToSegment(wx, wy, u.tileX, u.tileY, goalX, goalY) > 2.0f)
                    {
                        continue;
                    }
                    // Hostile by owner (two players of one faction can be enemies)
                    if (Players::Hostile(map.GetWallOwner(wx, wy), u.owner))
                    {
                        float wd = DetMath::Hypot((wx - u.tileX) * TILE_WIDTH, (wy - u.tileY) * TILE_HEIGHT);
                        if (std::find(allyWalls.begin(), allyWalls.end(), std::make_pair(wx, wy)) != allyWalls.end())
                            wd -= 2.0f * TILE_WIDTH;
                        if (wd < bestWDist)
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
    uDelegates.findSupportAlly = [this, &map](const Unit &u, int &outTargetEnemyId) -> int {
        outTargetEnemyId = 0;
        float maxDist = u.attackRange * TILE_WIDTH;
        float bestDist = maxDist;
        int bestAllyId = 0;
        for (const auto &other : m_entities)
        {
            if (!other.alive || other.state == ActionState::Die || Players::Hostile(other.owner, u.owner))
                continue;
            const Unit *allyUnit = other.AsUnit();
            if (!allyUnit || allyUnit->id == u.id)
                continue;
            // STRICT DEPTH = 1 INVARIANT:
            // Ally must be in direct combat (within its firing range) AND must NOT be a secondary support unit itself!
            if (!allyUnit->isDirectEngaged || allyUnit->isSupportAssisting || allyUnit->targetEntityId == 0)
                continue;

            const Entity *enemy = GetEntity(allyUnit->targetEntityId);
            if (!enemy || !enemy->alive || enemy->state == ActionState::Die)
                continue;
            if (!IsEntityVisibleToPlayer(*enemy, u.owner, map))
                continue;

            float dx = other.GetUnitCenterPixelX() - u.GetUnitCenterPixelX();
            float dy = other.GetUnitCenterPixelY() - u.GetUnitCenterPixelY();
            float d = DetMath::Hypot(dx, dy);
            if (d <= bestDist)
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
    bDelegates.getEntity = [this](int id) -> Entity * {
        return GetEntity(id);
    };
    bDelegates.spawnProjectile
        = [this](Entity &shooter, float srcX, float srcY, float dstX, float dstY, int targetId, TileMap &m) {
              SpawnProjectileFromEntity(shooter, srcX, srcY, dstX, dstY, targetId, m);
          };
    bDelegates.spawnExplosion = [this](float x, float y, int animId) {
        SpawnExplosion(x, y, animId);
    };
    bDelegates.onKill = [this](PlayerId victim, PlayerId killer) {
        NoteKill(victim, killer);
    };
    bDelegates.isEntityVisible = [this, &map](const Entity &entity, PlayerId observer) {
        return IsEntityVisibleToPlayer(entity, observer, map);
    };
    bDelegates.findTarget = [this, &map](const Building &b) -> int {
        float closestDist = b.attackRange * TILE_WIDTH;
        int bestId = 0;
        float bx = b.GetUnitCenterPixelX();
        float by = b.GetUnitCenterPixelY();
        for (const auto &other : m_entities)
        {
            if (!other.alive || other.state == ActionState::Die || !Players::Hostile(other.owner, b.owner))
                continue;
            if (!IsEntityVisibleToPlayer(other, b.owner, map))
                continue;
            float dx = other.GetUnitCenterPixelX() - bx;
            float dy = other.GetUnitCenterPixelY() - by;
            float d = DetMath::Hypot(dx, dy);
            if (d < closestDist)
            {
                closestDist = d;
                bestId = other.id;
            }
        }
        return bestId;
    };

    // 1. Update entities through polymorphic delegates
    for (auto &entity : m_entities)
    {
        if (!entity.alive)
            continue;

        if (Unit *unit = entity.AsUnit())
        {
            UpdateTurretShift(*unit);
            UpdateAutoSiege(*unit, map, deltaTime);
            unit->Update(deltaTime, map, pathfinder, uDelegates);
            UpdateTurretShift(*unit);
        }
        else if (Building *bldg = entity.AsBuilding())
        {
            bldg->isPowered = bldg->owner >= kMaxPlayers || powered[bldg->owner];
            bldg->Update(deltaTime, map, bDelegates);
        }
    }

    // Resolve any stacked units sharing the exact same tile
    ResolveUnitStacking(map);

    // 2. Update projectiles
    for (auto &proj : m_projectiles)
    {
        if (!proj.active)
            continue;

        float dx = proj.targetX - proj.x;
        float dy = proj.targetY - proj.y;
        float dist = std::sqrt(dx * dx + dy * dy);

        const ProjectileSpec &spec = Projectiles::of(proj.type);
        if (spec.smokeTrail)
        {
            proj.smokeTimer += deltaTime;
            if (proj.smokeTimer >= 0.030f)
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

        if (dist <= proj.speed)
        {
            if (proj.damage <= 0)
            {
                proj.active = false;
                continue;
            }

            Entity *target = (proj.targetId > 0) ? GetEntity(proj.targetId) : nullptr;
            bool targetAlive = (target && target->alive && target->state != ActionState::Die);
            bool isTargetInfantry = (target && target->AsUnit() && target->AsUnit()->IsInfantry());

            // Hit animation of the projectile kind, picked by the target
            const int impactAnim = spec.PickImpact(proj.targetId);
            if (proj.type == ProjectileType::Shell || proj.type == ProjectileType::Rocket)
                Cue(SoundCue::ExplodeSmall, proj.targetX, proj.targetY, proj.owner, proj.impactSound, proj.soundGain);
            else if (proj.impactSound != 0) // a flame / bullet with a hit sound of its own
                Cue(isTargetInfantry ? SoundCue::HitBullet : SoundCue::HitArmor, proj.targetX, proj.targetY, proj.owner,
                    proj.impactSound, proj.soundGain);
            // A small crater where a shell / rocket lands (on the ground or the unit it hit)
            if (const int mark = spec.PickGroundMark(
                    proj.shooterId + proj.targetId + static_cast<int>(proj.targetX) + static_cast<int>(proj.targetY));
                mark >= 0)
            {
                // At the point of contact (on a building the side it came down on), whatever it hit
                AddGroundMark(proj.targetX, proj.targetY, mark, spec.groundMarkSeconds);
            }

            if (targetAlive)
            {
                // Units blow up at their center, buildings where the projectile came down
                const bool hitUnit = target->AsUnit() != nullptr;
                float hitCenterX = hitUnit ? target->GetUnitCenterPixelX() : proj.targetX;
                float hitCenterY = hitUnit ? target->GetUnitCenterPixelY() : proj.targetY;
                NoteEntityHit(*target, proj.shooterId);
                target->currentHp -= ApplyMovingUnitDamage(proj.damage, target, proj.type);
                if (target->currentHp <= 0)
                {
                    target->currentHp = 0;
                    target->state = ActionState::Die;
                    target->animFrame = 0;
                    target->animTimer = 0.0f;
                    target->selected = false;
                    if (Unit *tu = target->AsUnit())
                    {
                        HandleUnitDeathState(tu, proj.type == ProjectileType::Flame);
                    }

                    if (Building *tb = target->AsBuilding())
                    {
                        tb->deathTimer = EffectsConfig::Get().burialSeconds;
                        for (int ty = tb->tileY; ty < tb->tileY + tb->buildHeight; ++ty)
                        {
                            for (int tx = tb->tileX; tx < tb->tileX + tb->buildWidth; ++tx)
                            {
                                map.SetPassable(tx, ty, true);
                            }
                        }
                        SpawnDeathExplosions(*tb);
                    }
                    else if (!isTargetInfantry)
                    {
                        target->deathTimer = EffectsConfig::Get().burialSeconds;
                        SpawnDeathExplosions(*target);
                    }
                    else
                    {
                        target->deathTimer = EffectsConfig::Get().burialSeconds;
                        SpawnExplosion(hitCenterX, hitCenterY, impactAnim);
                    }

                    NoteKill(*target, proj.owner);
                }
                else
                {
                    SpawnExplosion(hitCenterX, hitCenterY, impactAnim);
                }
            }
            else
            {
                int hitTileX = static_cast<int>(proj.targetX / TILE_WIDTH);
                int hitTileY = static_cast<int>(proj.targetY / TILE_HEIGHT);
                // The wall the shooter was ordered to destroy takes the hit, whoever owns it
                const bool forced = proj.forcedWallX >= 0 && map.GetWall(proj.forcedWallX, proj.forcedWallY) > 0;
                if (forced)
                {
                    hitTileX = proj.forcedWallX;
                    hitTileY = proj.forcedWallY;
                }
                if (map.GetWall(hitTileX, hitTileY) > 0
                    && (forced || Players::Hostile(map.GetWallOwner(hitTileX, hitTileY), proj.owner)))
                {
                    NoteHit(map.GetWallOwner(hitTileX, hitTileY), proj.shooterId);
                    map.DamageWall(hitTileX, hitTileY, WeaponBalance::Scale(proj.damage, proj.type, TargetClass::Wall));
                    if (map.GetWall(hitTileX, hitTileY) == 0)
                    {
                        SpawnExplosion(
                            hitTileX * TILE_WIDTH + TILE_WIDTH * 0.5f,
                            hitTileY * TILE_HEIGHT + TILE_HEIGHT * 0.5f,
                            spec.wallDestroyed);
                        const CharacterSounds *wallSounds = WallSoundsOf(map.GetWallFaction(hitTileX, hitTileY));
                        Cue(SoundCue::WallDestroyed, proj.targetX, proj.targetY, proj.owner,
                            wallSounds ? wallSounds->deathId : 0, wallSounds ? wallSounds->volume : 1.0f);
                    }
                    else
                    {
                        SpawnExplosion(proj.targetX, proj.targetY, spec.impactWall);
                    }
                }
                else
                {
                    SpawnExplosion(proj.targetX, proj.targetY, impactAnim);
                }
            }

            // Splash / AoE Damage (3x3 tiles around impact) for Shell, Rocket, Flame
            if (proj.splashPercent > 0 && !spec.hitscan)
            {
                float impactX = targetAlive ? target->GetUnitCenterPixelX() : proj.targetX;
                float impactY = targetAlive ? target->GetUnitCenterPixelY() : proj.targetY;
                int primaryHitId = targetAlive ? target->id : 0;
                int splashDmg = std::max(1, static_cast<int>(std::round(proj.damage * (proj.splashPercent / 100.0f))));
                // The splash covers 3x3 tiles around the tile of the impact (a building when any tile of it is inside)
                const int splashTileX = static_cast<int>(std::floor(impactX / TILE_WIDTH));
                const int splashTileY = static_cast<int>(std::floor(impactY / TILE_HEIGHT));

                for (auto &other : m_entities)
                {
                    if (!other.alive || other.state == ActionState::Die)
                        continue;
                    if (other.id == primaryHitId)
                        continue;
                    if (!Players::Hostile(other.owner, proj.owner))
                        continue;

                    bool inSplash = false;
                    if (const Building *ob = other.AsBuilding())
                    {
                        inSplash = ob->tileX <= splashTileX + 1 && ob->tileX + ob->buildWidth - 1 >= splashTileX - 1
                                   && ob->tileY <= splashTileY + 1 && ob->tileY + ob->buildHeight - 1 >= splashTileY - 1;
                    }
                    else
                    {
                        const int otx = static_cast<int>(std::floor(other.GetUnitCenterPixelX() / TILE_WIDTH));
                        const int oty = static_cast<int>(std::floor(other.GetUnitCenterPixelY() / TILE_HEIGHT));
                        inSplash = std::abs(otx - splashTileX) <= 1 && std::abs(oty - splashTileY) <= 1;
                    }
                    if (inSplash)
                    {
                        NoteEntityHit(other, proj.shooterId);
                        other.currentHp -= ApplyMovingUnitDamage(splashDmg, &other, proj.type);
                        if (other.currentHp <= 0)
                        {
                            other.currentHp = 0;
                            other.state = ActionState::Die;
                            other.animFrame = 0;
                            other.animTimer = 0.0f;
                            other.selected = false;
                            if (Unit *tu = other.AsUnit())
                            {
                                HandleUnitDeathState(tu, proj.type == ProjectileType::Flame);
                            }

                            if (Building *tb = other.AsBuilding())
                            {
                                tb->deathTimer = EffectsConfig::Get().burialSeconds;
                                for (int ty = tb->tileY; ty < tb->tileY + tb->buildHeight; ++ty)
                                {
                                    for (int tx = tb->tileX; tx < tb->tileX + tb->buildWidth; ++tx)
                                    {
                                        map.SetPassable(tx, ty, true);
                                    }
                                }
                                SpawnDeathExplosions(*tb);
                            }
                            else if (other.AsUnit() && other.AsUnit()->IsInfantry())
                            {
                                other.deathTimer = EffectsConfig::Get().burialSeconds;
                            }
                            else
                            {
                                other.deathTimer = EffectsConfig::Get().burialSeconds;
                                SpawnDeathExplosions(other);
                            }
                            NoteStats(other, proj.owner);
                        }
                    }
                }

                int cTileX = static_cast<int>(impactX / TILE_WIDTH);
                int cTileY = static_cast<int>(impactY / TILE_HEIGHT);
                for (int wy = cTileY - 1; wy <= cTileY + 1; ++wy)
                {
                    for (int wx = cTileX - 1; wx <= cTileX + 1; ++wx)
                    {
                        if (wx < 0 || wy < 0 || wx >= map.GetWidth() || wy >= map.GetHeight())
                            continue;
                        if (map.GetWall(wx, wy) > 0
                            && (Players::Hostile(map.GetWallOwner(wx, wy), proj.owner)
                                || (wx == proj.forcedWallX && wy == proj.forcedWallY)))
                        {
                            NoteHit(map.GetWallOwner(wx, wy), proj.shooterId);
                            map.DamageWall(wx, wy, WeaponBalance::Scale(splashDmg, proj.type, TargetClass::Wall));
                            if (map.GetWall(wx, wy) == 0)
                            {
                                SpawnExplosion(
                                    wx * TILE_WIDTH + TILE_WIDTH * 0.5f,
                                    wy * TILE_HEIGHT + TILE_HEIGHT * 0.5f,
                                    spec.wallDestroyed);
                                const CharacterSounds *wallSounds = WallSoundsOf(map.GetWallFaction(wx, wy));
                                Cue(SoundCue::WallDestroyed,
                                    wx * TILE_WIDTH + TILE_WIDTH * 0.5f,
                                    wy * TILE_HEIGHT + TILE_HEIGHT * 0.5f,
                                    proj.owner,
                                    wallSounds ? wallSounds->deathId : 0,
                                    wallSounds ? wallSounds->volume : 1.0f);
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

    // Craters fade away after their time
    for (GroundMark &m : m_groundMarks)
        m.timeLeft -= deltaTime;
    m_groundMarks.erase(
        std::remove_if(
            m_groundMarks.begin(), m_groundMarks.end(), [](const GroundMark &m) { return m.timeLeft <= 0.0f; }),
        m_groundMarks.end());

    // 3. Update smoke puffs
    for (auto &puff : m_smokePuffs)
    {
        if (!puff.active)
            continue;
        puff.lifetime += deltaTime;
        if (puff.lifetime >= puff.maxLifetime)
        {
            puff.active = false;
        }
    }
    m_smokePuffs.erase(
        std::remove_if(m_smokePuffs.begin(), m_smokePuffs.end(), [](const SmokePuff &p) { return !p.active; }),
        m_smokePuffs.end());

    // 3b. Flamethrower jets and muzzle flashes (visual only)
    for (auto &flash : m_muzzleFlashes)
        flash.age += deltaTime;
    m_muzzleFlashes.erase(
        std::remove_if(
            m_muzzleFlashes.begin(), m_muzzleFlashes.end(), [](const MuzzleFlash &f) { return f.age >= kMuzzleFlashKeep; }),
        m_muzzleFlashes.end());
    for (auto &burst : m_flameBursts)
        burst.age += deltaTime;
    m_flameBursts.erase(
        std::remove_if(
            m_flameBursts.begin(), m_flameBursts.end(), [](const FlameBurst &b) { return b.age >= b.duration; }),
        m_flameBursts.end());

    // 4. Update explosions
    for (auto &exp : m_explosions)
    {
        if (!exp.active)
            continue;
        if (exp.delay > 0.0f)
        {
            exp.delay -= deltaTime;
            continue;
        }
        exp.frameTimer += deltaTime;
        if (exp.frameTimer >= EffectsConfig::Get().frameTime)
        {
            exp.frameTimer = 0.0f;
            exp.currentFrame++;
            if (exp.currentFrame >= exp.totalFrames)
            {
                exp.active = false;
            }
        }
    }
    m_explosions.erase(
        std::remove_if(m_explosions.begin(), m_explosions.end(), [](const ExplosionParticle &e) { return !e.active; }),
        m_explosions.end());

    // 5. Clean up dead entities
    m_entities.Raw().erase(
        std::remove_if(
            m_entities.Raw().begin(),
            m_entities.Raw().end(),
            [](const std::unique_ptr<Entity> &e) { return !e->alive; }),
        m_entities.Raw().end());

    // 6. Update Fog of War post-movement
    UpdateFogOfWar(map);
}

void EntityManager::SpawnProjectileFromEntity(
    Entity &entity, float srcX, float srcY, float dstX, float dstY, int targetEntityId, TileMap &map)
{
    // Shots at a building land on the side facing the shooter, not in its middle (units are hit at their center)
    if (targetEntityId > 0)
    {
        if (const Building *tb = GetBuilding(targetEntityId))
        {
            const float inset = 3.0f;
            const float minX = tb->tileX * TILE_WIDTH + inset;
            const float minY = tb->tileY * TILE_HEIGHT + inset;
            const float maxX = (tb->tileX + tb->buildWidth) * TILE_WIDTH - inset;
            const float maxY = (tb->tileY + tb->buildHeight) * TILE_HEIGHT - inset;
            dstX = std::clamp(srcX, minX, maxX) + Utility::Random(-8.0f, 8.0f);
            dstY = std::clamp(srcY, minY, maxY) + Utility::Random(-6.0f, 6.0f);
            dstX = std::clamp(dstX, minX, maxX);
            dstY = std::clamp(dstY, minY, maxY);
            // ... a tenth of the way in towards its middle: shots, shells and rockets land on the building, not on
            // its very edge
            constexpr float kInwards = 0.10f;
            const float cx = (tb->tileX + tb->buildWidth * 0.5f) * TILE_WIDTH;
            const float cy = (tb->tileY + tb->buildHeight * 0.5f) * TILE_HEIGHT;
            dstX += (cx - dstX) * kInwards;
            dstY += (cy - dstY) * kInwards;
        }
    }

    float dx = dstX - srcX;
    float dy = dstY - srcY;
    float dist = std::max(1.0f, DetMath::Hypot(dx, dy));

    // Randomized shot dispersion
    const float normRng = Utility::RandomSigned(); // [-1 .. +1]

    // Authentic dispersion defaults per projectile type (or override from entity.spreadAngle)
    float maxSpread = 0.0f;
    if (entity.spreadAngle >= 0.0f)
    {
        // Explicit configuration: if spread == 0.0f, maxSpread is 0.0f (high precision pinpoint accuracy)
        maxSpread = entity.spreadAngle;
    }
    else
    {
        // Unconfigured default (-1.0f): the spread of the projectile kind (projectiles.yaml)
        maxSpread = Projectiles::of(entity.projectile).spread;
    }

    float spreadDstX = dstX;
    float spreadDstY = dstY;
    float spreadDx = dx;
    float spreadDy = dy;

    if (maxSpread > 0.0001f)
    {
        float spreadAngle = normRng * maxSpread;
        float baseAng = DetMath::Atan2(dy, dx);
        float spreadAng = baseAng + spreadAngle;

        spreadDstX = srcX + DetMath::Cos(spreadAng) * dist;
        spreadDstY = srcY + DetMath::Sin(spreadAng) * dist;
        spreadDx = spreadDstX - srcX;
        spreadDy = spreadDstY - srcY;
    }

    const int dir16 = Utility::DirFromNorth(spreadDx, spreadDy, 16);
    const int dir32 = Utility::DirFromNorth(spreadDx, spreadDy, 32);

    const ProjectileSpec &spec = Projectiles::of(entity.projectile);
    // The sounds of the gun firing (its character file may name its own) and how loud this character is
    const WeaponConfig *gun = FiringWeapon(entity);
    const CharacterSounds *charSounds = SoundsOf(entity);
    const float soundGain = (gun ? gun->soundVolume : 1.0f) * (charSounds ? charSounds->volume : 1.0f);
    {
        // The sound of the shot by weapon kind
        const Unit *shooter = entity.AsUnit();
        SoundCue fire = SoundCue::FireMg;
        switch (entity.projectile)
        {
            case ProjectileType::Flame:
                fire = SoundCue::FireFlame;
                break;
            case ProjectileType::Sniper:
                fire = SoundCue::FireSniper;
                break;
            case ProjectileType::Shell:
            {
                // The heavy tanks (T-22 Zeus, Rhino, in any of their forms) have a deeper gun than the rest
                const Utility::ArchetypeInfo *info = shooter ? Utility::GetArchetypeInfo(shooter->archetypeId) : nullptr;
                const int base = info && info->baseFormId >= 0 ? info->baseFormId : (shooter ? shooter->archetypeId : -1);
                fire = (base == 10 || base == 11) ? SoundCue::FireCannonHigh : SoundCue::FireCannon;
                break;
            }
            case ProjectileType::Rocket:
                fire = (shooter && shooter->burstCount >= 3) ? SoundCue::FireMlrs : SoundCue::FireRocket;
                break;
            default:
                fire = (shooter && shooter->IsInfantry()) ? SoundCue::FireRifle : SoundCue::FireMg;
                break;
        }
        Cue(fire, srcX, srcY, entity.owner, gun ? gun->soundId : 0, soundGain);
        if (m_muzzleFlashes.size() < 512) // its light at night (a flame jet lights itself)
            if (entity.projectile != ProjectileType::Flame)
                m_muzzleFlashes.push_back({srcX, srcY, 0.0f});
    }
    if (spec.hitscan)
    {
        float finalHitX = spreadDstX;
        float finalHitY = spreadDstY;

        if (targetEntityId > 0)
        {
            Entity *target = GetEntity(targetEntityId);
            if (target && target->alive && target->state != ActionState::Die)
            {
                bool isTargetInfantry = (target->AsUnit() && target->AsUnit()->IsInfantry());
                int impactAnim = spec.PickImpact(target->id);
                // The gun's own hit sound, else the damage sound of the one hit (at its own volume)
                const CharacterSounds *hurt = SoundsOf(*target);
                const bool gunHit = gun && gun->impactSoundId != 0;
                const bool ownHit = !gunHit && hurt && hurt->damageId != 0;
                Cue(isTargetInfantry ? SoundCue::HitBullet : SoundCue::HitArmor, finalHitX, finalHitY, entity.owner,
                    gunHit ? gun->impactSoundId : (ownHit ? hurt->damageId : 0),
                    ownHit ? hurt->volume : soundGain);

                NoteEntityHit(*target, entity.id);
                target->currentHp -= ApplyMovingUnitDamage(entity.attackDamage, target, entity.projectile);
                if (target->currentHp <= 0)
                {
                    target->currentHp = 0;
                    target->state = ActionState::Die;
                    target->animFrame = 0;
                    target->animTimer = 0.0f;
                    target->selected = false;
                    if (Unit *tu = target->AsUnit())
                    {
                        HandleUnitDeathState(tu, entity.projectile == ProjectileType::Flame);
                    }

                    if (Building *tb = target->AsBuilding())
                    {
                        tb->deathTimer = EffectsConfig::Get().burialSeconds;
                        for (int ty = tb->tileY; ty < tb->tileY + tb->buildHeight; ++ty)
                        {
                            for (int tx = tb->tileX; tx < tb->tileX + tb->buildWidth; ++tx)
                            {
                                map.SetPassable(tx, ty, true);
                            }
                        }
                        SpawnDeathExplosions(*tb);
                    }
                    else if (!isTargetInfantry)
                    {
                        target->deathTimer = EffectsConfig::Get().burialSeconds;
                        SpawnDeathExplosions(*target);
                    }
                    else
                    {
                        target->deathTimer = EffectsConfig::Get().burialSeconds;
                        SpawnExplosion(finalHitX, finalHitY, impactAnim);
                    }

                    NoteKill(*target, entity.owner);
                }
                else
                {
                    SpawnExplosion(finalHitX, finalHitY, impactAnim);
                }
            }
        }
        else if (entity.targetWallTileX >= 0 && entity.targetWallTileY >= 0)
        {
            int wx = entity.targetWallTileX;
            int wy = entity.targetWallTileY;
            if (map.GetWall(wx, wy) > 0)
            {
                NoteHit(map.GetWallOwner(wx, wy), entity.id);
                map.DamageWall(wx, wy, WeaponBalance::Scale(entity.attackDamage, entity.projectile, TargetClass::Wall));
                finalHitX = wx * TILE_WIDTH + (TILE_WIDTH * 0.5f);
                finalHitY = wy * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
                // (the wall character of the side names how its segments sound)
                const CharacterSounds *wallSounds = WallSoundsOf(map.GetWallFaction(wx, wy));
                const float wallGain = wallSounds ? wallSounds->volume : 1.0f;
                if (map.GetWall(wx, wy) == 0)
                {
                    SpawnExplosion(finalHitX, finalHitY, spec.wallDestroyed);
                    Cue(SoundCue::WallDestroyed, finalHitX, finalHitY, entity.owner, wallSounds ? wallSounds->deathId : 0,
                        wallGain);
                }
                else
                {
                    SpawnExplosion(finalHitX, finalHitY, spec.impactWall);
                    const bool gunHit = gun && gun->impactSoundId != 0;
                    Cue(SoundCue::HitArmor, finalHitX, finalHitY, entity.owner,
                        gunHit ? gun->impactSoundId : (wallSounds ? wallSounds->damageId : 0),
                        gunHit ? soundGain : wallGain);
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
        p.type = entity.projectile; // the tracer of its own kind (a sniper's round looks different)
        p.speed = spec.speed;
        m_projectiles.push_back(p);
        return;
    }

    // Physical projectile (Rocket, Shell, Flame)
    float projTargetX = spreadDstX;
    float projTargetY = spreadDstY;
    int projTargetId = targetEntityId;

    Projectile p;
    p.impactSound = gun ? gun->impactSoundId : 0;
    p.soundGain = soundGain;
    p.x = srcX;
    p.y = srcY;
    p.startX = p.x;
    p.startY = p.y;
    p.targetX = projTargetX;
    p.targetY = projTargetY;
    if (const Unit *su = entity.AsUnit(); su && su->isManualWallTarget && entity.targetWallTileX >= 0)
    {
        p.forcedWallX = entity.targetWallTileX;
        p.forcedWallY = entity.targetWallTileY;
    }
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
    p.speed = spec.speed;
    // A shooter with a flight speed of its own (combat.bullet_speed, px/s) sends its rockets and shells that fast
    if (entity.bulletSpeed > 0.0f && (p.type == ProjectileType::Rocket || p.type == ProjectileType::Shell))
        p.speed = std::max(0.5f, entity.bulletSpeed * static_cast<float>(SIMULATION_TIMESTEP));
    if (p.type == ProjectileType::Flame)
    {
        // The jet is drawn by its own effect, as long as the projectile would fly (22 px per tick) but never
        // shorter than the animation needs: a target close by got no visible flame at all before
        constexpr float kMinFlameSeconds = 0.35f;
        const float flight = DetMath::Hypot(p.targetX - p.startX, p.targetY - p.startY) / p.speed
                             * static_cast<float>(SIMULATION_TIMESTEP);
        FlameBurst burst;
        burst.startX = p.startX;
        burst.startY = p.startY;
        burst.targetX = p.targetX;
        burst.targetY = p.targetY;
        burst.duration = std::max(kMinFlameSeconds, flight);
        burst.faction = p.faction;
        burst.owner = p.owner;
        m_flameBursts.push_back(burst);
    }
    m_projectiles.push_back(p);
}

int EntityManager::DemolishBuilding(int buildingId, TileMap &map)
{
    Building *b = GetBuilding(buildingId);
    if (!b || !b->alive || b->state == ActionState::Die)
        return 0;
    int refund = 0;
    for (const auto &item : b->productionQueue)
        refund += item.cost;
    b->productionQueue.clear();
    b->isProducing = false;
    b->currentHp = 0;
    b->state = ActionState::Die;
    b->animFrame = 0;
    b->animTimer = 0.0f;
    b->selected = false;
    b->deathTimer = EffectsConfig::Get().burialSeconds;
    for (int ty = b->tileY; ty < b->tileY + b->buildHeight; ++ty)
    {
        for (int tx = b->tileX; tx < b->tileX + b->buildWidth; ++tx)
        {
            map.SetPassable(tx, ty, true);
        }
    }
    SpawnDeathExplosions(*b);
    return refund;
}

int EntityManager::ValueOf(const Entity &e) const
{
    if (const Unit *u = e.AsUnit())
    {
        const UnitConfig *cfg = GetUnitConfig(u->archetypeId);
        return cfg ? cfg->creditCost : 0;
    }
    if (const Building *b = e.AsBuilding())
    {
        const BuildingConfig *cfg = GetBuildingConfig(b->rawBuildingType);
        return cfg ? cfg->creditCost : 0;
    }
    return 0;
}

void EntityManager::NoteStats(const Entity &victim, PlayerId killer)
{
    const int value = ValueOf(victim);
    if (victim.owner < kMaxPlayers)
    {
        PlayerStats &lost = m_stats[victim.owner];
        (victim.isBuilding ? lost.buildingsLost : lost.unitsLost) += 1;
        lost.valueLost += value;
    }
    if (killer < kMaxPlayers && Players::Hostile(victim.owner, killer))
    {
        PlayerStats &won = m_stats[killer];
        (victim.isBuilding ? won.buildingsDestroyed : won.unitsKilled) += 1;
        won.valueDestroyed += value;
    }
}

void EntityManager::NoteKill(const Entity &victim, PlayerId killer)
{
    NoteKill(victim.owner, killer);
    NoteStats(victim, killer);
}

void EntityManager::KillEntity(Entity &e, TileMap &map)
{
    if (!e.alive || e.state == ActionState::Die)
        return;
    e.currentHp = 0;
    e.state = ActionState::Die;
    e.animFrame = 0;
    e.animTimer = 0.0f;
    e.selected = false;
    e.deathTimer = EffectsConfig::Get().burialSeconds;
    if (Building *b = e.AsBuilding())
    {
        b->productionQueue.clear();
        b->isProducing = false;
        for (int ty = b->tileY; ty < b->tileY + b->buildHeight; ++ty)
        {
            for (int tx = b->tileX; tx < b->tileX + b->buildWidth; ++tx)
                map.SetPassable(tx, ty, true);
        }
        SpawnDeathExplosions(*b);
    }
    else if (Unit *u = e.AsUnit())
    {
        HandleUnitDeathState(u, false);
        if (u->IsInfantry())
            SpawnExplosion(u->GetUnitCenterPixelX(), u->GetUnitCenterPixelY(), BulletImpactAnim(u->id));
        else
            SpawnDeathExplosions(*u);
    }
}

void EntityManager::DestroyPlayer(PlayerId player, TileMap &map)
{
    if (player >= kMaxPlayers)
        return;
    for (Entity &e : m_entities)
    {
        if (e.owner == player)
            KillEntity(e, map);
    }
    // The walls of the player's base go too
    for (int y = 0; y < map.GetHeight(); ++y)
    {
        for (int x = 0; x < map.GetWidth(); ++x)
        {
            if (map.GetWall(x, y) > 0 && map.GetWallOwner(x, y) == player)
            {
                map.DestroyWall(x, y);
                SpawnExplosion(x * TILE_WIDTH + TILE_WIDTH * 0.5f, y * TILE_HEIGHT + TILE_HEIGHT * 0.5f, 42);
            }
        }
    }
}

std::vector<PlayerId> EntityManager::UpdateElimination(TileMap &map)
{
    std::vector<PlayerId> out;
    std::array<bool, kMaxPlayers> hasBuilding {};
    for (const Entity &e : m_entities)
    {
        if (e.alive && e.state != ActionState::Die && e.isBuilding && e.owner < kMaxPlayers)
            hasBuilding[e.owner] = true;
    }
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        if (!Players::IsActive(p) || m_eliminated[p] || hasBuilding[p])
            continue;
        m_eliminated[p] = true;
        DestroyPlayer(p, map);
        out.push_back(p);
    }
    return out;
}

void EntityManager::SpawnDeathExplosions(const Entity &e)
{
    const EffectsConfig &fx = EffectsConfig::Get();
    const float cx = e.GetUnitCenterPixelX();
    const float cy = e.GetUnitCenterPixelY();
    auto pick = [](const std::vector<int> &list, int i) {
        return list.empty() ? -1 : list[static_cast<size_t>(i) % list.size()];
    };
    if (const Building *b = e.AsBuilding())
    {
        int main = fx.buildingLargeMain;
        if (b->buildWidth <= 2 && b->buildHeight <= 2)
        {
            main = fx.buildingSmallMain;
        }
        else if (b->rawBuildingType == 2 || b->rawBuildingType == 4 || b->rawBuildingType == 12)
        {
            main = fx.buildingPowerMain;
        }
        SpawnExplosion(cx, cy, main);
        // The rest of the structure goes up over the next half second, spread over its whole footprint
        const int extra = std::clamp(b->buildWidth * b->buildHeight / 2, fx.buildingExtraMin, fx.buildingExtraMax);
        for (int i = 0; i < extra; ++i)
        {
            const float x = (b->tileX + Utility::VisualRandom(0.2f, b->buildWidth - 0.2f)) * TILE_WIDTH;
            const float y = (b->tileY + Utility::VisualRandom(0.2f, b->buildHeight - 0.2f)) * TILE_HEIGHT;
            const int anim = (i % fx.buildingBigEvery == 0) ? fx.buildingBigAnim
                                                            : pick(fx.buildingExtraAnims, Utility::VisualRandom(0, 3));
            SpawnExplosion(
                x,
                y,
                anim,
                fx.buildingDelayStart + fx.buildingDelayStep * static_cast<float>(i)
                    + Utility::VisualRandom(0.0f, fx.buildingDelayJitter));
        }
        return;
    }
    // Vehicle: the blast, then the ammunition / fuel a moment later
    SpawnExplosion(cx, cy, pick(fx.vehicleMain, e.id));
    SpawnExplosion(
        cx + Utility::VisualRandom(-fx.vehicleSecondSpreadX, fx.vehicleSecondSpreadX),
        cy + Utility::VisualRandom(fx.vehicleSecondSpreadYMin, fx.vehicleSecondSpreadYMax),
        pick(fx.vehicleSecond, e.id + 2),
        Utility::VisualRandom(fx.vehicleSecondDelayMin, fx.vehicleSecondDelayMax));
}

void EntityManager::SpawnExplosion(float worldX, float worldY, int animId, float delay)
{
    if (animId < 0)
        return;
    ExplosionParticle exp;
    exp.x = worldX;
    exp.y = worldY;
    exp.animId = animId;
    exp.delay = delay;
    exp.currentFrame = 0;
    exp.frameTimer = 0.0f;
    exp.active = true;

    if (m_texManager)
    {
        IndexRange r = m_texManager->GetAnimRange(animId);
        if (r.end >= r.start)
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

void EntityManager::GetUnitsInBox(
    float minWorldX, float minWorldY, float maxWorldX, float maxWorldY, PlayerId owner, std::vector<int> &outUnitIds)
{
    outUnitIds.clear();
    for (auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || e.isBuilding || e.owner != owner)
            continue;
        float px = e.GetUnitCenterPixelX();
        float py = e.GetUnitCenterPixelY();
        if (px >= minWorldX && px <= maxWorldX && py >= minWorldY && py <= maxWorldY)
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

int EntityManager::GetUnitAt(float worldX, float worldY, float tolerance) const
{
    float bestDist = tolerance;
    int bestId = 0;
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die)
            continue;
        if (const Building *b = e.AsBuilding())
        {
            float minX = b->tileX * TILE_WIDTH;
            float maxX = (b->tileX + b->buildWidth) * TILE_WIDTH;
            float minY = b->tileY * TILE_HEIGHT;
            float maxY = (b->tileY + b->buildHeight) * TILE_HEIGHT;
            if (worldX >= minX && worldX <= maxX && worldY >= minY && worldY <= maxY)
            {
                return b->id;
            }
            continue;
        }
        float dx = e.GetUnitCenterPixelX() - worldX;
        float dy = e.GetUnitCenterPixelY() - worldY;
        float d = std::sqrt(dx * dx + dy * dy);
        if (d < bestDist)
        {
            bestDist = d;
            bestId = e.id;
        }
    }
    return bestId;
}

void EntityManager::AddGroundMark(float x, float y, int composite, float seconds)
{
    if (composite <= 0 || seconds <= 0.0f)
        return;
    constexpr size_t kMaxMarks = 400; // the oldest go first
    if (m_groundMarks.size() >= kMaxMarks)
        m_groundMarks.erase(m_groundMarks.begin());
    m_groundMarks.push_back({x, y, composite, seconds});
}

void EntityManager::RenderGroundRemnants(
    Renderer &renderer, const Camera &camera, const TileMap &map, bool fogEnabled) const
{
    // Craters of shells and rockets: under everything else, only where the player can see the ground
    for (const GroundMark &m : m_groundMarks)
    {
        if (fogEnabled
            && map.GetFogState(static_cast<int>(m.x / TILE_WIDTH), static_cast<int>(m.y / TILE_HEIGHT))
                   == FogState::Hidden)
            continue;
        renderer.DrawCompositeWorld(m.composite, m.x - 90.0f, m.y - 60.0f, camera, Faction::Resistance);
    }
    for (const auto &entity : m_entities)
    {
        if (!entity.alive || entity.state != ActionState::Die)
            continue;
        if (fogEnabled && Players::Hostile(entity.owner, Players::Local()))
        {
            if (map.GetFogState(entity.tileX, entity.tileY) == FogState::Hidden)
                continue;
        }
        // Wrecks and rubble replace the destroyed object from the middle of its explosion (elapsed >= 0.25s);
        // until then the building / vehicle is still standing there in its battered state
        float elapsedDeath = EffectsConfig::Get().burialSeconds - entity.deathTimer;
        const bool infantry = entity.AsUnit() && entity.AsUnit()->IsInfantry();
        if (elapsedDeath < 0.25f && !infantry)
        {
            if (const Building *b = entity.AsBuilding())
            {
                const int btype = (b->rawBuildingType >= 0) ? b->rawBuildingType : (static_cast<int>(b->type) % 15);
                for (int layer = 0; layer < 2; ++layer)
                {
                    renderer.SetDrawOwner(b->owner);
                    renderer.DrawBuildingWorld(
                        btype,
                        b->tileX * TILE_WIDTH,
                        b->tileY * TILE_HEIGHT,
                        layer,
                        camera,
                        b->faction,
                        0,
                        1.0f,
                        false,
                        0.0f,
                        b->turretFacing,
                        false,
                        false);
                }
            }
            else if (const Unit *u = entity.AsUnit())
            {
                renderer.SetDrawOwner(u->owner);
                renderer.DrawUnitWorld(
                    u->archetypeId,
                    static_cast<int>(ActionState::Idle),
                    u->facingDirection,
                    0,
                    u->GetWorldPixelX(),
                    u->GetWorldPixelY(),
                    camera,
                    u->faction,
                    u->turretFacing,
                    false);
            }
            continue;
        }

        // After the burial time the remains fade out: linear transparency over fade_seconds of effects.yaml
        {
            const EffectsConfig &fx = EffectsConfig::Get();
            const float over = -entity.deathTimer;
            renderer.SetDrawAlpha(over > 0.0f && fx.fadeSeconds > 0.0f ? std::clamp(1.0f - over / fx.fadeSeconds, 0.0f, 1.0f) : 1.0f);
        }
        if (const Building *b = entity.AsBuilding())
        {
            int btype = (b->rawBuildingType >= 0) ? b->rawBuildingType : (static_cast<int>(b->type) % 15);
            float bx = b->tileX * TILE_WIDTH;
            float by = b->tileY * TILE_HEIGHT;
            // The rubble sinks into the ground stage by stage over the burial time the remains stay
            int wreckComp = 0, stages = 0;
            Renderer::DefaultBuildingWreck(b->buildWidth, b->buildHeight, wreckComp, stages);
            if (b->wreckStages > 0)
                stages = b->wreckStages;
            const float burial = EffectsConfig::Get().burialSeconds;
            const int stage
                = std::min(stages - 1, static_cast<int>(std::clamp(elapsedDeath, 0.0f, burial) / burial * stages));
            renderer.SetDrawOwner(b->owner);
            renderer.DrawBuildingWorld(
                btype,
                bx,
                by,
                0,
                camera,
                b->faction,
                stage,
                1.0f,
                true,
                1.0f,
                0,
                false,
                true,
                b->wreckComposite,
                b->wreckStages);
        }
        else if (const Unit *u = entity.AsUnit())
        {
            renderer.SetDrawOwner(u->owner);
            renderer.DrawUnitWorld(
                u->archetypeId,
                static_cast<int>(ActionState::Die),
                u->facingDirection,
                u->animFrame,
                u->GetWorldPixelX(),
                u->GetWorldPixelY(),
                camera,
                u->faction,
                u->turretFacing,
                u->killedByFlame,
                u->wreckComposite,
                u->wreckStages,
                u->selectedDeathSeq);
        }
        renderer.SetDrawAlpha(1.0f);
    }
    renderer.SetDrawOwner(kNoPlayer);
}

bool EntityManager::IsHostileBuildingShown(const Building &b, const TileMap &map, bool *outInSight) const
{
    bool inSight = false;
    for (int ty = b.tileY; ty < b.tileY + b.buildHeight && !inSight; ++ty)
    {
        for (int tx = b.tileX; tx < b.tileX + b.buildWidth && !inSight; ++tx)
        {
            inSight = map.GetFogState(tx, ty) == FogState::Visible;
        }
    }
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(b.id)) << 32)
                         | (static_cast<uint64_t>(static_cast<uint16_t>(b.tileY)) << 16)
                         | static_cast<uint16_t>(b.tileX);
    if (inSight)
        m_seenBuildings.insert(key);
    if (outInSight)
        *outInSight = inSight;
    return inSight || (b.buildProgress >= 1.0f && m_seenBuildings.count(key) > 0);
}

namespace
{
// Bubble colour of a selected object by its health: green, yellow, red
SDL_Color SelectionHealthColor(float ratio)
{
    const SDL_Color green {80, 255, 130, 255}, yellow {255, 220, 70, 255}, red {255, 70, 60, 255};
    auto mix = [](SDL_Color a, SDL_Color b, float t) {
        return SDL_Color {
            static_cast<Uint8>(a.r + (b.r - a.r) * t),
            static_cast<Uint8>(a.g + (b.g - a.g) * t),
            static_cast<Uint8>(a.b + (b.b - a.b) * t),
            255};
    };
    return ratio >= 0.5f ? mix(yellow, green, (ratio - 0.5f) * 2.0f) : mix(red, yellow, ratio * 2.0f);
}
} // namespace

void EntityManager::Render(
    Renderer &renderer,
    const Camera &camera,
    const TileMap &map,
    bool fogEnabled,
    int globalAnimTick,
    bool debugMode,
    bool showAllHealth)
{
    float startWorldX, startWorldY, endWorldX, endWorldY;
    camera.ScreenToWorld(0, 0, startWorldX, startWorldY);
    camera.ScreenToWorld(camera.GetViewportWidth(), camera.GetViewportHeight(), endWorldX, endWorldY);
    int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, map.GetWidth() - 1);
    int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, map.GetWidth());
    int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, map.GetHeight() - 1);
    int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, map.GetHeight());

    // Depth order: everything is drawn by the bottom edge of what stands on the ground (a unit's tile, a
    // building's last footprint row, a wall row), so whatever reaches above its own tiles - the top of a
    // headquarters, a tall wall segment - covers units behind it, and units in front are drawn over it.
    // Health / progress bars come after all of it.
    struct DepthItem
    {
        float footY;
        int order; // equal foot: walls, then buildings and tree crowns, then units
        int row;   // wall row
        const Unit *unit;
        const Building *building;
        bool live;                            // building currently in sight (else drawn still, under the fog shading)
        const MapDecoration *decor = nullptr; // the crown of a tree / rock (its ground layer went with the terrain)
    };
    std::vector<DepthItem> items;
    items.reserve(m_entities.size() + static_cast<size_t>(std::max(0, endTileY - startTileY)));
    for (int ty = startTileY; ty < endTileY; ++ty)
    {
        items.push_back({static_cast<float>((ty + 1) * TILE_HEIGHT), 0, ty, nullptr, nullptr, true});
    }
    const float viewTop = startTileY * static_cast<float>(TILE_HEIGHT);
    const float viewBottom = (endTileY + 1) * static_cast<float>(TILE_HEIGHT);
    for (const auto &entity : m_entities)
    {
        if (!entity.alive || entity.state == ActionState::Die)
            continue;
        if (const Unit *u = entity.AsUnit())
        {
            if (fogEnabled && Players::Hostile(u->owner, Players::Local())
                && map.GetFogState(u->tileX, u->tileY) != FogState::Visible)
            {
                continue;
            }
            const float foot = u->GetWorldPixelY() + TILE_HEIGHT;
            if (foot < viewTop || foot > viewBottom + TILE_HEIGHT)
                continue;
            items.push_back({foot, 2, 0, u, nullptr, true});
        }
        else if (const Building *b = entity.AsBuilding())
        {
            // Enemy structures: in sight -> live; seen before and finished -> drawn still, the fog shading goes on top;
            // never seen, or still under construction out of sight -> hidden (no peeking through the fog)
            bool live = true;
            if (fogEnabled && Players::Hostile(b->owner, Players::Local()))
            {
                if (!IsHostileBuildingShown(*b, map, &live))
                    continue;
            }
            items.push_back({static_cast<float>((b->tileY + b->buildHeight) * TILE_HEIGHT), 1, 0, nullptr, b, live});
        }
    }
    // Crowns of trees and rocks, by the bottom row of their footprint (same view and fog rules as the terrain pass)
    for (const MapDecoration &dec : map.GetDecorations())
    {
        if (dec.x < startTileX - 4 || dec.x > endTileX + 4 || dec.y < startTileY - 4 || dec.y > endTileY + 4)
            continue;
        if (fogEnabled && map.GetFogState(dec.x, dec.y) == FogState::Hidden)
            continue;
        const int rows = renderer.TallDecorRows(map.GetTilesetId(), dec.type);
        if (rows <= 0)
            continue;
        DepthItem item {static_cast<float>((dec.y + rows) * TILE_HEIGHT), 1, 0, nullptr, nullptr, true};
        item.decor = &dec;
        items.push_back(item);
    }
    std::stable_sort(items.begin(), items.end(), [](const DepthItem &l, const DepthItem &r) {
        return l.footY != r.footY ? l.footY < r.footY : l.order < r.order;
    });

    for (const DepthItem &it : items)
    {
        if (it.order == 0)
        {
            map.RenderWallRow(it.row, renderer, camera, fogEnabled, globalAnimTick, startTileX, endTileX);
        }
        else if (const MapDecoration *dec = it.decor)
        {
            renderer.SetDrawOwner(kNoPlayer);
            renderer.DrawDecorWorld(
                map.GetTilesetId(), dec->type, dec->x * TILE_WIDTH, dec->y * TILE_HEIGHT, camera, globalAnimTick, 1);
        }
        else if (const Unit *u = it.unit)
        {
            float wx = u->GetWorldPixelX();
            float wy = u->GetWorldPixelY();
            if (u->selected)
            {
                renderer.SetDrawOwner(kNoPlayer);
                // A glowing bubble at the feet (health as its colour) instead of the ring
                const float ratio = u->maxHp > 0 ? std::clamp(static_cast<float>(u->currentHp) / u->maxHp, 0.0f, 1.0f) : 1.0f;
                const float rx = u->IsInfantry() ? TILE_WIDTH * 0.80f : TILE_WIDTH * 1.05f;
                renderer.DrawSelectionBubble(
                    wx + TILE_WIDTH * 0.5f, wy + TILE_HEIGHT * 0.75f, rx, rx * 0.55f, camera,
                    Players::Hostile(u->owner, Players::Local()) ? SDL_Color {240, 80, 70, 255} : SelectionHealthColor(ratio), 0.0f);
                if (u->isSiegeMode || u->state == ActionState::Transform)
                {
                    renderer.DrawCompositeWorld(1872, wx - 90.0f, wy - 60.0f, camera, u->faction);
                }
            }
            bool isFlaming
                = (u->state == ActionState::Die) ? u->killedByFlame : (u->projectile == ProjectileType::Flame);
            renderer.SetDrawOwner(u->owner);
            renderer.DrawUnitWorld(
                u->archetypeId,
                static_cast<int>(u->state),
                u->facingDirection,
                u->animFrame,
                wx,
                wy,
                camera,
                u->faction,
                u->turretFacing,
                isFlaming,
                u->wreckComposite,
                u->wreckStages,
                u->selectedDeathSeq,
                u->flashMode,
                u->flashFrames);
        }
        else if (const Building *b = it.building)
        {
            const bool live = it.live;
            int btype = (b->rawBuildingType >= 0) ? b->rawBuildingType : (static_cast<int>(b->type) % 15);
            float bx = b->tileX * TILE_WIDTH;
            float by = b->tileY * TILE_HEIGHT;
            bool isProdBuilding = (btype == 2 || btype == 3 || btype == 10 || btype == 11);
            bool isResearchBuilding = (btype == 4 || btype == 12);
            int bAnim = (!live || (isProdBuilding && !b->isProducing) || (isResearchBuilding && !b->isResearching))
                            ? 0
                            : (globalAnimTick + b->id);
            // Work animations appear only while the building works: the Resistance Factory smokes while it builds a
            // vehicle, and the Confederation Tech Center animates screen lights while its owner researches;
            // otherwise those animated parts are not drawn at all.
            // Note: The Resistance Research Lab (btype == 12) radar head is a physical structure part (anim sequence 6,
            // whose frame 0 is the resting radar dish). It must remain visible in IDLE state (bAnim == 0), stopping
            // rotation, rather than being hidden completely.
            bool workAnimation = true;
            if (btype == 11)
                workAnimation = live && b->isProducing && b->buildProgress >= 1.0f;
            else if (btype == 4)
                workAnimation = live && b->isResearching;
            renderer.SetAnimatedPartsVisible(workAnimation);
            float hpRatio = (b->maxHp > 0) ? (static_cast<float>(b->currentHp) / b->maxHp) : 1.0f;
            bool isAttacking = live && (b->state == ActionState::Attack && b->attackAnimTimer > 0.0f);

            renderer.SetDrawOwner(b->owner);
            if (b->selected && b->buildProgress >= 1.0f)
            {
                // The selected building itself shines, its colour breathing between white and the health colour
                const float t = 0.5f + 0.5f * std::sin(static_cast<float>(SDL_GetTicks() % 100000) * 0.00691f);
                const float hp = b->maxHp > 0 ? std::clamp(static_cast<float>(b->currentHp) / b->maxHp, 0.0f, 1.0f) : 1.0f;
                const SDL_Color hc = Players::Hostile(b->owner, Players::Local()) ? SDL_Color {240, 80, 70, 255}
                                                                                   : SelectionHealthColor(hp); // (an opponent's health is not shown)
                auto mix = [&](uint8_t c) { return static_cast<uint8_t>(255.0f + (c - 255.0f) * t); };
                renderer.SetSpriteGlow({mix(hc.r), mix(hc.g), mix(hc.b), 255}, 0.12f + 0.28f * t);
            }

            // The progress of an enemy construction is not shown: just the scaffolding
            const bool hiddenProgress
                = fogEnabled && b->buildProgress < 1.0f && Players::Hostile(b->owner, Players::Local());
            const float shownProgress = hiddenProgress ? 0.5f : b->buildProgress;
            renderer.DrawBuildingWorld(
                btype,
                bx,
                by,
                0,
                camera,
                b->faction,
                bAnim,
                shownProgress,
                false,
                hpRatio,
                b->turretFacing,
                isAttacking,
                // (an opponent's missing power is not shown: no offline sign, its parts keep working)
                b->isPowered || Players::Hostile(b->owner, Players::Local()));
            renderer.SetDrawOwner(b->owner);
            renderer.DrawBuildingWorld(
                btype,
                bx,
                by,
                1,
                camera,
                b->faction,
                bAnim,
                shownProgress,
                false,
                hpRatio,
                b->turretFacing,
                isAttacking,
                // (an opponent's missing power is not shown: no offline sign, its parts keep working)
                b->isPowered || Players::Hostile(b->owner, Players::Local()));
            renderer.SetAnimatedPartsVisible(true);
            renderer.SetSpriteGlow({255, 255, 255, 255}, 0.0f);
        }
    }

    // Bars on top of every sprite
    for (const DepthItem &it : items)
    {
        // The health and the construction progress of an opponent are not for the player's eyes (not even with Alt)
        if (!debugMode && Players::Hostile(it.unit ? it.unit->owner : (it.building ? it.building->owner : kNoPlayer), Players::Local()))
            continue;
        if (const Unit *u = it.unit)
        {
            // (the selection bubble shows the health of a selected unit by its colour)
            bool showUnitHp = showAllHealth || (debugMode && (u->currentHp < u->maxHp || u->selected));
            if (showUnitHp)
            {
                float barW = 24.0f;
                float barH = 3.0f;
                float barX = u->GetWorldPixelX() + (TILE_WIDTH * 0.5f) - barW * 0.5f;
                float barY = u->GetWorldPixelY() - 10.0f;
                float hpPct = std::clamp(static_cast<float>(u->currentHp) / u->maxHp, 0.0f, 1.0f);
                renderer.FillRectWorld(barX, barY, barW, barH, camera, {60, 0, 0, 200});
                renderer.FillRectWorld(barX, barY, barW * hpPct, barH, camera, {0, 230, 0, 255});
            }
            continue;
        }
        const Building *b = it.building;
        if (!b)
            continue;
        if (b->buildProgress < 1.0f)
        {
            // No progress bar over an enemy construction
            if (fogEnabled && Players::Hostile(b->owner, Players::Local()))
                continue;
            float barW = b->buildWidth * TILE_WIDTH;
            float barH = 4.0f;
            float barX = b->tileX * TILE_WIDTH;
            float barY = b->tileY * TILE_HEIGHT - 12.0f;

            float progPct = std::clamp(b->buildProgress, 0.0f, 1.0f);
            renderer.FillRectWorld(barX, barY, barW, barH, camera, {20, 30, 45, 220});
            renderer.FillRectWorld(barX, barY, barW * progPct, barH, camera, {255, 215, 0, 255});
            renderer.DrawRectWorld(barX, barY, barW, barH, camera, {255, 255, 255, 200});

            bool showBuildingHp = showAllHealth || (debugMode && (b->currentHp < b->maxHp || b->selected));
            if (showBuildingHp)
            {
                float hpPct = std::clamp(static_cast<float>(b->currentHp) / b->maxHp, 0.0f, 1.0f);
                renderer.FillRectWorld(barX, barY + 5.0f, barW, 3.0f, camera, {60, 0, 0, 200});
                renderer.FillRectWorld(barX, barY + 5.0f, barW * hpPct, 3.0f, camera, {0, 230, 0, 255});
            }
        }
        else
        {
            bool showBuildingHp = showAllHealth || b->selected || (debugMode && b->currentHp < b->maxHp);
            if (showBuildingHp)
            {
                float barW = b->buildWidth * TILE_WIDTH;
                float barH = b->selected ? 4.0f : 3.0f;
                float barX = b->tileX * TILE_WIDTH;
                float barY = b->tileY * TILE_HEIGHT - (b->selected ? 8.0f : 6.0f);

                float hpPct = std::clamp(static_cast<float>(b->currentHp) / b->maxHp, 0.0f, 1.0f);
                renderer.FillRectWorld(barX, barY, barW, barH, camera, {60, 0, 0, 200});
                renderer.FillRectWorld(
                    barX, barY, barW * hpPct, barH, camera, b->selected ? SelectionHealthColor(hpPct) : SDL_Color {0, 230, 0, 255});
                if (b->selected)
                    renderer.DrawRectWorld(barX, barY, barW, barH, camera, {255, 255, 255, 200});
            }
        }
    }

    // 4. Render Smoke Puffs
    renderer.SetDrawOwner(kNoPlayer);
    for (const auto &puff : m_smokePuffs)
    {
        if (!puff.active)
            continue;
        int frame = static_cast<int>((puff.lifetime / puff.maxLifetime) * 6.0f);
        renderer.DrawSmokePuffWorld(frame, puff.x, puff.y, camera);
    }

    // 5..6. Projectiles and explosions (drawn again by the glow pass of the shader effects)
    RenderEmissive(renderer, camera);
}

void EntityManager::CollectLights(const TileMap &map, bool fog, std::vector<LightSpot> &out) const
{
    using Kind = LightSpot::Kind;
    out.clear();
    auto seen = [&](float x, float y) {
        return !fog
               || map.GetFogState(static_cast<int>(x) / TILE_WIDTH, static_cast<int>(y) / TILE_HEIGHT) == FogState::Visible;
    };
    for (const auto &exp : m_explosions)
    {
        if (!exp.active || exp.delay > 0.0f || !seen(exp.x, exp.y))
            continue;
        LightSpot l;
        l.kind = Kind::Explosion;
        l.x = exp.x;
        l.y = exp.y;
        l.age = exp.totalFrames > 0 ? static_cast<float>(exp.currentFrame) / static_cast<float>(exp.totalFrames) : 0.0f;
        out.push_back(l);
    }
    for (const auto &burst : m_flameBursts)
    {
        if (seen(burst.startX, burst.startY))
            out.push_back({Kind::FlameStart, burst.startX, burst.startY});
        if (seen(burst.targetX, burst.targetY))
            out.push_back({Kind::FlameEnd, burst.targetX, burst.targetY});
    }
    for (const auto &proj : m_projectiles)
    {
        if (proj.active && (proj.type == ProjectileType::Rocket || proj.type == ProjectileType::Shell) && seen(proj.x, proj.y))
            out.push_back({Kind::Projectile, proj.x, proj.y});
    }
    for (const auto &flash : m_muzzleFlashes)
    {
        if (!seen(flash.x, flash.y))
            continue;
        LightSpot l;
        l.kind = Kind::MuzzleFlash;
        l.x = flash.x;
        l.y = flash.y;
        l.age = flash.age;
        out.push_back(l);
    }
    // Buildings in sight (one of their tiles is enough: a building hidden in the fog gives itself away by no light)
    for (const auto &ptr : m_entities.Raw())
    {
        const Building *b = ptr ? ptr->AsBuilding() : nullptr;
        if (!b || !b->alive || b->state == ActionState::Die)
            continue;
        bool visible = !fog;
        for (int ty = b->tileY; !visible && ty < b->tileY + b->buildHeight; ++ty)
            for (int tx = b->tileX; !visible && tx < b->tileX + b->buildWidth; ++tx)
                visible = map.GetFogState(tx, ty) == FogState::Visible;
        if (!visible)
            continue;
        LightSpot l;
        l.kind = Kind::Building;
        l.x = static_cast<float>(b->tileX * TILE_WIDTH);
        l.y = static_cast<float>(b->tileY * TILE_HEIGHT);
        l.w = static_cast<float>(b->buildWidth * TILE_WIDTH);
        l.h = static_cast<float>(b->buildHeight * TILE_HEIGHT);
        l.powered = b->isPowered;
        l.complete = b->buildProgress >= 1.0f;
        out.push_back(l);
    }
}

void EntityManager::RenderEmissive(Renderer &renderer, const Camera &camera) const
{
    // Projectiles
    for (const auto &proj : m_projectiles)
    {
        if (!proj.active)
            continue;
        const ProjectileSpec &spec = Projectiles::of(proj.type);
        if (proj.type == ProjectileType::Flame || spec.sprite <= 0)
            continue; // a flame is drawn as a FlameBurst below
        renderer.SetDrawOwner(proj.owner);
        renderer.DrawProjectileWorld(
            spec.sprite,
            spec.directions == 16 ? proj.direction : proj.dir32,
            proj.x,
            proj.y,
            camera,
            proj.faction,
            spec.directions);
    }

    // Flamethrower jets: the 12 frames of the flame over the jet's time
    for (const auto &burst : m_flameBursts)
    {
        const float angle = DetMath::Atan2(burst.targetY - burst.startY, burst.targetX - burst.startX);
        const float prog = burst.duration > 0.0f ? std::clamp(burst.age / burst.duration, 0.0f, 1.0f) : 1.0f;
        const int flameFrame = std::clamp(static_cast<int>(prog * 11.0f), 0, 11);
        renderer.SetDrawOwner(burst.owner);
        renderer.DrawAnimSequenceWorldRotated(59, flameFrame, burst.startX, burst.startY, angle, camera, burst.faction);
    }

    // 6. Render Explosions (with visual centering for bullet hits and impacts)
    renderer.SetDrawOwner(kNoPlayer);
    for (const auto &exp : m_explosions)
    {
        if (!exp.active || exp.delay > 0.0f)
            continue;
        float ox = 0.0f, oy = 0.0f;
        ExplosionCentering(exp.animId, ox, oy);
        renderer.DrawAnimSequenceWorld(exp.animId, exp.currentFrame, exp.x + ox, exp.y + oy, camera);
    }
}

} // namespace OpenArtOfWar
