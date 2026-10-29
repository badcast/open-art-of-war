#pragma once

#include <cstdint>
#include <string>

namespace OpenArtOfWar
{

    // Virtual J2ME resolution
    constexpr int VIRTUAL_SCREEN_WIDTH = 240;
    constexpr int VIRTUAL_SCREEN_HEIGHT = 320;

    // Default window resolution
    constexpr int DEFAULT_WINDOW_WIDTH = 1280;
    constexpr int DEFAULT_WINDOW_HEIGHT = 720;

    // Tile grid size
    constexpr int TILE_WIDTH = 30;    // 30 pixels per tile width
    constexpr int TILE_HEIGHT = 20;   // 20 pixels per tile height
    constexpr int TILE_SIZE = 30;     // legacy alias
    constexpr int MAX_MAP_SIZE = 128; // 128x128 max map dimension
    constexpr int MAX_UNITS = 512;    // max entities per match (buildings + units + waves)

    // Timesteps
    constexpr double SIMULATION_TICK_RATE = 20.0;                      // 20 TPS
    constexpr double SIMULATION_TIMESTEP = 1.0 / SIMULATION_TICK_RATE; // 50ms
    constexpr double MENU_TICK_RATE = 10.0;                            // 10 TPS
    constexpr double MENU_TIMESTEP = 1.0 / MENU_TICK_RATE;             // 100ms

    // Factions
    enum class Faction : uint8_t
    {
        Resistance = 0,    // Red (Сопротивление — партизаны Перу, красная палитра Sheet 9 / Sheets 12..13)
        Confederation = 1, // Blue (Конфедерация — регулярная армия, синяя палитра Sheet 8 / Sheets 10..11)
        Neutral = 2        // Green / Neutral
    };

    // Unit FSM Action States
    enum class ActionState : uint8_t
    {
        Idle = 0,
        Move = 1,
        Attack = 2,
        Die = 3,
        Build = 4,
        Harvest = 5,
        Transform = 6 // Siege mode transformation (Sniper 4<->14, Rhino 10<->15, Grad 13<->16)
    };

    // Unit Archetypes
    enum class UnitType : uint8_t
    {
        // Units: one value per unit (numbers kept from the original data). Siege / baseline forms share their unit's type.
        // Resistance
        ResistanceRifleman = 0, // archetype 0
        Grenadier = 2,          // archetype 2
        Sniper = 3,             // archetype 4 (prone: 14)
        Coyote = 4,             // archetype 6
        Armadillo = 5,          // archetype 8
        Rhino = 6,              // archetype 10 (siege: 15)
        Porcupine = 7,          // archetype 12
        // Confederation
        ConfederationRifleman = 8, // archetype 1
        HeavyAssault = 9,          // archetype 3
        Flamethrower = 10,         // archetype 5
        Fortress = 12,             // archetype 7 (baseline: 18)
        Hammer = 13,               // archetype 9
        Zeus = 14,                 // archetype 11 (baseline: 17)
        Grad = 15,                 // archetype 13 (deployed: 16)

        // Structures (16+)
        Headquarters = 16,
        PowerPlant = 17,
        Barracks = 18,
        Factory = 19,
        ResearchLab = 20,
        Tower = 21,  // guard tower / missile turret
        Bunker = 22, // machine-gun bunker
        Wall = 23,
        Radar = 24,
        Unknown = 255
    };

    // Application State Machine
    enum class AppState
    {
        SplashLogo,   // Studio logo (/l1)
        SplashTitle,  // Open Art Of War splash art (/l2)
        MainMenu,     // Authentic Open Art Of War main menu (Menu 0)
        MenuHelp,     // Authentic help & tutorial (/0/t0)
        MenuAbout,    // Authentic game credits (/0/t0)
        MenuSettings, // Sound & music options
        MenuNetwork,  // Network game: browser and game room
        Briefing,
        CampaignPlaying,
        SkirmishPlaying,
        Paused,
        Victory,
        Defeat
    };

    // J2ME Anchor flags
    namespace Anchor
    {
        constexpr int TOP = 0x01;
        constexpr int BOTTOM = 0x02;
        constexpr int LEFT = 0x04;
        constexpr int RIGHT = 0x08;
        constexpr int HCENTER = 0x10;
        constexpr int VCENTER = 0x20;
    } // namespace Anchor

} // namespace OpenArtOfWar
