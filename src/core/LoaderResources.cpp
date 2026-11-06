#include "core/LoaderResources.hpp"

#include "core/AssetStore.hpp"
#include "core/JsonFile.hpp"

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <set>
#include <unordered_map>

namespace OpenArtOfWar
{

namespace
{
struct State
{
    std::once_flag loaded;
    std::mutex lock;
    nlohmann::json doc;
    std::string text;
    std::string root = LoaderResources::kDefaultRoot;
    bool fromFile = false;
};

State &state()
{
    static State s;
    return s;
}

// The manifest is the only source of resource paths: there is no built-in layout to fall back on
bool LoadInto(State &s, const std::string &path)
{
    nlohmann::json doc = nlohmann::json::object();
    std::string text;
    bool ok = false;
    std::string actualPath = path;
    if (!Assets::ReadText(actualPath, text))
    {
        if (actualPath.size() >= 5 && actualPath.compare(actualPath.size() - 5, 5, ".yaml") == 0)
        {
            std::string alt = actualPath.substr(0, actualPath.size() - 5) + ".json";
            if (Assets::ReadText(alt, text))
                actualPath = alt;
        }
        else if (actualPath.size() >= 5 && actualPath.compare(actualPath.size() - 5, 5, ".json") == 0)
        {
            std::string alt = actualPath.substr(0, actualPath.size() - 5) + ".yaml";
            if (Assets::ReadText(alt, text))
                actualPath = alt;
        }
    }
    if (!text.empty())
    {
        nlohmann::json file = JsonFile::ReadFromString(text);
        if (file.is_object() && !file.empty())
        {
            doc = std::move(file);
            ok = true;
        }
        else
            std::cerr << "[LoaderResources] " << actualPath << " is not a valid manifest object: no resources" << std::endl;
    }
    else
        std::cerr << "[LoaderResources] " << path << " not found: no resources" << std::endl;
    std::lock_guard<std::mutex> guard(s.lock);
    s.doc = std::move(doc);
    s.text = ok ? text : std::string();
    s.fromFile = ok;
    return ok;
}

void EnsureLoaded()
{
    State &s = state();
    std::call_once(s.loaded, [&] {
        std::string path = LoaderResources::kManifestPath;
        std::string text;
        if (!Assets::ReadText(path, text))
        {
            std::string rootPath = s.root + "/main.yaml";
            if (Assets::ReadText(rootPath, text))
                path = rootPath;
            else
            {
                rootPath = s.root + "/main.json";
                if (Assets::ReadText(rootPath, text))
                    path = rootPath;
            }
        }
        LoadInto(s, path);
    });
}

std::string WithExt(const std::string &name, const std::string &ext)
{
    if (ext.empty() || (name.size() >= ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0))
        return name;
    return name + ext;
}
} // namespace

bool LoaderResources::Init(const std::string &assetRoot, const std::string &manifestPath)
{
    State &s = state();
    {
        std::lock_guard<std::mutex> guard(s.lock);
        s.root = assetRoot.empty() ? std::string(kDefaultRoot) : assetRoot;
    }
    std::call_once(s.loaded, [] {}); // later lazy loads must not overwrite this one
    std::string path = manifestPath;
    if (manifestPath == kManifestPath && !assetRoot.empty() && assetRoot != kDefaultRoot)
    {
        std::string text;
        if (!Assets::ReadText(path, text))
            path = s.root + "/main.yaml";
    }
    return LoadInto(s, path);
}

void LoaderResources::SetAssetRoot(const std::string &root)
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.root = root.empty() ? std::string(kDefaultRoot) : root;
}

const std::string &LoaderResources::AssetRoot()
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    return s.root;
}

bool LoaderResources::HasManifest()
{
    EnsureLoaded();
    return state().fromFile;
}

const std::string &LoaderResources::ManifestText()
{
    EnsureLoaded();
    return state().text;
}

const nlohmann::json &LoaderResources::Manifest()
{
    EnsureLoaded();
    return state().doc;
}

