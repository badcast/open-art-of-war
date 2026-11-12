#include "game/TileMap.hpp"
#include "graphics/SpriteRegistry.hpp"
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
    for (int y = 0; y < MAX_MAP_SIZE; ++y)
    {
        for (int x = 0; x < MAX_MAP_SIZE; ++x)
        {
            m_passable[y][x] = true;
            m_fog[y][x] = FogState::Hidden;
        }
    }
}

bool TileMap::LoadMap(const std::string &jsonPath)
{
    std::string text;
    if (!Assets::ReadText(jsonPath, text))
    {
        std::cerr << "[TileMap] Failed to open map file: " << jsonPath << std::endl;
        return false;
    }

    nlohmann::json mapJson = nlohmann::json::parse(text);

    m_width = std::clamp(mapJson["width"].get<int>(), 1, MAX_MAP_SIZE);
    m_height = std::clamp(mapJson["height"].get<int>(), 1, MAX_MAP_SIZE);
    m_tilesetId = mapJson["tilesetId"].get<int>();

    for (int y = 0; y < MAX_MAP_SIZE; ++y)
    {
        for (int x = 0; x < MAX_MAP_SIZE; ++x)
        {
            m_baseTiles[y][x] = 0;
            m_decorGrid[y][x] = 0;
            m_wallGrid[y][x] = 0;
            m_wallRubbleGrid[y][x] = 0;
            m_seenWall[y][x] = 0;
            m_seenWallOwner[y][x] = kNoPlayer;
            m_seenRubble[y][x] = 0;
            m_wallHp[y][x] = 0;
            m_passable[y][x] = true;
        }
    }

    // Terrain variation ranges by tileset.
    const int variationRanges0[5][2] = {{0, 10}, {10, 18}, {18, 19}, {19, 26}, {26, 34}};
    const int variationRanges1[5][2] = {{0, 10}, {10, 18}, {18, 26}, {26, 33}, {33, 36}};
    const auto &variationRanges = (m_tilesetId == 1) ? variationRanges1 : variationRanges0;

    // 1. Unpack base terrain tiles from RLE stream
    int currentX = 0;
    int currentY = 0;
    for (const auto &span : mapJson["terrainRle"])
    {
        int runLength = span["runLength"].get<int>();
        int tileType = span["tileType"].get<int>();
        for (int i = 0; i < runLength; ++i)
        {
            if (currentY < m_height && currentX < m_width)
            {
                int finalTile = tileType;
                for (int rangeIdx = 0; rangeIdx < 5; ++rangeIdx)
                {
                    if (tileType >= variationRanges[rangeIdx][0] && tileType < variationRanges[rangeIdx][1])
                    {
                        int spanCount = variationRanges[rangeIdx][1] - variationRanges[rangeIdx][0];
                        if (spanCount > 1)
                        {
                            std::uint32_t hash
                                = (static_cast<std::uint32_t>(currentX) * 73856093u
                                   ^ static_cast<std::uint32_t>(currentY) * 19349663u);
                            int variantIndex = static_cast<int>(hash % static_cast<std::uint32_t>(spanCount));
                            finalTile = variationRanges[rangeIdx][0] + variantIndex;
                        }
                        break;
                    }
                }
                m_baseTiles[currentY][currentX] = static_cast<std::uint8_t>(finalTile);
                if (TileCollision::IsBlocked(m_tilesetId, finalTile))
                {
                    m_passable[currentY][currentX] = false; // The sprite of this tile blocks movement
                }
            }
            currentX++;
            if (currentX >= m_width)
            {
                currentX = 0;
                currentY++;
            }
        }
    }

    // 2. Unpack decorations (trees, bridges, cliffs, landmarks)
    m_decorations.clear();
    for (const auto &decorJson : mapJson["decorations"])
    {
        MapDecoration decoration;
        decoration.type = decorJson["type"].get<int>();
        decoration.x = decorJson["x"].get<int>();
        decoration.y = decorJson["y"].get<int>();
        m_decorations.push_back(decoration);
        if (decoration.y >= 0 && decoration.y < m_height && decoration.x >= 0 && decoration.x < m_width)
        {
            m_decorGrid[decoration.y][decoration.x] = static_cast<std::uint16_t>(decoration.type + 1);
            m_passable[decoration.y][decoration.x] = false;
        }
    }

    // 3. Apply passability collision bitmask after decorations.
    // Where bit == 0, tile becomes passable (true); where bit == 1, blocked (false).
    // A tile whose sprite blocks (TileCollision) stays blocked whatever the mask says.
    const auto &collisionMask = mapJson["collisionMask"];
    for (int y = 0; y < m_height && y < static_cast<int>(collisionMask.size()); ++y)
    {
        const auto &rowChunks = collisionMask[y];
        int chunkIndex = 0;
        for (const auto &chunkValue : rowChunks)
        {
            std::uint32_t mask = chunkValue.get<std::uint32_t>();
            for (int bit = 0; bit < 32; ++bit)
            {
                int x = chunkIndex * 32 + bit;
                if (x < m_width)
                {
                    bool blocked
                        = (mask & (1u << (31 - bit))) != 0 || TileCollision::IsBlocked(m_tilesetId, m_baseTiles[y][x]);
                    m_passable[y][x] = !blocked;
                }
            }
            chunkIndex++;
        }
    }

    // 3b. Unpack preplaced defensive walls from map JSON if present
    if (mapJson.contains("walls"))
    {
        for (const auto &wallEntry : mapJson["walls"])
        {
            if (wallEntry.is_object())
            {
                int wallX = wallEntry.value("x", 0);
                int wallY = wallEntry.value("y", 0);
                int wallWidth = wallEntry.value("w", 1);
                int wallHeight = wallEntry.value("h", 1);
                int hp = wallEntry.value("hp", 100);
                Faction faction = Faction::Confederation;
                if (wallEntry.contains("faction"))
                {
                    if (wallEntry["faction"].is_string())
                    {
                        std::string factionStr = wallEntry["faction"].get<std::string>();
                        faction = (factionStr == "1") ? Faction::Resistance
                                                      : Utility::ParseFaction(factionStr, Faction::Confederation);
                    }
                    else if (wallEntry["faction"].is_number_integer())
                    {
                        if (wallEntry["faction"].get<int>() != 0)
                            faction = Faction::Resistance;
                    }
                }
                for (int tileY = wallY; tileY < wallY + wallHeight; ++tileY)
                {
                    for (int tileX = wallX; tileX < wallX + wallWidth; ++tileX)
                    {
                        SetWall(tileX, tileY, faction, hp);
                    }
                }
            }
            else if (wallEntry.is_array())
            {
                if (wallEntry.size() >= 4)
                {
                    int wallX = wallEntry[0].get<int>();
                    int wallY = wallEntry[1].get<int>();
                    int wallWidth = wallEntry[2].get<int>();
                    int wallHeight = wallEntry[3].get<int>();
                    Faction faction = (wallEntry.size() >= 5 && wallEntry[4].get<int>() != 0) ? Faction::Resistance
                                                                                             : Faction::Confederation;
                    int hp = (wallEntry.size() >= 6) ? wallEntry[5].get<int>() : 100;
                    for (int tileY = wallY; tileY < wallY + wallHeight; ++tileY)
                    {
                        for (int tileX = wallX; tileX < wallX + wallWidth; ++tileX)
                        {
                            SetWall(tileX, tileY, faction, hp);
                        }
                    }
                }
                else if (wallEntry.size() >= 2)
                {
                    SetWall(wallEntry[0].get<int>(), wallEntry[1].get<int>(), Faction::Confederation, 100);
                }
            }
        }
    }
    if (mapJson.contains("wallZones"))
    {
        for (const auto &wallZone : mapJson["wallZones"])
        {
            if (wallZone.is_array())
            {
                for (const auto &wallRect : wallZone)
                {
                    if (wallRect.is_array() && wallRect.size() >= 4)
                    {
                        int wallX = wallRect[0].get<int>();
                        int wallY = wallRect[1].get<int>();
                        int wallWidth = wallRect[2].get<int>();
                        int wallHeight = wallRect[3].get<int>();
                        for (int tileY = wallY; tileY < wallY + wallHeight; ++tileY)
                        {
                            for (int tileX = wallX; tileX < wallX + wallWidth; ++tileX)
                            {
                                SetWall(tileX, tileY, Faction::Confederation, 100);
                            }
                        }
                    }
                }
            }
        }
    }

    // Check if map explicitly enables or disables fog of war
    if (mapJson.contains("fogOfWar"))
    {
        m_hasFog = mapJson["fogOfWar"].get<bool>();
    }
    else if (mapJson.contains("hasFog"))
    {
        m_hasFog = mapJson["hasFog"].get<bool>();
    }
    else if (mapJson.contains("fog"))
    {
        m_hasFog = mapJson["fog"].get<bool>();
    }
    else
    {
        m_hasFog = true;
    }

    // Reset Fog of War
    for (int y = 0; y < m_height; ++y)
    {
        for (int x = 0; x < m_width; ++x)
        {
            m_fog[y][x] = m_hasFog ? FogState::Hidden : FogState::Visible;
        }
    }

    std::cout << "[TileMap] Loaded map: " << jsonPath << " (" << m_width << "x" << m_height
              << ", tileset=" << m_tilesetId << ", decors=" << m_decorations.size()
              << ", fog=" << (m_hasFog ? "ON" : "OFF") << ")" << std::endl;
    return true;
}

