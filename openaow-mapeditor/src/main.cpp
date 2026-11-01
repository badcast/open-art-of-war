#include "MapEditorApp.hpp"
#include "MapDocument.hpp"
#include "MapGenerator.hpp"
#include "TileCorners.hpp"
#include "core/AssetStore.hpp"
#include "game/TileCollision.hpp"
#include "game/TileMap.hpp"
#include <fstream>
#include <iostream>
#include <string>
#include <filesystem>

// 3b. MapGenerator: every symmetry on both tilesets, reproducible, seamless ground, bases joined by open ground
static bool RunMapGeneratorTest() {
    using namespace OpenArtOfWar;
    const GeneratorSymmetry symmetries[] = {GeneratorSymmetry::None, GeneratorSymmetry::Rotational180,
                                            GeneratorSymmetry::Diagonal, GeneratorSymmetry::Horizontal,
                                            GeneratorSymmetry::Vertical};
    for (int tileset = 0; tileset < 2; ++tileset) {
        for (GeneratorSymmetry symmetry : symmetries) {
            GeneratorParams params;
            params.width = 72;
            params.height = 56;
            params.tilesetId = tileset;
            MapGenerator::OriginalDefaults(params, tileset);
            params.symmetry = symmetry;
            params.randomSeed = false;
            params.seed = 4242;

            MapDocument doc;
            MapDocument again;
            MapGenerator::Generate(doc, params);
            MapGenerator::Generate(again, params);
            const TileMap& map = doc.GetTileMap();
            const std::string label = "tileset " + std::to_string(tileset) + ", symmetry " +
                                      std::to_string(static_cast<int>(symmetry));

            bool same = doc.GetEntities().size() == again.GetEntities().size() &&
                        doc.GetTileMap().GetDecorations().size() == again.GetTileMap().GetDecorations().size();
            int open = 0;
            for (int y = 0; y < map.GetHeight(); ++y) {
                for (int x = 0; x < map.GetWidth(); ++x) {
                    same = same && map.GetBaseTile(x, y) == again.GetTileMap().GetBaseTile(x, y);
                    open += map.IsPassable(x, y) ? 1 : 0;
                }
            }
            if (!same) {
                std::cerr << "[MapEditor-Test] FAILED: MapGenerator is not reproducible (" << label << ")." << std::endl;
                return false;
            }
            // Neighbouring ground tiles share their corners (no seams between grass, earth and water)
            const char* const* corners = tileset == 1 ? TileCorners::kTileset1 : TileCorners::kTileset0;
            int seams = 0;
            for (int y = 0; y < map.GetHeight(); ++y) {
                for (int x = 0; x < map.GetWidth(); ++x) {
                    const int tile = map.GetBaseTile(x, y);
                    if (tile >= TileCorners::kGroundTiles) continue;
                    const char* a = corners[tile];
                    if (x + 1 < map.GetWidth() && map.GetBaseTile(x + 1, y) < TileCorners::kGroundTiles) {
                        const char* b = corners[map.GetBaseTile(x + 1, y)];
                        seams += (a[1] != b[0] || a[3] != b[2]) ? 1 : 0;
                    }
                    if (y + 1 < map.GetHeight() && map.GetBaseTile(x, y + 1) < TileCorners::kGroundTiles) {
                        const char* b = corners[map.GetBaseTile(x, y + 1)];
                        seams += (a[2] != b[0] || a[3] != b[1]) ? 1 : 0;
                    }
                }
            }
            if (seams > 0) {
                std::cerr << "[MapEditor-Test] FAILED: MapGenerator left " << seams << " ground seams (" << label << ")."
                          << std::endl;
                return false;
            }

            const float openShare = static_cast<float>(open) / (map.GetWidth() * map.GetHeight());
            if (openShare < 0.3f || openShare > 0.9f) {
                std::cerr << "[MapEditor-Test] FAILED: MapGenerator open ground share " << openShare
                          << " out of range (" << label << ")." << std::endl;
                return false;
            }

            // Flood fill from the first open tile next to the player base
            std::vector<char> seen(map.GetWidth() * map.GetHeight(), 0);
            std::vector<std::pair<int, int>> queue;
            const auto seed = [&](int cx, int cy) {
                for (int dy = -6; dy <= 16 && queue.empty(); ++dy) {
                    for (int dx = -6; dx <= 16 && queue.empty(); ++dx) {
                        const int x = cx + dx, y = cy + dy;
                        if (x >= 0 && y >= 0 && x < map.GetWidth() && y < map.GetHeight() && map.IsPassable(x, y)) {
                            queue.push_back({x, y});
                            seen[y * map.GetWidth() + x] = 1;
                        }
                    }
                }
            };
            seed(doc.GetPlayerStartX(), doc.GetPlayerStartY());
            for (size_t head = 0; head < queue.size(); ++head) {
                const auto [cx, cy] = queue[head];
                const int step[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& s : step) {
                    const int nx = cx + s[0], ny = cy + s[1];
                    if (nx < 0 || ny < 0 || nx >= map.GetWidth() || ny >= map.GetHeight()) continue;
                    if (!map.IsPassable(nx, ny) || seen[ny * map.GetWidth() + nx]) continue;
                    seen[ny * map.GetWidth() + nx] = 1;
                    queue.push_back({nx, ny});
                }
            }
            bool reached = false;
            for (int dy = -3; dy <= 12 && !reached; ++dy) {
                for (int dx = -3; dx <= 12 && !reached; ++dx) {
                    const int x = doc.GetEnemyStartX() + dx, y = doc.GetEnemyStartY() + dy;
                    reached = x >= 0 && y >= 0 && x < map.GetWidth() && y < map.GetHeight() &&
                              seen[y * map.GetWidth() + x];
                }
            }
            if (!reached) {
                std::cerr << "[MapEditor-Test] FAILED: MapGenerator bases are not connected (" << label << ")." << std::endl;
                return false;
            }
        }
    }
    std::cout << "  - MapGenerator: 10 maps reproducible, seamless, bases connected OK" << std::endl;
    return true;
}

