#include "game/EntityManager.hpp"
#include "EntityManagerInternal.hpp"
#include "graphics/SpriteRegistry.hpp"
#include "game/Mines.hpp"
#include "game/Projectiles.hpp"
#include "game/WeaponBalance.hpp"
#include "core/DetMath.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/Localization.hpp"
#include "core/Utility.hpp"
#include "game/Effects.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "game/TileMap.hpp"
#include "graphics/TextureManager.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <yaml-cpp/yaml.h>

namespace OpenArtOfWar
{

std::vector<BuildingBlueprint> EntityManager::s_blueprints;
std::vector<BuildingBlueprint> EntityManager::s_factionBlueprints[2];

std::string EntityManager::DescriptionKey(const std::string &characterId)
{
    return characterId + ".description";
}

int EntityManager::ApplyMovingUnitDamage(int baseDamage, const Entity *target, ProjectileType kind)
{
    if (!target || baseDamage <= 0)
        return baseDamage;
    baseDamage = WeaponBalance::Scale(baseDamage, kind, WeaponBalance::ClassOf(*target));
    const Unit *u = target->AsUnit();
    // A unit on the move is harder to hit: it takes a random share of the damage (weapon_balance.json: moving_target),
    // from every kind of damage (direct hit, splash, flame)
    if (u
        && (u->state == ActionState::Move || (!u->path.empty() && u->pathIndex < u->path.size() && !u->waitingForPath)))
    {
        int lo = 50, hi = 100;
        WeaponBalance::MovingTargetPercent(lo, hi);
        const std::uint32_t roll = static_cast<std::uint32_t>(Utility::Random(lo, hi));
        int finalDamage = static_cast<int>((static_cast<std::uint64_t>(baseDamage) * roll + 50u) / 100u);
        return ReduceDamageByArmor((finalDamage < 1) ? 1 : finalDamage, *target);
    }
    return ReduceDamageByArmor(baseDamage, *target);
}

// Infantry heals by itself; vehicles are repaired close to their faction's Factory. The technologies of the
// faction make both 3 times faster.
void EntityManager::UpdateRegeneration(float deltaTime)
{
    constexpr float kInfantryRegen = 0.3f; // HP per second
    constexpr float kRepair = 1.0f;        // HP per second near a Factory
    constexpr float kRepairRadius = 5.0f;  // tiles
    std::vector<const Building *> factories;
    for (const auto &e : m_entities)
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->state != ActionState::Die && b->type == UnitType::Factory && b->buildProgress >= 1.0f)
        {
            factories.push_back(b);
        }
    }
    for (auto &e : m_entities)
    {
        Unit *u = e.AsUnit();
        if (!u || !u->alive || u->state == ActionState::Die || u->currentHp >= u->maxHp)
            continue;
        float rate = 0.0f;
        if (u->IsInfantry())
        {
            rate = kInfantryRegen * (IsEffectResearched(u->owner, ResearchEffect::InfantryRegen) ? 3.0f : 1.0f);
            if (u->state != ActionState::Idle)
                rate *= 0.5f; // walking wounded recover slower
        }
        else
        {
            for (const Building *f : factories)
            {
                if (f->owner != u->owner)
                    continue;
                if (DetMath::Hypot(static_cast<float>(f->tileX - u->tileX), static_cast<float>(f->tileY - u->tileY))
                    <= kRepairRadius)
                {
                    rate = kRepair * (IsEffectResearched(u->owner, ResearchEffect::RepairSpeed) ? 3.0f : 1.0f);
                    break;
                }
            }
        }
        if (rate <= 0.0f)
            continue;
        u->regenAccum += rate * deltaTime;
        if (u->regenAccum >= 1.0f)
        {
            const int heal = static_cast<int>(u->regenAccum);
            u->regenAccum -= static_cast<float>(heal);
            u->currentHp = std::min(u->maxHp, u->currentHp + heal);
        }
    }
}

EntityManager::EntityManager()
{
    m_entities.Reserve(MAX_UNITS);
    LoadCharacterConfigs();
}

