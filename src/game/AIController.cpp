#include "graphics/RenderUtility.hpp"
#include "game/AIController.hpp"
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

namespace OpenArtOfWar
{

namespace
{

// Confederation archetypes used by the wave planner
constexpr int kRifleman = 1;
constexpr int kHeavyAssault = 3;   // rocket trooper (ranged support)
constexpr int kFlamethrower = 5;   // close combat
constexpr int kMinAttackWave = 10; // smallest wave the AI marches with
constexpr int kMinAttackGroup = 5; // ... and the smallest group it splits a wave into
constexpr int kFortress = 7;
constexpr int kHammer = 9;
constexpr int kZeus = 11;
constexpr int kGrad = 13; // MLRS (artillery)

// Wave plans use the Confederation archetypes (odd ids); each Resistance unit with the same role
// has the even id right below (rifleman 0, grenadier 2, sniper 4, Coyote 6, Armadillo 8, Rhino 10, Porcupine 12)
int SideArchetype(int confederationArch, Faction side)
{
    return side == Faction::Confederation ? confederationArch : confederationArch - 1;
}

bool IsVehicleArchetype(int arch)
{
    return arch == kFortress || arch == kHammer || arch == kZeus || arch == kGrad;
}

int FallbackArchetypeCost(int arch)
{
    switch (arch)
    {
        case kRifleman:
            return 80;
        case kHeavyAssault:
            return 120;
        case kFlamethrower:
            return 150;
        case kFortress:
            return 220;
        case kHammer:
            return 350;
        case kZeus:
            return 420;
        case kGrad:
            return 400;
        default:
            return 80;
    }
}

const char *TierName(AIWaveTier tier)
{
    switch (tier)
    {
        case AIWaveTier::Scout:
            return "Scout";
        case AIWaveTier::Vanguard:
            return "Vanguard";
        case AIWaveTier::Assault:
            return "Assault";
        case AIWaveTier::Siege:
            return "Siege";
    }
    return "?";
}

void ParseTactics(const YAML::Node &node, AITacticsConfig &t)
{
    if (!node || !node.IsMap())
        return;
    auto f = [&](const char *key, float &out) {
        if (node[key])
            out = node[key].as<float>();
    };
    auto i = [&](const char *key, int &out) {
        if (node[key])
            out = node[key].as<int>();
    };
    auto b = [&](const char *key, bool &out) {
        if (node[key])
            out = node[key].as<bool>();
    };
    f("leash_tiles", t.leashTiles);
    f("leash_per_unit", t.leashPerUnit);
    f("corridor_leash_factor", t.corridorLeashFactor);
    b("wait_at_chokepoints", t.waitAtChokepoints);
    f("pre_choke_hold_seconds", t.preChokeHoldSeconds);
    b("sync_to_slowest", t.syncToSlowest);
    b("armor_front", t.armorFront);
    b("melee_ranged_pairs", t.meleeRangedPairs);
    f("retreat_loss_ratio", t.retreatLossRatio);
    i("wave_base_size", t.waveBaseSize);
    i("wave_growth", t.waveGrowth);
    i("wave_max_size", t.waveMaxSize);
    f("wave_interval_seconds", t.waveIntervalSeconds);
    i("max_wave_tier", t.maxWaveTier);
    i("tier_vanguard_wave", t.vanguardWave);
    i("tier_assault_wave", t.assaultWave);
    i("tier_siege_wave", t.siegeWave);
    f("vehicle_wait_seconds", t.vehicleWaitSeconds);
    f("research_seconds", t.researchSeconds);
    f("straggler_timeout_seconds", t.stragglerTimeoutSeconds);
    i("march_stride_tiles", t.marchStrideTiles);
    i("max_attack_groups", t.maxAttackGroups);
    i("credits_per_extra_group", t.creditsPerExtraGroup);
    b("build_defenses", t.buildDefenses);
    f("defense_first_wave_seconds", t.defenseFirstWaveSeconds);
    f("defense_wave_gap_seconds", t.defenseWaveGapSeconds);
    i("defense_squad_percent", t.defenseSquadPercent);
    f("defense_time_scale", t.defenseTimeScale);
    t.defenseSquadPercent = std::clamp(t.defenseSquadPercent, 10, 100);
    t.maxAttackGroups = std::clamp(t.maxAttackGroups, 1, 5);
}

} // namespace

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
    return e.owner < kMaxPlayers && Players::Hostile(e.owner, m_player);
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

AIWavePlan AIController::PlanWave(int waveNumber, bool hasFactory, bool hasFactoryPlan) const
{
    const AITacticsConfig &t = m_currentDifficultyConfig.tactics;
    AIWavePlan plan;
    plan.waveNumber = std::max(1, waveNumber);

    int size = std::min(t.waveMaxSize, t.waveBaseSize + t.waveGrowth * (plan.waveNumber - 1));
    size = std::clamp(static_cast<int>(std::lround(size * m_debugConfig.waveSpawnMultiplier)), 1, 30);
    size = std::max(size, kMinAttackWave); // the AI never sends a handful: at least ten troops march
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
    plan.archetypes.insert(plan.archetypes.end(), core.begin(), core.end());
    return plan;
}

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
        m_credits += m_creditsPerSec
                     * std::min(Economy::kMaxIncomeHeadquarters, entities.CountBuildingType(UnitType::Headquarters, m_player, true));

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
                std::string(lockedCfg ? lockedCfg->name : "A unit") + " is not researched yet: training another instead");
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
        const size_t before = m_vehicleQueue.size();
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

