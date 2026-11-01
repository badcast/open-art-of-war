#pragma once

#include "game/Entity.hpp"
#include <algorithm>
#include <deque>
#include <functional>

namespace OpenArtOfWar
{

class TileMap;

/**
 * @brief Delegates invoked by Building to interact with EntityManager and the world.
 */
struct BuildingDelegates
{
    std::function<Entity *(int id)> getEntity;
    std::function<void(
        Entity &shooter, float srcX, float srcY, float dstX, float dstY, int targetEntityId, TileMap &map)>
        spawnProjectile;
    std::function<void(float x, float y, int animId)> spawnExplosion;
    std::function<void(PlayerId victim, PlayerId killer)> onKill;
    std::function<bool(const Entity &entity, PlayerId observer)> isEntityVisible;
    std::function<int(const Building &b)> findTarget;
};

// One unit ordered at a Barracks / Factory
struct ProductionItem
{
    int archetype {0};
    int cost {0};
    float totalTime {1.0f};
    float progressTime {0.0f};
};

/**
 * @brief Static entity: occupies tiles, builds units, gives resources, provides defense.
 */
class Building : public Entity
{
public:
    int rawBuildingType {-1}; // 0..14 if building, 15..29 if wall
    int buildWidth {1};
    int buildHeight {1};
    float buildProgress {1.0f}; // 0..1
    bool isPowered {true};
    bool isProducing {false}; // Active production animation in Factory / Barracks
    // Drawing only (not part of the world checksum): the owner researches right now, the Tech Center / Laboratory
    // animates
    bool isResearching {false};
    float productionTimer {0.0f};
    float muzzleOffsetX {0.0f}; // localePosition X
    float muzzleOffsetY {0.0f}; // localePosition Y
    float turretOffsetX {0.0f}; // turret_offset / dome_offset X
    float turretOffsetY {0.0f}; // turret_offset / dome_offset Y
    bool hasCustomTurretOffset {false};

    // Unit production (Barracks / Factory): every building trains its own queue
    std::deque<ProductionItem> productionQueue;
    int wreckComposite {0}; // 0: rubble by footprint size
    int wreckStages {0};
    int rallyTileX {-1}; // where fresh units walk to; -1 = stay at the exit
    int rallyTileY {-1};

    bool HasRallyPoint() const
    {
        return rallyTileX >= 0 && rallyTileY >= 0;
    }
    bool IsUnitProducer() const
    {
        return type == UnitType::Barracks || type == UnitType::Factory;
    }
    float CurrentProductionProgress() const
    {
        if (productionQueue.empty())
            return 0.0f;
        const auto &item = productionQueue.front();
        return item.totalTime > 0.0f ? std::min(1.0f, item.progressTime / item.totalTime) : 1.0f;
    }

    Building();
    ~Building() override = default;

    bool IsBuildingEntity() const override
    {
        return true;
    }
    Building *AsBuilding() override
    {
        return this;
    }
    const Building *AsBuilding() const override
    {
        return this;
    }

    float GetUnitCenterPixelX() const override
    {
        return (tileX + buildWidth * 0.5f) * TILE_WIDTH;
    }
    float GetUnitCenterPixelY() const override
    {
        return (tileY + buildHeight * 0.5f) * TILE_HEIGHT;
    }

    void GetMuzzlePoint(float &outX, float &outY) const override;
    int GetPowerOutput() const;

    void Update(float deltaTime, TileMap &map, const BuildingDelegates &delegates);
    void UpdateConstruction(float deltaTime);
    void UpdateProduction(float deltaTime);
    void UpdateDefenseCombat(float deltaTime, TileMap &map, const BuildingDelegates &delegates);
};

} // namespace OpenArtOfWar
