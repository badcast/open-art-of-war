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
        static std::vector<StormcastSlot> computeGroupSlots(int targetTileX, int targetTileY, int count, const TileMap &map, const EntityManager *entities, const std::vector<int> &movingUnitIds);
    };

    class Pathfinder
    {
    public:
        explicit Pathfinder(const TileMap *map = nullptr);
        ~Pathfinder();

        void setMap(const TileMap *map);
        const TileMap *getMap() const
        {
            return m_map;
        }
        void setEntityManager(const EntityManager *entities)
        {
            m_entities = entities;
        }
        void syncMap(int ignoreEntityId = 0, bool lockUnits = true);

        bool findPath(int startX, int startY, int targetX, int targetY, std::vector<PathNode> &outPath, int ignoreEntityId = 0, bool *outIsPartial = nullptr, bool allowDiagonal = true);

        // Tiles treated as blocked for the NEXT findPath call only (e.g. a friendly unit we must walk around).
        void setAvoidTiles(std::vector<PathNode> tiles)
        {
            m_avoidTiles = std::move(tiles);
        }

        // Next findPath call plans over terrain, walls and buildings only (strategic routes for whole groups)
        void setIgnoreUnitsOnce()
        {
            m_ignoreUnitsOnce = true;
        }

    private:
        void ensureBrainMapSize(int width, int height);

        const TileMap *m_map {nullptr};
        const EntityManager *m_entities {nullptr};
        std::unique_ptr<brain::brain_map> m_brainMap;
        int m_lastWidth {0};
        int m_lastHeight {0};
        std::vector<PathNode> m_avoidTiles;
        bool m_ignoreUnitsOnce {false};
    };

} // namespace OpenArtOfWar