std::string LoaderResources::join(const std::string &relative)
{
    if (relative.empty())
        return std::string(); // not named in main.yaml: no file (never the bare root folder)
    const std::string root = AssetRoot();
    if (root.empty())
        return relative;
    if (root.back() == '/' || root.back() == '\\')
        return root + relative;
    return root + "/" + relative;
}

std::string LoaderResources::Str(const char *section, const char *key)
{
    const nlohmann::json &m = Manifest();
    const nlohmann::json *node = &m;
    const nlohmann::json *found = nullptr;
    if (section)
    {
        auto it = m.find(section);
        if (it != m.end() && it->is_object())
            node = &*it;
        else
            node = nullptr;
    }
    if (node)
    {
        auto it = node->find(key);
        if (it != node->end() && it->is_string())
            found = &*it;
    }
    if (found)
        return found->get<std::string>();
    // Not in main.yaml: nothing to load (reported once per key)
    static std::mutex reportedLock;
    static std::set<std::string> reported;
    const std::string name = section ? std::string(section) + "." + key : std::string(key);
    std::lock_guard<std::mutex> guard(reportedLock);
    if (reported.insert(name).second)
        std::cerr << "[LoaderResources] " << kManifestPath << " has no \"" << name << "\"" << std::endl;
    return std::string();
}

std::string LoaderResources::LocaleDir()
{
    return join(Str("locale", "dir"));
}

std::string LoaderResources::DefaultLanguage()
{
    return Str("locale", "default_language");
}

std::string LoaderResources::Environment()
{
    return join(Str(nullptr, "environment"));
}

std::string LoaderResources::UiTheme()
{
    return join(Str(nullptr, "ui_theme"));
}

std::string LoaderResources::FontRegular()
{
    return join(Str("fonts", "regular"));
}

std::string LoaderResources::FontBold()
{
    return join(Str("fonts", "bold"));
}

std::string LoaderResources::SpriteFrames()
{
    return join(Str("sprites", "frames"));
}

// The sheets by page number: the page of a sprite (characters/sprites.json) is the number in the file name
// ("sprites/sheet_07.png" is page 7), not the place in the list, so a sheet that is not listed (page 0 is unused)
// leaves a gap instead of moving every later page by one. Names without a number keep their place in the list.
std::vector<std::string> LoaderResources::SheetsByPage()
{
    const std::vector<std::string> listed = List("sprites", "sheets");
    std::vector<std::string> byPage;
    for (std::size_t i = 0; i < listed.size(); ++i)
    {
        std::size_t page = i;
        const std::string &name = listed[i];
        const std::size_t us = name.rfind('_'), dot = name.rfind('.');
        if (us != std::string::npos && dot != std::string::npos && dot > us + 1)
        {
            const std::string digits = name.substr(us + 1, dot - us - 1);
            if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos && digits.size() < 5)
                page = static_cast<std::size_t>(std::stoul(digits));
        }
        if (page >= byPage.size())
            byPage.resize(page + 1);
        byPage[page] = name;
    }
    return byPage;
}

int LoaderResources::SpriteSheetCount()
{
    return static_cast<int>(SheetsByPage().size());
}

std::string LoaderResources::SpriteSheet(int index)
{
    const std::vector<std::string> sheets = SheetsByPage();
    if (index < 0 || index >= static_cast<int>(sheets.size()) || sheets[static_cast<std::size_t>(index)].empty())
        return std::string();
    return join(sheets[static_cast<std::size_t>(index)]);
}

std::vector<std::pair<std::string, std::string>> LoaderResources::UiTextures()
{
    std::vector<std::pair<std::string, std::string>> out;
    const nlohmann::json &m = Manifest();
    auto it = m.find("ui_textures");
    if (it == m.end() || !it->is_object())
        return out;
    for (auto e = it->begin(); e != it->end(); ++e)
    {
        if (e.value().is_string())
            out.emplace_back(e.key(), join(e.value().get<std::string>()));
    }
    return out;
}

