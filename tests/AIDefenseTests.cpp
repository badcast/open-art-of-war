// Computer player, defense of the base: attackers known by the damage they do, who goes for them, waves that wait,
// groups on the march that turn back, the limit of a chase
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "game/AIController.hpp"
#include "game/BuildLimits.hpp"
#include "game/EntityManager.hpp"
#include "game/Pathfinder.hpp"
#include "game/TileMap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace OpenArtOfWar
{
struct AIDefenseTest
{
    static void Defend(AIController &ai, EntityManager &entities, Pathfinder &pf)
    {
        ai.UpdateBaseDefense(entities, pf);
    }
    static void Launch(AIController &ai, TileMap &map, EntityManager &entities, Pathfinder &pf)
    {
        ai.UpdateWaveLaunch(0.05f, map, entities, pf);
    }
    static bool IsDefender(const AIController &ai, int id)
    {
        return ai.m_defenders.count(id) != 0;
    }
    static std::pair<int, int> GoalOf(const AIController &ai, int id)
    {
        const auto it = ai.m_defenders.find(id);
        return it == ai.m_defenders.end() ? std::make_pair(-1, -1) : it->second;
    }
    // The troops are the computer's own reserve (not the mission's garrison), the wave plan asks for `planned`
    static void Prepare(AIController &ai, const std::vector<int> &reserve, int planned, int stagingX, int stagingY)
    {
        ai.m_garrisonRecorded = true;
        ai.m_garrison.clear();
        ai.m_rosterSeeded = true;
        ai.m_waveRoster.clear();
        ai.m_waveRoster.insert(reserve.begin(), reserve.end());
        ai.m_currentPlan.archetypes.assign(static_cast<std::size_t>(planned), 1);
        ai.m_waveCooldown = 0.0f;
        ai.m_firstRaidDone = true;
        ai.m_stagingX = stagingX;
        ai.m_stagingY = stagingY;
        ai.m_baseAttackedTimer = 0.0f;
        ai.m_baseAttackedSeconds = 0.0f;
        ai.m_defenders.clear();
    }
    static void SetAttacked(AIController &ai, float timer, float seconds)
    {
        ai.m_baseAttackedTimer = timer;
        ai.m_baseAttackedSeconds = seconds;
    }
    static float AttackedTimer(const AIController &ai)
    {
        return ai.m_baseAttackedTimer;
    }
    static int Squads(const AIController &ai)
    {
        return static_cast<int>(ai.m_squadController.GetSquads().size());
    }
    static AISquad *FirstSquad(AIController &ai)
    {
        const auto &squads = ai.m_squadController.GetSquads();
        return squads.empty() ? nullptr : ai.m_squadController.FindSquad(squads.front().id);
    }
    // The fortification plan, made anew
    static const std::vector<AIDefenseSite> &Plan(
        AIController &ai, TileMap &map, EntityManager &entities, Pathfinder &pf)
    {
        ai.m_territoryReady = false;
        ai.PlanBaseDefenses(map, entities, pf);
        return ai.m_defensePlan;
    }
    static void PrepareVehicleProduction(AIController &ai, int credits)
    {
        ai.m_factoryReady = true;
        ai.m_currentPlan.archetypes = {1, 7};
        ai.m_infantryQueue = {1};
        ai.m_vehicleQueue = {7};
        ai.m_vehicleHoldSeconds = 30.0f;
        ai.m_credits = credits;
        ai.m_creditsPerSec = 0;
        ai.m_hasLab = false;
    }
    static void FinishProductionQueues(AIController &ai)
    {
        ai.m_infantryQueue.clear();
        ai.m_vehicleQueue.clear();
        ai.m_hasLab = true;
        ai.m_researchActive = false;
        ai.m_researchIndex = 0;
    }
    static void Produce(AIController &ai, TileMap &map, EntityManager &entities)
    {
        ai.UpdateEconomyAndProduction(1.0f, map, entities);
    }
    static void SetCredits(AIController &ai, int credits)
    {
        ai.m_credits = credits;
    }
    static void SetLineOffset(AIController &ai, int tiles)
    {
        ai.m_style.lineOffset = tiles;
    }
    static int Dispatched(const AIController &ai)
    {
        return ai.m_dispatchedInWave;
    }
    static bool InRoster(const AIController &ai, int id)
    {
        return ai.m_waveRoster.count(id) != 0;
    }
    static void UpdateRoster(AIController &ai, EntityManager &entities)
    {
        ai.UpdateWaveRoster(entities);
    }
    static void SendToRepair(AIController &ai, int id)
    {
        ai.m_waveRoster.erase(id);
        ai.m_repairing.insert(id);
    }
    static bool Repairing(const AIController &ai, int id)
    {
        return ai.m_repairing.count(id) != 0;
    }
    static void Retreat(AIController &ai, EntityManager &entities, Pathfinder &pf)
    {
        ai.UpdateValuableRetreat(entities, pf);
    }
    static void SetPlan(AIController &ai, std::vector<int> archetypes)
    {
        ai.m_currentPlan.archetypes = std::move(archetypes);
    }
    static void SetGroupSize(AIController &ai, int size)
    {
        ai.m_currentDifficultyConfig.tactics.dispatchGroupSize = size;
    }
    static void Strand(AIController &ai, float assemblySeconds)
    {
        ai.m_hasProduction = false;
        ai.m_waveAssemblyTimer = assemblySeconds;
    }
    static void Replan(AIController &ai)
    {
        ai.PreparePlan(1, false, false);
    }
    static int HomeGuard(AIController &ai, int planned, int lastPlan)
    {
        ai.m_currentPlan.archetypes.assign(static_cast<std::size_t>(planned), 1);
        ai.m_lastPlanSize = lastPlan;
        return ai.HomeGuardCount();
    }
};
} // namespace OpenArtOfWar
using namespace OpenArtOfWar;