void EntityManager::RebuildBlueprints()
{
    // Blueprints of the player's faction in canonical order: HQ, power, barracks, factory, lab, bunker, (Confederation:
    // radar locator), tower, wall Both factions: commands of other players are looked up in their own faction's list
    for (int side = 0; side < 2; ++side)
    {
        std::vector<BuildingBlueprint> &list = s_factionBlueprints[side];
        list.clear();
        const std::vector<int> bpOrder = side == 0 ? std::vector<int> {0, 1, 2, 3, 4, 5, 6, 7, 15}
                                                   : std::vector<int> {8, 9, 10, 11, 12, 13, 14, 25};
        for (std::size_t slot = 0; slot < bpOrder.size(); ++slot)
        {
            const int bIdx = bpOrder[slot];
            auto it = m_buildingConfigs.find(bIdx);
            if (it != m_buildingConfigs.end())
            {
                const auto &cfg = it->second;
                BuildingBlueprint bp;
                bp.blueprintId = static_cast<int>(slot);
                bp.buildingIndex = cfg.buildingIndex;
                bp.iconCompId = cfg.iconCompId;
                bp.isWall = (bIdx == 15 || bIdx == 25);
                bp.unitType = cfg.type;
                bp.name = cfg.name;
                bp.width = cfg.width;
                bp.height = cfg.height;
                bp.creditCost = cfg.creditCost;
                bp.powerCost = cfg.powerCost;
                bp.desc = cfg.description;
                list.push_back(bp);
            }
        }
    }
    // The Resistance's mines come after its walls: one blueprint per kind, laid like a wall segment
    {
        std::vector<BuildingBlueprint> &list = s_factionBlueprints[1];
        const std::vector<MineSpec> &mines = Mines::Specs();
        const int firstSlot = 8; // after the eight buildings and walls of its list
        for (std::size_t k = 0; k < mines.size(); ++k)
        {
            BuildingBlueprint bp;
            bp.blueprintId = firstSlot + static_cast<int>(k);
            bp.buildingIndex = 40 + static_cast<int>(k); // no building has it
            bp.iconCompId = mines[k].iconComp;
            bp.unitType = UnitType::Headquarters;
            bp.name = mines[k].name;
            bp.width = bp.height = 1;
            bp.creditCost = mines[k].cost;
            bp.desc = mines[k].desc;
            bp.mineKind = static_cast<int>(k);
            list.push_back(bp);
        }
    }
    s_blueprints = s_factionBlueprints[PlayerFaction() == Faction::Confederation ? 0 : 1];
}

const std::vector<BuildingBlueprint> &EntityManager::GetBlueprints(Faction faction)
{
    const std::vector<BuildingBlueprint> &list = s_factionBlueprints[faction == Faction::Confederation ? 0 : 1];
    return list.empty() ? GetBlueprints() : list;
}

const BuildingBlueprint *EntityManager::FindBlueprint(Faction faction, int blueprintId)
{
    for (const BuildingBlueprint &bp : GetBlueprints(faction))
    {
        if (bp.blueprintId == blueprintId)
            return &bp;
    }
    return nullptr;
}

const UnitConfig *EntityManager::GetUnitConfig(int archetypeId) const
{
    auto it = m_unitConfigs.find(archetypeId);
    if (it != m_unitConfigs.end())
    {
        return &it->second;
    }
    return nullptr;
}

const BuildingConfig *EntityManager::GetBuildingConfig(int buildingIndex) const
{
    auto it = m_buildingConfigs.find(buildingIndex);
    if (it != m_buildingConfigs.end())
    {
        return &it->second;
    }
    if (buildingIndex >= 15 && buildingIndex <= 24)
    {
        it = m_buildingConfigs.find(15);
        if (it != m_buildingConfigs.end())
            return &it->second;
    }
    else if (buildingIndex >= 25 && buildingIndex <= 29)
    {
        it = m_buildingConfigs.find(25);
        if (it != m_buildingConfigs.end())
            return &it->second;
    }
    return nullptr;
}

const UnitConfig *EntityManager::GetUnitConfigByUnitType(UnitType type, Faction faction) const
{
    for (const auto &[arch, cfg] : m_unitConfigs)
    {
        if (cfg.type == type && cfg.faction == faction && !cfg.isSiegeMode)
        {
            return &cfg;
        }
    }
    for (const auto &[arch, cfg] : m_unitConfigs)
    {
        if (cfg.type == type && cfg.faction == faction)
        {
            return &cfg;
        }
    }
    return nullptr;
}