void TileMap::InitBlankArena(int width, int height, int tilesetId)
{
    m_width = std::clamp(width, 16, MAX_MAP_SIZE);
    m_height = std::clamp(height, 16, MAX_MAP_SIZE);
    m_tilesetId = tilesetId;
    m_hasFog = false;
    m_decorations.clear();

    for (int y = 0; y < MAX_MAP_SIZE; ++y)
    {
        for (int x = 0; x < MAX_MAP_SIZE; ++x)
        {
            std::uint32_t hash
                = (static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u);
            m_baseTiles[y][x] = static_cast<std::uint8_t>(hash % 10);
            m_decorGrid[y][x] = 0;
            m_wallGrid[y][x] = 0;
            m_wallRubbleGrid[y][x] = 0;
            m_seenWall[y][x] = 0;
            m_seenWallOwner[y][x] = kNoPlayer;
            m_seenRubble[y][x] = 0;
            m_wallHp[y][x] = 0;
            m_passable[y][x] = true;
            m_fog[y][x] = FogState::Visible;
        }
    }
    std::cout << "[TileMap] Initialized Blank Test Arena (" << m_width << "x" << m_height << ")" << std::endl;
}

bool TileMap::IsPassable(int tileX, int tileY) const
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
    {
        return false;
    }
    return m_passable[tileY][tileX];
}

