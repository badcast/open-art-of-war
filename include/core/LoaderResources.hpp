#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace OpenArtOfWar
{

    // The single place that knows where the game's resources are. Every path comes from the manifest
    // assets/main.json (read through AssetStore, so it works from the directory and from the blob); the
    // values compiled in here are only the fallback for a missing manifest or a missing key.
    //
    // Manifest paths are relative to the assets root. The getters take that root (default "assets") so the
    // editors, which may work on another copy of the assets, resolve the same layout below their own folder.
    class LoaderResources
    {
    public:
        static constexpr const char *kDefaultRoot = "assets";
        static constexpr const char *kManifestPath = "assets/main.json";

        // Reads the manifest (again). Call after Assets::init(); the getters load it on first use otherwise.
        // False when the manifest is missing or broken: the built-in layout is used then.
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
        static int SpriteSheetCount();
        static std::string SpriteSheet(int index, const std::string &root = kDefaultRoot);
        // Named UI pictures ("logo", "title", "icon") in manifest order
        static std::vector<std::pair<std::string, std::string>> UiTextures(const std::string &root = kDefaultRoot);
        static std::string GraphicsData(const std::string &root = kDefaultRoot);
        static std::string AudioDir(const std::string &root = kDefaultRoot);
        static std::string AudioFile(const std::string &name, const std::string &root = kDefaultRoot);
        static std::string Tracklist(const std::string &root = kDefaultRoot);
        static std::string AudioLicense(const std::string &root = kDefaultRoot);
        // Sound effects: event settings (sfx.json), the folder of <event>_<n>.ogg, their credits
        static std::string SfxConfig(const std::string &root = kDefaultRoot);
        static std::string SfxDir(const std::string &root = kDefaultRoot);
        static std::string SfxCredits(const std::string &root = kDefaultRoot);
        // Voice lines (unit replies, announcer): settings (voices.json) and the folder of a language's
        // <event>_<n>.ogg (audio/voices/<language>); lines missing in a language come from the fallback one
        static std::string VoicesConfig(const std::string &root = kDefaultRoot);
        static std::string VoicesDir(const std::string &language, const std::string &root = kDefaultRoot);
        static std::string VoicesFallbackLanguage();
        static std::string aiDifficulty(const std::string &root = kDefaultRoot);
        static std::string Effects(const std::string &root = kDefaultRoot);
        // How projectiles fly and hit (characters/projectiles.yaml)
        static std::string projectiles(const std::string &root = kDefaultRoot);
        // `faction` is "confederation" or "resistance"
        static std::string research(const std::string &faction, const std::string &root = kDefaultRoot);
        static std::string tileCollision(const std::string &root = kDefaultRoot);

        // ---- Unit and building definitions ----
        static std::string CharactersDir(const std::string &root = kDefaultRoot);
        static std::vector<std::string> CharacterExtensions();
        // Files in the characters folder that are not unit/building definitions
        static bool IsCharacterExcluded(const std::string &fileName);
        // Every unit/building definition file, sorted per extension
        static std::vector<std::string> CharacterFiles(const std::string &root = kDefaultRoot);

        // ---- Maps ----
        static std::string MapsDir(const std::string &root = kDefaultRoot);
        static std::string MapExtension();
        // "m26" -> "assets/maps/m26.json"; a name with ".json" or a path separator is returned unchanged
        static std::string MapPath(const std::string &name, const std::string &root = kDefaultRoot);
        static std::string TestMap(const std::string &root = kDefaultRoot);
        static std::string NetDefaultMap(const std::string &root = kDefaultRoot);
        static std::vector<std::string> ListMaps(const std::string &root = kDefaultRoot);

        // ---- Network ----
        // Files whose contents must match between the players (asset hash), in a stable order
        static std::vector<std::string> NetHashFiles(const std::string &root = kDefaultRoot);

    private:
        static const nlohmann::json &Manifest();
        static std::string join(const std::string &root, const std::string &relative);
        static std::string Str(const char *section, const char *key, const char *fallback);
    };

} // namespace OpenArtOfWar