namespace
{
int failures = 0;
void Check(bool ok, const char *name)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", name);
        ++failures;
    }
}

constexpr int kHqX = 12, kHqY = 12;       // the computer's headquarters (Confederation, slot 1)
constexpr int kFoeHqX = 70, kFoeHqY = 50; // the player's
constexpr Faction kOwn = Faction::Confederation, kFoe = Faction::Resistance;

struct World
{
    TileMap map;
    EntityManager entities;
    Pathfinder pf;
    AIController ai;
    int hq {0};

    explicit World(AIDifficulty difficulty = AIDifficulty::Medium)
    {
        Players::SetupSinglePlayer(kFoe);
        map.InitBlankArena(90, 70);
        map.SetFogEnabled(false);
        entities.LoadCharacterConfigs();
        pf.SetMap(&map);
        pf.SetEntityManager(&entities);
        ai.SetDifficulty(difficulty);
        ai.Init(kHqX, kHqY, kFoeHqX, kFoeHqY);
        hq = entities.SpawnBuildingByIndex(0, kHqX - 1, kHqY - 1, kOwn, 1.0f, ai.Player());
        entities.SpawnBuildingByIndex(8, kFoeHqX, kFoeHqY, kFoe, 1.0f);
    }
    int Own(int x, int y, int archetype = 1)
    {
        return entities.SpawnUnitByArchetype(archetype, x, y, kOwn, ai.Player());
    }
    int Foe(int x, int y, int archetype = 0)
    {
        return entities.SpawnUnitByArchetype(archetype, x, y, kFoe);
    }
    // `attacker` just hit the headquarters
    void HitHq(int attacker)
    {
        Building *b = entities.GetBuilding(hq);
        b->lastAttackerId = attacker;
        b->lastAttackedTimer = 6.0f;
    }
};
} // namespace