void TileMap::SetPassable(int tileX, int tileY, bool passable)
{
    if (tileX >= 0 && tileX < m_width && tileY >= 0 && tileY < m_height)
    {
        m_passable[tileY][tileX] = passable;
    }
}

void TileMap::ClearWalls()
{
    for (int y = 0; y < m_height; ++y)
    {
        for (int x = 0; x < m_width; ++x)
        {
            if (m_wallGrid[y][x] == 0)
                continue;
            m_wallGrid[y][x] = 0;
            m_wallHp[y][x] = 0;
            m_passable[y][x] = true;
        }
    }
}

void TileMap::SetWall(int tileX, int tileY, Faction faction, int hpPercent, PlayerId owner)
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
        return;
    if (hpPercent <= 0)
    {
        m_wallGrid[tileY][tileX] = 0;
        m_wallHp[tileY][tileX] = 0;
        m_passable[tileY][tileX] = true;
        return;
    }
    int damageTier = std::clamp((100 - hpPercent) / 20, 0, 4); // 0=100%, 1=80%, 2=60%, 3=40%, 4=20%
    int base = (faction == Faction::Resistance) ? 1101 : 1001;
    m_wallGrid[tileY][tileX] = static_cast<std::uint16_t>(base + damageTier * 20);
    m_wallHp[tileY][tileX] = static_cast<std::int16_t>((150 * std::clamp(hpPercent, 1, 100)) / 100);
    m_wallOwner[tileY][tileX] = owner == kNoPlayer ? Players::OwnerOf(faction) : owner;
    m_wallRubbleGrid[tileY][tileX] = 0; // Clear any old ruins when a new wall is erected!
    m_passable[tileY][tileX] = false;
}

std::uint16_t TileMap::GetWall(int tileX, int tileY) const
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
        return 0;
    return m_wallGrid[tileY][tileX];
}

int TileMap::GetWallHp(int tileX, int tileY) const
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
        return 0;
    return m_wallHp[tileY][tileX];
}

PlayerId TileMap::GetWallOwner(int tileX, int tileY) const
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height || m_wallGrid[tileY][tileX] == 0)
        return kNoPlayer;
    return m_wallOwner[tileY][tileX];
}

void TileMap::SetWallOwner(int tileX, int tileY, PlayerId owner)
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height || m_wallGrid[tileY][tileX] == 0)
        return;
    m_wallOwner[tileY][tileX] = owner;
}

Faction TileMap::GetWallFaction(int tileX, int tileY) const
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
        return Faction::Neutral;
    std::uint16_t wallTile = m_wallGrid[tileY][tileX];
    if (wallTile >= 1101)
        return Faction::Resistance;
    if (wallTile >= 1001)
        return Faction::Confederation;
    return Faction::Neutral;
}

std::uint16_t TileMap::GetWallRubble(int tileX, int tileY) const
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
        return 0;
    return m_wallRubbleGrid[tileY][tileX];
}

void TileMap::DestroyWall(int tileX, int tileY)
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height || m_wallGrid[tileY][tileX] == 0)
        return;
    m_wallHp[tileY][tileX] = 0;
    m_wallRubbleGrid[tileY][tileX] = 1778; // the compact rubble a wall shot down leaves
    m_wallGrid[tileY][tileX] = 0;
    m_passable[tileY][tileX] = true;
}

