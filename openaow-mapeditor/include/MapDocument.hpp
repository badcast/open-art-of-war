#pragma once

#include "core/Config.hpp"
#include "game/TileMap.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar
{

class TextureManager;

struct EditorEntity
{
    int id {0};
    int type {0}; // Building type (0..16) or Unit archetype (0..18)
    bool isBuilding {false};
    bool selected {false};
    Faction faction {Faction::Resistance};
    int tileX {0};
    int tileY {0};
    int buildWidth {1};
    int buildHeight {1};
    int facing {0}; // 0..7
    std::string name;
    nlohmann::json mapData = nlohmann::json::object();
};

struct MapSnapshot
{
    int width {0};
    int height {0};
    int tilesetId {0};
    std::vector<std::uint8_t> baseTiles;
    std::vector<MapDecoration> decorations;
    std::vector<std::uint16_t> wallGrid;
    std::vector<std::int16_t> wallHp;
    std::vector<bool> passability;
    std::vector<EditorEntity> entities;
    int playerStartX {0};
    int playerStartY {0};
    int enemyStartX {0};
    int enemyStartY {0};
    std::vector<std::pair<int, int>> extraStarts;
};

class MapDocument
{
public:
    MapDocument();

    void NewMap(int width, int height, int tilesetId, std::uint8_t fillTile = 0);
    bool LoadMap(std::string_view path);
    bool SaveMap(std::string_view path);

    // Decoration footprints come from the sprite data; without it every decoration covers one tile
    void SetTextureManager(const TextureManager *textures)
    {
        m_textures = textures;
    }

    // Getters
    TileMap &GetTileMap()
    {
        return m_map;
    }
    const TileMap &GetTileMap() const
    {
        return m_map;
    }

    int GetWidth() const
    {
        return m_map.GetWidth();
    }
    int GetHeight() const
    {
        return m_map.GetHeight();
    }
    int GetTilesetId() const
    {
        return m_map.GetTilesetId();
    }
    const std::string &GetName() const
    {
        return m_name;
    }
    void SetName(std::string_view name)
    {
        m_name = std::string(name);
        m_modified = true;
    }
    // In-game title of the map ("title" key of the map file), empty when it has none
    std::string GetTitle() const
    {
        return m_mapData.is_object() ? m_mapData.value("title", std::string()) : std::string();
    }
    const std::string &GetFilePath() const
    {
        return m_filePath;
    }
    bool IsModified() const
    {
        return m_modified;
    }
    void SetModified(bool mod)
    {
        m_modified = mod;
    }

    int GetPlayerStartX() const
    {
        return m_playerStartX;
    }
    int GetPlayerStartY() const
    {
        return m_playerStartY;
    }
    void setPlayerStart(int x, int y)
    {
        m_playerStartX = x;
        m_playerStartY = y;
        m_modified = true;
    }

    int GetEnemyStartX() const
    {
        return m_enemyStartX;
    }
    int GetEnemyStartY() const
    {
        return m_enemyStartY;
    }
    void setEnemyStart(int x, int y)
    {
        m_enemyStartX = x;
        m_enemyStartY = y;
        m_modified = true;
    }

    // More start positions (players 3..8 of a network game): a click on a free tile adds one, a click on an existing
    // one takes it away. The map holds as many players as it has start positions (the two above plus these).
    static constexpr std::size_t kMaxExtraStarts = 6;
    enum class StartToggle
    {
        Added,
        Removed,
        Full
    };
    StartToggle ToggleExtraStart(int x, int y)
    {
        for (std::size_t i = 0; i < m_extraStarts.size(); ++i)
        {
            if (m_extraStarts[i].first == x && m_extraStarts[i].second == y)
            {
                m_extraStarts.erase(m_extraStarts.begin() + static_cast<std::ptrdiff_t>(i));
                m_modified = true;
                return StartToggle::Removed;
            }
        }
        if (m_extraStarts.size() >= kMaxExtraStarts)
            return StartToggle::Full;
        m_extraStarts.push_back({x, y});
        m_modified = true;
        return StartToggle::Added;
    }
    const std::vector<std::pair<int, int>> &GetExtraStarts() const
    {
        return m_extraStarts;
    }

    // Uniform Player Start Management (Players 1..8)
    int GetPlayerCount() const
    {
        return 2 + static_cast<int>(m_extraStarts.size());
    }
    std::pair<int, int> GetStartPos(int playerIndex) const;
    bool SetStartPos(int playerIndex, int x, int y);
    int FindStartPosAt(int x, int y) const;
    bool AddStartPos(int x, int y);
    bool RemoveStartPos(int playerIndex);
    bool MoveStartPos(int playerIndex, int newX, int newY)
    {
        return SetStartPos(playerIndex, newX, newY);
    }

    const std::vector<EditorEntity> &GetEntities() const
    {
        return m_entities;
    }
    std::vector<EditorEntity> &GetEntities()
    {
        return m_entities;
    }

    // Map Editing Tools
    void SetTile(int x, int y, std::uint8_t tile);
    void SetTileBrush(int x, int y, std::uint8_t tile, int brushSize = 1);
    void FloodFill(int startX, int startY, std::uint8_t newTile);
    void FillAll(std::uint8_t tile);

    void AddDecoration(int type, int x, int y);
    void RemoveDecoration(int x, int y);

    void SetWall(int x, int y, Faction faction, int hp = 150);
    bool CanPlaceWallBrush(int x, int y, Faction faction, int brushSize) const;
    bool SetWallBrush(int x, int y, Faction faction, int brushSize);
    void RemoveWall(int x, int y);

    bool CanPlaceEntity(int type, bool isBuilding, int x, int y) const;
    int AddEntity(int type, bool isBuilding, Faction faction, int x, int y, int facing = 0);
    bool HasObjectsInBrush(int x, int y, int brushSize) const;
    bool RemoveObjectsBrush(int x, int y, int brushSize);
    bool SelectEntityAt(int x, int y, bool additive);
    void ClearEntitySelection();
    int CountSelectedUnits() const;
    int SetSelectedUnitFacing(int facing);
    void RemoveEntityAt(int x, int y);
    const EditorEntity *GetEntityAt(int x, int y) const;
    EditorEntity *GetEntityAt(int x, int y);

    void SetPassable(int x, int y, bool passable);
    void TogglePassable(int x, int y);
    void AutoRecalculatePassability();

    // Undo / Redo
    void PushUndoSnapshot();
    bool Undo();
    bool Redo();
    bool CanUndo() const
    {
        return !m_undoStack.empty();
    }
    bool CanRedo() const
    {
        return !m_redoStack.empty();
    }

    void ClearAll();

private:
    void RefreshPassability(int x, int y);
    std::pair<int, int> DecorationSize(int type) const;
    bool CoveredByDecoration(int x, int y) const;
    MapSnapshot CreateSnapshot() const;
    void RestoreSnapshot(const MapSnapshot &snap);

    const TextureManager *m_textures {nullptr};
    TileMap m_map;
    std::string m_name {"untitled"};
    std::string m_filePath {""};
    nlohmann::json m_mapData {nlohmann::json::object()};
    bool m_modified {false};

    int m_playerStartX {6};
    int m_playerStartY {7};
    int m_enemyStartX {44};
    int m_enemyStartY {1};
    std::vector<std::pair<int, int>> m_extraStarts;

    int m_nextEntityId {1};
    std::vector<EditorEntity> m_entities;

    std::vector<MapSnapshot> m_undoStack;
    std::vector<MapSnapshot> m_redoStack;
    static constexpr std::size_t MAX_UNDO_LEVELS = 30;
};

} // namespace OpenArtOfWar