void AIController::UpdateBaseDefense(EntityManager &entities, Pathfinder &pathfinder)
{
    // Find all player units trespassing within 18 tiles of enemy base
    int threatEntityId = 0;
    float closestDist = 18.0f * TILE_WIDTH;

    for (const auto &e : entities.GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die || !IsFoe(e))
            continue;
        if (!AiSees(e, entities))
            continue; // AI only detects threats it actually sees!
        float dx = e.GetUnitCenterPixelX() - (m_enemyHqX * TILE_WIDTH + TILE_WIDTH * 0.5f);
        float dy = e.GetUnitCenterPixelY() - (m_enemyHqY * TILE_HEIGHT + TILE_HEIGHT * 0.5f);
        float d = std::sqrt(dx * dx + dy * dy);
        if (d < closestDist)
        {
            closestDist = d;
            threatEntityId = e.id;
        }
    }

    if (threatEntityId == 0)
        return;

    Entity *threat = entities.GetEntity(threatEntityId);
    if (!threat)
        return;
    m_lastThreatX = threat->tileX; // the base builder reinforces this side
    m_lastThreatY = threat->tileY;
    if (threat->id != m_lastThreatId)
    {
        m_lastThreatId = threat->id;
        Think(
            "DEFENSE",
            "Enemy spotted near the base at (" + std::to_string(threat->tileX) + ", " + std::to_string(threat->tileY)
                + "): sending idle troops to intercept");
    }

    // Rally nearby idle reserve units to intercept the threat (marching groups keep their own orders)
    std::vector<int> defenders;
    for (const auto &e : entities.GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die || e.isBuilding || e.owner != m_player)
            continue;
        if (m_squadController.IsSquadMember(e.id))
            continue;
        if (e.state == ActionState::Idle && e.targetEntityId == 0)
        {
            float dx = e.GetUnitCenterPixelX() - threat->GetUnitCenterPixelX();
            float dy = e.GetUnitCenterPixelY() - threat->GetUnitCenterPixelY();
            float d = std::sqrt(dx * dx + dy * dy);
            if (d <= 22.0f * TILE_WIDTH)
            {
                defenders.push_back(e.id);
            }
        }
    }

    if (!defenders.empty())
    {
        entities.IssueAttackMoveOrder(defenders, threat->tileX, threat->tileY, pathfinder);
    }
}

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
        if (!u || !IsReserveUnit(*u))
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

    for (size_t i = 0; i < toStage.size() && i < slots.size(); ++i)
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
        if (!u || !IsReserveUnit(*u))
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
        if (u && IsReserveUnit(*u) && u->targetEntityId == 0 && !u->isAttackMoving && u->state == ActionState::Idle)
            idleReserve.push_back(u->id);
    }

    const AIDifficulty difficulty = GetEffectiveDifficulty();
    // Hard / Hardcore: the first raid leaves as soon as the first troops are ready
    if (!m_firstRaidDone && m_trainedUnits > 0
        && (difficulty == AIDifficulty::Hard || difficulty == AIDifficulty::Hardcore) && !ready.empty())
    {
        int targetX, targetY;
        FindAttackObjective(entities, map, targetX, targetY);
        if (targetX >= 0)
        {
            m_firstRaidDone = true;
            const int squadId = m_squadController.CreateSquad(
                ready, targetX, targetY, 0, entities, pathfinder, m_currentDifficultyConfig.tactics);
            std::ostringstream msg;
            msg << "Early raid G" << squadId << " with " << ready.size() << " unit(s) toward (" << targetX << ", "
                << targetY << ")";
            Think("ATTACK", msg.str());
            return;
        }
    }

    int required = static_cast<int>(m_currentPlan.archetypes.size());
    if (required == 0 || m_waveCooldown > 0.0f)
        return;

    // March only with 100% of the planned wave plus the home guard that stays; without production left, send
    // whatever is assembled
    const int guard = HomeGuardCount();
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
        ready = idleReserve;
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
            for (size_t i = 0; i < ready.size(); ++i)
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
        for (size_t i = 0; i < ready.size() && static_cast<int>(picked.size()) < required; ++i)
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
    std::vector<std::vector<int>> members(static_cast<size_t>(groups));
    for (size_t i = 0; i < byType.size(); ++i)
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

// ---------------------------------------------------------------------------
// Skirmish base builder: walls on the side facing the player (with two exits), towers and bunkers
// inside the wall, fragile buildings behind the HQ, and more defenses where the player attacks
// ---------------------------------------------------------------------------

