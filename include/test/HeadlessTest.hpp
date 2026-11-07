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
    static bool RunZeusTest(Application &app);
    // Allies let each other through: a gap in an own wall packed with idle units, two units head-on in a corridor;
    // diagonal steps cut corners (a diagonal wall line can be crossed)
    static bool RunMakeWayTest(Application &app);
    static bool RunSquadFocusTest(Application &app);
    // The same commands on the same world give the same world, tick for tick (lockstep needs this)
    static bool RunDeterminismTest(Application &app);
    // A match of four players in two mixed teams: bases, owners, teams, sight, economies, determinism
    static bool RunMatchTest(Application &app);
    static bool RunReplayTest(Application &app);
    static bool RunTechTest(Application &app);
    static bool RunAiVehicleTest(Application &app);
    static bool RunFogBlockTest(Application &app);
    static bool RunBuildRulesTest(Application &app);
    // Campaign maps in maps/mission (skirmish lists without them, old names still resolve); hard building limits
    // (data/limits.yaml) for a person's commands and for computer players, buildings of the map kept
    static bool RunLimitsAndMapsTest(Application &app);
    // A match with computer players in slots: they build and train, and do it alike every time
    static bool RunMatchAiTest(Application &app);
    // Computer allies on the four-player maps: no base closes itself or its ally in, each has a laboratory, power
    // plants stand behind the HQ, and the seed changes the layout
    static bool RunAiBaseLayoutTest(Application &app);
    // A custom game and a campaign mission of this computer, recorded tick by tick, play back to the same world
    static bool RunLocalReplayTest(Application &app);
    static bool RunBurstTest(Application &app);
    static bool RunDataSyncTest(Application &app);
};

} // namespace OpenArtOfWar