void TileMap::ForgetPlayerWalls(PlayerId player)
{
    for (int y = 0; y < m_height; ++y)
        for (int x = 0; x < m_width; ++x)
            if (m_seenWall[y][x] != 0 && m_seenWallOwner[y][x] == player)
            {
                m_seenWall[y][x] = 0;
                m_seenWallOwner[y][x] = kNoPlayer;
                m_seenRubble[y][x] = 1778;
            }
}

void TileMap::DamageWall(int tileX, int tileY, int damage)
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
        return;
    if (m_wallGrid[tileY][tileX] == 0)
        return;

    if (m_wallHp[tileY][tileX] <= 0)
    {
        m_wallHp[tileY][tileX] = 150;
    }

    damage = ReduceWallDamage(damage, m_wallOwner[tileY][tileX]);
    m_wallHp[tileY][tileX] -= damage;
    if (m_wallHp[tileY][tileX] <= 0)
    {
        m_wallHp[tileY][tileX] = 0;
        // Leave behind authentic compact wall rubble remnant (composite 1778, 31x22 px)
        m_wallRubbleGrid[tileY][tileX] = 1778;
        m_wallGrid[tileY][tileX] = 0;
        m_passable[tileY][tileX] = true;
    }
    else
    {
        std::uint16_t base = (m_wallGrid[tileY][tileX] >= 1101) ? 1101 : 1001;
        float hpRatio = static_cast<float>(m_wallHp[tileY][tileX]) / 150.0f;
        int damageTier = std::clamp(static_cast<int>((1.0f - hpRatio) * 5.0f), 0, 4);
        m_wallGrid[tileY][tileX] = static_cast<std::uint16_t>(base + damageTier * 20);
    }
}

namespace
{
inline std::uint8_t TeamBit(std::uint8_t team)
{
    return static_cast<std::uint8_t>(1u << (team & 7));
}
} // namespace

void TileMap::ResetVisibility()
{
    for (int y = 0; y < m_height; ++y)
    {
        for (int x = 0; x < m_width; ++x)
        {
            m_sight[y][x] = 0;
            if (m_fog[y][x] == FogState::Visible)
            {
                RememberWall(x, y); // what the viewer saw there last, until it looks again
                m_fog[y][x] = FogState::Explored;
            }
        }
    }
}

void TileMap::RememberWall(int x, int y)
{
    m_seenWall[y][x] = m_wallGrid[y][x];
    m_seenWallOwner[y][x] = m_wallOwner[y][x];
    m_seenRubble[y][x] = m_wallRubbleGrid[y][x];
}

bool TileMap::ViewerSeesLive(int x, int y) const
{
    return !m_hasFog || m_fog[y][x] == FogState::Visible;
}

std::uint16_t TileMap::ViewerWall(int tileX, int tileY) const
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height || m_fog[tileY][tileX] == FogState::Hidden)
        return 0;
    return ViewerSeesLive(tileX, tileY) ? m_wallGrid[tileY][tileX] : m_seenWall[tileY][tileX];
}

PlayerId TileMap::ViewerWallOwner(int tileX, int tileY) const
{
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
        return kNoPlayer;
    return ViewerSeesLive(tileX, tileY) ? m_wallOwner[tileY][tileX] : m_seenWallOwner[tileY][tileX];
}

bool TileMap::IsVisibleTo(int tileX, int tileY, std::uint8_t team) const
{
    if (!m_hasFog)
        return true;
    if (tileX < 0 || tileY < 0 || tileX >= m_width || tileY >= m_height)
        return false;
    return (m_sight[tileY][tileX] & TeamBit(team)) != 0;
}

void TileMap::ResetViewerFog()
{
    const std::uint8_t teamBit = TeamBit(m_viewerTeam);
    for (int y = 0; y < m_height; ++y)
    {
        for (int x = 0; x < m_width; ++x)
        {
            const bool seen = (m_sight[y][x] & teamBit) != 0;
            m_fog[y][x] = seen ? FogState::Visible : FogState::Hidden;
            if (seen)
                RememberWall(x, y);
        }
    }
}

void TileMap::ResetAllFog(FogState state)
{
    for (int y = 0; y < m_height; ++y)
    {
        for (int x = 0; x < m_width; ++x)
        {
            m_fog[y][x] = state;
            m_sight[y][x] = state == FogState::Visible ? 0xFF : 0;
            if (state == FogState::Visible)
                RememberWall(x, y);
        }
    }
}

