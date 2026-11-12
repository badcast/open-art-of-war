#include "graphics/RenderUtility.hpp"
#include "game/AIController.hpp"
#include "AIControllerInternal.hpp"
#include "game/BuildLimits.hpp"
#include "core/DetMath.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/TileMap.hpp"
#include "game/Pathfinder.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <set>

namespace OpenArtOfWar
{

namespace
{
// Tuning
// per run of UpdateBaseDefense (every 2 s): 0.5 ^ (2 s / 180 s), the threat memory halves in 3 minutes
constexpr float kThreatDecay = 0.99233f;
constexpr float kHqZoneTiles = 18.0f;        // an enemy this close to the headquarters is at the base
constexpr float kBuildingZoneTiles = 10.0f;  // ... or this close to any structure of the base
constexpr float kClusterTiles = 6.0f;        // enemies this close together are one attack
constexpr float kDefenderReachTiles = 22.0f; // idle troops this close to an attack go for it
constexpr float kHomeReachTiles = 40.0f;     // ... the computer's own troops standing at the base: this close
constexpr float kShooterZoneTiles = 16.0f;   // a shooter counts when this close to the base zone (artillery range)
// Fortification: the bunkers stand this far (tiles) behind the wall line and take the fire, the towers deeper
constexpr float kBunkerBehindWall = 2.0f;
constexpr float kTowerBehindWall = 5.0f;
constexpr float kBunkerFlankDeg = 22.0f; // ... to both sides of the way the enemy comes
constexpr float kTowerFlankDeg = 34.0f;
constexpr float kDefenseMargin = 1.2f;        // an attack wants this much more worth of defenders than it brings
constexpr float kReaimTiles = 4.0f;           // a defender on its way is sent again when the attack moved this far
constexpr float kAttackMemorySeconds = 10.0f; // the base counts as attacked this long after the last sighting
} // namespace

int AIController::SectorOf(int x, int y) const
{
    const int homeX = m_homeX >= 0 ? m_homeX : m_enemyHqX;
    const int homeY = m_homeY >= 0 ? m_homeY : m_enemyHqY;
    if (x == homeX && y == homeY)
        return 0;
    const float angle = DetMath::Atan2(static_cast<float>(y - homeY), static_cast<float>(x - homeX)); // -pi..pi
    const float turns = angle / (2.0f * static_cast<float>(M_PI)) + 1.0f / 16.0f;                   // centred on 0
    const int sector = static_cast<int>(std::floor(turns * 8.0f));
    return ((sector % 8) + 8) % 8;
}

int AIController::UnitThreatValue(const Unit &unit, const EntityManager &entities) const
{
    // Siege forms count as their mobile form: Sniper 14 -> 4, Rhino 15 -> 10, Grad 16 -> 13
    const int archetypeId
        = unit.archetypeId == 14 ? 4 : unit.archetypeId == 15 ? 10 : unit.archetypeId == 16 ? 13 : unit.archetypeId;
    const UnitConfig *unitConfig = entities.GetUnitConfig(archetypeId);
    return std::max(50, unitConfig ? unitConfig->creditCost : 0);
}

std::vector<const Building *> AIController::OwnStructures(const EntityManager &entities) const
{
    std::vector<const Building *> structures;
    for (const auto &entity : entities.GetAllEntities())
        if (const Building *building = entity.AsBuilding();
            building && building->alive && building->state != ActionState::Die && building->owner == m_player)
            structures.push_back(building);
    return structures;
}

bool AIController::IsNearBase(
    const Entity &entity, const std::vector<const Building *> &ownStructures, float extraTiles) const
{
    const float hqPixelX = m_enemyHqX * TILE_WIDTH + TILE_WIDTH * 0.5f;
    const float hqPixelY = m_enemyHqY * TILE_HEIGHT + TILE_HEIGHT * 0.5f;
    if (DetMath::Hypot(entity.GetUnitCenterPixelX() - hqPixelX, entity.GetUnitCenterPixelY() - hqPixelY)
        <= (kHqZoneTiles + extraTiles) * TILE_WIDTH)
        return true;
    for (const Building *building : ownStructures)
    {
        // tiles from the footprint's edge
        const int deltaX
            = std::max({building->tileX - entity.tileX, 0, entity.tileX - (building->tileX + building->buildWidth - 1)});
        const int deltaY = std::max(
            {building->tileY - entity.tileY, 0, entity.tileY - (building->tileY + building->buildHeight - 1)});
        if (DetMath::Hypot(static_cast<float>(deltaX), static_cast<float>(deltaY)) <= kBuildingZoneTiles + extraTiles)
            return true;
    }
    return false;
}

void AIController::UpdateBaseDefense(EntityManager &entities, Pathfinder &pathfinder)
{
    for (float &threat : m_sectorThreat)
        threat *= kThreatDecay;
    for (float &hostility : m_foeHostility)
        hostility *= kThreatDecay;

    const std::vector<const Building *> ownBuildings = OwnStructures(entities);
    auto atBase = [&](const Entity &entity) {
        return IsNearBase(entity, ownBuildings, 0.0f);
    };

    // Who attacks the base: the enemy troops seen at it, and whoever hit a structure or a unit of the base a moment
    // ago - also from out of sight or from beyond the zone (artillery, snipers, an enemy tower built next door)
    std::vector<int> threatIds;
    for (const auto &entity : entities.GetAllEntities())
    {
        if (!entity.alive || entity.state == ActionState::Die)
            continue;
        if (entity.AsUnit() && IsFoe(entity) && AiSees(entity, entities) && atBase(entity))
        {
            threatIds.push_back(entity.id);
            continue;
        }
        if (entity.owner != m_player || entity.lastAttackedTimer <= 0.0f || entity.lastAttackerId == 0)
            continue;
        if (!entity.isBuilding && (m_squadController.IsSquadMember(entity.id) || !atBase(entity)))
            continue; // (a squad fights its own fights, so does a unit away from the base)
        const Entity *attacker = entities.GetEntity(entity.lastAttackerId);
        if (attacker && attacker->alive && attacker->state != ActionState::Die && IsFoe(*attacker)
            && IsNearBase(*attacker, ownBuildings, kShooterZoneTiles))
            threatIds.push_back(attacker->id);
    }
    std::sort(threatIds.begin(), threatIds.end());
    threatIds.erase(std::unique(threatIds.begin(), threatIds.end()), threatIds.end());

    // Troops chasing an enemy outside the base switch to the offensive and never return to base
    for (auto &entity : entities.GetAllEntities())
    {
        Unit *unit = entity.AsUnit();
        if (!unit || !unit->alive || unit->state == ActionState::Die || unit->owner != m_player
            || unit->targetEntityId == 0)
            continue;
        if (m_squadController.IsSquadMember(unit->id) || m_garrison.count(unit->id))
            continue;
        const bool isDefender = m_defenders.count(unit->id) != 0;
        if (!isDefender && unit->orderedTargetId != unit->targetEntityId)
            continue; // a target the unit took by itself: its stance handles it
        if (std::binary_search(threatIds.begin(), threatIds.end(), unit->targetEntityId))
            continue; // it still attacks the base
        const Entity *target = entities.GetEntity(unit->targetEntityId);
        if (!target || IsNearBase(*target, ownBuildings, kBaseChaseTiles))
            continue;
        m_defenders.erase(unit->id);
        m_waveRoster.erase(unit->id);
        m_offensive.insert(unit->id);
    }

    // Enemy troops at the base, gathered into attacks (several attacks from several sides are told apart)
    struct Attack
    {
        float x {0.0f}, y {0.0f}; // centre, tiles
        float strength {0.0f};
        float assigned {0.0f};
        int count {0};
        int firstId {0};
        std::vector<int> defenders;
    };
    std::vector<Attack> attacks;
    for (int threatId : threatIds)
    {
        const Entity *entity = entities.GetEntity(threatId);
        if (!entity)
            continue;
        float value = 0.0f;
        if (const Unit *unit = entity->AsUnit())
            value = static_cast<float>(UnitThreatValue(*unit, entities));
        else if (const Building *building = entity->AsBuilding())
        {
            const BuildingConfig *buildingConfig = entities.GetBuildingConfig(building->rawBuildingType);
            value = static_cast<float>(std::max(50, buildingConfig ? buildingConfig->creditCost : 0));
        }
        m_sectorThreat[SectorOf(entity->tileX, entity->tileY)] += value;
        if (entity->owner < kMaxPlayers)
            m_foeHostility[entity->owner] += value;
        const float tileX = static_cast<float>(entity->tileX);
        const float tileY = static_cast<float>(entity->tileY);
        Attack *matchedAttack = nullptr;
        for (Attack &attack : attacks)
        {
            if (DetMath::Hypot(attack.x - tileX, attack.y - tileY) <= kClusterTiles)
            {
                matchedAttack = &attack;
                break;
            }
        }
        if (!matchedAttack)
        {
            attacks.push_back({});
            matchedAttack = &attacks.back();
            matchedAttack->firstId = threatId;
        }
        matchedAttack->x = (matchedAttack->x * matchedAttack->count + tileX) / static_cast<float>(matchedAttack->count + 1);
        matchedAttack->y = (matchedAttack->y * matchedAttack->count + tileY) / static_cast<float>(matchedAttack->count + 1);
        ++matchedAttack->count;
        matchedAttack->strength += value;
    }

    // Defenders that died or joined a squad are none any more; with the base quiet again all of them are released
    for (auto it = m_defenders.begin(); it != m_defenders.end();)
    {
        const Unit *unit = entities.GetUnit(it->first);
        if (attacks.empty() || !unit || !unit->alive || unit->state == ActionState::Die || unit->owner != m_player
            || m_squadController.IsSquadMember(it->first) || it->first == m_scoutId)
        {
            if (unit && unit->alive && unit->state != ActionState::Die && unit->owner == m_player
                && !m_squadController.IsSquadMember(it->first) && !m_garrison.count(it->first) && !atBase(*unit))
            {
                m_waveRoster.erase(it->first);
                m_offensive.insert(it->first);
            }
            it = m_defenders.erase(it);
        }
        else
            ++it;
    }
    if (attacks.empty())
    {
        m_lastThreatId = 0;
        return;
    }
    m_baseQuietSeconds = 0.0f;                  // no new headquarters while the base is under attack
    m_baseAttackedTimer = kAttackMemorySeconds; // no wave leaves either (UpdateWaveLaunch)

    const Attack *strongest = &attacks.front();
    for (const Attack &attack : attacks)
        if (attack.strength > strongest->strength)
            strongest = &attack;
    m_lastThreatX = static_cast<int>(std::lround(strongest->x));
    m_lastThreatY = static_cast<int>(std::lround(strongest->y));
    if (strongest->firstId != m_lastThreatId)
    {
        m_lastThreatId = strongest->firstId;
        std::ostringstream msg;
        msg << attacks.size() << (attacks.size() == 1 ? " attack" : " attacks") << " on the base, the strongest near ("
            << m_lastThreatX << ", " << m_lastThreatY << "): the troops at home split between them";
        Think("DEFENSE", msg.str());
    }

    auto nearestAttack = [&](float x, float y, float reach) {
        Attack *best = nullptr;
        float bestDist = reach;
        for (Attack &attack : attacks)
        {
            const float dist = DetMath::Hypot(x - attack.x, y - attack.y);
            if (dist <= bestDist)
            {
                bestDist = dist;
                best = &attack;
            }
        }
        return best;
    };
    // The towers and bunkers that reach an attack count as its defenders
    for (const Building *building : ownBuildings)
    {
        if (building->attackDamage <= 0 || building->attackRange <= 0.0f || building->buildProgress < 1.0f)
            continue;
        if (Attack *attack = nearestAttack(
                static_cast<float>(building->tileX), static_cast<float>(building->tileY), building->attackRange + 2.0f))
        {
            const BuildingConfig *buildingConfig = entities.GetBuildingConfig(building->rawBuildingType);
            attack->assigned += static_cast<float>(std::max(50, buildingConfig ? buildingConfig->creditCost : 0));
        }
    }

    // The troops at home (not in a squad) go where defenders are lacking most; ties: the nearer attack. Not only
    // the idle ones: also those walking to the staging area, and the defenders whose attack moved on
    for (const auto &entity : entities.GetAllEntities())
    {
        const Unit *unit = entity.AsUnit();
        if (!unit || !unit->alive || unit->state == ActionState::Die || unit->owner != m_player || unit->moveSpeed <= 0.0f
            || unit->isSiegeMode)
            continue;
        // (troops that went on the attack are never called home to defend)
        if (m_squadController.IsSquadMember(unit->id) || unit->id == m_scoutId || m_offensive.count(unit->id))
            continue;
        const float unitX = static_cast<float>(unit->tileX);
        const float unitY = static_cast<float>(unit->tileY);
        const float value = static_cast<float>(UnitThreatValue(*unit, entities));
        if (unit->targetEntityId != 0)
        {
            // Fighting already: it counts for the attack it stands at
            if (Attack *attack = nearestAttack(unitX, unitY, kDefenderReachTiles))
                attack->assigned += value;
            continue;
        }
        const auto defenderIt = m_defenders.find(unit->id);
        if (defenderIt != m_defenders.end()
            && ((unit->state == ActionState::Move && unit->isAttackMoving)
                || (unit->state == ActionState::Idle
                    && std::max(
                           std::abs(unit->tileX - defenderIt->second.first),
                           std::abs(unit->tileY - defenderIt->second.second))
                           <= 3)))
        {
            // On its way or arrived: left alone while the attack is still about where it was sent
            if (Attack *attack = nearestAttack(
                    static_cast<float>(defenderIt->second.first),
                    static_cast<float>(defenderIt->second.second),
                    kReaimTiles))
            {
                attack->assigned += value;
                continue;
            }
        }
        else if (
            defenderIt == m_defenders.end() && unit->state != ActionState::Idle
            && !(unit->state == ActionState::Move && !unit->isAttackMoving))
            continue; // (firing, transforming, attack-moving to its guard post)
        // The computer's own troops come from all over the base; the mission's garrisons only from close by
        const float reach = atBase(*unit) && !m_garrison.count(unit->id) ? kHomeReachTiles : kDefenderReachTiles;
        Attack *bestAttack = nullptr;
        float bestNeed = 0.0f, bestDist = 0.0f;
        for (Attack &attack : attacks)
        {
            const float dist = DetMath::Hypot(unitX - attack.x, unitY - attack.y);
            if (dist > reach)
                continue;
            const float need = attack.strength * kDefenseMargin - attack.assigned;
            if (!bestAttack || need > bestNeed + 0.5f || (std::abs(need - bestNeed) <= 0.5f && dist < bestDist))
            {
                bestAttack = &attack;
                bestNeed = need;
                bestDist = dist;
            }
        }
        if (!bestAttack)
            continue;
        bestAttack->assigned += value;
        bestAttack->defenders.push_back(unit->id);
    }
    for (const Attack &attack : attacks)
    {
        if (attack.defenders.empty())
            continue;
        const int goalX = static_cast<int>(std::lround(attack.x));
        const int goalY = static_cast<int>(std::lround(attack.y));
        entities.IssueAttackMoveOrder(attack.defenders, goalX, goalY, pathfinder);
        for (int defenderId : attack.defenders)
            m_defenders[defenderId] = {goalX, goalY};
    }

    // Still too few: the base makes do with what it has. A group that left for the enemy never turns back
}

bool AIController::FindSiteNear(
    int &x,
    int &y,
    int width,
    int height,
    int searchRadius,
    const TileMap &map,
    const EntityManager &entities,
    bool *unitBlocked,
    bool needsExit) const
{
    if (unitBlocked)
        *unitBlocked = false;
    WayOutCache wayOutCache; // (the buildings do not change during one search)
    auto clearOfGates = [&](int siteX, int siteY) {
        for (int gateIndex = 0; gateIndex < 2; ++gateIndex)
        {
            if (m_gateX[gateIndex] < 0)
                continue;
            // keep a 2-tile corridor around each exit free of structures
            if (siteX + width + 1 >= m_gateX[gateIndex] && siteX - 2 <= m_gateX[gateIndex]
                && siteY + height + 1 >= m_gateY[gateIndex] && siteY - 2 <= m_gateY[gateIndex])
                return false;
        }
        return true;
    };
    for (int radius = 0; radius <= searchRadius; ++radius)
    {
        for (int offsetY = -radius; offsetY <= radius; ++offsetY)
        {
            for (int offsetX = -radius; offsetX <= radius; ++offsetX)
            {
                if (std::max(std::abs(offsetX), std::abs(offsetY)) != radius)
                    continue;
                const int siteX = x + offsetX;
                const int siteY = y + offsetY;
                if (!clearOfGates(siteX, siteY))
                    continue;
                if (!entities.CanPlaceBuilding(siteX, siteY, width, height, map, m_player))
                    continue;
                // Not on a road between the bases, not on an ally's ground
                bool isTerrainFree = true;
                for (int tileY = siteY; tileY < siteY + height && isTerrainFree; ++tileY)
                {
                    for (int tileX = siteX; tileX < siteX + width && isTerrainFree; ++tileX)
                        isTerrainFree = !IsRoad(tileX, tileY) && IsOwnGround(tileX, tileY);
                }
                if (!isTerrainFree)
                    continue;
                bool unitInside = false;
                for (int tileY = siteY; tileY < siteY + height && !unitInside; ++tileY)
                {
                    for (int tileX = siteX; tileX < siteX + width && !unitInside; ++tileX)
                        unitInside = entities.IsTileOccupiedByUnit(tileX, tileY);
                }
                if (unitInside)
                {
                    if (unitBlocked)
                        *unitBlocked = true;
                    continue;
                }
                if (!KeepsWayOut(siteX, siteY, width, height, map, entities, needsExit, &wayOutCache))
                    continue; // it would close the base in (or itself)
                x = siteX;
                y = siteY;
                return true;
            }
        }
    }
    return false;
}

void AIController::PlanBaseDefenses(const TileMap &map, const EntityManager &entities, Pathfinder &pathfinder)
{
    m_defensePlan.clear();
    m_defensePlanned = true;
    m_gateX[0] = m_gateX[1] = m_gateY[0] = m_gateY[1] = -1;

    // Base centre: the AI headquarters; threat direction: toward the player's base
    int centerX = m_enemyHqX;
    int centerY = m_enemyHqY;
    int hqRadius = 2;
    for (const auto &entity : entities.GetAllEntities())
    {
        const Building *building = entity.AsBuilding();
        if (building && building->alive && building->owner == m_player && building->type == UnitType::Headquarters)
        {
            centerX = building->tileX + building->buildWidth / 2;
            centerY = building->tileY + building->buildHeight / 2;
            hqRadius = std::max(building->buildWidth, building->buildHeight) / 2 + 1;
            break;
        }
    }
    // The roads between the bases and the ground of the allies come first: nothing below may close them
    if (!m_territoryReady)
        PlanTerritory(map, entities, pathfinder);
    Think("BUILD", m_style.Describe());

    float threatDeltaX = static_cast<float>(m_playerHqX - centerX);
    float threatDeltaY = static_cast<float>(m_playerHqY - centerY);
    const float threatDist = std::max(1.0f, DetMath::Hypot(threatDeltaX, threatDeltaY));
    threatDeltaX /= threatDist;
    threatDeltaY /= threatDist;
    const float threatAngle = DetMath::Atan2(threatDeltaY, threatDeltaX);
    // The line of defense: as far out as the building zone allows (a wall near the HQ would close the base in)
    const float maxBuildDist = static_cast<float>(entities.GetBuildRadius(m_player)) - 0.5f;
    const float wallRadius = std::max(
        static_cast<float>(hqRadius + 4),
        std::min(static_cast<float>(hqRadius + 10 + m_style.lineOffset), maxBuildDist - 1.0f));
    auto ringTile = [&](float radius, float angleOffsetDeg, int &outX, int &outY) {
        const float angleRad = threatAngle + angleOffsetDeg * static_cast<float>(M_PI) / 180.0f;
        outX = std::clamp(static_cast<int>(std::lround(centerX + DetMath::Cos(angleRad) * radius)), 1, map.GetWidth() - 2);
        outY
            = std::clamp(static_cast<int>(std::lround(centerY + DetMath::Sin(angleRad) * radius)), 1, map.GetHeight() - 2);
    };
    auto addSite = [&](AIDefenseSite::Kind kind, int x, int y) {
        m_defensePlan.push_back({kind, x, y, false});
    };

    // Power plants, the barracks, the factory and the laboratory come from the core build order (UpdateCoreBuildOrder)

    // 1. Like the bases of the campaign: a wall straight across the narrowest passage of every road to an enemy,
    //    the gate on the road, a tower and a bunker behind it
    std::vector<std::pair<int, int>> wallTiles;
    std::vector<int> wallRow; // arc only: 0 the inner row, 1 the outer one
    const bool passages = PlanPassageWalls(centerX, centerY, hqRadius, map, entities, wallTiles);
    if (passages)
    {
        Think("BUILD", "Closing " + std::to_string(wallTiles.size()) + " tiles of the passages toward the enemy");
    }
    else
    {
        // 2. Open ground: a short arc in front of the base, the exits where the road to the enemy crosses it and
        //    beside it
        std::vector<PathNode> road;
        bool partial = false;
        pathfinder.SetIgnoreUnitsOnce();
        pathfinder.FindPath(centerX, centerY + hqRadius + 1, m_playerHqX, m_playerHqY, road, 0, &partial, true);
        for (const auto &node : road)
        {
            if (DetMath::Hypot(static_cast<float>(node.x - centerX), static_cast<float>(node.y - centerY))
                >= wallRadius - 0.5f)
            {
                m_gateX[0] = node.x;
                m_gateY[0] = node.y;
                break;
            }
        }
        if (m_gateX[0] < 0)
            ringTile(wallRadius, 0.0f, m_gateX[0], m_gateY[0]);
        const float gateAngle
            = DetMath::Atan2(static_cast<float>(m_gateY[0] - centerY), static_cast<float>(m_gateX[0] - centerX));
        auto angleDiff = [](float angleA, float angleB) {
            return std::abs(std::remainder(angleA - angleB, 2.0f * static_cast<float>(M_PI)));
        };
        const float rad30 = 30.0f * static_cast<float>(M_PI) / 180.0f;
        // Every way an enemy comes from gets its arc: the objective's first, then where each road to an enemy crosses
        // the line (directions closer than 40 degrees share one arc). The tile where the road crosses is kept for
        // the second exit.
        struct ArcDir
        {
            float angle;
            int roadX, roadY;
        };
        std::vector<ArcDir> arcs {{threatAngle, m_gateX[0], m_gateY[0]}};
        const float rad40 = 40.0f * static_cast<float>(M_PI) / 180.0f;
        for (const auto &enemyRoad : m_enemyRoads)
        {
            for (const auto &[roadX, roadY] : enemyRoad)
            {
                if (DetMath::Hypot(static_cast<float>(roadX - centerX), static_cast<float>(roadY - centerY))
                    < wallRadius - 0.5f)
                    continue;
                const float roadAngle
                    = DetMath::Atan2(static_cast<float>(roadY - centerY), static_cast<float>(roadX - centerX));
                bool merged = false;
                for (const ArcDir &existingArc : arcs)
                    merged = merged || angleDiff(roadAngle, existingArc.angle) < rad40;
                if (!merged)
                    arcs.push_back({roadAngle, roadX, roadY});
                break;
            }
        }
        // The second exit: toward the side most attacks came from (when one other than the objective's did)
        float worstThreat = 0.0f;
        for (std::size_t i = 1; i < arcs.size(); ++i)
        {
            const float threat = m_sectorThreat[SectorOf(arcs[i].roadX, arcs[i].roadY)];
            if (threat > worstThreat)
            {
                worstThreat = threat;
                m_gateX[1] = arcs[i].roadX;
                m_gateY[1] = arcs[i].roadY;
            }
        }
        const float firstSide
            = angleDiff(gateAngle + rad30, threatAngle) <= angleDiff(gateAngle - rad30, threatAngle) ? 1.0f : -1.0f;
        for (float side : {firstSide, -firstSide})
        {
            if (m_gateX[1] >= 0)
                break; // (set above, toward the worst side)
            const float exitAngle = gateAngle + side * rad30;
            const int gateX = std::clamp(
                static_cast<int>(std::lround(centerX + DetMath::Cos(exitAngle) * wallRadius)), 1, map.GetWidth() - 2);
            const int gateY = std::clamp(
                static_cast<int>(std::lround(centerY + DetMath::Sin(exitAngle) * wallRadius)), 1, map.GetHeight() - 2);
            if (map.IsPassable(gateX, gateY))
            {
                m_gateX[1] = gateX;
                m_gateY[1] = gateY;
                break;
            }
        }
        const int rows = m_style.towersOnly ? 0 : std::clamp(m_style.wallThickness, 1, 2);
        const float halfArc = 40.0f * static_cast<float>(M_PI) / 180.0f;
        for (const ArcDir &arc : arcs)
            for (int row = 0; row < rows; ++row)
            {
                const float ringRadius = wallRadius + static_cast<float>(row);
                const float step = 0.45f / ringRadius;
                for (float angleOffset = -halfArc; angleOffset <= halfArc; angleOffset += step)
                {
                    const int wallX
                        = static_cast<int>(std::lround(centerX + DetMath::Cos(arc.angle + angleOffset) * ringRadius));
                    const int wallY
                        = static_cast<int>(std::lround(centerY + DetMath::Sin(arc.angle + angleOffset) * ringRadius));
                    if (wallX < 1 || wallY < 1 || wallX >= map.GetWidth() - 1 || wallY >= map.GetHeight() - 1)
                        continue;
                    if (std::find(wallTiles.begin(), wallTiles.end(), std::make_pair(wallX, wallY)) != wallTiles.end())
                        continue;
                    bool nearGate = false;
                    for (int gateIndex = 0; gateIndex < 2; ++gateIndex)
                    {
                        if (m_gateX[gateIndex] >= 0
                            && std::max(std::abs(wallX - m_gateX[gateIndex]), std::abs(wallY - m_gateY[gateIndex]))
                                   <= 1)
                            nearGate = true;
                    }
                    // A road to an ally or the band of one stays open (an extra opening), so does an ally's ground;
                    // nothing walks over rock or water anyway
                    if (nearGate || IsRoad(wallX, wallY) || !IsOwnGround(wallX, wallY) || !map.IsPassable(wallX, wallY)
                        || map.GetWall(wallX, wallY) > 0)
                        continue;
                    wallTiles.emplace_back(wallX, wallY);
                    wallRow.push_back(row);
                }
            }
        // Behind every arc: two bunkers right behind the wall (they take the fire), the towers deeper in the base
        // behind them (they reach farther): one in the middle and one on each flank, for every seed.
        for (const ArcDir &arc : arcs)
        {
            const float offset = (arc.angle - threatAngle) * 180.0f / static_cast<float>(M_PI);
            int tileX = 0, tileY = 0;
            for (float flank : {-kBunkerFlankDeg, kBunkerFlankDeg})
            {
                ringTile(wallRadius - kBunkerBehindWall, offset + flank, tileX, tileY);
                addSite(AIDefenseSite::Bunker, tileX - 1, tileY - 1);
            }
            ringTile(wallRadius - kTowerBehindWall, offset, tileX, tileY);
            addSite(AIDefenseSite::Tower, tileX - 1, tileY - 1);
            for (float flank : {-kTowerFlankDeg, kTowerFlankDeg})
            {
                ringTile(wallRadius - kTowerBehindWall, offset + flank, tileX, tileY);
                addSite(AIDefenseSite::Tower, tileX - 1, tileY - 1);
            }
        }
        Think(
            "BUILD",
            "Open ground: " + std::to_string(arcs.size()) + " wall arcs, one toward each way an enemy comes from");
    }
    // This style changes walls only, never the number of towers or bunkers.
    if (m_style.towersOnly && passages)
        wallTiles.clear();

    // 3. Walls last, and only where an enemy can walk in: a ring tile that links the inside of the ring with
    //    the outside one reachable from a hostile base. Segments in front of trees, cliffs or a closed pocket
    //    stop nobody and are left out.
    if (!passages)
    {
        const int mapWidth = map.GetWidth();
        const int mapHeight = map.GetHeight();
        auto tileIndex = [mapWidth](int tileX, int tileY) {
            return tileY * mapWidth + tileX;
        };
        auto isPassableOpen = [&](int tileX, int tileY) {
            return tileX >= 0 && tileY >= 0 && tileX < mapWidth && tileY < mapHeight && map.IsPassable(tileX, tileY)
                   && map.GetWall(tileX, tileY) == 0;
        };
        std::vector<std::uint8_t> ring(static_cast<std::size_t>(mapWidth) * mapHeight, 0), inside(ring.size(), 0),
            outside(ring.size(), 0);
        for (std::size_t i = 0; i < wallTiles.size(); ++i)
            if (wallRow[i] == 0)
                ring[tileIndex(wallTiles[i].first, wallTiles[i].second)] = 1; // the outer row lies outside the ring
        auto distFromCenter = [&](int tileX, int tileY) {
            return DetMath::Hypot(static_cast<float>(tileX - centerX), static_cast<float>(tileY - centerY));
        };
        auto flood = [&](std::vector<std::uint8_t> &visited, std::vector<std::pair<int, int>> frontier, bool insideRing) {
            for (std::size_t i = 0; i < frontier.size(); ++i)
            {
                const auto [frontX, frontY] = frontier[i];
                for (int deltaY = -1; deltaY <= 1; ++deltaY)
                {
                    for (int deltaX = -1; deltaX <= 1; ++deltaX)
                    {
                        const int nextX = frontX + deltaX;
                        const int nextY = frontY + deltaY;
                        if ((deltaX == 0 && deltaY == 0) || !isPassableOpen(nextX, nextY)
                            || ring[tileIndex(nextX, nextY)] || visited[tileIndex(nextX, nextY)])
                            continue;
                        if (insideRing != (distFromCenter(nextX, nextY) < wallRadius))
                            continue;
                        visited[tileIndex(nextX, nextY)] = 1;
                        frontier.emplace_back(nextX, nextY);
                    }
                }
            }
        };
        std::vector<std::pair<int, int>> seedsIn, seedsOut;
        for (int tileY = std::max(0, centerY - hqRadius - 2);
             tileY <= std::min(mapHeight - 1, centerY + hqRadius + 2);
             ++tileY)
        {
            for (int tileX = std::max(0, centerX - hqRadius - 2);
                 tileX <= std::min(mapWidth - 1, centerX + hqRadius + 2);
                 ++tileX)
            {
                if (isPassableOpen(tileX, tileY) && !ring[tileIndex(tileX, tileY)]
                    && distFromCenter(tileX, tileY) < wallRadius)
                {
                    inside[tileIndex(tileX, tileY)] = 1;
                    seedsIn.emplace_back(tileX, tileY);
                }
            }
        }
        for (const auto &entity : entities.GetAllEntities())
        {
            if (!entity.alive || entity.state == ActionState::Die || !Players::Hostile(entity.owner, m_player)
                || entity.faction == Faction::Neutral)
                continue;
            for (int deltaY = -2; deltaY <= 2; ++deltaY)
            {
                for (int deltaX = -2; deltaX <= 2; ++deltaX)
                {
                    const int seedX = entity.tileX + deltaX;
                    const int seedY = entity.tileY + deltaY;
                    if (isPassableOpen(seedX, seedY) && !ring[tileIndex(seedX, seedY)]
                        && !outside[tileIndex(seedX, seedY)] && distFromCenter(seedX, seedY) >= wallRadius)
                    {
                        outside[tileIndex(seedX, seedY)] = 1;
                        seedsOut.emplace_back(seedX, seedY);
                    }
                }
            }
        }
        flood(inside, std::move(seedsIn), true);
        const bool knowsEnemy = !seedsOut.empty();
        flood(outside, std::move(seedsOut), false);
        int skipped = 0;
        // The inner row decides (it links the inside with the outside the enemy comes from); the outer row only
        // goes where the inner one next to it stays
        std::vector<std::uint8_t> kept(wallTiles.size(), 0);
        for (std::size_t i = 0; i < wallTiles.size(); ++i)
        {
            if (wallRow[i] != 0)
                continue;
            const auto [wallX, wallY] = wallTiles[i];
            bool linksIn = false, linksOut = false;
            for (int deltaY = -1; deltaY <= 1; ++deltaY)
            {
                for (int deltaX = -1; deltaX <= 1; ++deltaX)
                {
                    const int neighborX = wallX + deltaX;
                    const int neighborY = wallY + deltaY;
                    if (neighborX < 0 || neighborY < 0 || neighborX >= mapWidth || neighborY >= mapHeight)
                        continue;
                    linksIn = linksIn || inside[tileIndex(neighborX, neighborY)];
                    linksOut = linksOut || outside[tileIndex(neighborX, neighborY)];
                }
            }
            kept[i] = !knowsEnemy || (linksIn && linksOut);
        }
        for (std::size_t i = 0; i < wallTiles.size(); ++i)
        {
            if (wallRow[i] == 0)
                continue;
            for (std::size_t j = 0; j < wallTiles.size() && !kept[i]; ++j)
                kept[i] = wallRow[j] == 0 && kept[j]
                          && std::max(
                                 std::abs(wallTiles[i].first - wallTiles[j].first),
                                 std::abs(wallTiles[i].second - wallTiles[j].second))
                                 <= 1;
        }
        for (std::size_t i = 0; i < wallTiles.size(); ++i)
        {
            if (!kept[i])
            {
                ++skipped;
                continue;
            }
            addSite(AIDefenseSite::Wall, wallTiles[i].first, wallTiles[i].second);
        }
        if (skipped > 0)
            Think("BUILD", "Leaving out " + std::to_string(skipped) + " wall tiles nobody can walk through");
    }

    else
    {
        for (const auto &[wallX, wallY] : wallTiles)
            addSite(AIDefenseSite::Wall, wallX, wallY);
    }

    // The other headquarters of the base get their own, smaller defense
    PlanOutpostDefenses(centerX, centerY, wallRadius, map, entities);
    FinishDefensePlan(map, entities);

    std::ostringstream plan;
    plan << "Base plan: " << m_defensePlan.size() << " items, " << wallTiles.size()
         << (passages ? " wall tiles across passages" : " wall tiles in front of the base") << ", exits (" << m_gateX[0]
         << "," << m_gateY[0] << ") and (" << m_gateX[1] << "," << m_gateY[1] << ")";
    Think("BUILD", plan.str());
}

void AIController::PlanOutpostDefenses(
    int homeCenterX, int homeCenterY, float wallRadius, const TileMap &map, const EntityManager &entities)
{
    constexpr float kPi = static_cast<float>(M_PI);
    auto angleDiff = [](float angleA, float angleB) {
        return std::abs(std::remainder(angleA - angleB, 2.0f * static_cast<float>(M_PI)));
    };
    std::set<std::pair<int, int>> walled;
    for (const AIDefenseSite &site : m_defensePlan)
        if (site.kind == AIDefenseSite::Wall)
            walled.insert({site.tileX, site.tileY});
    int outposts = 0;
    for (const auto &entity : entities.GetAllEntities())
    {
        const Building *building = entity.AsBuilding();
        if (!building || !building->alive || building->state == ActionState::Die || building->owner != m_player
            || building->type != UnitType::Headquarters || building->buildProgress < 1.0f)
            continue;
        const int outpostX = building->tileX + building->buildWidth / 2;
        const int outpostY = building->tileY + building->buildHeight / 2;
        const float fromHome
            = DetMath::Hypot(static_cast<float>(outpostX - homeCenterX), static_cast<float>(outpostY - homeCenterY));
        if (fromHome <= wallRadius - 4.0f)
            continue; // the first headquarters itself, or one well inside the main line
        const float radius = static_cast<float>(std::max(building->buildWidth, building->buildHeight) / 2 + 1);
        // The ways an enemy comes from: the objective and every enemy the roads lead to; not the side of the main
        // base (its line stands there)
        const float homeAngle
            = DetMath::Atan2(static_cast<float>(homeCenterY - outpostY), static_cast<float>(homeCenterX - outpostX));
        std::vector<float> angles;
        auto addAngle = [&](int targetX, int targetY) {
            if (targetX == outpostX && targetY == outpostY)
                return;
            const float angle
                = DetMath::Atan2(static_cast<float>(targetY - outpostY), static_cast<float>(targetX - outpostX));
            if (angleDiff(angle, homeAngle) < 50.0f * kPi / 180.0f)
                return;
            for (float known : angles)
                if (angleDiff(angle, known) < 40.0f * kPi / 180.0f)
                    return;
            angles.push_back(angle);
        };
        addAngle(m_playerHqX, m_playerHqY);
        for (const auto &road : m_enemyRoads)
            if (!road.empty())
                addAngle(road.back().first, road.back().second);
        auto tileAt = [&](float angle, float dist, int &outX, int &outY) {
            outX = std::clamp(
                static_cast<int>(std::lround(outpostX + DetMath::Cos(angle) * dist)), 1, map.GetWidth() - 2);
            outY = std::clamp(
                static_cast<int>(std::lround(outpostY + DetMath::Sin(angle) * dist)), 1, map.GetHeight() - 2);
        };
        for (float angle : angles)
        {
            int siteX = 0, siteY = 0;
            for (float flank : {-kBunkerFlankDeg, kBunkerFlankDeg})
            {
                tileAt(angle + flank * kPi / 180.0f, radius + 7.0f, siteX, siteY);
                m_defensePlan.push_back({AIDefenseSite::Bunker, siteX - 1, siteY - 1, false});
            }
            tileAt(angle, radius + 4.0f, siteX, siteY);
            m_defensePlan.push_back({AIDefenseSite::Tower, siteX - 1, siteY - 1, false});
            if (m_style.towersOnly)
                continue;
            // A short wall in front of them, open in the middle
            const float wallRingRadius = radius + 9.0f;
            int midX = 0, midY = 0;
            tileAt(angle, wallRingRadius, midX, midY);
            const float halfArcRad = 35.0f * kPi / 180.0f;
            for (float angleOffset = -halfArcRad; angleOffset <= halfArcRad; angleOffset += 0.45f / wallRingRadius)
            {
                tileAt(angle + angleOffset, wallRingRadius, siteX, siteY);
                if (std::max(std::abs(siteX - midX), std::abs(siteY - midY)) <= 1)
                    continue;
                if (IsRoad(siteX, siteY) || !IsOwnGround(siteX, siteY) || !map.IsPassable(siteX, siteY)
                    || map.GetWall(siteX, siteY) > 0)
                    continue;
                if (walled.insert({siteX, siteY}).second)
                    m_defensePlan.push_back({AIDefenseSite::Wall, siteX, siteY, false});
            }
        }
        outposts += angles.empty() ? 0 : 1;
    }
    if (outposts > 0)
        Think("BUILD", std::to_string(outposts) + " further headquarters get bunkers, a tower and a wall of their own");
}

void AIController::FinishDefensePlan(const TileMap &map, const EntityManager &entities)
{
    // 1. What stands already (the plan is made again when a headquarters is finished or lost): not built twice
    const bool towerUnlocked = IsBuildingUnlocked(m_player, FactionBuildingIndex(AIDefenseSite::Tower, side()));
    std::vector<const Building *> standing;
    for (const auto &entity : entities.GetAllEntities())
        if (const Building *building = entity.AsBuilding();
            building && building->alive && building->state != ActionState::Die && building->owner == m_player
            && (building->type == UnitType::Tower || building->type == UnitType::Bunker))
            standing.push_back(building);
    std::vector<bool> taken(standing.size(), false);
    for (AIDefenseSite &site : m_defensePlan)
    {
        if (site.done || (site.kind != AIDefenseSite::Tower && site.kind != AIDefenseSite::Bunker))
            continue;
        // (a tower the tech level leaves out is built as a bunker: BuildDefenseSite)
        const UnitType wanted
            = site.kind == AIDefenseSite::Bunker || !towerUnlocked ? UnitType::Bunker : UnitType::Tower;
        for (std::size_t i = 0; i < standing.size(); ++i)
        {
            if (taken[i] || standing[i]->type != wanted
                || std::max(std::abs(standing[i]->tileX - site.tileX), std::abs(standing[i]->tileY - site.tileY)) > 3)
                continue;
            taken[i] = true;
            site.tileX = standing[i]->tileX;
            site.tileY = standing[i]->tileY;
            site.done = true;
            break;
        }
    }
    // 2. The building limits are those of the whole base: what does not fit is dropped now, the last planned first
    //    (a further headquarters before the first one's roads lose anything)
    for (AIDefenseSite::Kind kind : {AIDefenseSite::Bunker, AIDefenseSite::Tower})
    {
        const int index = FactionBuildingIndex(kind, side());
        const int limit = BuildLimits::LimitOf(entities, index);
        if (limit < 0)
            continue;
        int open = 0;
        for (const AIDefenseSite &site : m_defensePlan)
            open += !site.done && site.kind == kind ? 1 : 0;
        int excess = BuildLimits::CountOwned(entities, map, m_player, index) + open - limit;
        for (auto it = m_defensePlan.rbegin(); it != m_defensePlan.rend() && excess > 0; ++it)
            if (!it->done && it->kind == kind)
            {
                it->done = true;
                --excess;
            }
    }
    // 3. The order of building: the bunkers (they take the fire), the walls, then the towers behind them
    auto rank = [](const AIDefenseSite &site) {
        return site.kind == AIDefenseSite::Bunker ? 0 : site.kind == AIDefenseSite::Tower ? 2 : 1;
    };
    std::stable_sort(
        m_defensePlan.begin(), m_defensePlan.end(), [&](const AIDefenseSite &lhs, const AIDefenseSite &rhs) {
            return rank(lhs) < rank(rhs);
        });
}

bool AIController::BuildDefenseSite(AIDefenseSite &site, TileMap &map, EntityManager &entities)
{
    if (!entities.HasBuildingType(UnitType::Headquarters, m_player, true))
        return false; // no finished HQ, no construction (the spot stays planned)
    const int cost = DefenseCost(site.kind, entities, side());
    // A unit on the spot only delays the work; after this many build steps (3 s each) the spot is given up
    constexpr int kMaxUnitWaits = 20;
    if (site.kind == AIDefenseSite::Wall)
    {
        const int x = site.tileX, y = site.tileY;
        // Hard limits (data/limits.yaml) bind the computer as well: the segment is given up
        if (!BuildLimits::CanBuildMore(entities, map, m_player, side() == Faction::Confederation ? 15 : 25)
            || !IsBuildingUnlocked(m_player, side() == Faction::Confederation ? 15 : 25)) // (or the tech level)
        {
            site.done = true;
            ThinkOnce("limitwall", "BUILD", "Walls: the building limit is reached");
            return false;
        }
        // Checked first: a unit also makes the tile impassable and fails canPlaceBuilding
        if (entities.IsTileOccupiedByUnit(x, y))
        {
            site.done = ++site.attempts >= kMaxUnitWaits; // a unit stands there: try again next step
            return false;
        }
        if (!map.IsPassable(x, y) || map.GetWall(x, y) > 0 || !entities.CanPlaceBuilding(x, y, 1, 1, map, m_player)
            || !IsOwnGround(x, y) || !KeepsWayOut(x, y, 1, 1, map, entities))
        {
            site.done = true; // blocked for good (terrain / building / it would close the base): skip this segment
            return false;
        }
        m_credits -= cost;
        map.SetWall(x, y, side(), 100, m_player);
        m_startingWalls.push_back({x, y, side(), cost, false, 0.0f}); // rebuilt when destroyed
        site.done = true;
        return true;
    }

    // A tower the tech level (data/capabilities.yaml) leaves out: a bunker stands there instead, else nothing
    if (site.kind == AIDefenseSite::Tower && !IsBuildingUnlocked(m_player, FactionBuildingIndex(site.kind, side()))
        && IsBuildingUnlocked(m_player, FactionBuildingIndex(AIDefenseSite::Bunker, side())))
        site.kind = AIDefenseSite::Bunker;
    if (!IsBuildingUnlocked(m_player, FactionBuildingIndex(site.kind, side())))
    {
        site.done = true;
        ThinkOnce(
            "tech" + std::to_string(site.kind), "BUILD", "A defense the tech level leaves out: the spot is given up");
        return false;
    }
    const int index = FactionBuildingIndex(site.kind, side());
    // Hard limits (data/limits.yaml) bind the computer as well: no more of that kind, the spot is given up
    if (!BuildLimits::CanBuildMore(entities, map, m_player, index))
    {
        site.done = true;
        ThinkOnce(
            "limit" + std::to_string(index),
            "BUILD",
            std::string("No ") + BuildLimits::KeyOf(BuildLimits::KindOf(index))
                + " is started: the building limit is reached");
        return false;
    }
    const BuildingConfig *buildingConfig = entities.GetBuildingConfig(index);
    const int width = buildingConfig ? buildingConfig->width : 2;
    const int height = buildingConfig ? buildingConfig->height : 2;
    static const char *const kKindNames[]
        = {"tower",
           "bunker",
           "wall",
           "laboratory (behind the HQ)",
           "power plant (behind the HQ)",
           "barracks",
           "factory",
           "headquarters"};
    // Foresight: a building that takes power goes up only when the finished plants carry it as well; else it waits
    // and the next step raises a power plant (a base in the dark trains, researches and shoots nothing)
    const int power = entities.BuildingPower(index);
    if (power < 0)
    {
        const int balance = PowerBalance(entities, false);
        if (balance + power < 0)
        {
            m_powerNeeded = std::max(m_powerNeeded, -power);
            ThinkOnce(
                "power" + std::to_string(site.kind),
                "BUILD",
                std::string(kKindNames[std::clamp(static_cast<int>(site.kind), 0, 7)]) + " waits: it takes "
                    + std::to_string(-power) + " kW and only " + std::to_string(std::max(0, balance))
                    + " kW are spare - a power plant first");
            return false;
        }
    }
    int x = site.tileX, y = site.tileY;
    const bool core = site.kind == AIDefenseSite::Barracks || site.kind == AIDefenseSite::Factory
                      || site.kind == AIDefenseSite::PowerPlant || site.kind == AIDefenseSite::Lab
                      || site.kind == AIDefenseSite::Headquarters;
    const int searchRadius = site.exact ? 0 : (core ? 8 : 3);
    bool unitBlocked = false;
    const bool needsExit = site.kind == AIDefenseSite::Barracks || site.kind == AIDefenseSite::Factory
                           || site.kind == AIDefenseSite::Headquarters;
    if (!FindSiteNear(x, y, width, height, searchRadius, map, entities, &unitBlocked, needsExit))
    {
        // No room around the planned spot; when only units were in the way, try again later
        site.done = !unitBlocked || ++site.attempts >= kMaxUnitWaits;
        return false;
    }
    const int buildingId = entities.SpawnBuildingByIndex(index, x, y, side(), 0.0f, m_player);
    if (buildingId <= 0)
    {
        // Refused by the entity limit: the site stays planned and is tried again; anything else gives it up
        site.done = entities.FreeEntitySlots(m_player) > 0;
        return false;
    }
    m_credits -= cost;
    for (int tileY = y; tileY < y + height && tileY < map.GetHeight(); ++tileY)
    {
        for (int tileX = x; tileX < x + width && tileX < map.GetWidth(); ++tileX)
            map.SetPassable(tileX, tileY, false);
    }
    if (site.kind == AIDefenseSite::PowerPlant)
        m_powerNeeded = 0; // the plant for what waited is coming
    Think(
        "BUILD",
        std::string("Building a ") + kKindNames[site.kind] + " at (" + std::to_string(x) + ", " + std::to_string(y)
            + ") for $" + std::to_string(cost));
    if (const Building *building = entities.GetBuilding(buildingId))
    {
        AIStructurePlan plan;
        plan.type = building->type;
        plan.rawBuildingType = index;
        plan.tileX = x;
        plan.tileY = y;
        plan.width = width;
        plan.height = height;
        plan.creditCost = cost;
        m_startingBuildings.push_back(plan); // remembered for rebuilding
    }
    site.tileX = x;
    site.tileY = y;
    site.done = true;
    return true;
}

} // namespace OpenArtOfWar
