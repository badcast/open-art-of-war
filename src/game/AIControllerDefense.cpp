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
constexpr float kDefenseMargin = 1.2f;          // an attack wants this much more worth of defenders than it brings
constexpr float kReaimTiles = 4.0f;             // a defender on its way is sent again when the attack moved this far
constexpr float kAttackMemorySeconds = 10.0f;   // the base counts as attacked this long after the last sighting
} // namespace

int AIController::SectorOf(int x, int y) const
{
    const int hx = m_homeX >= 0 ? m_homeX : m_enemyHqX, hy = m_homeY >= 0 ? m_homeY : m_enemyHqY;
    if (x == hx && y == hy)
        return 0;
    const float a = DetMath::Atan2(static_cast<float>(y - hy), static_cast<float>(x - hx)); // -pi..pi
    const float turns = a / (2.0f * static_cast<float>(M_PI)) + 1.0f / 16.0f;               // centred on 0
    int s = static_cast<int>(std::floor(turns * 8.0f));
    return ((s % 8) + 8) % 8;
}

int AIController::UnitThreatValue(const Unit &u, const EntityManager &entities) const
{
    // Siege forms count as their mobile form: Sniper 14 -> 4, Rhino 15 -> 10, Grad 16 -> 13
    const int arch = u.archetypeId == 14 ? 4 : u.archetypeId == 15 ? 10 : u.archetypeId == 16 ? 13 : u.archetypeId;
    const UnitConfig *cfg = entities.GetUnitConfig(arch);
    return std::max(50, cfg ? cfg->creditCost : 0);
}

std::vector<const Building *> AIController::OwnStructures(const EntityManager &entities) const
{
    std::vector<const Building *> own;
    for (const auto &e : entities.GetAllEntities())
        if (const Building *b = e.AsBuilding(); b && b->alive && b->state != ActionState::Die && b->owner == m_player)
            own.push_back(b);
    return own;
}

bool AIController::IsNearBase(
    const Entity &e, const std::vector<const Building *> &ownStructures, float extraTiles) const
{
    const float hqPx = m_enemyHqX * TILE_WIDTH + TILE_WIDTH * 0.5f,
                hqPy = m_enemyHqY * TILE_HEIGHT + TILE_HEIGHT * 0.5f;
    if (DetMath::Hypot(e.GetUnitCenterPixelX() - hqPx, e.GetUnitCenterPixelY() - hqPy)
        <= (kHqZoneTiles + extraTiles) * TILE_WIDTH)
        return true;
    for (const Building *b : ownStructures)
    {
        // tiles from the footprint's edge
        const int dx = std::max({b->tileX - e.tileX, 0, e.tileX - (b->tileX + b->buildWidth - 1)});
        const int dy = std::max({b->tileY - e.tileY, 0, e.tileY - (b->tileY + b->buildHeight - 1)});
        if (DetMath::Hypot(static_cast<float>(dx), static_cast<float>(dy)) <= kBuildingZoneTiles + extraTiles)
            return true;
    }
    return false;
}