void TileMap::RevealArea(int centerTileX, int centerTileY, int radius, std::uint8_t team)
{
    const std::uint8_t teamBit = TeamBit(team);
    const bool isViewer = (team & 7) == m_viewerTeam;
    int radiusSq = radius * radius;
    int minX = std::max(0, centerTileX - radius);
    int maxX = std::min(m_width - 1, centerTileX + radius);
    int minY = std::max(0, centerTileY - radius);
    int maxY = std::min(m_height - 1, centerTileY + radius);

    for (int y = minY; y <= maxY; ++y)
    {
        int deltaY = y - centerTileY;
        for (int x = minX; x <= maxX; ++x)
        {
            int deltaX = x - centerTileX;
            if (deltaX * deltaX + deltaY * deltaY <= radiusSq)
            {
                m_sight[y][x] |= teamBit;
                if (isViewer)
                    m_fog[y][x] = FogState::Visible;
            }
        }
    }
}

void TileMap::RevealRect(int tileX, int tileY, int width, int height, std::uint8_t team)
{
    const std::uint8_t teamBit = TeamBit(team);
    const bool isViewer = (team & 7) == m_viewerTeam;
    for (int y = std::max(0, tileY); y < std::min(m_height, tileY + height); ++y)
    {
        for (int x = std::max(0, tileX); x < std::min(m_width, tileX + width); ++x)
        {
            m_sight[y][x] |= teamBit;
            if (isViewer)
                m_fog[y][x] = FogState::Visible;
        }
    }
}

FogState TileMap::GetFogState(int tileX, int tileY) const
{
    if (!m_hasFog)
    {
        return FogState::Visible;
    }
    if (tileX < 0 || tileX >= m_width || tileY < 0 || tileY >= m_height)
    {
        return FogState::Hidden;
    }
    return m_fog[tileY][tileX];
}

void TileMap::Render(Renderer &renderer, const Camera &camera, bool fogEnabled, int globalAnimTick)
{
    bool useFog = fogEnabled && m_hasFog;
    RenderTerrain(renderer, camera, useFog, globalAnimTick);
    RenderWallRubble(renderer, camera, useFog);
    RenderDecorations(renderer, camera, useFog, globalAnimTick);
    if (useFog)
    {
        RenderFog(renderer, camera);
    }
}

void TileMap::RenderTerrain(Renderer &renderer, const Camera &camera, bool fogEnabled, int globalAnimTick) const
{
    float startWorldX, startWorldY, endWorldX, endWorldY;
    camera.ScreenToWorld(0, 0, startWorldX, startWorldY);
    camera.ScreenToWorld(camera.GetViewportWidth(), camera.GetViewportHeight(), endWorldX, endWorldY);

    int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, m_width - 1);
    int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, m_width);
    int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, m_height - 1);
    int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, m_height);

    for (int tileY = startTileY; tileY < endTileY; ++tileY)
    {
        for (int tileX = startTileX; tileX < endTileX; ++tileX)
        {
            FogState fog = m_fog[tileY][tileX];
            if (fogEnabled && m_hasFog && fog == FogState::Hidden)
            {
                // Hidden ground next to seen ground is drawn: the soft fog edge reaches into it
                bool hasSeenNeighbor = false;
                for (int dy = -1; dy <= 1 && !hasSeenNeighbor; ++dy)
                {
                    for (int dx = -1; dx <= 1 && !hasSeenNeighbor; ++dx)
                    {
                        const int neighborX = tileX + dx, neighborY = tileY + dy;
                        hasSeenNeighbor = neighborX >= 0 && neighborY >= 0 && neighborX < m_width && neighborY < m_height
                                          && m_fog[neighborY][neighborX] != FogState::Hidden;
                    }
                }
                if (!hasSeenNeighbor)
                {
                    renderer.FillRectWorld(
                        tileX * TILE_WIDTH, tileY * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, camera, m_fogStyle.color);
                    continue;
                }
            }

            int tileIndex = m_baseTiles[tileY][tileX];
            renderer.DrawTileWorld(
                m_tilesetId, tileIndex, tileX * TILE_WIDTH, tileY * TILE_HEIGHT, camera, globalAnimTick);
        }
    }
}

