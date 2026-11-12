#include "graphics/RenderUtility.hpp"
#include "game/Capabilities.hpp"
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

void AIController::UpdateEconomyAndProduction(float /*deltaTime*/, TileMap &map, EntityManager &entities)
{
    // Like a person the computer orders units into the queue of a building (and pays for them then): it keeps up to
    // this many orders in each Barracks / Factory, so a building never stands idle between two of its decisions
    constexpr std::size_t kQueueDepth = 2;
    bool hasHq = false;
    bool hasBarracks = false;
    bool hasFactory = false;
    bool hasFactoryPlan = false;
    for (const auto &p : m_startingBuildings)
    {
        if (p.type == UnitType::Factory)
            hasFactoryPlan = true;
    }
    std::vector<Building *> availableBarracks, availableFactories; // up to three of each

    const auto difficulty = GetEffectiveDifficulty();
    const bool continuous = difficulty == AIDifficulty::Hard || difficulty == AIDifficulty::Hardcore;
    int reserveCount = 0;
    int enemyUnitCount = 0;
    for (auto &e : entities.GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die || e.owner != m_player)
            continue;
        if (Building *b = e.AsBuilding())
        {
            if (b->type == UnitType::Headquarters)
            {
                hasHq = hasHq || b->buildProgress >= 1.0f;
            }
            else if (b->type == UnitType::Barracks)
            {
                hasBarracks = true;
                enemyUnitCount += static_cast<int>(b->productionQueue.size()); // (ordered troops count to the army cap)
                if (b->buildProgress >= 1.0f && b->isPowered && b->productionQueue.size() < kQueueDepth)
                    availableBarracks.push_back(b);
            }
            else if (b->type == UnitType::Factory)
            {
                hasFactory = true;
                enemyUnitCount += static_cast<int>(b->productionQueue.size());
                if (b->buildProgress >= 1.0f && b->isPowered && b->productionQueue.size() < kQueueDepth)
                    availableFactories.push_back(b);
            }
        }
        else
        {
            enemyUnitCount++;
            if (const Unit *u = e.AsUnit();
                u && IsReserveUnit(*u) && !m_recalled.count(u->id)) // (troops still on the way back do not count)
                ++reserveCount;
        }
    }
    m_hasProduction = hasBarracks || hasFactory;

    // The first finished factory: a wave planned without it has no vehicles, make the plan again with them
    bool factoryDone = false;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *fb = e.AsBuilding();
        if (fb && fb->alive && fb->owner == m_player && fb->type == UnitType::Factory && fb->buildProgress >= 1.0f)
            factoryDone = true;
    }
    if (factoryDone && !m_factoryReady)
    {
        m_factoryReady = true;
        bool planHasVehicles = false;
        for (int arch : m_currentPlan.archetypes)
            planHasVehicles = planHasVehicles || IsVehicleArchetype(arch);
        if (!planHasVehicles && !m_currentPlan.archetypes.empty())
            PreparePlan(m_waveCount + 1, true, true);
    }

    if (m_currentPlan.archetypes.empty())
    {
        PreparePlan(m_waveCount + 1, hasFactory, hasFactoryPlan);
    }

    if (!hasHq && !hasBarracks && !hasFactory)
    {
        return; // All enemy production structures destroyed
    }

    if (hasHq) // the income is the headquarters': every finished one, up to three
        m_credits
            += m_creditsPerSec
               * std::min(
                   Economy::kMaxIncomeHeadquarters, entities.CountBuildingType(UnitType::Headquarters, m_player, true));

    // Wave readiness controls when troops march, never whether factories keep working.
    // Refill each production line independently, including a factory in an infantry-only wave.
    // A free factory beyond the plan builds a mix that grows heavier with the wave tier, not one light vehicle
    // over and over (it never depends on the seed: the same wave tier gives the same mix)
    const auto extraVehicle = [&]() {
        static const int kEarly[] = {kHammer, kFortress};
        static const int kLate[] = {kHammer, kZeus, kHammer, kFortress, kGrad};
        const bool late = m_currentPlan.tier >= AIWaveTier::Assault;
        const int *mix = late ? kLate : kEarly;
        const int count = late ? 5 : 2;
        for (int i = 0; i < count; ++i)
        {
            const int arch = mix[(m_extraVehicles + i) % count];
            if (IsUnitUnlocked(m_player, SideArchetype(arch, side())))
            {
                m_extraVehicles = (m_extraVehicles + i + 1) % count;
                return arch;
            }
        }
        ++m_extraVehicles;
        return kFortress;
    };
    const auto refill = [&](std::vector<int> &queue, bool vehicles, std::size_t idleBuildings) {
        if (!queue.empty() || idleBuildings == 0)
            return;
        for (int arch : m_currentPlan.archetypes)
            if (IsVehicleArchetype(arch) == vehicles)
                queue.push_back(arch);
        while (queue.size() < idleBuildings)
            queue.push_back(vehicles ? extraVehicle() : kRifleman);
    };
    if (continuous)
    {
        refill(m_infantryQueue, false, availableBarracks.size());
        refill(m_vehicleQueue, true, availableFactories.size());
    }
    else if (m_infantryQueue.empty() && m_vehicleQueue.empty())
    {
        const int needed = static_cast<int>(m_currentPlan.archetypes.size()) + HomeGuardCount();
        for (int i = reserveCount; i < needed; ++i)
            m_infantryQueue.push_back(kRifleman);
    }

    // Use the same configured army limit as human players, not AI-only limits of 40/52.
    const int armyCap = BuildLimits::EffectiveArmyCap(ArmyCapUnitsOf(Players::Slot(m_player).armyCap));
    if ((armyCap > 0 && enemyUnitCount >= armyCap) || (!continuous && enemyUnitCount >= 52))
        return;

    float speedMult = m_currentDifficultyConfig.productionSpeedMultiplier;
    if (speedMult <= 0.01f)
        speedMult = 1.0f;
    const float barracksBuildTime = 6.0f / speedMult;
    const float factoryBuildTime = 10.0f / speedMult;

    auto costOf = [&](int arch) {
        const UnitConfig *cfg = entities.GetUnitConfig(SideArchetype(arch, side()));
        return cfg ? cfg->creditCost : FallbackArchetypeCost(arch);
    };

    // Count only ground reachable from the exit: cliffs must not make a narrow
    // corridor look like an empty field. Allied armies share this capacity.
    const auto crowdedExit = [&](int sx, int sy) {
        constexpr int radius = 4, width = radius * 2 + 1;
        bool seen[width][width] {};
        std::vector<std::pair<int, int>> tiles {{sx, sy}};
        seen[radius][radius] = true;
        int occupied = 0;
        for (std::size_t i = 0; i < tiles.size(); ++i)
        {
            const auto [x, y] = tiles[i];
            occupied += entities.IsTileOccupiedByUnit(x, y) ? 1 : 0;
            for (const auto [dx, dy] : {std::pair<int, int> {1, 0}, {-1, 0}, {0, 1}, {0, -1}})
            {
                const int nx = x + dx, ny = y + dy;
                const int ix = nx - sx + radius, iy = ny - sy + radius;
                if (ix < 0 || iy < 0 || ix >= width || iy >= width || seen[iy][ix] || !map.IsPassable(nx, ny)
                    || map.GetWall(nx, ny) != 0)
                    continue;
                seen[iy][ix] = true;
                tiles.emplace_back(nx, ny);
            }
        }
        int stalled = 0;
        for (const auto &e : entities.GetAllEntities())
        {
            const Unit *u = e.AsUnit();
            if (!u || !u->alive || u->state == ActionState::Die || Players::Hostile(u->owner, m_player))
                continue;
            const int ix = u->tileX - sx + radius, iy = u->tileY - sy + radius;
            if (ix >= 0 && iy >= 0 && ix < width && iy < width && seen[iy][ix] && u->blockedTimer >= 3.0f)
                ++stalled;
        }
        return stalled >= 2 || (occupied >= 3 && occupied * 2 >= static_cast<int>(tiles.size()));
    };
    const int researchReserve = !continuous && m_hasLab && !m_researchActive && m_researchIndex < kResearchCount
                                    ? researchDef(side(), m_researchIndex).cost
                                    : 0;

    auto produce = [&](Building *building, std::vector<int> &queue, float buildTime, int savingFor = 0) {
        if (!building || queue.empty() || (armyCap > 0 && enemyUnitCount >= armyCap))
            return;
        int arch = queue.front();
        // A unit its technology has not opened yet (a sniper, a heavy tank) cannot be trained, by the computer no
        // more than by a person: the slot takes the simplest unit of the same kind that is open
        if (!IsUnitUnlocked(m_player, SideArchetype(arch, side())))
        {
            const int infantry[] = {kRifleman, kHeavyAssault, kFlamethrower};
            const int vehicles[] = {kHammer, kZeus, kFortress}; // (a locked MLRS gives a tank, not the lightest)
            int instead = -1;
            if (IsVehicleArchetype(arch))
            {
                for (int v : vehicles)
                    if (instead < 0 && IsUnitUnlocked(m_player, SideArchetype(v, side())))
                        instead = v;
            }
            else
            {
                for (int v : infantry)
                    if (instead < 0 && IsUnitUnlocked(m_player, SideArchetype(v, side())))
                        instead = v;
            }
            if (instead < 0)
            {
                queue.erase(queue.begin());
                return;
            }
            const UnitConfig *lockedCfg = entities.GetUnitConfig(SideArchetype(arch, side()));
            ThinkOnce(
                "locked" + std::to_string(arch),
                "WAVE",
                std::string(lockedCfg ? lockedCfg->name : "A unit")
                    + " is not researched yet: training another instead");
            queue.front() = arch = instead;
        }
        int cost = costOf(arch);
        if (m_credits - cost < savingFor + researchReserve)
        {
            ThinkOnce("economy", "ECONOMY", "Not enough credits to train the next unit");
            return;
        }
        ThinkOnce("economy", "ECONOMY", "Training troops for the next wave");
        // The exit must be free to take the unit when it is done: not an exit a crowd of stalled troops already shuts
        if (building->productionQueue.empty())
        {
            int sx = building->tileX;
            int sy = building->tileY;
            if (!entities.GetBuildingSpawnPoint(building->id, map, sx, sy) || crowdedExit(sx, sy))
                return;
        }
        ProductionItem item;
        item.archetype = SideArchetype(arch, side());
        item.cost = cost;
        item.totalTime = std::max(0.01f, buildTime);
        item.progressTime = 0.0f;
        building->productionQueue.push_back(item);
        building->isProducing = true;
        m_credits -= cost;
        queue.erase(queue.begin());
        ++enemyUnitCount;
    };

    // Factories spend first so cheap infantry cannot consume the tank budget.
    for (Building *factory : availableFactories)
    {
        const std::size_t before = m_vehicleQueue.size();
        produce(factory, m_vehicleQueue, factoryBuildTime);
        if (m_vehicleQueue.size() != before)
            m_vehicleHoldSeconds = 0.0f;
    }

    // Give up reserving money only while every idle factory exit is blocked.
    // Waiting for credits must never expire the reservation: that starves vehicles
    // indefinitely when the barracks spend the income every second.
    constexpr float kVehicleHoldMax = 25.0f;
    bool vehicleWaits = false, exitAvailable = false;
    for (Building *factory : availableFactories)
    {
        if (m_vehicleQueue.empty())
            continue;
        vehicleWaits = true;
        int sx = factory->tileX, sy = factory->tileY;
        exitAvailable
            = (entities.GetBuildingSpawnPoint(factory->id, map, sx, sy) && !crowdedExit(sx, sy)) || exitAvailable;
    }
    if (vehicleWaits && !exitAvailable)
        m_vehicleHoldSeconds += 1.0f;
    else
        m_vehicleHoldSeconds = 0.0f;
    const int savingFor = vehicleWaits && m_vehicleHoldSeconds < kVehicleHoldMax ? costOf(m_vehicleQueue.front()) : 0;
    for (Building *barracks : availableBarracks)
        if (continuous || enemyUnitCount < 40)
            produce(barracks, m_infantryQueue, barracksBuildTime, savingFor);
}

