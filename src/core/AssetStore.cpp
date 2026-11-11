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

struct LoadedBlob
{
    fs::path path;
    std::string certificate;
    std::map<std::string, std::string> fileHashes;
};

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
    std::vector<LoadedBlob> loadedBlobs;
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
    const fs::path &filePath,
    std::map<std::string, std::vector<std::uint8_t>> &files,
    const std::string &only = "",
    std::string *outCertificate = nullptr,
    std::map<std::string, std::string> *outFileHashes = nullptr)
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
        std::string rawName = fs::path(archive_entry_pathname(entry)).lexically_normal().generic_string();
        while (rawName.compare(0, 2, "./") == 0)
            rawName.erase(0, 2);
        std::string name = rawName;
        if (name.compare(0, 7, "assets/") == 0)
            name.erase(0, 7);

        if (!only.empty() && name != only && rawName != only)
        {
            archive_read_data_skip(blobArhive);
            continue;
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

        if (name == kCertificateName && outCertificate)
            outCertificate->assign(reinterpret_cast<const char *>(data.data()), data.size());

        if (outFileHashes)
            (*outFileHashes)[name] = Sha256::Hex(data.data(), data.size());

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

    // 2. data.blob or multiple *.blob files with the packed contents
    for (const fs::path &root : roots)
    {
        std::vector<fs::path> blobPaths;
        auto addBlobsFromDir = [&](const fs::path &dir) {
            std::error_code dirEc;
            if (!fs::is_directory(dir, dirEc))
                return;
            for (const auto &entry : fs::directory_iterator(dir, dirEc))
            {
                if (entry.is_regular_file(dirEc) && entry.path().extension() == ".blob")
                {
                    if (std::find(blobPaths.begin(), blobPaths.end(), entry.path()) == blobPaths.end())
                        blobPaths.push_back(entry.path());
                }
            }
        };

        addBlobsFromDir(root);
        addBlobsFromDir(root / "blobs");

        const fs::path mainBlob = root / MainAssetBlobName;
        if (fs::is_regular_file(mainBlob, ec))
        {
            if (std::find(blobPaths.begin(), blobPaths.end(), mainBlob) == blobPaths.end())
                blobPaths.push_back(mainBlob);
        }

        if (blobPaths.empty())
            continue;

        // Sort so that MainAssetBlobName comes first, then other blobs in alphabetical order
        std::sort(blobPaths.begin(), blobPaths.end(), [](const fs::path &a, const fs::path &b) {
            const bool aIsMain = (a.filename() == MainAssetBlobName);
            const bool bIsMain = (b.filename() == MainAssetBlobName);
            if (aIsMain != bIsMain)
                return aIsMain;
            return a.filename().string() < b.filename().string();
        });

        s.blobFiles.clear();
        s.loadedBlobs.clear();

        for (const fs::path &blob : blobPaths)
        {
            LoadedBlob lb;
            lb.path = fs::weakly_canonical(blob, ec);
            if (LoadBlob(blob, s.blobFiles, "", &lb.certificate, &lb.fileHashes))
            {
                s.loadedBlobs.push_back(std::move(lb));
            }
        }

        if (s.loadedBlobs.empty())
        {
            s.blobFiles.clear();
            continue;
        }

        s.mode = Mode::Blob;
        s.root = fs::absolute(root, ec);
        s.blobPath = s.loadedBlobs.front().path.string();

        if (s.loadedBlobs.size() == 1)
        {
            s.description = "blob " + s.blobPath;
        }
        else
        {
            std::string desc = std::to_string(s.loadedBlobs.size()) + " blobs in " + fs::absolute(root, ec).string() + " (";
            for (std::size_t i = 0; i < s.loadedBlobs.size(); ++i)
            {
                if (i > 0)
                    desc += ", ";
                desc += s.loadedBlobs[i].path.filename().string();
            }
            desc += ")";
            s.description = std::move(desc);
        }
        return true;
    }

    std::cerr << "[Assets] Neither an \"assets\" directory nor *.blob archives were found" << std::endl;
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

    if (s.loadedBlobs.empty())
    {
        problems.push_back("no blob loaded");
        return false;
    }

    if (s.loadedBlobs.size() == 1)
    {
        const auto &b = s.loadedBlobs.front();
        certificatePath = b.path.string() + ":" + kCertificateName;
        if (b.certificate.empty())
        {
            problems.push_back(std::string("the blob carries no ") + kCertificateName);
            return false;
        }

        std::istringstream lines(b.certificate);
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

            const auto it = b.fileHashes.find(key);
            if (it == b.fileHashes.end())
                problems.push_back(path + ": missing");
            else if (it->second != expected)
                problems.push_back(path + ": modified");
        }

        for (const auto &[key, hash] : b.fileHashes)
        {
            if (key != kCertificateName && listed.count(key) == 0)
                problems.push_back("assets/" + key + ": not in the certificate");
        }

        std::cout << "[Assets] " << listed.size() << " files checked against " << certificatePath << ": "
                  << (problems.empty() ? "all match" : std::to_string(problems.size()) + " problem(s)") << std::endl;
        return problems.empty();
    }

    // Multiple blobs
    certificatePath = "multiple blobs (" + std::to_string(s.loadedBlobs.size()) + ")";
    std::size_t totalChecked = 0;
    std::size_t blobsWithCert = 0;

    for (const auto &b : s.loadedBlobs)
    {
        if (b.certificate.empty())
            continue;

        ++blobsWithCert;
        const std::string blobPrefix = b.path.filename().string() + ": ";
        std::istringstream lines(b.certificate);
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
                problems.push_back(blobPrefix + "line " + std::to_string(lineNo) + " of the certificate is malformed");
                continue;
            }
            for (char &c : expected)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            listed.insert(key);

            const auto it = b.fileHashes.find(key);
            if (it == b.fileHashes.end())
                problems.push_back(blobPrefix + path + ": missing");
            else if (it->second != expected)
                problems.push_back(blobPrefix + path + ": modified");
        }

        for (const auto &[key, hash] : b.fileHashes)
        {
            if (key != kCertificateName && listed.count(key) == 0)
                problems.push_back(blobPrefix + "assets/" + key + ": not in the certificate");
        }
        totalChecked += listed.size();
    }

    if (blobsWithCert == 0)
    {
        problems.push_back(std::string("none of the blobs carry ") + kCertificateName);
        return false;
    }

    std::cout << "[Assets] " << totalChecked << " files checked across " << blobsWithCert << " blob certificate(s): "
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

std::vector<std::string> LoadedBlobPaths()
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    std::vector<std::string> paths;
    paths.reserve(s.loadedBlobs.size());
    for (const auto &b : s.loadedBlobs)
        paths.push_back(b.path.string());
    return paths;
}

void ResetForTesting()
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.mode = Mode::None;
    s.description = "no assets located";
    s.blobFiles.clear();
    s.overlay.clear();
    s.mods.clear();
    s.root.clear();
    s.blobPath.clear();
    s.loadedBlobs.clear();
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