namespace
{

int FactionBuildingIndex(AIDefenseSite::Kind kind, Faction side)
{
    const bool conf = (side == Faction::Confederation);
    switch (kind)
    {
        case AIDefenseSite::Tower:
            return conf ? 7 : 14;
        case AIDefenseSite::Bunker:
            return conf ? 5 : 13;
        case AIDefenseSite::Lab:
            return conf ? 4 : 12;
        case AIDefenseSite::PowerPlant:
            return conf ? 1 : 9;
        case AIDefenseSite::Barracks:
            return conf ? 2 : 10;
        case AIDefenseSite::Factory:
            return conf ? 3 : 11;
        case AIDefenseSite::Headquarters:
            return conf ? 0 : 8;
        case AIDefenseSite::Wall:
            return -1;
    }
    return -1;
}

// What a structure costs the computer: the cost of the building in its character file, the same price the player
// pays (no handicap and no bonus of a difficulty in prices). The old fixed numbers only stand in when there is no
// character file for it.
int DefenseCost(AIDefenseSite::Kind kind, const EntityManager &entities, Faction side)
{
    const int wallIndex = side == Faction::Confederation ? 15 : 25;
    const int index = kind == AIDefenseSite::Wall ? wallIndex : FactionBuildingIndex(kind, side);
    if (const BuildingConfig *cfg = entities.GetBuildingConfig(index))
        return cfg->creditCost;
    switch (kind)
    {
        case AIDefenseSite::Tower:
            return 250;
        case AIDefenseSite::Bunker:
            return 350;
        case AIDefenseSite::Lab:
            return 450;
        case AIDefenseSite::PowerPlant:
            return 200;
        case AIDefenseSite::Barracks:
            return 350;
        case AIDefenseSite::Factory:
            return 500;
        case AIDefenseSite::Wall:
            return 25;
        case AIDefenseSite::Headquarters:
            return 1500;
    }
    return 250;
}

} // namespace

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
    const float wallRadius
        = std::max(static_cast<float>(hqRadius + 4), std::min(static_cast<float>(hqRadius + 10 + m_style.lineOffset), maxD - 1.0f));
    auto ringTile = [&](float radius, float angleOffsetDeg, int &x, int &y) {
        const float a = threatAngle + angleOffsetDeg * static_cast<float>(M_PI) / 180.0f;
        x = std::clamp(static_cast<int>(std::lround(cx + DetMath::Cos(a) * radius)), 1, map.GetWidth() - 2);
        y = std::clamp(static_cast<int>(std::lround(cy + DetMath::Sin(a) * radius)), 1, map.GetHeight() - 2);
    };
    auto add = [&](AIDefenseSite::Kind kind, int x, int y) {
        m_defensePlan.push_back({kind, x, y, false});
    };

    // Power plants, the barracks, the factory and the laboratory come from the core build order (UpdateCoreBuildOrder)

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
        const float gateAngle = DetMath::Atan2(static_cast<float>(m_gateY[0] - cy), static_cast<float>(m_gateX[0] - cx));
        auto angleDiff = [](float a, float b) {
            return std::abs(std::remainder(a - b, 2.0f * static_cast<float>(M_PI)));
        };
        const float rad30 = 30.0f * static_cast<float>(M_PI) / 180.0f;
        const float firstSide
            = angleDiff(gateAngle + rad30, threatAngle) <= angleDiff(gateAngle - rad30, threatAngle) ? 1.0f : -1.0f;
        for (float side : {firstSide, -firstSide})
        {
            const float a = gateAngle + side * rad30;
            const int gx = std::clamp(static_cast<int>(std::lround(cx + DetMath::Cos(a) * wallRadius)), 1, map.GetWidth() - 2);
            const int gy = std::clamp(static_cast<int>(std::lround(cy + DetMath::Sin(a) * wallRadius)), 1, map.GetHeight() - 2);
            if (map.IsPassable(gx, gy))
            {
                m_gateX[1] = gx;
                m_gateY[1] = gy;
                break;
            }
        }
        const int rows = m_style.towersOnly ? 0 : std::clamp(m_style.wallThickness, 1, 2);
        const float halfArc = 40.0f * static_cast<float>(M_PI) / 180.0f;
        for (int row = 0; row < rows; ++row)
        {
            const float radius = wallRadius + static_cast<float>(row);
            const float step = 0.45f / radius;
            for (float a = -halfArc; a <= halfArc; a += step)
            {
                const int wx = static_cast<int>(std::lround(cx + DetMath::Cos(threatAngle + a) * radius));
                const int wy = static_cast<int>(std::lround(cy + DetMath::Sin(threatAngle + a) * radius));
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
                if (nearGate || IsRoad(wx, wy) || !IsOwnGround(wx, wy) || !map.IsPassable(wx, wy) || map.GetWall(wx, wy) > 0)
                    continue;
                wallTiles.emplace_back(wx, wy);
                wallRow.push_back(row);
            }
        }
        // Towers right behind the main exit, a bunker covering the second one, the flanks of the arc
        const float innerRadius = wallRadius - 3.0f;
        int x = 0, y = 0;
        ringTile(innerRadius, 0.0f, x, y);
        add(AIDefenseSite::Tower, x - 1, y - 1);
        ringTile(innerRadius, -30.0f, x, y);
        add(m_style.bunkerHeavy ? AIDefenseSite::Tower : AIDefenseSite::Bunker, x - 1, y - 1);
        ringTile(innerRadius, 30.0f, x, y);
        add(AIDefenseSite::Tower, x - 1, y - 1);
    }
    // More firepower when the walls are few
    if (m_style.towersOnly)
    {
        int x = 0, y = 0;
        ringTile(wallRadius - 2.0f, -15.0f, x, y);
        add(AIDefenseSite::Tower, x - 1, y - 1);
        ringTile(wallRadius - 2.0f, 15.0f, x, y);
        add(AIDefenseSite::Bunker, x - 1, y - 1);
        if (passages)
            wallTiles.clear();
    }

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
        std::vector<uint8_t> ring(static_cast<size_t>(mw) * mh, 0), inside(ring.size(), 0), outside(ring.size(), 0);
        for (size_t i = 0; i < wallTiles.size(); ++i)
            if (wallRow[i] == 0)
                ring[idx(wallTiles[i].first, wallTiles[i].second)] = 1; // the outer row lies outside the ring
        auto distOf = [&](int px, int py) {
            return DetMath::Hypot(static_cast<float>(px - cx), static_cast<float>(py - cy));
        };
        auto flood = [&](std::vector<uint8_t> &mark, std::vector<std::pair<int, int>> frontier, bool in) {
            for (size_t i = 0; i < frontier.size(); ++i)
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
        std::vector<uint8_t> kept(wallTiles.size(), 0);
        for (size_t i = 0; i < wallTiles.size(); ++i)
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
        for (size_t i = 0; i < wallTiles.size(); ++i)
        {
            if (wallRow[i] == 0)
                continue;
            for (size_t j = 0; j < wallTiles.size() && !kept[i]; ++j)
                kept[i] = wallRow[j] == 0 && kept[j]
                          && std::max(std::abs(wallTiles[i].first - wallTiles[j].first),
                                      std::abs(wallTiles[i].second - wallTiles[j].second)) <= 1;
        }
        for (size_t i = 0; i < wallTiles.size(); ++i)
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

    std::ostringstream plan;
    plan << "Base plan: " << m_defensePlan.size() << " items, " << wallTiles.size()
         << (passages ? " wall tiles across passages" : " wall tiles in front of the base") << ", exits (" << m_gateX[0]
         << "," << m_gateY[0] << ") and (" << m_gateX[1] << "," << m_gateY[1] << ")";
    Think("BUILD", plan.str());
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

    const int index = FactionBuildingIndex(site.kind, side());
    const BuildingConfig *cfg = entities.GetBuildingConfig(index);
    const int w = cfg ? cfg->width : 2;
    const int h = cfg ? cfg->height : 2;
    static const char *const kKindNames[] = {
        "tower", "bunker", "wall", "laboratory (behind the HQ)", "power plant (behind the HQ)", "barracks", "factory",
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
        site.done = true;
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

void AIController::UpdateIntel(EntityManager &entities)
{
    m_intelTimer -= SIMULATION_TIMESTEP;
    if (m_intelTimer > 0.0f)
        return;
    m_intelTimer = 1.0f;
    const bool seesAll = GetEffectiveDifficulty() == AIDifficulty::Hardcore;
    std::set<int> alive;
    for (const auto &e : entities.GetAllEntities())
    {
        if (!e.alive || e.state == ActionState::Die || !IsFoe(e))
            continue;
        alive.insert(e.id);
        if (!seesAll && !AiSees(e, entities))
            continue;
        if (const Building *b = e.AsBuilding())
        {
            if (b->rawBuildingType < 15)
                m_knownBuildings[e.id] = {b->tileX + b->buildWidth / 2, b->tileY + b->buildHeight / 2};
        }
        else if (const Unit *u = e.AsUnit())
        {
            m_seenEnemyUnits[e.id] = !Utility::IsInfantryArchetype(u->archetypeId);
        }
    }
    // Destroyed things are forgotten
    for (auto it = m_knownBuildings.begin(); it != m_knownBuildings.end();)
        it = alive.count(it->first) ? std::next(it) : m_knownBuildings.erase(it);
    for (auto it = m_seenEnemyUnits.begin(); it != m_seenEnemyUnits.end();)
        it = alive.count(it->first) ? std::next(it) : m_seenEnemyUnits.erase(it);
}

void AIController::UpdateScouting(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
{
    const AIDifficulty difficulty = GetEffectiveDifficulty();
    if (difficulty == AIDifficulty::Hardcore || (m_playerHqX <= 0 && m_playerHqY <= 0))
        return;
    m_scoutTimer -= deltaTime;
    Unit *scout = m_scoutId ? entities.GetUnit(m_scoutId) : nullptr;
    if (m_scoutId && (!scout || !scout->alive || scout->state == ActionState::Die))
    {
        Think("SCOUT", "The scout was lost");
        m_scoutId = 0;
        scout = nullptr;
    }
    if (scout)
    {
        // Back to the reserve once it got there, or when it stands without orders for a while
        const bool there = std::max(std::abs(scout->tileX - m_scoutX), std::abs(scout->tileY - m_scoutY)) <= 3;
        const bool idle = scout->state == ActionState::Idle && !scout->HasMoveTarget() && !scout->waitingForPath;
        m_scoutIdleTimer = idle ? m_scoutIdleTimer + deltaTime : 0.0f;
        if (there || m_scoutIdleTimer > 3.0f)
        {
            std::ostringstream msg;
            msg << "Scouting trip " << m_scoutTrips << " over: " << m_knownBuildings.size() << " enemy structures and "
                << m_seenEnemyUnits.size() << " units known";
            Think("SCOUT", msg.str());
            entities.IssueMoveOrder({scout->id}, m_stagingX, m_stagingY, pathfinder, false);
            m_waveRoster.insert(scout->id);
            m_recalled.insert(scout->id);
            m_scoutId = 0;
        }
        return;
    }
    if (m_scoutTimer > 0.0f)
        return;
    // The cheapest reserve unit goes
    int bestId = 0, bestCost = 1 << 30;
    for (const auto &e : entities.GetAllEntities())
    {
        const Unit *u = e.AsUnit();
        if (!u || !IsReserveUnit(*u) || m_repairing.count(u->id) || u->targetEntityId != 0)
            continue;
        const UnitConfig *cfg = entities.GetUnitConfig(u->archetypeId);
        const int cost = cfg ? cfg->creditCost : 1000;
        if (cost < bestCost)
        {
            bestCost = cost;
            bestId = u->id;
        }
    }
    if (!bestId)
        return;
    // First the enemy's base, then its flanks and where it last came from, in turn
    int tx = m_playerHqX, ty = m_playerHqY;
    if (m_scoutTrips % 3 == 1)
        FlankPoint(2, m_playerHqX, m_playerHqY, map, tx, ty);
    else if (m_scoutTrips % 3 == 2)
        FlankPoint(3, m_playerHqX, m_playerHqY, map, tx, ty);
    if (tx < 0)
    {
        tx = m_playerHqX;
        ty = m_playerHqY;
    }
    m_waveRoster.erase(bestId);
    m_scoutId = bestId;
    m_scoutIdleTimer = 0.0f;
    ++m_scoutTrips;
    entities.IssueAttackMoveOrder({bestId}, tx, ty, pathfinder);
    m_scoutX = tx;
    m_scoutY = ty;
    const float interval = difficulty == AIDifficulty::Easy     ? 180.0f
                           : difficulty == AIDifficulty::Medium ? 120.0f
                                                                : 90.0f;
    m_scoutTimer = interval;
    std::ostringstream msg;
    msg << "Sending a scout (trip " << m_scoutTrips << ") toward (" << tx << ", " << ty << ")";
    Think("SCOUT", msg.str());
}

bool AIController::IsValuable(const Unit &u, const EntityManager &entities) const
{
    // Siege forms count as their mobile form: Sniper 14 -> 4, Rhino 15 -> 10, Grad 16 -> 13
    auto baseOf = [](int arch) {
        return arch == 14 ? 4 : arch == 15 ? 10 : arch == 16 ? 13 : arch;
    };
    const int reference = side() == Faction::Confederation ? kZeus : 10; // Zeus / Rhino
    const UnitConfig *ref = entities.GetUnitConfig(reference);
    const UnitConfig *own = entities.GetUnitConfig(baseOf(u.archetypeId));
    return ref && own && own->creditCost >= ref->creditCost;
}

void AIController::UpdateValuableRetreat(EntityManager &entities, Pathfinder &pathfinder)
{
    const AIDifficulty difficulty = GetEffectiveDifficulty();
    const float threshold = difficulty == AIDifficulty::Easy     ? 0.0f
                            : difficulty == AIDifficulty::Medium ? 0.25f
                            : difficulty == AIDifficulty::Hard   ? 0.30f
                                                                 : 0.35f;
    // Where vehicles get repaired: next to an own Factory; otherwise (and for infantry) the staging area
    int factoryX = -1, factoryY = -1;
    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->owner == m_player && b->type == UnitType::Factory && b->buildProgress >= 1.0f)
        {
            factoryX = b->tileX + b->buildWidth / 2;
            factoryY = b->tileY + b->buildHeight + 1;
            break;
        }
    }
    // Healed (or dead): back to the reserve
    for (auto it = m_repairing.begin(); it != m_repairing.end();)
    {
        Unit *u = entities.GetUnit(*it);
        if (!u || !u->alive || u->state == ActionState::Die)
        {
            it = m_repairing.erase(it);
            continue;
        }
        if (u->currentHp * 10 >= u->maxHp * 9)
        {
            m_waveRoster.insert(u->id);
            Think("RETREAT", "A repaired unit rejoins the reserve");
            it = m_repairing.erase(it);
            continue;
        }
        const bool vehicle = !Utility::IsInfantryArchetype(u->archetypeId);
        const int rx = vehicle && factoryX >= 0 ? factoryX : m_stagingX;
        const int ry = vehicle && factoryX >= 0 ? factoryY : m_stagingY;
        if (u->state == ActionState::Idle && !u->HasMoveTarget()
            && std::max(std::abs(u->tileX - rx), std::abs(u->tileY - ry)) > 3)
            entities.IssueMoveOrder({u->id}, rx, ry, pathfinder, false);
        ++it;
    }
    if (threshold <= 0.0f)
        return;
    for (const AISquad &sq : m_squadController.GetSquads())
    {
        if (sq.playerSquad || sq.unitIds.size() <= 3)
            continue; // a few units left: they fight to the end
        std::vector<int> leaving;
        for (int id : sq.unitIds)
        {
            const Unit *u = entities.GetUnit(id);
            if (u && u->maxHp > 0 && static_cast<float>(u->currentHp) < threshold * static_cast<float>(u->maxHp)
                && IsValuable(*u, entities) && sq.unitIds.size() - leaving.size() > 4)
                leaving.push_back(id);
        }
        if (leaving.empty())
            continue;
        m_squadController.ReleaseUnits(leaving, entities);
        for (int id : leaving)
        {
            m_waveRoster.erase(id);
            m_repairing.insert(id);
            if (Unit *u = entities.GetUnit(id))
            {
                u->targetEntityId = 0;
                u->isAttackMoving = false;
                const bool vehicle = !Utility::IsInfantryArchetype(u->archetypeId);
                const int rx = vehicle && factoryX >= 0 ? factoryX : m_stagingX;
                const int ry = vehicle && factoryX >= 0 ? factoryY : m_stagingY;
                entities.IssueMoveOrder({id}, rx, ry, pathfinder, false);
            }
        }
        Think("RETREAT", std::to_string(leaving.size()) + " badly hurt expensive unit(s) pull back for repairs");
        break; // squads changed: the rest next time
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
        if (!full || hqCount >= Economy::kMaxIncomeHeadquarters || barracksDone == 0 || factoryDone == 0 || labDone == 0)
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
        wants.push_back({AIDefenseSite::PowerPlant, 0, "The next building would leave the base without power: a power plant first"});
    if (barracksCount == 0)
        wants.push_back({AIDefenseSite::Barracks, 0, "No barracks: raising one to start training troops"});
    if (needLab && m_style.labEarly)
        wants.push_back({AIDefenseSite::Lab, kTroopReserve / 2, "No laboratory: research needs one (early in this base's plan)"});
    if (full && m_style.barracksFirst && barracksCount < 2 && factoryCount == 0)
        wants.push_back({AIDefenseSite::Barracks, kTroopReserve / 2, "A second barracks before the factory: infantry first"});
    if (factoryCount == 0)
        wants.push_back({AIDefenseSite::Factory, kTroopReserve / 2, "No factory: building one for vehicles"});
    if (needLab)
        wants.push_back({AIDefenseSite::Lab, kTroopReserve / 2, "No laboratory: research needs one"});
    // (spare power after the laboratory: the plants would take the back of the base it belongs in)
    if (produced - consumed < sparePower && m_credits >= powerCost + kTroopReserve)
        wants.push_back({AIDefenseSite::PowerPlant, kTroopReserve, "Credits to spare: an extra power plant for the next buildings"});
    if (baseReady())
        wants.push_back({AIDefenseSite::Headquarters, 2 * kTroopReserve, "The base is ready and quiet: another headquarters for more income"});
    if (full && barracksCount < kMaxProduction && barracksCount <= factoryCount)
        wants.push_back({AIDefenseSite::Barracks, kTroopReserve, "Credits to spare: another barracks to raise the army faster"});
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
        const bool needsExit = kind == AIDefenseSite::Barracks || kind == AIDefenseSite::Factory
                               || kind == AIDefenseSite::Headquarters;
        // A gap around it first, then side by side, then also on the band along a road (never on the road itself)
        for (int crowding = 0; crowding < 3 && !found; ++crowding)
        {
            found = FindCoreSite(x, y, w, h, ox, oy, rear, minRing, map, entities, crowding, needsExit);
            if (!found && rear && kind != AIDefenseSite::PowerPlant)
                found = FindCoreSite(x, y, w, h, ox, oy, false, minRing, map, entities, crowding, needsExit); // the back is full
            if (!found && kind == AIDefenseSite::PowerPlant)
            {
                // The back is full: the sides of the base, never its front
                found = FindCoreSite(x, y, w, h, sideX, sideY, false, minRing, map, entities, crowding, needsExit);
                if (found)
                {
                    const float px = x + w * 0.5f - cx, py = y + h * 0.5f - cy;
                    found = px * tdx + py * tdy <= 0.35f * DetMath::Hypot(px, py);
                }
            }
        }
        if (!found)
        {
            ThinkOnce("core" + std::to_string(kind), "BUILD", std::string(want.why) + " (no room that keeps the base open)");
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

void AIController::UpdateIdleUnits(EntityManager &entities, Pathfinder &pathfinder)
{
    for (auto &e : entities.GetAllEntities())
    {
        Unit *u = e.AsUnit();
        if (!u || !u->alive || u->state == ActionState::Die || u->owner != m_player)
            continue;
        // A squad's members take orders from their squad only (a held member waits for its group); the scout
        // and units on their way to repairs do not pick fights either
        if (m_squadController.IsSquadMember(u->id) || u->id == m_scoutId || m_repairing.count(u->id))
            continue;
        if (u->state == ActionState::Idle && u->targetEntityId == 0 && !u->waitingForPath && !u->isAttackMoving)
        {
            float closestDist = u->sightRadius * TILE_WIDTH;
            int targetId = 0;

            for (const auto &other : entities.GetAllEntities())
            {
                if (!other.alive || other.state == ActionState::Die || !IsFoe(other))
                    continue;
                if (!AiSees(other, entities))
                    continue; // AI only engages units visible to Confederation!
                float dx = other.GetUnitCenterPixelX() - u->GetUnitCenterPixelX();
                float dy = other.GetUnitCenterPixelY() - u->GetUnitCenterPixelY();
                float d = DetMath::Hypot(dx, dy);
                if (d < closestDist)
                {
                    closestDist = d;
                    targetId = other.id;
                }
            }

            if (targetId != 0)
            {
                entities.IssueAttackOrder({u->id}, targetId, &pathfinder);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Debug visualization (--debug mode)
// ---------------------------------------------------------------------------

void AIController::RenderDebug(
    Renderer &renderer, BitmapFontRenderer &font, const Camera &camera, const EntityManager &entities) const
{
    if (m_debugConfig.visualizeChokepoints && m_chokes.Width() > 0)
    {
        float x0, y0, x1, y1;
        camera.ScreenToWorld(0.0f, 0.0f, x0, y0);
        camera.ScreenToWorld(
            static_cast<float>(camera.GetViewportWidth()), static_cast<float>(camera.GetViewportHeight()), x1, y1);
        int tx0 = std::max(0, static_cast<int>(x0 / TILE_WIDTH));
        int ty0 = std::max(0, static_cast<int>(y0 / TILE_HEIGHT));
        int tx1 = std::min(m_chokes.Width() - 1, static_cast<int>(x1 / TILE_WIDTH) + 1);
        int ty1 = std::min(m_chokes.Height() - 1, static_cast<int>(y1 / TILE_HEIGHT) + 1);
        for (int ty = ty0; ty <= ty1; ++ty)
        {
            for (int tx = tx0; tx <= tx1; ++tx)
            {
                if (!m_chokes.IsChoke(tx, ty))
                    continue;
                renderer.FillRectWorld(
                    tx * TILE_WIDTH + 1.0f,
                    ty * TILE_HEIGHT + 1.0f,
                    TILE_WIDTH - 2.0f,
                    TILE_HEIGHT - 2.0f,
                    camera,
                    {255, 140, 30, 70});
            }
        }
    }

    if (m_debugConfig.visualizeSquadLeash)
    {
        // Staging area marker
        RenderUtility::DrawTileRadius(
            renderer,
            camera,
            m_stagingX * TILE_WIDTH + TILE_WIDTH * 0.5f,
            m_stagingY * TILE_HEIGHT + TILE_HEIGHT * 0.5f,
            7.0f,
            SDL_Color {120, 200, 255, 90},
            1.5f);
        m_squadController.RenderDebug(renderer, font, camera, entities, m_currentDifficultyConfig.tactics.leashTiles);
    }

    // Director summary in the top-left corner of the map view
    std::string line1 = "AI " + DifficultyToString(GetEffectiveDifficulty())
                        + (m_debugConfig.forceDebugOverride ? " (debug override)" : "") + "  |  credits "
                        + std::to_string(m_credits);
    std::string line2
        = "Wave " + std::to_string(m_currentPlan.waveNumber) + " " + TierName(m_currentPlan.tier) + ": ready "
          + std::to_string(m_readyUnits) + "/" + std::to_string(m_currentPlan.archetypes.size())
          + (m_waveCooldown > 0.0f ? "  next in " + std::to_string(static_cast<int>(m_waveCooldown)) + "s" : "")
          + "  |  groups " + std::to_string(m_squadController.GetSquads().size());
    std::string line3 = "Research: ";
    if (m_researchIndex >= kResearchCount)
    {
        line3 += "complete";
    }
    else
    {
        line3 += std::string(researchDef(side(), m_researchIndex).name)
                 + (m_researchActive ? " " + std::to_string(static_cast<int>(GetResearchProgress() * 100.0f)) + "%"
                                     : (m_hasLab ? " (saving credits)" : " (no lab)"));
    }
    int y = 40;
    for (const std::string *l : {&line1, &line2, &line3})
    {
        int w = font.GetTextWidth(*l, 1) + 12;
        renderer.FillRectScreen(6, y, w, 18, {10, 12, 16, 200});
        font.DrawString(*l, 12, y + 2, 1, {226, 231, 238, 255}, false);
        y += 20;
    }
}

void AIController::OnEntityAttacked(int victimId, int attackerId, EntityManager &entities, Pathfinder &pathfinder)
{
    Entity *victim = entities.GetEntity(victimId);
    Entity *attacker = entities.GetEntity(attackerId);
    if (!victim || !attacker || victim->owner != m_player)
        return;

    if (victim->targetEntityId == 0)
    {
        victim->targetEntityId = attackerId;
    }

    // Call for help: alert nearby allies within 14 tiles
    std::vector<int> allies;
    for (auto &ally : entities.GetAllEntities())
    {
        if (!ally.alive || ally.state == ActionState::Die || ally.isBuilding || ally.owner != m_player)
            continue;
        // Members of a squad are not called away: their squad leads them (it fights when attacked on the way)
        if (m_squadController.IsSquadMember(ally.id))
            continue;
        if (ally.state == ActionState::Idle && ally.targetEntityId == 0)
        {
            float dx = ally.GetUnitCenterPixelX() - victim->GetUnitCenterPixelX();
            float dy = ally.GetUnitCenterPixelY() - victim->GetUnitCenterPixelY();
            if (std::sqrt(dx * dx + dy * dy) <= 14.0f * TILE_WIDTH)
            {
                allies.push_back(ally.id);
            }
        }
    }
    if (!allies.empty())
    {
        entities.IssueAttackMoveOrder(allies, attacker->tileX, attacker->tileY, pathfinder);
    }
}

void AIController::OnStructureAttacked(int structureId, int attackerId, EntityManager &entities, Pathfinder &pathfinder)
{
    m_baseQuietSeconds = 0.0f; // no new headquarters while the base is under fire
    OnEntityAttacked(structureId, attackerId, entities, pathfinder);
}

void AIController::RecordStartingBase(const EntityManager &entities, const TileMap &map)
{
    m_startingBuildings.clear();
    m_startingWalls.clear();

    for (const auto &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->owner != m_player)
            continue;

        AIStructurePlan plan;
        plan.type = b->type;
        plan.rawBuildingType = b->rawBuildingType;
        plan.tileX = b->tileX;
        plan.tileY = b->tileY;
        plan.width = b->buildWidth;
        plan.height = b->buildHeight;
        plan.destroyed = false;
        plan.rebuildTimer = 0.0f;

        // The price is the one in the character file of the building
        const BuildingConfig *bcfg = entities.GetBuildingConfig(b->rawBuildingType);
        plan.creditCost = bcfg ? bcfg->creditCost : 250;

        m_startingBuildings.push_back(plan);
    }

    // Record all walls of the AI side
    for (int y = 0; y < map.GetHeight(); ++y)
    {
        for (int x = 0; x < map.GetWidth(); ++x)
        {
            if (map.GetWall(x, y) > 0 && map.GetWallOwner(x, y) == m_player)
            {
                AIWallPlan wp;
                wp.tileX = x;
                wp.tileY = y;
                wp.faction = side();
                {
                    const BuildingConfig *wcfg = entities.GetBuildingConfig(side() == Faction::Confederation ? 15 : 25);
                    wp.creditCost = wcfg ? wcfg->creditCost : 25;
                }
                wp.destroyed = false;
                wp.rebuildTimer = 0.0f;
                m_startingWalls.push_back(wp);
            }
        }
    }

    m_startingBaseRecorded = true;
    std::cout << "[AIController] Recorded starting base: " << m_startingBuildings.size() << " structures, "
              << m_startingWalls.size() << " walls for rebuilding memory." << std::endl;
}

void AIController::UpdateRebuilding(float deltaTime, TileMap &map, EntityManager &entities)
{
    if (!m_startingBaseRecorded)
    {
        RecordStartingBase(entities, map);
        return;
    }

    // Without a finished headquarters nothing is built, exactly as for a person (CanPlaceBuilding says so too)
    const bool canBuild = entities.HasBuildingType(UnitType::Headquarters, m_player, true);

    // 1. Check building status & rebuild destroyed ones in the exact same location
    for (auto &plan : m_startingBuildings)
    {
        if (!plan.destroyed)
        {
            bool stillAlive = false;
            for (const auto &e : entities.GetAllEntities())
            {
                if (e.alive && e.state != ActionState::Die && e.isBuilding && e.owner == m_player
                    && e.tileX == plan.tileX && e.tileY == plan.tileY)
                {
                    stillAlive = true;
                    break;
                }
            }
            if (!stillAlive)
            {
                plan.destroyed = true;
                plan.rebuildTimer = 0.0f;
                Think(
                    "BUILD",
                    "Structure at (" + std::to_string(plan.tileX) + ", " + std::to_string(plan.tileY)
                        + ") destroyed: queued for reconstruction");
            }
        }
        else
        {
            // One laboratory is enough: the core build order may have put up another one elsewhere meanwhile
            if (plan.type == UnitType::ResearchLab && entities.HasBuildingType(UnitType::ResearchLab, m_player, false))
                continue;
            plan.rebuildTimer += deltaTime;
            if (canBuild && plan.rebuildTimer >= 6.0f && m_credits >= plan.creditCost
                && IsBuildingUnlocked(m_player, plan.rawBuildingType))
            {
                bool siteClear
                    = entities.CanPlaceBuilding(plan.tileX, plan.tileY, plan.width, plan.height, map, m_player);
                // Foresight as for a new building: one that takes power waits for the plants (rebuilt first)
                const int power = entities.BuildingPower(plan.rawBuildingType);
                if (siteClear && power < 0 && PowerBalance(entities, false) + power < 0)
                {
                    m_powerNeeded = std::max(m_powerNeeded, -power);
                    ThinkOnce("rebuildpower", "BUILD", "Rebuilding waits for power: a power plant first");
                    siteClear = false;
                }
                if (siteClear)
                {
                    m_credits -= plan.creditCost;
                    int newId = entities.SpawnBuildingByIndex(
                        plan.rawBuildingType, plan.tileX, plan.tileY, side(), 0.0f, m_player);
                    if (newId > 0)
                    {
                        for (int ty = plan.tileY; ty < plan.tileY + plan.height && ty < map.GetHeight(); ++ty)
                        {
                            for (int tx = plan.tileX; tx < plan.tileX + plan.width && tx < map.GetWidth(); ++tx)
                            {
                                map.SetPassable(tx, ty, false);
                            }
                        }
                        plan.destroyed = false;
                        plan.rebuildTimer = 0.0f;
                        Think(
                            "BUILD",
                            "Rebuilt the structure at (" + std::to_string(plan.tileX) + ", "
                                + std::to_string(plan.tileY) + ") for $" + std::to_string(plan.creditCost));
                    }
                }
            }
        }
    }

    // 2. Check wall status & rebuild destroyed walls in the exact same location
    for (auto &wp : m_startingWalls)
    {
        if (!wp.destroyed)
        {
            if (map.GetWall(wp.tileX, wp.tileY) == 0)
            {
                wp.destroyed = true;
                wp.rebuildTimer = 0.0f;
            }
        }
        else
        {
            wp.rebuildTimer += deltaTime;
            if (canBuild && wp.rebuildTimer >= 8.0f && m_credits >= wp.creditCost)
            {
                // Not on a unit that stands even partly on the tile (it would be walled in)
                if (!entities.IsTileOccupiedByUnit(wp.tileX, wp.tileY))
                {
                    m_credits -= wp.creditCost;
                    map.SetWall(wp.tileX, wp.tileY, wp.faction, 100, m_player);
                    wp.destroyed = false;
                    wp.rebuildTimer = 0.0f;
                    ThinkOnce(
                        "wall",
                        "BUILD",
                        "Patching the wall at (" + std::to_string(wp.tileX) + ", " + std::to_string(wp.tileY) + ")");
                }
            }
        }
    }
}

} // namespace OpenArtOfWar
