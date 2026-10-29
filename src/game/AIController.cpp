#include "graphics/RenderUtility.hpp"
#include "game/AIController.hpp"
#include "core/DetMath.hpp"
#include "core/AssetStore.hpp"
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
        int sideArchetype(int confederationArch, Faction side)
        {
            return side == Faction::Confederation ? confederationArch : confederationArch - 1;
        }

        bool isVehicleArchetype(int arch)
        {
            return arch == kFortress || arch == kHammer || arch == kZeus || arch == kGrad;
        }

        int fallbackArchetypeCost(int arch)
        {
            switch(arch)
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

        const char *tierName(AIWaveTier tier)
        {
            switch(tier)
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

        void parseTactics(const YAML::Node &node, AITacticsConfig &t)
        {
            if(!node || !node.IsMap())
                return;
            auto f = [&](const char *key, float &out)
            {
                if(node[key])
                    out = node[key].as<float>();
            };
            auto i = [&](const char *key, int &out)
            {
                if(node[key])
                    out = node[key].as<int>();
            };
            auto b = [&](const char *key, bool &out)
            {
                if(node[key])
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

    std::string difficultyToString(AIDifficulty diff)
    {
        switch(diff)
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

    AIDifficulty stringToDifficulty(const std::string &str)
    {
        if(str == "easy" || str == "Easy")
            return AIDifficulty::Easy;
        if(str == "hard" || str == "Hard")
            return AIDifficulty::Hard;
        if(str == "hardcore" || str == "Hardcore")
            return AIDifficulty::Hardcore;
        return AIDifficulty::Medium;
    }

    void AIController::initDifficultyDefaults()
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
        easy.tactics.maxWaveTier = 3;
        easy.tactics.researchSeconds = 60.0f;
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
        med.tactics.researchSeconds = 45.0f;
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
        hard.tactics.researchSeconds = 30.0f;
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
        hardcore.tactics.researchSeconds = 25.0f;
        hardcore.tactics.maxAttackGroups = 5;
        hardcore.tactics.creditsPerExtraGroup = 500;
        m_difficultyConfigs[AIDifficulty::Hardcore] = hardcore;

        m_difficulty = AIDifficulty::Medium;
        refreshDifficulty();
        m_credits = m_currentDifficultyConfig.startingCredits;
    }

    bool AIController::loadDifficultyConfig(const std::string &path)
    {
        try
        {
            std::string text;
            if(!Assets::readText(path, text))
                return false;
            YAML::Node root = YAML::Load(text);
            if(!root.IsDefined() || !root.IsMap())
                return false;

            auto parseMode = [&](const std::string &key, AIDifficulty diff)
            {
                if(!root[key] || !root[key].IsMap())
                    return;
                YAML::Node node = root[key];
                AIDifficultyConfig &cfg = m_difficultyConfigs[diff];
                if(node["id"])
                    cfg.id = node["id"].as<std::string>();
                if(node["name"])
                    cfg.name = node["name"].as<std::string>();
                if(node["description"])
                    cfg.description = node["description"].as<std::string>();
                if(node["starting_credits"])
                    cfg.startingCredits = node["starting_credits"].as<int>();
                if(node["credits_per_second"])
                    cfg.creditsPerSec = node["credits_per_second"].as<int>();
                if(node["resource_multiplier"])
                    cfg.resourceMultiplier = node["resource_multiplier"].as<float>();
                if(node["production_speed_multiplier"])
                    cfg.productionSpeedMultiplier = node["production_speed_multiplier"].as<float>();
                parseTactics(node["tactics"], cfg.tactics);
            };

            parseMode("easy", AIDifficulty::Easy);
            parseMode("medium", AIDifficulty::Medium);
            parseMode("hard", AIDifficulty::Hard);
            parseMode("hardcore", AIDifficulty::Hardcore);

            if(root["default_difficulty"])
            {
                std::string defDiff = root["default_difficulty"].as<std::string>();
                setDifficulty(stringToDifficulty(defDiff));
            }
            else
            {
                setDifficulty(m_difficulty);
            }
            std::cout << "[AIController] Loaded difficulty configuration from " << path << std::endl;
            return true;
        }
        catch(const std::exception &e)
        {
            std::cerr << "[AIController] Warning: Could not load " << path << " (" << e.what() << "), using defaults." << std::endl;
            return false;
        }
    }

    AIDifficulty AIController::getEffectiveDifficulty() const
    {
        if(m_debugConfig.forceDebugOverride)
        {
            return static_cast<AIDifficulty>(std::clamp(m_debugConfig.debugDifficultyLevel, 0, 3));
        }
        return m_difficulty;
    }

    void AIController::refreshDifficulty()
    {
        auto it = m_difficultyConfigs.find(getEffectiveDifficulty());
        if(it != m_difficultyConfigs.end())
        {
            m_currentDifficultyConfig = it->second;
        }
        m_creditsPerSec = m_currentDifficultyConfig.creditsPerSec;
    }

    void AIController::setDifficulty(AIDifficulty difficulty)
    {
        m_difficulty = difficulty;
        refreshDifficulty();
    }

    void AIController::setDebugConfig(const AIDebugConfig &config)
    {
        bool difficultyChanged = config.forceDebugOverride != m_debugConfig.forceDebugOverride || config.debugDifficultyLevel != m_debugConfig.debugDifficultyLevel;
        bool sizeChanged = config.waveSpawnMultiplier != m_debugConfig.waveSpawnMultiplier;
        m_debugConfig = config;
        m_debugConfig.waveSpawnMultiplier = std::clamp(m_debugConfig.waveSpawnMultiplier, 0.25f, 4.0f);
        if(difficultyChanged)
        {
            refreshDifficulty();
            std::cout << "[AIController] Debug difficulty -> " << difficultyToString(getEffectiveDifficulty()) << std::endl;
        }
        if(difficultyChanged || sizeChanged)
        {
            // Re-plan the wave that is being assembled with the new parameters
            m_infantryQueue.clear();
            m_vehicleQueue.clear();
            m_currentPlan.archetypes.clear();
        }
    }

    void AIController::think(const std::string &category, const std::string &text)
    {
        m_thoughts.push_back({m_gameTime, category, text});
        ++m_thoughtSerial;
        while(m_thoughts.size() > 300)
            m_thoughts.pop_front();
        std::cout << "[AIController] " << text << std::endl;
    }

    void AIController::thinkOnce(const std::string &key, const std::string &category, const std::string &text)
    {
        auto it = m_lastThoughtByKey.find(key);
        if(it != m_lastThoughtByKey.end() && it->second == text)
            return;
        m_lastThoughtByKey[key] = text;
        think(category, text);
    }

    void AIController::setEnemyCredits(int credits)
    {
        const int before = m_credits;
        m_credits = std::max(0, credits);
        think("ECONOMY", "Treasury set by debug: " + std::to_string(before) + " -> " + std::to_string(m_credits));
    }

    std::vector<std::string> AIController::currentIntentions() const
    {
        std::vector<std::string> out;
        const int required = static_cast<int>(m_currentPlan.archetypes.size());
        if(required > 0)
        {
            std::ostringstream w;
            w << "Assembling wave " << (m_waveCount + 1) << " (" << tierName(m_currentPlan.tier) << "): " << m_readyUnits << "/" << required << " ready";
            if(m_waveCooldown > 0.0f)
                w << ", may launch in " << static_cast<int>(m_waveCooldown) << " s";
            if(!m_hasProduction)
                w << " - no barracks / factory left";
            out.push_back(w.str());
            std::ostringstream q;
            q << "Training queue: " << m_infantryQueue.size() << " infantry, " << m_vehicleQueue.size() << " vehicles";
            out.push_back(q.str());
        }
        if(!m_squadController.getSquads().empty())
        {
            std::ostringstream g;
            g << m_squadController.getSquads().size() << " group(s) on the march";
            out.push_back(g.str());
        }
        if(m_researchIndex < kResearchCount)
        {
            const ResearchDef &r = researchDef(side(), m_researchIndex);
            std::ostringstream t;
            if(!m_hasLab)
                t << "Research paused: no Tech Center / Laboratory (next: " << r.name << ")";
            else if(m_researchActive)
                t << "Researching " << r.name << " " << static_cast<int>(getResearchProgress() * 100.0f) << "%";
            else
                t << "Saving " << r.cost << " credits for " << r.name;
            out.push_back(t.str());
        }
        if(m_skirmish && m_defensePlanned)
        {
            int left = 0;
            for(const auto &d : m_defensePlan)
                left += d.done ? 0 : 1;
            out.push_back(left > 0 ? "Fortifying the base: " + std::to_string(left) + " items left" : "Base fortified, " + std::to_string(m_expansions) + " extra defenses");
        }
        int destroyed = 0;
        for(const auto &p : m_startingBuildings)
            destroyed += p.destroyed ? 1 : 0;
        if(destroyed > 0)
            out.push_back("Rebuilding " + std::to_string(destroyed) + " destroyed structure(s)");
        if(m_lastThreatId != 0)
        {
            out.push_back("Watching the intruder near (" + std::to_string(m_lastThreatX) + ", " + std::to_string(m_lastThreatY) + ")");
        }
        return out;
    }

    AIController::AIController()
    {
        initDifficultyDefaults();
        loadDifficultyConfig("assets/characters/difficulty.yaml");
    }

    bool AIController::isFoe(const Entity &e) const
    {
        return e.owner < kMaxPlayers && Players::hostile(e.owner, m_player);
    }

    void AIController::setPlayer(PlayerId player)
    {
        m_player = player;
        m_playerFixed = true;
    }

    void AIController::init(int enemyHqX, int enemyHqY, int playerHqX, int playerHqY)
    {
        // Single player: the computer is the slot of the other faction (a match sets its slot with setPlayer)
        if(!m_playerFixed)
            m_player = Players::ownerOf(aiFaction());
        m_enemyHqX = enemyHqX;
        m_enemyHqY = enemyHqY;
        m_playerHqX = playerHqX;
        m_playerHqY = playerHqY;
        m_initialized = true;
        reset();
    }

    void AIController::reset()
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
        m_readyUnits = 0;
        m_hasProduction = true;

        m_researchIndex = 0;
        m_researchActive = false;
        m_researchTimer = 0.0f;
        resetResearch(m_player);

        m_squadController.clear();
        m_chokeRebuildTimer = 1e6f; // rebuild on the first update
        m_garrison.clear();
        m_garrisonRecorded = false;
        m_hasLab = false;

        m_defensePlanned = false;
        m_defensePlan.clear();
        m_buildTimer = 0.0f;
        m_expandTimer = 0.0f;
        m_expansions = 0;
        m_gateX[0] = m_gateX[1] = m_gateY[0] = m_gateY[1] = -1;
        m_lastThreatX = m_lastThreatY = -1;
        m_lastThreatId = 0;
        clearThoughts();
    }

    // ---------------------------------------------------------------------------
    // Director: wave planning
    // ---------------------------------------------------------------------------

    AIWavePlan AIController::planWave(int waveNumber, bool hasFactory, bool hasFactoryPlan) const
    {
        const AITacticsConfig &t = m_currentDifficultyConfig.tactics;
        AIWavePlan plan;
        plan.waveNumber = std::max(1, waveNumber);

        int size = std::min(t.waveMaxSize, t.waveBaseSize + t.waveGrowth * (plan.waveNumber - 1));
        size = std::clamp(static_cast<int>(std::lround(size * m_debugConfig.waveSpawnMultiplier)), 1, 30);
        size = std::max(size, kMinAttackWave); // the AI never sends a handful: at least ten troops march

        int tier = (plan.waveNumber <= 2) ? 1 : (plan.waveNumber <= 4) ? 2 : (plan.waveNumber <= 6) ? 3 : 4;
        tier = std::clamp(tier, 1, std::clamp(t.maxWaveTier, 1, 4));
        plan.tier = static_cast<AIWaveTier>(tier);

        std::vector<int> core; // tanks / heavy weapons
        int support = 0;       // rocket troopers
        int melee = 0;         // flamethrowers
        switch(plan.tier)
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
                for(int i = 0; i < tanks; ++i)
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
                for(int i = 0; i < tanks; ++i)
                    core.push_back(kPattern[i % 4]);
                int artillery = std::max(1, size / 8);
                for(int i = 0; i < artillery; ++i)
                    core.push_back(kGrad);
                support = std::max(1, size / 5);
                melee = t.meleeRangedPairs ? size / 6 : 0;
                break;
            }
        }

        // Without a factory (and none to rebuild) heavy slots become heavy infantry
        if(!hasFactory && !hasFactoryPlan)
        {
            for(int arch : core)
            {
                if(arch == kGrad)
                    ++support;
                else
                    ++melee;
            }
            core.clear();
        }

        int heavyCount = static_cast<int>(core.size()) + support + melee;
        if(heavyCount > size)
            size = heavyCount;
        int riflemen = size - heavyCount;

        // Interleave so the infantry queue alternates cheap and specialist troops
        for(int i = 0; i < riflemen || support > 0 || melee > 0; ++i)
        {
            if(i < riflemen)
                plan.archetypes.push_back(kRifleman);
            if(support > 0 && i % 2 == 1)
            {
                plan.archetypes.push_back(kHeavyAssault);
                --support;
            }
            if(melee > 0 && i % 2 == 0)
            {
                plan.archetypes.push_back(kFlamethrower);
                --melee;
            }
            if(i > 64)
                break;
        }
        plan.archetypes.insert(plan.archetypes.end(), core.begin(), core.end());
        return plan;
    }

    void AIController::preparePlan(int waveNumber, bool hasFactory, bool hasFactoryPlan)
    {
        m_currentPlan = planWave(waveNumber, hasFactory, hasFactoryPlan);
        m_infantryQueue.clear();
        m_vehicleQueue.clear();
        for(int arch : m_currentPlan.archetypes)
        {
            (isVehicleArchetype(arch) ? m_vehicleQueue : m_infantryQueue).push_back(arch);
        }
        std::ostringstream msg;
        msg << "Planning wave " << m_currentPlan.waveNumber << " (" << tierName(m_currentPlan.tier) << "): " << m_infantryQueue.size() << " infantry + " << m_vehicleQueue.size() << " vehicles";
        if(!hasFactory && !hasFactoryPlan)
            msg << " (no factory: heavy slots become infantry)";
        think("WAVE", msg.str());
    }

    bool AIController::isTechnologyResearched(int index) const
    {
        return isResearched(m_player, index);
    }

    float AIController::getResearchProgress() const
    {
        if(!m_researchActive)
            return 0.0f;
        float duration = std::max(1.0f, m_currentDifficultyConfig.tactics.researchSeconds);
        return std::clamp(m_researchTimer / duration, 0.0f, 1.0f);
    }

    bool AIController::aiSees(const Entity &e, const EntityManager &entities) const
    {
        return m_debugConfig.disableFogOfWarForAi || entities.isEntityVisibleToAi(e, m_player);
    }

    bool AIController::isReserveUnit(const Unit &u) const
    {
        if(!u.alive || u.state == ActionState::Die || u.owner != m_player)
            return false;
        if(u.moveSpeed <= 0.0f || u.isSiegeMode)
            return false;
        if(m_squadController.isSquadMember(u.id))
            return false;
        if(m_garrison.count(u.id))
            return false;
        // Only troops around the home base belong to the reserve (pre-placed outposts stay where they are)
        return DetMath::hypot(static_cast<float>(u.tileX - m_enemyHqX), static_cast<float>(u.tileY - m_enemyHqY)) <= 18.0f;
    }

    // ---------------------------------------------------------------------------
    // Main update
    // ---------------------------------------------------------------------------

    void AIController::update(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
    {
        m_gameTime += deltaTime;
        m_waveAssemblyTimer += deltaTime;

        // Auto-synchronize Enemy & Player base coordinates from living entities if uninitialized or destroyed
        if(m_enemyHqX <= 0 || m_enemyHqY <= 0 || m_playerHqX <= 0 || m_playerHqY <= 0)
        {
            bool foundEnemyHq = false;
            bool foundPlayerHq = false;
            for(const auto &e : entities.getAllEntities())
            {
                if(!e.alive || e.state == ActionState::Die)
                    continue;
                if(const Building *b = e.asBuilding())
                {
                    int cx = b->tileX + b->buildWidth / 2;
                    int cy = b->tileY + b->buildHeight / 2;
                    if(b->owner == m_player)
                    {
                        if(m_enemyHqX <= 0 || m_enemyHqY <= 0 || (b->type == UnitType::Headquarters && !foundEnemyHq))
                        {
                            m_enemyHqX = cx;
                            m_enemyHqY = cy;
                            if(b->type == UnitType::Headquarters)
                                foundEnemyHq = true;
                        }
                    }
                    else if(isFoe(*b))
                    {
                        if(m_playerHqX <= 0 || m_playerHqY <= 0 || (b->type == UnitType::Headquarters && !foundPlayerHq))
                        {
                            m_playerHqX = cx;
                            m_playerHqY = cy;
                            if(b->type == UnitType::Headquarters)
                                foundPlayerHq = true;
                        }
                    }
                }
            }
            if(m_playerHqX <= 0 || m_playerHqY <= 0)
            {
                for(const auto &e : entities.getAllEntities())
                {
                    if(e.alive && e.state != ActionState::Die && isFoe(e))
                    {
                        m_playerHqX = e.tileX;
                        m_playerHqY = e.tileY;
                        break;
                    }
                }
            }
            if(m_enemyHqX <= 0 || m_enemyHqY <= 0)
            {
                for(const auto &e : entities.getAllEntities())
                {
                    if(e.alive && e.state != ActionState::Die && e.owner == m_player)
                    {
                        m_enemyHqX = e.tileX;
                        m_enemyHqY = e.tileY;
                        break;
                    }
                }
            }
        }

        if(!m_garrisonRecorded)
        {
            m_garrisonRecorded = true;
            for(const auto &e : entities.getAllEntities())
            {
                if(e.alive && !e.isBuilding && e.owner == m_player)
                    m_garrison.insert(e.id);
            }
        }

        // Chokepoints change when structures and walls appear or disappear
        m_chokeRebuildTimer += deltaTime;
        if(m_chokeRebuildTimer >= 5.0f)
        {
            m_chokeRebuildTimer = 0.0f;
            m_chokes.rebuild(map);
        }

        updateStagingPoint(map);
        m_squadController.setStagingPoint(m_stagingX, m_stagingY);

        // Rebuild starting base structures & walls if destroyed
        updateRebuilding(deltaTime, map, entities);
        if(m_skirmish && m_currentDifficultyConfig.tactics.buildDefenses)
        {
            updateBaseBuilder(deltaTime, map, entities, pathfinder);
        }

        m_economyTimer += deltaTime;
        while(m_economyTimer >= 1.0f)
        {
            m_economyTimer -= 1.0f;
            updateEconomyAndProduction(1.0f, map, entities);
            updateResearch(1.0f, entities);
        }

        m_defenseCheckTimer += deltaTime;
        if(m_defenseCheckTimer >= 2.0f)
        {
            m_defenseCheckTimer = 0.0f;
            updateBaseDefense(entities, pathfinder);
        }

        updateStaging(map, entities, pathfinder);
        updateWaveLaunch(deltaTime, map, entities, pathfinder);
        m_squadController.update(deltaTime, map, entities, pathfinder, m_chokes, m_currentDifficultyConfig.tactics);
        m_squadController.takeReleasedUnits(); // released units rejoin the reserve through updateStaging
        updateIdleUnits(entities, pathfinder);
    }

    void AIController::updateEconomyAndProduction(float /*deltaTime*/, TileMap &map, EntityManager &entities)
    {
        bool hasHq = false;
        bool hasBarracks = false;
        bool hasFactory = false;
        bool hasFactoryPlan = false;
        for(const auto &p : m_startingBuildings)
        {
            if(p.type == UnitType::Factory)
                hasFactoryPlan = true;
        }
        Building *availableBarracks = nullptr;
        Building *availableFactory = nullptr;

        int enemyUnitCount = 0;
        int reserveCount = 0;
        for(auto &e : entities.getAllEntities())
        {
            if(!e.alive || e.state == ActionState::Die || e.owner != m_player)
                continue;
            if(Building *b = e.asBuilding())
            {
                if(b->type == UnitType::Headquarters)
                {
                    hasHq = true;
                }
                else if(b->type == UnitType::Barracks)
                {
                    hasBarracks = true;
                    if(!b->isProducing && b->productionTimer <= 0.0f && !availableBarracks)
                        availableBarracks = b;
                }
                else if(b->type == UnitType::Factory)
                {
                    hasFactory = true;
                    if(!b->isProducing && b->productionTimer <= 0.0f && !availableFactory)
                        availableFactory = b;
                }
            }
            else
            {
                enemyUnitCount++;
                if(const Unit *u = e.asUnit(); u && isReserveUnit(*u))
                    reserveCount++;
            }
        }
        m_hasProduction = hasBarracks || hasFactory;

        if(m_currentPlan.archetypes.empty())
        {
            preparePlan(m_waveCount + 1, hasFactory, hasFactoryPlan);
        }

        if(!hasHq && !hasBarracks && !hasFactory)
        {
            return; // All enemy production structures destroyed
        }

        m_credits += m_creditsPerSec;

        // Units lost while assembling are replaced so the wave can still reach 100% strength
        int planned = static_cast<int>(m_currentPlan.archetypes.size());
        int queued = static_cast<int>(m_infantryQueue.size() + m_vehicleQueue.size());
        if(queued == 0 && reserveCount < planned)
        {
            for(int i = reserveCount; i < planned; ++i)
                m_infantryQueue.push_back(kRifleman);
        }

        // Unit cap per match for performance & balance
        if(enemyUnitCount >= 40)
            return;

        float speedMult = m_currentDifficultyConfig.productionSpeedMultiplier;
        if(speedMult <= 0.01f)
            speedMult = 1.0f;
        const float barracksBuildTime = 6.0f / speedMult;
        const float factoryBuildTime = 10.0f / speedMult;

        auto costOf = [&](int arch)
        {
            const UnitConfig *cfg = entities.getUnitConfig(sideArchetype(arch, side()));
            return cfg ? cfg->creditCost : fallbackArchetypeCost(arch);
        };

        // While the lab waits for its next technology, the army only spends what exceeds that budget,
        // so research keeps advancing instead of being starved by production
        int researchReserve = (m_hasLab && !m_researchActive && m_researchIndex < kResearchCount) ? researchDef(side(), m_researchIndex).cost : 0;

        auto produce = [&](Building *building, std::vector<int> &queue, float buildTime)
        {
            if(!building || queue.empty())
                return;
            int arch = queue.front();
            int cost = costOf(arch);
            if(m_credits - cost < researchReserve)
            {
                thinkOnce("economy", "ECONOMY", researchReserve > 0 ? "Holding back unit production to afford the next technology" : "Not enough credits to train the next unit");
                return;
            }
            thinkOnce("economy", "ECONOMY", "Training troops for the next wave");
            int sx = building->tileX;
            int sy = building->tileY;
            if(!entities.getBuildingSpawnPoint(building->id, map, sx, sy))
                return;
            m_credits -= cost;
            queue.erase(queue.begin());
            int id = entities.spawnUnitByArchetype(sideArchetype(arch, side()), sx, sy, side(), m_player);
            // New troops get every technology the AI has already researched
            if(Unit *u = entities.getUnit(id))
                applyCompletedResearch(*u);
            building->isProducing = true;
            building->productionTimer = buildTime;
        };

        if(hasBarracks)
            produce(availableBarracks, m_infantryQueue, barracksBuildTime);
        if(hasFactory)
            produce(availableFactory, m_vehicleQueue, factoryBuildTime);
    }

    void AIController::updateResearch(float deltaTime, EntityManager &entities)
    {
        const Faction faction = side();
        // Skip technologies that only make sense for the player
        while(m_researchIndex < kResearchCount && researchDef(faction, m_researchIndex).playerOnly)
        {
            ++m_researchIndex;
        }
        if(m_researchIndex >= kResearchCount)
            return;

        // Research needs the Tech Center / Laboratory; it pauses while there is none
        m_hasLab = hasResearchBuilding(m_player, entities);
        const ResearchDef &def = researchDef(faction, m_researchIndex);
        if(!m_hasLab)
        {
            thinkOnce("research", "RESEARCH", std::string("No Tech Center / Laboratory: research paused before ") + def.name);
            return;
        }

        if(!m_researchActive)
        {
            if(m_credits >= def.cost)
            {
                m_credits -= def.cost;
                m_researchActive = true;
                m_researchTimer = 0.0f;
                thinkOnce("research", "RESEARCH", std::string("Research started: ") + def.name + " ($" + std::to_string(def.cost) + ")");
            }
            else
            {
                thinkOnce("research", "RESEARCH", std::string("Saving credits for ") + def.name + " ($" + std::to_string(def.cost) + ")");
            }
            return;
        }

        m_researchTimer += deltaTime;
        if(m_researchTimer >= std::max(1.0f, m_currentDifficultyConfig.tactics.researchSeconds))
        {
            completeResearch(m_player, m_researchIndex, entities);
            m_researchActive = false;
            m_researchTimer = 0.0f;
            thinkOnce("research", "RESEARCH", std::string("Research completed: ") + def.name);
            ++m_researchIndex;
        }
    }

    void AIController::updateBaseDefense(EntityManager &entities, Pathfinder &pathfinder)
    {
        // Find all player units trespassing within 18 tiles of enemy base
        int threatEntityId = 0;
        float closestDist = 18.0f * TILE_WIDTH;

        for(const auto &e : entities.getAllEntities())
        {
            if(!e.alive || e.state == ActionState::Die || !isFoe(e))
                continue;
            if(!aiSees(e, entities))
                continue; // AI only detects threats it actually sees!
            float dx = e.getUnitCenterPixelX() - (m_enemyHqX * TILE_WIDTH + TILE_WIDTH * 0.5f);
            float dy = e.getUnitCenterPixelY() - (m_enemyHqY * TILE_HEIGHT + TILE_HEIGHT * 0.5f);
            float d = std::sqrt(dx * dx + dy * dy);
            if(d < closestDist)
            {
                closestDist = d;
                threatEntityId = e.id;
            }
        }

        if(threatEntityId == 0)
            return;

        Entity *threat = entities.getEntity(threatEntityId);
        if(!threat)
            return;
        m_lastThreatX = threat->tileX; // the base builder reinforces this side
        m_lastThreatY = threat->tileY;
        if(threat->id != m_lastThreatId)
        {
            m_lastThreatId = threat->id;
            think("DEFENSE", "Enemy spotted near the base at (" + std::to_string(threat->tileX) + ", " + std::to_string(threat->tileY) + "): sending idle troops to intercept");
        }

        // Rally nearby idle reserve units to intercept the threat (marching groups keep their own orders)
        std::vector<int> defenders;
        for(const auto &e : entities.getAllEntities())
        {
            if(!e.alive || e.state == ActionState::Die || e.isBuilding || e.owner != m_player)
                continue;
            if(m_squadController.isSquadMember(e.id))
                continue;
            if(e.state == ActionState::Idle && e.targetEntityId == 0)
            {
                float dx = e.getUnitCenterPixelX() - threat->getUnitCenterPixelX();
                float dy = e.getUnitCenterPixelY() - threat->getUnitCenterPixelY();
                float d = std::sqrt(dx * dx + dy * dy);
                if(d <= 22.0f * TILE_WIDTH)
                {
                    defenders.push_back(e.id);
                }
            }
        }

        if(!defenders.empty())
        {
            entities.issueAttackMoveOrder(defenders, threat->tileX, threat->tileY, pathfinder);
        }
    }

    void AIController::updateStagingPoint(const TileMap &map)
    {
        // Rally point 6 tiles from the enemy HQ toward the player
        float dxToPlayer = static_cast<float>(m_playerHqX - m_enemyHqX);
        float dyToPlayer = static_cast<float>(m_playerHqY - m_enemyHqY);
        float len = DetMath::hypot(dxToPlayer, dyToPlayer);
        int stepX = (len > 0.001f) ? static_cast<int>(std::round((dxToPlayer / len) * 6.0f)) : -6;
        int stepY = (len > 0.001f) ? static_cast<int>(std::round((dyToPlayer / len) * 6.0f)) : 0;
        m_stagingX = std::clamp(m_enemyHqX + stepX, 3, std::max(3, map.getWidth() - 4));
        m_stagingY = std::clamp(m_enemyHqY + stepY, 3, std::max(3, map.getHeight() - 4));
    }

    void AIController::updateStaging(TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
    {
        if(m_enemyHqX <= 0 && m_enemyHqY <= 0)
            return;
        pathfinder.setEntityManager(&entities);

        // Fresh troops (and units that fell back) gather at the staging area instead of attacking piecemeal
        std::vector<int> toStage;
        for(const auto &e : entities.getAllEntities())
        {
            const Unit *u = e.asUnit();
            if(!u || !isReserveUnit(*u))
                continue;
            if(u->state == ActionState::Idle && u->targetEntityId == 0 && !u->isAttackMoving && !u->waitingForPath)
            {
                if(std::max(std::abs(u->tileX - m_stagingX), std::abs(u->tileY - m_stagingY)) > 5)
                {
                    toStage.push_back(u->id);
                }
            }
        }
        if(toStage.empty())
            return;

        std::vector<StormcastSlot> slots = AINavigataionStormcastView::computeGroupSlots(m_stagingX, m_stagingY, static_cast<int>(toStage.size()), map, &entities, toStage);

        for(size_t i = 0; i < toStage.size() && i < slots.size(); ++i)
        {
            Unit *u = entities.getUnit(toStage[i]);
            if(!u)
                continue;
            int slotX = slots[i].x;
            int slotY = slots[i].y;
            if(std::abs(u->tileX - slotX) > 1 || std::abs(u->tileY - slotY) > 1)
            {
                bool isPartial = false;
                if(pathfinder.findPath(u->tileX, u->tileY, slotX, slotY, u->path, u->id, &isPartial))
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

    void AIController::findAttackObjective(const EntityManager &entities, const TileMap &map, int &outX, int &outY) const
    {
        outX = -1;
        outY = -1;
        float bestBldgDist = 1e9f;

        // Player HQ first, otherwise the closest player structure, otherwise the closest player unit
        for(const auto &e : entities.getAllEntities())
        {
            if(!e.alive || e.state == ActionState::Die || !isFoe(e) || !e.isBuilding)
                continue;
            const Building *b = e.asBuilding();
            if(!b)
                continue;
            int cx = b->tileX + b->buildWidth / 2;
            int cy = b->tileY + b->buildHeight / 2;
            if(b->type == UnitType::Headquarters)
            {
                outX = cx;
                outY = cy;
                break;
            }
            float d = DetMath::hypot(static_cast<float>(cx - m_enemyHqX), static_cast<float>(cy - m_enemyHqY));
            if(d < bestBldgDist)
            {
                bestBldgDist = d;
                outX = cx;
                outY = cy;
            }
        }

        if(outX < 0)
        {
            float bestDist = 1e9f;
            for(const auto &e : entities.getAllEntities())
            {
                if(!e.alive || e.state == ActionState::Die || !isFoe(e))
                    continue;
                float d = DetMath::hypot(static_cast<float>(e.tileX - m_enemyHqX), static_cast<float>(e.tileY - m_enemyHqY));
                if(d < bestDist)
                {
                    bestDist = d;
                    outX = e.tileX;
                    outY = e.tileY;
                }
            }
        }

        if(outX < 0 && m_playerHqX > 0 && m_playerHqY > 0)
        {
            outX = m_playerHqX;
            outY = m_playerHqY;
        }
        if(outX >= 0)
        {
            outX = std::clamp(outX, 1, map.getWidth() - 2);
            outY = std::clamp(outY, 1, map.getHeight() - 2);
        }
    }

    void AIController::updateWaveLaunch(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
    {
        m_waveCooldown -= deltaTime;

        // Troops assembled at the staging area and ready to march
        std::vector<int> ready;
        for(const auto &e : entities.getAllEntities())
        {
            const Unit *u = e.asUnit();
            if(!u || !isReserveUnit(*u))
                continue;
            if(u->targetEntityId != 0 || u->isAttackMoving)
                continue;
            if(std::max(std::abs(u->tileX - m_stagingX), std::abs(u->tileY - m_stagingY)) <= 7)
            {
                ready.push_back(u->id);
            }
        }
        m_readyUnits = static_cast<int>(ready.size());

        int required = static_cast<int>(m_currentPlan.archetypes.size());
        if(required == 0 || m_waveCooldown > 0.0f)
            return;

        // March only with 100% of the planned wave; without production left, send whatever is assembled
        bool full = static_cast<int>(ready.size()) >= required;
        // Without production left the AI sends what is assembled, but still not a handful: ten troops, or after a long wait
        bool stranded = !m_hasProduction && !ready.empty() && ((static_cast<int>(ready.size()) >= kMinAttackWave && m_waveAssemblyTimer > 20.0f) || m_waveAssemblyTimer > 90.0f);
        if(!full && !stranded)
            return;

        int targetX, targetY;
        findAttackObjective(entities, map, targetX, targetY);
        if(targetX < 0)
            return;

        if(static_cast<int>(ready.size()) > required)
            ready.resize(required);

        m_waveCount++;
        {
            std::ostringstream why;
            why << "Wave " << m_waveCount << " is " << (full ? "complete" : "all that is left") << ", treasury $" << m_credits << " -> splitting into " << attackGroupCount(static_cast<int>(ready.size()), map) << " group(s)";
            think("ATTACK", why.str());
        }

        // The wave splits into 1..5 groups: the first goes straight, the others take flank routes
        // (through the second base exit when there is one) so the player is hit from several sides
        const int groups = attackGroupCount(static_cast<int>(ready.size()), map);
        std::vector<int> byType = ready;
        std::stable_sort(byType.begin(), byType.end(), [&](int a, int b) { return entities.getUnit(a)->archetypeId < entities.getUnit(b)->archetypeId; });
        std::vector<std::vector<int>> members(static_cast<size_t>(groups));
        for(size_t i = 0; i < byType.size(); ++i)
            members[i % groups].push_back(byType[i]); // mixed groups
        for(int g = 0; g < groups; ++g)
        {
            int viaX = -1, viaY = -1;
            if(g > 0)
                flankPoint(g, targetX, targetY, map, viaX, viaY);
            int squadId = m_squadController.createSquad(members[g], targetX, targetY, m_waveCount, entities, pathfinder, m_currentDifficultyConfig.tactics, viaX, viaY);
            std::ostringstream msg;
            msg << "Wave " << m_waveCount << " (" << tierName(m_currentPlan.tier) << ", " << difficultyToString(getEffectiveDifficulty()) << ") group G" << squadId << " " << (g + 1) << "/" << groups << " with " << members[g].size() << " units toward (" << targetX << ", " << targetY << ")";
            if(viaX >= 0)
                msg << " via flank (" << viaX << ", " << viaY << ")";
            think("ATTACK", msg.str());
        }

        m_waveCooldown = m_currentDifficultyConfig.tactics.waveIntervalSeconds;
        m_waveAssemblyTimer = 0.0f;
        m_currentPlan.archetypes.clear(); // next plan is prepared on the next economy tick
        m_infantryQueue.clear();
        m_vehicleQueue.clear();
    }

    int AIController::attackGroupCount(int readyUnits, const TileMap &map) const
    {
        const AITacticsConfig &t = m_currentDifficultyConfig.tactics;
        int groups = 1 + m_credits / std::max(1, t.creditsPerExtraGroup); // richer AI fields more groups
        if(std::sqrt(static_cast<float>(map.getWidth() * map.getHeight())) >= 80.0f)
            ++groups; // room to flank
        if(m_gateX[1] >= 0)
            groups = std::max(groups, 2); // a second exit is used every time
        groups = std::min(groups, std::clamp(t.maxAttackGroups, 1, 5));
        groups = std::min(groups, readyUnits / kMinAttackGroup); // never a squad of two or three
        return std::max(1, groups);
    }

    void AIController::flankPoint(int group, int targetX, int targetY, const TileMap &map, int &outX, int &outY) const
    {
        outX = outY = -1;
        // Group 1 leaves through the second exit of the base wall when it has one
        if(group == 1 && m_gateX[1] >= 0)
        {
            outX = m_gateX[1];
            outY = m_gateY[1];
            return;
        }
        // Otherwise a point beside the straight line base -> target: left, right, wide left, wide right
        const float dx = static_cast<float>(targetX - m_enemyHqX);
        const float dy = static_cast<float>(targetY - m_enemyHqY);
        const float len = std::max(1.0f, DetMath::hypot(dx, dy));
        const float side = (group % 2 == 1) ? -1.0f : 1.0f;
        const float spread = std::clamp(len * (group >= 3 ? 0.45f : 0.3f), 6.0f, 24.0f);
        const float mx = m_enemyHqX + dx * 0.55f + (-dy / len) * spread * side;
        const float my = m_enemyHqY + dy * 0.55f + (dx / len) * spread * side;
        const int cx = std::clamp(static_cast<int>(std::lround(mx)), 1, map.getWidth() - 2);
        const int cy = std::clamp(static_cast<int>(std::lround(my)), 1, map.getHeight() - 2);
        // Nearest open tile around that point
        for(int r = 0; r <= 6; ++r)
        {
            for(int oy = -r; oy <= r; ++oy)
            {
                for(int ox = -r; ox <= r; ++ox)
                {
                    if(std::max(std::abs(ox), std::abs(oy)) != r)
                        continue;
                    const int x = cx + ox, y = cy + oy;
                    if(x < 1 || y < 1 || x >= map.getWidth() - 1 || y >= map.getHeight() - 1)
                        continue;
                    if(map.isPassable(x, y) && map.getWall(x, y) == 0)
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

        int factionBuildingIndex(AIDefenseSite::Kind kind, Faction side)
        {
            const bool conf = (side == Faction::Confederation);
            switch(kind)
            {
                case AIDefenseSite::Tower:
                    return conf ? 7 : 14;
                case AIDefenseSite::Bunker:
                    return conf ? 5 : 13;
                case AIDefenseSite::Lab:
                    return conf ? 4 : 12;
                case AIDefenseSite::PowerPlant:
                    return conf ? 1 : 9;
                case AIDefenseSite::Wall:
                    return -1;
            }
            return -1;
        }

        int defenseCost(AIDefenseSite::Kind kind)
        {
            switch(kind)
            {
                case AIDefenseSite::Tower:
                    return 250;
                case AIDefenseSite::Bunker:
                    return 350;
                case AIDefenseSite::Lab:
                    return 450;
                case AIDefenseSite::PowerPlant:
                    return 200;
                case AIDefenseSite::Wall:
                    return 25;
            }
            return 250;
        }

    } // namespace

    bool AIController::findSiteNear(int &x, int &y, int w, int h, int searchRadius, const TileMap &map, const EntityManager &entities) const
    {
        auto clearOfGates = [&](int px, int py)
        {
            for(int g = 0; g < 2; ++g)
            {
                if(m_gateX[g] < 0)
                    continue;
                // keep a 2-tile corridor around each exit free of structures
                if(px + w + 1 >= m_gateX[g] && px - 2 <= m_gateX[g] && py + h + 1 >= m_gateY[g] && py - 2 <= m_gateY[g])
                    return false;
            }
            return true;
        };
        for(int r = 0; r <= searchRadius; ++r)
        {
            for(int oy = -r; oy <= r; ++oy)
            {
                for(int ox = -r; ox <= r; ++ox)
                {
                    if(std::max(std::abs(ox), std::abs(oy)) != r)
                        continue;
                    const int px = x + ox, py = y + oy;
                    if(!clearOfGates(px, py))
                        continue;
                    if(!entities.canPlaceBuilding(px, py, w, h, map, m_player))
                        continue;
                    bool unitInside = false;
                    for(int ty = py; ty < py + h && !unitInside; ++ty)
                    {
                        for(int tx = px; tx < px + w && !unitInside; ++tx)
                            unitInside = entities.isTileOccupiedByUnit(tx, ty);
                    }
                    if(unitInside)
                        continue;
                    x = px;
                    y = py;
                    return true;
                }
            }
        }
        return false;
    }

    void AIController::planBaseDefenses(const TileMap &map, const EntityManager &entities, Pathfinder &pathfinder)
    {
        m_defensePlan.clear();
        m_defensePlanned = true;

        // Base centre: the AI headquarters; threat direction: toward the player's base
        int cx = m_enemyHqX, cy = m_enemyHqY;
        int hqRadius = 2;
        for(const auto &e : entities.getAllEntities())
        {
            const Building *b = e.asBuilding();
            if(b && b->alive && b->owner == m_player && b->type == UnitType::Headquarters)
            {
                cx = b->tileX + b->buildWidth / 2;
                cy = b->tileY + b->buildHeight / 2;
                hqRadius = std::max(b->buildWidth, b->buildHeight) / 2 + 1;
                break;
            }
        }
        float tdx = static_cast<float>(m_playerHqX - cx);
        float tdy = static_cast<float>(m_playerHqY - cy);
        const float tlen = std::max(1.0f, DetMath::hypot(tdx, tdy));
        tdx /= tlen;
        tdy /= tlen;
        const float threatAngle = DetMath::atan2(tdy, tdx);
        const float wallRadius = static_cast<float>(hqRadius + 8);
        auto ringTile = [&](float radius, float angleOffsetDeg, int &x, int &y)
        {
            const float a = threatAngle + angleOffsetDeg * static_cast<float>(M_PI) / 180.0f;
            x = std::clamp(static_cast<int>(std::lround(cx + DetMath::cos(a) * radius)), 1, map.getWidth() - 2);
            y = std::clamp(static_cast<int>(std::lround(cy + DetMath::sin(a) * radius)), 1, map.getHeight() - 2);
        };

        // Exit 1: where the road to the player crosses the wall ring; exit 2: 60 degrees to one side
        std::vector<PathNode> road;
        bool partial = false;
        pathfinder.setIgnoreUnitsOnce();
        pathfinder.findPath(cx, cy + hqRadius + 1, m_playerHqX, m_playerHqY, road, 0, &partial, true);
        for(const auto &n : road)
        {
            if(DetMath::hypot(static_cast<float>(n.x - cx), static_cast<float>(n.y - cy)) >= wallRadius - 0.5f)
            {
                m_gateX[0] = n.x;
                m_gateY[0] = n.y;
                break;
            }
        }
        if(m_gateX[0] < 0)
            ringTile(wallRadius, 0.0f, m_gateX[0], m_gateY[0]);
        const float gateAngle = DetMath::atan2(static_cast<float>(m_gateY[0] - cy), static_cast<float>(m_gateX[0] - cx));
        // The second exit goes to the side of the first one that still faces the player
        auto angleDiff = [](float a, float b) { return std::abs(std::remainder(a - b, 2.0f * static_cast<float>(M_PI))); };
        const float rad60 = 60.0f * static_cast<float>(M_PI) / 180.0f;
        const float firstSide = angleDiff(gateAngle + rad60, threatAngle) <= angleDiff(gateAngle - rad60, threatAngle) ? 1.0f : -1.0f;
        for(float side : {firstSide, -firstSide})
        {
            int gx = 0, gy = 0;
            const float a = gateAngle + side * 60.0f * static_cast<float>(M_PI) / 180.0f;
            gx = std::clamp(static_cast<int>(std::lround(cx + DetMath::cos(a) * wallRadius)), 1, map.getWidth() - 2);
            gy = std::clamp(static_cast<int>(std::lround(cy + DetMath::sin(a) * wallRadius)), 1, map.getHeight() - 2);
            if(map.isPassable(gx, gy))
            {
                m_gateX[1] = gx;
                m_gateY[1] = gy;
                break;
            }
        }

        auto add = [&](AIDefenseSite::Kind kind, int x, int y) { m_defensePlan.push_back({kind, x, y, false}); };

        // 1. First tower right behind the main exit, then a bunker covering the second one
        const float innerRadius = wallRadius - 3.0f;
        int x = 0, y = 0;
        ringTile(innerRadius, 0.0f, x, y);
        add(AIDefenseSite::Tower, x - 1, y - 1);
        ringTile(innerRadius, -40.0f, x, y);
        add(AIDefenseSite::Bunker, x - 1, y - 1);

        // 2. Wall arc facing the player (+-80 degrees), leaving both exits and building doors open
        std::vector<std::pair<int, int>> wallTiles;
        const float step = 0.45f / wallRadius;
        for(float a = -80.0f * static_cast<float>(M_PI) / 180.0f; a <= 80.0f * static_cast<float>(M_PI) / 180.0f; a += step)
        {
            const int wx = static_cast<int>(std::lround(cx + DetMath::cos(threatAngle + a) * wallRadius));
            const int wy = static_cast<int>(std::lround(cy + DetMath::sin(threatAngle + a) * wallRadius));
            if(wx < 1 || wy < 1 || wx >= map.getWidth() - 1 || wy >= map.getHeight() - 1)
                continue;
            if(std::find(wallTiles.begin(), wallTiles.end(), std::make_pair(wx, wy)) != wallTiles.end())
                continue;
            bool nearGate = false;
            for(int g = 0; g < 2; ++g)
            {
                if(m_gateX[g] >= 0 && std::max(std::abs(wx - m_gateX[g]), std::abs(wy - m_gateY[g])) <= 1)
                    nearGate = true;
            }
            if(nearGate)
                continue;
            wallTiles.emplace_back(wx, wy);
        }
        for(const auto &[wx, wy] : wallTiles)
            add(AIDefenseSite::Wall, wx, wy);

        // 3. More firepower along the inside of the wall
        ringTile(innerRadius, 40.0f, x, y);
        add(AIDefenseSite::Tower, x - 1, y - 1);

        // 4. Fragile buildings go behind the headquarters, away from the attack side
        if(!entities.hasBuildingType(UnitType::ResearchLab, m_player, false))
        {
            ringTile(static_cast<float>(hqRadius + 4), 180.0f, x, y);
            add(AIDefenseSite::Lab, x - 1, y - 1);
        }
        ringTile(static_cast<float>(hqRadius + 4), 140.0f, x, y);
        add(AIDefenseSite::PowerPlant, x - 1, y - 1);

        // 5. Flanks of the arc
        ringTile(innerRadius, -75.0f, x, y);
        add(AIDefenseSite::Bunker, x - 1, y - 1);
        ringTile(innerRadius, 75.0f, x, y);
        add(AIDefenseSite::Tower, x - 1, y - 1);

        std::ostringstream plan;
        plan << "Base plan: " << m_defensePlan.size() << " items, " << wallTiles.size() << " wall tiles facing the enemy, exits (" << m_gateX[0] << "," << m_gateY[0] << ") and (" << m_gateX[1] << "," << m_gateY[1] << ")";
        think("BUILD", plan.str());
    }

    bool AIController::buildDefenseSite(AIDefenseSite &site, TileMap &map, EntityManager &entities)
    {
        const int cost = defenseCost(site.kind);
        if(site.kind == AIDefenseSite::Wall)
        {
            const int x = site.tileX, y = site.tileY;
            if(!map.isPassable(x, y) || map.getWall(x, y) > 0 || entities.isTileOccupiedByUnit(x, y) || !entities.canPlaceBuilding(x, y, 1, 1, map, m_player))
            {
                site.done = true; // blocked for good (terrain / building): skip this segment
                return false;
            }
            m_credits -= cost;
            map.setWall(x, y, side(), 100, m_player);
            m_startingWalls.push_back({x, y, side(), cost, false, 0.0f}); // rebuilt when destroyed
            site.done = true;
            return true;
        }

        const int index = factionBuildingIndex(site.kind, side());
        const BuildingConfig *cfg = entities.getBuildingConfig(index);
        const int w = cfg ? cfg->width : 2;
        const int h = cfg ? cfg->height : 2;
        int x = site.tileX, y = site.tileY;
        if(!findSiteNear(x, y, w, h, 3, map, entities))
        {
            site.done = true; // no room around the planned spot
            return false;
        }
        const int id = entities.spawnBuildingByIndex(index, x, y, side(), 0.0f, m_player);
        if(id <= 0)
        {
            site.done = true;
            return false;
        }
        m_credits -= cost;
        for(int ty = y; ty < y + h && ty < map.getHeight(); ++ty)
        {
            for(int tx = x; tx < x + w && tx < map.getWidth(); ++tx)
                map.setPassable(tx, ty, false);
        }
        static const char *const kKindNames[] = {"tower", "bunker", "wall", "laboratory (behind the HQ)", "power plant (behind the HQ)"};
        think("BUILD", std::string("Building a ") + kKindNames[site.kind] + " at (" + std::to_string(x) + ", " + std::to_string(y) + ") for $" + std::to_string(cost));
        if(const Building *b = entities.getBuilding(id))
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

    void AIController::updateBaseBuilder(float deltaTime, TileMap &map, EntityManager &entities, Pathfinder &pathfinder)
    {
        if(!m_startingBaseRecorded)
            return;
        if(!entities.hasBuildingType(UnitType::Headquarters, m_player, false))
            return; // no HQ, no construction
        if(!m_defensePlanned)
            planBaseDefenses(map, entities, pathfinder);

        m_buildTimer += deltaTime;
        if(m_buildTimer < 3.0f)
            return;
        m_buildTimer = 0.0f;

        // Troops come first: building only spends what is left above a reserve for the next wave
        const int reserve = 300;
        int wallsThisTick = 0;
        for(auto &site : m_defensePlan)
        {
            if(site.done)
                continue;
            const int cost = defenseCost(site.kind);
            if(m_credits - cost < reserve)
            {
                thinkOnce("build", "BUILD", "Fortification waits: troops come first (keeping $" + std::to_string(reserve) + " in reserve)");
                return;
            }
            const bool built = buildDefenseSite(site, map, entities);
            if(site.kind == AIDefenseSite::Wall)
            {
                if(built && ++wallsThisTick >= 6)
                {
                    think("BUILD", "Raised 6 wall segments on the side facing the enemy");
                    return; // a few wall segments per step
                }
                continue;
            }
            if(built)
                return; // one structure per step
        }

        // Plan finished: every 45 s add a tower on the side the player keeps attacking from
        m_expandTimer += 3.0f;
        if(m_expandTimer < 45.0f || m_expansions >= 6 || m_credits < 900)
            return;
        m_expandTimer = 0.0f;
        int ax = m_lastThreatX >= 0 ? m_lastThreatX : m_playerHqX;
        int ay = m_lastThreatY >= 0 ? m_lastThreatY : m_playerHqY;
        const float dx = static_cast<float>(ax - m_enemyHqX);
        const float dy = static_cast<float>(ay - m_enemyHqY);
        const float len = std::max(1.0f, DetMath::hypot(dx, dy));
        const float radius = std::min(len - 2.0f, 9.0f + 2.0f * static_cast<float>(m_expansions % 3));
        const float sideways = (m_expansions % 2 == 0 ? 1.0f : -1.0f) * 2.0f * static_cast<float>(m_expansions / 2);
        AIDefenseSite extra;
        extra.kind = (m_expansions % 3 == 2) ? AIDefenseSite::Bunker : AIDefenseSite::Tower;
        extra.tileX = static_cast<int>(std::lround(m_enemyHqX + dx / len * radius - dy / len * sideways)) - 1;
        extra.tileY = static_cast<int>(std::lround(m_enemyHqY + dy / len * radius + dx / len * sideways)) - 1;
        if(buildDefenseSite(extra, map, entities))
        {
            ++m_expansions;
            m_defensePlan.push_back(extra);
            think("BUILD", "Attacks keep coming from (" + std::to_string(ax) + ", " + std::to_string(ay) + "): extra " + (extra.kind == AIDefenseSite::Bunker ? "bunker" : "tower") + " on that side");
        }
    }

    void AIController::updateIdleUnits(EntityManager &entities, Pathfinder &pathfinder)
    {
        for(auto &e : entities.getAllEntities())
        {
            Unit *u = e.asUnit();
            if(!u || !u->alive || u->state == ActionState::Die || u->owner != m_player)
                continue;
            if(u->state == ActionState::Idle && u->targetEntityId == 0 && !u->waitingForPath && !u->isAttackMoving)
            {
                float closestDist = u->sightRadius * TILE_WIDTH;
                int targetId = 0;

                for(const auto &other : entities.getAllEntities())
                {
                    if(!other.alive || other.state == ActionState::Die || !isFoe(other))
                        continue;
                    if(!aiSees(other, entities))
                        continue; // AI only engages units visible to Confederation!
                    float dx = other.getUnitCenterPixelX() - u->getUnitCenterPixelX();
                    float dy = other.getUnitCenterPixelY() - u->getUnitCenterPixelY();
                    float d = DetMath::hypot(dx, dy);
                    if(d < closestDist)
                    {
                        closestDist = d;
                        targetId = other.id;
                    }
                }

                if(targetId != 0)
                {
                    entities.issueAttackOrder({u->id}, targetId, &pathfinder);
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Debug visualization (--debug mode)
    // ---------------------------------------------------------------------------

    void AIController::renderDebug(Renderer &renderer, BitmapFontRenderer &font, const Camera &camera, const EntityManager &entities) const
    {
        if(m_debugConfig.visualizeChokepoints && m_chokes.width() > 0)
        {
            float x0, y0, x1, y1;
            camera.screenToWorld(0.0f, 0.0f, x0, y0);
            camera.screenToWorld(static_cast<float>(camera.getViewportWidth()), static_cast<float>(camera.getViewportHeight()), x1, y1);
            int tx0 = std::max(0, static_cast<int>(x0 / TILE_WIDTH));
            int ty0 = std::max(0, static_cast<int>(y0 / TILE_HEIGHT));
            int tx1 = std::min(m_chokes.width() - 1, static_cast<int>(x1 / TILE_WIDTH) + 1);
            int ty1 = std::min(m_chokes.height() - 1, static_cast<int>(y1 / TILE_HEIGHT) + 1);
            for(int ty = ty0; ty <= ty1; ++ty)
            {
                for(int tx = tx0; tx <= tx1; ++tx)
                {
                    if(!m_chokes.isChoke(tx, ty))
                        continue;
                    renderer.fillRectWorld(tx * TILE_WIDTH + 1.0f, ty * TILE_HEIGHT + 1.0f, TILE_WIDTH - 2.0f, TILE_HEIGHT - 2.0f, camera, {255, 140, 30, 70});
                }
            }
        }

        if(m_debugConfig.visualizeSquadLeash)
        {
            // Staging area marker
            RenderUtility::DrawTileRadius(renderer, camera, m_stagingX * TILE_WIDTH + TILE_WIDTH * 0.5f, m_stagingY * TILE_HEIGHT + TILE_HEIGHT * 0.5f, 7.0f, SDL_Color {120, 200, 255, 90}, 1.5f);
            m_squadController.renderDebug(renderer, font, camera, entities, m_currentDifficultyConfig.tactics.leashTiles);
        }

        // Director summary in the top-left corner of the map view
        std::string line1 = "AI " + difficultyToString(getEffectiveDifficulty()) + (m_debugConfig.forceDebugOverride ? " (debug override)" : "") + "  |  credits " + std::to_string(m_credits);
        std::string line2 = "Wave " + std::to_string(m_currentPlan.waveNumber) + " " + tierName(m_currentPlan.tier) + ": ready " + std::to_string(m_readyUnits) + "/" + std::to_string(m_currentPlan.archetypes.size()) +
            (m_waveCooldown > 0.0f ? "  next in " + std::to_string(static_cast<int>(m_waveCooldown)) + "s" : "") + "  |  groups " + std::to_string(m_squadController.getSquads().size());
        std::string line3 = "Research: ";
        if(m_researchIndex >= kResearchCount)
        {
            line3 += "complete";
        }
        else
        {
            line3 += std::string(researchDef(side(), m_researchIndex).name) + (m_researchActive ? " " + std::to_string(static_cast<int>(getResearchProgress() * 100.0f)) + "%" : (m_hasLab ? " (saving credits)" : " (no lab)"));
        }
        int y = 40;
        for(const std::string *l : {&line1, &line2, &line3})
        {
            int w = font.getTextWidth(*l, 1) + 12;
            renderer.fillRectScreen(6, y, w, 18, {10, 12, 16, 200});
            font.drawString(*l, 12, y + 2, 1, {226, 231, 238, 255}, false);
            y += 20;
        }
    }

    void AIController::onEntityAttacked(int victimId, int attackerId, EntityManager &entities, Pathfinder &pathfinder)
    {
        Entity *victim = entities.getEntity(victimId);
        Entity *attacker = entities.getEntity(attackerId);
        if(!victim || !attacker || victim->owner != m_player)
            return;

        if(victim->targetEntityId == 0)
        {
            victim->targetEntityId = attackerId;
        }

        // Call for help: alert nearby allies within 14 tiles
        std::vector<int> allies;
        for(auto &ally : entities.getAllEntities())
        {
            if(!ally.alive || ally.state == ActionState::Die || ally.isBuilding || ally.owner != m_player)
                continue;
            if(ally.state == ActionState::Idle && ally.targetEntityId == 0)
            {
                float dx = ally.getUnitCenterPixelX() - victim->getUnitCenterPixelX();
                float dy = ally.getUnitCenterPixelY() - victim->getUnitCenterPixelY();
                if(std::sqrt(dx * dx + dy * dy) <= 14.0f * TILE_WIDTH)
                {
                    allies.push_back(ally.id);
                }
            }
        }
        if(!allies.empty())
        {
            entities.issueAttackMoveOrder(allies, attacker->tileX, attacker->tileY, pathfinder);
        }
    }

    void AIController::onStructureAttacked(int structureId, int attackerId, EntityManager &entities, Pathfinder &pathfinder)
    {
        onEntityAttacked(structureId, attackerId, entities, pathfinder);
    }

    void AIController::recordStartingBase(const EntityManager &entities, const TileMap &map)
    {
        m_startingBuildings.clear();
        m_startingWalls.clear();

        for(const auto &e : entities.getAllEntities())
        {
            const Building *b = e.asBuilding();
            if(!b || !b->alive || b->owner != m_player)
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

            switch(b->type)
            {
                case UnitType::Headquarters:
                    plan.creditCost = 1000;
                    break;
                case UnitType::PowerPlant:
                    plan.creditCost = 200;
                    break;
                case UnitType::Barracks:
                    plan.creditCost = 350;
                    break;
                case UnitType::Factory:
                    plan.creditCost = 600;
                    break;
                case UnitType::ResearchLab:
                    plan.creditCost = 450;
                    break;
                default:
                    plan.creditCost = (b->rawBuildingType == 6) ? 400 : 250;
                    break; // radar / turrets & bunkers
            }

            m_startingBuildings.push_back(plan);
        }

        // Record all walls of the AI side
        for(int y = 0; y < map.getHeight(); ++y)
        {
            for(int x = 0; x < map.getWidth(); ++x)
            {
                if(map.getWall(x, y) > 0 && map.getWallOwner(x, y) == m_player)
                {
                    AIWallPlan wp;
                    wp.tileX = x;
                    wp.tileY = y;
                    wp.faction = side();
                    wp.creditCost = 25;
                    wp.destroyed = false;
                    wp.rebuildTimer = 0.0f;
                    m_startingWalls.push_back(wp);
                }
            }
        }

        m_startingBaseRecorded = true;
        std::cout << "[AIController] Recorded starting base: " << m_startingBuildings.size() << " structures, " << m_startingWalls.size() << " walls for rebuilding memory." << std::endl;
    }

    void AIController::updateRebuilding(float deltaTime, TileMap &map, EntityManager &entities)
    {
        if(!m_startingBaseRecorded)
        {
            recordStartingBase(entities, map);
            return;
        }

        // 1. Check building status & rebuild destroyed ones in the exact same location
        for(auto &plan : m_startingBuildings)
        {
            if(!plan.destroyed)
            {
                bool stillAlive = false;
                for(const auto &e : entities.getAllEntities())
                {
                    if(e.alive && e.state != ActionState::Die && e.isBuilding && e.owner == m_player && e.tileX == plan.tileX && e.tileY == plan.tileY)
                    {
                        stillAlive = true;
                        break;
                    }
                }
                if(!stillAlive)
                {
                    plan.destroyed = true;
                    plan.rebuildTimer = 0.0f;
                    think("BUILD", "Structure at (" + std::to_string(plan.tileX) + ", " + std::to_string(plan.tileY) + ") destroyed: queued for reconstruction");
                }
            }
            else
            {
                plan.rebuildTimer += deltaTime;
                if(plan.rebuildTimer >= 6.0f && m_credits >= plan.creditCost)
                {
                    bool siteClear = entities.canPlaceBuilding(plan.tileX, plan.tileY, plan.width, plan.height, map, m_player);
                    if(siteClear)
                    {
                        m_credits -= plan.creditCost;
                        int newId = entities.spawnBuildingByIndex(plan.rawBuildingType, plan.tileX, plan.tileY, side(), 0.0f, m_player);
                        if(newId > 0)
                        {
                            for(int ty = plan.tileY; ty < plan.tileY + plan.height && ty < map.getHeight(); ++ty)
                            {
                                for(int tx = plan.tileX; tx < plan.tileX + plan.width && tx < map.getWidth(); ++tx)
                                {
                                    map.setPassable(tx, ty, false);
                                }
                            }
                            plan.destroyed = false;
                            plan.rebuildTimer = 0.0f;
                            think("BUILD", "Rebuilt the structure at (" + std::to_string(plan.tileX) + ", " + std::to_string(plan.tileY) + ") for $" + std::to_string(plan.creditCost));
                        }
                    }
                }
            }
        }

        // 2. Check wall status & rebuild destroyed walls in the exact same location
        for(auto &wp : m_startingWalls)
        {
            if(!wp.destroyed)
            {
                if(map.getWall(wp.tileX, wp.tileY) == 0)
                {
                    wp.destroyed = true;
                    wp.rebuildTimer = 0.0f;
                }
            }
            else
            {
                wp.rebuildTimer += deltaTime;
                if(wp.rebuildTimer >= 8.0f && m_credits >= wp.creditCost)
                {
                    int occupantId = entities.getUnitAt(wp.tileX * TILE_WIDTH + 12, wp.tileY * TILE_HEIGHT + 12);
                    if(occupantId <= 0)
                    {
                        m_credits -= wp.creditCost;
                        map.setWall(wp.tileX, wp.tileY, wp.faction, 100, m_player);
                        wp.destroyed = false;
                        wp.rebuildTimer = 0.0f;
                        thinkOnce("wall", "BUILD", "Patching the wall at (" + std::to_string(wp.tileX) + ", " + std::to_string(wp.tileY) + ")");
                    }
                }
            }
        }
    }

} // namespace OpenArtOfWar
