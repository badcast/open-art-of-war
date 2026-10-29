#include "CharacterEditor.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

// Without a window: openaow-charactereditor --export-sprites <folder>            one atlas + JSON per object
//                   openaow-charactereditor --export-single <folder> [columns [packed]]   every sprite on one grid (or packed atlas) per group
//                   openaow-charactereditor --import-sprites <folder> [assets]   put them back (OVERWRITES assets/sprites)
static int exportWithoutWindow(const std::string& mode, const std::string& folder, int columns, const std::string& assets = {},
                               bool packed = false) {
    const CharEditor::fs::path root = assets.empty() ? CharEditor::findAssetRoot() : CharEditor::fs::path(assets);
    if (root.empty()) {
        std::cerr << "Could not find the assets folder with graphics data.\n";
        return 1;
    }
    std::string report;
    if (mode == "--import-sprites") {
        const bool ok = CharEditor::importSpriteFolder(root, folder, report);
        (ok ? std::cout : std::cerr) << report << '\n';
        return ok ? 0 : 1;
    }
    CharEditor::SpriteCatalog catalog;
    if (!catalog.load(root, report)) {
        std::cerr << report << '\n';
        return 1;
    }
    bool ok = false;
    if (mode == "--export-sprites") {
        std::vector<int> all(catalog.objects().size());
        for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<int>(i);
        ok = CharEditor::exportSpriteObjects(catalog, root, folder, all, report);
    } else {
        CharEditor::SingleExportOptions options;
        options.columns = columns;
        options.packed = packed;
        ok = CharEditor::exportSingleSprites(catalog, root, folder, options, report);
    }
    (ok ? std::cout : std::cerr) << report << '\n';
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 3 && (std::string(argv[1]) == "--export-sprites" || std::string(argv[1]) == "--export-single")) {
        return exportWithoutWindow(argv[1], argv[2], argc >= 4 ? std::max(1, std::atoi(argv[3])) : 64, {},
                                   argc >= 5 && std::string(argv[4]) == "packed");
    }
    if (argc >= 3 && std::string(argv[1]) == "--import-sprites") {
        return exportWithoutWindow(argv[1], argv[2], 64, argc >= 4 ? argv[3] : "");
    }
    CharEditor::CharacterEditor editor;
    if (!editor.init()) {
        std::cerr << editor.errorMessage() << '\n';
        return 1;
    }
    return editor.run();
}
