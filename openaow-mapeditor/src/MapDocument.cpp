#include "MapDocument.hpp"
#include "core/AssetStore.hpp"
#include "core/Utility.hpp"
#include "game/TileCollision.hpp"
#include "graphics/TextureManager.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <queue>
#include <cmath>
#include <tuple>
#include <utility>

namespace OpenArtOfWar {

namespace {

Faction readFaction(const nlohmann::json& item) {
    const auto faction = item.find("faction");
    if (faction == item.end()) return Faction::Resistance;
    if (faction->is_string()) return Utility::parseFaction(faction->get<std::string>());
    if (faction->is_number_integer()) {
        const int value = faction->get<int>();
        return value == 1 ? Faction::Confederation : (value == 2 ? Faction::Neutral : Faction::Resistance);
    }
    return Faction::Resistance;
}

int legacyBuildingIndex(int type, Faction faction) {
    if (faction == Faction::Confederation) {
        switch (type) {
            case 8: return 0;
            case 9: return 2;
            case 10: return 3;
            case 11: return 4;
            case 12: return 7;
            case 13: return 5;
            case 14: return 1;
            case 15: return 6;
            default: return type;
        }
    }
    switch (type) {
        case 0: return 8;
        case 1: return 10;
        case 2: return 11;
        case 3: return 12;
        case 4: return 14;
        case 5: return 13;
        case 6: return 9;
        default: return type;
    }
}

std::pair<int, int> buildingSize(int type) {
    static constexpr int widths[] = {4, 3, 3, 4, 3, 2, 2, 2, 3, 3, 3, 3, 3, 2, 2};
    static constexpr int heights[] = {4, 3, 3, 3, 3, 2, 2, 2, 4, 3, 3, 3, 3, 2, 2};
    if (type < 0 || type >= 15) return {1, 1};
    return {widths[type], heights[type]};
}

} // namespace

MapDocument::MapDocument() {
    newMap(60, 40, 0, 0);
}

void MapDocument::newMap(int width, int height, int tilesetId, uint8_t fillTile) {
    width = std::clamp(width, 10, MAX_MAP_SIZE);
    height = std::clamp(height, 10, MAX_MAP_SIZE);
    tilesetId = std::clamp(tilesetId, 0, 1);

    m_map.initBlankArena(width, height, tilesetId);
    m_map.setSize(width, height);
    m_map.setTilesetId(tilesetId);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            m_map.setBaseTile(x, y, fillTile);
            m_map.setPassable(x, y, !TileCollision::isBlocked(tilesetId, fillTile));
        }
    }

    m_map.clearDecorations();
    m_entities.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_nextEntityId = 1;

    m_playerStartX = std::clamp(6, 1, width - 2);
    m_playerStartY = std::clamp(7, 1, height - 2);
    m_enemyStartX = std::clamp(width - 8, 1, width - 2);
    m_enemyStartY = std::clamp(height - 8, 1, height - 2);
    m_extraStarts.clear();

    m_name = "new_map";
    m_filePath = "";
    m_mapData = nlohmann::json::object();
    m_modified = false;
}

