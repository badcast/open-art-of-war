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
// Most orders one Barracks / Factory holds at once
constexpr size_t kMaxProductionQueue = 99;

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
    int burstCount {1};         // shots of one volley
    float burstDelay {0.1f};    // seconds between them
    int burstShotsRemaining {0};
    float burstTimer {0.0f};
    int radarRange {0};         // tiles seen beyond sightRadius once finished (Radar Locator)
    float buildProgress {1.0f}; // 0..1
    bool isPowered {true};
    bool isProducing {false}; // Active production animation in Factory / Barracks
    // Drawing only (not part of the world checksum): the owner researches right now, the Tech Center / Laboratory
    // animates
    bool isResearching {false};
    float productionTimer {0.0f};
    // Repair for credits: a full repair from 0 takes kRepairSeconds and costs kRepairCostShare of the price,
    // paid as it goes; it stops when the hit points are full or the owner runs out of credits
    static constexpr float kRepairSeconds = 180.0f;
    static constexpr float kRepairCostShare = 0.30f;
    bool repairing {false};
    int repeatArchetype {-1}; // a unit this producer trains again and again (-1: off)
    float repairHpAccum {0.0f};    // fractions of a hit point healed so far
    float repairCreditDebt {0.0f}; // fractions of a credit owed so far
    // One repair step: heals, returns the credits it costs (never more than `budget`); stops by itself
    int RepairStep(float dt, int buildingCost, int budget);
    bool CanBeRepaired() const noexcept
    {
        return alive && state != ActionState::Die && buildProgress >= 1.0f && currentHp < maxHp && maxHp > 0;
    }
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
    // Power it gives (> 0) or takes (< 0): production.power of its character file, the same number the build menu
    // shows (set when it is placed)
    int GetPowerOutput() const;
    int powerOutput {0};
    // The power of a building kind without a character file (the original game's numbers)
    static int DefaultPowerOutput(int rawBuildingType);

    void Update(float deltaTime, TileMap &map, const BuildingDelegates &delegates);
    void UpdateConstruction(float deltaTime);
    void UpdateProduction(float deltaTime);
    void UpdateDefenseCombat(float deltaTime, TileMap &map, const BuildingDelegates &delegates);
};

} // namespace OpenArtOfWar
