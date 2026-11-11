#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Mods: data packages over the installed game data (docs/modding.md).
//
// A mod is either an archive "<name>.mod" (a tar.zst, packed like data.blob: tools/pack_assets.py --mod) or a folder
// "<name>/" with the same contents. Its root holds mod.yaml and files laid out as below assets/ ("sprites/x.png",
// "characters/y.yaml"): a file replaces the installed one with that path, a new one is simply added. mod.yaml may
// also add to the manifest (main.yaml) in its "manifest:" section: maps merge, lists grow, values are replaced.
// Mods hold data only, never code.
//
// They are looked for in "<the game's folder>/mods" and "<the user directory>/mods". Of two mods with the same id the
// higher version counts; on the same version the one in the user directory.
//
// Which mods are enabled, and in what order, is the player's choice (Settings::enabledMods); a later mod wins over an
// earlier one. Lookup order of a data file: session overlay (network / replay data) > mods > the installed data.
namespace OpenArtOfWar::Mods
{

struct ModInfo
{
    std::string id; // [a-z0-9_]
    std::string name;
    std::string version; // "1.2.0"
    std::string author;
    std::string description;
    std::string gameVersion; // the game version it is made for (informative)
    std::string path;        // the .mod file or the folder
    bool archive {false};
    bool fromUser {false}; // found in the user directory (else next to the game)
    std::string error;     // why it cannot be used (a broken mod.yaml, no id...); empty when fine
    std::string preview;   // relative path to preview image (e.g. "preview.png")
    nlohmann::json manifest = nlohmann::json::object(); // its "manifest:" section
};

// Reads the raw bytes of a mod's preview image (from folder or .mod archive)
bool LoadPreview(const ModInfo &mod, std::vector<std::uint8_t> &out);

// The folders searched (tests use their own); empty strings: the defaults
void SetSearchDirs(const std::string &appMods, const std::string &userMods);
std::vector<std::string> SearchDirs();

// Every mod found, one per id (the higher version, the user's on a tie), sorted by name
std::vector<ModInfo> Scan();

// -1 / 0 / 1: "1.10" > "1.9", numbers compared as numbers, the rest as text
int CompareVersions(const std::string &a, const std::string &b);

// A path inside a mod as a key below assets/ ("./sprites/../sprites/x.png" -> "sprites/x.png"; an "assets/" in front is
// dropped); false for anything that would leave it (".." on top, absolute, drive letters, backslashes) or is empty
bool KeyOf(const std::string &path, std::string &key);

struct ActivateReport
{
    std::vector<std::string> problems; // ids not found / unusable
    std::map<std::string, std::vector<std::string>>
        conflicts; // a file several enabled mods bring -> their ids, in order
};
// Puts these mods (in this order, the last one wins) over the installed data; an empty list takes every mod away.
// False when one of them could not be used (the others are still in place). Call LoaderResources::Reload() after.
bool Activate(const std::vector<std::string> &orderedIds, ActivateReport *report = nullptr);
void Deactivate();
const std::vector<ModInfo> &Active();
// "id 1.0, other 2.1" (the enabled mods, in order); empty when none
std::string ActiveSummary();

// The manifest of `add` merged into `base`: objects key by key, arrays get the items they lack, anything else replaced
void MergeManifest(nlohmann::json &base, const nlohmann::json &add);

} // namespace OpenArtOfWar::Mods