bool MapDocument::loadMap(const std::string& path) {
    std::string fileText;
    if (!OpenArtOfWar::Assets::readText(path, fileText)) {
        std::cerr << "[MapDocument] Failed to open file: " << path << std::endl;
        return false;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(fileText);
    } catch (const std::exception& e) {
        std::cerr << "[MapDocument] JSON parse error: " << e.what() << std::endl;
        return false;
    }

    if (!m_map.loadMap(path)) {
        return false;
    }

    m_name = j.value("name", "untitled");
    m_filePath = path;
    m_mapData = j;
    m_entities.clear();
    m_nextEntityId = 1;

    if (j.contains("headquarters") && j["headquarters"].is_object() && j["headquarters"].contains("player")) {
        m_playerStartX = j["headquarters"]["player"].value("x", 6);
        m_playerStartY = j["headquarters"]["player"].value("y", 7);
    } else if (j.contains("playerPos") && j["playerPos"].is_object()) {
        m_playerStartX = j["playerPos"].value("x", 6);
        m_playerStartY = j["playerPos"].value("y", 7);
    } else {
        m_playerStartX = 6;
        m_playerStartY = 7;
    }

    if (j.contains("headquarters") && j["headquarters"].is_object() && j["headquarters"].contains("enemy")) {
        m_enemyStartX = j["headquarters"]["enemy"].value("x", m_map.getWidth() - 8);
        m_enemyStartY = j["headquarters"]["enemy"].value("y", m_map.getHeight() - 8);
    } else if (j.contains("enemyPos") && j["enemyPos"].is_object()) {
        m_enemyStartX = j["enemyPos"].value("x", m_map.getWidth() - 8);
        m_enemyStartY = j["enemyPos"].value("y", m_map.getHeight() - 8);
    } else {
        m_enemyStartX = std::clamp(m_map.getWidth() - 8, 0, m_map.getWidth() - 1);
        m_enemyStartY = std::clamp(m_map.getHeight() - 8, 0, m_map.getHeight() - 1);
    }

    // start_positions: player, enemy, then the extra ones (players 3..8)
    m_extraStarts.clear();
    if (j.contains("start_positions") && j["start_positions"].is_array() && j["start_positions"].size() > 2) {
        for (size_t i = 2; i < j["start_positions"].size() && m_extraStarts.size() < kMaxExtraStarts; ++i) {
            const auto& p = j["start_positions"][i];
            if (!p.is_object()) continue;
            m_extraStarts.push_back({std::clamp(p.value("x", 0), 0, std::max(0, m_map.getWidth() - 1)),
                                     std::clamp(p.value("y", 0), 0, std::max(0, m_map.getHeight() - 1))});
        }
    }

    auto addMapEntity = [&](const nlohmann::json& item, bool isBuilding, bool gameFormat) {
        EditorEntity ent;
        ent.id = m_nextEntityId++;
        ent.isBuilding = isBuilding;
        ent.faction = readFaction(item);
        ent.tileX = item.value("x", 0);
        ent.tileY = item.value("y", 0);
        ent.facing = item.value("facing", 0);
        ent.mapData = item;
        if (isBuilding) {
            const int type = gameFormat ? item.value("buildingIndex", 0) : item.value("type", 0);
            ent.type = gameFormat ? type : legacyBuildingIndex(type, ent.faction);
            const auto [width, height] = buildingSize(ent.type);
            ent.buildWidth = item.value("buildWidth", width);
            ent.buildHeight = item.value("buildHeight", height);
            for (int y = ent.tileY; y < ent.tileY + ent.buildHeight && y < m_map.getHeight(); ++y) {
                for (int x = ent.tileX; x < ent.tileX + ent.buildWidth && x < m_map.getWidth(); ++x) {
                    m_map.setPassable(x, y, false);
                }
            }
        } else {
            ent.type = gameFormat ? item.value("archetypeId", 0) : item.value("type", 0);
        }
        ent.name = item.value("name", "");
        m_entities.push_back(std::move(ent));
    };

    if (j.contains("buildings") && j["buildings"].is_array()) {
        for (const auto& item : j["buildings"]) addMapEntity(item, true, true);
    }
    if (j.contains("units") && j["units"].is_array()) {
        for (const auto& item : j["units"]) addMapEntity(item, false, true);
    }
    if (m_entities.empty() && j.contains("entities") && j["entities"].is_array()) {
        for (const auto& ej : j["entities"]) {
            addMapEntity(ej, ej.value("isBuilding", false), false);
        }
    }

    m_undoStack.clear();
    m_redoStack.clear();
    m_modified = false;
    return true;
}

