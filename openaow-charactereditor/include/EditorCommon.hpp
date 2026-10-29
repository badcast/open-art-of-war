#pragma once

#include "core/Config.hpp"
#include "core/Localization.hpp"
#include "core/Utility.hpp"
#include "graphics/Camera.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/TextureManager.hpp"
#include "game/EntityManager.hpp"
#include "game/Pathfinder.hpp"
#include "game/TileMap.hpp"

#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <imgui.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace CharEditor {

namespace fs = std::filesystem;

// Tooltip of the last widget: `key` is the widget label (msgid) or its ##id; widgets without a hint get none
void fieldHint(const char* key);

// Shows the hint of a widget just drawn and passes its result on: if (hinted(ImGui::Button(_("Idle")), "Idle")) ...
inline bool hinted(bool result, const char* key) {
    fieldHint(key);
    return result;
}

// Result of the folder dialog (SDL calls back on its own thread)
struct FolderPick {
    std::mutex mutex;
    std::string path;
    bool ready{false};
};

struct CharacterFile {
    fs::path path;
    std::string name;
    std::string faction;
    std::string type;
    int numericId{-1};
    std::string stringId;
};

enum class PreviewMode {
    Idle,
    Move,
    Attack,
    Die,
    Wreck   // the remains left behind, stage by stage
};

// Wreck ("остов") composite families: first composite of the family and the number of burial stages
struct WreckFamily {
    int composite;
    int stages;
    const char* label; // gettext msgid
};

inline const WreckFamily kWreckFamilies[] = {
    {1759, 5, "Light vehicle"},
    {1764, 5, "Vehicle"},
    {1769, 4, "Heavy vehicle"},
    {1741, 6, "Rubble 2x2"},
    {1747, 6, "Rubble 3x3"},
    {1753, 6, "Rubble 4x4"},
};

fs::path findAssetRoot();

std::string yamlString(const YAML::Node& node, const char* key, const std::string& fallback = {});

int yamlInt(const YAML::Node& node, const char* key, int fallback = 0);

float yamlFloat(const YAML::Node& node, const char* key, float fallback = 0.0f);

bool yamlBool(const YAML::Node& node, const char* key, bool fallback = false);

// The main gun of a combat block: combat.weapons[0]; an undefined node when there are no weapons.
// Damage, range, delay, projectile and muzzle live only in weapons (combat keeps turret / FX settings).
YAML::Node mainWeapon(const YAML::Node& combat);

// Same, creating weapons[0] for editing when the block has none yet
YAML::Node ensureMainWeapon(YAML::Node combat);

bool weaponRandomDelay(const YAML::Node& w);

// Delay between attack cycles: fixed `cooldown`, or random in [cooldown_min, cooldown_max].
// sample=false gives the average (for DPS figures), sample=true draws one delay (for the shot simulation).
float weaponCooldown(const YAML::Node& w, bool sample, float fallback = 1.0f);

// Files store only archetype_id / building_index; faction, class and internal type are what the game
// derives from them (OpenArtOfWar::Utility tables), never typed in by hand
bool docIsBuilding(const YAML::Node& doc);

OpenArtOfWar::Faction docFaction(const YAML::Node& doc);

// Wreck of a character: from its YAML (animations.wreck_composite / wreck_stages) or the game's default
void wreckOf(const YAML::Node& doc, int& composite, int& stages);

void getDefaultTurretOffset(int archetypeId, bool isBuilding, float& outX, float& outY);

// Muzzle (localePosition) and turret pivot offsets of one character, read from its YAML
struct VisualOffsets {
    float muzzleX{0.0f};
    float muzzleY{0.0f};
    float turretX{0.0f};
    float turretY{0.0f};
    bool customTurret{false};
};

// Takes the node by value: yaml-cpp lookups of missing keys need a mutable node
VisualOffsets readVisualOffsets(YAML::Node doc);

// Combat and economy figures used by Compare mode
struct CharacterStats {
    bool building{false};
    std::string name;
    float hp{0.0f};
    float damage{0.0f};
    int burst{1};
    float cooldown{0.0f};
    float dps{0.0f};
    float range{0.0f};
    float minRange{0.0f};
    float sight{0.0f};
    float speed{0.0f};
    float cost{0.0f};
    float buildTime{0.0f};
    float power{0.0f};
    int width{1};
    int height{1};
};

// Damage per second of one weapon: a burst of N shots every (cooldown + burst span)
float weaponDps(YAML::Node weapon);

CharacterStats computeStats(YAML::Node doc, bool siege);

enum class EditorMode {
    Preview,
    Compare,
    Simulation,
    Research,  // technology trees of both factions
    Explosions, // how buildings and vehicles blow up (assets/characters/effects.yaml)
    Wrecks,     // the remains they leave (the same file)
    GameUi,     // colors of the interface (assets/characters/game_ui.yaml)
    Environment, // fog, construction, network, overlay and font (assets/data/game_environment.json)
    AiDifficulty, // AI levels (assets/characters/difficulty.yaml)
    SpriteTools // sprite browser and exports for repacking the assets
};

} // namespace CharEditor
