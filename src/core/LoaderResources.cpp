#include "core/LoaderResources.hpp"

#include "core/AssetStore.hpp"
#include "core/JsonFile.hpp"

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <set>

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
    if (Assets::ReadText(path, text))
    {
        nlohmann::json file = JsonFile::ReadFromString(text);
        if (file.is_object() && !file.empty())
        {
            doc = std::move(file);
            ok = true;
        }
        else
            std::cerr << "[LoaderResources] " << path << " is not a JSON object: no resources" << std::endl;
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
    std::call_once(s.loaded, [&] { LoadInto(s, LoaderResources::kManifestPath); });
}

std::string WithExt(const std::string &name, const std::string &ext)
{
    if (ext.empty() || (name.size() >= ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0))
        return name;
    return name + ext;
}
} // namespace

bool LoaderResources::Init(const std::string &manifestPath)
{
    State &s = state();
    std::call_once(s.loaded, [] {}); // later lazy loads must not overwrite this one
    return LoadInto(s, manifestPath);
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

std::string LoaderResources::join(const std::string &root, const std::string &relative)
{
    if (relative.empty())
        return std::string(); // not named in main.json: no file (never the bare root folder)
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
    // Not in main.json: nothing to load (reported once per key)
    static std::mutex reportedLock;
    static std::set<std::string> reported;
    const std::string name = section ? std::string(section) + "." + key : std::string(key);
    std::lock_guard<std::mutex> guard(reportedLock);
    if (reported.insert(name).second)
        std::cerr << "[LoaderResources] " << kManifestPath << " has no \"" << name << "\"" << std::endl;
    return std::string();
}

std::string LoaderResources::LocaleDir(const std::string &root)
{
    return join(root, Str("locale", "dir"));
}

std::string LoaderResources::DefaultLanguage()
{
    return Str("locale", "default_language");
}

std::string LoaderResources::environment(const std::string &root)
{
    return join(root, Str(nullptr, "environment"));
}

std::string LoaderResources::UiTheme(const std::string &root)
{
    return join(root, Str(nullptr, "ui_theme"));
}

std::string LoaderResources::FontRegular(const std::string &root)
{
    return join(root, Str("fonts", "regular"));
}

std::string LoaderResources::FontBold(const std::string &root)
{
    return join(root, Str("fonts", "bold"));
}

std::string LoaderResources::SpriteFrames(const std::string &root)
{
    return join(root, Str("sprites", "frames"));
}

// The sheets by page number: the page of a sprite (characters/sprites.json) is the number in the file name
// ("sprites/sheet_07.png" is page 7), not the place in the list, so a sheet that is not listed (page 0 is unused)
// leaves a gap instead of moving every later page by one. Names without a number keep their place in the list.
std::vector<std::string> LoaderResources::SheetsByPage()
{
    const std::vector<std::string> listed = List("sprites", "sheets");
    std::vector<std::string> byPage;
    for (size_t i = 0; i < listed.size(); ++i)
    {
        size_t page = i;
        const std::string &name = listed[i];
        const size_t us = name.rfind('_'), dot = name.rfind('.');
        if (us != std::string::npos && dot != std::string::npos && dot > us + 1)
        {
            const std::string digits = name.substr(us + 1, dot - us - 1);
            if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos && digits.size() < 5)
                page = static_cast<size_t>(std::stoul(digits));
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

std::string LoaderResources::SpriteSheet(int index, const std::string &root)
{
    const std::vector<std::string> sheets = SheetsByPage();
    if (index < 0 || index >= static_cast<int>(sheets.size()) || sheets[static_cast<size_t>(index)].empty())
        return std::string();
    return join(root, sheets[static_cast<size_t>(index)]);
}

std::vector<std::pair<std::string, std::string>> LoaderResources::UiTextures(const std::string &root)
{
    std::vector<std::pair<std::string, std::string>> out;
    const nlohmann::json &m = Manifest();
    auto it = m.find("ui_textures");
    if (it == m.end() || !it->is_object())
        return out;
    for (auto e = it->begin(); e != it->end(); ++e)
    {
        if (e.value().is_string())
            out.emplace_back(e.key(), join(root, e.value().get<std::string>()));
    }
    return out;
}

std::string LoaderResources::GraphicsData(const std::string &root)
{
    return join(root, Str(nullptr, "graphics_data"));
}

std::string LoaderResources::AudioFile(const std::string &name, const std::string &root)
{
    return join(root, name);
}

std::string LoaderResources::Tracklist(const std::string &root)
{
    return join(root, Str("audio", "tracklist"));
}

std::string LoaderResources::Ambient(bool night, const std::string &root)
{
    const std::string name = Str("audio", night ? "ambient_night" : "ambient_day");
    return name.empty() ? std::string() : join(root, name);
}

std::string LoaderResources::AudioLicense(const std::string &root)
{
    return join(root, Str("audio", "license"));
}

std::string LoaderResources::SfxConfig(const std::string &root)
{
    return join(root, Str("sfx", "config"));
}

std::string LoaderResources::VoicesConfig(const std::string &root)
{
    return join(root, Str("voices", "config"));
}

std::vector<std::string> LoaderResources::VoiceClips(const std::string &language, const std::string &root)
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
            out.push_back(join(root, v.get<std::string>()));
    }
    return out;
}

std::string LoaderResources::VoicesFallbackLanguage()
{
    return Str("voices", "fallback_language");
}

std::vector<std::string> LoaderResources::SfxClips(const std::string &root)
{
    return Joined(root, List("sfx", "clips"));
}

std::string LoaderResources::aiDifficulty(const std::string &root)
{
    return join(root, Str(nullptr, "ai_difficulty"));
}

std::string LoaderResources::Nights(const std::string &root)
{
    return join(root, Str(nullptr, "nights"));
}

std::string LoaderResources::Effects(const std::string &root)
{
    return join(root, Str(nullptr, "effects"));
}

std::string LoaderResources::projectiles(const std::string &root)
{
    return join(root, Str(nullptr, "projectiles"));
}

std::string LoaderResources::weaponBalance(const std::string &root)
{
    return join(root, Str(nullptr, "weapon_balance"));
}

std::string LoaderResources::research(const std::string &faction, const std::string &root)
{
    return join(root, Str("research", faction.c_str()));
}

std::string LoaderResources::tileCollision(const std::string &root)
{
    return join(root, Str(nullptr, "tile_collision"));
}

std::vector<std::string> LoaderResources::CharacterFiles(const std::string &root)
{
    return Joined(root, List("characters", "files"));
}

std::string LoaderResources::MapsDir(const std::string &root)
{
    return join(root, Str("maps", "dir"));
}

std::string LoaderResources::MapPath(const std::string &name, const std::string &root)
{
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        return WithExt(name, ".json");
    return join(MapsDir(root), WithExt(name, ".json"));
}

std::string LoaderResources::TestMap(const std::string &root)
{
    return join(root, Str("maps", "test_map"));
}

std::string LoaderResources::NetDefaultMap(const std::string &root)
{
    return join(root, Str("maps", "net_default"));
}

std::vector<std::string> LoaderResources::ListMaps(const std::string &root)
{
    std::vector<std::string> maps = Joined(root, List("maps", "files"));
    // Maps the player saved with the editor (blob install: below the per-user directory) come after the shipped ones
    if (Assets::IsBlob())
    {
        std::error_code ec;
        const std::filesystem::path userMaps = std::filesystem::path(Assets::UserDataDir()) / MapsDir(root);
        std::vector<std::string> own;
        for (const auto &entry : std::filesystem::directory_iterator(userMaps, ec))
        {
            const std::string path = MapsDir(root) + "/" + entry.path().filename().string();
            if (entry.is_regular_file(ec) && entry.path().extension() == ".json"
                && std::find(maps.begin(), maps.end(), path) == maps.end())
                own.push_back(path);
        }
        std::sort(own.begin(), own.end());
        maps.insert(maps.end(), own.begin(), own.end());
    }
    return maps;
}

std::vector<std::string> LoaderResources::NetHashFiles(const std::string &root)
{
    // Only what the simulation reads: sound, voice and picture files are not listed (see NetVisualFiles), different
    // ones must not keep players apart
    return Joined(root, List(nullptr, "net_hash"));
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

std::vector<std::string> LoaderResources::NetVisualFiles(const std::string &root)
{
    // Sprites, palettes, the music list, the UI colours: they never change the simulation, so players may differ in
    // them
    return Joined(root, List(nullptr, "net_visual"));
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

std::vector<std::string> LoaderResources::Joined(const std::string &root, std::vector<std::string> files)
{
    for (std::string &f : files)
        f = join(root, f);
    return files;
}

} // namespace OpenArtOfWar
