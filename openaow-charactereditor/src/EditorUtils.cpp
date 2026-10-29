#include "EditorCommon.hpp"
#include "game/Effects.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace CharEditor {

fs::path findAssetRoot() {
    std::vector<fs::path> candidates = {
        fs::current_path() / "assets",
        fs::current_path() / "../assets"
    };

    if (const char* executablePath = SDL_GetBasePath()) {
        const fs::path base(executablePath);
        candidates.push_back(base / "assets");
        candidates.push_back(base / "../assets");
    }

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate / "characters" / "sprites.json") &&
            fs::exists(candidate / "data" / "graphics_data.json")) {
            return fs::weakly_canonical(candidate);
        }
    }
    return {};
}

std::string yamlString(const YAML::Node& node, const char* key, const std::string& fallback) {
    try {
        return node[key].as<std::string>(fallback);
    } catch (...) {
        return fallback;
    }
}

int yamlInt(const YAML::Node& node, const char* key, int fallback) {
    try {
        return node[key].as<int>(fallback);
    } catch (...) {
        return fallback;
    }
}

float yamlFloat(const YAML::Node& node, const char* key, float fallback) {
    try {
        return node[key].as<float>(fallback);
    } catch (...) {
        return fallback;
    }
}

bool yamlBool(const YAML::Node& node, const char* key, bool fallback) {
    try {
        return node[key].as<bool>(fallback);
    } catch (...) {
        return fallback;
    }
}

YAML::Node mainWeapon(const YAML::Node& combat) {
    if (combat && combat["weapons"] && combat["weapons"].IsSequence() && combat["weapons"].size() > 0) {
        return combat["weapons"][0];
    }
    return YAML::Node();
}

YAML::Node ensureMainWeapon(YAML::Node combat) {
    if (!combat["weapons"] || !combat["weapons"].IsSequence()) combat["weapons"] = YAML::Node(YAML::NodeType::Sequence);
    if (combat["weapons"].size() == 0) {
        YAML::Node w(YAML::NodeType::Map);
        w["name"] = "Primary";
        w["projectile"] = "bullet";
        w["damage"] = 10;
        w["range"] = 4.0f;
        w["cooldown"] = 1.0f;
        w["enabled"] = true;
        combat["weapons"].push_back(w);
    }
    return combat["weapons"][0];
}

bool weaponRandomDelay(const YAML::Node& w) { return yamlString(w, "cooldown_mode", "fixed") == "random"; }

float weaponCooldown(const YAML::Node& w, bool sample, float fallback) {
    if (!weaponRandomDelay(w)) return yamlFloat(w, "cooldown", fallback);
    const float lo = yamlFloat(w, "cooldown_min", yamlFloat(w, "cooldown", fallback));
    const float hi = std::max(lo, yamlFloat(w, "cooldown_max", lo));
    return sample ? OpenArtOfWar::Utility::randomFloat(lo, hi) : 0.5f * (lo + hi);
}

bool docIsBuilding(const YAML::Node& doc) {
    return yamlString(doc, "type") == "building" || (doc["building_index"].IsDefined() && !doc["archetype_id"].IsDefined());
}

OpenArtOfWar::Faction docFaction(const YAML::Node& doc) {
    if (docIsBuilding(doc)) {
        const auto* k = OpenArtOfWar::Utility::buildingKindInfo(yamlInt(doc, "building_index", -1));
        return k ? k->faction : OpenArtOfWar::Faction::Neutral;
    }
    const auto* a = OpenArtOfWar::Utility::archetypeInfo(yamlInt(doc, "archetype_id", -1));
    return a ? a->faction : OpenArtOfWar::Faction::Neutral;
}

void wreckOf(const YAML::Node& doc, int& composite, int& stages) {
    const YAML::Node anim = doc["animations"];
    if (yamlString(doc, "type") == "building") {
        OpenArtOfWar::Renderer::defaultBuildingWreck(yamlInt(doc["size"], "width", 2), yamlInt(doc["size"], "height", 2), composite, stages);
    } else {
        const OpenArtOfWar::EffectsConfig::Wreck w =
            OpenArtOfWar::EffectsConfig::get().wreckOfVehicle(yamlInt(doc, "archetype_id", 0));
        composite = w.composite;
        stages = w.stages;
    }
    composite = yamlInt(anim, "wreck_composite", composite);
    stages = std::max(1, yamlInt(anim, "wreck_stages", stages));
}

