// Computer player, defense of the base: attackers known by the damage they do, who goes for them, waves that wait,
// groups on the march that turn back, the limit of a chase
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "game/AIController.hpp"
#include "game/BuildLimits.hpp"
#include "game/EntityManager.hpp"
#include "game/Pathfinder.hpp"
#include "game/TileMap.hpp"

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
    static void SetCredits(AIController &ai, int credits)
    {
        ai.m_credits = credits;
    }
    static void SetLineOffset(AIController &ai, int tiles)
    {
        ai.m_style.lineOffset = tiles;
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
    for (unsigned seed = 1; seed <= 12; ++seed)
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

    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