// Passability follows the tile sprite: the table agrees with the collision masks of the map_mission_*/map_skirmish_*.json maps it was
// derived from, every map loads with the blocking tiles blocked, and the editing tools keep a cell's passability in
// step with its tile, decoration, wall and building
static bool RunTileCollisionTest() {
    using namespace OpenArtOfWar;
    namespace fs = std::filesystem;

    long cells = 0, agree = 0, blockedTiles = 0, blockedTilesOpen = 0;
    int mapsUsed = 0;
    for (const auto& entry : fs::directory_iterator("assets/maps")) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("map_", 0) != 0 || entry.path().extension() != ".json") continue;
        std::string text;
        if (!Assets::ReadText(entry.path().generic_string(), text)) continue;
        const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
        if (doc.is_discarded()) continue;
        ++mapsUsed;
        const int w = doc["width"].get<int>(), h = doc["height"].get<int>(), tileset = doc["tilesetId"].get<int>();
        std::vector<int> tiles;
        for (const auto& run : doc["terrainRle"]) {
            tiles.insert(tiles.end(), run["runLength"].get<int>(), run["tileType"].get<int>());
        }
        tiles.resize(static_cast<size_t>(w) * h, 0);
        std::vector<char> nearDecor(w * h, 0);   // footprints reach up to 4 right / 3 down of the anchor
        for (const auto& d : doc["decorations"]) {
            const int dx = d["x"].get<int>(), dy = d["y"].get<int>();
            for (int y = dy; y < dy + 3 && y < h; ++y) {
                for (int x = dx; x < dx + 4 && x < w; ++x) nearDecor[y * w + x] = 1;
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (nearDecor[y * w + x]) continue;
                const bool blocked = TileCollision::IsBlocked(tileset, tiles[y * w + x]);
                const bool masked = (doc["collisionMask"][y][x / 32].get<uint32_t>() & (1u << (31 - x % 32))) != 0;
                ++cells;
                agree += blocked == masked ? 1 : 0;
                if (blocked) {
                    ++blockedTiles;
                    blockedTilesOpen += masked ? 0 : 1;
                }
            }
        }
        TileMap loaded;
        if (!loaded.LoadMap(entry.path().generic_string())) continue;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (TileCollision::IsBlocked(tileset, loaded.GetBaseTile(x, y)) && loaded.IsPassable(x, y)) {
                    std::cerr << "[MapEditor-Test] FAILED: " << name << " loads with a blocking tile open at " << x
                              << "," << y << "." << std::endl;
                    return false;
                }
            }
        }
    }
    if (mapsUsed < 10 || agree < cells * 0.95 || blockedTilesOpen > blockedTiles * 0.05) {
        std::cerr << "[MapEditor-Test] FAILED: tile collision table disagrees with the map_* maps ("
                  << mapsUsed << " maps, agree " << agree << "/" << cells << ", blocked tiles left open "
                  << blockedTilesOpen << "/" << blockedTiles << ")." << std::endl;
        return false;
    }

    // A hand-made map without a mask of its own: its bare-earth strip blocks all the same
    TileMap handMade;
    if (handMade.LoadMap("assets/maps/testmap.json") &&
        (handMade.IsPassable(27, 1) || handMade.IsPassable(26, 5))) {
        std::cerr << "[MapEditor-Test] FAILED: testmap.json loads with its bare-earth strip open." << std::endl;
        return false;
    }

    const int earth = 26, grass = 0;
    MapDocument doc;
    doc.NewMap(20, 20, 0, grass);
    if (!TileCollision::IsBlocked(0, earth) || TileCollision::IsBlocked(0, grass) ||
        !TileCollision::IsBlocked(1, 33) || TileCollision::IsBlocked(1, 0) || TileCollision::IsBlocked(0, 60)) {
        std::cerr << "[MapEditor-Test] FAILED: tile collision table has wrong entries." << std::endl;
        return false;
    }
    doc.SetTile(5, 5, earth);
    doc.SetTileBrush(8, 8, earth, 2);
    doc.FloodFill(15, 15, earth);
    const bool paintedBlocked = !doc.GetTileMap().IsPassable(5, 5) && !doc.GetTileMap().IsPassable(9, 9) &&
                                !doc.GetTileMap().IsPassable(0, 19);
    doc.FloodFill(15, 15, grass);
    doc.SetTile(5, 5, grass);
    doc.AddDecoration(1, 3, 3);
    doc.SetTile(3, 3, grass);
    doc.SetWall(12, 12, Faction::Resistance);
    doc.SetTile(12, 12, grass);
    if (!paintedBlocked || !doc.GetTileMap().IsPassable(5, 5) || !doc.GetTileMap().IsPassable(0, 19) ||
        doc.GetTileMap().IsPassable(3, 3) || doc.GetTileMap().IsPassable(12, 12)) {
        std::cerr << "[MapEditor-Test] FAILED: painting tiles did not update passability by sprite." << std::endl;
        return false;
    }
    doc.RemoveDecoration(3, 3);
    if (!doc.GetTileMap().IsPassable(3, 3)) {
        std::cerr << "[MapEditor-Test] FAILED: removing a decoration did not free the open tile." << std::endl;
        return false;
    }
    doc.SetTile(4, 4, earth);
    doc.AddDecoration(1, 4, 4);
    doc.RemoveDecoration(4, 4);
    if (doc.GetTileMap().IsPassable(4, 4)) {
        std::cerr << "[MapEditor-Test] FAILED: removing a decoration freed a blocked tile." << std::endl;
        return false;
    }
    std::cout << "  - Tile collision table matches " << mapsUsed << " map_* maps, loading and painting follow sprites OK" << std::endl;
    return true;
}

