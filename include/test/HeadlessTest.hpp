#pragma once

namespace OpenArtOfWar
{

    class Application;

    class HeadlessTest
    {
    public:
        static bool run(Application &app, int numTicks = 200);
        static bool runGroupMovementTest(Application &app);
        static bool runAIBehaviourTest(Application &app);
        // The same commands on the same world give the same world, tick for tick (lockstep needs this)
        static bool runDeterminismTest(Application &app);
        // A match of four players in two mixed teams: bases, owners, teams, sight, economies, determinism
        static bool runMatchTest(Application &app);
        // A match with computer players in slots: they build and train, and do it alike every time
        static bool runMatchAiTest(Application &app);
    };

} // namespace OpenArtOfWar
