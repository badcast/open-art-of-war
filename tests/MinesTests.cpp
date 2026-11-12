// Resistance mines: laid like wall segments once the technology is done, armed after a moment, fired by hostile units
// only (a Monitor Lizard mine by vehicles only), invisible to nobody in the simulation but gone once they blow.
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "game/EntityManager.hpp"
#include "game/Mines.hpp"
#include "game/Pathfinder.hpp"
#include "game/Players.hpp"
#include "game/Research.hpp"
#include "game/TileMap.hpp"

#include <array>
#include <cstdio>

using namespace OpenArtOfWar;

namespace
{
int g_failures = 0;
void Check(bool ok, const char *what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

int Run(EntityManager &entities, TileMap &map, Pathfinder &pf, int ticks)
{
    int n = 0;
    for (; n < ticks && !entities.GetMines().empty(); ++n)
        entities.Update(0.05f, map, pf);
    return n;
}
} // namespace

int main()
{
    if (!Assets::Init() || !LoaderResources::Init())
    {
        std::printf("FAIL: assets did not load\n");
        return 1;
    }
    std::array<PlayerSlot, kMaxPlayers> slots {};
    slots[0].active = true;
    slots[0].faction = Faction::Resistance;
    slots[0].team = 0;
    slots[0].human = true;
    slots[1].active = true;
    slots[1].faction = Faction::Confederation;
    slots[1].team = 1;
    Players::Setup(slots, 0);
    ResetResearch();

    Mines::Reload();
    Check(Mines::Specs().size() >= 2, "mines.yaml names the two Resistance mines");
    const auto &specs = Mines::Specs();

    EntityManager entities;
    entities.LoadCharacterConfigs();
    entities.RebuildBlueprints();
    Check(
        EntityManager::GetBlueprints(Faction::Resistance).back().mineKind == static_cast<int>(specs.size()) - 1,
        "the Resistance's build menu ends with its mines");
    Check(EntityManager::GetBlueprints(Faction::Confederation).back().mineKind < 0, "the Confederation has none");

    TileMap map;
    map.InitBlankArena(48, 48);
    map.SetFogEnabled(false);
    Pathfinder pf;
    entities.SpawnBuildingByIndex(8, 10, 10, Faction::Resistance, 1.0f, 0); // headquarters

    Check(!IsMineResearched(0, 0), "no mine before the technology");
    int frogIndex = -1;
    for (int i = 0; i < kResearchCount; ++i)
        if (researchDef(Faction::Resistance, i).effect == ResearchEffect::Mine && researchDef(Faction::Resistance, i).value == 0)
            frogIndex = i;
    Check(frogIndex >= 0, "the Frog mine technology exists");
    CompleteResearch(0, frogIndex, entities);
    Check(IsMineResearched(0, 0) && !IsMineResearched(0, 1), "only its own mine is unlocked");

    Check(entities.PlaceMine(0, 16, 12, map, 0), "a mine is laid on a free tile in the base");
    Check(!entities.PlaceMine(0, 16, 12, map, 0), "not twice on one tile");
    Check(!entities.PlaceMine(0, 46, 46, map, 0), "not outside the building radius");
    Check(entities.CountMines(0, 0) == 1, "one mine laid");

    // Own units walk over it harmlessly
    const int friendId = entities.SpawnUnitByArchetype(0, 16, 12, Faction::Resistance, 0);
    Run(entities, map, pf, 120);
    Check(entities.GetMines().size() == 1, "a friendly unit does not fire the mine");
    Check(entities.GetUnit(friendId) && entities.GetUnit(friendId)->currentHp == entities.GetUnit(friendId)->maxHp,
          "and is not hurt");

    // A hostile infantryman fires it once it is armed, and is hurt
    const int foeId = entities.SpawnUnitByArchetype(1, 16, 12, Faction::Confederation, 1);
    const Unit *foe = entities.GetUnit(foeId);
    const int hp0 = foe ? foe->maxHp : 0;
    Run(entities, map, pf, 200);
    Check(entities.GetMines().empty(), "a hostile unit fires the Frog mine");
    foe = entities.GetUnit(foeId);
    Check(!foe || !foe->alive || foe->currentHp < hp0, "and takes the blast");

    // The heavy mine ignores infantry
    CompleteResearch(0, frogIndex + 1 < kResearchCount ? frogIndex + 1 : frogIndex, entities);
    if (IsMineResearched(0, 1))
    {
        Check(entities.PlaceMine(1, 20, 12, map, 0), "the Monitor Lizard mine is laid");
        entities.SpawnUnitByArchetype(1, 20, 12, Faction::Confederation, 1);
        Run(entities, map, pf, 200);
        Check(entities.GetMines().size() == 1, "the Monitor Lizard mine ignores infantry");
    }

    if (g_failures == 0)
        std::printf("all passed\n");
    return g_failures == 0 ? 0 : 1;
}