bool MapDocument::saveMap(const std::string& path) {
    nlohmann::json j = m_mapData.is_object() ? m_mapData : nlohmann::json::object();
    j["name"] = m_name;
    j["width"] = m_map.getWidth();
    j["height"] = m_map.getHeight();
    j["tilesetId"] = m_map.getTilesetId();

    // 1. RLE Compression of terrain base tiles
    nlohmann::json rleArray = nlohmann::json::array();
    int curTile = -1;
    int runLength = 0;
    int w = m_map.getWidth();
    int h = m_map.getHeight();

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t t = m_map.getBaseTile(x, y);
            if (curTile < 0) {
                curTile = t;
                runLength = 1;
            } else if (t == curTile) {
                runLength++;
            } else {
                rleArray.push_back({
                    {"runLength", runLength},
                    {"tileType", curTile}
                });
                curTile = t;
                runLength = 1;
            }
        }
    }
    if (runLength > 0 && curTile >= 0) {
        rleArray.push_back({
            {"runLength", runLength},
            {"tileType", curTile}
        });
    }
    j["terrainRle"] = rleArray;

    // 2. Decorations
    nlohmann::json decArray = nlohmann::json::array();
    for (const auto& d : m_map.getDecorations()) {
        decArray.push_back({
            {"type", d.type},
            {"x", d.x},
            {"y", d.y}
        });
    }
    j["decorations"] = decArray;

    // 3. Collision Bitmask
    nlohmann::json collMask = nlohmann::json::array();
    int numChunks = (w + 31) / 32;
    for (int y = 0; y < h; ++y) {
        nlohmann::json rowChunks = nlohmann::json::array();
        for (int chunkIdx = 0; chunkIdx < numChunks; ++chunkIdx) {
            uint32_t mask = 0;
            for (int bit = 0; bit < 32; ++bit) {
                int x = chunkIdx * 32 + bit;
                if (x < w) {
                    bool blocked = !m_map.isPassable(x, y);
                    if (blocked) {
                        mask |= (1u << (31 - bit));
                    }
                }
            }
            rowChunks.push_back(mask);
        }
        collMask.push_back(rowChunks);
    }
    j["collisionMask"] = collMask;

    // 4. Defensive Walls
    nlohmann::json wallsArray = nlohmann::json::array();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (m_map.getWall(x, y) > 0) {
                Faction fac = m_map.getWallFaction(x, y);
                int hp = m_map.getWallHp(x, y);
                int facInt = (fac == Faction::Resistance) ? 1 : 0;
                wallsArray.push_back({
                    {"x", x},
                    {"y", y},
                    {"w", 1},
                    {"h", 1},
                    {"faction", facInt},
                    {"hp", hp}
                });
            }
        }
    }
    j["walls"] = wallsArray;

    // 5. Game entities
    nlohmann::json buildingsArray = nlohmann::json::array();
    nlohmann::json unitsArray = nlohmann::json::array();
    for (const auto& e : m_entities) {
        nlohmann::json item = e.mapData.is_object() ? e.mapData : nlohmann::json::object();
        item["faction"] = Utility::factionKey(e.faction);
        item["x"] = e.tileX;
        item["y"] = e.tileY;
        if (e.isBuilding) {
            item["buildingIndex"] = e.type;
            buildingsArray.push_back(std::move(item));
        } else {
            item["archetypeId"] = e.type;
            item["facing"] = e.facing;
            unitsArray.push_back(std::move(item));
        }
    }
    j.erase("entities");
    j["buildings"] = std::move(buildingsArray);
    j["units"] = std::move(unitsArray);

    // 6. Starting positions
    if (!j.contains("headquarters") || !j["headquarters"].is_object()) {
        j["headquarters"] = nlohmann::json::object();
    }
    j["headquarters"]["player"] = {{"x", m_playerStartX}, {"y", m_playerStartY}};
    j["headquarters"]["enemy"] = {{"x", m_enemyStartX}, {"y", m_enemyStartY}};
    j["playerPos"] = {{"x", m_playerStartX}, {"y", m_playerStartY}};
    j["enemyPos"] = {{"x", m_enemyStartX}, {"y", m_enemyStartY}};
    // With extra positions the map is for more than two players: the list the game reads (player, enemy, extras)
    if (m_extraStarts.empty()) {
        j.erase("start_positions");
    } else {
        nlohmann::json list = nlohmann::json::array();
        list.push_back({{"x", m_playerStartX}, {"y", m_playerStartY}});
        list.push_back({{"x", m_enemyStartX}, {"y", m_enemyStartY}});
        for (const auto& p : m_extraStarts) list.push_back({{"x", p.first}, {"y", p.second}});
        j["start_positions"] = std::move(list);
    }

    std::ofstream out(path);
    if (!out.is_open()) {
        std::cerr << "[MapDocument] Failed to write file: " << path << std::endl;
        return false;
    }
    out << j.dump(2);
    m_mapData = j;
    m_filePath = path;
    m_modified = false;
    std::cout << "[MapDocument] Successfully saved map to: " << path << std::endl;
    return true;
}

void MapDocument::setTile(int x, int y, uint8_t tile) {
    if (x < 0 || x >= m_map.getWidth() || y < 0 || y >= m_map.getHeight()) return;
    if (m_map.getBaseTile(x, y) != tile) {
        m_map.setBaseTile(x, y, tile);
        refreshPassability(x, y);
        m_modified = true;
    }
}