void getDefaultTurretOffset(int archetypeId, bool isBuilding, float& outX, float& outY) {
    if (isBuilding) {
        outX = 0.0f;
        outY = -16.0f;
    } else {
        switch (archetypeId) {
            case 4:  outX = 0.0f; outY = -11.0f; break; // Coyote
            case 6:  outX = -2.0f; outY = -14.0f; break; // Assault Tank
            case 7:
            case 18: outX = 0.0f; outY = -14.0f; break; // Fortress
            case 9:  outX = -2.0f; outY = -14.0f; break; // Hammer
            case 10: outX = -2.0f; outY = -14.0f; break; // Heavy Tank
            case 11:
            case 17: outX = 0.0f; outY = -14.0f; break; // Zeus
            case 15: outX = -2.0f; outY = -14.0f; break; // Rhino Siege
            case 16: outX = -2.0f; outY = -15.0f; break; // Grad Deployed
            default: outX = 0.0f; outY = -13.5f; break;
        }
    }
}

VisualOffsets readVisualOffsets(YAML::Node doc) {
    VisualOffsets v;
    YAML::Node combat = doc["combat"];
    auto readPair = [&](const char* key, float& outX, float& outY) -> bool {
        if (!combat[key] || !combat[key].IsSequence() || combat[key].size() < 2) return false;
        try {
            outX = combat[key][0].as<float>(outX);
            outY = combat[key][1].as<float>(outY);
            return true;
        } catch (...) {
            return false;
        }
    };

    // Projectile exit point of the main gun
    const YAML::Node w0 = mainWeapon(combat);
    if (w0 && w0["localePosition"] && w0["localePosition"].IsSequence() && w0["localePosition"].size() >= 2) {
        try {
            v.muzzleX = w0["localePosition"][0].as<float>(0.0f);
            v.muzzleY = w0["localePosition"][1].as<float>(0.0f);
        } catch (...) {}
    }

    const bool isBld = (yamlString(doc, "type", "") == "building" || doc["building_index"].IsDefined());
    const int id = isBld ? yamlInt(doc, "building_index", -1) : yamlInt(doc, "archetype_id", -1);
    getDefaultTurretOffset(id, isBld, v.turretX, v.turretY);
    v.customTurret = readPair("turret_offset", v.turretX, v.turretY) ||
                     readPair("dome_offset", v.turretX, v.turretY) ||
                     readPair("turret_pivot", v.turretX, v.turretY);
    return v;
}

float weaponDps(YAML::Node weapon) {
    const float damage = yamlFloat(weapon, "damage", 0.0f);
    const int burst = std::max(1, yamlInt(weapon, "burst_count", 1));
    const float burstDelay = std::max(0.0f, yamlFloat(weapon, "burst_delay", 0.1f));
    const float cooldown = std::max(0.05f, weaponCooldown(weapon, false));
    return damage * static_cast<float>(burst) / (cooldown + static_cast<float>(burst - 1) * burstDelay);
}

CharacterStats computeStats(YAML::Node doc, bool siege) {
    CharacterStats s;
    s.building = yamlString(doc, "type") == "building";
    s.name = yamlString(doc, "name", "?");

    YAML::Node stats = doc["stats"];
    YAML::Node combat = doc["combat"];
    YAML::Node production = doc["production"];
    s.hp = yamlFloat(stats, "max_hp", 100.0f);
    s.sight = yamlFloat(stats, "sight_radius", 5.0f);
    s.speed = s.building ? 0.0f : yamlFloat(stats, "move_speed", 1.4f);
    s.cost = yamlFloat(production, "cost", 100.0f);
    s.buildTime = yamlFloat(production, "build_time", 4.0f);
    s.power = yamlFloat(production, "power", 0.0f);
    s.width = yamlInt(doc["size"], "width", 1);
    s.height = yamlInt(doc["size"], "height", 1);

    // Siege form overrides combat and some stats
    if (siege && doc["abilities"] && doc["abilities"]["siege"].IsMap()) {
        YAML::Node sNode = doc["abilities"]["siege"];
        if (sNode["combat"].IsMap()) combat = sNode["combat"];
        if (sNode["stats"].IsMap()) {
            s.hp = yamlFloat(sNode["stats"], "max_hp", s.hp);
            s.sight = yamlFloat(sNode["stats"], "sight_radius", s.sight);
            s.speed = yamlFloat(sNode["stats"], "move_speed", 0.0f);
        } else {
            s.speed = 0.0f;
        }
    }

    const bool canAttack = yamlBool(combat, "can_attack", !s.building);
    if (!canAttack) return s;

    if (combat["weapons"] && combat["weapons"].IsSequence()) {
        bool first = true;
        for (size_t i = 0; i < combat["weapons"].size(); ++i) {
            YAML::Node w = combat["weapons"][i];
            if (!yamlBool(w, "enabled", true)) continue;
            s.dps += weaponDps(w);
            s.range = std::max(first ? 0.0f : s.range, yamlFloat(w, "range", 0.0f));
            if (first) {
                s.damage = yamlFloat(w, "damage", 0.0f);
                s.burst = std::max(1, yamlInt(w, "burst_count", 1));
                s.cooldown = weaponCooldown(w, false);
                s.minRange = yamlFloat(w, "min_range", 0.0f);
                first = false;
            }
        }
    }
    return s;
}

} // namespace CharEditor
