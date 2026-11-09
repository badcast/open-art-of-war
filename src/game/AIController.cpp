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
#include <iterator>

namespace OpenArtOfWar
{

std::string DifficultyToString(AIDifficulty diff)
{
    switch (diff)
    {
        case AIDifficulty::Easy:
            return "easy";
        case AIDifficulty::Medium:
            return "medium";
        case AIDifficulty::Hard:
            return "hard";
        case AIDifficulty::Hardcore:
            return "hardcore";
    }
    return "medium";
}

AIDifficulty StringToDifficulty(const std::string &str)
{
    if (str == "easy" || str == "Easy")
        return AIDifficulty::Easy;
    if (str == "hard" || str == "Hard")
        return AIDifficulty::Hard;
    if (str == "hardcore" || str == "Hardcore")
        return AIDifficulty::Hardcore;
    return AIDifficulty::Medium;
}

void AIController::InitDifficultyDefaults()
{
    m_difficultyConfigs.clear();

    // Easy: small waves with long pauses, loose formation, no waiting at chokepoints
    AIDifficultyConfig easy;
    easy.id = "easy";
    easy.name = "Easy";
    easy.description = "AI resource handicap of 50% (50% starting credits and income)";
    easy.startingCredits = 1000;
    easy.creditsPerSec = 7;
    easy.resourceMultiplier = 0.50f;
    easy.productionSpeedMultiplier = 1.00f;
    easy.tactics.leashTiles = 6.0f;
    easy.tactics.waitAtChokepoints = false;
    easy.tactics.preChokeHoldSeconds = 0.0f;
    easy.tactics.meleeRangedPairs = false;
    easy.tactics.waveBaseSize = 10;
    easy.tactics.waveGrowth = 1;
    easy.tactics.waveMaxSize = 14;
    easy.tactics.waveIntervalSeconds = 75.0f;
    easy.tactics.maxWaveTier = 4; // the siege fist comes late (wave 8) but does come
    easy.tactics.researchSeconds = 100.0f;
    easy.tactics.vanguardWave = 3;
    easy.tactics.assaultWave = 5;
    easy.tactics.siegeWave = 8;
    easy.tactics.vehicleWaitSeconds = 90.0f;
    easy.tactics.stragglerTimeoutSeconds = 4.0f;
    easy.tactics.marchStrideTiles = 7;
    easy.tactics.maxAttackGroups = 2;
    easy.tactics.creditsPerExtraGroup = 1200;
    easy.tactics.buildDefenses = false;
    m_difficultyConfigs[AIDifficulty::Easy] = easy;

    // Medium (Normal): strict 3-tile leash, melee + ranged pairs, gathers before chokepoints
    AIDifficultyConfig med;
    med.id = "medium";
    med.name = "Medium";
    med.description = "AI resource handicap of 20% (80% starting credits and income)";
    med.startingCredits = 1600;
    med.creditsPerSec = 12;
    med.resourceMultiplier = 0.80f;
    med.productionSpeedMultiplier = 1.00f;
    med.tactics.waveIntervalSeconds = 45.0f;
    med.tactics.researchSeconds = 85.0f;
    med.tactics.vanguardWave = 2;
    med.tactics.assaultWave = 3;
    med.tactics.siegeWave = 5;
    med.tactics.vehicleWaitSeconds = 60.0f;
    med.tactics.maxAttackGroups = 3;
    m_difficultyConfigs[AIDifficulty::Medium] = med;

    // Hard: + pace of the slowest unit, armor in front, retreat when a wave dies for nothing
    AIDifficultyConfig hard;
    hard.id = "hard";
    hard.name = "Hard";
    hard.description = "Standard fair AI with no handicap (100% base resources and speed)";
    hard.startingCredits = 2000;
    hard.creditsPerSec = 15;
    hard.resourceMultiplier = 1.00f;
    hard.productionSpeedMultiplier = 1.00f;
    hard.tactics.syncToSlowest = true;
    hard.tactics.armorFront = true;
    hard.tactics.retreatLossRatio = 0.6f;
    hard.tactics.waveBaseSize = 14;
    hard.tactics.waveMaxSize = 22;
    hard.tactics.waveIntervalSeconds = 30.0f;
    hard.tactics.researchSeconds = 70.0f;
    hard.tactics.vanguardWave = 2;
    hard.tactics.assaultWave = 3;
    hard.tactics.siegeWave = 5;
    hard.tactics.vehicleWaitSeconds = 45.0f;
    hard.tactics.stragglerTimeoutSeconds = 8.0f;
    hard.tactics.marchStrideTiles = 4;
    hard.tactics.maxAttackGroups = 4;
    hard.tactics.creditsPerExtraGroup = 600;
    m_difficultyConfigs[AIDifficulty::Hard] = hard;

    AIDifficultyConfig hardcore = hard;
    hardcore.id = "hardcore";
    hardcore.name = "Hardcore";
    hardcore.description = "Hardcore AI: +30% bonus resources and +10% faster unit production";
    hardcore.startingCredits = 2600;
    hardcore.creditsPerSec = 20;
    hardcore.resourceMultiplier = 1.30f;
    hardcore.productionSpeedMultiplier = 1.10f;
    hardcore.tactics.waveBaseSize = 16;
    hardcore.tactics.waveGrowth = 3;
    hardcore.tactics.waveMaxSize = 26;
    hardcore.tactics.waveIntervalSeconds = 25.0f;
    hardcore.tactics.researchSeconds = 65.0f;
    hardcore.tactics.vanguardWave = 2;
    hardcore.tactics.assaultWave = 3;
    hardcore.tactics.siegeWave = 5;
    hardcore.tactics.vehicleWaitSeconds = 40.0f;
    hardcore.tactics.maxAttackGroups = 5;
    hardcore.tactics.creditsPerExtraGroup = 500;
    m_difficultyConfigs[AIDifficulty::Hardcore] = hardcore;

    m_difficulty = AIDifficulty::Medium;
    RefreshDifficulty();
    m_credits = m_currentDifficultyConfig.startingCredits;
}

bool AIController::LoadDifficultyConfig(const std::string &path)
{
    try
    {
        std::string text;
        if (!Assets::ReadText(path, text))
            return false;
        YAML::Node root = YAML::Load(text);
        if (!root.IsDefined() || !root.IsMap())
            return false;

        auto parseMode = [&](const std::string &key, AIDifficulty diff) {
            if (!root[key] || !root[key].IsMap())
                return;
            YAML::Node node = root[key];
            AIDifficultyConfig &cfg = m_difficultyConfigs[diff];
            if (node["id"])
                cfg.id = node["id"].as<std::string>();
            if (node["name"])
                cfg.name = node["name"].as<std::string>();
            if (node["description"])
                cfg.description = node["description"].as<std::string>();
            if (node["starting_credits"])
                cfg.startingCredits = node["starting_credits"].as<int>();
            if (node["credits_per_second"])
                cfg.creditsPerSec = node["credits_per_second"].as<int>();
            if (node["resource_multiplier"])
                cfg.resourceMultiplier = node["resource_multiplier"].as<float>();
            if (node["production_speed_multiplier"])
                cfg.productionSpeedMultiplier = node["production_speed_multiplier"].as<float>();
            ParseTactics(node["tactics"], cfg.tactics);
        };

        parseMode("easy", AIDifficulty::Easy);
        parseMode("medium", AIDifficulty::Medium);
        parseMode("hard", AIDifficulty::Hard);
        parseMode("hardcore", AIDifficulty::Hardcore);

        if (root["default_difficulty"])
        {
            std::string defDiff = root["default_difficulty"].as<std::string>();
            SetDifficulty(StringToDifficulty(defDiff));
        }
        else
        {
            SetDifficulty(m_difficulty);
        }
        std::cout << "[AIController] Loaded difficulty configuration from " << path << std::endl;
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[AIController] Warning: Could not load " << path << " (" << e.what() << "), using defaults."
                  << std::endl;
        return false;
    }
}

AIDifficulty AIController::GetEffectiveDifficulty() const
{
    if (m_debugConfig.forceDebugOverride)
    {
        return static_cast<AIDifficulty>(std::clamp(m_debugConfig.debugDifficultyLevel, 0, 3));
    }
    return m_difficulty;
}

void AIController::RefreshDifficulty()
{
    auto it = m_difficultyConfigs.find(GetEffectiveDifficulty());
    if (it != m_difficultyConfigs.end())
    {
        m_currentDifficultyConfig = it->second;
    }
    m_creditsPerSec = m_currentDifficultyConfig.creditsPerSec;
}

void AIController::SetDifficulty(AIDifficulty difficulty)
{
    m_difficulty = difficulty;
    RefreshDifficulty();
}

void AIController::SetDebugConfig(const AIDebugConfig &config)
{
    bool difficultyChanged = config.forceDebugOverride != m_debugConfig.forceDebugOverride
                             || config.debugDifficultyLevel != m_debugConfig.debugDifficultyLevel;
    bool sizeChanged = config.waveSpawnMultiplier != m_debugConfig.waveSpawnMultiplier;
    m_debugConfig = config;
    m_debugConfig.waveSpawnMultiplier = std::clamp(m_debugConfig.waveSpawnMultiplier, 0.25f, 4.0f);
    if (difficultyChanged)
    {
        RefreshDifficulty();
        std::cout << "[AIController] Debug difficulty -> " << DifficultyToString(GetEffectiveDifficulty()) << std::endl;
    }
    if (difficultyChanged || sizeChanged)
    {
        // Re-plan the wave that is being assembled with the new parameters
        m_infantryQueue.clear();
        m_vehicleQueue.clear();
        m_currentPlan.archetypes.clear();
    }
}

void AIController::Think(const std::string &category, const std::string &text)
{
    m_thoughts.push_back({m_gameTime, category, text});
    ++m_thoughtSerial;
    while (m_thoughts.size() > 300)
        m_thoughts.pop_front();
    std::cout << "[AIController p" << int(m_player) << "] " << text << std::endl;
}

void AIController::ThinkOnce(const std::string &key, const std::string &category, const std::string &text)
{
    auto it = m_lastThoughtByKey.find(key);
    if (it != m_lastThoughtByKey.end() && it->second == text)
        return;
    m_lastThoughtByKey[key] = text;
    Think(category, text);
}

void AIController::SetEnemyCredits(int credits)
{
    const int before = m_credits;
    m_credits = std::max(0, credits);
    Think("ECONOMY", "Treasury set by debug: " + std::to_string(before) + " -> " + std::to_string(m_credits));
}

std::vector<std::string> AIController::CurrentIntentions() const
{
    std::vector<std::string> out;
    const int required = static_cast<int>(m_currentPlan.archetypes.size());
    if (required > 0)
    {
        std::ostringstream w;
        w << "Assembling wave " << (m_waveCount + 1) << " (" << TierName(m_currentPlan.tier) << "): " << m_readyUnits
          << "/" << required << " ready";
        if (m_waveCooldown > 0.0f)
            w << ", may launch in " << static_cast<int>(m_waveCooldown) << " s";
        if (!m_hasProduction)
            w << " - no barracks / factory left";
        out.push_back(w.str());
        std::ostringstream q;
        q << "Training queue: " << m_infantryQueue.size() << " infantry, " << m_vehicleQueue.size() << " vehicles";
        out.push_back(q.str());
    }
    if (!m_squadController.GetSquads().empty())
    {
        std::ostringstream g;
        g << m_squadController.GetSquads().size() << " group(s) on the march";
        out.push_back(g.str());
    }
    if (m_researchIndex < kResearchCount)
    {
        const ResearchDef &r = researchDef(side(), m_researchIndex);
        std::ostringstream t;
        if (!m_hasLab)
            t << "Research paused: no Tech Center / Laboratory (next: " << r.name << ")";
        else if (m_researchActive)
            t << "Researching " << r.name << " " << static_cast<int>(GetResearchProgress() * 100.0f) << "%";
        else
            t << "Saving " << r.cost << " credits for " << r.name;
        out.push_back(t.str());
    }
    if (m_skirmish && m_defensePlanned)
    {
        int left = 0;
        for (const auto &d : m_defensePlan)
            left += d.done ? 0 : 1;
        out.push_back(
            left > 0 ? "Fortifying the base: " + std::to_string(left) + " items left"
                     : "Base fortified, " + std::to_string(m_expansions) + " extra defenses");
    }
    int destroyed = 0;
    for (const auto &p : m_startingBuildings)
        destroyed += p.destroyed ? 1 : 0;
    if (destroyed > 0)
        out.push_back("Rebuilding " + std::to_string(destroyed) + " destroyed structure(s)");
    if (m_lastThreatId != 0)
    {
        out.push_back(
            "Watching the intruder near (" + std::to_string(m_lastThreatX) + ", " + std::to_string(m_lastThreatY)
            + ")");
    }
    return out;
}

AIController::AIController()
{
    InitDifficultyDefaults();
    LoadDifficultyConfig(LoaderResources::aiDifficulty());
}

bool AIController::IsFoe(const Entity &e) const
{
    return e.owner < kMaxPlayers && Players::Hostile(e.owner, m_player) && !((m_eliminatedFoes >> e.owner) & 1u);
}

void AIController::RefreshFoeAnchor(EntityManager &entities)
{
    m_foeAnchorTimer -= SIMULATION_TIMESTEP;
    // A player who is out: never a target again, and the squads marching (perhaps at him) come back
    bool newlyOut = false;
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        if (p == m_player || !Players::Hostile(p, m_player) || ((m_eliminatedFoes >> p) & 1u)
            || !entities.IsEliminated(p))
            continue;
        m_eliminatedFoes |= 1u << p;
        newlyOut = true;
    }
    if (!newlyOut && m_foeAnchorTimer > 0.0f)
        return;
    m_foeAnchorTimer = 2.0f;
    m_anchorHoldSeconds = std::max(0.0f, m_anchorHoldSeconds - 2.0f);
    // Every living foe gets a score: near (by its nearest headquarters, else any building), the one that keeps
    // attacking this base (m_foeHostility) and the weak one (few structures known) are preferred. The objective is
    // kept at least kAnchorHoldSeconds unless its building is gone. Tuning:
    constexpr float kNearWeight = 1.0f, kHostileWeight = 1.0f, kWeakWeight = 0.5f;
    constexpr float kAnchorHoldSeconds = 120.0f;
    constexpr float kSwitchMargin = 0.15f; // a new foe must score this much more than the current one
    struct FoeCandidate
    {
        const Building *building {nullptr};
        bool hq {false};
        float dist {1e9f};
        int known {0};
    };
    FoeCandidate foes[kMaxPlayers];
    bool anyFoe = false, anchorHolds = false;
    int anchorOwner = -1;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || !IsFoe(e) || b->owner >= kMaxPlayers)
            continue;
        anyFoe = true;
        if (m_playerHqX >= b->tileX && m_playerHqX < b->tileX + b->buildWidth && m_playerHqY >= b->tileY
            && m_playerHqY < b->tileY + b->buildHeight)
        {
            anchorHolds = true;
            anchorOwner = b->owner;
        }
        FoeCandidate &f = foes[b->owner];
        const bool hq = b->type == UnitType::Headquarters;
        const float d = DetMath::Hypot(
            static_cast<float>(b->tileX + b->buildWidth / 2 - m_enemyHqX),
            static_cast<float>(b->tileY + b->buildHeight / 2 - m_enemyHqY));
        if ((hq && !f.hq) || (hq == f.hq && d < f.dist))
        {
            f.building = b;
            f.hq = hq;
            f.dist = d;
        }
    }
    for (const auto &[id, tile] : m_knownBuildings)
    {
        (void) tile;
        const Entity *e = entities.GetEntity(id);
        if (e && e->alive && e->state != ActionState::Die && e->owner < kMaxPlayers)
            ++foes[e->owner].known;
    }
    float maxDist = 1.0f, maxHostility = 0.0f;
    int maxKnown = 0;
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        if (!foes[p].building)
            continue;
        maxDist = std::max(maxDist, foes[p].dist);
        maxHostility = std::max(maxHostility, m_foeHostility[p]);
        maxKnown = std::max(maxKnown, foes[p].known);
    }
    auto scoreOf = [&](int p) {
        const FoeCandidate &f = foes[p];
        float score = kNearWeight * (1.0f - f.dist / maxDist);
        if (maxHostility > 0.0f)
            score += kHostileWeight * m_foeHostility[p] / maxHostility;
        if (maxKnown > 0)
            score += kWeakWeight * (1.0f - static_cast<float>(f.known) / static_cast<float>(maxKnown));
        return score;
    };
    int bestFoe = -1;
    float bestScore = -1e9f;
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        if (!foes[p].building)
            continue;
        const float sc = scoreOf(p);
        if (sc > bestScore)
        {
            bestScore = sc;
            bestFoe = p;
        }
    }
    const Entity *best = nullptr;
    if (bestFoe >= 0)
    {
        const bool switchFoe
            = !anchorHolds
              || (bestFoe != anchorOwner && m_anchorHoldSeconds <= 0.0f
                  && (!foes[anchorOwner].building || bestScore > scoreOf(anchorOwner) + kSwitchMargin));
        if (switchFoe)
        {
            best = foes[bestFoe].building;
            anchorHolds = false;
            m_anchorHoldSeconds = kAnchorHoldSeconds;
        }
    }
    if (!anyFoe)
    {
        // Units of a living foe may still be about (no building left, not out yet): they stay targets
        for (const auto &e : entities.GetAllEntities())
            if (e.alive && e.state != ActionState::Die && !e.isBuilding && IsFoe(e))
            {
                anyFoe = true;
                break;
            }
    }
    if (m_noFoesLeft != !anyFoe)
        Think("ATTACK", anyFoe ? "A foe is in sight again" : "No foe is left: no more waves");
    m_noFoesLeft = !anyFoe;
    if (best && !anchorHolds)
    {
        const Building *b = best->AsBuilding();
        m_playerHqX = b->tileX + b->buildWidth / 2;
        m_playerHqY = b->tileY + b->buildHeight / 2;
        std::ostringstream msg;
        msg << "New objective: the base at (" << m_playerHqX << ", " << m_playerHqY << ")";
        Think("ATTACK", msg.str());
        std::ostringstream why;
        why << "Objective chosen by score " << bestScore << " (near, attacks on this base, weakness)";
        Think("ATTACK", why.str());
    }
    if (newlyOut)
    {
        std::vector<int> marching;
        for (const AISquad &sq : m_squadController.GetSquads())
            marching.insert(marching.end(), sq.unitIds.begin(), sq.unitIds.end());
        if (!marching.empty())
        {
            m_squadController.ReleaseUnits(marching, entities);
            Think("ATTACK", "A foe is out of the game: the squads come back for a new objective");
        }
    }
}

