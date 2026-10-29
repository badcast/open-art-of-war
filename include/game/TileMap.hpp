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

        bool loadMap(const std::string &jsonPath);
        // Takes away the walls the map file placed (a game that starts from scratch)
        void clearWalls();
        // Takes the environment settings (fog look, construction rules); missing keys keep the defaults
        void loadEnvironment(const EnvironmentConfig &env);
        const FogStyle &getFogStyle() const
        {
            return m_fogStyle;
        }
        const ConstructionRules &getConstructionRules() const
        {
            return m_constructionRules;
        }
        void setFogStyle(const FogStyle &style)
        {
            m_fogStyle = style;
        }
        void initBlankArena(int width, int height, int tilesetId = 0);
        void render(Renderer &renderer, const Camera &camera, bool fogEnabled = true, int globalAnimTick = 0);
        void renderTerrain(Renderer &renderer, const Camera &camera, bool fogEnabled = true, int globalAnimTick = 0) const;
        void renderWallRubble(Renderer &renderer, const Camera &camera, bool fogEnabled = true) const;
        void renderDecorations(Renderer &renderer, const Camera &camera, bool fogEnabled = true, int globalAnimTick = 0) const;
        // `smooth` false (low graphics): one texel per tile, no blur, hard tile edges
        void renderFog(Renderer &renderer, const Camera &camera, bool smooth = true) const;
        void renderWalls(Renderer &renderer, const Camera &camera, bool fogEnabled = true, int globalAnimTick = 0) const;
        void renderWallRow(int ty, Renderer &renderer, const Camera &camera, bool fogEnabled = true, int globalAnimTick = 0, int startTileX = -1, int endTileX = -1) const;

        bool isPassable(int tileX, int tileY) const;
        void setPassable(int tileX, int tileY, bool passable);

        // `owner`: the player the wall belongs to (kNoPlayer + a faction: the first player of that faction)
        void setWall(int tileX, int tileY, Faction faction, int hpPercent = 100, PlayerId owner = kNoPlayer);
        PlayerId getWallOwner(int tileX, int tileY) const;
        uint16_t getWall(int tileX, int tileY) const;
        void damageWall(int tileX, int tileY, int damage);
        int getWallHp(int tileX, int tileY) const;
        int getWallMaxHp(int /*tileX*/, int /*tileY*/) const
        {
            return 150;
        }
        Faction getWallFaction(int tileX, int tileY) const;
        uint16_t getWallRubble(int tileX, int tileY) const;
        bool isWall(int tileX, int tileY) const
        {
            return getWall(tileX, tileY) > 0;
        }

        // Sight: every team (Players::teamOf, 0..7) has its own "seen this tick" bit per tile, worked out
        // the same way on every peer of a network game (the simulation asks isVisibleTo). The fog that is
        // drawn (getFogState: hidden / explored / visible) follows the local player's team, the viewer.
        void setViewerTeam(uint8_t team)
        {
            m_viewerTeam = team & 7;
        }
        uint8_t getViewerTeam() const
        {
            return m_viewerTeam;
        }
        // Start of a sight update: nothing seen; the viewer's visible tiles become explored
        void resetVisibility();
        void resetAllFog(FogState state = FogState::Hidden);
        void revealArea(int centerTileX, int centerTileY, int radius, uint8_t team);
        void revealArea(int centerTileX, int centerTileY, int radius)
        {
            revealArea(centerTileX, centerTileY, radius, m_viewerTeam);
        }
        // Exactly the tiles of a footprint (a unit's tile, a building's WxH)
        void revealRect(int tileX, int tileY, int width, int height, uint8_t team);
        void revealRect(int tileX, int tileY, int width, int height)
        {
            revealRect(tileX, tileY, width, height, m_viewerTeam);
        }
        bool isVisibleTo(int tileX, int tileY, uint8_t team) const;
        FogState getFogState(int tileX, int tileY) const;
        bool isFogEnabled() const
        {
            return m_hasFog;
        }
        void setFogEnabled(bool enabled)
        {
            m_hasFog = enabled;
        }

        int getWidth() const
        {
            return m_width;
        }
        int getHeight() const
        {
            return m_height;
        }
        int getTilesetId() const
        {
            return m_tilesetId;
        }

        uint8_t getBaseTile(int x, int y) const;
        void setBaseTile(int x, int y, uint8_t tile);
        void setSize(int width, int height);
        void setTilesetId(int tilesetId)
        {
            m_tilesetId = tilesetId;
        }
        const std::vector<MapDecoration> &getDecorations() const
        {
            return m_decorations;
        }
        void clearDecorations();
        void addDecoration(int type, int x, int y);
        void removeDecorationAt(int x, int y);
        uint16_t getDecorGrid(int x, int y) const;

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
