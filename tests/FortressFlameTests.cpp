// The Fortress's flamethrower comes with its technology: without it a Fortress is made as the base form (machine gun
// only, archetype 18); the research turns the ones that exist into the full form (archetype 7, flame as second gun).
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "game/EntityManager.hpp"
#include "game/Players.hpp"
#include "game/Research.hpp"

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
    slots[0].faction = Faction::Confederation;
    slots[0].team = 0;
    slots[0].human = true;
    slots[1].active = true;
    slots[1].faction = Faction::Resistance;
    slots[1].team = 1;
    Players::Setup(slots, 0);
    ResetResearch();

    EntityManager entities;
    entities.LoadCharacterConfigs();
    Check(entities.GetUnitConfig(7) && entities.GetUnitConfig(18), "the Fortress has its full and its base form");

    const int form = ProductionForm(0, 7);
    Check(form == 18, "without the technology a Fortress is produced as the base form");
    const int id = entities.SpawnUnitByArchetype(form, 10, 10, Faction::Confederation, 0);
    const Unit *u = entities.GetUnit(id);
    Check(u && u->archetypeId == 18 && !u->hasSecondary, "the base Fortress has no flamethrower");
    Check(!entities.IsFortressFlamethrowerUnlocked(0), "the flamethrower is not unlocked");

    int index = -1;
    for (int i = 0; i < kResearchCount; ++i)
        if (researchDef(Faction::Confederation, i).effect == ResearchEffect::FortressFlamethrower)
            index = i;
    Check(index >= 0, "the Fortress technology exists");
    CompleteResearch(0, index, entities);
    u = entities.GetUnit(id);
    Check(u && u->archetypeId == 7 && u->hasSecondary, "the research gives the Fortresses that exist the flamethrower");
    Check(ProductionForm(0, 7) == 7, "after the technology new Fortresses are the full form");
    Check(ProductionForm(1, 6) == 6, "the other faction's units are not touched");

    if (g_failures == 0)
        std::printf("all passed\n");
    return g_failures == 0 ? 0 : 1;
}
