#pragma once

#include "game/Entity.hpp"

#include <string>
#include <vector>

namespace OpenArtOfWar
{

    // How each kind of projectile flies, looks and hits: assets/characters/projectiles.yaml (edited in the character
    // editor, tab Projectiles). Every value has the built-in default of the game, so a missing file changes nothing.
    // The file is part of the network assets check: speed, spread and hit-scan change the simulation.
    struct ProjectileSpec
    {
        const char *key {"bullet"}; // name in the file and in the characters' weapons (projectile: <key>)
        // Hit-scan: the damage is dealt when the shot is fired; the projectile on screen is only the tracer.
        // Otherwise it flies, deals its damage where it lands and splashes (splash_percent of the weapon).
        bool hitscan {true};
        int sprite {1309};                        // first composite of the flying projectile, one per direction (0: nothing drawn)
        int directions {32};                      // 16 or 32
        float speed {36.0f};                      // pixels per simulation tick
        float spread {0.065f};                    // largest aim error in radians (a weapon's own spread overrides it)
        std::vector<int> impact {55, 56, 57, 58}; // animation where it hits a unit / building / the ground, by target id
        int impactWall {55};                      // animation on a wall that stands the hit
        int wallDestroyed {42};                   // animation on a wall the hit brings down
        bool smokeTrail {false};                  // puffs of smoke behind it (rockets)
        std::vector<int> groundMarks;             // composites of the small crater left where it lands (one picked); empty: none
        float groundMarkSeconds {45.0f};          // how long a crater stays on the ground

        // Impact animation for a target / place (-1 none)
        int pickImpact(int seed) const;
        int pickGroundMark(int seed) const;
    };

    namespace Projectiles
    {
        constexpr int kCount = 5; // ProjectileType values

        const ProjectileSpec &of(ProjectileType type);
        const char *keyOf(ProjectileType type);
        // A key of the file / a weapon's `projectile:` (unknown: bullet)
        ProjectileType fromKey(const std::string &key);

        // Reads the file text over the defaults; false when it cannot be parsed
        bool loadText(const std::string &yamlText);
        // Back to the defaults overlaid with assets/characters/projectiles.yaml
        void reload();
        // Defaults overlaid with this text (the editor shows its changes at once)
        void apply(const std::string &yamlText);
    } // namespace Projectiles

} // namespace OpenArtOfWar
