#include "game/AIController.hpp"
#include "game/EntityManager.hpp"
#include "game/TileMap.hpp"
#include <cstdio>

namespace OpenArtOfWar
{
struct AIBaseLayoutTest
{
    static bool Keeps(AIController &ai, TileMap &map, EntityManager &entities, int x, int y, int w, int h)
    {
        return ai.KeepsWayOut(x, y, w, h, map, entities);
    }
    static bool Site(
        AIController &ai, TileMap &map, EntityManager &entities, int &x, int &y, int centre = 4, int crowding = 3)
    {
        ai.m_homeX = 4;
        ai.m_homeY = 4;

        return ai.FindCoreSite(x, y, 2, 2, -1, 0, false, 0, map, entities, crowding, false, centre, 4);
    }
};
} // namespace OpenArtOfWar
using namespace OpenArtOfWar;
int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        if (!ok)
        {
            std::printf("FAIL: %s\n", name);
            ++failures;
        }
    };
    AIController ai;
    EntityManager entities;
    TileMap map;
    map.InitBlankArena(12, 8);
    for (int y = 0; y < map.GetHeight(); ++y)
        for (int x = 0; x < map.GetWidth(); ++x)
            map.SetPassable(x, y, false);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 12; ++x)
            map.SetPassable(x, y, true);
    check(
        !AIBaseLayoutTest::Keeps(ai, map, entities, 5, 0, 2, 8),
        "small map: full divider rejected without road targets");
    check(AIBaseLayoutTest::Keeps(ai, map, entities, 5, 0, 2, 6), "two tile exit preserved");
    check(!AIBaseLayoutTest::Keeps(ai, map, entities, 5, 0, 2, 7), "vehicle exit narrowing rejected");
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 12; ++x)
            map.SetPassable(x, y, y == 3);
    check(!AIBaseLayoutTest::Keeps(ai, map, entities, 5, 3, 1, 1), "existing one tile corridor cannot be closed");
    check(AIBaseLayoutTest::Keeps(ai, map, entities, 5, 0, 2, 2), "existing narrow corridor need not be widened");
    map.InitBlankArena(8, 12);
    for (int y = 0; y < map.GetHeight(); ++y)
        for (int x = 0; x < map.GetWidth(); ++x)
            map.SetPassable(x, y, false);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 8; ++x)
            map.SetPassable(x, y, true);
    check(!AIBaseLayoutTest::Keeps(ai, map, entities, 0, 5, 8, 2), "vertical map divider rejected");
    const char *reason = nullptr;
    check(
        !entities.CanPlaceBuilding(-1, 0, 2, 2, map, ai.Player(), &reason) && reason != nullptr,
        "placement rejection has diagnostic reason");
    Players::SetupSinglePlayer(Faction::Resistance);
    map.InitBlankArena(128, 16);
    map.SetFogEnabled(false);
    entities.SpawnBuildingByIndex(0, 2, 2, Faction::Resistance, 1.0f, ai.Player());
    entities.SpawnBuildingByIndex(0, 98, 2, Faction::Resistance, 1.0f, ai.Player());
    // Leave just a connected eastern pocket beside the second headquarters.
    for (int y = 0; y < map.GetHeight(); ++y)
        for (int x = 0; x < map.GetWidth(); ++x)
            map.SetPassable(x, y, x >= 102 && y < 8);
    for (unsigned seed = 1; seed <= 12; ++seed)
    {
        ai.SetSeed(seed);
        int x = -1, y = -1, againX = -1, againY = -1;
        check(!AIBaseLayoutTest::Site(ai, map, entities, x, y), "first HQ zone is full");
        check(AIBaseLayoutTest::Site(ai, map, entities, x, y, 100), "second HQ eastern pocket found for every style");
        check(
            AIBaseLayoutTest::Site(ai, map, entities, againX, againY, 100) && x == againX && y == againY,
            "placement is deterministic");
    }
    // Narrow map: a long strip with a niche by a cliff. Closing the niche (a dead end) is fine, closing the strip is not
    {
        EntityManager none;
        TileMap strip;
        strip.InitBlankArena(40, 6); // (the arena may come out bigger: everything outside the strip is closed)
        for (int y = 0; y < strip.GetHeight(); ++y)
            for (int x = 0; x < strip.GetWidth(); ++x)
                strip.SetPassable(x, y, x < 40 && y < 6);
        strip.SetPassable(2, 0, false); // the niche (0..1, 0..1) opens only downwards
        strip.SetPassable(2, 1, false);
        check(AIBaseLayoutTest::Keeps(ai, strip, none, 0, 2, 2, 2), "narrow map: a niche may be closed");
        check(!AIBaseLayoutTest::Keeps(ai, strip, none, 20, 0, 2, 6), "narrow map: the strip may not be cut");
        for (int y = 0; y < 4; ++y)
            strip.SetPassable(20, y, false); // a cliff: the strip goes on through two rows at the bottom
        check(!AIBaseLayoutTest::Keeps(ai, strip, none, 21, 4, 1, 2), "narrow map: the last gap stays open");
        check(!AIBaseLayoutTest::Keeps(ai, strip, none, 21, 5, 1, 1), "narrow map: the gap stays wide for vehicles");
    }
    std::printf("AI layout: %d failures\n", failures);
    return failures ? 1 : 0;
}