// Footprint (width, height in tiles) of a decoration, anchored at its top-left tile
std::pair<int, int> MapDocument::decorationSize(int type) const {
    if (m_textures) {
        const int building = m_textures->getDecorBuildingId(m_map.getTilesetId(), type);
        if (const BuildingDef* def = m_textures->getBuildingDef(building)) {
            return {std::max<int>(1, def->width), std::max<int>(1, def->height)};
        }
    }
    return {1, 1};
}

bool MapDocument::coveredByDecoration(int x, int y) const {
    for (const auto& d : m_map.getDecorations()) {
        if (x < d.x || y < d.y) continue;
        const auto [dw, dh] = decorationSize(d.type);
        if (x < d.x + dw && y < d.y + dh) return true;
    }
    return false;
}

// Passability of a tile follows its sprite; decorations, walls and buildings on it block as well
void MapDocument::refreshPassability(int x, int y) {
    bool open = !TileCollision::isBlocked(m_map.getTilesetId(), m_map.getBaseTile(x, y)) &&
                !m_map.isWall(x, y) && !coveredByDecoration(x, y);
    for (const auto& ent : m_entities) {
        if (open && ent.isBuilding && x >= ent.tileX && x < ent.tileX + ent.buildWidth &&
            y >= ent.tileY && y < ent.tileY + ent.buildHeight) {
            open = false;
        }
    }
    m_map.setPassable(x, y, open);
}

void MapDocument::setTileBrush(int x, int y, uint8_t tile, int brushSize) {
    brushSize = std::clamp(brushSize, 1, 3);
    const int start = -(brushSize - 1) / 2;
    for (int dy = start; dy < start + brushSize; ++dy) {
        for (int dx = start; dx < start + brushSize; ++dx) {
            setTile(x + dx, y + dy, tile);
        }
    }
}

void MapDocument::floodFill(int startX, int startY, uint8_t newTile) {
    int w = m_map.getWidth();
    int h = m_map.getHeight();
    if (startX < 0 || startX >= w || startY < 0 || startY >= h) return;

    uint8_t targetTile = m_map.getBaseTile(startX, startY);
    if (targetTile == newTile) return;

    std::queue<std::pair<int, int>> q;
    q.push({startX, startY});
    m_map.setBaseTile(startX, startY, newTile);
    refreshPassability(startX, startY);
    m_modified = true;

    const int dx[] = {1, -1, 0, 0};
    const int dy[] = {0, 0, 1, -1};

    while (!q.empty()) {
        auto [cx, cy] = q.front();
        q.pop();

        for (int i = 0; i < 4; ++i) {
            int nx = cx + dx[i];
            int ny = cy + dy[i];
            if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
                if (m_map.getBaseTile(nx, ny) == targetTile) {
                    m_map.setBaseTile(nx, ny, newTile);
                    refreshPassability(nx, ny);
                    q.push({nx, ny});
                }
            }
        }
    }
}

void MapDocument::fillAll(uint8_t tile) {
    int w = m_map.getWidth();
    int h = m_map.getHeight();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            m_map.setBaseTile(x, y, tile);
            refreshPassability(x, y);
        }
    }
    m_modified = true;
}

void MapDocument::addDecoration(int type, int x, int y) {
    if (x < 0 || x >= m_map.getWidth() || y < 0 || y >= m_map.getHeight()) return;
    m_map.addDecoration(type, x, y);
    const auto [dw, dh] = decorationSize(type);
    for (int ty = y; ty < y + dh && ty < m_map.getHeight(); ++ty) {
        for (int tx = x; tx < x + dw && tx < m_map.getWidth(); ++tx) m_map.setPassable(tx, ty, false);
    }
    m_modified = true;
}

void MapDocument::removeDecoration(int x, int y) {
    if (x < 0 || x >= m_map.getWidth() || y < 0 || y >= m_map.getHeight()) return;
    int footprintW = 1, footprintH = 1;
    for (const auto& d : m_map.getDecorations()) {
        if (d.x == x && d.y == y) {
            std::tie(footprintW, footprintH) = decorationSize(d.type);
            break;
        }
    }
    m_map.removeDecorationAt(x, y);
    for (int ty = y; ty < y + footprintH && ty < m_map.getHeight(); ++ty) {
        for (int tx = x; tx < x + footprintW && tx < m_map.getWidth(); ++tx) refreshPassability(tx, ty);
    }
    m_modified = true;
}

