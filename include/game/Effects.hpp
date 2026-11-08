#pragma once

#include <string>
#include <vector>

namespace OpenArtOfWar
{

// Looks of destruction: the explosions of dying units and buildings and the wrecks they leave behind.
// The values come from assets/characters/effects.yaml (edited in the character editor, tabs Explosions and
// Wrecks); every value has the built-in default of the original game, so a missing file changes nothing.
// Animations and wrecks are sprites, written by name ("fx_explosions.blast_0", "fx_wreck_vehicle.stages") and kept
// here as handles of the sprite tables (-1: none).
struct EffectsConfig
{
    // One wreck family: first composite and the number of burial stages (composite + stage is drawn)
    struct Wreck
    {
        int composite {-1};
        int stages {1};
    };

    EffectsConfig(); // the defaults

    // --- Explosions ---
    float frameTime {0.065f};       // seconds per frame of an explosion animation
    std::vector<int> vehicleMain;   // blast of a dying vehicle, picked by the unit id
    std::vector<int> vehicleSecond; // the ammunition / fuel a moment later, picked by id + 2
    float vehicleSecondDelayMin {0.18f}, vehicleSecondDelayMax {0.3f};
    float vehicleSecondSpreadX {6.0f}, vehicleSecondSpreadYMin {-5.0f}, vehicleSecondSpreadYMax {3.0f};
    int buildingSmallMain {-1}; // footprint up to 2x2
    int buildingPowerMain {-1}; // power plants, tech centers (raw types 2, 4, 12)
    int buildingLargeMain {-1};
    int buildingExtraMin {2}, buildingExtraMax {7}; // secondary blasts: footprint area / 2, clamped to this range
    std::vector<int> buildingExtraAnims;
    int buildingBigAnim {-1}; // every `buildingBigEvery`-th extra blast is this big one
    int buildingBigEvery {3};
    float buildingDelayStart {0.08f}, buildingDelayStep {0.07f}, buildingDelayJitter {0.05f};

    // --- Wrecks ---
    float burialSeconds {120.0f}; // how long a wreck stays; it sinks stage by stage
    float fadeSeconds {3.0f};     // then it fades out (linear transparency) over this long and is gone
    Wreck vehicle;
    Wreck light;
    Wreck heavy;
    std::vector<int> lightArchetypes {6};
    std::vector<int> heavyArchetypes {7, 10, 11, 12, 13, 15, 17, 18};
    Wreck buildingSmall;  // 2x2 rubble
    Wreck buildingMedium; // 3x3
    Wreck buildingLarge;  // 4x4 and bigger

    // Wreck of a vehicle archetype
    Wreck WreckOfVehicle(int archetype) const;
    // Rubble of a building of the given footprint
    Wreck WreckOfBuilding(int width, int height) const;

    // Reads effects.yaml text over the current values; false when it cannot be parsed
    bool LoadText(const std::string &yamlText);
    // The shared instance (read from assets/characters/effects.yaml on first use)
    static EffectsConfig &Get();
    static void reload();
    // Replaces the shared instance by the defaults overlaid with this text (the editor shows changes at once)
    static void Apply(const std::string &yamlText);
};

// Where the picture of an explosion / impact animation sits relative to the point it is spawned at
void ExplosionCentering(int animId, float &outOx, float &outOy);

} // namespace OpenArtOfWar
