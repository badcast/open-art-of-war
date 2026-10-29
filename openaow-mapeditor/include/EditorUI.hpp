#pragma once

#include "MapDocument.hpp"
#include "MapAutoGenerator.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/TextureManager.hpp"
#include <string>
#include <vector>

namespace OpenArtOfWar {

enum class EditorTool : int {
    TileBrush = 0,
    BucketFill = 1,
    PlaceDecor = 2,
    PlaceWall = 3,
    PlaceBuilding = 4,
    PlaceUnit = 5,
    SetPlayerStart = 6,
    SetEnemyStart = 7,
    PassabilityBrush = 8,
    Eraser = 9,
    Eyedropper = 10,
    Select = 11,
    AddStartPosition = 12
};

class EditorUI {
public:
    EditorUI();

    void init();
    void render(MapDocument& doc, Camera& camera, TextureManager& texManager, Renderer& renderer);

    EditorTool getActiveTool() const { return m_activeTool; }
    void setActiveTool(EditorTool tool) { m_activeTool = tool; }

    int getSelectedTile() const { return m_selectedTile; }
    int getBrushSize() const { return m_brushSize; }
    int getSelectedDecorType() const { return m_selectedDecorType; }
    Faction getSelectedWallFaction() const { return m_selectedWallFaction; }
    int getSelectedBuildingType() const { return m_selectedBuildingType; }
    Faction getSelectedBuildingFaction() const { return m_selectedBuildingFaction; }
    int getSelectedUnitArchetype() const { return m_selectedUnitArchetype; }
    Faction getSelectedUnitFaction() const { return m_selectedUnitFaction; }
    int getSelectedUnitFacing() const { return m_selectedUnitFacing; }

    bool isShowGrid() const { return m_showGrid; }
    bool isShowPassability() const { return m_showPassability; }
    bool isShowMiniMap() const { return m_showMiniMap; }

    void setHoveredTile(int tx, int ty) { m_hoverTileX = tx; m_hoverTileY = ty; }
    void selectTileFromEyedropper(int tile) { m_selectedTile = tile; m_activeTool = EditorTool::TileBrush; }
    void showToast(const std::string& message, float duration = 3.5f);

    // Dialog controls
    void openSaveAsDialog(const MapDocument& doc);
    void openOpenDialog();
    void openNewMapDialog();
    void openGeneratorDialog();
    void requestClose(const MapDocument& doc);
    bool shouldClose() const { return m_shouldClose; }

private:
    void renderMenuBar(MapDocument& doc, Camera& camera);
    void renderToolbar(MapDocument& doc, TextureManager& texManager);
    void renderPalettePanel(MapDocument& doc, TextureManager& texManager);
    void renderMiniMapWindow(MapDocument& doc, Camera& camera);
    void renderStatusBar(const MapDocument& doc, const Camera& camera);
    void renderToastNotification();

    // Modals
    void renderSaveAsModal(MapDocument& doc);
    void renderOpenModal(MapDocument& doc);
    void renderNewMapModal(MapDocument& doc);
    void renderGeneratorModal(MapDocument& doc);
    void renderCloseConfirmation(MapDocument& doc);

    void scanAvailableMaps();

    // Editor state
    EditorTool m_activeTool{EditorTool::TileBrush};
    int m_paletteTabToSelect{-1};
    int m_brushSize{1};
    int m_selectedTile{0};
    int m_selectedDecorType{0};

    Faction m_selectedWallFaction{Faction::Resistance};
    int m_selectedBuildingType{8};
    Faction m_selectedBuildingFaction{Faction::Resistance};

    int m_selectedUnitArchetype{0};
    Faction m_selectedUnitFaction{Faction::Resistance};
    int m_selectedUnitFacing{0};

    bool m_passabilityPaintMode{false}; // false = set blocked, true = set passable

    // View toggles
    bool m_showGrid{true};
    bool m_showPassability{false};
    bool m_showMiniMap{true};
    bool m_showDecors{true};
    bool m_showWalls{true};
    bool m_showEntities{true};

    int m_hoverTileX{-1};
    int m_hoverTileY{-1};

    // Toast notification
    std::string m_toastMessage;
    float m_toastTimer{0.0f};

    // Dialog states
    bool m_openSaveModalReq{false};
    bool m_openOpenModalReq{false};
    bool m_openNewModalReq{false};
    bool m_openGenModalReq{false};
    bool m_openCloseConfirmationReq{false};
    bool m_closeAfterSave{false};
    bool m_shouldClose{false};

    // Save As modal data
    char m_saveFilename[128]{"custom_map_01.json"};
    char m_saveMapName[128]{"Custom Skirmish"};

    // Open modal data
    struct MapFileInfo {
        std::string filename;
        std::string fullPath;
        int width{0};
        int height{0};
        int tilesetId{0};
        size_t fileSize{0};
    };
    std::vector<MapFileInfo> m_availableMaps;
    char m_openSearchFilter[64]{""};
    int m_selectedMapIndex{-1};

    // New Map modal data
    int m_newMapWidth{60};
    int m_newMapHeight{40};
    int m_newMapTileset{0};
    int m_newMapFillTile{0};

    // Generator modal data
    GeneratorParams m_genParams;
    int m_genMode{0};                   // 0 = classic generator, 1 = SmartGenerator
};

} // namespace OpenArtOfWar