std::string LoaderResources::GraphicsData()
{
    return join(Str(nullptr, "graphics_data"));
}

std::string LoaderResources::AudioFile(const std::string &name)
{
    return join(name);
}

std::string LoaderResources::Tracklist()
{
    return join(Str("audio", "tracklist"));
}

std::string LoaderResources::Ambient(bool night)
{
    const std::string name = Str("audio", night ? "ambient_night" : "ambient_day");
    return name.empty() ? std::string() : join(name);
}

std::string LoaderResources::AudioLicense()
{
    return join(Str("audio", "license"));
}

std::string LoaderResources::SfxConfig()
{
    return join(Str("sfx", "config"));
}

std::string LoaderResources::VoicesConfig()
{
    return join(Str("voices", "config"));
}

std::vector<std::string> LoaderResources::VoiceClips(const std::string &language)
{
    std::vector<std::string> out;
    const nlohmann::json &m = Manifest();
    auto it = m.find("voices");
    if (it == m.end() || !it->is_object())
        return out;
    auto clips = it->find("clips");
    if (clips == it->end() || !clips->is_object())
        return out;
    auto lang = clips->find(language);
    if (lang == clips->end() || !lang->is_array())
        return out;
    for (const auto &v : *lang)
    {
        if (v.is_string())
            out.push_back(join(v.get<std::string>()));
    }
    return out;
}

std::string LoaderResources::VoicesFallbackLanguage()
{
    return Str("voices", "fallback_language");
}

std::vector<std::string> LoaderResources::SfxClips()
{
    return Joined(List("sfx", "clips"));
}

std::string LoaderResources::AiDifficulty()
{
    return join(Str(nullptr, "ai_difficulty"));
}

std::string LoaderResources::Nights()
{
    return join(Str(nullptr, "nights"));
}

std::string LoaderResources::Effects()
{
    return join(Str(nullptr, "effects"));
}

std::string LoaderResources::Projectiles()
{
    return join(Str(nullptr, "projectiles"));
}

std::string LoaderResources::WeaponBalance()
{
    return join(Str(nullptr, "weapon_balance"));
}

std::string LoaderResources::Research(const std::string &faction)
{
    return join(Str("research", faction.c_str()));
}

std::string LoaderResources::TileCollision()
{
    return join(Str(nullptr, "tile_collision"));
}

std::vector<std::string> LoaderResources::CharacterFiles()
{
    return Joined(List("characters", "files"));
}

std::string LoaderResources::MapsDir()
{
    return join(Str("maps", "dir"));
}

std::string LoaderResources::MapPath(const std::string &name)
{
    static const std::unordered_map<std::string, std::string> kAliases = {
        {"map_mission_1", "confederation_mission_1"},
        {"map_mission_2", "confederation_mission_2"},
        {"map_mission_3", "confederation_mission_3"},
        {"map_mission_4", "confederation_mission_4"},
        {"map_mission_5", "confederation_mission_5"},
        {"map_mission_6", "confederation_mission_6"},
        {"m37", "resistance_mission_1"},
        {"m42", "resistance_mission_2"},
        {"m43", "resistance_mission_3"},
        {"m26", "resistance_mission_4"},
        {"m35", "resistance_mission_5"},
        {"m44", "resistance_mission_6"},
        {"m0", "resistance_mission_7"},
        {"brod", "resistance_mission_8"},
        {"m100", "resistance_mission_8"},
        {"4x_gage", "skirmish_1"},
        {"map_skirmish_2", "skirmish_2"},
        {"map_skirmish_3", "skirmish_3"},
        {"map_skirmish_4", "skirmish_4"},
        {"map_skirmish_5", "skirmish_5"},
        {"map_skirmish_6", "skirmish_6"},
        {"map_skirmish_7", "skirmish_7"},
        {"4x_kings", "skirmish_8"},
        {"map_skirmish_9", "skirmish_9"},
        {"map_skirmish_10", "skirmish_10"},
        {"map_skirmish_11", "skirmish_11"},
        {"map_skirmish_12", "skirmish_12"},
        {"map_skirmish_13", "skirmish_13"},
        {"map_skirmish_14", "skirmish_14"},
        {"map_skirmish_15", "skirmish_15"},
        {"4x_runs", "skirmish_16"},
        {"generated_map_7142", "skirmish_17"}};

    std::filesystem::path p(name);
    std::string stem = p.stem().string();
    auto it = kAliases.find(stem);
    std::string targetStem = (it != kAliases.end()) ? it->second : stem;

    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
    {
        if (it != kAliases.end())
            return (p.parent_path() / WithExt(targetStem, ".json")).generic_string();
        return WithExt(name, ".json");
    }
    return MapsDir() + "/" + WithExt(targetStem, ".json");
}