void EntityManager::Clear()
{
    m_entities.Clear();
    m_collectedDeaths.clear();
    m_eventGroundMarks.clear();
    m_replicaVisible.clear();
    m_replicaWorld = false;
    m_seenBuildings.clear();
    m_projectiles.clear();
    m_explosions.clear();
    m_smokePuffs.clear();
    m_flameBursts.clear();
    m_muzzleFlashes.clear();
    m_groundMarks.clear();
    m_mines.clear();
    m_nextMineId = 1;
    m_soundCues.clear();
    m_buildingBlasts.clear();
    m_eliminated = {};
    // Muzzle-flash reveals name entity ids: the next game reuses them from 1, so old pings would show its units
    m_revealPings.clear();
    ++m_generation;
    m_nextId = 1;
    m_kills = {};
    m_stats = {};
    // Research effects belong to the game that is being cleared
    m_buildRadiusExtra = {};
    std::fill(std::begin(m_flamethrowerUnlocked), std::end(m_flamethrowerUnlocked), false);
    std::fill(std::begin(m_siegeUnlocked), std::end(m_siegeUnlocked), false);
}

Entity *EntityManager::GetEntity(int id)
{
    for (auto &e : m_entities)
    {
        if (e.id == id && e.alive && e.state != ActionState::Die)
            return &e;
    }
    return nullptr;
}

const Entity *EntityManager::GetEntity(int id) const
{
    for (const auto &e : m_entities)
    {
        if (e.id == id && e.alive && e.state != ActionState::Die)
            return &e;
    }
    return nullptr;
}

Unit *EntityManager::GetUnit(int id)
{
    Entity *e = GetEntity(id);
    return e ? e->AsUnit() : nullptr;
}

const Unit *EntityManager::GetUnit(int id) const
{
    const Entity *e = GetEntity(id);
    return e ? e->AsUnit() : nullptr;
}

Building *EntityManager::GetBuilding(int id)
{
    Entity *e = GetEntity(id);
    return e ? e->AsBuilding() : nullptr;
}

const Building *EntityManager::GetBuilding(int id) const
{
    const Entity *e = GetEntity(id);
    return e ? e->AsBuilding() : nullptr;
}

bool EntityManager::HasBuildingType(UnitType type, PlayerId owner, bool mustBeComplete) const
{
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner != owner)
            continue;
        const Building *b = e.AsBuilding();
        if (mustBeComplete && b && b->buildProgress < 1.0f)
            continue;
        if (e.type == type)
            return true;
    }
    return false;
}

std::uint16_t EntityManager::InternSound(const std::string &name)
{
    if (name.empty())
        return 0;
    for (std::size_t i = 0; i < m_soundNames.size(); ++i)
        if (m_soundNames[i] == name)
            return static_cast<std::uint16_t>(i + 1);
    if (m_soundNames.size() >= 0xFFFE)
        return 0;
    m_soundNames.push_back(name);
    return static_cast<std::uint16_t>(m_soundNames.size());
}

const WeaponConfig *EntityManager::FiringWeapon(const Entity &e) const
{
    const std::vector<WeaponConfig> *list = nullptr;
    std::size_t slot = 0;
    if (const Unit *u = e.AsUnit())
    {
        if (const UnitConfig *cfg = GetUnitConfig(u->archetypeId))
            list = &cfg->weapons;
        slot = u->firingSecondary ? 1 : 0;
    }
    else if (const Building *b = e.AsBuilding())
    {
        if (const BuildingConfig *cfg = GetBuildingConfig(b->rawBuildingType))
            list = &cfg->weapons;
    }
    return list && slot < list->size() ? &(*list)[slot] : nullptr;
}

const CharacterSounds *EntityManager::SoundsOf(const Entity &e) const
{
    if (const Unit *u = e.AsUnit())
    {
        const UnitConfig *cfg = GetUnitConfig(u->archetypeId);
        return cfg ? &cfg->sounds : nullptr;
    }
    if (const Building *b = e.AsBuilding())
    {
        const BuildingConfig *cfg = GetBuildingConfig(b->rawBuildingType);
        return cfg ? &cfg->sounds : nullptr;
    }
    return nullptr;
}

int EntityManager::BuildingPower(int buildingIndex) const
{
    const BuildingConfig *cfg = GetBuildingConfig(buildingIndex);
    return cfg ? cfg->powerCost : Building::DefaultPowerOutput(buildingIndex);
}

const CharacterSounds *EntityManager::WallSoundsOf(Faction faction) const
{
    const BuildingConfig *cfg = GetBuildingConfig(faction == Faction::Resistance ? 25 : 15);
    return cfg ? &cfg->sounds : nullptr;
}

int EntityManager::CountBuildingType(UnitType type, PlayerId owner, bool mustBeComplete) const
{
    int n = 0;
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner != owner || e.type != type)
            continue;
        const Building *b = e.AsBuilding();
        if (!mustBeComplete || !b || b->buildProgress >= 1.0f)
            ++n;
    }
    return n;
}

