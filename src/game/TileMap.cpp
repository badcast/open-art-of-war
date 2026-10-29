#include "game/TileMap.hpp"
#include "game/TileCollision.hpp"
#include "game/Research.hpp"
#include "core/AssetStore.hpp"
#include "core/EnvironmentConfig.hpp"
#include "core/Utility.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <cmath>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar
{

    TileMap::TileMap()
    {
        for(int y = 0; y < MAX_MAP_SIZE; ++y)
        {
            for(int x = 0; x < MAX_MAP_SIZE; ++x)
            {
                m_passable[y][x] = true;
                m_fog[y][x] = FogState::Hidden;
            }
        }
    }

    bool TileMap::loadMap(const std::string &jsonPath)
    {
        std::string text;
        if(!Assets::readText(jsonPath, text))
        {
            std::cerr << "[TileMap] Failed to open map file: " << jsonPath << std::endl;
            return false;
        }

        nlohmann::json j = nlohmann::json::parse(text);

        m_width = std::clamp(j["width"].get<int>(), 1, MAX_MAP_SIZE);
        m_height = std::clamp(j["height"].get<int>(), 1, MAX_MAP_SIZE);
        m_tilesetId = j["tilesetId"].get<int>();

        for(int y = 0; y < MAX_MAP_SIZE; ++y)
        {
            for(int x = 0; x < MAX_MAP_SIZE; ++x)
            {
                m_baseTiles[y][x] = 0;
                m_decorGrid[y][x] = 0;
                m_wallGrid[y][x] = 0;
                m_wallRubbleGrid[y][x] = 0;
                m_wallHp[y][x] = 0;
                m_passable[y][x] = true;
            }
        }

        // Authentic terrain variation ranges e[0..5] per tileset (f.java:2191, mission_list.json)
        const int eRanges0[5][2] = {{0, 10}, {10, 18}, {18, 19}, {19, 26}, {26, 34}};
        const int eRanges1[5][2] = {{0, 10}, {10, 18}, {18, 26}, {26, 33}, {33, 36}};
        const auto &eRanges = (m_tilesetId == 1) ? eRanges1 : eRanges0;

        // 1. Unpack base terrain tiles from RLE stream
        int curX = 0;
        int curY = 0;
        for(const auto &span : j["terrainRle"])
        {
            int runLen = span["runLength"].get<int>();
            int tileType = span["tileType"].get<int>();
            for(int i = 0; i < runLen; ++i)
            {
                if(curY < m_height && curX < m_width)
                {
                    int finalTile = tileType;
                    for(int r = 0; r < 5; ++r)
                    {
                        if(tileType >= eRanges[r][0] && tileType < eRanges[r][1])
                        {
                            int spanCount = eRanges[r][1] - eRanges[r][0];
                            if(spanCount > 1)
                            {
                                uint32_t hash = (static_cast<uint32_t>(curX) * 73856093u ^ static_cast<uint32_t>(curY) * 19349663u);
                                int varIdx = static_cast<int>(hash % static_cast<uint32_t>(spanCount));
                                finalTile = eRanges[r][0] + varIdx;
                            }
                            break;
                        }
                    }
                    m_baseTiles[curY][curX] = static_cast<uint8_t>(finalTile);
                    if(TileCollision::isBlocked(m_tilesetId, finalTile))
                    {
                        m_passable[curY][curX] = false; // The sprite of this tile blocks movement
                    }
                }
                curX++;
                if(curX >= m_width)
                {
                    curX = 0;
                    curY++;
                }
            }
        }

        // 2. Unpack decorations (trees, bridges, cliffs, landmarks)
        m_decorations.clear();
        for(const auto &d : j["decorations"])
        {
            MapDecoration dec;
            dec.type = d["type"].get<int>();
            dec.x = d["x"].get<int>();
            dec.y = d["y"].get<int>();
            m_decorations.push_back(dec);
            if(dec.y >= 0 && dec.y < m_height && dec.x >= 0 && dec.x < m_width)
            {
                m_decorGrid[dec.y][dec.x] = static_cast<uint16_t>(dec.type + 1);
                m_passable[dec.y][dec.x] = false;
            }
        }

        // 3. Apply passability collision bitmask AFTER decorations (f.java:2222-2231)
        // Where bit == 0, tile becomes passable (true); where bit == 1, blocked (false).
        // A tile whose sprite blocks (TileCollision) stays blocked whatever the mask says.
        const auto &collisionMask = j["collisionMask"];
        for(int y = 0; y < m_height && y < static_cast<int>(collisionMask.size()); ++y)
        {
            const auto &rowChunks = collisionMask[y];
            int chunkIdx = 0;
            for(const auto &chunkVal : rowChunks)
            {
                uint32_t mask = chunkVal.get<uint32_t>();
                for(int bit = 0; bit < 32; ++bit)
                {
                    int x = chunkIdx * 32 + bit;
                    if(x < m_width)
                    {
                        bool blocked = (mask & (1u << (31 - bit))) != 0 || TileCollision::isBlocked(m_tilesetId, m_baseTiles[y][x]);
                        m_passable[y][x] = !blocked;
                    }
                }
                chunkIdx++;
            }
        }

        // 3b. Unpack preplaced defensive walls from map JSON if present
        if(j.contains("walls"))
        {
            for(const auto &w : j["walls"])
            {
                if(w.is_object())
                {
                    int wx = w.value("x", 0);
                    int wy = w.value("y", 0);
                    int ww = w.value("w", 1);
                    int wh = w.value("h", 1);
                    int hp = w.value("hp", 100);
                    Faction fac = Faction::Confederation;
                    if(w.contains("faction"))
                    {
                        if(w["faction"].is_string())
                        {
                            std::string fstr = w["faction"].get<std::string>();
                            fac = (fstr == "1") ? Faction::Resistance : Utility::parseFaction(fstr, Faction::Confederation);
                        }
                        else if(w["faction"].is_number_integer())
                        {
                            if(w["faction"].get<int>() != 0)
                                fac = Faction::Resistance;
                        }
                    }
                    for(int ty = wy; ty < wy + wh; ++ty)
                    {
                        for(int tx = wx; tx < wx + ww; ++tx)
                        {
                            setWall(tx, ty, fac, hp);
                        }
                    }
                }
                else if(w.is_array())
                {
                    if(w.size() >= 4)
                    {
                        int wx = w[0].get<int>();
                        int wy = w[1].get<int>();
                        int ww = w[2].get<int>();
                        int wh = w[3].get<int>();
                        Faction fac = (w.size() >= 5 && w[4].get<int>() != 0) ? Faction::Resistance : Faction::Confederation;
                        int hp = (w.size() >= 6) ? w[5].get<int>() : 100;
                        for(int ty = wy; ty < wy + wh; ++ty)
                        {
                            for(int tx = wx; tx < wx + ww; ++tx)
                            {
                                setWall(tx, ty, fac, hp);
                            }
                        }
                    }
                    else if(w.size() >= 2)
                    {
                        setWall(w[0].get<int>(), w[1].get<int>(), Faction::Confederation, 100);
                    }
                }
            }
        }
        if(j.contains("wallZones"))
        {
            for(const auto &wz : j["wallZones"])
            {
                if(wz.is_array())
                {
                    for(const auto &rect : wz)
                    {
                        if(rect.is_array() && rect.size() >= 4)
                        {
                            int wx = rect[0].get<int>();
                            int wy = rect[1].get<int>();
                            int ww = rect[2].get<int>();
                            int wh = rect[3].get<int>();
                            for(int ty = wy; ty < wy + wh; ++ty)
                            {
                                for(int tx = wx; tx < wx + ww; ++tx)
                                {
                                    setWall(tx, ty, Faction::Confederation, 100);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Check if map explicitly enables or disables fog of war
        if(j.contains("fogOfWar"))
        {
            m_hasFog = j["fogOfWar"].get<bool>();
        }
        else if(j.contains("hasFog"))
        {
            m_hasFog = j["hasFog"].get<bool>();
        }
        else if(j.contains("fog"))
        {
            m_hasFog = j["fog"].get<bool>();
        }
        else
        {
            m_hasFog = true;
        }

        // Reset Fog of War
        for(int y = 0; y < m_height; ++y)
        {
            for(int x = 0; x < m_width; ++x)
            {
                m_fog[y][x] = m_hasFog ? FogState::Hidden : FogState::Visible;
            }
        }

        std::cout << "[TileMap] Loaded map: " << jsonPath << " (" << m_width << "x" << m_height << ", tileset=" << m_tilesetId << ", decors=" << m_decorations.size() << ", fog=" << (m_hasFog ? "ON" : "OFF") << ")" << std::endl;
        return true;
    }

    void TileMap::initBlankArena(int width, int height, int tilesetId)
    {
        m_width = std::clamp(width, 16, MAX_MAP_SIZE);
        m_height = std::clamp(height, 16, MAX_MAP_SIZE);
        m_tilesetId = tilesetId;
        m_hasFog = false;
        m_decorations.clear();

        for(int y = 0; y < MAX_MAP_SIZE; ++y)
        {
            for(int x = 0; x < MAX_MAP_SIZE; ++x)
            {
                uint32_t hash = (static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u);
                m_baseTiles[y][x] = static_cast<uint8_t>(hash % 10);
                m_decorGrid[y][x] = 0;
                m_wallGrid[y][x] = 0;
                m_wallRubbleGrid[y][x] = 0;
                m_wallHp[y][x] = 0;
                m_passable[y][x] = true;
                m_fog[y][x] = FogState::Visible;
            }
        }
        std::cout << "[TileMap] Initialized Blank Test Arena (" << m_width << "x" << m_height << ")" << std::endl;
    }

    bool TileMap::isPassable(int tileX, int tileY) const
    {
        if(tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
        {
            return false;
        }
        return m_passable[tileY][tileX];
    }

    void TileMap::setPassable(int tileX, int tileY, bool passable)
    {
        if(tileX >= 0 && tileX < m_width && tileY >= 0 && tileY < m_height)
        {
            m_passable[tileY][tileX] = passable;
        }
    }

    void TileMap::clearWalls()
    {
        for(int y = 0; y < m_height; ++y)
        {
            for(int x = 0; x < m_width; ++x)
            {
                if(m_wallGrid[y][x] == 0)
                    continue;
                m_wallGrid[y][x] = 0;
                m_wallHp[y][x] = 0;
                m_passable[y][x] = true;
            }
        }
    }

    void TileMap::setWall(int tileX, int tileY, Faction faction, int hpPercent, PlayerId owner)
    {
        if(tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
            return;
        if(hpPercent <= 0)
        {
            m_wallGrid[tileY][tileX] = 0;
            m_wallHp[tileY][tileX] = 0;
            m_passable[tileY][tileX] = true;
            return;
        }
        int damageTier = std::clamp((100 - hpPercent) / 20, 0, 4); // 0=100%, 1=80%, 2=60%, 3=40%, 4=20%
        int base = (faction == Faction::Resistance) ? 1101 : 1001;
        m_wallGrid[tileY][tileX] = static_cast<uint16_t>(base + damageTier * 20);
        m_wallHp[tileY][tileX] = static_cast<int16_t>((150 * std::clamp(hpPercent, 1, 100)) / 100);
        m_wallOwner[tileY][tileX] = owner == kNoPlayer ? Players::ownerOf(faction) : owner;
        m_wallRubbleGrid[tileY][tileX] = 0; // Clear any old ruins when a new wall is erected!
        m_passable[tileY][tileX] = false;
    }

    uint16_t TileMap::getWall(int tileX, int tileY) const
    {
        if(tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
            return 0;
        return m_wallGrid[tileY][tileX];
    }

    int TileMap::getWallHp(int tileX, int tileY) const
    {
        if(tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
            return 0;
        return m_wallHp[tileY][tileX];
    }

    PlayerId TileMap::getWallOwner(int tileX, int tileY) const
    {
        if(tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height || m_wallGrid[tileY][tileX] == 0)
            return kNoPlayer;
        return m_wallOwner[tileY][tileX];
    }

    Faction TileMap::getWallFaction(int tileX, int tileY) const
    {
        if(tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
            return Faction::Neutral;
        uint16_t w = m_wallGrid[tileY][tileX];
        if(w >= 1101)
            return Faction::Resistance;
        if(w >= 1001)
            return Faction::Confederation;
        return Faction::Neutral;
    }

    uint16_t TileMap::getWallRubble(int tileX, int tileY) const
    {
        if(tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
            return 0;
        return m_wallRubbleGrid[tileY][tileX];
    }

    void TileMap::damageWall(int tileX, int tileY, int damage)
    {
        if(tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
            return;
        if(m_wallGrid[tileY][tileX] == 0)
            return;

        if(m_wallHp[tileY][tileX] <= 0)
        {
            m_wallHp[tileY][tileX] = 150;
        }

        damage = reduceWallDamage(damage, m_wallOwner[tileY][tileX]);
        m_wallHp[tileY][tileX] -= damage;
        if(m_wallHp[tileY][tileX] <= 0)
        {
            m_wallHp[tileY][tileX] = 0;
            // Leave behind authentic compact wall rubble remnant (composite 1778, 31x22 px)
            m_wallRubbleGrid[tileY][tileX] = 1778;
            m_wallGrid[tileY][tileX] = 0;
            m_passable[tileY][tileX] = true;
        }
        else
        {
            uint16_t base = (m_wallGrid[tileY][tileX] >= 1101) ? 1101 : 1001;
            float hpRatio = static_cast<float>(m_wallHp[tileY][tileX]) / 150.0f;
            int damageTier = std::clamp(static_cast<int>((1.0f - hpRatio) * 5.0f), 0, 4);
            m_wallGrid[tileY][tileX] = static_cast<uint16_t>(base + damageTier * 20);
        }
    }

    namespace
    {
        inline uint8_t teamBit(uint8_t team)
        {
            return static_cast<uint8_t>(1u << (team & 7));
        }
    } // namespace

    void TileMap::resetVisibility()
    {
        for(int y = 0; y < m_height; ++y)
        {
            for(int x = 0; x < m_width; ++x)
            {
                m_sight[y][x] = 0;
                if(m_fog[y][x] == FogState::Visible)
                {
                    m_fog[y][x] = FogState::Explored;
                }
            }
        }
    }

    bool TileMap::isVisibleTo(int tileX, int tileY, uint8_t team) const
    {
        if(!m_hasFog)
            return true;
        if(tileX < 0 || tileY < 0 || tileX >= m_width || tileY >= m_height)
            return false;
        return (m_sight[tileY][tileX] & teamBit(team)) != 0;
    }

    void TileMap::resetAllFog(FogState state)
    {
        for(int y = 0; y < m_height; ++y)
        {
            for(int x = 0; x < m_width; ++x)
            {
                m_fog[y][x] = state;
                m_sight[y][x] = state == FogState::Visible ? 0xFF : 0;
            }
        }
    }

    void TileMap::revealArea(int centerTileX, int centerTileY, int radius, uint8_t team)
    {
        const uint8_t bit = teamBit(team);
        const bool viewer = (team & 7) == m_viewerTeam;
        int r2 = radius * radius;
        int minX = std::max(0, centerTileX - radius);
        int maxX = std::min(m_width - 1, centerTileX + radius);
        int minY = std::max(0, centerTileY - radius);
        int maxY = std::min(m_height - 1, centerTileY + radius);

        for(int y = minY; y <= maxY; ++y)
        {
            int dy = y - centerTileY;
            for(int x = minX; x <= maxX; ++x)
            {
                int dx = x - centerTileX;
                if(dx * dx + dy * dy <= r2)
                {
                    m_sight[y][x] |= bit;
                    if(viewer)
                        m_fog[y][x] = FogState::Visible;
                }
            }
        }
    }

    void TileMap::revealRect(int tileX, int tileY, int width, int height, uint8_t team)
    {
        const uint8_t bit = teamBit(team);
        const bool viewer = (team & 7) == m_viewerTeam;
        for(int y = std::max(0, tileY); y < std::min(m_height, tileY + height); ++y)
        {
            for(int x = std::max(0, tileX); x < std::min(m_width, tileX + width); ++x)
            {
                m_sight[y][x] |= bit;
                if(viewer)
                    m_fog[y][x] = FogState::Visible;
            }
        }
    }

    FogState TileMap::getFogState(int tileX, int tileY) const
    {
        if(!m_hasFog)
        {
            return FogState::Visible;
        }
        if(tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
        {
            return FogState::Hidden;
        }
        return m_fog[tileY][tileX];
    }

    void TileMap::render(Renderer &renderer, const Camera &camera, bool fogEnabled, int globalAnimTick)
    {
        bool useFog = fogEnabled && m_hasFog;
        renderTerrain(renderer, camera, useFog, globalAnimTick);
        renderWallRubble(renderer, camera, useFog);
        renderDecorations(renderer, camera, useFog, globalAnimTick);
        if(useFog)
        {
            renderFog(renderer, camera);
        }
    }

    void TileMap::renderTerrain(Renderer &renderer, const Camera &camera, bool fogEnabled, int globalAnimTick) const
    {
        float startWorldX, startWorldY, endWorldX, endWorldY;
        camera.screenToWorld(0, 0, startWorldX, startWorldY);
        camera.screenToWorld(camera.getViewportWidth(), camera.getViewportHeight(), endWorldX, endWorldY);

        int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, m_width - 1);
        int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, m_width);
        int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, m_height - 1);
        int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, m_height);

        for(int ty = startTileY; ty < endTileY; ++ty)
        {
            for(int tx = startTileX; tx < endTileX; ++tx)
            {
                FogState fog = m_fog[ty][tx];
                if(fogEnabled && m_hasFog && fog == FogState::Hidden)
                {
                    // Hidden ground next to seen ground is drawn: the soft fog edge reaches into it
                    bool border = false;
                    for(int dy = -1; dy <= 1 && !border; ++dy)
                    {
                        for(int dx = -1; dx <= 1 && !border; ++dx)
                        {
                            const int nx = tx + dx, ny = ty + dy;
                            border = nx >= 0 && ny >= 0 && nx < m_width && ny < m_height && m_fog[ny][nx] != FogState::Hidden;
                        }
                    }
                    if(!border)
                    {
                        renderer.fillRectWorld(tx * TILE_WIDTH, ty * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, camera, m_fogStyle.color);
                        continue;
                    }
                }

                int tileIdx = m_baseTiles[ty][tx];
                renderer.drawTileWorld(m_tilesetId, tileIdx, tx * TILE_WIDTH, ty * TILE_HEIGHT, camera, globalAnimTick);
            }
        }
    }

    void TileMap::renderWallRubble(Renderer &renderer, const Camera &camera, bool fogEnabled) const
    {
        float startWorldX, startWorldY, endWorldX, endWorldY;
        camera.screenToWorld(0, 0, startWorldX, startWorldY);
        camera.screenToWorld(camera.getViewportWidth(), camera.getViewportHeight(), endWorldX, endWorldY);

        int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, m_width - 1);
        int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, m_width);
        int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, m_height - 1);
        int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, m_height);

        for(int ty = startTileY; ty < endTileY; ++ty)
        {
            for(int tx = startTileX; tx < endTileX; ++tx)
            {
                if(fogEnabled && m_hasFog && m_fog[ty][tx] == FogState::Hidden)
                    continue;
                uint16_t rubble = m_wallRubbleGrid[ty][tx];
                if(rubble > 0)
                {
                    renderer.drawCompositeWorld(rubble, tx * TILE_WIDTH - 90.0f, ty * TILE_HEIGHT - 60.0f, camera, Faction::Neutral);
                }
            }
        }
    }

    void TileMap::renderDecorations(Renderer &renderer, const Camera &camera, bool fogEnabled, int globalAnimTick) const
    {
        float startWorldX, startWorldY, endWorldX, endWorldY;
        camera.screenToWorld(0, 0, startWorldX, startWorldY);
        camera.screenToWorld(camera.getViewportWidth(), camera.getViewportHeight(), endWorldX, endWorldY);

        int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, m_width - 1);
        int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, m_width);
        int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, m_height - 1);
        int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, m_height);

        for(const auto &dec : m_decorations)
        {
            if(dec.x < startTileX - 4 || dec.x > endTileX + 4 || dec.y < startTileY - 4 || dec.y > endTileY + 4)
            {
                continue;
            }

            if(fogEnabled && m_hasFog && m_fog[dec.y][dec.x] == FogState::Hidden)
            {
                continue;
            }

            renderer.drawDecorWorld(m_tilesetId, dec.type, dec.x * TILE_WIDTH, dec.y * TILE_HEIGHT, camera, globalAnimTick);
        }
    }

    void TileMap::loadEnvironment(const EnvironmentConfig &env)
    {
        const nlohmann::json &j = env.root();
        if(j.contains("construction") && j["construction"].is_object())
        {
            const auto &c = j["construction"];
            m_constructionRules.enemyBlockTiles = std::clamp(c.value("enemy_block_tiles", m_constructionRules.enemyBlockTiles), 0, 64);
        }
        if(!j.contains("fog") || !j["fog"].is_object())
            return;
        const auto &f = j["fog"];
        m_fogStyle.rounding = std::clamp(f.value("rounding", m_fogStyle.rounding), 0.0f, 3.0f);
        m_fogStyle.texelsPerTile = std::clamp(f.value("texels_per_tile", m_fogStyle.texelsPerTile), 1, 16);
        m_fogStyle.exploredAlpha = std::clamp(f.value("explored_alpha", m_fogStyle.exploredAlpha), 0, 255);
        m_fogStyle.hiddenAlpha = std::clamp(f.value("hidden_alpha", m_fogStyle.hiddenAlpha), 0, 255);
        if(f.contains("color") && f["color"].is_array() && f["color"].size() >= 3)
        {
            m_fogStyle.color = {static_cast<Uint8>(f["color"][0].get<int>()), static_cast<Uint8>(f["color"][1].get<int>()), static_cast<Uint8>(f["color"][2].get<int>()), 255};
        }
    }

    namespace
    {

        // Separable box blur, run twice (close to a Gaussian): rounds off the staircase of tile edges
        void blurMask(std::vector<uint8_t> &img, std::vector<uint8_t> &tmp, int w, int h, int radius)
        {
            if(radius <= 0)
                return;
            tmp.resize(img.size());
            const int window = 2 * radius + 1;
            for(int pass = 0; pass < 2; ++pass)
            {
                for(int y = 0; y < h; ++y)
                {
                    const uint8_t *row = &img[static_cast<size_t>(y) * w];
                    int sum = 0;
                    for(int k = -radius; k <= radius; ++k)
                        sum += row[std::clamp(k, 0, w - 1)];
                    for(int x = 0; x < w; ++x)
                    {
                        tmp[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(sum / window);
                        sum += row[std::min(x + radius + 1, w - 1)] - row[std::max(x - radius, 0)];
                    }
                }
                for(int x = 0; x < w; ++x)
                {
                    int sum = 0;
                    for(int k = -radius; k <= radius; ++k)
                        sum += tmp[static_cast<size_t>(std::clamp(k, 0, h - 1)) * w + x];
                    for(int y = 0; y < h; ++y)
                    {
                        img[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(sum / window);
                        sum += tmp[static_cast<size_t>(std::min(y + radius + 1, h - 1)) * w + x] - tmp[static_cast<size_t>(std::max(y - radius, 0)) * w + x];
                    }
                }
            }
        }

    } // namespace

    void TileMap::renderFog(Renderer &renderer, const Camera &camera, bool smooth) const
    {
        if(!m_hasFog)
            return;

        float startWorldX, startWorldY, endWorldX, endWorldY;
        camera.screenToWorld(0, 0, startWorldX, startWorldY);
        camera.screenToWorld(camera.getViewportWidth(), camera.getViewportHeight(), endWorldX, endWorldY);

        // Visible window plus a margin, so the blur near the screen border sees the real neighbours
        const int s = smooth ? m_fogStyle.texelsPerTile : 1;
        const int radius = smooth ? static_cast<int>(std::lround(m_fogStyle.rounding * s * 0.5f)) : 0;
        const int margin = radius / s + 2;
        const int x0 = static_cast<int>(std::floor(startWorldX / TILE_WIDTH)) - margin;
        const int y0 = static_cast<int>(std::floor(startWorldY / TILE_HEIGHT)) - margin;
        const int x1 = static_cast<int>(std::ceil(endWorldX / TILE_WIDTH)) + margin;
        const int y1 = static_cast<int>(std::ceil(endWorldY / TILE_HEIGHT)) + margin;
        const int tilesW = x1 - x0;
        const int tilesH = y1 - y0;
        if(tilesW <= 0 || tilesH <= 0)
            return;

        const int w = tilesW * s;
        const int h = tilesH * s;
        m_fogMask.assign(static_cast<size_t>(w) * h, 0);
        for(int ty = 0; ty < tilesH; ++ty)
        {
            for(int tx = 0; tx < tilesW; ++tx)
            {
                const int mx = std::clamp(x0 + tx, 0, m_width - 1); // beyond the map edge: repeat the border
                const int my = std::clamp(y0 + ty, 0, m_height - 1);
                const FogState fog = m_fog[my][mx];
                const uint8_t a = fog == FogState::Hidden ? static_cast<uint8_t>(m_fogStyle.hiddenAlpha) : fog == FogState::Explored ? static_cast<uint8_t>(m_fogStyle.exploredAlpha) : 0;
                if(a == 0)
                    continue;
                for(int py = 0; py < s; ++py)
                {
                    std::fill_n(&m_fogMask[static_cast<size_t>(ty * s + py) * w + tx * s], s, a);
                }
            }
        }
        blurMask(m_fogMask, m_fogScratch, w, h, radius);

        renderer.drawAlphaMaskWorld(m_fogMask, w, h, static_cast<float>(x0 * TILE_WIDTH), static_cast<float>(y0 * TILE_HEIGHT), static_cast<float>(tilesW * TILE_WIDTH), static_cast<float>(tilesH * TILE_HEIGHT), camera, m_fogStyle.color, smooth);
    }

    void TileMap::renderWalls(Renderer &renderer, const Camera &camera, bool fogEnabled, int globalAnimTick) const
    {
        float startWorldX, startWorldY, endWorldX, endWorldY;
        camera.screenToWorld(0, 0, startWorldX, startWorldY);
        camera.screenToWorld(camera.getViewportWidth(), camera.getViewportHeight(), endWorldX, endWorldY);

        int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, m_width - 1);
        int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, m_width);
        int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, m_height - 1);
        int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, m_height);

        for(int ty = startTileY; ty < endTileY; ++ty)
        {
            renderWallRow(ty, renderer, camera, fogEnabled, globalAnimTick, startTileX, endTileX);
        }
    }

    void TileMap::renderWallRow(int ty, Renderer &renderer, const Camera &camera, bool fogEnabled, int globalAnimTick, int startTileX, int endTileX) const
    {
        if(ty < 0 || ty >= m_height)
            return;

        if(startTileX < 0 || endTileX < 0)
        {
            float startWorldX, startWorldY, endWorldX, endWorldY;
            camera.screenToWorld(0, 0, startWorldX, startWorldY);
            camera.screenToWorld(camera.getViewportWidth(), camera.getViewportHeight(), endWorldX, endWorldY);
            startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, m_width - 1);
            endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, m_width);
        }

        // Defensive walls (1001..1100 Confederation building 15..19, 1101..1200 Resistance building 25..29; f.java:6823).
        // Confederation walls of a player who has researched Active Wall Armor are drawn as the reinforced wall
        // (building 20..24, same damage steps): the research upgrades the walls already standing too.
        int reinforced[kMaxPlayers];
        std::fill(std::begin(reinforced), std::end(reinforced), -1);
        for(int tx = startTileX; tx < endTileX; ++tx)
        {
            if(fogEnabled && m_hasFog && m_fog[ty][tx] == FogState::Hidden)
                continue;

            // Intact or damaged standing wall (ground ruins/rubble rendered on Layer 1)
            uint16_t wval = m_wallGrid[ty][tx];
            if(wval >= 1001 && wval <= 1200)
            {
                int wallBase = (wval >= 1101) ? 25 : 15;
                const PlayerId owner = m_wallOwner[ty][tx];
                if(wallBase == 15 && owner < kMaxPlayers)
                {
                    if(reinforced[owner] < 0)
                        reinforced[owner] = isEffectResearched(owner, ResearchEffect::WallStrength) ? 1 : 0;
                    if(reinforced[owner] == 1)
                        wallBase = 20;
                }
                int damageStep = std::clamp(((wval - 1001) % 100) / 20, 0, 4);
                int wallBuildingId = wallBase + damageStep;
                Faction wallFaction = (wval >= 1101) ? Faction::Resistance : Faction::Confederation;
                renderer.drawBuildingWorld(wallBuildingId, tx * TILE_WIDTH, ty * TILE_HEIGHT, 0, camera, wallFaction, globalAnimTick, 1.0f, false);
                renderer.drawBuildingWorld(wallBuildingId, tx * TILE_WIDTH, ty * TILE_HEIGHT, 1, camera, wallFaction, globalAnimTick, 1.0f, false);
            }
        }
    }

    uint8_t TileMap::getBaseTile(int x, int y) const
    {
        if(x < 0 || x >= m_width || y < 0 || y >= m_height)
            return 0;
        return m_baseTiles[y][x];
    }

    void TileMap::setBaseTile(int x, int y, uint8_t tile)
    {
        if(x >= 0 && x < m_width && y >= 0 && y < m_height)
        {
            m_baseTiles[y][x] = tile;
        }
    }

    void TileMap::setSize(int width, int height)
    {
        m_width = std::clamp(width, 1, MAX_MAP_SIZE);
        m_height = std::clamp(height, 1, MAX_MAP_SIZE);
    }

    void TileMap::clearDecorations()
    {
        m_decorations.clear();
        for(int y = 0; y < m_height; ++y)
        {
            for(int x = 0; x < m_width; ++x)
            {
                m_decorGrid[y][x] = 0;
            }
        }
    }

    void TileMap::addDecoration(int type, int x, int y)
    {
        if(x < 0 || x >= m_width || y < 0 || y >= m_height)
            return;
        removeDecorationAt(x, y);
        MapDecoration dec;
        dec.type = type;
        dec.x = x;
        dec.y = y;
        m_decorations.push_back(dec);
        m_decorGrid[y][x] = static_cast<uint16_t>(type + 1);
        m_passable[y][x] = false;
    }

    void TileMap::removeDecorationAt(int x, int y)
    {
        if(x < 0 || x >= m_width || y < 0 || y >= m_height)
            return;
        m_decorGrid[y][x] = 0;
        for(auto it = m_decorations.begin(); it != m_decorations.end();)
        {
            if(it->x == x && it->y == y)
            {
                it = m_decorations.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    uint16_t TileMap::getDecorGrid(int x, int y) const
    {
        if(x < 0 || x >= m_width || y < 0 || y >= m_height)
            return 0;
        return m_decorGrid[y][x];
    }

} // namespace OpenArtOfWar
