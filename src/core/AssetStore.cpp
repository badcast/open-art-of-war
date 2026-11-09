#include "core/AssetStore.hpp"
#include "core/Sha256.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include <archive.h>
#include <archive_entry.h>

namespace OpenArtOfWar::Assets
{

namespace fs = std::filesystem;

namespace
{
//
constexpr const char *MainAssetBlobName = MAIN_ASSET_BLOB_NAME;

struct State
{
    Mode mode = Mode::None;
    std::string description = "no assets located";
    std::mutex lock;
    std::map<std::string, std::vector<std::uint8_t>> blobFiles;
    std::map<std::string, std::vector<std::uint8_t>> overlay; // session overlay, see SetSessionOverlay
    std::vector<ModLayer> mods;                               // enabled mods, see SetModLayers
    std::string userDir;
    fs::path root;        // the folder that holds "assets" or the filePath
    std::string blobPath; // the loaded blob (blob mode)
};

State &state()
{
    static State s;
    return s;
}

std::vector<fs::path> CandidateRoots()
{
    std::vector<fs::path> roots {fs::path(".")};
    if (const char *base = SDL_GetBasePath())
    {
        roots.push_back(fs::path(base));
        roots.push_back(fs::path(base) / "..");
        roots.push_back(fs::path(base) / ".." / "share" / "openaow");
    }
    return roots;
}

// "assets/maps/m0.json", "./assets/x", "../assets/x" -> "maps/m0.json"; false for non-asset paths
bool AssetKey(const std::string &path, std::string &key)
{
    fs::path normal = fs::path(path).lexically_normal();
    auto it = normal.begin();
    while (it != normal.end() && (*it == ".." || *it == "."))
        ++it;
    if (it == normal.end() || *it != "assets")
        return false;
    ++it;
    fs::path rest;
    for (; it != normal.end(); ++it)
        rest /= *it;
    key = rest.generic_string();
    return true;
}

std::string UserDirLocked(State &state)
{
    if (state.userDir.empty())
    {
        if (char *pref = SDL_GetPrefPath("badcast", "openaow"))
        {
            state.userDir = std::move(std::string(pref));
            SDL_free(pref);
        }
        else
        {
            state.userDir = "./";
        }
        while (state.userDir.size() > 1 && (state.userDir.back() == '/' || state.userDir.back() == '\\'))
            state.userDir.pop_back();
    }
    return state.userDir;
}

bool ReadFile(const fs::path &p, std::vector<std::uint8_t> &out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Unpacks every regular file of the tar.zst blob into memory (blobArhive tar stream has no random access)
bool LoadBlob(
    const fs::path &filePath, std::map<std::string, std::vector<std::uint8_t>> &files, const std::string &only = "")
{
    archive *blobArhive = archive_read_new();
    archive_read_support_filter_all(blobArhive);
    archive_read_support_format_tar(blobArhive);
    if (archive_read_open_filename(blobArhive, filePath.string().c_str(), 1 << 20) != ARCHIVE_OK)
    {
        std::cerr << "[Assets] Cannot open " << filePath << ": " << archive_error_string(blobArhive) << std::endl;
        archive_read_free(blobArhive);
        return false;
    }
    archive_entry *entry = nullptr;
    int status;
    while ((status = archive_read_next_header(blobArhive, &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN)
    {
        if (archive_entry_filetype(entry) != AE_IFREG)
            continue;
        if (!only.empty())
        {
            std::string entryName = fs::path(archive_entry_pathname(entry)).lexically_normal().generic_string();
            if (entryName.compare(0, 2, "./") == 0)
                entryName.erase(0, 2);
            if (entryName != only)
            {
                archive_read_data_skip(blobArhive);
                continue;
            }
        }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(archive_entry_size(entry)));
        std::size_t got = 0;
        while (got < data.size())
        {
            const la_ssize_t n = archive_read_data(blobArhive, data.data() + got, data.size() - got);
            if (n <= 0)
                break;
            got += static_cast<std::size_t>(n);
        }
        data.resize(got);
        std::string name = fs::path(archive_entry_pathname(entry)).lexically_normal().generic_string();
        if (name.compare(0, 2, "./") == 0)
            name.erase(0, 2);
        files[name] = std::move(data);
    }
    const bool ok = status == ARCHIVE_EOF;
    if (!ok)
        std::cerr << "[Assets] Cannot read " << filePath << ": " << archive_error_string(blobArhive) << std::endl;
    archive_read_free(blobArhive);
    return ok;
}

// Blob lookups; the caller holds the lock
bool BlobRead(State &s, const std::string &key, std::vector<std::uint8_t> &out)
{
    const auto it = s.blobFiles.find(key);
    if (key.empty() || it == s.blobFiles.end())
        return false;
    out = it->second;
    return true;
}

bool BlobHas(State &s, const std::string &key)
{
    return !key.empty() && s.blobFiles.count(key) != 0;
}

} // namespace

bool Init()
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    if (s.mode != Mode::None)
        return true;

    std::error_code ec;
    const std::vector<fs::path> roots = CandidateRoots();

    // 1. A real "assets" directory
    for (const fs::path &root : roots)
    {
        if (fs::is_directory(root / "assets" / "characters", ec))
        {
            if (root != fs::path("."))
                fs::current_path(root, ec);
            s.mode = Mode::Directory;
            s.root = fs::current_path(ec);
            s.description = "directory " + fs::current_path(ec).string() + "/assets";
            return true;
        }
    }

    // 2. data.blob with the packed contents of that directory
    for (const fs::path &root : roots)
    {
        const fs::path blob = root / MainAssetBlobName;
        if (!fs::is_regular_file(blob, ec))
            continue;
        if (!LoadBlob(blob, s.blobFiles))
        {
            s.blobFiles.clear();
            continue;
        }
        s.mode = Mode::Blob;
        s.root = fs::absolute(root, ec);
        s.blobPath = fs::weakly_canonical(blob, ec).string();
        s.description = "blob " + fs::weakly_canonical(blob, ec).string();
        return true;
    }

    std::cerr << "[Assets] Neither an \"assets\" directory nor " << MainAssetBlobName << " was found" << std::endl;
    return false;
}

bool VerifyCertificate(std::vector<std::string> &problems, std::string &certificatePath)
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    problems.clear();
    certificatePath.clear();
    // Only a blob carries a certificate (written into it by tools/pack_assets.py); a plain assets directory is the
    // developer's working copy and is not checked
    if (s.mode != Mode::Blob)
        return true;
    certificatePath = s.blobPath + ":" + kCertificateName;