void AIController::UpdateResearch(float deltaTime, EntityManager &entities)
{
    const Faction faction = side();
    // Skip technologies that only make sense for the player, and those its tech level leaves out
    while (m_researchIndex < kResearchCount
           && (researchDef(faction, m_researchIndex).playerOnly
               || !Capabilities::AllowsResearch(m_player, m_researchIndex)))
    {
        ++m_researchIndex;
    }
    if (m_researchIndex >= kResearchCount)
        return;

    // Research needs the Tech Center / Laboratory; it pauses while there is none
    m_hasLab = HasResearchBuilding(m_player, entities);
    const ResearchDef &def = researchDef(faction, m_researchIndex);
    if (!m_hasLab)
    {
        ThinkOnce(
            "research", "RESEARCH", std::string("No Tech Center / Laboratory: research paused before ") + def.name);
        return;
    }

    if (!m_researchActive)
    {
        if (m_credits >= def.cost)
        {
            m_credits -= def.cost;
            m_researchActive = true;
            m_researchTimer = 0.0f;
            ThinkOnce(
                "research",
                "RESEARCH",
                std::string("Research started: ") + def.name + " ($" + std::to_string(def.cost) + ")");
        }
        else
        {
            ThinkOnce(
                "research",
                "RESEARCH",
                std::string("Saving credits for ") + def.name + " ($" + std::to_string(def.cost) + ")");
        }
        return;
    }

    m_researchTimer += deltaTime;
    if (m_researchTimer >= std::max(1.0f, m_currentDifficultyConfig.tactics.researchSeconds))
    {
        CompleteResearch(m_player, m_researchIndex, entities);
        m_researchActive = false;
        m_researchTimer = 0.0f;
        ThinkOnce("research", "RESEARCH", std::string("Research completed: ") + def.name);
        ++m_researchIndex;
    }
}

