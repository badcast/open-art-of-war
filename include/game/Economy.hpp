#pragma once

#include "core/Config.hpp"
#include "game/Players.hpp"
#include <vector>

namespace OpenArtOfWar
{

class EntityManager;
class TileMap;
class Pathfinder;
class Building;

// Where new units of a Barracks / Factory gather: the rally point set by the player, otherwise
// a couple of tiles in front of the exit so the doorway stays clear
void RallyPointOf(const Building &b, const TileMap &map, int &outTileX, int &outTileY);

/**
 * @brief Player economy: credits, power balance and per-building unit production.
 */
class Economy
{
public:
    static constexpr int kMaxIncomeHeadquarters = 3; // every finished headquarters up to this many brings income
    Economy();

    // `player`: whose economy it is (single player: the local one)
    void Reset(int startingCredits = 1000);
    void SetPlayer(PlayerId player)
    {
        m_player = player;
    }
    PlayerId Player() const
    {
        return m_player;
    }
    inline PlayerId player() const
    {
        return Player();
    }
    void Update(float deltaTime, EntityManager &entities, const TileMap &map, Pathfinder &pathfinder);

    // Advances the production queues of the player's Barracks / Factories by one tick and creates the units that are
    // done (at the exit, sent to the rally point, with the player's technologies). Returns their ids; `refunded` gets
    // the price of the orders of buildings that were destroyed. `setPower`: the building's power switch is set from
    // `powerOk` (a person's economy); otherwise each building's own switch decides (a computer player).
    static std::vector<int> AdvanceQueues(
        PlayerId player,
        float deltaTime,
        EntityManager &entities,
        const TileMap &map,
        Pathfinder &pathfinder,
        bool setPower,
        bool powerOk,
        int &refunded);

    bool CanAfford(int cost) const
    {
        return m_credits >= cost;
    }
    void AddCredits(int amount)
    {
        m_credits += amount;
    }
    void SetCredits(int credits)
    {
        m_credits = credits;
    }
    // Replicas only: the power balance as the authority computed it
    void SetPower(int produced, int consumed)
    {
        m_powerProduced = produced;
        m_powerConsumed = consumed;
    }
    bool SpendCredits(int amount);

    // Pays and appends a unit (by archetype) to the queue of that Barracks / Factory
    bool QueueUnit(int archetype, int cost, float buildTime, int buildingId, EntityManager &entities);
    // Removes the last queued unit of that archetype from the building and refunds it
    bool CancelUnit(int archetype, int buildingId, EntityManager &entities);

    int GetCredits() const
    {
        return m_credits;
    }
    int GetPowerProduced() const
    {
        return m_powerProduced;
    }
    int GetPowerConsumed() const
    {
        return m_powerConsumed;
    }
    bool HasPowerDeficit() const
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