    const auto cert = s.blobFiles.find(kCertificateName);
    if (cert == s.blobFiles.end())
    {
        problems.push_back(std::string("the blob carries no ") + kCertificateName);
        return false;
    }
    std::istringstream lines(std::string(cert->second.begin(), cert->second.end()));
    std::string line;
    int lineNo = 0;
    std::set<std::string> listed;
    while (std::getline(lines, line))
    {
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        // "<path>  <sha256>": the digest is the last 64 characters, the path may hold spaces
        std::string path, expected;
        if (line.size() > 65)
        {
            expected = line.substr(line.size() - 64);
            path = line.substr(0, line.size() - 64);
            while (!path.empty() && (path.back() == ' ' || path.back() == '\t'))
                path.pop_back();
        }
        std::string key;
        const bool hex
            = expected.size() == 64 && expected.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
        if (path.empty() || !hex || !AssetKey(path, key) || key.empty())
        {
            problems.push_back("line " + std::to_string(lineNo) + " of the certificate is malformed");
            continue;
        }
        for (char &c : expected)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        listed.insert(key);
        // The packed original, never a per-user copy that shadows it
        const auto it = s.blobFiles.find(key);
        if (it == s.blobFiles.end())
            problems.push_back(path + ": missing");
        else if (Sha256::Hex(it->second.data(), it->second.size()) != expected)
            problems.push_back(path + ": modified");
    }
    // A file slipped into the blob after it was packed is not covered by the certificate
    for (const auto &[key, data] : s.blobFiles)
    {
        if (key != kCertificateName && listed.count(key) == 0)
            problems.push_back("assets/" + key + ": not in the certificate");
    }
    std::cout << "[Assets] " << listed.size() << " files checked against " << certificatePath << ": "
              << (problems.empty() ? "all match" : std::to_string(problems.size()) + " problem(s)") << std::endl;
    return problems.empty();
}

Mode GetMode()
{
    return state().mode;
}

std::string Describe()
{
    return state().description;
}

void SetSessionOverlay(std::map<std::string, std::vector<std::uint8_t>> files)
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.overlay = std::move(files);
}

void ClearSessionOverlay()
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.overlay.clear();
}

bool HasSessionOverlay()
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    return !s.overlay.empty();
}

namespace
{
// The overlay's file for `path`, when there is one
bool OverlayRead(State &s, const std::string &path, std::vector<std::uint8_t> *out)
{
    std::lock_guard<std::mutex> guard(s.lock);
    if (s.overlay.empty())
        return false;
    std::string key;
    if (!AssetKey(path, key))
        return false;
    const auto it = s.overlay.find(key);
    if (it == s.overlay.end())
        return false;
    if (out)
        *out = it->second;
    return true;
}
// The file of the topmost enabled mod that has `path`
bool ModRead(State &s, const std::string &path, std::vector<std::uint8_t> *out)
{
    std::lock_guard<std::mutex> guard(s.lock);
    if (s.mods.empty())
        return false;
    std::string key;
    if (!AssetKey(path, key) || key.empty())
        return false;
    for (auto layer = s.mods.rbegin(); layer != s.mods.rend(); ++layer)
    {
        if (!layer->folder.empty())
        {
            std::error_code ec;
            const fs::path p = fs::path(layer->folder) / key;
            if (!fs::is_regular_file(p, ec))
                continue;
            if (!out)
                return true;
            if (ReadFile(p, *out))
                return true;
            continue;
        }
        const auto it = layer->files.find(key);
        if (it == layer->files.end())
            continue;
        if (out)
            *out = it->second;
        return true;
    }
    return false;
}
} // namespace

void SetModLayers(std::vector<ModLayer> layers)
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.mods = std::move(layers);
}