std::string LoaderResources::TestMap()
{
    return join(Str("maps", "test_map"));
}

std::string LoaderResources::NetDefaultMap()
{
    return join(Str("maps", "net_default"));
}

std::vector<std::string> LoaderResources::ListMaps()
{
    std::vector<std::string> maps = Joined(List("maps", "files"));
    // Maps the player saved with the editor (blob install: below the per-user directory) come after the shipped ones
    if (Assets::IsBlob())
    {
        std::error_code ec;
        const std::filesystem::path userMaps = std::filesystem::path(Assets::UserDataDir()) / MapsDir();
        std::vector<std::string> own;
        for (const auto &entry : std::filesystem::directory_iterator(userMaps, ec))
        {
            const std::string path = MapsDir() + "/" + entry.path().filename().string();
            if (entry.is_regular_file(ec) && entry.path().extension() == ".json"
                && std::find(maps.begin(), maps.end(), path) == maps.end())
                own.push_back(path);
        }
        std::sort(own.begin(), own.end());
        maps.insert(maps.end(), own.begin(), own.end());
    }
    return maps;
}

std::vector<std::string> LoaderResources::NetHashFiles()
{
    // Only what the simulation reads: sound, voice and picture files are not listed (see NetVisualFiles), different
    // ones must not keep players apart
    return Joined(List(nullptr, "net_hash"));
}

std::string LoaderResources::NetManifestText()
{
    // The parts of the manifest the simulation depends on, in a fixed order: which characters, research, maps and
    // settings files exist. The lists of pictures, sounds and fonts are left out: they are a player's own business.
    static const char *const kKeys[]
        = {"characters",
           "ai_difficulty",
           "effects",
           "projectiles",
           "weapon_balance",
           "research",
           "tile_collision",
           "maps",
           "environment",
           "net_hash"};
    const nlohmann::json &m = Manifest();
    nlohmann::json subset = nlohmann::json::object();
    for (const char *key : kKeys)
    {
        if (m.contains(key))
            subset[key] = m[key];
    }
    return subset.dump();
}

std::vector<std::string> LoaderResources::NetVisualFiles()
{
    // Sprites, palettes, the music list, the UI colours: they never change the simulation, so players may differ in
    // them
    return Joined(List(nullptr, "net_visual"));
}

std::vector<std::string> LoaderResources::List(const char *section, const char *key)
{
    std::vector<std::string> out;
    const nlohmann::json &m = Manifest();
    const nlohmann::json *node = &m;
    if (section)
    {
        auto it = m.find(section);
        if (it == m.end() || !it->is_object())
            return out;
        node = &*it;
    }
    auto it = node->find(key);
    if (it == node->end() || !it->is_array())
        return out;
    for (const auto &v : *it)
    {
        if (v.is_string())
            out.push_back(v.get<std::string>());
    }
    return out;
}

std::vector<std::string> LoaderResources::Joined(std::vector<std::string> files)
{
    for (std::string &f : files)
        f = join(f);
    return files;
}

} // namespace OpenArtOfWar