float AIController::HomeGuardShare() const
{
    switch (GetEffectiveDifficulty())
    {
        case AIDifficulty::Easy:
            return 0.40f;
        case AIDifficulty::Medium:
            return 0.30f;
        case AIDifficulty::Hard:
            return 0.20f;
        case AIDifficulty::Hardcore:
            return 0.10f;
    }
    return 0.30f;
}

int AIController::HomeGuardCount() const
{
    // Between two plans (a wave just left) the last one counts: the guard is never zero for that second
    const int wave
        = m_currentPlan.archetypes.empty() ? m_lastPlanSize : static_cast<int>(m_currentPlan.archetypes.size());
    return static_cast<int>(std::lround(static_cast<float>(wave) * HomeGuardShare()));
}

void AIController::UpdateRepairs(float deltaTime, EntityManager &entities)
{
    const AIDifficulty difficulty = GetEffectiveDifficulty();
    if (difficulty != AIDifficulty::Hard && difficulty != AIDifficulty::Hardcore)
        return;
    constexpr int kKeepForTroops = 150;
    std::vector<int> damaged;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->owner == m_player && b->rawBuildingType < 15 && b->CanBeRepaired())
            damaged.push_back(b->id);
    }
    for (int id : damaged)
    {
        Building *b = entities.GetBuilding(id);
        if (!b)
            continue;
        // Under fire repairs are wasted: wait until the attack is over
        b->repairing = b->lastAttackedTimer <= 0.0f && m_credits > kKeepForTroops;
        if (!b->repairing)
            continue;
        const BuildingConfig *cfg = entities.GetBuildingConfig(b->rawBuildingType);
        m_credits -= b->RepairStep(deltaTime, cfg ? cfg->creditCost : 0, m_credits - kKeepForTroops);
    }
}

