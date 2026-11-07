#include "openaow/net/ReplayFile.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace OpenArtOfWar::Net
{

namespace
{

namespace fs = std::filesystem;

constexpr const char *kMetaName = "replay.json";
constexpr const char *kSetupName = "setup.bin";
constexpr const char *kTurnsName = "turns.bin";
constexpr const char *kChecksumsName = "checksums.bin";

void putU32(std::vector<std::uint8_t> &out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void putU64(std::vector<std::uint8_t> &out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint32_t getU32(const std::uint8_t *p)
{
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    return v;
}

std::uint64_t getU64(const std::uint8_t *p)
{
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

bool addEntry(archive *a, const std::string &name, const std::uint8_t *data, std::size_t size, std::string &error)
{
    archive_entry *e = archive_entry_new();
    archive_entry_set_pathname(e, name.c_str());
    archive_entry_set_size(e, static_cast<la_int64_t>(size));
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_entry_set_mtime(e, 0, 0); // the same bytes in, the same file out
    const bool ok = archive_write_header(a, e) == ARCHIVE_OK
                    && (size == 0 || archive_write_data(a, data, size) == static_cast<la_ssize_t>(size));
    if (!ok)
        error = std::string("cannot write ") + name + ": " + archive_error_string(a);
    archive_entry_free(e);
    return ok;
}

} // namespace

bool validReplayFileName(const std::string &name)
{
    if (name.empty() || name.size() > kMaxReplayNameLength)
        return false;
    if (name.rfind("maps/", 0) != 0 && name.rfind("data/", 0) != 0)
        return false;
    if (name.back() == '/' || name.find("//") != std::string::npos)
        return false;
    std::size_t partStart = 0;
    for (std::size_t i = 0; i <= name.size(); ++i)
    {
        if (i == name.size() || name[i] == '/')
        {
            const std::string part = name.substr(partStart, i - partStart);
            if (part.empty() || part == "." || part == "..")
                return false;
            partStart = i + 1;
            continue;
        }
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'
                        || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

std::string replayFileName(const std::string &mapName, std::int64_t when)
{
    const std::time_t t = static_cast<std::time_t>(when);
    std::tm local {};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    std::string map;
    for (char c : fs::path(mapName).stem().string())
    {
        const bool ok
            = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        map += ok ? c : '_';
    }
    if (map.empty())
        map = "match";
    return std::string("open-art-of-war-") + stamp + "_" + map + ".awr";
}

bool writeReplay(const std::string &path, const ReplayData &data, std::string &error)
{
    // Apply the reader's limits before touching the destination. Otherwise a
    // successful recording can produce a replay we cannot open ourselves.
    std::size_t total = 0;
    auto account = [&](std::size_t size)
    {
        if (size > kMaxReplayEntryBytes || size > kMaxReplayTotalBytes - total)
            return false;
        total += size;
        return true;
    };
    if (data.files.size() > kMaxReplayEntries - 4 || data.turns.size() > kMaxReplayTurns
        || data.checksums.size() > kMaxReplayEntryBytes / 12)
    {
        error = "replay has too many entries, turns or checksums";
        return false;
    }
    std::size_t turnsSize = 0;
    for (const auto &turn : data.turns)
    {
        if (kMaxReplayEntryBytes - turnsSize < 4 || turn.size() > kMaxReplayEntryBytes - turnsSize - 4)
        {
            error = "turn list too big";
            return false;
        }
        turnsSize += 4 + turn.size();
    }
    if (!account(data.meta.size()) || !account(data.setup.size()) || !account(turnsSize)
        || !account(data.checksums.size() * 12))
    {
        error = "replay too big";
        return false;
    }
    for (const auto &[name, bytes] : data.files)
    {
        if (!validReplayFileName(name) || !account(bytes.size()))
        {
            error = "bad extra file " + name;
            return false;
        }
    }
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path())
        fs::create_directories(target.parent_path(), ec);
    const std::string tmp = path + ".tmp";

    archive *a = archive_write_new();
    if (archive_write_set_format_pax_restricted(a) != ARCHIVE_OK || archive_write_add_filter_zstd(a) != ARCHIVE_OK)
    {
        error = std::string("zstd tar not available: ") + archive_error_string(a);
        archive_write_free(a);
        return false;
    }
    archive_write_set_filter_option(a, "zstd", "compression-level", "9");
    if (archive_write_open_filename(a, tmp.c_str()) != ARCHIVE_OK)
    {
        error = std::string("cannot create ") + tmp + ": " + archive_error_string(a);
        archive_write_free(a);
        return false;
    }
    bool ok = addEntry(a, kMetaName, reinterpret_cast<const std::uint8_t *>(data.meta.data()), data.meta.size(), error);
    ok = ok && addEntry(a, kSetupName, data.setup.data(), data.setup.size(), error);
    if (ok)
    {
        std::vector<std::uint8_t> turns;
        for (const auto &t : data.turns)
        {
            putU32(turns, static_cast<std::uint32_t>(t.size()));
            turns.insert(turns.end(), t.begin(), t.end());
        }
        ok = addEntry(a, kTurnsName, turns.data(), turns.size(), error);
    }
    if (ok)
    {
        std::vector<std::uint8_t> sums;
        for (const auto &[tick, hash] : data.checksums)
        {
            putU32(sums, tick);
            putU64(sums, hash);
        }
        ok = addEntry(a, kChecksumsName, sums.data(), sums.size(), error);
    }
    for (auto it = data.files.begin(); ok && it != data.files.end(); ++it)
        ok = addEntry(a, it->first, it->second.data(), it->second.size(), error);
    if (archive_write_close(a) != ARCHIVE_OK && ok)
    {
        error = std::string("cannot finish ") + tmp + ": " + archive_error_string(a);
        ok = false;
    }
    archive_write_free(a);
    if (!ok)
    {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec)
    {
        error = "cannot rename to " + path + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool readReplay(const std::string &path, ReplayData &out, std::string &error)
{
    out = ReplayData {};
    archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_tar(a);
    if (archive_read_open_filename(a, path.c_str(), 1 << 16) != ARCHIVE_OK)
    {
        error = std::string("cannot open: ") + archive_error_string(a);
        archive_read_free(a);
        return false;
    }
    bool haveMeta = false, haveSetup = false, haveTurns = false, haveSums = false;
    std::vector<std::uint8_t> turnBytes, sumBytes;
    std::size_t total = 0, entries = 0;
    bool ok = true;
    archive_entry *e = nullptr;
    int status;
    while ((status = archive_read_next_header(a, &e)) == ARCHIVE_OK || status == ARCHIVE_WARN)
    {
        if (++entries > kMaxReplayEntries)
        {
            error = "too many entries";
            ok = false;
            break;
        }
        if (archive_entry_filetype(e) != AE_IFREG)
        {
            error = "unexpected entry type";
            ok = false;
            break;
        }
        const char *rawName = archive_entry_pathname(e);
        const std::string name = rawName ? rawName : "";
        const la_int64_t declared = archive_entry_size(e);
        if (declared < 0 || static_cast<std::uint64_t>(declared) > kMaxReplayEntryBytes
            || total + static_cast<std::size_t>(declared) > kMaxReplayTotalBytes)
        {
            error = "entry too big: " + name;
            ok = false;
            break;
        }
        total += static_cast<std::size_t>(declared);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(declared));
        std::size_t got = 0;
        while (got < bytes.size())
        {
            const la_ssize_t n = archive_read_data(a, bytes.data() + got, bytes.size() - got);
            if (n <= 0)
                break;
            got += static_cast<std::size_t>(n);
        }
        if (got != bytes.size())
        {
            error = "truncated entry: " + name;
            ok = false;
            break;
        }
        if (name == kMetaName && !haveMeta)
        {
            out.meta.assign(bytes.begin(), bytes.end());
            haveMeta = true;
        }
        else if (name == kSetupName && !haveSetup)
        {
            out.setup = std::move(bytes);
            haveSetup = true;
        }
        else if (name == kTurnsName && !haveTurns)
        {
            turnBytes = std::move(bytes);
            haveTurns = true;
        }
        else if (name == kChecksumsName && !haveSums)
        {
            sumBytes = std::move(bytes);
            haveSums = true;
        }
        else if (validReplayFileName(name) && !out.files.count(name))
        {
            out.files[name] = std::move(bytes);
        }
        else
        {
            error = "unexpected entry: " + name;
            ok = false;
            break;
        }
    }
    if (ok && status != ARCHIVE_EOF)
    {
        error = std::string("damaged archive: ") + archive_error_string(a);
        ok = false;
    }
    archive_read_free(a);
    if (!ok)
        return false;
    if (!haveMeta || !haveSetup || !haveTurns || !haveSums)
    {
        error = "not a replay (missing entries)";
        return false;
    }
    for (std::size_t pos = 0; pos < turnBytes.size();)
    {
        if (turnBytes.size() - pos < 4 || out.turns.size() >= kMaxReplayTurns)
        {
            error = "damaged turn list";
            return false;
        }
        const std::uint32_t len = getU32(turnBytes.data() + pos);
        pos += 4;
        if (len > turnBytes.size() - pos)
        {
            error = "damaged turn list";
            return false;
        }
        out.turns.emplace_back(turnBytes.begin() + pos, turnBytes.begin() + pos + len);
        pos += len;
    }
    if (sumBytes.size() % 12 != 0)
    {
        error = "damaged checksum list";
        return false;
    }
    for (std::size_t pos = 0; pos < sumBytes.size(); pos += 12)
        out.checksums.emplace_back(getU32(sumBytes.data() + pos), getU64(sumBytes.data() + pos + 4));
    return true;
}

} // namespace OpenArtOfWar::Net