int main()
{
    if (!Assets::Init() || !LoaderResources::Init())
    {
        std::printf("FAIL: no assets\n");
        return 1;
    }

    // 1. An enemy that shells the base from out of sight and from beyond the base zone is an attack all the same
    {
        World w;
        const int a = w.Own(14, 15), b = w.Own(15, 15);
        const int walker = w.Own(16, 14);
        AIDefenseTest::Prepare(w.ai, {a, b, walker}, 2, kHqX + 3, kHqY + 3);
        const int foe = w.Foe(kHqX, kHqY + 30);
        Check(foe > 0 && a > 0 && w.hq > 0, "units and buildings spawn");
        AIDefenseTest::Defend(w.ai, w.entities, w.pf);
        Check(!AIDefenseTest::IsDefender(w.ai, a), "an enemy out of sight and not shooting is no attack");
        Check(AIDefenseTest::AttackedTimer(w.ai) <= 0.0f, "... and the base is not under attack");

        // (a reserve unit on a plain move, as UpdateStaging orders it)
        w.entities.IssueMoveOrder({walker}, kHqX + 5, kHqY - 4, w.pf, false);
        w.HitHq(foe);
        AIDefenseTest::Defend(w.ai, w.entities, w.pf);
        const Unit *ua = w.entities.GetUnit(a);
        Check(
            AIDefenseTest::IsDefender(w.ai, a) && AIDefenseTest::IsDefender(w.ai, b), "idle troops go for the shooter");
        Check(ua && ua->isAttackMoving && ua->state == ActionState::Move, "... by attack-move");
        Check(AIDefenseTest::GoalOf(w.ai, a) == std::make_pair(kHqX, kHqY + 30), "... to where it stands");
        const Unit *uw = w.entities.GetUnit(walker);
        Check(AIDefenseTest::IsDefender(w.ai, walker) && uw && uw->isAttackMoving, "a unit on a plain move goes too");
        Check(AIDefenseTest::AttackedTimer(w.ai) > 0.0f, "the base counts as attacked");

        // On its way and the attack where it was: the order is not given again (a new order resets blockedTimer)
        w.entities.GetUnit(a)->blockedTimer = 5.0f;
        AIDefenseTest::Defend(w.ai, w.entities, w.pf);
        Check(ua->blockedTimer == 5.0f, "a defender on its way keeps its order");

        // A structure remembers its attacker for a few seconds only
        for (int tick = 0; tick < 140; ++tick)
            w.entities.Update(0.05f, w.map, w.pf);
        const Building *hq = w.entities.GetBuilding(w.hq);
        Check(hq->lastAttackedTimer <= 0.0f && hq->lastAttackerId == 0, "a structure forgets its attacker");
        // (the first shooter is dead by now: the defenders reached it)
        Check(w.entities.GetUnit(foe) == nullptr || !w.entities.GetUnit(foe)->alive, "the defenders kill the shooter");
        AIDefenseTest::Defend(w.ai, w.entities, w.pf);
        Check(!AIDefenseTest::IsDefender(w.ai, a), "... and are released after it");
        const int fresh = w.Own(15, 16);
        w.HitHq(w.Foe(kHqX, kHqY + 28));
        AIDefenseTest::Defend(w.ai, w.entities, w.pf);
        Check(AIDefenseTest::IsDefender(w.ai, fresh), "... and a new hit is a new attack");

        // A shooter far away from the base (a stale id) is nobody to go for
        {
            World other;
            const int c = other.Own(14, 15);
            AIDefenseTest::Prepare(other.ai, {c}, 2, kHqX + 3, kHqY + 3);
            other.HitHq(other.Foe(80, 65));
            AIDefenseTest::Defend(other.ai, other.entities, other.pf);
            Check(!AIDefenseTest::IsDefender(other.ai, c), "a shooter across the map is no attack on the base");
        }

        // The shooter is gone: the defenders are released
        w.entities.GetBuilding(w.hq)->lastAttackedTimer = 0.0f;
        AIDefenseTest::Defend(w.ai, w.entities, w.pf);
        Check(!AIDefenseTest::IsDefender(w.ai, a), "defenders are released when the base is quiet");
    }

    // 2. No wave leaves while the base is attacked - but not for ever
    {
        World w;
        std::vector<int> troops;
        for (int i = 0; i < 4; ++i)
            troops.push_back(w.Own(kHqX + 3 + i, kHqY + 4));
        AIDefenseTest::Prepare(w.ai, troops, 2, kHqX + 4, kHqY + 4);
        AIDefenseTest::SetAttacked(w.ai, 5.0f, 2.0f);
        AIDefenseTest::Launch(w.ai, w.map, w.entities, w.pf);
        Check(w.ai.GetWaveCount() == 0 && AIDefenseTest::Squads(w.ai) == 0, "no wave leaves a base under attack");
        AIDefenseTest::SetAttacked(w.ai, 5.0f, 61.0f);
        AIDefenseTest::Launch(w.ai, w.map, w.entities, w.pf);
        Check(w.ai.GetWaveCount() == 1, "... but it does not wait for ever");
    }
    {
        World w;
        std::vector<int> troops;
        for (int i = 0; i < 4; ++i)
            troops.push_back(w.Own(kHqX + 3 + i, kHqY + 4));
        AIDefenseTest::Prepare(w.ai, troops, 2, kHqX + 4, kHqY + 4);
        AIDefenseTest::Launch(w.ai, w.map, w.entities, w.pf);
        Check(w.ai.GetWaveCount() == 1 && AIDefenseTest::Squads(w.ai) >= 1, "a wave leaves a quiet base");
        Check(AIDefenseTest::HomeGuard(w.ai, 0, 10) > 0, "the home guard is not zero between two wave plans");
        Check(AIDefenseTest::HomeGuard(w.ai, 20, 10) > AIDefenseTest::HomeGuard(w.ai, 0, 10), "the plan counts first");
    }

    // Hard/Hardcore do not wait for the whole wave, but a group leaves the staging area only once
    // dispatch_group_size troops (beyond the home guard) are assembled there: a troop fresh from the barracks never
    // marches off alone. Easy/Medium wait for the whole wave
    for (auto level : {AIDifficulty::Easy, AIDifficulty::Medium, AIDifficulty::Hard, AIDifficulty::Hardcore})
    {
        World w(level);
        const int sx = kHqX + 4, sy = kHqY + 4;
        AIDefenseTest::Prepare(w.ai, {}, 10, sx, sy);
        AIDefenseTest::SetGroupSize(w.ai, 3);
        const int guard = AIDefenseTest::HomeGuard(w.ai, 10, 10);
        std::vector<int> troops;
        const int fresh = w.Own(60, 50); // just out of a barracks far away
        troops.push_back(fresh);
        for (int i = 0; i < guard + 2; ++i)
            troops.push_back(w.Own(sx - 2 + i % 5, sy + 1 + i / 5));
        AIDefenseTest::Prepare(w.ai, troops, 10, sx, sy);
        AIDefenseTest::SetGroupSize(w.ai, 3);
        AIDefenseTest::Launch(w.ai, w.map, w.entities, w.pf);
        Check(AIDefenseTest::Squads(w.ai) == 0, "no group leaves before it is assembled at the staging area");
        const int last = w.Own(sx + 3, sy + 5);
        troops.push_back(last);
        AIDefenseTest::Prepare(w.ai, troops, 10, sx, sy);
        AIDefenseTest::SetGroupSize(w.ai, 3);
        AIDefenseTest::Launch(w.ai, w.map, w.entities, w.pf);
        const bool immediate = level == AIDifficulty::Hard || level == AIDifficulty::Hardcore;
        Check(
            AIDefenseTest::Squads(w.ai) == (immediate ? 1 : 0),
            "Hard/Hardcore send an assembled group without the whole wave");
        if (!immediate)
            continue;
        const AISquad *sq = AIDefenseTest::FirstSquad(w.ai);
        Check(sq && sq->unitIds.size() == 3, "the group has dispatch_group_size troops");
        Check(
            sq && std::find(sq->unitIds.begin(), sq->unitIds.end(), fresh) == sq->unitIds.end(),
            "a troop not at the staging area does not leave with it");
        Check(AIDefenseTest::Dispatched(w.ai) == 3, "dispatched troops are counted");
        AIDefenseTest::Launch(w.ai, w.map, w.entities, w.pf);
        Check(AIDefenseTest::Squads(w.ai) == 1, "the home guard and the rest wait for the next group");
        Check(w.ai.GetWaveCount() == 0, "small groups do not advance the composition once per dispatch");
        AIDefenseTest::Replan(w.ai);
        Check(AIDefenseTest::Dispatched(w.ai) == 0, "a new plan restarts the dispatch count");
    }

    // Hard: no group leaves while the base is attacked; a lone troop never leaves while production goes on,
    // without production the assembled troops go after a while
    {
        World w(AIDifficulty::Hard);
        std::vector<int> troops;
        for (int i = 0; i < 12; ++i)
            troops.push_back(w.Own(kHqX + 2 + i % 6, kHqY + 5 + i / 6));
        AIDefenseTest::Prepare(w.ai, troops, 2, kHqX + 4, kHqY + 4);
        AIDefenseTest::SetAttacked(w.ai, 5.0f, 0.0f);
        AIDefenseTest::Launch(w.ai, w.map, w.entities, w.pf);
        Check(AIDefenseTest::Squads(w.ai) == 0, "no front group leaves a base under attack");
        AIDefenseTest::SetAttacked(w.ai, 0.0f, 0.0f);
        AIDefenseTest::Launch(w.ai, w.map, w.entities, w.pf);
        Check(AIDefenseTest::Squads(w.ai) == 1, "... and leaves once the base is quiet");
    }
    {
        World w(AIDifficulty::Hard);
        const int a = w.Own(kHqX + 4, kHqY + 4);
        AIDefenseTest::Prepare(w.ai, {a}, 0, kHqX + 4, kHqY + 4);
        for (int s = 0; s < 3; ++s)
            AIDefenseTest::Launch(w.ai, w.map, w.entities, w.pf);
        Check(AIDefenseTest::Squads(w.ai) == 0, "a lone troop does not march off on its own");
        AIDefenseTest::Strand(w.ai, 25.0f);
        AIDefenseTest::Launch(w.ai, w.map, w.entities, w.pf);
        Check(AIDefenseTest::Squads(w.ai) == 1, "without production what is assembled goes after a while");
    }

    // The entity limit counts the living: dead ones stay in the list, and used to fill it for good (no spawn for
    // anybody, the computer's production included, in a long match)
    {
        World w(AIDifficulty::Medium);
        std::vector<int> ids;
        for (int i = 0; i < MAX_UNITS + 5; ++i)
        {
            const int id = w.Own(5 + i % 60, 5 + (i / 60) % 50);
            if (id == 0)
                break;
            ids.push_back(id);
        }
        Check(w.Own(3, 3) == 0, "no spawn while the limit of living entities is reached");
        for (int id : ids)
            w.entities.GetUnit(id)->alive = false;
        Check(w.Own(3, 3) != 0, "dead entities do not use up the limit");
    }

    // 3. A group on the march turns back when the base lacks defenders; not on Easy, not a group that storms
    for (int variant = 0; variant < 3; ++variant)
    {
        World w(variant == 1 ? AIDifficulty::Easy : AIDifficulty::Medium);
        std::vector<int> troops;
        for (int i = 0; i < 3; ++i)
            troops.push_back(w.Own(kHqX + 3 + i, kHqY + 4));
        AIDefenseTest::Prepare(w.ai, troops, 2, kHqX + 4, kHqY + 4);
        AIDefenseTest::Launch(w.ai, w.map, w.entities, w.pf);
        AISquad *squad = AIDefenseTest::FirstSquad(w.ai);
        Check(squad != nullptr, "the wave is a squad");
        if (!squad)
            continue;
        const std::vector<int> members = squad->unitIds;
        if (variant == 2)
            squad->state = AISquadState::Assault;
        // The squad is on its way, nearer to home than to the enemy; six enemies stand at the headquarters
        for (int id : members)
            if (Unit *u = w.entities.GetUnit(id))
            {
                u->tileX = 25;
                u->tileY = 20 + (id % 3);
            }
        for (int i = 0; i < 6; ++i)
            w.Foe(kHqX - 3 + i, kHqY + 4);
        AIDefenseTest::Defend(w.ai, w.entities, w.pf);
        const bool recalled = !members.empty() && AIDefenseTest::IsDefender(w.ai, members.front())
                              && !w.ai.GetSquadController().IsSquadMember(members.front());
        if (variant == 0)
            Check(recalled, "a group on the march turns back for the base");
        else if (variant == 1)
            Check(!recalled, "Easy: the groups march on");
        else
            Check(!recalled, "a group that storms the enemy base goes on");
    }

    // 4. A chase ends a little outside the base
    {
        World w;
        const int near = w.Own(14, 15), far = w.Own(15, 15), self = w.Own(16, 15);
        AIDefenseTest::Prepare(w.ai, {near, far, self}, 2, kHqX + 3, kHqY + 3);
        const int foeNear = w.Foe(kHqX + 8, kHqY + 8), foeFar = w.Foe(60, 60);
        w.entities.IssueAttackOrder({near}, foeNear, &w.pf);
        w.entities.IssueAttackOrder({far}, foeFar, &w.pf);
        w.entities.GetUnit(self)->targetEntityId = foeFar; // (a target it took by itself: not the computer's order)
        Check(w.entities.GetUnit(far)->targetEntityId == foeFar, "the attack order is taken");
        AIDefenseTest::Defend(w.ai, w.entities, w.pf);
        Check(w.entities.GetUnit(self)->targetEntityId == foeFar, "a unit's own target is left to its stance");
        const Unit *uf = w.entities.GetUnit(far);
        Check(uf->orderedTargetId != foeFar && uf->targetEntityId != foeFar, "a chase far from the base is called off");
        Check(w.entities.GetUnit(near)->targetEntityId == foeNear, "an enemy at the base is still fought");
    }

    // 5. The same game twice gives the same world
    {
        auto run = [] {
            World w;
            std::vector<int> troops;
            for (int i = 0; i < 6; ++i)
                troops.push_back(w.Own(kHqX + 3 + i % 3, kHqY + 4 + i / 3));
            AIDefenseTest::Prepare(w.ai, troops, 4, kHqX + 4, kHqY + 4);
            for (int i = 0; i < 4; ++i)
                w.Foe(kHqX + 9 + i, kHqY + 9);
            std::uint64_t sum = 1469598103934665603ull;
            for (int tick = 0; tick < 400; ++tick)
            {
                w.entities.Update(0.05f, w.map, w.pf);
                w.ai.Update(0.05f, w.map, w.entities, w.pf);
            }
            for (const auto &e : w.entities.GetAllEntities())
                for (std::uint64_t v :
                     {static_cast<std::uint64_t>(e.id),
                      static_cast<std::uint64_t>(e.tileX),
                      static_cast<std::uint64_t>(e.tileY),
                      static_cast<std::uint64_t>(e.currentHp),
                      static_cast<std::uint64_t>(e.alive)})
                    sum = (sum ^ v) * 1099511628211ull;
            return sum;
        };
        Check(run() == run(), "the defense is deterministic");
    }

    // 6. From the tank waves on the plain riflemen are a small share of a wave
    for (const AIDifficulty level : {AIDifficulty::Hard, AIDifficulty::Hardcore})
    {
        World w(level);
        const AITacticsConfig &t = w.ai.GetTactics();
        for (const bool factory : {false, true})
            for (int wave = t.assaultWave; wave <= t.assaultWave + 8; ++wave)
            {
                const AIWavePlan plan = w.ai.PlanWave(wave, factory, factory);
                int riflemen = 0, heavy = 0;
                for (int arch : plan.archetypes)
                {
                    riflemen += arch == 1 ? 1 : 0;
                    heavy += arch == 3 ? 1 : 0;
                }
                const int total = static_cast<int>(plan.archetypes.size());
                Check(total >= 10 && riflemen * 100 <= total * t.lateRiflemanPercent, "late waves: few riflemen");
                Check(heavy > riflemen, "late waves: heavy infantry outnumbers the riflemen");
            }
        if (level == AIDifficulty::Hard)
        {
            World easy(AIDifficulty::Easy);
            const AITacticsConfig &et = easy.ai.GetTactics();
            const AIWavePlan plan = easy.ai.PlanWave(et.assaultWave + 4, true, true);
            int plain = 0;
            for (int arch : plan.archetypes)
                plain += arch == 1 ? 1 : 0;
            Check(
                plain * 100 <= static_cast<int>(plan.archetypes.size()) * et.lateRiflemanPercent,
                "Easy: its own share of riflemen");
        }
        const AIWavePlan first = w.ai.PlanWave(1, false, false);
        int riflemen = 0;
        for (int arch : first.archetypes)
            riflemen += arch == 1 ? 1 : 0;
        Check(riflemen * 2 > static_cast<int>(first.archetypes.size()), "the first wave is still mostly riflemen");
    }

    // 7. The fortification: bunkers in front right behind the wall, towers behind them, built bunkers - walls - towers
    auto fromHq = [](const AIDefenseSite &site) {
        const float dx = static_cast<float>(site.tileX + 1 - kHqX), dy = static_cast<float>(site.tileY + 1 - kHqY);
        return dx * dx + dy * dy;
    };
    auto rank = [](const AIDefenseSite &site) {
        return site.kind == AIDefenseSite::Bunker ? 0 : site.kind == AIDefenseSite::Tower ? 2 : 1;
    };
    std::pair<int, int> defenseCounts[2] {};
    for (unsigned seed = 1; seed <= 64; ++seed)
        for (const bool corridor : {false, true})
        {
            World w;
            w.ai.SetSkirmish(true);
            w.ai.SetSeed(seed);
            if (corridor)
            {
                // A cliff across the way to the enemy with one gap, 6 tiles wide, where the line of defense may be
                AIDefenseTest::SetLineOffset(w.ai, 0);
                for (int y = 0; y < w.map.GetHeight(); ++y)
                    if (y < 17 || y > 22)
                        w.map.SetPassable(26, y, false);
            }
            const std::vector<AIDefenseSite> plan = AIDefenseTest::Plan(w.ai, w.map, w.entities, w.pf);
            int bunkers = 0, towers = 0, atWall = 0;
            float nearestBunker = 1e9f, farthestTower = 0.0f;
            bool ordered = true;
            for (std::size_t i = 0; i < plan.size(); ++i)
            {
                ordered = ordered && (i == 0 || rank(plan[i - 1]) <= rank(plan[i]));
                if (plan[i].kind == AIDefenseSite::Bunker)
                {
                    ++bunkers;
                    nearestBunker = std::min(nearestBunker, fromHq(plan[i]));
                    atWall += plan[i].tileX < 26 && plan[i].tileX >= 21 ? 1 : 0;
                }
                else if (plan[i].kind == AIDefenseSite::Tower)
                {
                    ++towers;
                    farthestTower = std::max(farthestTower, fromHq(plan[i]));
                }
            }
            if (seed == 1)
                defenseCounts[corridor] = {bunkers, towers};
            Check(
                defenseCounts[corridor] == std::make_pair(bunkers, towers),
                "seed does not change the number of bunkers or towers");
            Check(bunkers >= 2, "at least two bunkers on every way in");
            Check(towers >= 1, "towers are planned");
            Check(farthestTower < nearestBunker, "every tower stands behind the bunkers");
            Check(ordered, "the plan is built bunkers, walls, towers");
            if (corridor)
                Check(atWall >= 2, "passage: two bunkers right behind the wall, one on each side of the gate");

            // Built, and planned again (a headquarters was finished): nothing is built twice
            const bool conf = w.ai.Side() == Faction::Confederation;
            for (const AIDefenseSite &site : plan)
                if (site.kind != AIDefenseSite::Wall)
                    w.entities.SpawnBuildingByIndex(
                        site.kind == AIDefenseSite::Bunker ? (conf ? 5 : 13) : (conf ? 7 : 14),
                        site.tileX,
                        site.tileY,
                        w.ai.Side(),
                        1.0f,
                        w.ai.Player());
            int again = 0;
            for (const AIDefenseSite &site : AIDefenseTest::Plan(w.ai, w.map, w.entities, w.pf))
                again += site.kind != AIDefenseSite::Wall && !site.done ? 1 : 0;
            Check(again == 0, "what stands already is not planned twice");
        }
    // A further headquarters far from the first gets its own bunkers; the limits are those of the whole base
    {
        World w;
        w.ai.SetSkirmish(true);
        w.ai.SetSeed(3);
        const int before = static_cast<int>(AIDefenseTest::Plan(w.ai, w.map, w.entities, w.pf).size());
        const int secondId = w.entities.SpawnBuildingByIndex(0, kHqX - 10, kHqY + 16, kOwn, 1.0f, w.ai.Player());
        const Building *second = w.entities.GetBuilding(secondId);
        const int sx = second->tileX + second->buildWidth / 2, sy = second->tileY + second->buildHeight / 2;
        auto atSecond = [&](const AIDefenseSite &site) {
            const float dx = static_cast<float>(site.tileX + 1 - sx), dy = static_cast<float>(site.tileY + 1 - sy);
            return dx * dx + dy * dy < fromHq(site);
        };
        const std::vector<AIDefenseSite> plan = AIDefenseTest::Plan(w.ai, w.map, w.entities, w.pf);
        int atOutpost = 0, bunkers = 0;
        for (const AIDefenseSite &site : plan)
            if (site.kind == AIDefenseSite::Bunker)
            {
                ++bunkers;
                atOutpost += atSecond(site) ? 1 : 0;
            }
        Check(static_cast<int>(plan.size()) > before && atOutpost >= 2, "a further headquarters gets its own bunkers");
        Check(BuildLimits::LoadText("buildings:\n  bunker: 3\n"), "limits load");
        int open = 0, firstOpen = 0;
        for (const AIDefenseSite &site : AIDefenseTest::Plan(w.ai, w.map, w.entities, w.pf))
            if (site.kind == AIDefenseSite::Bunker && !site.done)
            {
                ++open;
                firstOpen += atSecond(site) ? 0 : 1;
            }
        Check(bunkers > 3 && open == 3, "no more bunkers are planned than the limit allows");
        Check(firstOpen >= 2, "... and the first headquarters keeps its two");
        BuildLimits::Reload();
    }

    // 8. A rich computer player builds its base: bunkers before towers, a further headquarters far out, the limits
    {
        World w(AIDifficulty::Hard);
        w.ai.SetSkirmish(true);
        w.ai.SetSeed(5);
        w.ai.RecordStartingBase(w.entities, w.map);
        bool towerBeforeBunker = false;
        for (int tick = 0; tick < 20 * 60 * 8; ++tick)
        {
            AIDefenseTest::SetCredits(w.ai, 20000);
            w.entities.Update(0.05f, w.map, w.pf);
            w.ai.Update(0.05f, w.map, w.entities, w.pf);
            if (tick % 100 == 0 && !towerBeforeBunker)
            {
                int bunkers = 0, towers = 0;
                for (const auto &e : w.entities.GetAllEntities())
                    if (const Building *b = e.AsBuilding(); b && b->alive && b->owner == w.ai.Player())
                    {
                        bunkers += b->type == UnitType::Bunker ? 1 : 0;
                        towers += b->type == UnitType::Tower ? 1 : 0;
                    }
                towerBeforeBunker = towers > 0 && bunkers < 2;
            }
        }
        int bunkers = 0, towers = 0, headquarters = 0;
        float farthestHq = 0.0f;
        for (const auto &e : w.entities.GetAllEntities())
            if (const Building *b = e.AsBuilding(); b && b->alive && b->owner == w.ai.Player())
            {
                bunkers += b->type == UnitType::Bunker ? 1 : 0;
                towers += b->type == UnitType::Tower ? 1 : 0;
                if (b->type == UnitType::Headquarters)
                {
                    ++headquarters;
                    const float dx = static_cast<float>(b->tileX + b->buildWidth / 2 - kHqX - 1),
                                dy = static_cast<float>(b->tileY + b->buildHeight / 2 - kHqY);
                    farthestHq = std::max(farthestHq, dx * dx + dy * dy);
                }
            }
        std::printf(
            "base: %d headquarters (farthest %.1f), %d bunkers, %d towers\n",
            headquarters,
            std::sqrt(farthestHq),
            bunkers,
            towers);
        Check(!towerBeforeBunker, "the bunkers are built before the towers");
        Check(bunkers >= 2 && towers >= 1, "bunkers and towers are built");
        Check(bunkers <= 10 && towers <= 10, "the building limits hold");
        Check(headquarters >= 2 && farthestHq >= 12.0f * 12.0f, "a further headquarters is built far from the first");
    }

    // A long wait must not let infantry spend the factory's budget first.
    {
        World w;
        const int barracks = w.entities.SpawnBuildingByIndex(2, 30, 30, kOwn, 1.0f, w.ai.Player());
        const int factory = w.entities.SpawnBuildingByIndex(3, 40, 30, kOwn, 1.0f, w.ai.Player());
        w.entities.GetBuilding(barracks)->isPowered = true;
        w.entities.GetBuilding(factory)->isPowered = true;
        const int cost = w.entities.GetUnitConfig(7)->creditCost;
        AIDefenseTest::PrepareVehicleProduction(w.ai, cost - 1);
        AIDefenseTest::Produce(w.ai, w.map, w.entities);
        Check(!w.entities.GetBuilding(barracks)->isProducing, "infantry preserves vehicle savings after a long wait");
        AIDefenseTest::PrepareVehicleProduction(w.ai, cost);
        AIDefenseTest::Produce(w.ai, w.map, w.entities);
        Check(w.entities.GetBuilding(factory)->isProducing, "factory gets credits before barracks after a long wait");
        Check(!w.entities.GetBuilding(barracks)->isProducing, "infantry cannot spend the last tank budget");
    }

    // Full waves, a large army and research savings must not idle production.
    for (bool limited : {false, true})
    {
        World w(AIDifficulty::Hard);
        BuildLimits::LoadText(limited ? "max_army: 53\n" : "max_army: 1024\n");
        const int barracks = w.entities.SpawnBuildingByIndex(2, 30, 30, kOwn, 1.0f, w.ai.Player());
        const int factory = w.entities.SpawnBuildingByIndex(3, 40, 30, kOwn, 1.0f, w.ai.Player());
        w.entities.GetBuilding(barracks)->isPowered = true;
        w.entities.GetBuilding(factory)->isPowered = true;
        for (int i = 0; i < 52; ++i)
            w.Own(5 + i % 20, 55 + i / 20);
        const int budget = w.entities.GetUnitConfig(7)->creditCost + w.entities.GetUnitConfig(1)->creditCost;
        AIDefenseTest::PrepareVehicleProduction(w.ai, budget);
        AIDefenseTest::FinishProductionQueues(w.ai);
        AIDefenseTest::Produce(w.ai, w.map, w.entities);
        Check(w.entities.GetBuilding(factory)->isProducing, "empty vehicle queue refilled beyond old AI army cap");
        Check(
            w.entities.GetBuilding(barracks)->isProducing == !limited,
            "infantry keeps producing unless the configured army cap is reached in this tick");
    }
    BuildLimits::LoadText("max_army: 1024\n");

    // Lower difficulties still stop at a completed wave; only Hard/Hardcore refill forever.
    for (auto level : {AIDifficulty::Easy, AIDifficulty::Medium, AIDifficulty::Hard, AIDifficulty::Hardcore})
    {
        World w(level);
        const int factory = w.entities.SpawnBuildingByIndex(3, 30, 30, kOwn, 1.0f, w.ai.Player());
        w.entities.GetBuilding(factory)->isPowered = true;
        AIDefenseTest::PrepareVehicleProduction(w.ai, 10000);
        AIDefenseTest::FinishProductionQueues(w.ai);
        AIDefenseTest::Produce(w.ai, w.map, w.entities);
        Check(
            w.entities.GetBuilding(factory)->isProducing
                == (level == AIDifficulty::Hard || level == AIDifficulty::Hardcore),
            "continuous factory production is restricted to Hard and Hardcore");
    }

    // A free factory beyond the plan (an infantry-only wave) builds a mix of vehicles, not one light vehicle over
    // and over
    {
        World w(AIDifficulty::Hard);
        const int factory = w.entities.SpawnBuildingByIndex(3, 30, 30, kOwn, 1.0f, w.ai.Player());
        w.entities.GetBuilding(factory)->isPowered = true;
        std::vector<int> built;
        for (int round = 0; round < 4; ++round)
        {
            AIDefenseTest::PrepareVehicleProduction(w.ai, 10000);
            AIDefenseTest::FinishProductionQueues(w.ai);
            AIDefenseTest::SetPlan(w.ai, {1, 1, 1}); // (an infantry-only wave)
            Building *f = w.entities.GetBuilding(factory);
            f->isProducing = false;
            f->productionTimer = 0.0f;
            AIDefenseTest::Produce(w.ai, w.map, w.entities);
            for (const auto &e : w.entities.GetAllEntities())
            {
                Unit *u = const_cast<Entity &>(e).AsUnit();
                if (u && u->alive && u->owner == w.ai.Player() && !u->IsInfantry())
                {
                    built.push_back(u->archetypeId);
                    u->alive = false; // (clears the exit for the next one)
                }
            }
        }
        std::sort(built.begin(), built.end());
        Check(built.size() == 4, "a free factory keeps building");
        Check(
            std::unique(built.begin(), built.end()) - built.begin() >= 2,
            "a free factory builds a mix of vehicles, not only the lightest one");
    }

    // Skirmish: no troop of the computer is forgotten - one that no list holds joins the reserve; a mission's
    // troops stay its script's
    for (bool skirmish : {true, false})
    {
        World w(AIDifficulty::Hard);
        w.ai.SetSkirmish(skirmish);
        AIDefenseTest::Prepare(w.ai, {}, 10, kHqX + 4, kHqY + 4);
        const int lost = w.Own(60, 50);
        AIDefenseTest::UpdateRoster(w.ai, w.entities);
        Check(AIDefenseTest::InRoster(w.ai, lost) == skirmish, "a forgotten troop joins the reserve in a skirmish");
    }

    // A damaged vehicle without a factory is never repaired: it does not wait for it for good
    {
        World w(AIDifficulty::Hard);
        w.ai.SetSkirmish(true);
        const int tank = w.Own(kHqX + 6, kHqY + 6, 9);
        AIDefenseTest::Prepare(w.ai, {tank}, 10, kHqX + 4, kHqY + 4);
        Unit *t = w.entities.GetUnit(tank);
        t->currentHp = t->maxHp / 4;
        AIDefenseTest::SendToRepair(w.ai, tank);
        AIDefenseTest::Retreat(w.ai, w.entities, w.pf);
        Check(
            !AIDefenseTest::Repairing(w.ai, tank) && AIDefenseTest::InRoster(w.ai, tank),
            "a vehicle with no factory to repair it rejoins the reserve");
    }

    // A one-tile exit shared with an ally must drain before more units are spawned.
    {
        World w(AIDifficulty::Hard);
        std::array<PlayerSlot, kMaxPlayers> slots {};
        slots[0] = Players::Slot(0);
        slots[1] = Players::Slot(1);
        slots[0].team = slots[1].team;
        Players::Setup(slots, 0);
        const int factory = w.entities.SpawnBuildingByIndex(3, 30, 30, kOwn, 1.0f, w.ai.Player());
        w.entities.GetBuilding(factory)->isPowered = true;
        int sx = 0, sy = 0;
        Check(w.entities.GetBuildingSpawnPoint(factory, w.map, sx, sy), "factory initially has a free exit");
        for (int y = sy - 10; y <= sy + 10; ++y)
            for (int x = sx - 10; x <= sx + 10; ++x)
                w.map.SetPassable(x, y, y == sy && x >= sx - 4 && x <= sx + 4);
        std::vector<int> blockers;
        for (int dx = -4; dx <= 3; ++dx)
            if (dx != 0)
                blockers.push_back(w.entities.SpawnUnitByArchetype(0, sx + dx, sy, kFoe, 0));
        AIDefenseTest::PrepareVehicleProduction(w.ai, 10000);
        AIDefenseTest::Produce(w.ai, w.map, w.entities);
        Check(!w.entities.GetBuilding(factory)->isProducing, "allied congestion pauses factory production");
        Check(w.ai.GetEnemyCredits() == 10000, "congestion does not spend credits");
        for (int id : blockers)
            w.entities.GetUnit(id)->alive = false;
        AIDefenseTest::Produce(w.ai, w.map, w.entities);
        Check(w.entities.GetBuilding(factory)->isProducing, "production resumes after the allied column leaves");
    }

    // Head-on allies may yield, including a moving chain held by its group.
    {
        World w;
        std::array<PlayerSlot, kMaxPlayers> slots {};
        slots[0] = Players::Slot(0);
        slots[1] = Players::Slot(1);
        slots[0].team = slots[1].team;
        Players::Setup(slots, 0);
        const int first = w.Own(30, 30);
        const int second = w.Foe(31, 30);
        const int third = w.Foe(32, 30);
        Unit &tail = *w.entities.GetUnit(third);
        tail.state = ActionState::Move;
        tail.path = {{31, 30}};
        tail.blockedTimer = 3.0f;
        for (int y = 24; y <= 36; ++y)
            for (int x = 24; x <= 38; ++x)
                w.map.SetPassable(x, y, y == 30);
        Unit &a = *w.entities.GetUnit(first), &b = *w.entities.GetUnit(second);
        a.state = b.state = ActionState::Move;
        a.path = {{31, 30}};
        b.path = {{30, 30}};
        b.groupHold = true;
        Check(b.CanMakeWayFor(a), "head-on ally with lower priority can yield");
        Check(!a.CanMakeWayFor(b), "head-on priority cannot reverse");
        UnitDelegates delegates;
        delegates.getEntity = [&](int id) -> Entity * {
            return w.entities.GetUnit(id);
        };
        delegates.getUnitAtTile = [&](int x, int y, int ignore) {
            for (int id : {first, second, third})
            {
                const Unit *u = w.entities.GetUnit(id);
                if (id != ignore && u->tileX == x && u->tileY == y)
                    return id;
            }
            return 0;
        };
        Check(a.RequestMakeWay(b, w.map, delegates), "head-on ally receives a make-way route");
        Check(
            !b.groupHold && !b.path.empty() && b.path.front().x == 32,
            "group-held ally yields into the next tile of the narrow corridor");
        Check(
            !tail.path.empty() && tail.path.front().x == 33,
            "the blocked moving tail steps back to release the allied column");
        b.resTileX = 32;
        b.resTileY = 30;
        Check(!b.CanMakeWayFor(a), "a unit already stepping keeps its reservation");
    }

    // Elimination removes both physical walls and their remembered image under fog.
    {
        World w;
        const PlayerId owner = w.ai.Player();
        w.map.SetFogEnabled(true);
        w.map.SetWall(25, 25, kOwn, 100, owner);
        w.map.SetWall(26, 25, kFoe, 100, Players::Local());
        w.map.RevealArea(25, 25, 4);
        w.map.ResetVisibility();
        Check(w.map.ViewerWall(25, 25) != 0, "enemy wall remembered under fog");
        w.entities.GetBuilding(w.hq)->alive = false;
        const auto eliminated = w.entities.UpdateElimination(w.map);
        Check(eliminated.size() == 1 && eliminated.front() == owner, "only defeated owner is eliminated");
        Check(w.entities.IsEliminated(owner), "defeat state available to statistics");
        Check(w.map.GetWall(25, 25) == 0 && w.map.IsPassable(25, 25), "defeated player's wall destroyed and passable");
        Check(w.map.ViewerWall(25, 25) == 0, "defeated player's remembered wall removed");
        Check(w.map.GetWall(26, 25) != 0 && w.map.ViewerWall(26, 25) != 0, "surviving player's walls retained");
        Check(w.entities.UpdateElimination(w.map).empty(), "elimination reported only once");
    }

    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
