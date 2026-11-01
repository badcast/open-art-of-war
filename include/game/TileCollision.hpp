#pragma once

namespace OpenArtOfWar
{

// Which ground tiles are impassable. Passability belongs to the tile sprite (bare earth, jungle and rock block,
// grass, transition and rubble tiles do not); the table is derived from the original maps by
// tools/derive_tile_collision.py into assets/data/maps_tile_collision.json.
class TileCollision
{
public:
    static bool IsBlocked(int tilesetId, int tile);
    // Reads the table again (a network game plays with the host's data)
    static void Reload();
};

} // namespace OpenArtOfWar
