#pragma once

#include "MapAutoGenerator.hpp"

namespace OpenArtOfWar {

// Skirmish map generator that follows the layout of the original maps: the ground is built from the
// game's own grass/dirt transition tiles, blocked terrain forms jungle clumps, decorations are grouped by
// the terrain they stand on, the map edge is walled in, and the bases are joined by an open road.
// Shares GeneratorParams with MapAutoGenerator (biome and river options are ignored).
class SmartGenerator {
public:
    static void generate(MapDocument& doc, GeneratorParams params);
};

} // namespace OpenArtOfWar
