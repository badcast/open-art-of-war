#pragma once

#include "core/Config.hpp"
#include "game/TileMap.hpp"
#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar {

class TextureManager;

struct EditorEntity {
    int id{0};
    int type{0};            // Building type (0..16) or Unit archetype (0..18)
    bool isBuilding{false};
    bool selected{false};
    Faction faction{Faction::Resistance};
    int tileX{0};
    int tileY{0};
    int buildWidth{1};
    int buildHeight{1};
    int facing{0};          // 0..7
    std::string name;
    nlohmann::json mapData = nlohmann::json::object();
};

struct MapSnapshot {
    int width{0};
    int height{0};
    int tilesetId{0};
    std::vector<uint8_t> baseTiles;
    std::vector<MapDecoration> decorations;
    std::vector<uint16_t> wallGrid;
    std::vector<int16_t> wallHp;
    std::vector<bool> passability;
    std::vector<EditorEntity> entities;
    int playerStartX{0};
    int playerStartY{0};
    int enemyStartX{0};
    int enemyStartY{0};
    std::vector<std::pair<int, int>> extraStarts;
};

class MapDocument {
public:
    MapDocument();

    void newMap(int width, int height, int tilesetId, uint8_t fillTile = 0);
    bool loadMap(const std::string& path);
    bool saveMap(const std::string& path);

    // Decoration footprints come from the sprite data; without it every decoration covers one tile
    void setTextureManager(const TextureManager* textures) { m_textures = textures; }

    // Getters
    TileMap& getTileMap() { return m_map; }
    const TileMap& getTileMap() const { return m_map; }

    int getWidth() const { return m_map.getWidth(); }
    int getHeight() const { return m_map.getHeight(); }
    int getTilesetId() const { return m_map.getTilesetId(); }
    const std::string& getName() const { return m_name; }
    void setName(const std::string& name) { m_name = name; m_modified = true; }
    // In-game title of the map ("title" key of the map file), empty when it has none
    std::string getTitle() const { return m_mapData.is_object() ? m_mapData.value("title", std::string()) : std::string(); }
    const std::string& getFilePath() const { return m_filePath; }
    bool isModified() const { return m_modified; }
    void setModified(bool mod) { m_modified = mod; }

    int getPlayerStartX() const { return m_playerStartX; }
    int getPlayerStartY() const { return m_playerStartY; }
    void setPlayerStart(int x, int y) { m_playerStartX = x; m_playerStartY = y; m_modified = true; }

    int getEnemyStartX() const { return m_enemyStartX; }
    int getEnemyStartY() const { return m_enemyStartY; }
    void setEnemyStart(int x, int y) { m_enemyStartX = x; m_enemyStartY = y; m_modified = true; }

    // More start positions (players 3..8 of a network game): a click on a free tile adds one, a click on an existing
    // one takes it away. The map holds as many players as it has start positions (the two above plus these).
    static constexpr size_t kMaxExtraStarts = 6;
    enum class StartToggle { Added, Removed, Full };
    StartToggle toggleExtraStart(int x, int y) {
        for (size_t i = 0; i < m_extraStarts.size(); ++i) {
            if (m_extraStarts[i].first == x && m_extraStarts[i].second == y) {
                m_extraStarts.erase(m_extraStarts.begin() + static_cast<std::ptrdiff_t>(i));
                m_modified = true;
                return StartToggle::Removed;
            }
        }
        if (m_extraStarts.size() >= kMaxExtraStarts) return StartToggle::Full;
        m_extraStarts.push_back({x, y});
        m_modified = true;
        return StartToggle::Added;
    }
    const std::vector<std::pair<int, int>>& getExtraStarts() const { return m_extraStarts; }

    const std::vector<EditorEntity>& getEntities() const { return m_entities; }
    std::vector<EditorEntity>& getEntities() { return m_entities; }

    // Map Editing Tools
    void setTile(int x, int y, uint8_t tile);
    void setTileBrush(int x, int y, uint8_t tile, int brushSize = 1);
    void floodFill(int startX, int startY, uint8_t newTile);
    void fillAll(uint8_t tile);

    void addDecoration(int type, int x, int y);
    void removeDecoration(int x, int y);

    void setWall(int x, int y, Faction faction, int hp = 150);
    bool canPlaceWallBrush(int x, int y, Faction faction, int brushSize) const;
    bool setWallBrush(int x, int y, Faction faction, int brushSize);
    void removeWall(int x, int y);

    bool canPlaceEntity(int type, bool isBuilding, int x, int y) const;
    int addEntity(int type, bool isBuilding, Faction faction, int x, int y, int facing = 0);
    bool hasObjectsInBrush(int x, int y, int brushSize) const;
    bool removeObjectsBrush(int x, int y, int brushSize);
    bool selectEntityAt(int x, int y, bool additive);
    void clearEntitySelection();
    int countSelectedUnits() const;
    int setSelectedUnitFacing(int facing);
    void removeEntityAt(int x, int y);
    const EditorEntity* getEntityAt(int x, int y) const;
    EditorEntity* getEntityAt(int x, int y);

    void setPassable(int x, int y, bool passable);
    void togglePassable(int x, int y);
    void autoRecalculatePassability();

    // Undo / Redo
    void pushUndoSnapshot();
    bool undo();
    bool redo();
    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }

    void clearAll();

private:
    void refreshPassability(int x, int y);
    std::pair<int, int> decorationSize(int type) const;
    bool coveredByDecoration(int x, int y) const;
    MapSnapshot createSnapshot() const;
    void restoreSnapshot(const MapSnapshot& snap);

    const TextureManager* m_textures{nullptr};
    TileMap m_map;
    std::string m_name{"untitled"};
    std::string m_filePath{""};
    nlohmann::json m_mapData{nlohmann::json::object()};
    bool m_modified{false};

    int m_playerStartX{6};
    int m_playerStartY{7};
    int m_enemyStartX{44};
    int m_enemyStartY{1};
    std::vector<std::pair<int, int>> m_extraStarts;

    int m_nextEntityId{1};
    std::vector<EditorEntity> m_entities;

    std::vector<MapSnapshot> m_undoStack;
    std::vector<MapSnapshot> m_redoStack;
    static constexpr size_t MAX_UNDO_LEVELS = 30;
};

} // namespace OpenArtOfWar
