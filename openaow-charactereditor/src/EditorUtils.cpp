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

fs::path FindAssetRoot() {
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

std::string YamlString(const YAML::Node& node, const char* key, const std::string& fallback) {
    try {
        return node[key].as<std::string>(fallback);
    } catch (...) {
        return fallback;
    }
}

int YamlInt(const YAML::Node& node, const char* key, int fallback) {
    try {
        return node[key].as<int>(fallback);
    } catch (...) {
        return fallback;
    }
}

float YamlFloat(const YAML::Node& node, const char* key, float fallback) {
    try {
        return node[key].as<float>(fallback);
    } catch (...) {
        return fallback;
    }
}

bool YamlBool(const YAML::Node& node, const char* key, bool fallback) {
    try {
        return node[key].as<bool>(fallback);
    } catch (...) {
        return fallback;
    }
}

YAML::Node MainWeapon(const YAML::Node& combat) {
    if (combat && combat["weapons"] && combat["weapons"].IsSequence() && combat["weapons"].size() > 0) {
        return combat["weapons"][0];
    }
    return YAML::Node();
}

YAML::Node EnsureMainWeapon(YAML::Node combat) {
    if (!combat["weapons"]) combat["weapons"] = YAML::Node(YAML::NodeType::Sequence);
    // Another shape than the editor knows: leave it alone (edits land in a scratch weapon that is not saved)
    if (!combat["weapons"].IsSequence()) return YAML::Node(YAML::NodeType::Map);
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

bool WeaponRandomDelay(const YAML::Node& w) { return YamlString(w, "cooldown_mode", "fixed") == "random"; }

float WeaponCooldown(const YAML::Node& w, bool sample, float fallback) {
    if (!WeaponRandomDelay(w)) return YamlFloat(w, "cooldown", fallback);
    const float lo = YamlFloat(w, "cooldown_min", YamlFloat(w, "cooldown", fallback));
    const float hi = std::max(lo, YamlFloat(w, "cooldown_max", lo));
    return sample ? OpenArtOfWar::Utility::RandomFloat(lo, hi) : 0.5f * (lo + hi);
}

bool DocIsBuilding(const YAML::Node& doc) {
    return YamlString(doc, "type") == "building" || (doc["building_index"].IsDefined() && !doc["archetype_id"].IsDefined());
}

OpenArtOfWar::Faction DocFaction(const YAML::Node& doc) {
    if (DocIsBuilding(doc)) {
        const auto* k = OpenArtOfWar::Utility::buildingKindInfo(YamlInt(doc, "building_index", -1));
        return k ? k->faction : OpenArtOfWar::Faction::Neutral;
    }
    const auto* a = OpenArtOfWar::Utility::archetypeInfo(YamlInt(doc, "archetype_id", -1));
    return a ? a->faction : OpenArtOfWar::Faction::Neutral;
}

void WreckOf(const YAML::Node& doc, int& composite, int& stages) {
    const YAML::Node anim = doc["animations"];
    if (YamlString(doc, "type") == "building") {
        OpenArtOfWar::Renderer::DefaultBuildingWreck(YamlInt(doc["size"], "width", 2), YamlInt(doc["size"], "height", 2), composite, stages);
    } else {
        const OpenArtOfWar::EffectsConfig::Wreck w =
            OpenArtOfWar::EffectsConfig::Get().WreckOfVehicle(YamlInt(doc, "archetype_id", 0));
        composite = w.composite;
        stages = w.stages;
    }
    composite = YamlInt(anim, "wreck_composite", composite);
    stages = std::max(1, YamlInt(anim, "wreck_stages", stages));
}

void GetDefaultTurretOffset(int archetypeId, bool isBuilding, float& outX, float& outY) {
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

VisualOffsets ReadVisualOffsets(YAML::Node doc) {
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
    const YAML::Node w0 = MainWeapon(combat);
    if (w0 && w0["localePosition"] && w0["localePosition"].IsSequence() && w0["localePosition"].size() >= 2) {
        try {
            v.muzzleX = w0["localePosition"][0].as<float>(0.0f);
            v.muzzleY = w0["localePosition"][1].as<float>(0.0f);
        } catch (...) {}
    }

    const bool isBld = (YamlString(doc, "type", "") == "building" || doc["building_index"].IsDefined());
    const int id = isBld ? YamlInt(doc, "building_index", -1) : YamlInt(doc, "archetype_id", -1);
    GetDefaultTurretOffset(id, isBld, v.turretX, v.turretY);
    v.customTurret = readPair("turret_offset", v.turretX, v.turretY) ||
                     readPair("dome_offset", v.turretX, v.turretY) ||
                     readPair("turret_pivot", v.turretX, v.turretY);
    return v;
}

float WeaponDps(YAML::Node weapon) {
    const float damage = YamlFloat(weapon, "damage", 0.0f);
    const int burst = std::max(1, YamlInt(weapon, "burst_count", 1));
    const float burstDelay = std::max(0.0f, YamlFloat(weapon, "burst_delay", 0.1f));
    const float cooldown = std::max(0.05f, WeaponCooldown(weapon, false));
    return damage * static_cast<float>(burst) / (cooldown + static_cast<float>(burst - 1) * burstDelay);
}

CharacterStats ComputeStats(YAML::Node doc, bool siege) {
    CharacterStats s;
    s.building = YamlString(doc, "type") == "building";
    s.name = YamlString(doc, "name", "?");

    YAML::Node stats = doc["stats"];
    YAML::Node combat = doc["combat"];
    YAML::Node production = doc["production"];
    s.hp = YamlFloat(stats, "max_hp", 100.0f);
    s.sight = YamlFloat(stats, "sight_radius", 5.0f);
    s.speed = s.building ? 0.0f : YamlFloat(stats, "move_speed", 1.4f);
    s.cost = YamlFloat(production, "cost", 100.0f);
    s.buildTime = YamlFloat(production, "build_time", 4.0f);
    s.power = YamlFloat(production, "power", 0.0f);
    s.width = YamlInt(doc["size"], "width", 1);
    s.height = YamlInt(doc["size"], "height", 1);

    // Siege form overrides combat and some stats
    if (siege && doc["abilities"] && doc["abilities"]["siege"].IsMap()) {
        YAML::Node sNode = doc["abilities"]["siege"];
        if (sNode["combat"].IsMap()) combat = sNode["combat"];
        if (sNode["stats"].IsMap()) {
            s.hp = YamlFloat(sNode["stats"], "max_hp", s.hp);
            s.sight = YamlFloat(sNode["stats"], "sight_radius", s.sight);
            s.speed = YamlFloat(sNode["stats"], "move_speed", 0.0f);
        } else {
            s.speed = 0.0f;
        }
    }

    const bool canAttack = YamlBool(combat, "can_attack", !s.building);
    if (!canAttack) return s;

    if (combat["weapons"] && combat["weapons"].IsSequence()) {
        bool first = true;
        for (size_t i = 0; i < combat["weapons"].size(); ++i) {
            YAML::Node w = combat["weapons"][i];
            if (!YamlBool(w, "enabled", true)) continue;
            s.dps += WeaponDps(w);
            s.range = std::max(first ? 0.0f : s.range, YamlFloat(w, "range", 0.0f));
            if (first) {
                s.damage = YamlFloat(w, "damage", 0.0f);
                s.burst = std::max(1, YamlInt(w, "burst_count", 1));
                s.cooldown = WeaponCooldown(w, false);
                s.minRange = YamlFloat(w, "min_range", 0.0f);
                first = false;
            }
        }
    }
    return s;
}

} // namespace CharEditor
