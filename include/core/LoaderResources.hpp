#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace OpenArtOfWar
{

// The single place that knows where the game's resources are. Every path comes from the manifest
// assets/main.yaml (read through AssetStore, so it works from the directory and from the blob) and from nowhere
// else: nothing is compiled in. A key main.yaml lacks gives an empty path (or list), reported once.
//
// Manifest paths are relative to the assets root. The getters take that root (default "assets") so the
// editors, which may work on another copy of the assets, resolve the same layout below their own folder.
class LoaderResources
{
    public:
    static constexpr const std::uint32_t kMinimalVersionManifest = 1;
    static constexpr const char *kDefaultRoot = "assets";
    static constexpr const char *kManifestPath = "assets/main.yaml";

    // Sets the asset root directory and/or reads the manifest. Call after Assets::Init().
    // False when the manifest is missing or broken: there are no resources then.
    static bool Init();
    // Sets only the asset root used to resolve relative resource paths
    static void SetAssetRoot(const std::string &root);
    // Gets the current asset root
    static std::string AssetRoot();

    // True when main.yaml was found and parsed
    static bool HasManifest();
    // Reads main.yaml again, with the mods enabled now (Mods::Activate) merged into it
    static bool Reload();
    // The raw manifest text (empty when missing), e.g. for the network asset hash
    static const std::string &ManifestText();

    // ---- Single files ----
    static std::string LocaleDir();
    static std::string DefaultLanguage();
    static std::string Environment();
    static inline std::string environment()
    {
        return Environment();
    }
    static std::string UiTheme();
    static std::string FontRegular();
    static std::string FontBold();
    // Folder of the per-object sprites (sprite_<name>.png + sprite_<name>_meta.yaml): manifest sprites.dir
    static std::string SpritesDir();
    // Named UI pictures ("logo", "title", "icon") in manifest order
    static std::vector<std::pair<std::string, std::string>> UiTextures();
    // A music file named in the tracklist by its path below the assets root
    static std::string AudioFile(const std::string &name);
    static std::string Tracklist();
    static std::string AudioLicense();
    // The loop of the surroundings in a match (audio.ambient_day / audio.ambient_night; empty: none)
    static std::string Ambient(bool night);
    // Sound effects: event settings (audio_sfx.yaml) and every clip <event>_<n>.ogg (sfx.clips)
    static std::string SfxConfig();
    static std::vector<std::string> SfxClips();
    // Voice lines (unit replies, announcer): settings (audio_voices.yaml) and every <event>_<n>.ogg of a language
    // (voices.clips.<language>); lines missing in a language come from the fallback one
    static std::string VoicesConfig();
    static std::vector<std::string> VoiceClips(const std::string &language);
    static std::string VoicesFallbackLanguage();
    static std::string AiDifficulty();
    static inline std::string aiDifficulty()
    {
        return AiDifficulty();
    }
    static std::string Effects();
    // How the night of a match is lit (data/nights.yaml)
    static std::string Nights();
    // How projectiles fly and hit (characters/projectiles.yaml)
    static std::string Projectiles();
    static inline std::string projectiles()
    {
        return Projectiles();
    }
    // Hard per-player building limits (data/limits.yaml)
    static std::string Limits();
    // Tech levels: buildings, units and researches allowed per level (data/capabilities.yaml)
    static std::string Capabilities();
    // Damage multipliers by projectile kind and target class (data/weapon_balance.yaml)
    static std::string WeaponBalance();
    static inline std::string weaponBalance()
    {
        return WeaponBalance();
    }
    // `faction` is "confederation" or "resistance"
    static std::string Research(const std::string &faction);
    static inline std::string research(const std::string &faction)
    {
        return Research(faction);
    }
    static std::string TileCollision();
    static inline std::string tileCollision()
    {
        return TileCollision();
    }

    // ---- Unit and building definitions ----
    // Every unit/building definition file (characters.files)
    static std::vector<std::string> CharacterFiles();

    // ---- Maps ----
    // The folder of the skirmish maps (where the editor saves; network maps must come from below it)
    static std::string MapsDir();
    // The folder of the campaign mission maps (maps.mission_dir, below MapsDir)
    static std::string MissionMapsDir();
    // "skirmish_2" -> "maps/skirmish_2.json"; a campaign map's name or an old alias ("m26", "resistance_mission_4")
    // -> "maps/mission/resistance_mission_4.json"; a name with a path separator is returned unchanged (".json" added,
    // an old mission path moved to the mission folder)
    static std::string MapPath(const std::string &name);
    // The map of a campaign mission (MissionData::mapFile) in the mission folder
    static std::string MissionMapPath(const std::string &name);
    static std::string TestMap();
    static std::string NetDefaultMap();
    // The shipped skirmish maps (maps.files), then in a blob install the player's own maps from the per-user directory
    static std::vector<std::string> ListMaps();
    // The campaign mission maps (maps.missions)
    static std::vector<std::string> ListMissionMaps();

    // ---- Network ----
    // Files whose contents must match between the players (net_hash), in manifest order
    static std::vector<std::string> NetHashFiles();
    // The simulation's part of the manifest (see net_hash): what must be the same for two players to agree
    static std::string NetManifestText();
    // Pictures, palettes, the music list and the UI colours (net_visual): never compared between players
    static std::vector<std::string> NetVisualFiles();

private:
    static const nlohmann::json &Manifest();
    static std::string join(const std::string &relative);
    static std::string Str(const char *section, const char *key);
    // A string array of the manifest (empty when missing)
    static std::vector<std::string> List(const char *section, const char *key);
    static std::vector<std::string> Joined(std::vector<std::string> files);
};

} // namespace OpenArtOfWar
