#pragma once

#include "core/Config.hpp"
#include "core/Localization.hpp"
#include "core/Utility.hpp"
#include "graphics/Camera.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/SpriteRegistry.hpp"
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

namespace CharEditor
{

namespace fs = std::filesystem;

// Tooltip of the last widget: `key` is the widget label (msgid) or its ##id; widgets without a hint get none
void FieldHint(const char *key);

// Shows the hint of a widget just drawn and passes its result on: if (hinted(ImGui::Button(_("Idle")), "Idle")) ...
inline bool Hinted(bool result, const char *key)
{
    FieldHint(key);
    return result;
}

// Camera of a sprite canvas (Sprite Tools, Sprite Merge). A reset waits for the next draw: it needs the canvas size
enum class SpriteCamera
{
    None,
    Fit,    // the whole content
    Actual, // 1:1
    Frame   // the focused rectangle
};

// Camera of a canvas drawn at `origin` / `avail` (an invisible button over it gave hovered / active / the double middle
// click): the Fit / 1:1 / Frame buttons in its top right corner, Home / F, wheel zoom about the cursor, panning with
// the right / middle button. `content` and `focus` (nullptr: none) are x, y, w, h in image pixels.
void SpriteCanvasCamera(
    const ImVec2 &origin,
    const ImVec2 &avail,
    bool hovered,
    bool active,
    bool middleDoubleClick,
    const float content[4],
    const float *focus,
    float &zoom,
    ImVec2 &pan,
    SpriteCamera &pending);

// Result of the folder dialog (SDL calls back on its own thread)
struct FolderPick
{
    std::mutex mutex;
    std::string path;
    bool ready {false};
};

struct CharacterFile
{
    fs::path path;
    std::string name;
    std::string faction;
    std::string type;
    int numericId {-1};
    std::string stringId;
};

enum class PreviewMode
{
    Idle,
    Move,
    Attack,
    Die,
    Wreck // the remains left behind, stage by stage
};

// Wreck (hull remains) families of the game: a sprite family of burial stages
struct WreckFamily
{
    const char *family; // "<object>.<family>"
    const char *label;  // gettext msgid
};

inline const WreckFamily kWreckFamilies[] = {
    {"fx_wreck_light.stages", "Light vehicle"},
    {"fx_wreck_vehicle.stages", "Vehicle"},
    {"fx_wreck_heavy.stages", "Heavy vehicle"},
    {"fx_rubble_small.stages", "Rubble 2x2"},
    {"fx_rubble_medium.stages", "Rubble 3x3"},
    {"fx_rubble_large.stages", "Rubble 4x4"},
};

// Sprites in the configs are written by name ("<object>.<item>", "<object>.<family>"); the editor draws them by the
// handle of the game's sprite tables
struct FamilyRef
{
    int start {-1};
    int count {0};
};
FamilyRef SpriteFamilyRef(const std::string &ref);                           // a family by name; {-1, 0} unknown
int SpriteRefHandle(const YAML::Node &value, OpenArtOfWar::SpriteKind kind); // -1: none / unknown
std::vector<int> SpriteRefHandles(const YAML::Node &list, OpenArtOfWar::SpriteKind kind);
// The name to write for a handle: the family for the first member of a family ("<object>.<family>")
std::string SpriteRefName(OpenArtOfWar::SpriteKind kind, int handle);
std::string SpriteRefList(OpenArtOfWar::SpriteKind kind, const std::vector<int> &handles); // "[a.b, c.d]"

fs::path FindAssetRoot();

template <typename T>
inline T YamlValue(const YAML::Node &node, const char *key, T fallback = {})
{
    try
    {
        return node[key].as<T>(fallback);
    }
    catch (...)
    {
        return fallback;
    }
}

std::string YamlString(const YAML::Node &node, const char *key, const std::string &fallback = {});

int YamlInt(const YAML::Node &node, const char *key, int fallback = 0);

float YamlFloat(const YAML::Node &node, const char *key, float fallback = 0.0f);

bool YamlBool(const YAML::Node &node, const char *key, bool fallback = false);

// The main gun of a combat block: combat.weapons[0]; an undefined node when there are no weapons.
// Damage, range, delay, projectile and muzzle live only in weapons (combat keeps turret / FX settings).
YAML::Node MainWeapon(const YAML::Node &combat);

// Same, creating weapons[0] for editing when the block has none yet
YAML::Node EnsureMainWeapon(YAML::Node combat);

bool WeaponRandomDelay(const YAML::Node &w);

// Delay between attack cycles: fixed `cooldown`, or random in [cooldown_min, cooldown_max].
// sample=false gives the average (for DPS figures), sample=true draws one delay (for the shot simulation).
float WeaponCooldown(const YAML::Node &w, bool sample, float fallback = 1.0f);

// Files store only archetype_id / building_index; faction, class and internal type are what the game
// derives from them (OpenArtOfWar::Utility tables), never typed in by hand
bool DocIsBuilding(const YAML::Node &doc);

OpenArtOfWar::Faction DocFaction(const YAML::Node &doc);

// Wreck of a character: from its YAML (animations.wreck_composite / wreck_stages) or the game's default
void WreckOf(const YAML::Node &doc, int &composite, int &stages);

void GetDefaultTurretOffset(int archetypeId, bool isBuilding, float &outX, float &outY);

// Muzzle (localePosition) and turret pivot offsets of one character, read from its YAML
struct VisualOffsets
{
    float muzzleX {0.0f};
    float muzzleY {0.0f};
    float turretX {0.0f};
    float turretY {0.0f};
    bool customTurret {false};
};

// Takes the node by value: yaml-cpp lookups of missing keys need a mutable node
VisualOffsets ReadVisualOffsets(YAML::Node doc);

// Combat and economy figures used by Compare mode
struct CharacterStats
{
    bool building {false};
    std::string name;
    float hp {0.0f};
    float damage {0.0f};
    int burst {1};
    float cooldown {0.0f};
    float dps {0.0f};
    float range {0.0f};
    float minRange {0.0f};
    float sight {0.0f};
    float speed {0.0f};
    float cost {0.0f};
    float buildTime {0.0f};
    float power {0.0f};
    int width {1};
    int height {1};
};

// Damage per second of one weapon: a burst of N shots every (cooldown + burst span)
float WeaponDps(YAML::Node weapon);

CharacterStats ComputeStats(YAML::Node doc, bool siege);

// Timing of the pictures (animations: block of a character file); the same defaults and rules as the game
struct AnimTiming
{
    float moveFrame {0.09f};   // seconds per walking frame
    float attackFrame {-1.0f}; // seconds per firing-pose frame (< 0: the archetype's default)
    float attackPose {-1.0f};  // the firing pose lasts this long after the first shot of a volley (< 0: by projectile)
    float burstPose {0.15f};   // ... after every later shot of a burst
    int flashMode {0};         // 0 loops, 1 once, 2 once and holds
    int flashFrames {0};       // 0 = the whole animation
};
AnimTiming ReadAnimTiming(const YAML::Node &doc);
// Seconds per firing-pose frame with the archetype's default applied
float AttackFrameTime(const AnimTiming &t, int archetypeId);
// Default length of the firing pose after the first shot: by projectile (hit-scan 0.12 s, others 0.22 s)
float DefaultAttackPose(const std::string &projectile);

enum class EditorMode
{
    Preview,
    Compare,
    Simulation,
    Research,     // technology trees of both factions
    Explosions,   // how buildings and vehicles blow up (assets/characters/effects.yaml)
    Wrecks,       // the remains they leave (the same file)
    Projectiles,  // how bullets, sniper rounds, shells, rockets and flames fly and hit
                  // (assets/characters/projectiles.yaml)
    GameUi,       // colors of the interface (assets/characters/game_ui.yaml)
    Environment,  // fog, construction, network, overlay and font (assets/data/game_environment.json)
    AiDifficulty, // AI levels (assets/characters/difficulty.yaml)
    Animator,     // timing of the pictures of the selected character: pose, muzzle flash, burst
    SpriteTools,  // sprite browser and exports for repacking the assets
    SpriteMerge   // several sprite objects into one, or frames baked into one picture
};

} // namespace CharEditor