void AIController::UpdateBaseDefense(EntityManager &entities, Pathfinder &pathfinder)
{
    for (float &t : m_sectorThreat)
        t *= kThreatDecay;
    for (float &t : m_foeHostility)
        t *= kThreatDecay;

    const std::vector<const Building *> ownBuildings = OwnStructures(entities);
    auto atBase = [&](const Entity &e) {
        return IsNearBase(e, ownBuildings, 0.0f);
    };

    // Who attacks the base: the enemy troops seen at it, and whoever hit a structure or a unit of the base a moment
    // ago - also from out of sight or from beyond the zone (artillery, snipers, an enemy tower built next door)
    std::vector<int> threatIds;
    for (const auto &e : entities.GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die)
            continue;
        if (e.AsUnit() && IsFoe(e) && AiSees(e, entities) && atBase(e))
        {
            threatIds.push_back(e.id);
            continue;
        }
        if (e.owner != m_player || e.lastAttackedTimer <= 0.0f || e.lastAttackerId == 0)
            continue;
        if (!e.isBuilding && (m_squadController.IsSquadMember(e.id) || !atBase(e)))
            continue; // (a squad fights its own fights, so does a unit away from the base)
        const Entity *attacker = entities.GetEntity(e.lastAttackerId);
        if (attacker && attacker->alive && attacker->state != ActionState::Die && IsFoe(*attacker)
            && IsNearBase(*attacker, ownBuildings, kShooterZoneTiles))
            threatIds.push_back(attacker->id);
    }
    std::sort(threatIds.begin(), threatIds.end());
    threatIds.erase(std::unique(threatIds.begin(), threatIds.end()), threatIds.end());

    // Troops that left the base after an enemy come back: an attack order of the computer (UpdateIdleUnits) and
    // an attack-move of a defender both pursue without a leash
    for (auto &e : entities.GetAllEntities())
    {
        Unit *u = e.AsUnit();
        if (!u || !u->alive || u->state == ActionState::Die || u->owner != m_player || u->targetEntityId == 0)
            continue;
        if (m_squadController.IsSquadMember(u->id) || m_garrison.count(u->id))
            continue;
        const bool defender = m_defenders.count(u->id) != 0;
        if (!defender && u->orderedTargetId != u->targetEntityId)
            continue; // a target the unit took by itself: its stance leashes it
        if (std::binary_search(threatIds.begin(), threatIds.end(), u->targetEntityId))
            continue; // it still attacks the base
        const Entity *target = entities.GetEntity(u->targetEntityId);
        if (!target || IsNearBase(*target, ownBuildings, kBaseChaseTiles))
            continue;
        u->ClearPathState();
        u->ClearMoveTarget();
        u->targetEntityId = 0;
        u->orderedTargetId = 0;
        u->isAttackMoving = false;
        u->state = ActionState::Idle; // the reserve / its guard post takes it back
        m_defenders.erase(u->id);
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
    for (int id : threatIds)
    {
        const Entity *e = entities.GetEntity(id);
        if (!e)
            continue;
        float value = 0.0f;
        if (const Unit *u = e->AsUnit())
            value = static_cast<float>(UnitThreatValue(*u, entities));
        else if (const Building *b = e->AsBuilding())
        {
            const BuildingConfig *cfg = entities.GetBuildingConfig(b->rawBuildingType);
            value = static_cast<float>(std::max(50, cfg ? cfg->creditCost : 0));
        }
        m_sectorThreat[SectorOf(e->tileX, e->tileY)] += value;
        if (e->owner < kMaxPlayers)
            m_foeHostility[e->owner] += value;
        const float tx = static_cast<float>(e->tileX), ty = static_cast<float>(e->tileY);
        Attack *into = nullptr;
        for (Attack &a : attacks)
        {
            if (DetMath::Hypot(a.x - tx, a.y - ty) <= kClusterTiles)
            {
                into = &a;
                break;
            }
        }
        if (!into)
        {
            attacks.push_back({});
            into = &attacks.back();
            into->firstId = id;
        }
        into->x = (into->x * into->count + tx) / static_cast<float>(into->count + 1);
        into->y = (into->y * into->count + ty) / static_cast<float>(into->count + 1);
        ++into->count;
        into->strength += value;
    }

    // Defenders that died or joined a squad are none any more; with the base quiet again all of them are released
    for (auto it = m_defenders.begin(); it != m_defenders.end();)
    {
        const Unit *u = entities.GetUnit(it->first);
        if (attacks.empty() || !u || !u->alive || u->state == ActionState::Die || u->owner != m_player
            || m_squadController.IsSquadMember(it->first) || it->first == m_scoutId || m_repairing.count(it->first))
            it = m_defenders.erase(it);
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
    for (const Attack &a : attacks)
        if (a.strength > strongest->strength)
            strongest = &a;
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
        for (Attack &a : attacks)
        {
            const float d = DetMath::Hypot(x - a.x, y - a.y);
            if (d <= bestDist)
            {
                bestDist = d;
                best = &a;
            }
        }
        return best;
    };
    // The towers and bunkers that reach an attack count as its defenders
    for (const Building *b : ownBuildings)
    {
        if (b->attackDamage <= 0 || b->attackRange <= 0.0f || b->buildProgress < 1.0f)
            continue;
        if (Attack *a
            = nearestAttack(static_cast<float>(b->tileX), static_cast<float>(b->tileY), b->attackRange + 2.0f))
        {
            const BuildingConfig *cfg = entities.GetBuildingConfig(b->rawBuildingType);
            a->assigned += static_cast<float>(std::max(50, cfg ? cfg->creditCost : 0));
        }
    }

    // The troops at home (not in a squad) go where defenders are lacking most; ties: the nearer attack. Not only
    // the idle ones: also those walking to the staging area, and the defenders whose attack moved on
    for (const auto &e : entities.GetAllEntities())
    {
        const Unit *u = e.AsUnit();
        if (!u || !u->alive || u->state == ActionState::Die || u->owner != m_player || u->moveSpeed <= 0.0f
            || u->isSiegeMode)
            continue;
        // (troops that went on the attack are never called home to defend)
        if (m_squadController.IsSquadMember(u->id) || u->id == m_scoutId || m_repairing.count(u->id)
            || m_offensive.count(u->id))
            continue;
        const float ux = static_cast<float>(u->tileX), uy = static_cast<float>(u->tileY);
        const float value = static_cast<float>(UnitThreatValue(*u, entities));
        if (u->targetEntityId != 0)
        {
            // Fighting already: it counts for the attack it stands at
            if (Attack *a = nearestAttack(ux, uy, kDefenderReachTiles))
                a->assigned += value;
            continue;
        }
        const auto defender = m_defenders.find(u->id);
        if (defender != m_defenders.end()
            && ((u->state == ActionState::Move && u->isAttackMoving)
                || (u->state == ActionState::Idle
                    && std::max(
                           std::abs(u->tileX - defender->second.first), std::abs(u->tileY - defender->second.second))
                           <= 3)))
        {
            // On its way or arrived: left alone while the attack is still about where it was sent
            if (Attack *a = nearestAttack(
                    static_cast<float>(defender->second.first),
                    static_cast<float>(defender->second.second),
                    kReaimTiles))
            {
                a->assigned += value;
                continue;
            }
        }
        else if (
            defender == m_defenders.end() && u->state != ActionState::Idle
            && !(u->state == ActionState::Move && !u->isAttackMoving))
            continue; // (firing, transforming, attack-moving to its guard post)
        // The computer's own troops come from all over the base; the mission's garrisons only from close by
        const float reach = atBase(*u) && !m_garrison.count(u->id) ? kHomeReachTiles : kDefenderReachTiles;
        Attack *best = nullptr;
        float bestNeed = 0.0f, bestDist = 0.0f;
        for (Attack &a : attacks)
        {
            const float d = DetMath::Hypot(ux - a.x, uy - a.y);
            if (d > reach)
                continue;
            const float need = a.strength * kDefenseMargin - a.assigned;
            if (!best || need > bestNeed + 0.5f || (std::abs(need - bestNeed) <= 0.5f && d < bestDist))
            {
                best = &a;
                bestNeed = need;
                bestDist = d;
            }
        }
        if (!best)
            continue;
        best->assigned += value;
        best->defenders.push_back(u->id);
    }
    for (const Attack &a : attacks)
    {
        if (a.defenders.empty())
            continue;
        const int gx = static_cast<int>(std::lround(a.x)), gy = static_cast<int>(std::lround(a.y));
        entities.IssueAttackMoveOrder(a.defenders, gx, gy, pathfinder);
        for (int id : a.defenders)
            m_defenders[id] = {gx, gy};
    }

    // Still too few: the base makes do with what it has. A group that left for the enemy never turns back
}