static bool RunEditorSelfTest() {
    namespace fs = std::filesystem;
    std::cout << "[MapEditor-Test] Starting automated Map Editor self-test..." << std::endl;

    OpenArtOfWar::MapDocument toolsDoc;
    toolsDoc.NewMap(16, 16, 0, 0);
    toolsDoc.SetTileBrush(5, 5, 42, 2);
    if (toolsDoc.GetTileMap().GetBaseTile(5, 5) != 42 ||
        toolsDoc.GetTileMap().GetBaseTile(6, 5) != 42 ||
        toolsDoc.GetTileMap().GetBaseTile(5, 6) != 42 ||
        toolsDoc.GetTileMap().GetBaseTile(6, 6) != 42 ||
        toolsDoc.GetTileMap().GetBaseTile(4, 5) == 42 ||
        toolsDoc.GetTileMap().GetBaseTile(5, 4) == 42 ||
        toolsDoc.GetTileMap().GetBaseTile(7, 7) == 42) {
        std::cerr << "[MapEditor-Test] FAILED: 2x2 brush did not paint exactly four tiles." << std::endl;
        return false;
    }
    const int firstUnitId = toolsDoc.AddEntity(0, false, OpenArtOfWar::Faction::Resistance, 2, 2);
    const int secondUnitId = toolsDoc.AddEntity(2, false, OpenArtOfWar::Faction::Resistance, 4, 2);
    if (firstUnitId <= 0 || secondUnitId <= 0 ||
        !toolsDoc.SelectEntityAt(2, 2, false) || !toolsDoc.SelectEntityAt(4, 2, true) ||
        toolsDoc.CountSelectedUnits() != 2 || toolsDoc.SetSelectedUnitFacing(5) != 2 ||
        toolsDoc.GetEntities()[0].facing != 5 || toolsDoc.GetEntities()[1].facing != 5) {
        std::cerr << "[MapEditor-Test] FAILED: Multi-selection or shared unit direction failed." << std::endl;
        return false;
    }
    toolsDoc.SetWall(8, 8, OpenArtOfWar::Faction::Resistance);
    if (toolsDoc.CanPlaceEntity(0, false, 8, 8) ||
        toolsDoc.AddEntity(0, false, OpenArtOfWar::Faction::Resistance, 8, 8) != -1 ||
        toolsDoc.CanPlaceEntity(8, true, 2, 2)) {
        std::cerr << "[MapEditor-Test] FAILED: Entity placement ignored blocked tiles or occupied footprints." << std::endl;
        return false;
    }
    toolsDoc.SetWall(9, 9, OpenArtOfWar::Faction::Confederation);
    if (toolsDoc.CanPlaceWallBrush(8, 8, OpenArtOfWar::Faction::Resistance, 2) ||
        toolsDoc.SetWallBrush(8, 8, OpenArtOfWar::Faction::Resistance, 2) ||
        toolsDoc.GetTileMap().GetWall(8, 9) != 0 ||
        !toolsDoc.SetWallBrush(12, 12, OpenArtOfWar::Faction::Resistance, 2) ||
        toolsDoc.GetTileMap().GetWall(12, 12) == 0 || toolsDoc.GetTileMap().GetWall(13, 13) == 0) {
        std::cerr << "[MapEditor-Test] FAILED: Wall brush collision or size handling failed." << std::endl;
        return false;
    }
    const int eraseUnitId = toolsDoc.AddEntity(0, false, OpenArtOfWar::Faction::Resistance, 9, 8);
    toolsDoc.AddDecoration(1, 8, 9);
    const uint8_t terrainBeforeErase = toolsDoc.GetTileMap().GetBaseTile(8, 9);
    if (eraseUnitId <= 0 || !toolsDoc.HasObjectsInBrush(8, 8, 2) ||
        !toolsDoc.RemoveObjectsBrush(8, 8, 2) ||
        toolsDoc.GetTileMap().IsWall(8, 8) || toolsDoc.GetTileMap().IsWall(9, 9) ||
        toolsDoc.GetTileMap().GetDecorGrid(8, 9) != 0 || toolsDoc.GetEntityAt(9, 8) ||
        toolsDoc.GetTileMap().GetBaseTile(8, 9) != terrainBeforeErase) {
        std::cerr << "[MapEditor-Test] FAILED: 2x2 erase brush did not remove objects while preserving terrain." << std::endl;
        return false;
    }
    std::cout << "  - Exact 2x2 brush and multi-unit selection/facing OK" << std::endl;

    // Verify the game's extended map format survives an editor save/load round trip.
    OpenArtOfWar::MapDocument gameMapDoc;
    const std::string gameMapPath = "assets/maps/testmap.json";
    if (!gameMapDoc.LoadMap(gameMapPath)) {
        std::cerr << "[MapEditor-Test] FAILED: Could not load " << gameMapPath << std::endl;
        return false;
    }
    std::string gameMapText;
    OpenArtOfWar::Assets::ReadText(gameMapPath, gameMapText);
    nlohmann::json originalGameMap = nlohmann::json::parse(gameMapText);
    const size_t expectedEntities = originalGameMap.value("buildings", nlohmann::json::array()).size() +
                                    originalGameMap.value("units", nlohmann::json::array()).size();
    const auto* playerHq = gameMapDoc.GetEntityAt(4, 17);
    const auto* enemyHq = gameMapDoc.GetEntityAt(46, 18);
    if (gameMapDoc.GetWidth() != 56 || gameMapDoc.GetHeight() != 44 ||
        gameMapDoc.GetEntities().size() != expectedEntities ||
        gameMapDoc.GetPlayerStartX() != 5 || gameMapDoc.GetPlayerStartY() != 18 ||
        gameMapDoc.GetEnemyStartX() != 46 || gameMapDoc.GetEnemyStartY() != 18 ||
        !playerHq || playerHq->type != 8 || !enemyHq || enemyHq->type != 0) {
        std::cerr << "[MapEditor-Test] FAILED: Game-format map data did not import correctly." << std::endl;
        return false;
    }

    const std::string roundTripPath = "assets/maps/testmap_editor_roundtrip.json";
    const std::string roundTripFilePath = OpenArtOfWar::Assets::WritablePath(roundTripPath);
    if (!gameMapDoc.SaveMap(roundTripFilePath)) {
        std::cerr << "[MapEditor-Test] FAILED: Could not save round-trip test map." << std::endl;
        return false;
    }
    std::string roundTripText;
    OpenArtOfWar::Assets::ReadText(roundTripPath, roundTripText);
    nlohmann::json roundTripData = nlohmann::json::parse(roundTripText);
    OpenArtOfWar::MapDocument reloadedGameMap;
    bool roundTripOk = reloadedGameMap.LoadMap(roundTripPath) &&
        reloadedGameMap.GetEntities().size() == expectedEntities &&
        reloadedGameMap.GetPlayerStartX() == gameMapDoc.GetPlayerStartX() &&
        reloadedGameMap.GetPlayerStartY() == gameMapDoc.GetPlayerStartY() &&
        reloadedGameMap.GetEnemyStartX() == gameMapDoc.GetEnemyStartX() &&
        reloadedGameMap.GetEnemyStartY() == gameMapDoc.GetEnemyStartY() &&
        roundTripData.value("missionName", "") == originalGameMap.value("missionName", "") &&
        roundTripData.value("briefing", "") == originalGameMap.value("briefing", "") &&
        roundTripData.value("startingCredits", -1) == originalGameMap.value("startingCredits", -2) &&
        roundTripData.value("fogOfWar", false) == originalGameMap.value("fogOfWar", false) &&
        roundTripData["headquarters"] == originalGameMap["headquarters"] &&
        roundTripData["buildings"].size() == originalGameMap["buildings"].size() &&
        roundTripData["units"].size() == originalGameMap["units"].size();
    for (int y = 0; roundTripOk && y < gameMapDoc.GetHeight(); ++y) {
        for (int x = 0; x < gameMapDoc.GetWidth(); ++x) {
            if (gameMapDoc.GetTileMap().GetWall(x, y) != reloadedGameMap.GetTileMap().GetWall(x, y)) {
                roundTripOk = false;
                break;
            }
        }
    }
    fs::remove(roundTripFilePath);
    if (!roundTripOk) {
        std::cerr << "[MapEditor-Test] FAILED: Game-format map data was lost during round trip." << std::endl;
        return false;
    }
    std::cout << "  - Game-format testmap entities, walls, metadata and HQ positions round-trip OK" << std::endl;

    // 1. Test MapDocument load existing game map
    OpenArtOfWar::MapDocument doc;
    std::string testMapPath = "assets/maps/m0.json";
    if (!doc.LoadMap(testMapPath)) {
        std::cerr << "[MapEditor-Test] FAILED: Could not load map from " << testMapPath << std::endl;
        return false;
    }
    if (doc.GetWidth() != 60 || doc.GetHeight() != 40) {
        std::cerr << "[MapEditor-Test] FAILED: Map dimensions mismatch. Expected 60x40, got "
                  << doc.GetWidth() << "x" << doc.GetHeight() << std::endl;
        return false;
    }
    std::cout << "  - Document loaded map: " << testMapPath << " (" << doc.GetWidth() << "x" << doc.GetHeight() << ") OK" << std::endl;

    // 2. Test Editing Operations & Undo / Redo
    doc.PushUndoSnapshot();
    doc.SetTile(5, 5, 42);
    doc.SetWall(10, 10, OpenArtOfWar::Faction::Resistance, 150);
    doc.AddDecoration(100, 15, 15);
    int entId = doc.AddEntity(0, false, OpenArtOfWar::Faction::Resistance, 20, 20, 2);

    if (doc.GetTileMap().GetBaseTile(5, 5) != 42 ||
        doc.GetTileMap().GetWall(10, 10) == 0 ||
        doc.GetEntityAt(20, 20) == nullptr) {
        std::cerr << "[MapEditor-Test] FAILED: Tool edit operations did not apply properly." << std::endl;
        return false;
    }
    std::cout << "  - Edit tools (Tile, Wall, Decoration, Unit Entity #" << entId << ") OK" << std::endl;

    // Test Undo
    if (!doc.Undo()) {
        std::cerr << "[MapEditor-Test] FAILED: Undo failed." << std::endl;
        return false;
    }
    if (doc.GetTileMap().GetBaseTile(5, 5) == 42 || doc.GetEntityAt(20, 20) != nullptr) {
        std::cerr << "[MapEditor-Test] FAILED: Undo did not revert state!" << std::endl;
        return false;
    }
    // Test Redo
    if (!doc.Redo() || doc.GetTileMap().GetBaseTile(5, 5) != 42) {
        std::cerr << "[MapEditor-Test] FAILED: Redo did not re-apply state!" << std::endl;
        return false;
    }
    std::cout << "  - Undo / Redo history stack OK" << std::endl;

    // 3. Test MapGenerator
    std::cout << "  - Testing MapGenerator (180-deg symmetry, original shares)..." << std::endl;
    OpenArtOfWar::GeneratorParams params;
    params.width = 64;
    params.height = 48;
    params.tilesetId = 0;
    params.symmetry = OpenArtOfWar::GeneratorSymmetry::Rotational180;
    params.seed = 4242;
    params.randomSeed = false;
    OpenArtOfWar::MapGenerator::OriginalDefaults(params, params.tilesetId);

    OpenArtOfWar::MapGenerator::Generate(doc, params);

    if (doc.GetWidth() != 64 || doc.GetHeight() != 48) {
        std::cerr << "[MapEditor-Test] FAILED: Generated map dimensions mismatch!" << std::endl;
        return false;
    }
    std::cout << "  - Map generated with " << doc.GetTileMap().GetDecorations().size() << " decorations OK" << std::endl;

    if (!RunTileCollisionTest()) return false;
    if (!RunMapGeneratorTest()) return false;

    // 4. Test Saving Map to assets/maps/ and verification with Game Engine TileMap
    std::string testOutMap = "assets/maps/test_generated_map.json";
    const std::string testOutFilePath = OpenArtOfWar::Assets::WritablePath(testOutMap);
    if (!doc.SaveMap(testOutFilePath)) {
        std::cerr << "[MapEditor-Test] FAILED: Could not save map to " << testOutMap << std::endl;
        return false;
    }
    std::cout << "  - Saved generated map to " << testOutMap << " OK" << std::endl;

    // Load with game engine TileMap directly
    OpenArtOfWar::TileMap engineMap;
    if (!engineMap.LoadMap(testOutMap)) {
        std::cerr << "[MapEditor-Test] FAILED: Game engine TileMap could not load editor-generated map!" << std::endl;
        return false;
    }
    if (engineMap.GetWidth() != 64 || engineMap.GetHeight() != 48) {
        std::cerr << "[MapEditor-Test] FAILED: Game engine TileMap loaded incorrect dimensions!" << std::endl;
        return false;
    }
    std::cout << "  - Game engine TileMap loaded generated map (" << engineMap.GetWidth() << "x"
              << engineMap.GetHeight() << ", decors=" << engineMap.GetDecorations().size() << ") successfully OK" << std::endl;

    // Clean up temporary test map
    fs::remove(testOutFilePath);
    std::cout << "[MapEditor-Test] ALL MAP EDITOR TESTS PASSED SUCCESSFULLY!" << std::endl;
    return true;
}

