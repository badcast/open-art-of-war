#pragma once

namespace OpenArtOfWar
{

class Application;

class HeadlessTest
{
public:
    static bool run(Application &app, int numTicks = 200);
    static bool RunGroupMovementTest(Application &app);
    static bool RunAiBehaviourTest(Application &app);
    // A group in front of a hostile wall with one gap breaches the wall; a unit hit from afar goes
    // after the shooter, except when holding position
    static bool RunCombatBehaviourTest(Application &app);
    // Allies let each other through: a gap in an own wall packed with idle units, two units head-on in a corridor;
    // diagonal steps cut corners (a diagonal wall line can be crossed)
    static bool RunMakeWayTest(Application &app);
    // The same commands on the same world give the same world, tick for tick (lockstep needs this)
    static bool RunDeterminismTest(Application &app);
    // A match of four players in two mixed teams: bases, owners, teams, sight, economies, determinism
    static bool RunMatchTest(Application &app);
    static bool RunReplayTest(Application &app);
    static bool RunTechTest(Application &app);
    static bool RunAiVehicleTest(Application &app);
    static bool RunFogBlockTest(Application &app);
    static bool RunBuildRulesTest(Application &app);
    // A match with computer players in slots: they build and train, and do it alike every time
    static bool RunMatchAiTest(Application &app);
    static bool RunBurstTest(Application &app);
    static bool RunDataSyncTest(Application &app);
};

} // namespace OpenArtOfWar