void AIController::UpdateGuardPosts(EntityManager &entities, Pathfinder &pathfinder)
{
    if (m_guardPosts.empty())
        return;
    // Guards that died, joined a squad or left the reserve are no guards any more
    for (auto it = m_guardOf.begin(); it != m_guardOf.end();)
    {
        const Unit *u = entities.GetUnit(it->first);
        if (!u || !IsReserveUnit(*u) || it->second >= static_cast<int>(m_guardPosts.size()))
            it = m_guardOf.erase(it);
        else
            ++it;
    }

    // How many each post gets: one each first (the most threatened posts first, when there are too few guards),
    // the rest one by one to the post whose threat per guard is highest
    const int posts = static_cast<int>(m_guardPosts.size());
    std::vector<float> weight(static_cast<std::size_t>(posts));
    for (int i = 0; i < posts; ++i)
        weight[i] = 1.0f + m_sectorThreat[SectorOf(m_guardPosts[i].first, m_guardPosts[i].second)];
    std::vector<int> order(static_cast<std::size_t>(posts));
    for (int i = 0; i < posts; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return weight[a] > weight[b]; });
    std::vector<int> want(static_cast<std::size_t>(posts), 0);
    int left = HomeGuardCount();
    for (int i = 0; i < posts && left > 0; ++i, --left)
        ++want[order[i]];
    for (; left > 0; --left)
    {
        int best = 0;
        for (int i = 1; i < posts; ++i)
            if (weight[i] / static_cast<float>(want[i] + 1) > weight[best] / static_cast<float>(want[best] + 1))
                best = i;
        ++want[best];
    }

    // Too many at a post: the extra ones go back to the reserve
    std::vector<int> have(static_cast<std::size_t>(posts), 0);
    for (auto it = m_guardOf.begin(); it != m_guardOf.end();)
    {
        if (have[it->second] >= want[it->second])
        {
            it = m_guardOf.erase(it);
            continue;
        }
        ++have[it->second];
        ++it;
    }
    // Too few: the nearest idle troops of the reserve
    for (int i = 0; i < posts; ++i)
    {
        while (have[i] < want[i])
        {
            const auto [px, py] = m_guardPosts[i];
            int bestId = 0;
            int bestDist = 1 << 30;
            for (const auto &e : entities.GetAllEntities())
            {
                const Unit *u = e.AsUnit();
                if (!u || !IsReserveUnit(*u) || IsPostGuard(u->id) || u->id == m_scoutId || m_repairing.count(u->id)
                    || m_defenders.count(u->id) || u->state != ActionState::Idle || u->targetEntityId != 0
                    || u->isAttackMoving)
                    continue;
                const int d = std::max(std::abs(u->tileX - px), std::abs(u->tileY - py));
                if (d < bestDist)
                {
                    bestDist = d;
                    bestId = u->id;
                }
            }
            if (bestId == 0)
                break;
            m_guardOf[bestId] = i;
            ++have[i];
        }
    }

    // Guards away from their post (it was the reserve, or it went for an intruder) walk back to it
    std::vector<std::vector<int>> back(static_cast<std::size_t>(posts));
    for (const auto &[id, post] : m_guardOf)
    {
        const Unit *u = entities.GetUnit(id);
        const auto [px, py] = m_guardPosts[post];
        if (u && u->state == ActionState::Idle && u->targetEntityId == 0 && !u->isAttackMoving
            && std::max(std::abs(u->tileX - px), std::abs(u->tileY - py)) > 2)
            back[post].push_back(id);
    }
    for (int i = 0; i < posts; ++i)
        if (!back[i].empty() && m_baseAttackedTimer <= 0.0f) // (during an attack the guards fight where it is)
            entities.IssueAttackMoveOrder(back[i], m_guardPosts[i].first, m_guardPosts[i].second, pathfinder);
    ThinkOnce(
        "guardposts",
        "DEFENSE",
        std::to_string(m_guardOf.size()) + " guard(s) at " + std::to_string(posts) + " post(s) on the roads in");
}