void AIController::SetPlayer(PlayerId player)
{
    m_player = player;
    m_playerFixed = true;
}

void AIController::Init(int enemyHqX, int enemyHqY, int playerHqX, int playerHqY)
{
    // Single player: the computer is the slot of the other faction (a match sets its slot with setPlayer)
    if (!m_playerFixed)
        m_player = Players::OwnerOf(AiFaction());
    m_enemyHqX = enemyHqX;
    m_enemyHqY = enemyHqY;
    m_playerHqX = playerHqX;
    m_playerHqY = playerHqY;
    m_initialized = true;
    Reset();
}

void AIController::Reset()
{
    m_credits = m_currentDifficultyConfig.startingCredits;
    m_creditsPerSec = m_currentDifficultyConfig.creditsPerSec;
    m_gameTime = 0.0f;
    m_economyTimer = 0.0f;
    m_defenseCheckTimer = 0.0f;
    m_waveCount = 0;
    m_startingBuildings.clear();
    m_startingWalls.clear();
    m_startingBaseRecorded = false;

    m_currentPlan = AIWavePlan {};
    m_infantryQueue.clear();
    m_vehicleQueue.clear();
    m_waveCooldown = m_currentDifficultyConfig.tactics.waveIntervalSeconds * 0.5f;
    m_waveAssemblyTimer = 0.0f;
    m_factoryReady = false;
    m_vehicleHoldSeconds = 0.0f;
    m_readyUnits = 0;
    m_hasProduction = true;

    m_researchIndex = 0;
    m_researchActive = false;
    m_researchTimer = 0.0f;
    ResetResearch(m_player);

    m_squadController.Clear();
    m_chokeRebuildTimer = 1e6f; // rebuild on the first update
    m_garrison.clear();
    m_garrisonRecorded = false;
    m_recalled.clear();
    m_waveRoster.clear();
    m_rosterSeeded = false;
    m_hasLab = false;

    m_defensePlanned = false;
    m_defensePlan.clear();
    m_territoryReady = false;
    m_road.clear();
    m_ownGround.clear();
    m_enemyRoads.clear();
    m_homeX = m_homeY = -1;
    m_baseQuietSeconds = 0.0f;
    m_buildTimer = 0.0f;
    m_expandTimer = 0.0f;
    m_expansions = 0;
    m_gateX[0] = m_gateX[1] = m_gateY[0] = m_gateY[1] = -1;
    m_stagingTimer = 0.0f;
    m_lastThreatX = m_lastThreatY = -1;
    m_lastThreatId = 0;
    m_knownBuildings.clear();
    std::fill(std::begin(m_sectorThreat), std::end(m_sectorThreat), 0.0f);
    std::fill(std::begin(m_foeHostility), std::end(m_foeHostility), 0.0f);
    m_eliminatedFoes = 0;
    m_noFoesLeft = false;
    m_foeAnchorTimer = 0.0f;
    m_anchorHoldSeconds = 0.0f;
    m_seenEnemyUnits.clear();
    m_intelTimer = 0.0f;
    m_scoutId = 0;
    m_scoutX = m_scoutY = -1;
    m_scoutTimer = 0.0f;
    m_scoutIdleTimer = 0.0f;
    m_scoutTrips = 0;
    m_repairing.clear();
    m_retreatTimer = 0.0f;
    m_firstRaidDone = false;
    m_failedWaves = 0;
    m_trackedWave = 0;
    m_trackedBuildingsKilled = 0;
    m_trainedUnits = 0;
    ClearThoughts();
}

