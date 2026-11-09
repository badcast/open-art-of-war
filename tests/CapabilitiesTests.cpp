// Capabilities: tech level parsing, the default level, what a level allows and the researches it takes away
#include "core/LoaderResources.hpp"
#include "game/Capabilities.hpp"
#include "game/EntityManager.hpp"
#include "game/Research.hpp"
#include "net/NetMatch.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdio>

using namespace OpenArtOfWar;

static int fails = 0;
#define CHECK(c)                                                    \
    do                                                              \
    {                                                               \
        if (!(c))                                                   \
        {                                                           \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++fails;                                                \
        }                                                           \
    } while (0)

static const char *kFile = R"(
default: "full"
basic:
    confederation:
        builds: [command_center_conf, wall_conf]
        units: [rifleman_conf]
        researches: [0, 1, 5, 6]
    resistance:
        builds: [headquarters_res]
        units: [rifleman_res]
        researches: []
full:
    confederation:
        builds: [command_center_conf, tech_center_conf, wall_conf]
        units: [rifleman_conf, flamethrower_conf]
        researches: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]
)";

int main()
{
    EntityManager entities;
    for (const char *yaml :
         {"id: command_center_conf\nbuilding_index: 0\n",
          "id: tech_center_conf\nbuilding_index: 4\n",
          "id: wall_conf\nbuilding_index: 15\n",
          "id: rifleman_conf\narchetype_id: 1\n",
          "id: flamethrower_conf\narchetype_id: 5\n",
          "id: headquarters_res\nbuilding_index: 8\n",
          "id: rifleman_res\narchetype_id: 0\n"})
        entities.LoadCharacterYaml(YAML::Load(yaml));

    std::array<PlayerSlot, kMaxPlayers> slots {};
    slots[0].active = slots[1].active = true;
    slots[0].faction = Faction::Confederation;
    slots[1].faction = Faction::Resistance;
    Players::Setup(slots, 0);

    // Nothing restricted before a level is set
    CHECK(Capabilities::LoadText(kFile));
    CHECK(Capabilities::AllowsBuilding(0, 4));
    CHECK(Capabilities::AllowsUnit(0, 5));
    CHECK(Capabilities::AllowsResearch(0, 13));

    CHECK(Capabilities::Levels().size() == 2);
    CHECK(Capabilities::Levels()[0] == "basic");
    CHECK(Capabilities::LevelIndex("full") == 1);
    CHECK(Capabilities::LevelIndex("none") == -1);
    CHECK(Capabilities::DefaultLevel() == "full");
    CHECK(Capabilities::ResolveLevel(-1, -1) == "full");
    CHECK(Capabilities::ResolveLevel(0, -1) == "basic");
    CHECK(Capabilities::ResolveLevel(0, 1) == "full");
    CHECK(Capabilities::ResolveLevel(-1, -1, "basic") == "basic");
    CHECK(Capabilities::ResolveLevel(1, -1, "basic") == "full");
    CHECK(Capabilities::ResolveLevel(-1, -1, "nope") == "full");

    // Every active player at its slot's level, else the match's, else the preset
    slots[1].techLevel = 1;
    Players::Setup(slots, 0);
    Capabilities::ApplyLevels(-1, "basic", entities);
    CHECK(Capabilities::PlayerLevel(0) == "basic");
    CHECK(Capabilities::PlayerLevel(1) == "full");
    CHECK(Capabilities::PlayerLevel(2).empty());
    slots[1].techLevel = -1;
    Players::Setup(slots, 0);

    Capabilities::SetPlayerLevel(0, "basic", entities);
    CHECK(Capabilities::PlayerLevel(0) == "basic");
    CHECK(Capabilities::AllowsBuilding(0, 0));
    CHECK(!Capabilities::AllowsBuilding(0, 4));
    CHECK(Capabilities::AllowsBuilding(0, 15)); // walls by any segment index
    CHECK(Capabilities::AllowsBuilding(0, 20));
    CHECK(Capabilities::AllowsUnit(0, 1));
    CHECK(!Capabilities::AllowsUnit(0, 5));
    CHECK(Capabilities::AllowsResearch(0, 0));
    CHECK(!Capabilities::AllowsResearch(0, 2)); // not listed
    // Listed, but it unlocks the flamethrower the level leaves out, and the one after it needs it
    CHECK(researchDef(Faction::Confederation, 5).effect == ResearchEffect::UnlockUnit);
    CHECK(!Capabilities::AllowsResearch(0, 5));
    CHECK(researchDef(Faction::Confederation, 6).needs == 5);
    CHECK(!Capabilities::AllowsResearch(0, 6));
    // Research 1 needs 0, which is allowed
    CHECK(Capabilities::AllowsResearch(0, 1));

    Capabilities::SetPlayerLevel(1, "basic", entities);
    CHECK(Capabilities::AllowsBuilding(1, 8));
    CHECK(!Capabilities::AllowsResearch(1, 0));
    CHECK(Capabilities::AllowsBuilding(0, 0)); // the other player keeps its own

    Capabilities::SetPlayerLevel(0, "full", entities);
    CHECK(Capabilities::AllowsUnit(0, 5));
    CHECK(Capabilities::AllowsResearch(0, 5));

    // An unknown level restricts nothing
    Capabilities::SetPlayerLevel(1, "nope", entities);
    CHECK(Capabilities::AllowsBuilding(1, 12));

    Capabilities::ResetLevels();
    CHECK(Capabilities::AllowsBuilding(0, 4) && Capabilities::PlayerLevel(0).empty());

    // A file that cannot be read keeps what was loaded; one without levels restricts nothing
    CHECK(!Capabilities::LoadText("basic: [unclosed"));
    CHECK(Capabilities::Levels().size() == 2);
    CHECK(Capabilities::LoadText("default: basic\n"));
    CHECK(Capabilities::Levels().empty() && Capabilities::DefaultLevel().empty());

    // The match setup carries the levels to every computer and into replays
    {
        MatchSetup setup;
        setup.mapPath = LoaderResources::MapsDir() + "/skirmish_1.json";
        setup.slots[0].active = setup.slots[2].active = true;
        setup.slots[0].human = true;
        setup.slots[2].techLevel = 2;
        setup.techLevel = 0;
        for (const bool night : {false, true})
        {
            setup.night = night;
            MatchSetup out;
            CHECK(NetMatch::DecodeSetup(NetMatch::EncodeSetup(setup), out));
            CHECK(out.night == night && out.techLevel == 0);
            CHECK(out.slots[0].techLevel == -1 && out.slots[2].techLevel == 2);
        }
    }

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
