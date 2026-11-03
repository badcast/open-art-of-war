#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace OpenArtOfWar
{

// The single place that knows where the game's resources are. Every path comes from the manifest
// assets/main.json (read through AssetStore, so it works from the directory and from the blob) and from nowhere
// else: nothing is compiled in. A key main.json lacks gives an empty path (or list), reported once.
//
// Manifest paths are relative to the assets root. The getters take that root (default "assets") so the
// editors, which may work on another copy of the assets, resolve the same layout below their own folder.
class LoaderResources
{
public:
    static constexpr const char *kDefaultRoot = "assets";
    static constexpr const char *kManifestPath = "assets/main.json";

    // Reads the manifest (again). Call after Assets::init(); the getters load it on first use otherwise.
    // False when the manifest is missing or broken: there are no resources then.
    static bool Init(const std::string &manifestPath = kManifestPath);
    // True when main.json was found and parsed
    static bool HasManifest();
    // The raw manifest text (empty when missing), e.g. for the network asset hash
    static const std::string &ManifestText();

    // ---- Single files ----
    static std::string LocaleDir(const std::string &root = kDefaultRoot);
    static std::string DefaultLanguage();
    static std::string environment(const std::string &root = kDefaultRoot);
    static std::string UiTheme(const std::string &root = kDefaultRoot);
    static std::string FontRegular(const std::string &root = kDefaultRoot);
    static std::string FontBold(const std::string &root = kDefaultRoot);
    static std::string SpriteFrames(const std::string &root = kDefaultRoot);
    // The sprite sheets in page order (sprites.sheets); an empty path for a page that is not listed
    static int SpriteSheetCount();
    static std::string SpriteSheet(int index, const std::string &root = kDefaultRoot);
    // Named UI pictures ("logo", "title", "icon") in manifest order
    static std::vector<std::pair<std::string, std::string>> UiTextures(const std::string &root = kDefaultRoot);
    static std::string GraphicsData(const std::string &root = kDefaultRoot);
    // A music file named in the tracklist by its path below the assets root
    static std::string AudioFile(const std::string &name, const std::string &root = kDefaultRoot);
    static std::string Tracklist(const std::string &root = kDefaultRoot);
    static std::string AudioLicense(const std::string &root = kDefaultRoot);
    // The loop of the surroundings in a match (audio.ambient_day / audio.ambient_night; empty: none)
    static std::string Ambient(bool night, const std::string &root = kDefaultRoot);
    // Sound effects: event settings (audio_sfx.json) and every clip <event>_<n>.ogg (sfx.clips)
    static std::string SfxConfig(const std::string &root = kDefaultRoot);
    static std::vector<std::string> SfxClips(const std::string &root = kDefaultRoot);
    // Voice lines (unit replies, announcer): settings (audio_voices.json) and every <event>_<n>.ogg of a language
    // (voices.clips.<language>); lines missing in a language come from the fallback one
    static std::string VoicesConfig(const std::string &root = kDefaultRoot);
    static std::vector<std::string> VoiceClips(const std::string &language, const std::string &root = kDefaultRoot);
    static std::string VoicesFallbackLanguage();
    static std::string aiDifficulty(const std::string &root = kDefaultRoot);
    static std::string Effects(const std::string &root = kDefaultRoot);
    // How projectiles fly and hit (characters/projectiles.yaml)
    static std::string projectiles(const std::string &root = kDefaultRoot);
    // Damage multipliers by projectile kind and target class (data/weapon_balance.json)
    static std::string weaponBalance(const std::string &root = kDefaultRoot);
    // `faction` is "confederation" or "resistance"
    static std::string research(const std::string &faction, const std::string &root = kDefaultRoot);
    static std::string tileCollision(const std::string &root = kDefaultRoot);

    // ---- Unit and building definitions ----
    // Every unit/building definition file (characters.files)
    static std::vector<std::string> CharacterFiles(const std::string &root = kDefaultRoot);

    // ---- Maps ----
    // The folder of the maps (where the editor saves; network maps must come from there)
    static std::string MapsDir(const std::string &root = kDefaultRoot);
    // "m26" -> "assets/maps/m26.json"; a name with a path separator is returned unchanged (".json" added)
    static std::string MapPath(const std::string &name, const std::string &root = kDefaultRoot);
    static std::string TestMap(const std::string &root = kDefaultRoot);
    static std::string NetDefaultMap(const std::string &root = kDefaultRoot);
    // The shipped maps (maps.files), then in a blob install the player's own maps from the per-user directory
    static std::vector<std::string> ListMaps(const std::string &root = kDefaultRoot);

    // ---- Network ----
    // Files whose contents must match between the players (net_hash), in manifest order
    static std::vector<std::string> NetHashFiles(const std::string &root = kDefaultRoot);
    // The simulation's part of the manifest (see net_hash): what must be the same for two players to agree
    static std::string NetManifestText();
    // Pictures, palettes, the music list and the UI colours (net_visual): never compared between players
    static std::vector<std::string> NetVisualFiles(const std::string &root = kDefaultRoot);

private:
    static const nlohmann::json &Manifest();
    static std::string join(const std::string &root, const std::string &relative);
    static std::string Str(const char *section, const char *key);
    // A string array of the manifest (empty when missing)
    static std::vector<std::string> List(const char *section, const char *key);
    static std::vector<std::string> SheetsByPage(); // the sprite sheets by page number (gaps stay empty)
    static std::vector<std::string> Joined(const std::string &root, std::vector<std::string> files);
};

} // namespace OpenArtOfWar
