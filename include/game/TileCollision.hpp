#pragma once

namespace OpenArtOfWar
{

    // Which ground tiles are impassable. Passability belongs to the tile sprite (bare earth, jungle and rock block,
    // grass, transition and rubble tiles do not); the table is derived from the original maps by
    // tools/derive_tile_collision.py into assets/data/tile_collision.json.
    class TileCollision
    {
    public:
        static bool isBlocked(int tilesetId, int tile);
    };

} // namespace OpenArtOfWar