// ---------------------------------------------------------------------------
// Director: wave planning
// ---------------------------------------------------------------------------

void AIController::PreparePlan(int waveNumber, bool hasFactory, bool hasFactoryPlan)
{
    m_currentPlan = PlanWave(waveNumber, hasFactory, hasFactoryPlan);
    m_infantryQueue.clear();
    m_vehicleQueue.clear();
    for (int arch : m_currentPlan.archetypes)
    {
        (IsVehicleArchetype(arch) ? m_vehicleQueue : m_infantryQueue).push_back(arch);
    }
    std::ostringstream msg;
    msg << "Planning wave " << m_currentPlan.waveNumber << " (" << TierName(m_currentPlan.tier)
        << "): " << m_infantryQueue.size() << " infantry + " << m_vehicleQueue.size() << " vehicles";
    if (!hasFactory && !hasFactoryPlan)
        msg << " (no factory: heavy slots become infantry)";
    Think("WAVE", msg.str());
}

bool AIController::IsTechnologyResearched(int index) const
{
    return IsResearched(m_player, index);
}

float AIController::GetResearchProgress() const
{
    if (!m_researchActive)
        return 0.0f;
    float duration = std::max(1.0f, m_currentDifficultyConfig.tactics.researchSeconds);
    return std::clamp(m_researchTimer / duration, 0.0f, 1.0f);
}

