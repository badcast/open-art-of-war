#pragma once

#include "core/Config.hpp"
#include "game/Players.hpp"
#include "graphics/Renderer.hpp"
#include <string>
#include <vector>

namespace OpenArtOfWar
{

    class EnvironmentConfig;

    enum class FogState : uint8_t
    {
        Hidden = 0,   // Pitch black
        Explored = 1, // Visited earlier, currently out of sight (dimmed)
        Visible = 2   // Currently in sight (full brightness)
    };

    // Look of the fog of war (assets/data/game_environment.json, section "fog")
    struct FogStyle
    {
        float rounding {0.6f};   // 0 = square tile edges, 1 = edges softened over a whole tile
        int texelsPerTile {8};   // mask resolution; more texels = smoother curves
        int exploredAlpha {140}; // darkening of explored-but-unseen ground
        int hiddenAlpha {255};   // never seen
        SDL_Color color {8, 10, 14, 255};
    };

    // Construction rules (assets/data/game_environment.json, section "construction")
    struct ConstructionRules
    {
        int enemyBlockTiles {6}; // no building while a visible enemy is this many tiles or closer to the footprint
    };

    struct MapDecoration
    {
        int type {0};
        int x {0};
        int y {0};
        int width {1};
        int height {1};
    };

    class TileMap
    {
    public:
        TileMap();

        bool LoadMap(const std::string &jsonPath);
        // Takes away the walls the map file placed (a game that starts from scratch)
        void ClearWalls();
        // Takes the environment settings (fog look, construction rules); missing keys keep the defaults
        void LoadEnvironment(const EnvironmentConfig &env);
        const FogStyle &GetFogStyle() const
        {
            return m_fogStyle;
        }
        const ConstructionRules &GetConstructionRules() const
        {
            return m_constructionRules;
        }
        void SetFogStyle(const FogStyle &style)
        {
            m_fogStyle = style;
        }
        void InitBlankArena(int width, int height, int tilesetId = 0);
        void render(Renderer &renderer, const Camera &camera, bool fogEnabled = true, int globalAnimTick = 0);
        void RenderTerrain(Renderer &renderer, const Camera &camera, bool fogEnabled = true, int globalAnimTick = 0) const;
        void RenderWallRubble(Renderer &renderer, const Camera &camera, bool fogEnabled = true) const;
        void RenderDecorations(Renderer &renderer, const Camera &camera, bool fogEnabled = true, int globalAnimTick = 0) const;
        // `smooth` false (low graphics): one texel per tile, no blur, hard tile edges
        void RenderFog(Renderer &renderer, const Camera &camera, bool smooth = true) const;
        void RenderWalls(Renderer &renderer, const Camera &camera, bool fogEnabled = true, int globalAnimTick = 0) const;
        void RenderWallRow(int ty, Renderer &renderer, const Camera &camera, bool fogEnabled = true, int globalAnimTick = 0, int startTileX = -1, int endTileX = -1) const;

        bool IsPassable(int tileX, int tileY) const;
        void SetPassable(int tileX, int tileY, bool passable);

        // `owner`: the player the wall belongs to (kNoPlayer + a faction: the first player of that faction)
        void SetWall(int tileX, int tileY, Faction faction, int hpPercent = 100, PlayerId owner = kNoPlayer);
        PlayerId GetWallOwner(int tileX, int tileY) const;
        uint16_t GetWall(int tileX, int tileY) const;
        void DamageWall(int tileX, int tileY, int damage);
        // The wall is gone at once (rubble stays), whatever its armour
        void DestroyWall(int tileX, int tileY);
        int GetWallHp(int tileX, int tileY) const;
        int GetWallMaxHp(int /*tileX*/, int /*tileY*/) const
        {
            return 150;
        }
        Faction GetWallFaction(int tileX, int tileY) const;
        uint16_t GetWallRubble(int tileX, int tileY) const;
        bool IsWall(int tileX, int tileY) const
        {
            return GetWall(tileX, tileY) > 0;
        }

