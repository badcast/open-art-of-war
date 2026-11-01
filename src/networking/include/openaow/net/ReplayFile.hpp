#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace OpenArtOfWar::Net
{

// A recorded network match, as a file: a tar archive compressed with zstd, extension ".awr" (Art of War Replay).
// The container knows nothing about the game: it keeps the match setup and the stream of turns as the network
// sent them, plus the checksums of the world and any extra files the writer wants to keep with them.
//
//   replay.json      meta (JSON text, written and read by the owner of the file)
//   setup.bin        the match setup bytes of StartMatch
//   turns.bin        every closed turn: u32 length + the encoded TurnBundle, in order
//   checksums.bin    (u32 tick, u64 hash) pairs, little endian
//   maps/..., data/...  extra files (a client's replay carries its map and the simulation data files)
struct ReplayData
{
    std::string meta;
    std::vector<std::uint8_t> setup;
    std::vector<std::vector<std::uint8_t>> turns;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> checksums;
    std::map<std::string, std::vector<std::uint8_t>> files; // name inside the archive -> bytes
};

// Limits of a file read from disk (it may come from anywhere): anything bigger is refused
constexpr std::size_t kMaxReplayEntryBytes = 32u * 1024u * 1024u;
constexpr std::size_t kMaxReplayTotalBytes = 96u * 1024u * 1024u;
constexpr std::size_t kMaxReplayEntries = 160;
constexpr std::size_t kMaxReplayNameLength = 128;
constexpr std::size_t kMaxReplayTurns = 2u * 1000u * 1000u;

// Extra file names: relative, below maps/ or data/, only [A-Za-z0-9._-] and '/', no ".." and no empty parts
bool validReplayFileName(const std::string &name);

// "open-art-of-war-YYYYMMDD-HHMMSS_<map>.awr" for a match that started at `when` (local time); the map name is
// reduced to safe characters
std::string replayFileName(const std::string &mapName, std::int64_t when);

// Writes `data` to `path` (through a temporary file and a rename; missing folders are created). False with `error`.
bool writeReplay(const std::string &path, const ReplayData &data, std::string &error);
// Reads and checks a file. False with `error` for anything that is not a well-formed replay within the limits.
bool readReplay(const std::string &path, ReplayData &out, std::string &error);

} // namespace OpenArtOfWar::Net