int main(int argc, char* argv[]) {
    OpenArtOfWar::Assets::Init();
    std::cout << "========================================================\n"
              << "Open Art Of War Map Editor — openaow-mapeditor\n"
              << "Native Map & Campaign Editor for Open Art Of War\n"
              << "========================================================\n" << std::endl;

    std::string initialMap = "";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: openaow-mapeditor [options] [path_to_map.json]\n"
                      << "Options:\n"
                      << "  -h, --help       Show this help message\n"
                      << "  --test           Run automated headless self-tests and exit\n"
                      << "  --test-collision Run only the tile collision self-test\n"
                      << "  --test-generator Run only the map generator self-test\n"
                      << "  --generate-map FILE [SEED] [TILESET]  Generate a map to FILE and exit\n"
                      << "  path_to_map.json Initial map file to open on startup\n"
                      << "\nHotkeys:\n"
                      << "  Ctrl+N           Create new map\n"
                      << "  Ctrl+O           Open map dialog\n"
                      << "  Ctrl+S           Save current map\n"
                      << "  Ctrl+Shift+S     Save map as...\n"
                      << "  Ctrl+Z / Ctrl+Y  Undo / Redo\n"
                      << "  Ctrl+G           Open the map generator\n"
                      << "  G                Toggle grid\n"
                      << "  C                Toggle passability/collision view\n"
                      << "  M                Toggle minimap\n"
                      << "  F                Center camera on map\n"
                      << "  WASD / Arrows    Pan camera\n"
                      << "  RMB / MMB Drag   Smooth camera pan\n"
                      << "  Mouse Wheel      Zoom in/out\n"
                      << std::endl;
            return 0;
        } else if (arg == "--generate-map" && i + 1 < argc) {
            OpenArtOfWar::GeneratorParams params;
            params.randomSeed = i + 2 >= argc;
            if (i + 2 < argc) params.seed = static_cast<uint32_t>(std::stoul(argv[i + 2]));
            if (i + 3 < argc) params.tilesetId = std::stoi(argv[i + 3]);
            OpenArtOfWar::MapGenerator::OriginalDefaults(params, params.tilesetId);
            params.width = 80;
            params.height = 60;
            OpenArtOfWar::MapDocument doc;
            OpenArtOfWar::MapGenerator::Generate(doc, params);
            return doc.SaveMap(argv[i + 1]) ? 0 : 1;
        } else if (arg == "--test-collision") {
            return RunTileCollisionTest() ? 0 : 1;
        } else if (arg == "--test-generator") {
            return RunMapGeneratorTest() ? 0 : 1;
        } else if (arg == "--test") {
            return RunEditorSelfTest() ? 0 : 1;
        } else if (!arg.empty() && arg[0] != '-') {
            initialMap = arg;
        }
    }

    OpenArtOfWar::MapEditorApp app;
    if (!app.Init(1280, 720, initialMap)) {
        std::cerr << "[MapEditor] Failed to initialize editor." << std::endl;
        return 1;
    }

    app.run();
    return 0;
}
