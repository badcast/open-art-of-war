// BuildLimits: max_army parsing, its default and how it holds the lobby's army size choice
#include "game/BuildLimits.hpp"

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

int main()
{
    CHECK(BuildLimits::LoadText("default: 4\n"));
    CHECK(BuildLimits::MaxArmy() == BuildLimits::kDefaultMaxArmy);
    CHECK(BuildLimits::EffectiveArmyCap(0) == 10000);
    CHECK(BuildLimits::EffectiveArmyCap(50) == 50);

    CHECK(BuildLimits::LoadText("max_army: 30\n"));
    CHECK(BuildLimits::MaxArmy() == 30);
    CHECK(BuildLimits::EffectiveArmyCap(0) == 30);
    CHECK(BuildLimits::EffectiveArmyCap(10) == 10);
    CHECK(BuildLimits::EffectiveArmyCap(1000) == 30);

    CHECK(BuildLimits::LoadText("max_army: -1\n"));
    CHECK(BuildLimits::MaxArmy() == BuildLimits::kUnlimited);
    CHECK(BuildLimits::EffectiveArmyCap(0) == 0);
    CHECK(BuildLimits::EffectiveArmyCap(200) == 200);

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