        // Sight: every team (Players::teamOf, 0..7) has its own "seen this tick" bit per tile, worked out
        // the same way on every peer of a network game (the simulation asks isVisibleTo). The fog that is
        // drawn (getFogState: hidden / explored / visible) follows the local player's team, the viewer.
        void SetViewerTeam(uint8_t team)
        {
            m_viewerTeam = team & 7;
        }
        uint8_t GetViewerTeam() const
        {
            return m_viewerTeam;
        }
        // Start of a sight update: nothing seen; the viewer's visible tiles become explored
        void ResetVisibility();
        void ResetAllFog(FogState state = FogState::Hidden);
        void RevealArea(int centerTileX, int centerTileY, int radius, uint8_t team);
        void RevealArea(int centerTileX, int centerTileY, int radius)
        {
            RevealArea(centerTileX, centerTileY, radius, m_viewerTeam);
        }
        // Exactly the tiles of a footprint (a unit's tile, a building's WxH)
        void RevealRect(int tileX, int tileY, int width, int height, uint8_t team);
        void RevealRect(int tileX, int tileY, int width, int height)
        {
            RevealRect(tileX, tileY, width, height, m_viewerTeam);
        }
        bool IsVisibleTo(int tileX, int tileY, uint8_t team) const;
        FogState GetFogState(int tileX, int tileY) const;
        bool IsFogEnabled() const
        {
            return m_hasFog;
        }
        void SetFogEnabled(bool enabled)
        {
            m_hasFog = enabled;
        }

        int GetWidth() const
        {
            return m_width;
        }
        int GetHeight() const
        {
            return m_height;
        }
        int GetTilesetId() const
        {
            return m_tilesetId;
        }

        uint8_t GetBaseTile(int x, int y) const;
        void SetBaseTile(int x, int y, uint8_t tile);
        void SetSize(int width, int height);
        void SetTilesetId(int tilesetId)
        {
            m_tilesetId = tilesetId;
        }
        const std::vector<MapDecoration> &GetDecorations() const
        {
            return m_decorations;
        }
        void ClearDecorations();
        void AddDecoration(int type, int x, int y);
        void RemoveDecorationAt(int x, int y);
        uint16_t GetDecorGrid(int x, int y) const;

    private:
        int m_width {60};
        int m_height {40};
        int m_tilesetId {0};

        uint8_t m_baseTiles[MAX_MAP_SIZE][MAX_MAP_SIZE] {{0}};
        uint16_t m_decorGrid[MAX_MAP_SIZE][MAX_MAP_SIZE] {{0}};
        uint16_t m_wallGrid[MAX_MAP_SIZE][MAX_MAP_SIZE] {{0}};       // 1001..1100 Conf wall, 1101..1200 Res wall (f.java:2049, 6823)
        uint16_t m_wallRubbleGrid[MAX_MAP_SIZE][MAX_MAP_SIZE] {{0}}; // Ground rubble/ruin remnant of destroyed wall
        int16_t m_wallHp[MAX_MAP_SIZE][MAX_MAP_SIZE] {{0}};          // Current wall health (0..150 HP)
        PlayerId m_wallOwner[MAX_MAP_SIZE][MAX_MAP_SIZE] {{0}};
        bool m_passable[MAX_MAP_SIZE][MAX_MAP_SIZE] {{true}};
        FogState m_fog[MAX_MAP_SIZE][MAX_MAP_SIZE] {{FogState::Hidden}};
        uint8_t m_sight[MAX_MAP_SIZE][MAX_MAP_SIZE] {{0}}; // bit per team: seen this tick
        uint8_t m_viewerTeam {0};
        bool m_hasFog {true};
        FogStyle m_fogStyle;
        ConstructionRules m_constructionRules;
        mutable std::vector<uint8_t> m_fogMask; // per-frame fog mask of the visible window
        mutable std::vector<uint8_t> m_fogScratch;

        std::vector<MapDecoration> m_decorations;
    };

} // namespace OpenArtOfWar
