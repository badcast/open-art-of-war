#pragma once

#include <vector>
#include <memory>

namespace brain
{
struct the_site;
struct the_neuron;
template <typename ISite, typename INeuron>
class basic_brain_map;
typedef basic_brain_map<the_site, the_neuron> brain_map;
} // namespace brain

namespace OpenArtOfWar
{

class TileMap;
class EntityManager;

struct PathNode
{
    int x {0};
    int y {0};

    bool operator==(const PathNode &other) const
    {
        return x == other.x && y == other.y;
    }
};

struct StormcastSlot
{
    int x {0};
    int y {0};
};

class AINavigataionStormcastView
{
public:
    static std::vector<StormcastSlot> ComputeGroupSlots(
        int targetTileX,
        int targetTileY,
        int count,
        const TileMap &map,
        const EntityManager *entities,
        const std::vector<int> &movingUnitIds);
};

class Pathfinder
{
public:
    explicit Pathfinder(const TileMap *map = nullptr);
    ~Pathfinder();

    void SetMap(const TileMap *map);
    const TileMap *GetMap() const
    {
        return m_map;
    }
    void SetEntityManager(const EntityManager *entities)
    {
        m_entities = entities;
    }
    const EntityManager *GetEntityManager() const
    {
        return m_entities;
    }
    void SyncMap(int ignoreEntityId = 0, bool lockUnits = true);

    bool FindPath(
        int startX,
        int startY,
        int targetX,
        int targetY,
        std::vector<PathNode> &outPath,
        int ignoreEntityId = 0,
        bool *outIsPartial = nullptr,
        bool allowDiagonal = true);

    // Tiles treated as blocked for the NEXT findPath call only (e.g. a friendly unit we must walk around).
    void SetAvoidTiles(std::vector<PathNode> tiles)
    {
        m_avoidTiles = std::move(tiles);
    }

    // Next findPath call plans over terrain, walls and buildings only (strategic routes for whole groups)
    void SetIgnoreUnitsOnce()
    {
        m_ignoreUnitsOnce = true;
    }

private:
    void EnsureBrainMapSize(int width, int height);

    const TileMap *m_map {nullptr};
    const EntityManager *m_entities {nullptr};
    std::unique_ptr<brain::brain_map> m_brainMap;
    int m_lastWidth {0};
    int m_lastHeight {0};
    std::vector<PathNode> m_avoidTiles;
    bool m_ignoreUnitsOnce {false};
};

} // namespace OpenArtOfWar