void TileMap::RenderWallRubble(Renderer &renderer, const Camera &camera, bool fogEnabled) const
{
    float startWorldX, startWorldY, endWorldX, endWorldY;
    camera.ScreenToWorld(0, 0, startWorldX, startWorldY);
    camera.ScreenToWorld(camera.GetViewportWidth(), camera.GetViewportHeight(), endWorldX, endWorldY);

    int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, m_width - 1);
    int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, m_width);
    int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, m_height - 1);
    int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, m_height);

    for (int tileY = startTileY; tileY < endTileY; ++tileY)
    {
        for (int tileX = startTileX; tileX < endTileX; ++tileX)
        {
            if (fogEnabled && m_hasFog && m_fog[tileY][tileX] == FogState::Hidden)
                continue;
            // Under the fog of war the viewer sees the rubble as it was when the tile was last in sight
            const bool seesLive = !fogEnabled || ViewerSeesLive(tileX, tileY);
            std::uint16_t rubble = seesLive ? m_wallRubbleGrid[tileY][tileX] : m_seenRubble[tileY][tileX];
            if (rubble > 0)
            {
                // The grid only marks the rubble (the simulation compares it); its picture is a sprite
                static const CachedSprite sprite("fx_wall_rubble.rubble", SpriteKind::Part);
                renderer.DrawCompositeWorld(
                    sprite, tileX * TILE_WIDTH - 90.0f, tileY * TILE_HEIGHT - 60.0f, camera, Faction::Neutral);
            }
        }
    }
}

void TileMap::RenderDecorations(Renderer &renderer, const Camera &camera, bool fogEnabled, int globalAnimTick) const
{
    float startWorldX, startWorldY, endWorldX, endWorldY;
    camera.ScreenToWorld(0, 0, startWorldX, startWorldY);
    camera.ScreenToWorld(camera.GetViewportWidth(), camera.GetViewportHeight(), endWorldX, endWorldY);

    int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, m_width - 1);
    int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, m_width);
    int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, m_height - 1);
    int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, m_height);

    for (const auto &decoration : m_decorations)
    {
        if (decoration.x < startTileX - 4 || decoration.x > endTileX + 4 || decoration.y < startTileY - 4
            || decoration.y > endTileY + 4)
        {
            continue;
        }

        if (fogEnabled && m_hasFog && m_fog[decoration.y][decoration.x] == FogState::Hidden)
        {
            continue;
        }

        // Trees and rocks: only the ground layer here, their crowns are sorted with the units (EntityManager::render)
        const int layer = renderer.TallDecorRows(m_tilesetId, decoration.type) > 0 ? 0 : -1;
        renderer.DrawDecorWorld(
            m_tilesetId,
            decoration.type,
            decoration.x * TILE_WIDTH,
            decoration.y * TILE_HEIGHT,
            camera,
            globalAnimTick,
            layer);
    }
}

void TileMap::LoadEnvironment(const EnvironmentConfig &envConfig)
{
    const nlohmann::json &rootJson = envConfig.Root();
    if (rootJson.contains("construction") && rootJson["construction"].is_object())
    {
        const auto &constructionJson = rootJson["construction"];
        m_constructionRules.enemyBlockTiles
            = std::clamp(constructionJson.value("enemy_block_tiles", m_constructionRules.enemyBlockTiles), 0, 64);
    }
    if (!rootJson.contains("fog") || !rootJson["fog"].is_object())
        return;
    const auto &fogJson = rootJson["fog"];
    m_fogStyle.rounding = std::clamp(fogJson.value("rounding", m_fogStyle.rounding), 0.0f, 3.0f);
    m_fogStyle.texelsPerTile = std::clamp(fogJson.value("texels_per_tile", m_fogStyle.texelsPerTile), 1, 16);
    m_fogStyle.exploredAlpha = std::clamp(fogJson.value("explored_alpha", m_fogStyle.exploredAlpha), 0, 255);
    m_fogStyle.hiddenAlpha = std::clamp(fogJson.value("hidden_alpha", m_fogStyle.hiddenAlpha), 0, 255);
    if (fogJson.contains("color") && fogJson["color"].is_array() && fogJson["color"].size() >= 3)
    {
        m_fogStyle.color
            = {static_cast<Uint8>(fogJson["color"][0].get<int>()),
               static_cast<Uint8>(fogJson["color"][1].get<int>()),
               static_cast<Uint8>(fogJson["color"][2].get<int>()),
               255};
    }
}

namespace
{

// Separable box blur, run twice (close to a Gaussian): rounds off the staircase of tile edges
void BlurMask(std::vector<std::uint8_t> &mask, std::vector<std::uint8_t> &scratch, int width, int height, int radius)
{
    if (radius <= 0)
        return;
    scratch.resize(mask.size());
    const int window = 2 * radius + 1;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int y = 0; y < height; ++y)
        {
            const std::uint8_t *row = &mask[static_cast<std::size_t>(y) * width];
            int sum = 0;
            for (int k = -radius; k <= radius; ++k)
                sum += row[std::clamp(k, 0, width - 1)];
            for (int x = 0; x < width; ++x)
            {
                scratch[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(sum / window);
                sum += row[std::min(x + radius + 1, width - 1)] - row[std::max(x - radius, 0)];
            }
        }
        for (int x = 0; x < width; ++x)
        {
            int sum = 0;
            for (int k = -radius; k <= radius; ++k)
                sum += scratch[static_cast<std::size_t>(std::clamp(k, 0, height - 1)) * width + x];
            for (int y = 0; y < height; ++y)
            {
                mask[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(sum / window);
                sum += scratch[static_cast<std::size_t>(std::min(y + radius + 1, height - 1)) * width + x]
                       - scratch[static_cast<std::size_t>(std::max(y - radius, 0)) * width + x];
            }
        }
    }
}

} // namespace