void MapDocument::setWall(int x, int y, Faction faction, int hp) {
    if (x < 0 || x >= m_map.getWidth() || y < 0 || y >= m_map.getHeight()) return;
    m_map.setWall(x, y, faction, (hp * 100) / 150);
    m_modified = true;
}

bool MapDocument::canPlaceWallBrush(int x, int y, Faction faction, int brushSize) const {
    brushSize = std::clamp(brushSize, 1, 3);
    const int start = -(brushSize - 1) / 2;
    for (int dy = start; dy < start + brushSize; ++dy) {
        for (int dx = start; dx < start + brushSize; ++dx) {
            const int tileX = x + dx;
            const int tileY = y + dy;
            if (tileX < 0 || tileX >= m_map.getWidth() || tileY < 0 || tileY >= m_map.getHeight() ||
                m_map.getDecorGrid(tileX, tileY) > 0 || getEntityAt(tileX, tileY)) {
                return false;
            }
            if (m_map.isWall(tileX, tileY)) {
                if (m_map.getWallFaction(tileX, tileY) != faction) return false;
            } else if (!m_map.isPassable(tileX, tileY)) {
                return false;
            }
        }
    }
    return true;
}

bool MapDocument::setWallBrush(int x, int y, Faction faction, int brushSize) {
    if (!canPlaceWallBrush(x, y, faction, brushSize)) return false;
    brushSize = std::clamp(brushSize, 1, 3);
    const int start = -(brushSize - 1) / 2;
    for (int dy = start; dy < start + brushSize; ++dy) {
        for (int dx = start; dx < start + brushSize; ++dx) {
            setWall(x + dx, y + dy, faction, 150);
        }
    }
    return true;
}

void MapDocument::removeWall(int x, int y) {
    if (x < 0 || x >= m_map.getWidth() || y < 0 || y >= m_map.getHeight()) return;
    m_map.setWall(x, y, Faction::Resistance, 0);
    m_modified = true;
}

int MapDocument::addEntity(int type, bool isBuilding, Faction faction, int x, int y, int facing) {
    if (!canPlaceEntity(type, isBuilding, x, y)) return -1;

    EditorEntity ent;
    ent.id = m_nextEntityId++;
    ent.type = type;
    ent.isBuilding = isBuilding;
    ent.faction = faction;
    ent.tileX = x;
    ent.tileY = y;
    ent.facing = facing;

    if (isBuilding) {
        ent.type = type;
        const auto [width, height] = buildingSize(ent.type);
        ent.buildWidth = width;
        ent.buildHeight = height;
    } else {
        ent.buildWidth = 1;
        ent.buildHeight = 1;
    }

    m_entities.push_back(ent);
    m_modified = true;
    return ent.id;
}

bool MapDocument::hasObjectsInBrush(int x, int y, int brushSize) const {
    brushSize = std::clamp(brushSize, 1, 3);
    const int start = -(brushSize - 1) / 2;
    for (int dy = start; dy < start + brushSize; ++dy) {
        for (int dx = start; dx < start + brushSize; ++dx) {
            const int tileX = x + dx;
            const int tileY = y + dy;
            if (tileX < 0 || tileX >= m_map.getWidth() || tileY < 0 || tileY >= m_map.getHeight()) continue;
            const bool hasObject = getEntityAt(tileX, tileY) != nullptr || m_map.isWall(tileX, tileY) ||
                                   m_map.getDecorGrid(tileX, tileY) > 0;
            if (hasObject) return true;
        }
    }
    return false;
}

bool MapDocument::removeObjectsBrush(int x, int y, int brushSize) {
    if (!hasObjectsInBrush(x, y, brushSize)) return false;
    brushSize = std::clamp(brushSize, 1, 3);
    const int start = -(brushSize - 1) / 2;
    for (int dy = start; dy < start + brushSize; ++dy) {
        for (int dx = start; dx < start + brushSize; ++dx) {
            const int tileX = x + dx;
            const int tileY = y + dy;
            if (tileX < 0 || tileX >= m_map.getWidth() || tileY < 0 || tileY >= m_map.getHeight()) continue;
            const bool hasObject = getEntityAt(tileX, tileY) != nullptr || m_map.isWall(tileX, tileY) ||
                                   m_map.getDecorGrid(tileX, tileY) > 0;
            if (!hasObject) continue;
            removeEntityAt(tileX, tileY);
            removeWall(tileX, tileY);
            removeDecoration(tileX, tileY);
        }
    }
    return true;
}

