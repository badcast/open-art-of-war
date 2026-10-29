#include "net/MapFiles.hpp"
#include "core/AssetStore.hpp"
#include "openaow/net/Crc32.hpp"
#include "openaow/net/Protocol.hpp"

#include <zstd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar::MapFiles
{

    namespace fs = std::filesystem;

    namespace
    {

        std::string downloadedPath(const std::string &path, uint64_t hash)
        {
            char suffix[24];
            std::snprintf(suffix, sizeof suffix, "_%016llx.json", static_cast<unsigned long long>(hash));
            return (fs::path(Assets::userDataDir()) / "downloaded_maps" / (fs::path(path).stem().string() + suffix)).string();
        }

        bool readAll(const std::string &path, std::vector<uint8_t> &out)
        {
            if(Assets::readBytes(path, out))
                return true;
            std::ifstream in(path, std::ios::binary);
            if(!in)
                return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

    } // namespace

    uint64_t hashOfBytes(const std::vector<uint8_t> &bytes)
    {
        // Two CRCs side by side: 64 bits, cheap, the same on every platform
        const uint32_t a = Net::crc32(bytes.data(), bytes.size());
        const uint32_t b = Net::crc32(bytes.data(), bytes.size(), 0x9e3779b9u ^ static_cast<uint32_t>(bytes.size()));
        return (static_cast<uint64_t>(a) << 32) | b;
    }

    uint64_t hashOf(const std::string &path)
    {
        std::vector<uint8_t> bytes;
        return readAll(path, bytes) ? hashOfBytes(bytes) : 0;
    }

    std::string resolve(const std::string &path, uint64_t hash)
    {
        if(hash == 0 || hashOf(path) == hash)
            return path;
        const std::string copy = downloadedPath(path, hash);
        return hashOf(copy) == hash ? copy : std::string();
    }

    std::vector<uint8_t> packed(const std::string &path)
    {
        std::vector<uint8_t> raw;
        if(!readAll(path, raw))
            return {};
        std::vector<uint8_t> out(ZSTD_compressBound(raw.size()));
        const size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), 15);
        if(ZSTD_isError(n) || n > Net::kMaxFileBytes)
            return {};
        out.resize(n);
        return out;
    }

    std::string store(const std::string &path, uint64_t hash, const std::vector<uint8_t> &packedBytes)
    {
        if(packedBytes.empty())
            return {};
        // The size it claims, bounded: a hostile file must not make us allocate gigabytes
        const unsigned long long size = ZSTD_getFrameContentSize(packedBytes.data(), packedBytes.size());
        if(size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > 16u * 1024 * 1024)
            return {};
        std::vector<uint8_t> raw(static_cast<size_t>(size));
        const size_t n = ZSTD_decompress(raw.data(), raw.size(), packedBytes.data(), packedBytes.size());
        if(ZSTD_isError(n) || n != raw.size() || hashOfBytes(raw) != hash)
            return {};
        // It must be a map: a JSON object with a sensible size
        const nlohmann::json doc = nlohmann::json::parse(raw.begin(), raw.end(), nullptr, false);
        if(!doc.is_object() || !doc.contains("width") || !doc.contains("height"))
            return {};
        const std::string target = downloadedPath(path, hash);
        std::error_code ec;
        fs::create_directories(fs::path(target).parent_path(), ec);
        const fs::path tmp = target + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if(!out)
                return {};
            out.write(reinterpret_cast<const char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
            if(!out.good())
                return {};
        }
        fs::rename(tmp, target, ec);
        return ec ? std::string() : target;
    }

} // namespace OpenArtOfWar::MapFiles
