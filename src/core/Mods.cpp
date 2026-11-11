#include "core/Mods.hpp"

#include "core/AssetStore.hpp"
#include "core/JsonFile.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>

namespace OpenArtOfWar::Mods
{

namespace fs = std::filesystem;

namespace
{

struct State
{
    std::mutex lock;
    std::string appDir, userDir; // overrides (tests)
    std::vector<ModInfo> active;
};

State &state()
{
    static State s;
    return s;
}

bool ValidId(const std::string &id)
{
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::islower(c) || std::isdigit(c) || c == '_';
    });
}

std::string Text(const nlohmann::json &j, const char *key, const std::string &fallback = "")
{
    if (!j.contains(key))
        return fallback;
    const nlohmann::json &v = j[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<long long>());
    if (v.is_number())
    {
        std::string s = v.dump();
        return s;
    }
    return fallback;
}

// The description of one mod from its mod.yaml text
ModInfo Describe(const std::string &text, const fs::path &path, bool archive, bool fromUser)
{
    ModInfo m;
    m.path = path.string();
    m.archive = archive;
    m.fromUser = fromUser;
    m.id = path.stem().string();
    const nlohmann::json j = JsonFile::ReadYamlFromString(text);
    if (text.empty())
    {
        m.error = "no mod.yaml";
        return m;
    }
    if (!j.is_object() || j.empty())
    {
        m.error = "mod.yaml is not a YAML mapping";
        return m;
    }
    const std::string id = Text(j, "id");
    if (!ValidId(id))
    {
        m.error = "mod.yaml: \"id\" must be lower case letters, digits and _";
        return m;
    }
    m.id = id;
    m.name = Text(j, "name", id);
    m.version = Text(j, "version", "0");
    m.author = Text(j, "author");
    m.description = Text(j, "description");
    m.gameVersion = Text(j, "game_version");
    m.preview = Text(j, "preview");
    if (m.preview.empty())
    {
        std::error_code ec;
        if (!archive)
        {
            if (fs::is_regular_file(path / "preview.png", ec))
                m.preview = "preview.png";
        }
        else
        {
            m.preview = "preview.png";
        }
    }
    if (j.contains("manifest"))
    {
        if (j["manifest"].is_object())
            m.manifest = j["manifest"];
        else
            m.error = "mod.yaml: \"manifest\" must be a mapping";
    }
    return m;
}

void ScanDir(const fs::path &dir, bool fromUser, std::vector<ModInfo> &out)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return;
    std::vector<fs::path> entries;
    for (const auto &e : fs::directory_iterator(dir, ec))
        entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());
    for (const fs::path &p : entries)
    {
        if (fs::is_directory(p, ec))
        {
            if (!fs::is_regular_file(p / "mod.yaml", ec))
                continue; // not a mod folder
            std::ifstream f(p / "mod.yaml", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            out.push_back(Describe(text, p, false, fromUser));
        }
        else if (fs::is_regular_file(p, ec) && p.extension() == ".mod")
        {
            std::map<std::string, std::vector<std::uint8_t>> files;
            if (!Assets::ReadArchive(p.string(), files, "mod.yaml"))
            {
                ModInfo m;
                m.id = p.stem().string();
                m.path = p.string();
                m.archive = true;
                m.fromUser = fromUser;
                m.error = "not a readable archive (tar.zst)";
                out.push_back(m);
                continue;
            }
            const auto it = files.find("mod.yaml");
            const std::string text
                = it == files.end() ? std::string() : std::string(it->second.begin(), it->second.end());
            out.push_back(Describe(text, p, true, fromUser));
        }
    }
}

// Every file of an unpacked mod, as keys
void FolderKeys(const fs::path &root, std::vector<std::string> &keys)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
    {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        std::string key;
        if (KeyOf(fs::relative(it->path(), root, ec).generic_string(), key) && key != "mod.yaml")
            keys.push_back(key);
    }
}

} // namespace

void SetSearchDirs(const std::string &appMods, const std::string &userMods)
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.appDir = appMods;
    s.userDir = userMods;
}

