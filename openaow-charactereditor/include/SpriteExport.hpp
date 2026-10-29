#pragma once

#include "SpriteCatalog.hpp"

#include <string>
#include <vector>

namespace CharEditor {

constexpr int kMaxExportColumns = 102400;

struct SingleExportOptions {
    bool groups[kSpriteGroupCount]{true, true, true, true};
    int columns{64};
    bool compress{false};  // rows packed to the width of their sprites (no empty cells)
    bool packed{false};    // one atlas with the sprites fitted into each other's empty space (MaxRects), cropped to what is used
    bool allInOne{false}; // one file with the chosen groups one after another instead of a file per group
};

// Puts what the exports above wrote back into the assets: the pixels of every sprite into assets/sprites/sheet_NN.png
// and the edited rows of the graphics tables (composites, animations, bodies, sequences, buildings) into
// assets/data/graphics_data.json. This OVERWRITES those files (only the sheets and tables that really differ).
// Reads the object files (<NNN>_*.json + atlas) and the single sprite files (sprites_*.json + grid) of `inDir`.
bool importSpriteFolder(const fs::path& assetsRoot, const fs::path& inDir, std::string& report);

// For every object of the catalog the sprite that shows it best in a list: the one with the most solid pixels
// (shadows and smoke are translucent). -1 for an object without sprites.
std::vector<int> pickObjectThumbnails(const SpriteCatalog& catalog, const fs::path& assetsRoot);

// One PNG atlas (every sprite the object uses) and one JSON (ids, rectangles, dependency data) per object, named
// <NNN>_<kind>_<faction>_<name>_id<id>, plus index.json. `objectIndices` index SpriteCatalog::objects().
// `report` gets the result or the error.
bool exportSpriteObjects(const SpriteCatalog& catalog, const fs::path& assetsRoot,
                         const fs::path& outDir, const std::vector<int>& objectIndices, std::string& report);

// Every raw sprite in the order of its id on a grid of rows and columns, one PNG (and JSON) per chosen group
bool exportSingleSprites(const SpriteCatalog& catalog, const fs::path& assetsRoot,
                         const fs::path& outDir, const SingleExportOptions& options, std::string& report);

} // namespace CharEditor
