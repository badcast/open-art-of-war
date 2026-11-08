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

void AIController::UpdateEconomyAndProduction(float /*deltaTime*/, TileMap &map, EntityManager &entities)
{
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

    int enemyUnitCount = 0;
    int reserveCount = 0;
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
                if (b->buildProgress >= 1.0f && b->isPowered && !b->isProducing && b->productionTimer <= 0.0f)
                    availableBarracks.push_back(b);
            }
            else if (b->type == UnitType::Factory)
            {
                hasFactory = true;
                if (b->buildProgress >= 1.0f && b->isPowered && !b->isProducing && b->productionTimer <= 0.0f)
                    availableFactories.push_back(b);
            }
        }
        else
        {
            enemyUnitCount++;
            if (const Unit *u = e.AsUnit(); u && IsReserveUnit(*u))
                reserveCount++;
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

    // Units lost while assembling are replaced so the wave can still reach 100% strength
    int planned = static_cast<int>(m_currentPlan.archetypes.size());
    int queued = static_cast<int>(m_infantryQueue.size() + m_vehicleQueue.size());
    // ... and so is the home guard that stays when the wave leaves
    const int withGuard = planned + HomeGuardCount();
    if (queued == 0 && reserveCount < withGuard)
    {
        for (int i = reserveCount; i < withGuard; ++i)
            m_infantryQueue.push_back(kRifleman);
    }

    // Unit cap per match for performance & balance
    // (a crowd of infantry must not keep the vehicles from the factory: they have a little more room)
    const bool infantryAllowed = enemyUnitCount < 40;
    if (enemyUnitCount >= 52)
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

    // While the lab waits for its next technology, the army only spends what exceeds that budget,
    // so research keeps advancing instead of being starved by production
    int researchReserve = (m_hasLab && !m_researchActive && m_researchIndex < kResearchCount)
                              ? researchDef(side(), m_researchIndex).cost
                              : 0;

    auto produce = [&](Building *building, std::vector<int> &queue, float buildTime, int savingFor = 0) {
        if (!building || queue.empty())
            return;
        int arch = queue.front();
        // A unit its technology has not opened yet (a sniper, a heavy tank) cannot be trained, by the computer no
        // more than by a person: the slot takes the simplest unit of the same kind that is open
        if (!IsUnitUnlocked(m_player, SideArchetype(arch, side())))
        {
            const int infantry[] = {kRifleman, kHeavyAssault, kFlamethrower};
            const int vehicles[] = {kFortress, kHammer, kGrad, kZeus};
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
        if (m_credits - cost < researchReserve + savingFor)
        {
            ThinkOnce(
                "economy",
                "ECONOMY",
                researchReserve > 0 ? "Holding back unit production to afford the next technology"
                                    : "Not enough credits to train the next unit");
            return;
        }
        ThinkOnce("economy", "ECONOMY", "Training troops for the next wave");
        int sx = building->tileX;
        int sy = building->tileY;
        if (!entities.GetBuildingSpawnPoint(building->id, map, sx, sy))
            return;
        m_credits -= cost;
        queue.erase(queue.begin());
        int id = entities.SpawnUnitByArchetype(
            ProductionForm(m_player, SideArchetype(arch, side())), sx, sy, side(), m_player);
        // New troops get every technology the AI has already researched, and join the list of the next wave
        if (Unit *u = entities.GetUnit(id))
        {
            ApplyCompletedResearch(*u);
            m_waveRoster.insert(id);
            ++m_trainedUnits;
        }
        building->isProducing = true;
        building->productionTimer = buildTime;
    };

    // A free factory with a vehicle waiting comes first: the barracks leave enough credits for it, or cheap infantry
    // would eat every credit before a tank (220-420) could ever be paid. Not for ever: if the vehicle cannot be
    // trained (its exit is blocked), the barracks go on after a while.
    constexpr float kVehicleHoldMax = 25.0f;
    const bool vehicleWaits = !availableFactories.empty() && !m_vehicleQueue.empty();
    int savingFor = 0;
    if (vehicleWaits && m_vehicleHoldSeconds < kVehicleHoldMax)
        savingFor = costOf(m_vehicleQueue.front());
    if (vehicleWaits)
        m_vehicleHoldSeconds += 1.0f;
    else
        m_vehicleHoldSeconds = 0.0f;
    for (Building *barracks : availableBarracks)
        if (infantryAllowed)
            produce(barracks, m_infantryQueue, barracksBuildTime, savingFor);
    for (Building *factory : availableFactories)
    {
        const std::size_t before = m_vehicleQueue.size();
        produce(factory, m_vehicleQueue, factoryBuildTime);
        if (m_vehicleQueue.size() != before)
            m_vehicleHoldSeconds = 0.0f; // a vehicle went out
    }
}

void AIController::UpdateResearch(float deltaTime, EntityManager &entities)
{
    const Faction faction = side();
    // Skip technologies that only make sense for the player
    while (m_researchIndex < kResearchCount && researchDef(faction, m_researchIndex).playerOnly)
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
    const int wave = static_cast<int>(m_currentPlan.archetypes.size());
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
    const bool needLab = labCount == 0 && !rebuildPending(UnitType::ResearchLab);

    // The base is ready for another headquarters (more income): everything works, the defense stands, there is
    // money to spare and the base has had peace for a minute
    auto baseReady = [&]() {
        if (!full || hqCount >= Economy::kMaxIncomeHeadquarters || barracksDone == 0 || factoryDone == 0
            || labDone == 0)
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
        else if (kind == AIDefenseSite::Headquarters)
        {
            minRing += 5; // a new HQ widens the building zone: not right beside the old one
        }

        int x = 0, y = 0;
        bool found = false;
        const bool needsExit
            = kind == AIDefenseSite::Barracks || kind == AIDefenseSite::Factory || kind == AIDefenseSite::Headquarters;
        // A gap around it first, then side by side, then also on the band along a road (never on the road itself)
        for (int crowding = 0; crowding < 3 && !found; ++crowding)
        {
            found = FindCoreSite(x, y, w, h, ox, oy, rear, minRing, map, entities, crowding, needsExit);
            if (!found && rear && kind != AIDefenseSite::PowerPlant)
                found = FindCoreSite(
                    x, y, w, h, ox, oy, false, minRing, map, entities, crowding, needsExit); // the back is full
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

    // Plan finished: every 45 s add a tower on the side the player keeps attacking from
    m_expandTimer += 3.0f;
    if (m_expandTimer < 45.0f || m_expansions >= 6 || m_credits < 900)
        return;
    m_expandTimer = 0.0f;
    int ax = m_lastThreatX >= 0 ? m_lastThreatX : m_playerHqX;
    int ay = m_lastThreatY >= 0 ? m_lastThreatY : m_playerHqY;
    const float dx = static_cast<float>(ax - m_enemyHqX);
    const float dy = static_cast<float>(ay - m_enemyHqY);
    const float len = std::max(1.0f, DetMath::Hypot(dx, dy));
    const float radius = std::min(len - 2.0f, 9.0f + 2.0f * static_cast<float>(m_expansions % 3));
    const float sideways = (m_expansions % 2 == 0 ? 1.0f : -1.0f) * 2.0f * static_cast<float>(m_expansions / 2);
    AIDefenseSite extra;
    extra.kind = (m_expansions % 3 == 2) ? AIDefenseSite::Bunker : AIDefenseSite::Tower;
    extra.tileX = static_cast<int>(std::lround(m_enemyHqX + dx / len * radius - dy / len * sideways)) - 1;
    extra.tileY = static_cast<int>(std::lround(m_enemyHqY + dy / len * radius + dx / len * sideways)) - 1;
    if (BuildDefenseSite(extra, map, entities))
    {
        ++m_expansions;
        m_defensePlan.push_back(extra);
        Think(
            "BUILD",
            "Attacks keep coming from (" + std::to_string(ax) + ", " + std::to_string(ay) + "): extra "
                + (extra.kind == AIDefenseSite::Bunker ? "bunker" : "tower") + " on that side");
    }
}

} // namespace OpenArtOfWar