std::vector<std::string> SearchDirs()
{
    State &s = state();
    std::string app, user;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        app = s.appDir;
        user = s.userDir;
    }
    if (app.empty())
    {
        const char *base = SDL_GetBasePath();
        app = (fs::path(base ? base : ".") / "mods").string();
    }
    if (user.empty())
        user = (fs::path(Assets::UserDataDir()) / "mods").string();
    return {app, user};
}

bool LoadPreview(const ModInfo &mod, std::vector<std::uint8_t> &out)
{
    out.clear();
    std::string filename = mod.preview.empty() ? "preview.png" : mod.preview;
    while (filename.rfind("./", 0) == 0)
        filename = filename.substr(2);
    while (!filename.empty() && filename.front() == '/')
        filename = filename.substr(1);
    if (filename.empty())
        return false;

    if (!mod.archive)
    {
        std::error_code ec;
        fs::path p = fs::path(mod.path) / filename;
        if (!fs::is_regular_file(p, ec))
            return false;
        std::ifstream in(p, std::ios::binary);
        if (!in)
            return false;
        in.seekg(0, std::ios::end);
        auto sz = in.tellg();
        if (sz <= 0)
            return false;
        in.seekg(0, std::ios::beg);
        out.resize(static_cast<std::size_t>(sz));
        if (!in.read(reinterpret_cast<char *>(out.data()), sz))
        {
            out.clear();
            return false;
        }
        return true;
    }

    std::map<std::string, std::vector<std::uint8_t>> files;
    if (Assets::ReadArchive(mod.path, files, filename))
    {
        auto it = files.find(filename);
        if (it != files.end() && !it->second.empty())
        {
            out = std::move(it->second);
            return true;
        }
        if (!files.empty() && !files.begin()->second.empty())
        {
            out = std::move(files.begin()->second);
            return true;
        }
    }
    return false;
}

std::vector<ModInfo> Scan()
{
    const std::vector<std::string> dirs = SearchDirs();
    std::vector<ModInfo> found;
    ScanDir(dirs[0], false, found);
    if (fs::path(dirs[1]).lexically_normal() != fs::path(dirs[0]).lexically_normal())
        ScanDir(dirs[1], true, found);
    // One per id: the higher version; on the same version the user's copy
    std::vector<ModInfo> out;
    for (ModInfo &m : found)
    {
        auto same = std::find_if(out.begin(), out.end(), [&](const ModInfo &o) { return o.id == m.id; });
        if (same == out.end())
        {
            out.push_back(std::move(m));
            continue;
        }
        if (!m.error.empty() && same->error.empty())
            continue;
        const int c = CompareVersions(m.version, same->version);
        if (c > 0 || (c == 0 && m.fromUser && !same->fromUser) || (same->error.size() && m.error.empty()))
            *same = std::move(m);
    }
    std::sort(out.begin(), out.end(), [](const ModInfo &a, const ModInfo &b) {
        return a.name != b.name ? a.name < b.name : a.id < b.id;
    });
    return out;
}

int CompareVersions(const std::string &a, const std::string &b)
{
    std::size_t i = 0, j = 0;
    while (i < a.size() || j < b.size())
    {
        auto part = [](const std::string &s, std::size_t &k) {
            std::string out;
            while (k < s.size() && s[k] != '.' && s[k] != '-')
                out += s[k++];
            if (k < s.size())
                ++k;
            return out;
        };
        const std::string pa = part(a, i), pb = part(b, j);
        const bool na = !pa.empty() && std::all_of(pa.begin(), pa.end(), ::isdigit);
        const bool nb = !pb.empty() && std::all_of(pb.begin(), pb.end(), ::isdigit);
        if (na && nb)
        {
            const unsigned long long x = std::stoull(pa), y = std::stoull(pb);
            if (x != y)
                return x < y ? -1 : 1;
        }
        else if (pa != pb)
        {
            if (pa.empty() || pb.empty())
                return pa.empty() ? -1 : 1;
            return pa < pb ? -1 : 1;
        }
    }
    return 0;
}

