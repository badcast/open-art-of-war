#pragma once

#include "MapDocument.hpp"
#include "MapGenerator.hpp"
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
    PlayerStarts = 6,
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

    void Init();
    void render(MapDocument& doc, Camera& camera, TextureManager& texManager, Renderer& renderer);

    EditorTool GetActiveTool() const { return m_activeTool; }
    void SetActiveTool(EditorTool tool) { m_activeTool = tool; }

    int GetSelectedTile() const { return m_selectedTile; }
    int GetBrushSize() const { return m_brushSize; }
    int GetSelectedDecorType() const { return m_selectedDecorType; }
    Faction GetSelectedWallFaction() const { return m_selectedWallFaction; }
    int GetSelectedBuildingType() const { return m_selectedBuildingType; }
    Faction GetSelectedBuildingFaction() const { return m_selectedBuildingFaction; }
    int GetSelectedUnitArchetype() const { return m_selectedUnitArchetype; }
    Faction GetSelectedUnitFaction() const { return m_selectedUnitFaction; }
    int GetSelectedUnitFacing() const { return m_selectedUnitFacing; }
    int GetSelectedPlayerSlot() const { return m_selectedPlayerSlot; }
    void SetSelectedPlayerSlot(int slot) { m_selectedPlayerSlot = slot; }

    bool IsShowGrid() const { return m_showGrid; }
    bool IsShowPassability() const { return m_showPassability; }
    bool IsShowMiniMap() const { return m_showMiniMap; }

    void SetHoveredTile(int tx, int ty) { m_hoverTileX = tx; m_hoverTileY = ty; }
    void SelectTileFromEyedropper(int tile) { m_selectedTile = tile; m_activeTool = EditorTool::TileBrush; }
    void ShowToast(const std::string& message, float duration = 3.5f);

    void OpenContextMenu(int tx, int ty) {
        m_contextMenuTileX = tx;
        m_contextMenuTileY = ty;
        m_openContextMenuReq = true;
    }

    // Dialog controls
    void OpenSaveAsDialog(const MapDocument& doc);
    void OpenOpenDialog();
    void OpenNewMapDialog();
    void OpenGeneratorDialog();
    void RequestClose(const MapDocument& doc);
    bool ShouldClose() const { return m_shouldClose; }

private:
    void RenderMenuBar(MapDocument& doc, Camera& camera);
    void RenderToolbar(MapDocument& doc, TextureManager& texManager);
    void RenderPalettePanel(MapDocument& doc, TextureManager& texManager, Camera& camera);
    void RenderMiniMapWindow(MapDocument& doc, Camera& camera);
    void RenderStatusBar(const MapDocument& doc, const Camera& camera);
    void RenderToastNotification();

    // Modals
    void RenderSaveAsModal(MapDocument& doc);
    void RenderOpenModal(MapDocument& doc);
    void RenderNewMapModal(MapDocument& doc);
    void RenderGeneratorModal(MapDocument& doc);
    void RenderCloseConfirmation(MapDocument& doc);
    void RenderGeneratorConfirmation(MapDocument& doc);
    void RenderContextMenu(MapDocument& doc, Camera& camera);

    void RequestGenerate(MapDocument& doc, bool useCustomParams = false);

    void ScanAvailableMaps();

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
    int m_selectedPlayerSlot{0};

    int m_contextMenuTileX{-1};
    int m_contextMenuTileY{-1};
    bool m_openContextMenuReq{false};

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
    bool m_openGenConfirmationReq{false};
    bool m_pendingGenWithCustomParams{false};
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
};

} // namespace OpenArtOfWar
