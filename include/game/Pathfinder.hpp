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
template <typename ISite, typename INeuron>
class basic_flow_field;
typedef basic_flow_field<the_site, the_neuron> flow_field;
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

    // Searches nobody ordered (a unit stuck in a crowd planning a way around, retrying a path that was shut, checking
    // a wall) share a budget per simulation tick: with hundreds of units jammed at a passage each of them would
    // otherwise search the whole map several times a second. A refused search is simply tried again on a later tick.
    // Player orders and the computer's group plans do not use it.
    static constexpr int kBackgroundSearchesPerTick = 8;
    void BeginTick()
    {
        m_backgroundBudget = kBackgroundSearchesPerTick;
    }
    bool TakeBackgroundSearch()
    {
        if (m_backgroundBudget <= 0)
            return false;
        --m_backgroundBudget;
        return true;
    }

    // Group orders: one flow field (a Dijkstra map from the goal over terrain, water, walls and buildings) gives every
    // unit of the group its way to that goal without a search of its own. False when the goal is blocked or outside.
    bool BuildFlowField(int goalX, int goalY);
    // The way from start to dest (a formation slot near the field's goal): the field's road up to a few tiles
    // before dest, then a short search (units counted) for the rest. A start the field does not reach (walled in,
    // another island) gets a plain findPath, partial paths and all. Same output contract as findPath.
    bool FlowPath(
        int startX,
        int startY,
        int destX,
        int destY,
        std::vector<PathNode> &outPath,
        int ignoreEntityId = 0,
        bool *outIsPartial = nullptr);

private:
    void EnsureBrainMapSize(int width, int height);
    // Reorders a staircase of steps into long straight runs (same tiles count, same length, fewer turns)
    bool StraightenPath(int startX, int startY, std::vector<PathNode> &pathVec) const;

    const TileMap *m_map {nullptr};
    const EntityManager *m_entities {nullptr};
    std::unique_ptr<brain::brain_map> m_brainMap;
    std::unique_ptr<brain::flow_field> m_flowField; // the last group order's field (its own copy of the data)
    int m_lastWidth {0};
    int m_lastHeight {0};
    std::vector<PathNode> m_avoidTiles;
    bool m_ignoreUnitsOnce {false};
    int m_backgroundBudget {kBackgroundSearchesPerTick};
};

} // namespace OpenArtOfWar
