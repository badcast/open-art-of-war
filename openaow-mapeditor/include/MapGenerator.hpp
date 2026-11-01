#pragma once

#include "MapDocument.hpp"
#include <cstdint>

namespace OpenArtOfWar {

enum class GeneratorSymmetry : int {
    None = 0,           // Natural asymmetry
    Rotational180 = 1,  // 180° rotational (tournament symmetry)
    Diagonal = 2,       // Diagonal
    Horizontal = 3,     // Mirrored horizontally
    Vertical = 4        // Mirrored vertically
};

struct GeneratorParams {
    int width{60};
    int height{40};
    int tilesetId{0};                   // 0 = Jungle, 1 = Desert
    GeneratorSymmetry symmetry{GeneratorSymmetry::Rotational180};
    uint32_t seed{1337};
    bool randomSeed{true};
    int players{2};                     // 2..8 bases; more than two get no symmetry (bases on a ring around the centre)

    // Shares in percent. Defaults: medians of the original jungle maps (tileset 0);
    // MapGenerator::OriginalDefaults() gives the values of either tileset.
    int islandPercent{41};              // impassable islands, share of the map
    int edgePercent{74};                // share of the map edge closed by islands
    int rockPercent{9};                 // rocks among the island decorations (the rest are trees)
    int coverPercent{105};              // decoration footprints added up, share of the island area (over 100 = overlap)
    int darkGrassPercent{53};           // dark grass, share of the open ground
    int earthPercent{8};                // earth (tileset 1: earth and sand), share of the open ground
    int variantPercent{46};             // speckled variants among the plain ground tiles
    int rubblePercent{2};               // rubble on the open grass
    int coreWaterPercent{30};           // water under the island cores
    int coreJunglePercent{27};          // jungle tiles under the island cores
};

// Skirmish map generator that follows the layout of the original maps: seamless ground from the game's
// corner tiles (light / dark grass, earth, sand, water), impassable islands built from overlapping rock and
// tree decorations, open ground left free, and the bases joined by open roads. The percentages of
// GeneratorParams set every share.
class MapGenerator {
public:
    static void Generate(MapDocument& doc, GeneratorParams params);

    // One click: a map of the document's size and tileset with the values of the original maps
    static void GenerateQuick(MapDocument& doc);

    // Copies the medians measured on the original maps of a tileset into the percentage fields
    static void OriginalDefaults(GeneratorParams& params, int tilesetId);

    // One percentage setting: what it is, its slider range and the range seen on the original maps
    struct Setting {
        const char* label;
        int GeneratorParams::*field;
        int min;
        int max;
        const char* hint;
    };
    static const Setting* Settings(int& count);
};

} // namespace OpenArtOfWar
