#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "core/Utility.hpp"
#include "game/Pathfinder.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "game/TileMap.hpp"
#include <algorithm>
#include <vector>

namespace OpenArtOfWar
{

    void rallyPointOf(const Building &b, const TileMap &map, int &outTileX, int &outTileY)
    {
        if(b.hasRallyPoint())
        {
            outTileX = b.rallyTileX;
            outTileY = b.rallyTileY;
            return;
        }
        const int cx = b.tileX + b.buildWidth / 2;
        const int base = b.tileY + b.buildHeight;
        outTileX = cx;
        outTileY = base;
        for(int d = 2; d >= 0; --d)
        {
            if(map.isPassable(cx, base + d))
            {
                outTileY = base + d;
                return;
            }
        }
    }

    Economy::Economy()
    {
        reset();
    }

    void Economy::reset(int startingCredits)
    {
        m_player = Players::local();
        m_credits = startingCredits;
        m_powerProduced = 100;
        m_powerConsumed = 30;
        m_incomeTimer = 0.0f;
        m_seenKills = 0;
    }

    bool Economy::spendCredits(int amount)
    {
        if(m_credits >= amount)
        {
            m_credits -= amount;
            return true;
        }
        return false;
    }

    bool Economy::queueUnit(int archetype, int cost, float buildTime, int buildingId, EntityManager &entities)
    {
        Building *b = entities.getBuilding(buildingId);
        if(!b || !b->alive || b->state == ActionState::Die || !b->isUnitProducer())
            return false;
        if(!spendCredits(cost))
            return false;
        ProductionItem item;
        item.archetype = archetype;
        item.cost = cost;
        // Training / production technologies shorten the order of the player's side
        const int cut = researchTotal(m_player, Utility::isInfantryArchetype(archetype) ? ResearchEffect::TrainTime : ResearchEffect::VehicleTime);
        item.totalTime = std::max(0.01f, buildTime * (100 - cut) / 100.0f);
        item.progressTime = 0.0f;
        b->productionQueue.push_back(item);
        return true;
    }

    bool Economy::cancelUnit(int archetype, int buildingId, EntityManager &entities)
    {
        Building *b = entities.getBuilding(buildingId);
        if(!b)
            return false;
        for(auto it = b->productionQueue.rbegin(); it != b->productionQueue.rend(); ++it)
        {
            if(it->archetype == archetype)
            {
                m_credits += it->cost;
                b->productionQueue.erase(std::next(it).base());
                return true;
            }
        }
        return false;
    }

    void Economy::update(float deltaTime, EntityManager &entities, const TileMap &map, Pathfinder &pathfinder)
    {
        const PlayerId player = m_player;
        const Faction faction = Players::factionOf(player);

        // 1. Passive income from headquarters: +50 credits every 3 seconds
        m_incomeTimer += deltaTime;
        if(m_incomeTimer >= 3.0f)
        {
            m_incomeTimer = 0.0f;
            m_credits += 50 * (100 + researchTotal(player, ResearchEffect::Income)) / 100;
        }

        // Reward for every enemy unit destroyed since the last tick
        const int kills = entities.getKillCount(player);
        if(kills > m_seenKills)
        {
            m_credits += (kills - m_seenKills) * kKillReward * (100 + researchTotal(player, ResearchEffect::KillReward)) / 100;
        }
        m_seenKills = kills;

        // 2. Count structures for power balance
        int powerProd = 0;
        int powerCons = 0;

        for(const auto &e : entities.getAllEntities())
        {
            const Building *b = e.asBuilding();
            if(!b || !b->alive || b->state == ActionState::Die || b->owner != player)
                continue;
            if(b->buildProgress < 1.0f)
                continue;

            if(b->type == UnitType::Headquarters)
            {
                powerProd += 50;
            }
            else if(b->type == UnitType::PowerPlant)
            {
                powerProd += 100;
            }
            else if(b->type == UnitType::Barracks)
            {
                powerCons += 20;
            }
            else if(b->type == UnitType::Factory)
            {
                powerCons += 40;
            }
            else if(b->type == UnitType::ResearchLab)
            {
                powerCons += 25;
            }
            else if(b->rawBuildingType == 5 || b->rawBuildingType == 13)
            {
                // Concrete machine-gun bunkers are autonomous field fortifications (0 kW power consumption)
            }
            else if(b->type == UnitType::Tower || b->type == UnitType::Bunker)
            {
                powerCons += 25;
            }
        }
        powerProd += powerProd * researchTotal(player, ResearchEffect::PowerBonus) / 100;
        m_powerProduced = powerProd;
        m_powerConsumed = powerCons;

        // 3. Every Barracks / Factory trains its own queue; units appear only at that building's exit
        const bool powerOk = !hasPowerDeficit();
        const float speedMultiplier = powerOk ? 1.0f : 0.5f;
        struct Spawn
        {
            int archetype;
            int x, y;
            int rallyX, rallyY;
        };
        std::vector<Spawn> spawns; // spawning while iterating would invalidate the entity container
        for(auto &e : entities.getAllEntities())
        {
            Building *b = e.asBuilding();
            if(!b || b->owner != player)
                continue;

            if(!b->alive || b->state == ActionState::Die)
            {
                // Destroyed with orders pending: refund what was paid
                for(const auto &item : b->productionQueue)
                    m_credits += item.cost;
                b->productionQueue.clear();
                continue;
            }
            b->isPowered = powerOk;
            if(!b->isUnitProducer())
                continue;
            b->isProducing = !b->productionQueue.empty();
            if(b->productionQueue.empty() || b->buildProgress < 1.0f)
                continue;

            ProductionItem &item = b->productionQueue.front();
            item.progressTime = std::min(item.totalTime, item.progressTime + deltaTime * speedMultiplier);
            if(item.progressTime < item.totalTime)
                continue;

            int sx = 0, sy = 0;
            if(!entities.getBuildingSpawnPoint(b->id, map, sx, sy))
                continue; // exit blocked: retry next tick
            int rx = sx, ry = sy;
            rallyPointOf(*b, map, rx, ry);
            spawns.push_back({item.archetype, sx, sy, rx, ry});
            b->productionQueue.pop_front();
        }

        for(const Spawn &sp : spawns)
        {
            int unitId = entities.spawnUnitByArchetype(sp.archetype, sp.x, sp.y, faction, player);
            if(Unit *u = entities.getUnit(unitId))
                applyCompletedResearch(*u);
            if(unitId > 0 && (sp.rallyX != sp.x || sp.rallyY != sp.y))
            {
                entities.issueMoveOrder({unitId}, sp.rallyX, sp.rallyY, pathfinder);
            }
        }
    }

} // namespace OpenArtOfWar
