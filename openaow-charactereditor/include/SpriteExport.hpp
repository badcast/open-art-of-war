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
// assets/data/sprites_graphics.json. This OVERWRITES those files (only the sheets and tables that really differ).
// Reads the object files (<NNN>_*.json + atlas) and the single sprite files (sprites_*.json + grid) of `inDir`.
bool ImportSpriteFolder(const fs::path& assetsRoot, const fs::path& inDir, std::string& report);

// For every object of the catalog the sprite that shows it best in a list: the one with the most solid pixels
// (shadows and smoke are translucent). -1 for an object without sprites.
std::vector<int> PickObjectThumbnails(const SpriteCatalog& catalog, const fs::path& assetsRoot);

// One PNG atlas (every sprite the object uses) and one JSON (ids, rectangles, dependency data) per object, named
// <NNN>_<kind>_<faction>_<name>_id<id>, plus index.json. `objectIndices` index SpriteCatalog::objects().
// `report` gets the result or the error.
bool ExportSpriteObjects(const SpriteCatalog& catalog, const fs::path& assetsRoot,
                         const fs::path& outDir, const std::vector<int>& objectIndices, std::string& report);

// Every raw sprite in the order of its id on a grid of rows and columns, one PNG (and JSON) per chosen group
bool ExportSingleSprites(const SpriteCatalog& catalog, const fs::path& assetsRoot,
                         const fs::path& outDir, const SingleExportOptions& options, std::string& report);

struct CustomAtlasItem {
    int id{0};
    int x{0};
    int y{0};
    int w{0};
    int h{0};
};

// Exports a custom sprite atlas layout with user-defined positions directly to PNG and JSON.
bool ExportCustomAtlas(const SpriteCatalog& catalog, const fs::path& assetsRoot,
                       const fs::path& outDir, const std::string& baseName,
                       const std::string& title, const std::vector<CustomAtlasItem>& items,
                       int atlasWidth, int atlasHeight,
                       const nlohmann::json* extraGraphicsData, std::string& report);

// Packs items tightly using MaxRects best-short-side fit
std::vector<CustomAtlasItem> PackCustomMaxRects(std::vector<CustomAtlasItem> items, int& width, int& height, int pad);

} // namespace CharEditor
