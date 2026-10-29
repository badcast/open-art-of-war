#pragma once

#include <string>
#include <vector>

namespace OpenArtOfWar
{

    // Looks of destruction: the explosions of dying units and buildings and the wrecks they leave behind.
    // The numbers come from assets/characters/effects.yaml (edited in the character editor, tabs Explosions and
    // Wrecks); every value has the built-in default of the original game, so a missing file changes nothing.
    struct EffectsConfig
    {
        // One wreck family: first composite and the number of burial stages (composite + stage is drawn)
        struct Wreck
        {
            int composite {1764};
            int stages {5};
        };

        // --- Explosions ---
        float frameTime {0.065f};                        // seconds per frame of an explosion animation
        std::vector<int> vehicleMain {11, 12, 13, 14};   // blast of a dying vehicle, picked by the unit id
        std::vector<int> vehicleSecond {11, 12, 13, 14}; // the ammunition / fuel a moment later, picked by id + 2
        float vehicleSecondDelayMin {0.18f}, vehicleSecondDelayMax {0.3f};
        float vehicleSecondSpreadX {6.0f}, vehicleSecondSpreadYMin {-5.0f}, vehicleSecondSpreadYMax {3.0f};
        int buildingSmallMain {37}; // footprint up to 2x2
        int buildingPowerMain {35}; // power plants, tech centers (raw types 2, 4, 12)
        int buildingLargeMain {32};
        int buildingExtraMin {2}, buildingExtraMax {7}; // secondary blasts: footprint area / 2, clamped to this range
        std::vector<int> buildingExtraAnims {11, 12, 13, 14};
        int buildingBigAnim {37}; // every `buildingBigEvery`-th extra blast is this big one
        int buildingBigEvery {3};
        float buildingDelayStart {0.08f}, buildingDelayStep {0.07f}, buildingDelayJitter {0.05f};

        // --- Wrecks ---
        float burialSeconds {120.0f}; // how long a wreck stays; it sinks stage by stage
        Wreck vehicle {1764, 5};
        Wreck light {1759, 5};
        Wreck heavy {1769, 4};
        std::vector<int> lightArchetypes {6};
        std::vector<int> heavyArchetypes {7, 10, 11, 12, 13, 15, 17, 18};
        Wreck buildingSmall {1741, 6};  // 2x2 rubble
        Wreck buildingMedium {1747, 6}; // 3x3
        Wreck buildingLarge {1753, 6};  // 4x4 and bigger

        // Wreck of a vehicle archetype
        Wreck wreckOfVehicle(int archetype) const;
        // Rubble of a building of the given footprint
        Wreck wreckOfBuilding(int width, int height) const;

        // Reads effects.yaml text over the current values; false when it cannot be parsed
        bool loadText(const std::string &yamlText);
        // The shared instance (read from assets/characters/effects.yaml on first use)
        static EffectsConfig &get();
        static void reload();
        // Replaces the shared instance by the defaults overlaid with this text (the editor shows changes at once)
        static void apply(const std::string &yamlText);
    };

    // Where the picture of an explosion / impact animation sits relative to the point it is spawned at
    void explosionCentering(int animId, float &outOx, float &outOy);

} // namespace OpenArtOfWar
