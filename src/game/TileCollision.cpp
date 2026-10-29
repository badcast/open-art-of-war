#include "game/TileCollision.hpp"
#include "core/AssetStore.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <iostream>
#include <string>

namespace OpenArtOfWar
{

    namespace
    {

        constexpr int kTilesets = 2;
        constexpr int kTiles = 256;

        using Table = std::array<std::array<bool, kTiles>, kTilesets>;

        // Without the data file: the old rule, the tiles from the first water tile up are blocked
        Table fallbackTable()
        {
            constexpr int kBlockedFrom[kTilesets] = {211, 191};
            Table table {};
            for(int tileset = 0; tileset < kTilesets; ++tileset)
            {
                for(int tile = kBlockedFrom[tileset]; tile < kTiles; ++tile)
                    table[tileset][tile] = true;
            }
            return table;
        }

        Table loadTable()
        {
            std::string text;
            if(!Assets::readText("assets/data/tile_collision.json", text))
            {
                std::cerr << "[TileCollision] assets/data/tile_collision.json not found, using the water-tile rule" << std::endl;
                return fallbackTable();
            }
            Table table {};
            try
            {
                const nlohmann::json doc = nlohmann::json::parse(text);
                const auto &tilesets = doc.at("tilesets");
                for(int tileset = 0; tileset < kTilesets && tileset < static_cast<int>(tilesets.size()); ++tileset)
                {
                    for(const auto &range : tilesets[tileset].at("blocked"))
                    {
                        for(int tile = range.at(0).get<int>(); tile <= range.at(1).get<int>() && tile < kTiles; ++tile)
                        {
                            if(tile >= 0)
                                table[tileset][tile] = true;
                        }
                    }
                }
            }
            catch(const nlohmann::json::exception &error)
            {
                std::cerr << "[TileCollision] bad tile_collision.json (" << error.what() << "), using the water-tile rule" << std::endl;
                return fallbackTable();
            }
            return table;
        }

    } // namespace

    bool TileCollision::isBlocked(int tilesetId, int tile)
    {
        static const Table table = loadTable();
        if(tile < 0 || tile >= kTiles)
            return false;
        return table[tilesetId == 1 ? 1 : 0][tile];
    }

} // namespace OpenArtOfWar
