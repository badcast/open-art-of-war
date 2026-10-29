#pragma once

#include "core/Config.hpp"
#include "game/Players.hpp"

namespace OpenArtOfWar
{

    class EntityManager;
    class TileMap;
    class Pathfinder;
    class Building;

    // Where new units of a Barracks / Factory gather: the rally point set by the player, otherwise
    // a couple of tiles in front of the exit so the doorway stays clear
    void rallyPointOf(const Building &b, const TileMap &map, int &outTileX, int &outTileY);

    /**
     * @brief Player economy: credits, power balance and per-building unit production.
     */
    class Economy
    {
    public:
        Economy();

        // `player`: whose economy it is (single player: the local one)
        void reset(int startingCredits = 1000);
        void setPlayer(PlayerId player)
        {
            m_player = player;
        }
        PlayerId player() const
        {
            return m_player;
        }
        void update(float deltaTime, EntityManager &entities, const TileMap &map, Pathfinder &pathfinder);

        bool canAfford(int cost) const
        {
            return m_credits >= cost;
        }
        void addCredits(int amount)
        {
            m_credits += amount;
        }
        void setCredits(int credits)
        {
            m_credits = credits;
        }
        bool spendCredits(int amount);

        // Pays and appends a unit (by archetype) to the queue of that Barracks / Factory
        bool queueUnit(int archetype, int cost, float buildTime, int buildingId, EntityManager &entities);
        // Removes the last queued unit of that archetype from the building and refunds it
        bool cancelUnit(int archetype, int buildingId, EntityManager &entities);

        int getCredits() const
        {
            return m_credits;
        }
        int getPowerProduced() const
        {
            return m_powerProduced;
        }
        int getPowerConsumed() const
        {
            return m_powerConsumed;
        }
        bool hasPowerDeficit() const
        {
            return m_powerConsumed > m_powerProduced;
        }

    private:
        PlayerId m_player {0};
        int m_credits {1000};
        int m_powerProduced {100};
        int m_powerConsumed {50};
        float m_incomeTimer {0.0f};
        int m_seenKills {0};
        static constexpr int kKillReward = 10; // credits for a destroyed enemy unit
    };

} // namespace OpenArtOfWar