void TileMap::RenderFog(Renderer &renderer, const Camera &camera, bool smooth) const
{
    if (!m_hasFog)
        return;

    float startWorldX, startWorldY, endWorldX, endWorldY;
    camera.ScreenToWorld(0, 0, startWorldX, startWorldY);
    camera.ScreenToWorld(camera.GetViewportWidth(), camera.GetViewportHeight(), endWorldX, endWorldY);

    // Visible window plus a margin, so the blur near the screen border sees the real neighbours
    const int texelsPerTile = smooth ? m_fogStyle.texelsPerTile : 1;
    const int radius = smooth ? static_cast<int>(std::lround(m_fogStyle.rounding * texelsPerTile * 0.5f)) : 0;
    const int marginTiles = radius / texelsPerTile + 2;
    const int startTileX = static_cast<int>(std::floor(startWorldX / TILE_WIDTH)) - marginTiles;
    const int startTileY = static_cast<int>(std::floor(startWorldY / TILE_HEIGHT)) - marginTiles;
    const int endTileX = static_cast<int>(std::ceil(endWorldX / TILE_WIDTH)) + marginTiles;
    const int endTileY = static_cast<int>(std::ceil(endWorldY / TILE_HEIGHT)) + marginTiles;
    const int tilesWidth = endTileX - startTileX;
    const int tilesHeight = endTileY - startTileY;
    if (tilesWidth <= 0 || tilesHeight <= 0)
        return;

    const int maskWidth = tilesWidth * texelsPerTile;
    const int maskHeight = tilesHeight * texelsPerTile;
    m_fogMask.assign(static_cast<std::size_t>(maskWidth) * maskHeight, 0);
    for (int tileY = 0; tileY < tilesHeight; ++tileY)
    {
        for (int tileX = 0; tileX < tilesWidth; ++tileX)
        {
            const int mapX = std::clamp(startTileX + tileX, 0, m_width - 1); // beyond the map edge: repeat the border
            const int mapY = std::clamp(startTileY + tileY, 0, m_height - 1);
            const FogState fog = m_fog[mapY][mapX];
            const std::uint8_t alpha = fog == FogState::Hidden     ? static_cast<std::uint8_t>(m_fogStyle.hiddenAlpha)
                                       : fog == FogState::Explored ? static_cast<std::uint8_t>(m_fogStyle.exploredAlpha)
                                                                   : 0;
            if (alpha == 0)
                continue;
            for (int texelY = 0; texelY < texelsPerTile; ++texelY)
            {
                std::fill_n(
                    &m_fogMask[static_cast<std::size_t>(tileY * texelsPerTile + texelY) * maskWidth + tileX * texelsPerTile],
                    texelsPerTile,
                    alpha);
            }
        }
    }
    BlurMask(m_fogMask, m_fogScratch, maskWidth, maskHeight, radius);

    renderer.DrawAlphaMaskWorld(
        m_fogMask,
        maskWidth,
        maskHeight,
        static_cast<float>(startTileX * TILE_WIDTH),
        static_cast<float>(startTileY * TILE_HEIGHT),
        static_cast<float>(tilesWidth * TILE_WIDTH),
        static_cast<float>(tilesHeight * TILE_HEIGHT),
        camera,
        m_fogStyle.color,
        smooth);
}

void TileMap::RenderWalls(Renderer &renderer, const Camera &camera, bool fogEnabled, int globalAnimTick) const
{
    float startWorldX, startWorldY, endWorldX, endWorldY;
    camera.ScreenToWorld(0, 0, startWorldX, startWorldY);
    camera.ScreenToWorld(camera.GetViewportWidth(), camera.GetViewportHeight(), endWorldX, endWorldY);

    int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, m_width - 1);
    int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, m_width);
    int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, m_height - 1);
    int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, m_height);

    for (int tileY = startTileY; tileY < endTileY; ++tileY)
    {
        RenderWallRow(tileY, renderer, camera, fogEnabled, globalAnimTick, startTileX, endTileX);
    }
}