bool MapDocument::canPlaceEntity(int type, bool isBuilding, int x, int y) const {
    const auto [width, height] = isBuilding ? buildingSize(type) : std::pair<int, int>{1, 1};
    if (x < 0 || y < 0 || x + width > m_map.getWidth() || y + height > m_map.getHeight()) return false;

    for (int tileY = y; tileY < y + height; ++tileY) {
        for (int tileX = x; tileX < x + width; ++tileX) {
            if (!m_map.isPassable(tileX, tileY) || m_map.isWall(tileX, tileY) ||
                m_map.getDecorGrid(tileX, tileY) > 0 || getEntityAt(tileX, tileY)) {
                return false;
            }
        }
    }
    return true;
}

bool MapDocument::selectEntityAt(int x, int y, bool additive) {
    EditorEntity* target = getEntityAt(x, y);
    if (!additive) {
        clearEntitySelection();
    }
    if (!target) return false;
    target->selected = additive ? !target->selected : true;
    return true;
}

void MapDocument::clearEntitySelection() {
    for (auto& entity : m_entities) {
        entity.selected = false;
    }
}

int MapDocument::countSelectedUnits() const {
    return static_cast<int>(std::count_if(m_entities.begin(), m_entities.end(),
        [](const EditorEntity& entity) { return entity.selected && !entity.isBuilding; }));
}

int MapDocument::setSelectedUnitFacing(int facing) {
    facing = (facing % 8 + 8) % 8;
    int changed = 0;
    for (auto& entity : m_entities) {
        if (entity.selected && !entity.isBuilding && entity.facing != facing) {
            entity.facing = facing;
            entity.mapData["facing"] = facing;
            changed++;
        }
    }
    if (changed > 0) m_modified = true;
    return changed;
}

void MapDocument::removeEntityAt(int x, int y) {
    for (auto it = m_entities.begin(); it != m_entities.end(); ) {
        if (it->isBuilding) {
            if (x >= it->tileX && x < it->tileX + it->buildWidth &&
                y >= it->tileY && y < it->tileY + it->buildHeight) {
                it = m_entities.erase(it);
                m_modified = true;
                continue;
            }
        } else {
            if (it->tileX == x && it->tileY == y) {
                it = m_entities.erase(it);
                m_modified = true;
                continue;
            }
        }
        ++it;
    }
}

const EditorEntity* MapDocument::getEntityAt(int x, int y) const {
    for (const auto& e : m_entities) {
        if (e.isBuilding) {
            if (x >= e.tileX && x < e.tileX + e.buildWidth &&
                y >= e.tileY && y < e.tileY + e.buildHeight) {
                return &e;
            }
        } else {
            if (e.tileX == x && e.tileY == y) {
                return &e;
            }
        }
    }
    return nullptr;
}

EditorEntity* MapDocument::getEntityAt(int x, int y) {
    for (auto& e : m_entities) {
        if (e.isBuilding) {
            if (x >= e.tileX && x < e.tileX + e.buildWidth &&
                y >= e.tileY && y < e.tileY + e.buildHeight) {
                return &e;
            }
        } else {
            if (e.tileX == x && e.tileY == y) {
                return &e;
            }
        }
    }
    return nullptr;
}

void MapDocument::setPassable(int x, int y, bool passable) {
    if (x < 0 || x >= m_map.getWidth() || y < 0 || y >= m_map.getHeight()) return;
    m_map.setPassable(x, y, passable);
    m_modified = true;
}

void MapDocument::togglePassable(int x, int y) {
    if (x < 0 || x >= m_map.getWidth() || y < 0 || y >= m_map.getHeight()) return;
    m_map.setPassable(x, y, !m_map.isPassable(x, y));
    m_modified = true;
}