bool KeyOf(const std::string &path, std::string &key)
{
    if (path.empty() || path.find('\\') != std::string::npos || path.find(':') != std::string::npos)
        return false;
    const fs::path p(path);
    if (p.is_absolute() || path[0] == '/')
        return false;
    std::string normal = p.lexically_normal().generic_string();
    while (normal.compare(0, 2, "./") == 0)
        normal.erase(0, 2);
    if (normal.empty() || normal == "." || normal == ".." || normal.compare(0, 3, "../") == 0)
        return false;
    if (normal.compare(0, 7, "assets/") == 0)
        normal.erase(0, 7);
    if (normal.empty() || normal.back() == '/')
        return false;
    key = normal;
    return true;
}

bool Activate(const std::vector<std::string> &orderedIds, ActivateReport *report)
{
    ActivateReport local;
    ActivateReport &r = report ? *report : local;
    r = {};
    std::vector<ModInfo> enabled;
    std::vector<Assets::ModLayer> layers;
    std::map<std::string, std::vector<std::string>> owners;
    if (!orderedIds.empty())
    {
        const std::vector<ModInfo> found = Scan();
        for (const std::string &id : orderedIds)
        {
            if (std::any_of(enabled.begin(), enabled.end(), [&](const ModInfo &m) { return m.id == id; }))
                continue;
            const auto it = std::find_if(found.begin(), found.end(), [&](const ModInfo &m) { return m.id == id; });
            if (it == found.end())
            {
                r.problems.push_back(id + ": not found");
                continue;
            }
            if (!it->error.empty())
            {
                r.problems.push_back(id + ": " + it->error);
                continue;
            }
            Assets::ModLayer layer;
            layer.id = id;
            std::vector<std::string> keys;
            if (it->archive)
            {
                std::map<std::string, std::vector<std::uint8_t>> files;
                if (!Assets::ReadArchive(it->path, files))
                {
                    r.problems.push_back(id + ": " + it->path + " cannot be read");
                    continue;
                }
                for (auto &[name, data] : files)
                {
                    std::string key;
                    if (!KeyOf(name, key))
                    {
                        r.problems.push_back(id + ": " + name + " is outside the mod (ignored)");
                        continue;
                    }
                    if (key == "mod.yaml")
                        continue;
                    keys.push_back(key);
                    layer.files[key] = std::move(data);
                }
            }
            else
            {
                layer.folder = it->path;
                FolderKeys(it->path, keys);
            }
            for (const std::string &k : keys)
                owners[k].push_back(id);
            layers.push_back(std::move(layer));
            enabled.push_back(*it);
        }
    }
    for (auto &[key, ids] : owners)
        if (ids.size() > 1)
            r.conflicts[key] = ids;
    Assets::SetModLayers(std::move(layers));
    {
        State &s = state();
        std::lock_guard<std::mutex> guard(s.lock);
        s.active = enabled;
    }
    for (const std::string &p : r.problems)
        std::cerr << "[Mods] " << p << std::endl;
    if (!enabled.empty())
        std::cout << "[Mods] Enabled: " << ActiveSummary() << std::endl;
    return r.problems.empty();
}

void Deactivate()
{
    Activate({});
}

const std::vector<ModInfo> &Active()
{
    return state().active;
}

std::string ActiveSummary()
{
    std::string out;
    for (const ModInfo &m : Active())
        out += (out.empty() ? "" : ", ") + m.id + " " + m.version;
    return out;
}

void MergeManifest(nlohmann::json &base, const nlohmann::json &add)
{
    if (base.is_object() && add.is_object())
    {
        for (auto it = add.begin(); it != add.end(); ++it)
        {
            if (base.contains(it.key()))
                MergeManifest(base[it.key()], it.value());
            else
                base[it.key()] = it.value();
        }
        return;
    }
    if (base.is_array() && add.is_array())
    {
        for (const auto &item : add)
            if (std::find(base.begin(), base.end(), item) == base.end())
                base.push_back(item);
        return;
    }
    base = add;
}

} // namespace OpenArtOfWar::Mods