std::vector<int> EntityManager::GetBuildingsByType(UnitType type, PlayerId owner, bool mustBeComplete) const
{
    std::vector<int> result;
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || !e.isBuilding || e.owner != owner)
            continue;
        const Building *b = e.AsBuilding();
        if (mustBeComplete && b && b->buildProgress < 1.0f)
            continue;
        if (e.type == type)
            result.push_back(e.id);
    }
    return result;
}

bool EntityManager::GetBuildingSpawnPoint(int buildingId, const TileMap &map, int &outTileX, int &outTileY) const
{
    const Building *b = GetBuilding(buildingId);
    if (!b)
        return false;

    auto isFreeSpawnTile = [&](int x, int y) -> bool {
        if (!map.IsPassable(x, y))
            return false;
        if (map.GetWall(x, y) > 0)
            return false;
        if (IsTileOccupiedByUnit(x, y))
            return false;
        for (const auto &e : m_entities)
        {
            if (!e.alive || e.state == ActionState::Die || !e.isBuilding)
                continue;
            const Building *eb = e.AsBuilding();
            if (eb && x >= eb->tileX && x < eb->tileX + eb->buildWidth && y >= eb->tileY
                && y < eb->tileY + eb->buildHeight)
            {
                return false;
            }
        }
        return true;
    };

    // 1. Bottom side
    for (int x = b->tileX; x < b->tileX + b->buildWidth; ++x)
    {
        int y = b->tileY + b->buildHeight;
        if (isFreeSpawnTile(x, y))
        {
            outTileX = x;
            outTileY = y;
            return true;
        }
    }
    // 2. Right side
    for (int y = b->tileY; y < b->tileY + b->buildHeight; ++y)
    {
        int x = b->tileX + b->buildWidth;
        if (isFreeSpawnTile(x, y))
        {
            outTileX = x;
            outTileY = y;
            return true;
        }
    }
    // 3. Left side
    for (int y = b->tileY; y < b->tileY + b->buildHeight; ++y)
    {
        int x = b->tileX - 1;
        if (isFreeSpawnTile(x, y))
        {
            outTileX = x;
            outTileY = y;
            return true;
        }
    }
    // 4. Top side
    for (int x = b->tileX; x < b->tileX + b->buildWidth; ++x)
    {
        int y = b->tileY - 1;
        if (isFreeSpawnTile(x, y))
        {
            outTileX = x;
            outTileY = y;
            return true;
        }
    }
    // 5. Fallback: search in radius 2..5
    for (int r = 2; r <= 5; ++r)
    {
        for (int dy = -r; dy <= b->buildHeight + r; ++dy)
        {
            for (int dx = -r; dx <= b->buildWidth + r; ++dx)
            {
                int tx = b->tileX + dx;
                int ty = b->tileY + dy;
                if (isFreeSpawnTile(tx, ty))
                {
                    outTileX = tx;
                    outTileY = ty;
                    return true;
                }
            }
        }
    }
    outTileX = b->tileX + b->buildWidth;
    outTileY = b->tileY;
    return true;
}

bool EntityManager::IsTileOccupiedByUnit(int tileX, int tileY, int ignoreEntityId) const
{
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || e.isBuilding)
            continue;
        if (e.id == ignoreEntityId)
            continue;
        if (e.OccupiesTile(tileX, tileY))
            return true;
    }
    return false;
}

int EntityManager::GetUnitIdAtTile(int tileX, int tileY, int ignoreEntityId) const
{
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || e.isBuilding)
            continue;
        if (e.id == ignoreEntityId)
            continue;
        if (e.OccupiesTile(tileX, tileY))
            return e.id;
    }
    return 0;
}