void MapDocument::autoRecalculatePassability() {
    int w = m_map.getWidth();
    int h = m_map.getHeight();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t t = m_map.getBaseTile(x, y);
            bool passable = !TileCollision::isBlocked(m_map.getTilesetId(), t);
            if (m_map.getWall(x, y) > 0) passable = false;
            m_map.setPassable(x, y, passable);
        }
    }

    // A decoration blocks its whole footprint, not only the anchor tile
    for (const auto& d : m_map.getDecorations()) {
        const auto [dw, dh] = decorationSize(d.type);
        for (int dy = d.y; dy < d.y + dh && dy < h; ++dy) {
            for (int dx = d.x; dx < d.x + dw && dx < w; ++dx) m_map.setPassable(dx, dy, false);
        }
    }

    for (const auto& ent : m_entities) {
        if (ent.isBuilding) {
            for (int by = ent.tileY; by < ent.tileY + ent.buildHeight && by < h; ++by) {
                for (int bx = ent.tileX; bx < ent.tileX + ent.buildWidth && bx < w; ++bx) {
                    m_map.setPassable(bx, by, false);
                }
            }
        }
    }
    m_modified = true;
}

void MapDocument::clearAll() {
    newMap(m_map.getWidth(), m_map.getHeight(), m_map.getTilesetId(), 0);
}

MapSnapshot MapDocument::createSnapshot() const {
    MapSnapshot snap;
    snap.width = m_map.getWidth();
    snap.height = m_map.getHeight();
    snap.tilesetId = m_map.getTilesetId();

    int total = snap.width * snap.height;
    snap.baseTiles.resize(total);
    snap.wallGrid.resize(total);
    snap.wallHp.resize(total);
    snap.passability.resize(total);

    for (int y = 0; y < snap.height; ++y) {
        for (int x = 0; x < snap.width; ++x) {
            int idx = y * snap.width + x;
            snap.baseTiles[idx] = m_map.getBaseTile(x, y);
            snap.wallGrid[idx] = m_map.getWall(x, y);
            snap.wallHp[idx] = static_cast<int16_t>(m_map.getWallHp(x, y));
            snap.passability[idx] = m_map.isPassable(x, y);
        }
    }

    snap.decorations = m_map.getDecorations();
    snap.entities = m_entities;
    snap.playerStartX = m_playerStartX;
    snap.playerStartY = m_playerStartY;
    snap.enemyStartX = m_enemyStartX;
    snap.enemyStartY = m_enemyStartY;
    snap.extraStarts = m_extraStarts;
    return snap;
}

void MapDocument::restoreSnapshot(const MapSnapshot& snap) {
    m_map.setSize(snap.width, snap.height);
    m_map.setTilesetId(snap.tilesetId);
    m_map.clearDecorations();

    for (int y = 0; y < snap.height; ++y) {
        for (int x = 0; x < snap.width; ++x) {
            int idx = y * snap.width + x;
            m_map.setBaseTile(x, y, snap.baseTiles[idx]);
            m_map.setPassable(x, y, snap.passability[idx]);
            if (snap.wallGrid[idx] > 0) {
                Faction fac = (snap.wallGrid[idx] >= 1101) ? Faction::Resistance : Faction::Confederation;
                m_map.setWall(x, y, fac, (snap.wallHp[idx] * 100) / 150);
            } else {
                m_map.setWall(x, y, Faction::Resistance, 0);
            }
        }
    }

    for (const auto& d : snap.decorations) {
        m_map.addDecoration(d.type, d.x, d.y);
    }

    m_entities = snap.entities;
    m_playerStartX = snap.playerStartX;
    m_playerStartY = snap.playerStartY;
    m_enemyStartX = snap.enemyStartX;
    m_enemyStartY = snap.enemyStartY;
    m_extraStarts = snap.extraStarts;
    m_modified = true;
}

void MapDocument::pushUndoSnapshot() {
    m_undoStack.push_back(createSnapshot());
    if (m_undoStack.size() > MAX_UNDO_LEVELS) {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_redoStack.clear();
}

bool MapDocument::undo() {
    if (m_undoStack.empty()) return false;
    m_redoStack.push_back(createSnapshot());
    restoreSnapshot(m_undoStack.back());
    m_undoStack.pop_back();
    return true;
}

bool MapDocument::redo() {
    if (m_redoStack.empty()) return false;
    m_undoStack.push_back(createSnapshot());
    restoreSnapshot(m_redoStack.back());
    m_redoStack.pop_back();
    return true;
}

} // namespace OpenArtOfWar