void TileMap::RenderWallRow(
    int tileY,
    Renderer &renderer,
    const Camera &camera,
    bool fogEnabled,
    int globalAnimTick,
    int startTileX,
    int endTileX) const
{
    if (tileY < 0 || tileY >= m_height)
        return;

    if (startTileX < 0 || endTileX < 0)
    {
        float startWorldX, startWorldY, endWorldX, endWorldY;
        camera.ScreenToWorld(0, 0, startWorldX, startWorldY);
        camera.ScreenToWorld(camera.GetViewportWidth(), camera.GetViewportHeight(), endWorldX, endWorldY);
        startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, m_width - 1);
        endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, m_width);
    }

    // Defensive walls (1001..1100 Confederation building 15..19, 1101..1200 Resistance building 25..29).
    // Confederation walls of a player who has researched Active Wall Armor are drawn as the reinforced wall
    // (building 20..24, same damage steps): the research upgrades the walls already standing too.
    int reinforced[kMaxPlayers];
    std::fill(std::begin(reinforced), std::end(reinforced), -1);
    for (int tileX = startTileX; tileX < endTileX; ++tileX)
    {
        if (fogEnabled && m_hasFog && m_fog[tileY][tileX] == FogState::Hidden)
            continue;

        // Intact or damaged standing wall (ground ruins/rubble rendered on Layer 1). Under the fog of war (explored,
        // not in sight) the viewer sees the wall as it was when the tile was last in sight: walls built, damaged or
        // destroyed there since stay unknown
        const bool seesLive = !fogEnabled || ViewerSeesLive(tileX, tileY);
        std::uint16_t wallValue = seesLive ? m_wallGrid[tileY][tileX] : m_seenWall[tileY][tileX];
        if (wallValue >= 1001 && wallValue <= 1200)
        {
            int wallBase = (wallValue >= 1101) ? 25 : 15;
            const PlayerId owner = seesLive ? m_wallOwner[tileY][tileX] : m_seenWallOwner[tileY][tileX];
            if (wallBase == 15 && owner < kMaxPlayers)
            {
                if (reinforced[owner] < 0)
                    reinforced[owner] = IsEffectResearched(owner, ResearchEffect::WallStrength) ? 1 : 0;
                if (reinforced[owner] == 1)
                    wallBase = 20;
            }
            int damageStep = std::clamp(((wallValue - 1001) % 100) / 20, 0, 4);
            int wallBuildingId = wallBase + damageStep;
            Faction wallFaction = (wallValue >= 1101) ? Faction::Resistance : Faction::Confederation;
            renderer.SetDrawOwner(owner); // the wall wears its owner's army colour
            renderer.DrawBuildingWorld(
                wallBuildingId, tileX * TILE_WIDTH, tileY * TILE_HEIGHT, 0, camera, wallFaction, globalAnimTick, 1.0f, false);
            renderer.DrawBuildingWorld(
                wallBuildingId, tileX * TILE_WIDTH, tileY * TILE_HEIGHT, 1, camera, wallFaction, globalAnimTick, 1.0f, false);
        }
    }
    renderer.SetDrawOwner(kNoPlayer);
}

std::uint8_t TileMap::GetBaseTile(int x, int y) const
{
    if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        return 0;
    return m_baseTiles[y][x];
}

void TileMap::SetBaseTile(int x, int y, std::uint8_t tile)
{
    if (x >= 0 && x < m_width && y >= 0 && y < m_height)
    {
        m_baseTiles[y][x] = tile;
    }
}

void TileMap::SetSize(int width, int height)
{
    m_width = std::clamp(width, 1, MAX_MAP_SIZE);
    m_height = std::clamp(height, 1, MAX_MAP_SIZE);
}

void TileMap::ClearDecorations()
{
    m_decorations.clear();
    for (int y = 0; y < m_height; ++y)
    {
        for (int x = 0; x < m_width; ++x)
        {
            m_decorGrid[y][x] = 0;
        }
    }
}

void TileMap::AddDecoration(int type, int x, int y)
{
    if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        return;
    RemoveDecorationAt(x, y);
    MapDecoration decoration;
    decoration.type = type;
    decoration.x = x;
    decoration.y = y;
    m_decorations.push_back(decoration);
    m_decorGrid[y][x] = static_cast<std::uint16_t>(type + 1);
    m_passable[y][x] = false;
}

void TileMap::RemoveDecorationAt(int x, int y)
{
    if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        return;
    m_decorGrid[y][x] = 0;
    for (auto it = m_decorations.begin(); it != m_decorations.end();)
    {
        if (it->x == x && it->y == y)
        {
            it = m_decorations.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::uint16_t TileMap::GetDecorGrid(int x, int y) const
{
    if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        return 0;
    return m_decorGrid[y][x];
}

} // namespace OpenArtOfWar