void EntityManager::ResolveUnitStacking(TileMap &map)
{
    std::vector<Unit *> livingUnits;
    for (auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || e.isBuilding)
            continue;
        if (Unit *u = e.AsUnit())
        {
            livingUnits.push_back(u);
        }
    }

    for (std::size_t i = 0; i < livingUnits.size(); ++i)
    {
        Unit *u1 = livingUnits[i];
        for (std::size_t j = i + 1; j < livingUnits.size(); ++j)
        {
            Unit *u2 = livingUnits[j];
            if (u1->tileX == u2->tileX && u1->tileY == u2->tileY)
            {
                // Two units occupy the exact same tile: push u2 to the nearest free, passable, unblocked tile
                static constexpr int offsets[8][2]
                    = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
                for (const auto &off : offsets)
                {
                    int nx = u2->tileX + off[0];
                    int ny = u2->tileY + off[1];
                    if (nx >= 0 && nx < map.GetWidth() && ny >= 0 && ny < map.GetHeight())
                    {
                        if (map.IsPassable(nx, ny) && map.GetWall(nx, ny) == 0 && !IsTileOccupiedByUnit(nx, ny, u2->id))
                        {
                            u2->tileX = nx;
                            u2->tileY = ny;
                            u2->subX = 0.0f;
                            u2->subY = 0.0f;
                            u2->ReleaseReservation();
                            if (u2->state == ActionState::Move)
                            {
                                u2->state = ActionState::Idle;
                                u2->path.clear();
                                u2->waitingForPath = true;
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
}

const std::vector<BuildingBlueprint> &EntityManager::GetBlueprints()
{
    if (!s_blueprints.empty())
    {
        return s_blueprints;
    }
    static const std::vector<BuildingBlueprint> defaultBlueprints
        = {{0,
            8,
            2074,
            UnitType::Headquarters,
            "HEADQUARTERS",
            3,
            4,
            800,
            50,
            "Resistance Command Center. Provides radar and logistics."},
           {1,
            9,
            2075,
            UnitType::PowerPlant,
            "POWER PLANT",
            3,
            3,
            300,
            100,
            "Power Plant (+100 kW). Supplies energy to factories and defenses."},
           {2,
            10,
            2076,
            UnitType::Barracks,
            "BARRACKS",
            3,
            3,
            400,
            -20,
            "Infantry Training: Rifleman, Grenadier, Sniper."},
           {3,
            11,
            2077,
            UnitType::Factory,
            "FACTORY",
            3,
            3,
            600,
            -40,
            "Vehicle Factory: Coyote, Armadillo, Rhino, and Porcupine MLRS."},
           {4,
            12,
            2078,
            UnitType::ResearchLab,
            "RESEARCH LAB",
            3,
            3,
            500,
            -30,
            "Scientific center for combat research upgrades (RESEARCH tab)."},
           {5,
            13,
            2079,
            UnitType::Bunker,
            "BUNKER",
            2,
            2,
            350,
            -15,
            "Reinforced machine-gun bunker for perimeter defense."},
           {6,
            14,
            2080,
            UnitType::Tower,
            "TURRET",
            2,
            2,
            250,
            -10,
            "Watchtower with heavy machine gun and extended vision."},
           {7,
            25,
            2081,
            UnitType::Wall,
            "WALL",
            1,
            1,
            50,
            0,
            "Reinforced concrete wall (1x1) to block enemy armor.",
            true}};
    return defaultBlueprints;
}

void EntityManager::GetUnitsInBox(
    float minWorldX, float minWorldY, float maxWorldX, float maxWorldY, PlayerId owner, std::vector<int> &outUnitIds)
{
    outUnitIds.clear();
    for (auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die || e.isBuilding || e.owner != owner)
            continue;
        float px = e.GetUnitCenterPixelX();
        float py = e.GetUnitCenterPixelY();
        if (px >= minWorldX && px <= maxWorldX && py >= minWorldY && py <= maxWorldY)
        {
            outUnitIds.push_back(e.id);
            e.selected = true;
        }
        else
        {
            e.selected = false;
        }
    }
}

int EntityManager::GetUnitAt(float worldX, float worldY, float tolerance) const
{
    float bestDist = tolerance;
    int bestId = 0;
    for (const auto &e : m_entities)
    {
        if (!e.alive || e.state == ActionState::Die)
            continue;
        if (const Building *b = e.AsBuilding())
        {
            float minX = b->tileX * TILE_WIDTH;
            float maxX = (b->tileX + b->buildWidth) * TILE_WIDTH;
            float minY = b->tileY * TILE_HEIGHT;
            float maxY = (b->tileY + b->buildHeight) * TILE_HEIGHT;
            if (worldX >= minX && worldX <= maxX && worldY >= minY && worldY <= maxY)
            {
                return b->id;
            }
            continue;
        }
        float dx = e.GetUnitCenterPixelX() - worldX;
        float dy = e.GetUnitCenterPixelY() - worldY;
        float d = std::sqrt(dx * dx + dy * dy);
        if (d < bestDist)
        {
            bestDist = d;
            bestId = e.id;
        }
    }
    return bestId;
}

} // namespace OpenArtOfWar
