#pragma once

#include "MapAutoGenerator.hpp"

namespace OpenArtOfWar {

// Skirmish map generator that follows the layout of the original maps: seamless ground from the game's
// corner tiles (light / dark grass, earth, sand, water), impassable islands built from overlapping rock and
// tree decorations, open ground left free, and the bases joined by open roads. The percentages of
// GeneratorParams set every share; the biome and river options of MapAutoGenerator are ignored.
class SmartGenerator {
public:
    static void Generate(MapDocument& doc, GeneratorParams params);

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
