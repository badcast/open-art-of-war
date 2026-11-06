#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace OpenArtOfWar::MapFiles
{

// Maps in a network game: everybody must play on exactly the same file. The room names the map by its
// path and its hash; a player whose file differs (or is missing) gets it from the room, checked and
// kept in the per-user directory (downloaded_maps/), never in the game's own assets.

// Hash of a map file's bytes (0 when it cannot be read)
std::uint64_t HashOf(const std::string &path);
std::uint64_t HashOfBytes(const std::vector<std::uint8_t> &bytes);

// The file to play `path` with `hash` from: the game's own when it matches, else a downloaded copy;
// empty when there is none yet
std::string Resolve(const std::string &path, std::uint64_t hash);

// The game's own file, compressed for sending (empty when it cannot be read)
std::vector<std::uint8_t> Packed(const std::string &path);
// A received file, unpacked and checked against `hash` and `maxBytes` (what it claims to be, never more); false when
// anything is wrong
bool Unpack(const std::vector<std::uint8_t> &packed, std::uint64_t hash, std::vector<std::uint8_t> &raw, std::size_t maxBytes);
// A received file: unpacked, checked against `hash` and against being a map at all, stored.
// Returns the stored path, empty when anything is wrong.
std::string Store(const std::string &path, std::uint64_t hash, const std::vector<std::uint8_t> &packedBytes);

// The bytes of a map file (the game's own or any path); false when it cannot be read
bool Read(const std::string &path, std::vector<std::uint8_t> &bytes);
// Raw bytes of a map with `hash` (a replay carries its map): checked, then stored like a downloaded one.
// Returns the stored path, empty when the bytes are not that map.
std::string StoreRaw(const std::string &path, std::uint64_t hash, const std::vector<std::uint8_t> &raw);

} // namespace OpenArtOfWar::MapFiles
