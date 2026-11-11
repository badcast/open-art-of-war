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

namespace OpenArtOfWar
{

AIWavePlan AIController::PlanWave(int waveNumber, bool hasFactory, bool hasFactoryPlan) const
{
    const AITacticsConfig &t = m_currentDifficultyConfig.tactics;
    AIWavePlan plan;
    plan.waveNumber = std::max(1, waveNumber);

    int size = std::min(t.waveMaxSize, t.waveBaseSize + t.waveGrowth * (plan.waveNumber - 1));
    size = std::clamp(static_cast<int>(std::lround(size * m_debugConfig.waveSpawnMultiplier)), 1, 30);
    size = std::max(size, kMinAttackWave);          // the AI never sends a handful: at least ten troops march
    size = std::min(30, size + EscalationTroops()); // waves that broke nothing: more troops (what the money allows)

    const int n = plan.waveNumber;
    int tier = n >= t.siegeWave ? 4 : n >= t.assaultWave ? 3 : n >= t.vanguardWave ? 2 : 1;
    tier = std::clamp(tier, 1, std::clamp(t.maxWaveTier, 1, 4));
    plan.tier = static_cast<AIWaveTier>(tier);

    std::vector<int> core; // tanks / heavy weapons
    int support = 0;       // rocket troopers
    int melee = 0;         // flamethrowers
    switch (plan.tier)
    {
        case AIWaveTier::Scout:
            break;
        case AIWaveTier::Vanguard:
            support = std::clamp(size / 6, 1, 2);
            melee = t.meleeRangedPairs ? std::max(1, size / 5) : 0;
            break;
        case AIWaveTier::Assault:
        {
            int tanks = std::max(1, size / 5);
            for (int i = 0; i < tanks; ++i)
            {
                core.push_back((t.armorFront || i % 2 == 1) ? kHammer : kFortress);
            }
            support = std::max(1, size / 5);
            melee = t.meleeRangedPairs ? size / 6 : 0;
            break;
        }
        case AIWaveTier::Siege:
        {
            int tanks = std::max(2, size / 4);
            static const int kPattern[] = {kHammer, kZeus, kHammer, kFortress};
            for (int i = 0; i < tanks; ++i)
                core.push_back(kPattern[i % 4]);
            int artillery = std::max(1, size / 8);
            for (int i = 0; i < artillery; ++i)
                core.push_back(kGrad);
            support = std::max(1, size / 5);
            melee = t.meleeRangedPairs ? size / 6 : 0;
            break;
        }
    }

    // Without a factory (and none to rebuild) heavy slots become heavy infantry
    if (!hasFactory && !hasFactoryPlan)
    {
        for (int arch : core)
        {
            if (arch == kGrad)
                ++support;
            else
                ++melee;
        }
        core.clear();
    }

    int heavyCount = static_cast<int>(core.size()) + support + melee;
    if (heavyCount > size)
        size = heavyCount;
    int riflemen = size - heavyCount;

    // Interleave so the infantry queue alternates cheap and specialist troops
    for (int i = 0; i < riflemen || support > 0 || melee > 0; ++i)
    {
        if (i < riflemen)
            plan.archetypes.push_back(kRifleman);
        if (support > 0 && i % 2 == 1)
        {
            plan.archetypes.push_back(kHeavyAssault);
            --support;
        }
        if (melee > 0 && i % 2 == 0)
        {
            plan.archetypes.push_back(kFlamethrower);
            --melee;
        }
        if (i > 64)
            break;
    }
    // Counter-composition from what the computer has seen of the enemy army (Hardcore: all of it)
    int seenVehicles = 0, seenInfantry = 0;
    for (const auto &seenUnit : m_seenEnemyUnits)
        (seenUnit.second ? seenVehicles : seenInfantry) += 1;
    const int seen = seenVehicles + seenInfantry;
    if (seen >= 4)
    {
        int riflemenLeft = 0;
        for (int arch : plan.archetypes)
            riflemenLeft += arch == kRifleman ? 1 : 0;
        if (seenVehicles * 10 >= seen * 6)
        {
            // Armour: rocket troopers in place of half of the riflemen
            int swaps = riflemenLeft / 2;
            for (int &arch : plan.archetypes)
            {
                if (swaps > 0 && arch == kRifleman)
                {
                    arch = kHeavyAssault;
                    --swaps;
                }
            }
        }
        else if (seenInfantry * 10 >= seen * 7 && plan.tier != AIWaveTier::Scout)
        {
            // Infantry: flamethrowers in place of a third of the riflemen, and an MLRS when there is a factory
            int swaps = riflemenLeft / 3;
            for (int &arch : plan.archetypes)
            {
                if (swaps > 0 && arch == kRifleman)
                {
                    arch = kFlamethrower;
                    --swaps;
                }
            }
            if ((hasFactory || hasFactoryPlan) && plan.tier != AIWaveTier::Vanguard)
                plan.archetypes.push_back(kGrad);
        }
    }
    // From the middle of the game on the plain riflemen are only a small share of a wave: the rest of them become
    // heavy infantry (rocket troopers / grenadiers). The vanguard waves are half way there.
    if (plan.tier != AIWaveTier::Scout)
    {
        const int percent = std::clamp(t.lateRiflemanPercent, 0, 100);
        const int allowedPercent = plan.tier == AIWaveTier::Vanguard ? (100 + percent) / 2 : percent;
        const int total = static_cast<int>(plan.archetypes.size() + core.size());
        int riflemenLeft = 0;
        for (int arch : plan.archetypes)
            riflemenLeft += arch == kRifleman ? 1 : 0;
        int swaps = riflemenLeft - total * allowedPercent / 100;
        // (from the end of the list: the first troops trained stay the cheap ones, the wave is not held up)
        for (auto it = plan.archetypes.rbegin(); it != plan.archetypes.rend() && swaps > 0; ++it)
        {
            if (*it == kRifleman)
            {
                *it = kHeavyAssault;
                --swaps;
            }
        }
    }
    plan.archetypes.insert(plan.archetypes.end(), core.begin(), core.end());
    return plan;
}

bool AIController::IsReserveUnit(const Unit &u) const
{
    if (!u.alive || u.state == ActionState::Die || u.owner != m_player)
        return false;
    if (u.moveSpeed <= 0.0f || u.isSiegeMode)
        return false;
    if (m_squadController.IsSquadMember(u.id))
        return false;
    if (m_garrison.count(u.id))
        return false;
    // The reserve is the computer's own list of troops (m_waveRoster), not whoever stands near its base
    return m_waveRoster.count(u.id) != 0;
}

void AIController::UpdateWaveRoster(EntityManager &entities)
{
    // Once, when the base is known: the troops that stand at the home base when the game begins (pre-placed outposts
    // further away stay where they are)
    if (!m_rosterSeeded && (m_enemyHqX > 0 || m_enemyHqY > 0))
    {
        m_rosterSeeded = true;
        for (const auto &e : entities.GetAllEntities())
        {
            const Unit *u = e.AsUnit();
            if (!u || !u->alive || u->state == ActionState::Die || u->owner != m_player || u->moveSpeed <= 0.0f)
                continue;
            if (m_garrison.count(u->id) || m_squadController.IsSquadMember(u->id))
                continue;
            if (DetMath::Hypot(static_cast<float>(u->tileX - m_enemyHqX), static_cast<float>(u->tileY - m_enemyHqY))
                <= 18.0f)
                m_waveRoster.insert(u->id);
        }
    }
    // Dead troops leave the list, and so do the ones a wave took (their squad holds them now)
    for (auto it = m_waveRoster.begin(); it != m_waveRoster.end();)
    {
        const Unit *u = entities.GetUnit(*it);
        if (!u || !u->alive || u->state == ActionState::Die || u->owner != m_player
            || m_squadController.IsSquadMember(*it))
            it = m_waveRoster.erase(it);
        else
            ++it;
    }
}

void AIController::UpdateUnitStances(EntityManager &entities)
{
    std::vector<int> own;
    for (const auto &e : entities.GetAllEntities())
    {
        if (e.alive && !e.isBuilding && e.owner == m_player && e.state != ActionState::Die)
            own.push_back(e.id);
    }
    for (int id : own)
    {
        Unit *u = entities.GetUnit(id);
        if (!u)
            continue;
        // Artillery and units with a siege form: Porcupine / Grad MLRS, Sniper, Rhino
        const int a = u->archetypeId;
        const bool artillery = a == 4 || a == 10 || a == 12 || a == 13 || a == 14 || a == 15 || a == 16;
        UnitStance wanted = UnitStance::PursueAndReturn;
        if (id == m_scoutId || m_repairing.count(id))
            wanted = UnitStance::HoldGround; // looks / pulls back, does not chase
        else if (artillery)
            wanted = UnitStance::HoldGround;
        else if (m_squadController.IsSquadMember(id))
            wanted = UnitStance::Pursue;
        if (u->stance != wanted)
        {
            u->stance = wanted;
            u->stanceHomeX = u->stanceHomeY = -1;
            u->stanceReturning = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Main update
// ---------------------------------------------------------------------------

void AIController::UpdateStagingPoint(const TileMap &map, Pathfinder &pathfinder, float deltaTime)
{
    m_stagingTimer -= deltaTime;
    if (m_stagingTimer > 0.0f)
        return;
    m_stagingTimer = 5.0f;

    // Fallback: 6 tiles from the enemy HQ straight toward the player
    float dxToPlayer = static_cast<float>(m_playerHqX - m_enemyHqX);
    float dyToPlayer = static_cast<float>(m_playerHqY - m_enemyHqY);
    float len = DetMath::Hypot(dxToPlayer, dyToPlayer);
    int stepX = (len > 0.001f) ? static_cast<int>(std::round((dxToPlayer / len) * 6.0f)) : -6;
    int stepY = (len > 0.001f) ? static_cast<int>(std::round((dyToPlayer / len) * 6.0f)) : 0;
    m_stagingX = std::clamp(m_enemyHqX + stepX, 3, std::max(3, map.GetWidth() - 4));
    m_stagingY = std::clamp(m_enemyHqY + stepY, 3, std::max(3, map.GetHeight() - 4));
    if (m_playerHqX <= 0 && m_playerHqY <= 0)
        return;

    // The waves gather on the road the base is actually left by (the straight line may point at the back
    // wall of the base while the exit is on the other side): a few tiles before the wall exit the road
    // crosses, or ~9 tiles out along the road when there is no wall
    constexpr int kBeforeGate = 4;
    constexpr int kAlongRoad = 9;
    std::vector<PathNode> road;
    bool partial = false;
    pathfinder.SetIgnoreUnitsOnce();
    if (!pathfinder.FindPath(m_enemyHqX, m_enemyHqY, m_playerHqX, m_playerHqY, road, 0, &partial) || road.empty())
        return;
    int pick = std::min(static_cast<int>(road.size()) - 1, kAlongRoad);
    for (int i = 0; i < static_cast<int>(road.size()) && i <= 24; ++i)
    {
        bool atGate = false;
        for (int g = 0; g < 2; ++g)
        {
            if (m_gateX[g] >= 0 && std::max(std::abs(road[i].x - m_gateX[g]), std::abs(road[i].y - m_gateY[g])) <= 1)
                atGate = true;
        }
        if (atGate)
        {
            pick = std::max(0, i - kBeforeGate);
            break;
        }
    }
    // Stay where the reserve lives (isReserveUnit: within 18 tiles of the HQ)
    while (
        pick > 0
        && DetMath::Hypot(static_cast<float>(road[pick].x - m_enemyHqX), static_cast<float>(road[pick].y - m_enemyHqY))
               > 12.0f)
        --pick;
    // The waves gather on open ground near that road tile, not against a cliff or in a corner of the base:
    // the tile around which most of a 7x7 square is free (ties: the nearest, in scan order)
    auto openAround = [&](int x, int y) {
        int free = 0;
        for (int dy = -3; dy <= 3; ++dy)
        {
            for (int dx = -3; dx <= 3; ++dx)
            {
                const int nx = x + dx, ny = y + dy;
                free += (nx >= 0 && ny >= 0 && nx < map.GetWidth() && ny < map.GetHeight() && map.IsPassable(nx, ny)
                         && map.GetWall(nx, ny) == 0)
                            ? 1
                            : 0;
            }
        }
        return free;
    };
    int bestX = road[pick].x, bestY = road[pick].y;
    int bestFree = openAround(bestX, bestY), bestDist = 0;
    for (int dy = -3; dy <= 3; ++dy)
    {
        for (int dx = -3; dx <= 3; ++dx)
        {
            const int x = road[pick].x + dx, y = road[pick].y + dy;
            if (x < 0 || y < 0 || x >= map.GetWidth() || y >= map.GetHeight() || !map.IsPassable(x, y)
                || map.GetWall(x, y) > 0)
                continue;
            const int free = openAround(x, y);
            const int dist = std::max(std::abs(dx), std::abs(dy));
            if (free > bestFree || (free == bestFree && dist < bestDist))
            {
                bestFree = free;
                bestDist = dist;
                bestX = x;
                bestY = y;
            }
        }
    }
    m_stagingX = std::clamp(bestX, 3, std::max(3, map.GetWidth() - 4));
    m_stagingY = std::clamp(bestY, 3, std::max(3, map.GetHeight() - 4));
}

void AIController::UpdateStaging(TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
{
    if (m_enemyHqX <= 0 && m_enemyHqY <= 0)
        return;
    pathfinder.SetEntityManager(&entities);

    // Fresh troops (and units that fell back) gather at the staging area instead of attacking piecemeal
    std::vector<int> toStage;
    for (const auto &e : entities.GetAllEntities())
    {
        const Unit *u = e.AsUnit();
        // (a guard stays at its post, a defender where the attack on the base is)
        if (!u || !IsReserveUnit(*u) || IsPostGuard(u->id) || m_defenders.count(u->id))
            continue;
        if (u->state == ActionState::Idle && u->targetEntityId == 0 && !u->isAttackMoving && !u->waitingForPath)
        {
            if (std::max(std::abs(u->tileX - m_stagingX), std::abs(u->tileY - m_stagingY)) > 5)
            {
                toStage.push_back(u->id);
            }
        }
    }
    if (toStage.empty())
        return;

    std::vector<StormcastSlot> slots = AINavigataionStormcastView::ComputeGroupSlots(
        m_stagingX, m_stagingY, static_cast<int>(toStage.size()), map, &entities, toStage);

    for (std::size_t i = 0; i < toStage.size() && i < slots.size(); ++i)
    {
        Unit *u = entities.GetUnit(toStage[i]);
        if (!u)
            continue;
        int slotX = slots[i].x;
        int slotY = slots[i].y;
        if (std::abs(u->tileX - slotX) > 1 || std::abs(u->tileY - slotY) > 1)
        {
            bool isPartial = false;
            bool found = pathfinder.FindPath(u->tileX, u->tileY, slotX, slotY, u->path, u->id, &isPartial);
            if (!found)
            {
                // Boxed in by its own crowd (troops standing about in a crowded base block each other): plan the
                // road through them, the units make way for each other when they walk
                pathfinder.SetIgnoreUnitsOnce();
                found = pathfinder.FindPath(u->tileX, u->tileY, slotX, slotY, u->path, u->id, &isPartial);
            }
            if (found)
            {
                u->pathIndex = 0;
                u->targetTileX = slotX;
                u->targetTileY = slotY;
                u->state = ActionState::Move;
                u->waitingForPath = isPartial;
            }
        }
    }
}

void AIController::FindAttackObjective(const EntityManager &entities, const TileMap &map, int &outX, int &outY) const
{
    outX = -1;
    outY = -1;
    if (m_noFoesLeft)
        return; // everybody hostile is out: nobody to attack
    float bestBldgDist = 1e9f;

    // The closest enemy structure the computer knows of (walls on the way get breached), otherwise the closest
    // enemy unit it sees, otherwise the enemy's starting position
    for (const auto &known : m_knownBuildings)
    {
        const int cx = known.second.first, cy = known.second.second;
        float d = DetMath::Hypot(static_cast<float>(cx - m_enemyHqX), static_cast<float>(cy - m_enemyHqY));
        if (d < bestBldgDist)
        {
            bestBldgDist = d;
            outX = cx;
            outY = cy;
        }
    }

    if (outX < 0)
    {
        float bestDist = 1e9f;
        const bool seesAll = GetEffectiveDifficulty() == AIDifficulty::Hardcore;
        for (const auto &e : entities.GetAllEntities())
        {
            if (!e.alive || e.state == ActionState::Die || !IsFoe(e) || (!seesAll && !AiSees(e, entities)))
                continue;
            float d
                = DetMath::Hypot(static_cast<float>(e.tileX - m_enemyHqX), static_cast<float>(e.tileY - m_enemyHqY));
            if (d < bestDist)
            {
                bestDist = d;
                outX = e.tileX;
                outY = e.tileY;
            }
        }
    }

    if (outX < 0 && m_playerHqX > 0 && m_playerHqY > 0)
    {
        outX = m_playerHqX;
        outY = m_playerHqY;
    }
    if (outX >= 0)
    {
        outX = std::clamp(outX, 1, map.GetWidth() - 2);
        outY = std::clamp(outY, 1, map.GetHeight() - 2);
    }
}

void AIController::UpdateWaveLaunch(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
{
    m_waveCooldown -= deltaTime;

    // Troops assembled at the staging area and ready to march
    std::vector<int> ready;
    for (const auto &e : entities.GetAllEntities())
    {
        const Unit *u = e.AsUnit();
        if (!u || !IsReserveUnit(*u) || IsPostGuard(u->id)) // (the guards at the posts never leave with a wave)
            continue;
        if (u->targetEntityId != 0 || u->isAttackMoving)
            continue;
        if (std::max(std::abs(u->tileX - m_stagingX), std::abs(u->tileY - m_stagingY)) <= 7)
        {
            ready.push_back(u->id);
        }
    }
    m_readyUnits = static_cast<int>(ready.size());
    // Troops that stand about idle in the base but never made it to the staging area (a crowded or walled-in base):
    // after a long wait the wave marches from where they are instead of waiting for ever
    std::vector<int> idleReserve;
    for (const auto &e : entities.GetAllEntities())
    {
        const Unit *u = e.AsUnit();
        if (u && IsReserveUnit(*u) && !IsPostGuard(u->id) && u->targetEntityId == 0 && !u->isAttackMoving
            && u->state == ActionState::Idle)
            idleReserve.push_back(u->id);
    }

    // No wave leaves while the base is attacked: its troops are the defenders. Not for ever (an enemy nobody can
    // reach must not pin the army down)
    if (m_baseAttackedTimer > 0.0f && m_baseAttackedSeconds < kWaveHoldMaxSeconds)
        return;

    const AIDifficulty difficulty = GetEffectiveDifficulty();
    // Hard/Hardcore dispatch the front of the reserve immediately, from its current
    // position. No rally-point, full-wave, minimum-size or inter-wave wait.
    if (difficulty == AIDifficulty::Hard || difficulty == AIDifficulty::Hardcore)
    {
        int targetX, targetY;
        FindAttackObjective(entities, map, targetX, targetY);
        if (targetX < 0)
            return;
        std::vector<int> front;
        for (const auto &e : entities.GetAllEntities())
        {
            const Unit *u = e.AsUnit();
            if (u && IsReserveUnit(*u) && !IsPostGuard(u->id) && !m_defenders.count(u->id) && !m_repairing.count(u->id)
                && u->id != m_scoutId && u->targetEntityId == 0 && !u->isAttackMoving && u->state != ActionState::Attack
                && u->state != ActionState::Transform)
                front.push_back(u->id);
        }
        if (front.empty())
            return;
        const long long dx = targetX - m_enemyHqX, dy = targetY - m_enemyHqY;
        std::sort(front.begin(), front.end(), [&](int a, int b) {
            const Unit *ua = entities.GetUnit(a), *ub = entities.GetUnit(b);
            const long long pa = ua->tileX * dx + ua->tileY * dy;
            const long long pb = ub->tileX * dx + ub->tileY * dy;
            return pa != pb ? pa > pb : a < b;
        });
        const Unit *leader = entities.GetUnit(front.front());
        const int lx = leader->tileX, ly = leader->tileY;
        std::vector<int> group;
        for (int id : front)
        {
            const Unit *u = entities.GetUnit(id);
            // Distant troops leave in their own group instead of pulling this one back to regroup.
            if (std::max(std::abs(u->tileX - lx), std::abs(u->tileY - ly)) <= m_currentDifficultyConfig.tactics.dispatchGroupRadius)
                group.push_back(id);
            if (static_cast<int>(group.size()) >= m_currentDifficultyConfig.tactics.dispatchGroupSize)
                break;
        }
        for (int id : group)
            entities.GetUnit(id)->groupHold = false;
        const int wave = m_waveCount + 1;
        const int squadId = m_squadController.CreateSquad(
            group, targetX, targetY, wave, entities, pathfinder, m_currentDifficultyConfig.tactics);
        if (squadId == 0)
            return;
        m_firstRaidDone = true;
        if (m_trackedWave == 0)
        {
            m_trackedWave = wave;
            m_trackedBuildingsKilled = entities.GetPlayerStats(m_player).buildingsDestroyed;
        }
        m_dispatchedInWave += static_cast<int>(group.size());
        m_readyUnits = static_cast<int>(front.size() - group.size());
        Think(
            "ATTACK",
            "Immediate front group G" + std::to_string(squadId) + " with " + std::to_string(group.size()) + " unit(s)");
        // Advance the composition after a wave's worth has left; small groups must
        // not advance the technology mix once per soldier or discard its queues.
        if (!m_currentPlan.archetypes.empty()
            && m_dispatchedInWave >= static_cast<int>(m_currentPlan.archetypes.size()))
        {
            ++m_waveCount;
            m_dispatchedInWave = 0;
            m_lastPlanSize = static_cast<int>(m_currentPlan.archetypes.size());
            m_currentPlan.archetypes.clear();
            m_waveAssemblyTimer = 0.0f;
        }
        return;
    }

    int required = static_cast<int>(m_currentPlan.archetypes.size());
    if (required == 0 || m_waveCooldown > 0.0f)
        return;

    // March only with 100% of the planned wave plus the home guard that stays; without production left, send
    // whatever is assembled
    // (the guards already at the posts count toward it: the rest of the home guard stays at the staging area)
    const int guard = std::max(0, HomeGuardCount() - static_cast<int>(m_guardOf.size()));
    bool full = static_cast<int>(ready.size()) >= required + guard;
    // A wave does not wait for its vehicles for ever: after the difficulty's wait it marches with what is there
    // (the vehicles still being built join the next wave)
    int plannedVehicles = 0, readyVehicles = 0;
    for (int arch : m_currentPlan.archetypes)
        plannedVehicles += IsVehicleArchetype(arch) ? 1 : 0;
    for (int id : ready)
        if (const Unit *ru = entities.GetUnit(id); ru && !ru->IsInfantry())
            ++readyVehicles;
    const int missingVehicles = std::max(0, plannedVehicles - readyVehicles);
    const bool vehicleTimeout
        = !full && missingVehicles > 0 && m_waveAssemblyTimer > m_currentDifficultyConfig.tactics.vehicleWaitSeconds
          && static_cast<int>(ready.size()) >= std::max(kMinAttackWave, required - missingVehicles + guard);
    // Without production left the AI sends what is assembled, but still not a handful: ten troops, or after a long wait
    bool stranded = !m_hasProduction && !ready.empty()
                    && ((static_cast<int>(ready.size()) >= kMinAttackWave && m_waveAssemblyTimer > 20.0f)
                        || m_waveAssemblyTimer > 90.0f);
    constexpr float kStagingGiveUpSeconds = 120.0f;
    const bool lagging = !full && m_waveAssemblyTimer > kStagingGiveUpSeconds
                         && static_cast<int>(idleReserve.size()) >= std::max(kMinAttackWave, required * 3 / 4);
    if (lagging)
    {
        ready = idleReserve; // ... but for the home guard
        const int send = std::max(kMinAttackWave, static_cast<int>(ready.size()) - guard);
        if (static_cast<int>(ready.size()) > send)
            ready.resize(static_cast<std::size_t>(send));
    }
    if (!full && !stranded && !vehicleTimeout && !lagging)
        return;

    int targetX, targetY;
    FindAttackObjective(entities, map, targetX, targetY);
    if (targetX < 0)
        return;

    // More troops ready than planned: the plan's unit types first, then the rest in list order
    if (static_cast<int>(ready.size()) > required)
    {
        std::vector<int> picked;
        std::vector<bool> used(ready.size(), false);
        for (int arch : m_currentPlan.archetypes)
        {
            const int wanted = SideArchetype(arch, side());
            for (std::size_t i = 0; i < ready.size(); ++i)
            {
                const Unit *u = entities.GetUnit(ready[i]);
                if (!used[i] && u && u->archetypeId == wanted)
                {
                    used[i] = true;
                    picked.push_back(ready[i]);
                    break;
                }
            }
        }
        for (std::size_t i = 0; i < ready.size() && static_cast<int>(picked.size()) < required; ++i)
        {
            if (!used[i])
            {
                used[i] = true;
                picked.push_back(ready[i]);
            }
        }
        if (static_cast<int>(picked.size()) > required)
            picked.resize(required);
        ready = std::move(picked);
    }

    m_waveCount++;
    if (m_trackedWave == 0)
    { // this wave is watched: did it destroy anything by the time its groups are gone?
        m_trackedWave = m_waveCount;
        m_trackedBuildingsKilled = entities.GetPlayerStats(m_player).buildingsDestroyed;
    }
    {
        std::ostringstream why;
        why << "Wave " << m_waveCount << " is " << (full ? "complete" : "all that is left") << ", treasury $"
            << m_credits << " -> splitting into " << AttackGroupCount(static_cast<int>(ready.size()), map)
            << " group(s)";
        Think("ATTACK", why.str());
    }

    // The wave splits into 1..5 groups: the first goes straight, the others take flank routes
    // (through the second base exit when there is one) so the player is hit from several sides
    const int groups = AttackGroupCount(static_cast<int>(ready.size()), map);
    std::vector<int> byType = ready;
    std::stable_sort(byType.begin(), byType.end(), [&](int a, int b) {
        return entities.GetUnit(a)->archetypeId < entities.GetUnit(b)->archetypeId;
    });
    std::vector<std::vector<int>> members(static_cast<std::size_t>(groups));
    for (std::size_t i = 0; i < byType.size(); ++i)
        members[i % groups].push_back(byType[i]); // mixed groups
    for (int g = 0; g < groups; ++g)
    {
        int viaX = -1, viaY = -1;
        if (g > 0)
            FlankPoint(g, targetX, targetY, map, viaX, viaY);
        int squadId = m_squadController.CreateSquad(
            members[g],
            targetX,
            targetY,
            m_waveCount,
            entities,
            pathfinder,
            m_currentDifficultyConfig.tactics,
            viaX,
            viaY);
        std::ostringstream msg;
        msg << "Wave " << m_waveCount << " (" << TierName(m_currentPlan.tier) << ", "
            << DifficultyToString(GetEffectiveDifficulty()) << ") group G" << squadId << " " << (g + 1) << "/" << groups
            << " with " << members[g].size() << " units toward (" << targetX << ", " << targetY << ")";
        if (viaX >= 0)
            msg << " via flank (" << viaX << ", " << viaY << ")";
        Think("ATTACK", msg.str());
    }

    m_waveCooldown = m_currentDifficultyConfig.tactics.waveIntervalSeconds;
    m_waveAssemblyTimer = 0.0f;
    m_lastPlanSize = required;        // (the home guard is counted by it until the next plan is there)
    m_currentPlan.archetypes.clear(); // next plan is prepared on the next economy tick
    m_infantryQueue.clear();
    m_vehicleQueue.clear();
}

bool AIController::Escalates() const
{
    const AIDifficulty difficulty = GetEffectiveDifficulty();
    return (difficulty == AIDifficulty::Hard || difficulty == AIDifficulty::Hardcore) && m_failedWaves > 0;
}

int AIController::EscalationTroops() const
{
    if (!Escalates())
        return 0;
    // Three more troops for every wave that broke nothing, but only what the treasury pays for above a reserve for
    // the base (rebuilding, defense, research): the escalation never drains the economy
    constexpr int kBaseReserve = 600, kTroopCost = 150;
    const int affordable = std::max(0, (m_credits - kBaseReserve) / kTroopCost);
    return std::min(3 * m_failedWaves, affordable);
}

void AIController::UpdateWaveOutcome(const EntityManager &entities)
{
    if (m_trackedWave == 0)
        return;
    for (const AISquad &sq : m_squadController.GetSquads())
        if (sq.waveNumber == m_trackedWave && !sq.unitIds.empty())
            return; // still under way
    const int destroyed = entities.GetPlayerStats(m_player).buildingsDestroyed - m_trackedBuildingsKilled;
    if (destroyed > 0)
    {
        if (m_failedWaves > 0)
            Think("ATTACK", "Wave " + std::to_string(m_trackedWave) + " broke through: back to the usual waves");
        m_failedWaves = 0;
    }
    else
    {
        m_failedWaves = std::min(m_failedWaves + 1, 4);
        if (Escalates())
            Think(
                "ATTACK",
                "Wave " + std::to_string(m_trackedWave) + " broke nothing (" + std::to_string(m_failedWaves)
                    + " in a row): the next ones get more groups and troops, as far as the treasury allows");
    }
    m_trackedWave = 0;
}

int AIController::AttackGroupCount(int readyUnits, const TileMap &map) const
{
    const AITacticsConfig &t = m_currentDifficultyConfig.tactics;
    // Flank groups only on Hard / Hardcore and with six units or more; a smaller wave stays together
    const AIDifficulty difficulty = GetEffectiveDifficulty();
    if ((difficulty != AIDifficulty::Hard && difficulty != AIDifficulty::Hardcore) || readyUnits < 6)
        return 1;
    int groups = 1 + m_credits / std::max(1, t.creditsPerExtraGroup); // richer AI fields more groups
    if (std::sqrt(static_cast<float>(map.GetWidth() * map.GetHeight())) >= 80.0f)
        ++groups; // room to flank
    if (m_gateX[1] >= 0)
        groups = std::max(groups, 2); // a second exit is used every time
    // Waves that broke nothing: another route or two for the next one
    const int extra = Escalates() ? std::min(m_failedWaves, 2) : 0;
    groups += extra;
    groups = std::min(groups, std::clamp(t.maxAttackGroups + extra, 1, 5));
    groups = std::min(groups, readyUnits / kMinAttackGroup); // never a squad of two or three
    return std::max(1, groups);
}

void AIController::FlankPoint(int group, int targetX, int targetY, const TileMap &map, int &outX, int &outY) const
{
    outX = outY = -1;
    // Group 1 leaves through the second exit of the base wall when it has one
    if (group == 1 && m_gateX[1] >= 0)
    {
        outX = m_gateX[1];
        outY = m_gateY[1];
        return;
    }
    // Otherwise a point beside the straight line base -> target: left, right, wide left, wide right
    const float dx = static_cast<float>(targetX - m_enemyHqX);
    const float dy = static_cast<float>(targetY - m_enemyHqY);
    const float len = std::max(1.0f, DetMath::Hypot(dx, dy));
    const float side = (group % 2 == 1) ? -1.0f : 1.0f;
    const float spread = std::clamp(len * (group >= 3 ? 0.45f : 0.3f), 6.0f, 24.0f);
    const float mx = m_enemyHqX + dx * 0.55f + (-dy / len) * spread * side;
    const float my = m_enemyHqY + dy * 0.55f + (dx / len) * spread * side;
    const int cx = std::clamp(static_cast<int>(std::lround(mx)), 1, map.GetWidth() - 2);
    const int cy = std::clamp(static_cast<int>(std::lround(my)), 1, map.GetHeight() - 2);
    // Nearest open tile around that point
    for (int r = 0; r <= 6; ++r)
    {
        for (int oy = -r; oy <= r; ++oy)
        {
            for (int ox = -r; ox <= r; ++ox)
            {
                if (std::max(std::abs(ox), std::abs(oy)) != r)
                    continue;
                const int x = cx + ox, y = cy + oy;
                if (x < 1 || y < 1 || x >= map.GetWidth() - 1 || y >= map.GetHeight() - 1)
                    continue;
                if (map.IsPassable(x, y) && map.GetWall(x, y) == 0)
                {
                    outX = x;
                    outY = y;
                    return;
                }
            }
        }
    }
}

} // namespace OpenArtOfWar
