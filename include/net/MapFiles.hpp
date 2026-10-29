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
    uint64_t hashOf(const std::string &path);
    uint64_t hashOfBytes(const std::vector<uint8_t> &bytes);

    // The file to play `path` with `hash` from: the game's own when it matches, else a downloaded copy;
    // empty when there is none yet
    std::string resolve(const std::string &path, uint64_t hash);

    // The game's own file, compressed for sending (empty when it cannot be read)
    std::vector<uint8_t> packed(const std::string &path);
    // A received file: unpacked, checked against `hash` and against being a map at all, stored.
    // Returns the stored path, empty when anything is wrong.
    std::string store(const std::string &path, uint64_t hash, const std::vector<uint8_t> &packedBytes);

} // namespace OpenArtOfWar::MapFiles
