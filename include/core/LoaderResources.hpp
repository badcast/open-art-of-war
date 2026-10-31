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
        static bool init(const std::string &manifestPath = kManifestPath);
        // True when main.json was found and parsed
        static bool hasManifest();
        // The raw manifest text (empty when missing), e.g. for the network asset hash
        static const std::string &manifestText();

        // ---- Single files ----
        static std::string localeDir(const std::string &root = kDefaultRoot);
        static std::string defaultLanguage();
        static std::string environment(const std::string &root = kDefaultRoot);
        static std::string uiTheme(const std::string &root = kDefaultRoot);
        static std::string fontRegular(const std::string &root = kDefaultRoot);
        static std::string fontBold(const std::string &root = kDefaultRoot);
        static std::string spriteFrames(const std::string &root = kDefaultRoot);
        static int spriteSheetCount();
        static std::string spriteSheet(int index, const std::string &root = kDefaultRoot);
        // Named UI pictures ("logo", "title", "icon") in manifest order
        static std::vector<std::pair<std::string, std::string>> uiTextures(const std::string &root = kDefaultRoot);
        static std::string graphicsData(const std::string &root = kDefaultRoot);
        static std::string audioDir(const std::string &root = kDefaultRoot);
        static std::string audioFile(const std::string &name, const std::string &root = kDefaultRoot);
        static std::string tracklist(const std::string &root = kDefaultRoot);
        static std::string audioLicense(const std::string &root = kDefaultRoot);
        // Sound effects: event settings (sfx.json), the folder of <event>_<n>.ogg, their credits
        static std::string sfxConfig(const std::string &root = kDefaultRoot);
        static std::string sfxDir(const std::string &root = kDefaultRoot);
        static std::string sfxCredits(const std::string &root = kDefaultRoot);
        // Voice lines (unit replies, announcer): settings (voices.json) and the folder of a language's
        // <event>_<n>.ogg (audio/voices/<language>); lines missing in a language come from the fallback one
        static std::string voicesConfig(const std::string &root = kDefaultRoot);
        static std::string voicesDir(const std::string &language, const std::string &root = kDefaultRoot);
        static std::string voicesFallbackLanguage();
        static std::string aiDifficulty(const std::string &root = kDefaultRoot);
        static std::string effects(const std::string &root = kDefaultRoot);
        // How projectiles fly and hit (characters/projectiles.yaml)
        static std::string projectiles(const std::string &root = kDefaultRoot);
        // `faction` is "confederation" or "resistance"
        static std::string research(const std::string &faction, const std::string &root = kDefaultRoot);
        static std::string tileCollision(const std::string &root = kDefaultRoot);

        // ---- Unit and building definitions ----
        static std::string charactersDir(const std::string &root = kDefaultRoot);
        static std::vector<std::string> characterExtensions();
        // Files in the characters folder that are not unit/building definitions
        static bool isCharacterExcluded(const std::string &fileName);
        // Every unit/building definition file, sorted per extension
        static std::vector<std::string> characterFiles(const std::string &root = kDefaultRoot);

        // ---- Maps ----
        static std::string mapsDir(const std::string &root = kDefaultRoot);
        static std::string mapExtension();
        // "m26" -> "assets/maps/m26.json"; a name with ".json" or a path separator is returned unchanged
        static std::string mapPath(const std::string &name, const std::string &root = kDefaultRoot);
        static std::string testMap(const std::string &root = kDefaultRoot);
        static std::string netDefaultMap(const std::string &root = kDefaultRoot);
        static std::vector<std::string> listMaps(const std::string &root = kDefaultRoot);

        // ---- Network ----
        // Files whose contents must match between the players (asset hash), in a stable order
        static std::vector<std::string> netHashFiles(const std::string &root = kDefaultRoot);

    private:
        static const nlohmann::json &manifest();
        static std::string join(const std::string &root, const std::string &relative);
        static std::string str(const char *section, const char *key, const char *fallback);
    };

} // namespace OpenArtOfWar
