#pragma once

#include "MapDocument.hpp"
#include <cstdint>

namespace OpenArtOfWar {

enum class GeneratorBiome : int {
    Plains = 0,         // Green plains and meadows
    RiverValley = 1,    // River valley with bridges
    DenseJungle = 2,    // Dense Peruvian jungle
    DesertCanyon = 3,   // Desert canyon and dunes
    Archipelago = 4     // Island archipelago
};

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
    GeneratorBiome biome{GeneratorBiome::RiverValley};
    GeneratorSymmetry symmetry{GeneratorSymmetry::Rotational180};

    bool addRiver{true};
    int riverBridges{2};
    bool generateBases{true};           // clear the start areas of both players (always done by SmartGenerator)
    int decorDensity{1};                // 0 = few, 1 = medium, 2 = many
    uint32_t seed{1337};
    bool randomSeed{true};

    // Used by SmartGenerator only, in percent. Defaults: medians of the original jungle maps (tileset 0);
    // SmartGenerator::OriginalDefaults() gives the values of either tileset.
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

class MapAutoGenerator {
public:
    static void Generate(MapDocument& doc, GeneratorParams params);
    static void GenerateQuickRandom(MapDocument& doc);
};

} // namespace OpenArtOfWar