int AIController::PowerBalance(const EntityManager &entities, bool plannedPlants) const
{
    int produced = 0, consumed = 0;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->owner != m_player)
            continue;
        const int pwr = b->GetPowerOutput();
        if (pwr < 0)
            consumed -= pwr;
        else if (b->buildProgress >= 1.0f || plannedPlants)
            produced += pwr;
    }
    produced += produced * ResearchTotal(m_player, ResearchEffect::PowerBonus) / 100;
    return produced - consumed;
}

bool AIController::UpdateCoreBuildOrder(TileMap &map, EntityManager &entities, bool full)
{
    // Power as planned: buildings under construction count already, so one shortage starts one plant
    int produced = 0, consumed = 0;
    int barracksCount = 0, factoryCount = 0, labCount = 0, hqCount = 0; // the AI may keep up to three of each
    int barracksDone = 0, factoryDone = 0, labDone = 0;
    constexpr int kMaxProduction = 3;
    const Building *hq = nullptr;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->owner != m_player)
            continue;
        const int pwr = b->GetPowerOutput();
        (pwr > 0 ? produced : consumed) += std::abs(pwr);
        const bool done = b->buildProgress >= 1.0f;
        barracksCount += b->type == UnitType::Barracks;
        factoryCount += b->type == UnitType::Factory;
        labCount += b->type == UnitType::ResearchLab;
        hqCount += b->type == UnitType::Headquarters;
        barracksDone += b->type == UnitType::Barracks && done;
        factoryDone += b->type == UnitType::Factory && done;
        labDone += b->type == UnitType::ResearchLab && done;
        if (b->type == UnitType::Headquarters && done && !hq)
            hq = b;
    }
    if (!hq)
        return false; // no finished HQ, no construction
    // A structure the rebuilding memory will put up again does not need a second one elsewhere
    auto rebuildPending = [&](UnitType t) {
        for (const AIStructurePlan &plan : m_startingBuildings)
            if (plan.destroyed && plan.type == t && plan.rebuildTimer < 30.0f) // a spot that stays blocked is given up
                return true;
        return false;
    };
    const int sparePower = m_style.sparePower;
    constexpr int kTroopReserve = 300;
    // The tech level (data/capabilities.yaml) may leave a kind out: it is not wanted, and not waited for
    auto allowed = [&](AIDefenseSite::Kind kind) {
        return IsBuildingUnlocked(m_player, FactionBuildingIndex(kind, side()));
    };
    const bool needLab = labCount == 0 && !rebuildPending(UnitType::ResearchLab) && allowed(AIDefenseSite::Lab);

    // The base is ready for another headquarters (more income): everything works, the defense stands, there is
    // money to spare and the base has had peace for a minute
    auto baseReady = [&]() {
        if (!full || hqCount >= Economy::kMaxIncomeHeadquarters || barracksDone == 0
            || (factoryDone == 0 && allowed(AIDefenseSite::Factory)) || (labDone == 0 && allowed(AIDefenseSite::Lab)))
            return false;
        if (produced < consumed + sparePower || m_baseQuietSeconds < 60.0f)
            return false;
        for (const AIDefenseSite &site : m_defensePlan)
            if (!site.done && site.kind != AIDefenseSite::Wall)
                return false;
        return m_credits >= DefenseCost(AIDefenseSite::Headquarters, entities, side()) + 2 * kTroopReserve;
    };

    // What the base needs, most urgent first. A thing that has no room is passed over for the next one (a full back
    // of the base must not stop the barracks); a thing that waits for money stops the list (it comes first)
    struct Want
    {
        AIDefenseSite::Kind kind;
        int reserve;
        const char *why;
    };
    std::vector<Want> wants;
    const int powerCost = DefenseCost(AIDefenseSite::PowerPlant, entities, side());
    // Looking ahead: with the plants already being built, is there power for what waits for it?
    const int plannedBalance = PowerBalance(entities, true);
    if (plannedBalance < 0)
        wants.push_back({AIDefenseSite::PowerPlant, 0, "Power is short: a power plant comes first"});
    else if (m_powerNeeded > 0 && plannedBalance < m_powerNeeded)
        wants.push_back(
            {AIDefenseSite::PowerPlant,
             0,
             "The next building would leave the base without power: a power plant first"});
    if (barracksCount == 0)
        wants.push_back({AIDefenseSite::Barracks, 0, "No barracks: raising one to start training troops"});
    if (needLab && m_style.labEarly)
        wants.push_back(
            {AIDefenseSite::Lab, kTroopReserve / 2, "No laboratory: research needs one (early in this base's plan)"});
    if (full && m_style.barracksFirst && barracksCount < 2 && factoryCount == 0)
        wants.push_back(
            {AIDefenseSite::Barracks, kTroopReserve / 2, "A second barracks before the factory: infantry first"});
    if (factoryCount == 0)
        wants.push_back({AIDefenseSite::Factory, kTroopReserve / 2, "No factory: building one for vehicles"});
    if (needLab)
        wants.push_back({AIDefenseSite::Lab, kTroopReserve / 2, "No laboratory: research needs one"});
    // (spare power after the laboratory: the plants would take the back of the base it belongs in)
    if (produced - consumed < sparePower && m_credits >= powerCost + kTroopReserve)
        wants.push_back(
            {AIDefenseSite::PowerPlant,
             kTroopReserve,
             "Credits to spare: an extra power plant for the next buildings"});
    if (baseReady())
        wants.push_back(
            {AIDefenseSite::Headquarters,
             2 * kTroopReserve,
             "The base is ready and quiet: another headquarters for more income"});
    if (full && barracksCount < kMaxProduction && barracksCount <= factoryCount)
        wants.push_back(
            {AIDefenseSite::Barracks, kTroopReserve, "Credits to spare: another barracks to raise the army faster"});
    if (full && factoryCount < kMaxProduction)
        wants.push_back({AIDefenseSite::Factory, kTroopReserve, "Credits to spare: another factory for more vehicles"});

    wants.erase(
        std::remove_if(wants.begin(), wants.end(), [&](const Want &w) { return !allowed(w.kind); }), wants.end());

    if (m_homeX < 0)
    {
        m_homeX = hq->tileX + hq->buildWidth / 2;
        m_homeY = hq->tileY + hq->buildHeight / 2;
    }
    const int cx = m_homeX, cy = m_homeY;
    float tdx = static_cast<float>(m_playerHqX - cx), tdy = static_cast<float>(m_playerHqY - cy);
    const float tlen = std::max(1.0f, DetMath::Hypot(tdx, tdy));
    tdx /= tlen;
    tdy /= tlen;
    const float turn = m_style.turnDeg * static_cast<float>(M_PI) / 180.0f;
    const float tc = DetMath::Cos(turn), ts = DetMath::Sin(turn);
    float sideX = -tdy, sideY = tdx;
    if (m_style.mirror)
    {
        sideX = -sideX;
        sideY = -sideY;
    }

    for (const Want &want : wants)
    {
        const AIDefenseSite::Kind kind = want.kind;
        const int cost = DefenseCost(kind, entities, side());
        if (m_credits - cost < want.reserve)
        {
            ThinkOnce("core", "BUILD", std::string(want.why) + " (waiting for credits)");
            return false;
        }
        const int existing
            = kind == AIDefenseSite::Barracks ? barracksCount : (kind == AIDefenseSite::Factory ? factoryCount : 0);
        const int index = FactionBuildingIndex(kind, side());
        const BuildingConfig *cfg = entities.GetBuildingConfig(index);
        const int w = cfg ? cfg->width : 2, h = cfg ? cfg->height : 2;
        // Power plants, the laboratory and another HQ behind the HQ, away from the enemy (the power must not be the
        // first thing an attack meets); barracks and factory to its sides, turned and swapped as the style says
        float ox = -tdx, oy = -tdy; // behind
        bool rear = true;
        int minRing = std::max(hq->buildWidth, hq->buildHeight) / 2 + 2;
        if (kind == AIDefenseSite::Barracks || kind == AIDefenseSite::Factory)
        {
            const float sgn = kind == AIDefenseSite::Barracks ? 1.0f : -1.0f;
            ox = (sideX * tc - sideY * ts) * sgn;
            oy = (sideX * ts + sideY * tc) * sgn;
            if (m_style.layout == AIBaseStyle::Road)
            { // along the road out of the base: a little toward the back
                ox -= tdx * 0.5f;
                oy -= tdy * 0.5f;
            }
            if (existing > 0)
            { // the next ones turn towards the back of the base, so they do not stand in each other's way
                ox += -tdx * 0.8f * existing;
                oy += -tdy * 0.8f * existing;
            }
            rear = false;
        }
        // A new HQ widens the building zone: as far from the first one as the zone allows, counted from the
        // headquarters that stands farthest out already (the base grows away from where it began); nearer only
        // when there is no room out there
        std::vector<int> rings {minRing};
        int centreX = -1, centreY = -1;
        if (kind == AIDefenseSite::Headquarters)
        {
            minRing += 5; // never right beside the old one
            float farthest = 0.0f;
            for (const auto &e : entities.GetAllEntities())
            {
                const Building *base = e.AsBuilding();
                if (!base || !base->alive || base->state == ActionState::Die || base->owner != m_player
                    || base->type != UnitType::Headquarters || base->buildProgress < 1.0f)
                    continue;
                const int bx = base->tileX + base->buildWidth / 2, by = base->tileY + base->buildHeight / 2;
                const float d = DetMath::Hypot(static_cast<float>(bx - cx), static_cast<float>(by - cy));
                if (d > farthest)
                {
                    farthest = d;
                    centreX = bx;
                    centreY = by;
                }
            }
            if (centreX >= 0)
            { // on from there, away from the first headquarters
                ox = static_cast<float>(centreX - cx);
                oy = static_cast<float>(centreY - cy);
            }
            // (FindCoreSite looks outward from the ring it is given: far out, half way, then the nearest allowed)
            const int farRing = entities.GetBuildRadius(m_player) - std::max(w, h) / 2 - 1;
            rings.clear();
            for (int ring : {farRing, (farRing + minRing) / 2})
                if (ring > minRing && (rings.empty() || rings.back() != ring))
                    rings.push_back(ring);
            rings.push_back(minRing);
        }

        int x = 0, y = 0;
        bool found = false;
        const bool needsExit
            = kind == AIDefenseSite::Barracks || kind == AIDefenseSite::Factory || kind == AIDefenseSite::Headquarters;
        // A gap around it first, then side by side, then also on the band along a road (never on the road itself)
        for (std::size_t ri = 0; ri < rings.size() && !found; ++ri)
            for (int crowding = 0; crowding < 3 && !found; ++crowding)
            {
                found = FindCoreSite(
                    x, y, w, h, ox, oy, rear, rings[ri], map, entities, crowding, needsExit, centreX, centreY);
                if (!found && rear && kind != AIDefenseSite::PowerPlant)
                    found = FindCoreSite(
                        x,
                        y,
                        w,
                        h,
                        ox,
                        oy,
                        false,
                        rings[ri],
                        map,
                        entities,
                        crowding,
                        needsExit,
                        centreX,
                        centreY); // the back is full
            }
        std::string failure;
        // Exhaustive fallback: all finished HQ zones, no aesthetic exclusions.
        if (!found)
            for (const auto &e : entities.GetAllEntities())
            {
                const Building *base = e.AsBuilding();
                if (!base || !base->alive || base->state == ActionState::Die || base->owner != m_player
                    || base->type != UnitType::Headquarters || base->buildProgress < 1.0f)
                    continue;
                found = FindCoreSite(
                    x,
                    y,
                    w,
                    h,
                    ox,
                    oy,
                    false,
                    0,
                    map,
                    entities,
                    3,
                    needsExit,
                    base->tileX + base->buildWidth / 2,
                    base->tileY + base->buildHeight / 2,
                    &failure);
                if (found)
                    break;
            }

        if (!found)
        {
            ThinkOnce(
                "core" + std::to_string(kind),
                "BUILD",
                std::string(want.why) + " (placement rejected: " + failure + ")");
            continue;
        }
        AIDefenseSite site;
        site.kind = kind;
        site.tileX = x;
        site.tileY = y;
        site.exact = true;
        if (BuildDefenseSite(site, map, entities))
        {
            Think("BUILD", want.why);
            return true;
        }
    }
    return false;
}

