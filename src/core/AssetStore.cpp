#include "core/AssetStore.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
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
            std::map<std::string, std::vector<uint8_t>> blobFiles;
            std::string userDir;
        };

        State &state()
        {
            static State s;
            return s;
        }

        std::vector<fs::path> CandidateRoots()
        {
            std::vector<fs::path> roots {fs::path(".")};
            if(const char *base = SDL_GetBasePath())
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
            while(it != normal.end() && (*it == ".." || *it == "."))
                ++it;
            if(it == normal.end() || *it != "assets")
                return false;
            ++it;
            fs::path rest;
            for(; it != normal.end(); ++it)
                rest /= *it;
            key = rest.generic_string();
            return true;
        }

        std::string UserDirLocked(State &s)
        {
            if(s.userDir.empty())
            {
                if(char *pref = SDL_GetPrefPath("openaow", "openaow"))
                {
                    s.userDir = pref;
                    SDL_free(pref);
                }
                else
                {
                    s.userDir = "./";
                }
                while(s.userDir.size() > 1 && (s.userDir.back() == '/' || s.userDir.back() == '\\'))
                    s.userDir.pop_back();
            }
            return s.userDir;
        }

        bool ReadFile(const fs::path &p, std::vector<uint8_t> &out)
        {
            std::ifstream f(p, std::ios::binary);
            if(!f)
                return false;
            out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            return true;
        }

        // Unpacks every regular file of the tar.zst blob into memory (a tar stream has no random access)
        bool LoadBlob(const fs::path &blob, std::map<std::string, std::vector<uint8_t>> &files)
        {
            archive *a = archive_read_new();
            archive_read_support_filter_all(a);
            archive_read_support_format_tar(a);
            if(archive_read_open_filename(a, blob.string().c_str(), 1 << 20) != ARCHIVE_OK)
            {
                std::cerr << "[Assets] Cannot open " << blob << ": " << archive_error_string(a) << std::endl;
                archive_read_free(a);
                return false;
            }
            archive_entry *entry = nullptr;
            int status;
            while((status = archive_read_next_header(a, &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN)
            {
                if(archive_entry_filetype(entry) != AE_IFREG)
                    continue;
                std::vector<uint8_t> data(static_cast<size_t>(archive_entry_size(entry)));
                size_t got = 0;
                while(got < data.size())
                {
                    const la_ssize_t n = archive_read_data(a, data.data() + got, data.size() - got);
                    if(n <= 0)
                        break;
                    got += static_cast<size_t>(n);
                }
                data.resize(got);
                std::string name = fs::path(archive_entry_pathname(entry)).lexically_normal().generic_string();
                if(name.compare(0, 2, "./") == 0)
                    name.erase(0, 2);
                files[name] = std::move(data);
            }
            const bool ok = status == ARCHIVE_EOF;
            if(!ok)
                std::cerr << "[Assets] Cannot read " << blob << ": " << archive_error_string(a) << std::endl;
            archive_read_free(a);
            return ok;
        }

        // Blob lookups; the caller holds the lock
        bool BlobRead(State &s, const std::string &key, std::vector<uint8_t> &out)
        {
            const auto it = s.blobFiles.find(key);
            if(key.empty() || it == s.blobFiles.end())
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
        if(s.mode != Mode::None)
            return true;

        std::error_code ec;
        const std::vector<fs::path> roots = CandidateRoots();

        // 1. A real "assets" directory
        for(const fs::path &root : roots)
        {
            if(fs::is_directory(root / "assets" / "characters", ec))
            {
                if(root != fs::path("."))
                    fs::current_path(root, ec);
                s.mode = Mode::Directory;
                s.description = "directory " + fs::current_path(ec).string() + "/assets";
                return true;
            }
        }

        // 2. data.blob with the packed contents of that directory
        for(const fs::path &root : roots)
        {
            const fs::path blob = root / MainAssetBlobName;
            if(!fs::is_regular_file(blob, ec))
                continue;
            if(!LoadBlob(blob, s.blobFiles))
            {
                s.blobFiles.clear();
                continue;
            }
            s.mode = Mode::Blob;
            s.description = "blob " + fs::weakly_canonical(blob, ec).string();
            return true;
        }

        std::cerr << "[Assets] Neither an \"assets\" directory nor " << MainAssetBlobName << " was found" << std::endl;
        return false;
    }

    Mode mode()
    {
        return state().mode;
    }

    std::string Describe()
    {
        return state().description;
    }

    bool Exists(const std::string &path)
    {
        State &s = state();
        std::string key;
        if(s.mode != Mode::Blob || !AssetKey(path, key))
        {
            std::error_code ec;
            return fs::exists(path, ec);
        }
        std::lock_guard<std::mutex> guard(s.lock);
        std::error_code ec;
        return fs::exists(fs::path(UserDirLocked(s)) / key, ec) || BlobHas(s, key);
    }

    bool ReadBytes(const std::string &path, std::vector<uint8_t> &out)
    {
        State &s = state();
        std::string key;
        if(s.mode != Mode::Blob || !AssetKey(path, key))
            return ReadFile(path, out);
        std::lock_guard<std::mutex> guard(s.lock);
        if(ReadFile(fs::path(UserDirLocked(s)) / key, out))
            return true;
        return BlobRead(s, key, out);
    }

    bool ReadText(const std::string &path, std::string &out)
    {
        std::vector<uint8_t> bytes;
        if(!ReadBytes(path, bytes))
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

        auto scanDirectory = [&](const fs::path &p)
        {
            for(const auto &entry : fs::directory_iterator(p, ec))
            {
                if(entry.is_regular_file(ec))
                    names.insert(entry.path().filename().string());
            }
        };

        if(!inBlob)
        {
            scanDirectory(dir);
        }
        else
        {
            std::lock_guard<std::mutex> guard(s.lock);
            scanDirectory(fs::path(UserDirLocked(s)) / key);
            const std::string prefix = key.empty() ? "" : key + "/";
            for(const auto &[full, data] : s.blobFiles)
            {
                if(full.compare(0, prefix.size(), prefix) != 0)
                    continue;
                const std::string rest = full.substr(prefix.size());
                if(!rest.empty() && rest.find('/') == std::string::npos)
                    names.insert(rest);
            }
        }

        std::vector<std::string> result;
        for(const std::string &name : names)
        {
            if(!extension.empty() && fs::path(name).extension() != extension)
                continue;
            result.push_back((fs::path(dir) / name).generic_string());
        }
        return result;
    }

    std::string WritablePath(const std::string &path)
    {
        State &s = state();
        std::string target = path;
        if(s.mode == Mode::Blob)
        {
            std::string key;
            std::lock_guard<std::mutex> guard(s.lock);
            target = (fs::path(UserDirLocked(s)) / (AssetKey(path, key) ? key : fs::path(path).lexically_normal().generic_string())).string();
        }
        std::error_code ec;
        const fs::path parent = fs::path(target).parent_path();
        if(!parent.empty())
            fs::create_directories(parent, ec);
        return target;
    }

    std::string RealDirectory(const std::string &dir)
    {
        State &s = state();
        std::string key;
        if(s.mode != Mode::Blob || !AssetKey(dir, key))
            return dir;

        std::lock_guard<std::mutex> guard(s.lock);
        const fs::path out = fs::path(UserDirLocked(s)) / "cache" / key;
        std::error_code ec;
        fs::create_directories(out, ec);
        const std::string prefix = key.empty() ? "" : key + "/";
        for(const auto &[full, data] : s.blobFiles)
        {
            if(full.compare(0, prefix.size(), prefix) != 0)
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