bool AIController::FindSiteNear(
    int &x,
    int &y,
    int w,
    int h,
    int searchRadius,
    const TileMap &map,
    const EntityManager &entities,
    bool *unitBlocked,
    bool needsExit) const
{
    if (unitBlocked)
        *unitBlocked = false;
    auto clearOfGates = [&](int px, int py) {
        for (int g = 0; g < 2; ++g)
        {
            if (m_gateX[g] < 0)
                continue;
            // keep a 2-tile corridor around each exit free of structures
            if (px + w + 1 >= m_gateX[g] && px - 2 <= m_gateX[g] && py + h + 1 >= m_gateY[g] && py - 2 <= m_gateY[g])
                return false;
        }
        return true;
    };
    for (int r = 0; r <= searchRadius; ++r)
    {
        for (int oy = -r; oy <= r; ++oy)
        {
            for (int ox = -r; ox <= r; ++ox)
            {
                if (std::max(std::abs(ox), std::abs(oy)) != r)
                    continue;
                const int px = x + ox, py = y + oy;
                if (!clearOfGates(px, py))
                    continue;
                if (!entities.CanPlaceBuilding(px, py, w, h, map, m_player))
                    continue;
                // Not on a road between the bases, not on an ally's ground
                bool free = true;
                for (int ty = py; ty < py + h && free; ++ty)
                {
                    for (int tx = px; tx < px + w && free; ++tx)
                        free = !IsRoad(tx, ty) && IsOwnGround(tx, ty);
                }
                if (!free)
                    continue;
                bool unitInside = false;
                for (int ty = py; ty < py + h && !unitInside; ++ty)
                {
                    for (int tx = px; tx < px + w && !unitInside; ++tx)
                        unitInside = entities.IsTileOccupiedByUnit(tx, ty);
                }
                if (unitInside)
                {
                    if (unitBlocked)
                        *unitBlocked = true;
                    continue;
                }
                if (!KeepsWayOut(px, py, w, h, map, entities, needsExit))
                    continue; // it would close the base in (or itself)
                x = px;
                y = py;
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
    int cx = m_enemyHqX, cy = m_enemyHqY;
    int hqRadius = 2;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->owner == m_player && b->type == UnitType::Headquarters)
        {
            cx = b->tileX + b->buildWidth / 2;
            cy = b->tileY + b->buildHeight / 2;
            hqRadius = std::max(b->buildWidth, b->buildHeight) / 2 + 1;
            break;
        }
    }
    // The roads between the bases and the ground of the allies come first: nothing below may close them
    if (!m_territoryReady)
        PlanTerritory(map, entities, pathfinder);
    Think("BUILD", m_style.Describe());

    float tdx = static_cast<float>(m_playerHqX - cx);
    float tdy = static_cast<float>(m_playerHqY - cy);
    const float tlen = std::max(1.0f, DetMath::Hypot(tdx, tdy));
    tdx /= tlen;
    tdy /= tlen;
    const float threatAngle = DetMath::Atan2(tdy, tdx);
    // The line of defense: as far out as the building zone allows (a wall near the HQ would close the base in)
    const float maxD = static_cast<float>(entities.GetBuildRadius(m_player)) - 0.5f;
    const float wallRadius = std::max(
        static_cast<float>(hqRadius + 4),
        std::min(static_cast<float>(hqRadius + 10 + m_style.lineOffset), maxD - 1.0f));
    auto ringTile = [&](float radius, float angleOffsetDeg, int &x, int &y) {
        const float a = threatAngle + angleOffsetDeg * static_cast<float>(M_PI) / 180.0f;
        x = std::clamp(static_cast<int>(std::lround(cx + DetMath::Cos(a) * radius)), 1, map.GetWidth() - 2);
        y = std::clamp(static_cast<int>(std::lround(cy + DetMath::Sin(a) * radius)), 1, map.GetHeight() - 2);
    };
    auto add = [&](AIDefenseSite::Kind kind, int x, int y) {
        m_defensePlan.push_back({kind, x, y, false});
    };

    // Power plants, the barracks, the factory and the laboratory come from the core build order (UpdateCoreBuildOrder)

    // Guard posts: on every road to an enemy, the last road tile two tiles inside the line of defense (posts closer
    // than 6 tiles to each other are one)
    m_guardPosts.clear();
    m_guardOf.clear();
    for (const auto &enemyRoad : m_enemyRoads)
    {
        int px = -1, py = -1;
        for (const auto &[rx, ry] : enemyRoad)
        {
            if (DetMath::Hypot(static_cast<float>(rx - cx), static_cast<float>(ry - cy)) > wallRadius - 2.0f)
                break;
            px = rx;
            py = ry;
        }
        if (px < 0)
            continue;
        bool near = false;
        for (const auto &[gx, gy] : m_guardPosts)
            near = near || std::max(std::abs(gx - px), std::abs(gy - py)) < 6;
        if (!near)
            m_guardPosts.emplace_back(px, py);
    }

    // 1. Like the bases of the campaign: a wall straight across the narrowest passage of every road to an enemy,
    //    the gate on the road, a tower and a bunker behind it
    std::vector<std::pair<int, int>> wallTiles;
    std::vector<int> wallRow; // arc only: 0 the inner row, 1 the outer one
    const bool passages = PlanPassageWalls(cx, cy, hqRadius, map, entities, wallTiles);
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
        pathfinder.FindPath(cx, cy + hqRadius + 1, m_playerHqX, m_playerHqY, road, 0, &partial, true);
        for (const auto &n : road)
        {
            if (DetMath::Hypot(static_cast<float>(n.x - cx), static_cast<float>(n.y - cy)) >= wallRadius - 0.5f)
            {
                m_gateX[0] = n.x;
                m_gateY[0] = n.y;
                break;
            }
        }
        if (m_gateX[0] < 0)
            ringTile(wallRadius, 0.0f, m_gateX[0], m_gateY[0]);
        const float gateAngle
            = DetMath::Atan2(static_cast<float>(m_gateY[0] - cy), static_cast<float>(m_gateX[0] - cx));
        auto angleDiff = [](float a, float b) {
            return std::abs(std::remainder(a - b, 2.0f * static_cast<float>(M_PI)));
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
            for (const auto &[rx, ry] : enemyRoad)
            {
                if (DetMath::Hypot(static_cast<float>(rx - cx), static_cast<float>(ry - cy)) < wallRadius - 0.5f)
                    continue;
                const float a = DetMath::Atan2(static_cast<float>(ry - cy), static_cast<float>(rx - cx));
                bool merged = false;
                for (const ArcDir &d : arcs)
                    merged = merged || angleDiff(a, d.angle) < rad40;
                if (!merged)
                    arcs.push_back({a, rx, ry});
                break;
            }
        }
        // The second exit: toward the side most attacks came from (when one other than the objective's did)
        float worst = 0.0f;
        for (std::size_t i = 1; i < arcs.size(); ++i)
        {
            const float threat = m_sectorThreat[SectorOf(arcs[i].roadX, arcs[i].roadY)];
            if (threat > worst)
            {
                worst = threat;
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
            const float a = gateAngle + side * rad30;
            const int gx
                = std::clamp(static_cast<int>(std::lround(cx + DetMath::Cos(a) * wallRadius)), 1, map.GetWidth() - 2);
            const int gy
                = std::clamp(static_cast<int>(std::lround(cy + DetMath::Sin(a) * wallRadius)), 1, map.GetHeight() - 2);
            if (map.IsPassable(gx, gy))
            {
                m_gateX[1] = gx;
                m_gateY[1] = gy;
                break;
            }
        }
        const int rows = m_style.towersOnly ? 0 : std::clamp(m_style.wallThickness, 1, 2);
        const float halfArc = 40.0f * static_cast<float>(M_PI) / 180.0f;
        for (const ArcDir &arc : arcs)
            for (int row = 0; row < rows; ++row)
            {
                const float radius = wallRadius + static_cast<float>(row);
                const float step = 0.45f / radius;
                for (float a = -halfArc; a <= halfArc; a += step)
                {
                    const int wx = static_cast<int>(std::lround(cx + DetMath::Cos(arc.angle + a) * radius));
                    const int wy = static_cast<int>(std::lround(cy + DetMath::Sin(arc.angle + a) * radius));
                    if (wx < 1 || wy < 1 || wx >= map.GetWidth() - 1 || wy >= map.GetHeight() - 1)
                        continue;
                    if (std::find(wallTiles.begin(), wallTiles.end(), std::make_pair(wx, wy)) != wallTiles.end())
                        continue;
                    bool nearGate = false;
                    for (int g = 0; g < 2; ++g)
                    {
                        if (m_gateX[g] >= 0 && std::max(std::abs(wx - m_gateX[g]), std::abs(wy - m_gateY[g])) <= 1)
                            nearGate = true;
                    }
                    // A road to an ally or the band of one stays open (an extra opening), so does an ally's ground;
                    // nothing walks over rock or water anyway
                    if (nearGate || IsRoad(wx, wy) || !IsOwnGround(wx, wy) || !map.IsPassable(wx, wy)
                        || map.GetWall(wx, wy) > 0)
                        continue;
                    wallTiles.emplace_back(wx, wy);
                    wallRow.push_back(row);
                }
            }
        // Behind every arc: two bunkers right behind the wall (they take the fire), the towers deeper in the base
        // behind them (they reach farther): one in the middle and one on each flank, for every seed.
        for (const ArcDir &arc : arcs)
        {
            const float offset = (arc.angle - threatAngle) * 180.0f / static_cast<float>(M_PI);
            int x = 0, y = 0;
            for (float flank : {-kBunkerFlankDeg, kBunkerFlankDeg})
            {
                ringTile(wallRadius - kBunkerBehindWall, offset + flank, x, y);
                add(AIDefenseSite::Bunker, x - 1, y - 1);
            }
            ringTile(wallRadius - kTowerBehindWall, offset, x, y);
            add(AIDefenseSite::Tower, x - 1, y - 1);
            for (float flank : {-kTowerFlankDeg, kTowerFlankDeg})
            {
                ringTile(wallRadius - kTowerBehindWall, offset + flank, x, y);
                add(AIDefenseSite::Tower, x - 1, y - 1);
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
        const int mw = map.GetWidth(), mh = map.GetHeight();
        auto idx = [mw](int px, int py) {
            return py * mw + px;
        };
        auto open = [&](int px, int py) {
            return px >= 0 && py >= 0 && px < mw && py < mh && map.IsPassable(px, py) && map.GetWall(px, py) == 0;
        };
        std::vector<std::uint8_t> ring(static_cast<std::size_t>(mw) * mh, 0), inside(ring.size(), 0),
            outside(ring.size(), 0);
        for (std::size_t i = 0; i < wallTiles.size(); ++i)
            if (wallRow[i] == 0)
                ring[idx(wallTiles[i].first, wallTiles[i].second)] = 1; // the outer row lies outside the ring
        auto distOf = [&](int px, int py) {
            return DetMath::Hypot(static_cast<float>(px - cx), static_cast<float>(py - cy));
        };
        auto flood = [&](std::vector<std::uint8_t> &mark, std::vector<std::pair<int, int>> frontier, bool in) {
            for (std::size_t i = 0; i < frontier.size(); ++i)
            {
                const auto [fx, fy] = frontier[i];
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nx = fx + dx, ny = fy + dy;
                        if ((dx == 0 && dy == 0) || !open(nx, ny) || ring[idx(nx, ny)] || mark[idx(nx, ny)])
                            continue;
                        if (in != (distOf(nx, ny) < wallRadius))
                            continue;
                        mark[idx(nx, ny)] = 1;
                        frontier.emplace_back(nx, ny);
                    }
                }
            }
        };
        std::vector<std::pair<int, int>> seedsIn, seedsOut;
        for (int py = std::max(0, cy - hqRadius - 2); py <= std::min(mh - 1, cy + hqRadius + 2); ++py)
        {
            for (int px = std::max(0, cx - hqRadius - 2); px <= std::min(mw - 1, cx + hqRadius + 2); ++px)
            {
                if (open(px, py) && !ring[idx(px, py)] && distOf(px, py) < wallRadius)
                {
                    inside[idx(px, py)] = 1;
                    seedsIn.emplace_back(px, py);
                }
            }
        }
        for (const auto &e : entities.GetAllEntities())
        {
            if (!e.alive || e.state == ActionState::Die || !Players::Hostile(e.owner, m_player)
                || e.faction == Faction::Neutral)
                continue;
            for (int dy = -2; dy <= 2; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    const int px = e.tileX + dx, py = e.tileY + dy;
                    if (open(px, py) && !ring[idx(px, py)] && !outside[idx(px, py)] && distOf(px, py) >= wallRadius)
                    {
                        outside[idx(px, py)] = 1;
                        seedsOut.emplace_back(px, py);
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
            const auto [wx, wy] = wallTiles[i];
            bool linksIn = false, linksOut = false;
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int nx = wx + dx, ny = wy + dy;
                    if (nx < 0 || ny < 0 || nx >= mw || ny >= mh)
                        continue;
                    linksIn = linksIn || inside[idx(nx, ny)];
                    linksOut = linksOut || outside[idx(nx, ny)];
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
            add(AIDefenseSite::Wall, wallTiles[i].first, wallTiles[i].second);
        }
        if (skipped > 0)
            Think("BUILD", "Leaving out " + std::to_string(skipped) + " wall tiles nobody can walk through");
    }

    else
    {
        for (const auto &[wx, wy] : wallTiles)
            add(AIDefenseSite::Wall, wx, wy);
    }

    // The other headquarters of the base get their own, smaller defense
    PlanOutpostDefenses(cx, cy, wallRadius, map, entities);
    FinishDefensePlan(map, entities);

    std::ostringstream plan;
    plan << "Base plan: " << m_defensePlan.size() << " items, " << wallTiles.size()
         << (passages ? " wall tiles across passages" : " wall tiles in front of the base") << ", exits (" << m_gateX[0]
         << "," << m_gateY[0] << ") and (" << m_gateX[1] << "," << m_gateY[1] << ")";
    Think("BUILD", plan.str());
}

void AIController::PlanOutpostDefenses(
    int cx, int cy, float wallRadius, const TileMap &map, const EntityManager &entities)
{
    constexpr float kPi = static_cast<float>(M_PI);
    auto angleDiff = [](float a, float b) {
        return std::abs(std::remainder(a - b, 2.0f * static_cast<float>(M_PI)));
    };
    std::set<std::pair<int, int>> walled;
    for (const AIDefenseSite &site : m_defensePlan)
        if (site.kind == AIDefenseSite::Wall)
            walled.insert({site.tileX, site.tileY});
    int outposts = 0;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->owner != m_player || b->type != UnitType::Headquarters
            || b->buildProgress < 1.0f)
            continue;
        const int ox = b->tileX + b->buildWidth / 2, oy = b->tileY + b->buildHeight / 2;
        const float fromHome = DetMath::Hypot(static_cast<float>(ox - cx), static_cast<float>(oy - cy));
        if (fromHome <= wallRadius - 4.0f)
            continue; // the first headquarters itself, or one well inside the main line
        const float radius = static_cast<float>(std::max(b->buildWidth, b->buildHeight) / 2 + 1);
        // The ways an enemy comes from: the objective and every enemy the roads lead to; not the side of the main
        // base (its line stands there)
        const float homeAngle = DetMath::Atan2(static_cast<float>(cy - oy), static_cast<float>(cx - ox));
        std::vector<float> angles;
        auto addAngle = [&](int tx, int ty) {
            if (tx == ox && ty == oy)
                return;
            const float a = DetMath::Atan2(static_cast<float>(ty - oy), static_cast<float>(tx - ox));
            if (angleDiff(a, homeAngle) < 50.0f * kPi / 180.0f)
                return;
            for (float known : angles)
                if (angleDiff(a, known) < 40.0f * kPi / 180.0f)
                    return;
            angles.push_back(a);
        };
        addAngle(m_playerHqX, m_playerHqY);
        for (const auto &road : m_enemyRoads)
            if (!road.empty())
                addAngle(road.back().first, road.back().second);
        auto tileAt = [&](float a, float r, int &x, int &y) {
            x = std::clamp(static_cast<int>(std::lround(ox + DetMath::Cos(a) * r)), 1, map.GetWidth() - 2);
            y = std::clamp(static_cast<int>(std::lround(oy + DetMath::Sin(a) * r)), 1, map.GetHeight() - 2);
        };
        for (float a : angles)
        {
            int x = 0, y = 0;
            for (float flank : {-kBunkerFlankDeg, kBunkerFlankDeg})
            {
                tileAt(a + flank * kPi / 180.0f, radius + 7.0f, x, y);
                m_defensePlan.push_back({AIDefenseSite::Bunker, x - 1, y - 1, false});
            }
            tileAt(a, radius + 4.0f, x, y);
            m_defensePlan.push_back({AIDefenseSite::Tower, x - 1, y - 1, false});
            if (m_style.towersOnly)
                continue;
            // A short wall in front of them, open in the middle
            const float ring = radius + 9.0f;
            int midX = 0, midY = 0;
            tileAt(a, ring, midX, midY);
            const float half = 35.0f * kPi / 180.0f;
            for (float off = -half; off <= half; off += 0.45f / ring)
            {
                tileAt(a + off, ring, x, y);
                if (std::max(std::abs(x - midX), std::abs(y - midY)) <= 1)
                    continue;
                if (IsRoad(x, y) || !IsOwnGround(x, y) || !map.IsPassable(x, y) || map.GetWall(x, y) > 0)
                    continue;
                if (walled.insert({x, y}).second)
                    m_defensePlan.push_back({AIDefenseSite::Wall, x, y, false});
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
    for (const auto &e : entities.GetAllEntities())
        if (const Building *b = e.AsBuilding(); b && b->alive && b->state != ActionState::Die && b->owner == m_player
                                                && (b->type == UnitType::Tower || b->type == UnitType::Bunker))
            standing.push_back(b);
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
    std::stable_sort(m_defensePlan.begin(), m_defensePlan.end(), [&](const AIDefenseSite &a, const AIDefenseSite &b) {
        return rank(a) < rank(b);
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
    const BuildingConfig *cfg = entities.GetBuildingConfig(index);
    const int w = cfg ? cfg->width : 2;
    const int h = cfg ? cfg->height : 2;
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
    if (!FindSiteNear(x, y, w, h, searchRadius, map, entities, &unitBlocked, needsExit))
    {
        // No room around the planned spot; when only units were in the way, try again later
        site.done = !unitBlocked || ++site.attempts >= kMaxUnitWaits;
        return false;
    }
    const int id = entities.SpawnBuildingByIndex(index, x, y, side(), 0.0f, m_player);
    if (id <= 0)
    {
        // Refused by the entity limit: the site stays planned and is tried again; anything else gives it up
        site.done = entities.FreeEntitySlots(m_player) > 0;
        return false;
    }
    m_credits -= cost;
    for (int ty = y; ty < y + h && ty < map.GetHeight(); ++ty)
    {
        for (int tx = x; tx < x + w && tx < map.GetWidth(); ++tx)
            map.SetPassable(tx, ty, false);
    }
    if (site.kind == AIDefenseSite::PowerPlant)
        m_powerNeeded = 0; // the plant for what waited is coming
    Think(
        "BUILD",
        std::string("Building a ") + kKindNames[site.kind] + " at (" + std::to_string(x) + ", " + std::to_string(y)
            + ") for $" + std::to_string(cost));
    if (const Building *b = entities.GetBuilding(id))
    {
        AIStructurePlan plan;
        plan.type = b->type;
        plan.rawBuildingType = index;
        plan.tileX = x;
        plan.tileY = y;
        plan.width = w;
        plan.height = h;
        plan.creditCost = cost;
        m_startingBuildings.push_back(plan); // remembered for rebuilding
    }
    site.tileX = x;
    site.tileY = y;
    site.done = true;
    return true;
}

} // namespace OpenArtOfWar