void AIController::UpdateBaseBuilder(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
{
    if (!m_startingBaseRecorded)
        return;
    if (!entities.HasBuildingType(UnitType::Headquarters, m_player, true))
        return; // no finished HQ, no construction
    if (!m_defensePlanned)
        PlanBaseDefenses(map, entities, pathfinder);

    m_buildTimer += deltaTime;
    if (m_buildTimer < 3.0f)
        return;
    m_buildTimer = 0.0f;
    m_baseQuietSeconds += 3.0f;

    if (UpdateCoreBuildOrder(map, entities))
        return; // one structure per step

    // Troops come first: building only spends what is left above a reserve for the next wave
    int wallsThisTick = 0;
    for (auto &site : m_defensePlan)
    {
        if (site.done)
            continue;
        const int cost = DefenseCost(site.kind, entities, side());
        const int reserve = site.kind == AIDefenseSite::Barracks ? 0 : 300;
        if (m_credits - cost < reserve)
        {
            ThinkOnce(
                "build",
                "BUILD",
                "Fortification waits: troops come first (keeping $" + std::to_string(reserve) + " in reserve)");
            return;
        }
        const bool built = BuildDefenseSite(site, map, entities);
        if (site.kind == AIDefenseSite::Wall)
        {
            if (built && ++wallsThisTick >= 6)
            {
                Think("BUILD", "Raised 6 wall segments on the side facing the enemy");
                return; // a few wall segments per step
            }
            continue;
        }
        if (built)
            return; // one structure per step
    }

    // Plan finished: every 45 s a tower or bunker on the side the attacks keep coming from (the threat memory of
    // the 8 sectors around the base, a sector that has towers already counts less), at the edge of the buildings
    // on that side, by the road that leaves the base there
    m_expandTimer += 3.0f;
    constexpr int kMaxExpansions = 8;
    if (m_expandTimer < 45.0f || m_expansions >= kMaxExpansions || m_credits < 900)
        return;
    m_expandTimer = 0.0f;
    const int hx = m_homeX >= 0 ? m_homeX : m_enemyHqX, hy = m_homeY >= 0 ? m_homeY : m_enemyHqY;
    int defensesIn[8] = {}, bunkersIn[8] = {};
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->state != ActionState::Die && b->owner == m_player
            && (b->type == UnitType::Tower || b->type == UnitType::Bunker))
        {
            const int sct = SectorOf(b->tileX + b->buildWidth / 2, b->tileY + b->buildHeight / 2);
            ++defensesIn[sct];
            bunkersIn[sct] += b->type == UnitType::Bunker ? 1 : 0;
        }
    }
    int sector = -1;
    float bestWeight = 0.0f;
    for (int sct = 0; sct < 8; ++sct)
    {
        const float weight = m_sectorThreat[sct] / static_cast<float>(1 + defensesIn[sct]);
        if (weight > bestWeight)
        {
            bestWeight = weight;
            sector = sct;
        }
    }
    if (sector < 0)
        sector = SectorOf(m_playerHqX, m_playerHqY); // nothing came yet: the side of the objective
    const float angle = static_cast<float>(sector) * static_cast<float>(M_PI) / 4.0f;
    const float dx = DetMath::Cos(angle), dy = DetMath::Sin(angle);
    // The edge of the base on that side: the farthest building within the sector's cone, two tiles past it
    float edge = 4.0f;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->owner != m_player || b->type == UnitType::Tower
            || b->type == UnitType::Bunker)
            continue;
        const float bx = static_cast<float>(b->tileX) + b->buildWidth * 0.5f - static_cast<float>(hx);
        const float by = static_cast<float>(b->tileY) + b->buildHeight * 0.5f - static_cast<float>(hy);
        const float along = bx * dx + by * dy;
        const float across = std::abs(-bx * dy + by * dx);
        if (along > 0.0f && across <= along + 3.0f) // within about 45 degrees either side
            edge = std::max(edge, along + std::max(b->buildWidth, b->buildHeight) * 0.5f + 2.0f);
    }
    // Beside the road that leaves the base on that side (when there is one): the spot along the edge nearest to it
    float sideways = 0.0f;
    float bestRoad = 1e9f;
    for (int off = -6; off <= 6; ++off)
    {
        const int px = static_cast<int>(std::lround(hx + dx * edge - dy * off));
        const int py = static_cast<int>(std::lround(hy + dy * edge + dx * off));
        for (int ry = py - 3; ry <= py + 3; ++ry)
            for (int rx = px - 3; rx <= px + 3; ++rx)
                if (IsRoadCore(rx, ry))
                {
                    const float d = DetMath::Hypot(static_cast<float>(rx - px), static_cast<float>(ry - py))
                                    + 0.1f * std::abs(static_cast<float>(off));
                    if (d < bestRoad)
                    {
                        bestRoad = d;
                        sideways = static_cast<float>(off);
                    }
                }
    }
    if (bestRoad >= 1e9f) // no road there: spread the towers of one sector to both sides of its middle
        sideways = (defensesIn[sector] % 2 == 0 ? 1.0f : -1.0f) * 2.0f * static_cast<float>(defensesIn[sector] / 2);
    // A bunker at the edge first: it takes the fire. A tower only behind the bunkers of that side, never ahead of
    // them (a side has at least as many bunkers as towers)
    const int towersThere = defensesIn[sector] - bunkersIn[sector];
    AIDefenseSite::Kind kind = bunkersIn[sector] <= towersThere ? AIDefenseSite::Bunker : AIDefenseSite::Tower;
    // (the building limit of that kind is reached: the other kind, rather than nothing for ever)
    if (!BuildLimits::CanBuildMore(entities, map, m_player, FactionBuildingIndex(kind, side())))
        kind = kind == AIDefenseSite::Bunker ? AIDefenseSite::Tower : AIDefenseSite::Bunker;
    // As far out as building is allowed there: the zones of all headquarters decide, so a new headquarters moves
    // the line out; step by step nearer until a spot is found
    AIDefenseSite extra;
    bool built = false;
    for (float back = 0.0f; back <= 12.0f && !built; back += 2.0f)
    {
        const float out = edge - back - (kind == AIDefenseSite::Tower ? 3.0f : 0.0f);
        if (out < 4.0f)
            break;
        extra = AIDefenseSite {};
        extra.kind = kind;
        extra.tileX = static_cast<int>(std::lround(hx + dx * out - dy * sideways)) - 1;
        extra.tileY = static_cast<int>(std::lround(hy + dy * out + dx * sideways)) - 1;
        built = BuildDefenseSite(extra, map, entities);
    }
    if (built)
    {
        ++m_expansions;
        m_defensePlan.push_back(extra);
        static const char *const kSectorNames[8]
            = {"east", "south-east", "south", "south-west", "west", "north-west", "north", "north-east"};
        Think(
            "BUILD",
            std::string("Attacks keep coming from the ") + kSectorNames[sector] + ": extra "
                + (extra.kind == AIDefenseSite::Bunker ? "bunker" : "tower") + " at the edge of the base there");
    }
}

} // namespace OpenArtOfWar