void ClearModLayers()
{
    SetModLayers({});
}

bool HasModLayers()
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    return !s.mods.empty();
}

bool ReadArchive(
    const std::string &file, std::map<std::string, std::vector<std::uint8_t>> &files, const std::string &only)
{
    return LoadBlob(fs::path(file), files, only);
}

bool Exists(const std::string &path)
{
    State &s = state();
    if (OverlayRead(s, path, nullptr) || ModRead(s, path, nullptr))
        return true;
    std::string key;
    if (s.mode != Mode::Blob || !AssetKey(path, key))
    {
        std::error_code ec;
        return fs::exists(path, ec);
    }
    std::lock_guard<std::mutex> guard(s.lock);
    std::error_code ec;
    return fs::exists(fs::path(UserDirLocked(s)) / key, ec) || BlobHas(s, key);
}

bool ReadBytes(const std::string &path, std::vector<std::uint8_t> &out)
{
    State &s = state();
    if (OverlayRead(s, path, &out) || ModRead(s, path, &out))
        return true;
    std::string key;
    if (s.mode != Mode::Blob || !AssetKey(path, key))
        return ReadFile(path, out);
    std::lock_guard<std::mutex> guard(s.lock);
    if (ReadFile(fs::path(UserDirLocked(s)) / key, out))
        return true;
    return BlobRead(s, key, out);
}

bool ReadText(const std::string &path, std::string &out)
{
    std::vector<std::uint8_t> bytes;
    if (!ReadBytes(path, bytes))
        return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

std::vector<std::string> List(const std::string &dir, const std::string &extension)
{
    State &s = state();
    std::set<std::string> names;
    std::string key;
    const bool inBlob = s.mode == Mode::Blob && AssetKey(dir, key);
    std::error_code ec;

    auto scanDirectory = [&](const fs::path &p) {
        for (const auto &entry : fs::directory_iterator(p, ec))
        {
            if (entry.is_regular_file(ec))
                names.insert(entry.path().filename().string());
        }
    };

    if (!inBlob)
    {
        scanDirectory(dir);
    }
    else
    {
        std::lock_guard<std::mutex> guard(s.lock);
        scanDirectory(fs::path(UserDirLocked(s)) / key);
        const std::string prefix = key.empty() ? "" : key + "/";
        for (const auto &[full, data] : s.blobFiles)
        {
            if (full.compare(0, prefix.size(), prefix) != 0)
                continue;
            const std::string rest = full.substr(prefix.size());
            if (!rest.empty() && rest.find('/') == std::string::npos)
                names.insert(rest);
        }
    }

    // What the enabled mods add to that directory
    if (std::string modKey; AssetKey(dir, modKey))
    {
        std::lock_guard<std::mutex> guard(s.lock);
        const std::string prefix = modKey.empty() ? "" : modKey + "/";
        for (const ModLayer &layer : s.mods)
        {
            if (!layer.folder.empty())
            {
                const fs::path p = fs::path(layer.folder) / modKey;
                if (fs::is_directory(p, ec))
                    scanDirectory(p);
                continue;
            }
            for (const auto &[full, data] : layer.files)
            {
                if (full.compare(0, prefix.size(), prefix) != 0)
                    continue;
                const std::string rest = full.substr(prefix.size());
                if (!rest.empty() && rest.find('/') == std::string::npos)
                    names.insert(rest);
            }
        }
    }

    std::vector<std::string> result;
    for (const std::string &name : names)
    {
        if (!extension.empty() && fs::path(name).extension() != extension)
            continue;
        result.push_back((fs::path(dir) / name).generic_string());
    }
    return result;
}

std::string WritablePath(const std::string &path)
{
    State &s = state();
    std::string target = path;
    if (s.mode == Mode::Blob)
    {
        std::string key;
        std::lock_guard<std::mutex> guard(s.lock);
        target = (fs::path(UserDirLocked(s))
                  / (AssetKey(path, key) ? key : fs::path(path).lexically_normal().generic_string()))
                     .string();
    }
    std::error_code ec;
    const fs::path parent = fs::path(target).parent_path();
    if (!parent.empty())
        fs::create_directories(parent, ec);
    return target;
}

std::string RealDirectory(const std::string &dir)
{
    State &s = state();
    std::string key;
    if (s.mode != Mode::Blob || !AssetKey(dir, key))
        return dir;

    std::lock_guard<std::mutex> guard(s.lock);
    const fs::path out = fs::path(UserDirLocked(s)) / "cache" / key;
    std::error_code ec;
    fs::create_directories(out, ec);
    const std::string prefix = key.empty() ? "" : key + "/";
    for (const auto &[full, data] : s.blobFiles)
    {
        if (full.compare(0, prefix.size(), prefix) != 0)
            continue;
        const fs::path dest = out / full.substr(prefix.size());
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream f(dest, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return out.string();
}

std::string UserDataDir()
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    return UserDirLocked(s);
}

void SetUserDataDir(const std::string &dir)
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.userDir = dir;
    std::error_code ec;
    fs::create_directories(dir, ec);
}

} // namespace OpenArtOfWar::Assets