bool AIController::AiSees(const Entity &e, const EntityManager &entities) const
{
    return m_debugConfig.disableFogOfWarForAi || entities.IsEntityVisibleToAi(e, m_player);
}

void AIController::Update(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
{
    m_gameTime += deltaTime;
    // The computer's own research drives the animation of its Tech Center / Laboratory
    if (m_researchActive)
        SetResearchAnimation(m_player, true, entities);
    m_waveAssemblyTimer += deltaTime;
    UpdateUnitStances(entities);
    m_squadController.SetSeesAll(GetEffectiveDifficulty() == AIDifficulty::Hardcore);
    UpdateRepairs(deltaTime, entities);
    RefreshFoeAnchor(entities);

    // Auto-synchronize Enemy & Player base coordinates from living entities if uninitialized or destroyed
    if (m_enemyHqX <= 0 || m_enemyHqY <= 0 || m_playerHqX <= 0 || m_playerHqY <= 0)
    {
        bool foundEnemyHq = false;
        bool foundPlayerHq = false;
        for (const auto &e : entities.GetAllEntities())
        {
            if (!e.alive || e.state == ActionState::Die)
                continue;
            if (const Building *b = e.AsBuilding())
            {
                int cx = b->tileX + b->buildWidth / 2;
                int cy = b->tileY + b->buildHeight / 2;
                if (b->owner == m_player)
                {
                    if (m_enemyHqX <= 0 || m_enemyHqY <= 0 || (b->type == UnitType::Headquarters && !foundEnemyHq))
                    {
                        m_enemyHqX = cx;
                        m_enemyHqY = cy;
                        if (b->type == UnitType::Headquarters)
                            foundEnemyHq = true;
                    }
                }
                else if (IsFoe(*b))
                {
                    if (m_playerHqX <= 0 || m_playerHqY <= 0 || (b->type == UnitType::Headquarters && !foundPlayerHq))
                    {
                        m_playerHqX = cx;
                        m_playerHqY = cy;
                        if (b->type == UnitType::Headquarters)
                            foundPlayerHq = true;
                    }
                }
            }
        }
        if (m_playerHqX <= 0 || m_playerHqY <= 0)
        {
            for (const auto &e : entities.GetAllEntities())
            {
                if (e.alive && e.state != ActionState::Die && IsFoe(e))
                {
                    m_playerHqX = e.tileX;
                    m_playerHqY = e.tileY;
                    break;
                }
            }
        }
        if (m_enemyHqX <= 0 || m_enemyHqY <= 0)
        {
            for (const auto &e : entities.GetAllEntities())
            {
                if (e.alive && e.state != ActionState::Die && e.owner == m_player)
                {
                    m_enemyHqX = e.tileX;
                    m_enemyHqY = e.tileY;
                    break;
                }
            }
        }
    }

    if (!m_garrisonRecorded)
    {
        m_garrisonRecorded = true;
        for (const auto &e : entities.GetAllEntities())
        {
            if (e.alive && !e.isBuilding && e.owner == m_player)
                m_garrison.insert(e.id);
        }
    }

    // Chokepoints change when structures and walls appear or disappear
    m_chokeRebuildTimer += deltaTime;
    if (m_chokeRebuildTimer >= 5.0f)
    {
        m_chokeRebuildTimer = 0.0f;
        m_chokes.Rebuild(map);
    }

    UpdateStagingPoint(map, pathfinder, deltaTime);
    m_squadController.SetStagingPoint(m_stagingX, m_stagingY);

    // Rebuild starting base structures & walls if destroyed
    UpdateRebuilding(deltaTime, map, entities);
    std::vector<int> headquarters;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->state != ActionState::Die && b->type == UnitType::Headquarters
            && b->buildProgress >= 1.0f)
            headquarters.push_back(b->id);
    }
    if (m_skirmish && (!m_territoryReady || headquarters != m_territoryHeadquarters))
    {
        PlanTerritory(map, entities, pathfinder);
        m_territoryHeadquarters = std::move(headquarters);
        m_defensePlanned = false;
    }
    // The builder also runs while the base lacks a barracks or a factory: without a factory there are no vehicles
    if (m_skirmish
        && (m_currentDifficultyConfig.tactics.buildDefenses
            || !entities.HasBuildingType(UnitType::Barracks, m_player, false)
            || !entities.HasBuildingType(UnitType::Factory, m_player, false)))
    {
        UpdateBaseBuilder(deltaTime, map, entities, pathfinder);
    }
    else if (m_skirmish && m_startingBaseRecorded)
    {
        // A base without the defense building still builds its core: power, the barracks, the factory and the
        // laboratory (no extra production, no second HQ)
        m_buildTimer += deltaTime;
        if (m_buildTimer >= 3.0f)
        {
            m_buildTimer = 0.0f;
            if (!m_territoryReady)
                PlanTerritory(map, entities, pathfinder);
            UpdateCoreBuildOrder(map, entities, false);
        }
    }

    m_economyTimer += deltaTime;
    while (m_economyTimer >= 1.0f)
    {
        m_economyTimer -= 1.0f;
        UpdateEconomyAndProduction(1.0f, map, entities);
        UpdateResearch(1.0f, entities);
        UpdateWaveOutcome(entities);
    }

    m_defenseCheckTimer += deltaTime;
    if (m_defenseCheckTimer >= 2.0f)
    {
        m_defenseCheckTimer = 0.0f;
        UpdateBaseDefense(entities, pathfinder);
    }

    UpdateIntel(entities);
    UpdateScouting(deltaTime, map, entities, pathfinder);
    m_retreatTimer += deltaTime;
    if (m_retreatTimer >= 1.0f)
    {
        m_retreatTimer = 0.0f;
        UpdateValuableRetreat(entities, pathfinder);
    }
    UpdateStaging(map, entities, pathfinder);
    UpdateWaveLaunch(deltaTime, map, entities, pathfinder);
    UpdateWaveRoster(entities);
    m_squadController.Update(deltaTime, map, entities, pathfinder, m_chokes, m_currentDifficultyConfig.tactics);
    RecallReleasedUnits(entities); // released units rejoin the reserve through UpdateStaging
    UpdateIdleUnits(entities, pathfinder);
}

void AIController::RecallReleasedUnits(EntityManager &entities)
{
    for (int id : m_squadController.TakeReleasedUnits())
    {
        const Unit *u = entities.GetUnit(id);
        if (u && u->alive && u->owner == m_player && !m_garrison.count(id) && !m_squadController.IsSquadMember(id)
            && !m_repairing.count(id) && id != m_scoutId)
        {
            m_recalled.insert(id);
            m_waveRoster.insert(id); // released by its squad: back on the computer's list for the next wave
        }
    }
    // Back home (the reserve radius holds them now), dead, or taken into a new squad: forgotten
    for (auto it = m_recalled.begin(); it != m_recalled.end();)
    {
        const Unit *u = entities.GetUnit(*it);
        const bool home
            = u
              && DetMath::Hypot(static_cast<float>(u->tileX - m_enemyHqX), static_cast<float>(u->tileY - m_enemyHqY))
                     <= 18.0f;
        if (!u || !u->alive || u->state == ActionState::Die || home || m_squadController.IsSquadMember(*it))
            it = m_recalled.erase(it);
        else
            ++it;
    }
}

} // namespace OpenArtOfWar
