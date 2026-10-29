#include "core/Utility.hpp"
#include "core/DetMath.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace OpenArtOfWar::Utility
{

    Random &rng()
    {
        static Random s_rng;
        return s_rng;
    }

    const float kDir8X[8] = {1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f, 0.0f, 0.7071f};
    const float kDir8Y[8] = {0.0f, 0.7071f, 1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f};

    int dir8FromVector(float dx, float dy)
    {
        constexpr float kPi = static_cast<float>(M_PI);
        float a = DetMath::atan2(dy, dx);
        if(a < 0.0f)
            a += 2.0f * kPi;
        return static_cast<int>(std::floor((a + kPi / 8.0f) / (kPi / 4.0f))) % 8;
    }

    int dirFromNorth(float dx, float dy, int steps)
    {
        constexpr float kPi = static_cast<float>(M_PI);
        float a = DetMath::atan2(dx, -dy);
        if(a < 0.0f)
            a += 2.0f * kPi;
        const float sector = 2.0f * kPi / static_cast<float>(steps);
        return static_cast<int>(std::floor((a + sector * 0.5f) / sector)) % steps;
    }

    int turnStep8(int from, int to)
    {
        from = ((from % 8) + 8) % 8;
        to = ((to % 8) + 8) % 8;
        // Turning around (180 degrees), a vehicle always swings through "down" (south, facing 2): moving left
        // and ordered right it first turns down, then right - the same for every vehicle
        if(dir8Distance(from, to) == 4)
        {
            for(int k = 1; k <= 3; ++k)
            {
                if((from + k) % 8 == 2)
                    return 1;
                if((from - k + 8) % 8 == 2)
                    return -1;
            }
        }
        // Otherwise the short way round (original J2ME turn table indexed by the raw difference -7..7)
        static constexpr int kTurn[15] = {1, 1, 1, -1, -1, -1, -1, 0, 1, 1, 1, 1, -1, -1, -1};
        return kTurn[7 + (to - from)];
    }

    int dir8Distance(int a, int b)
    {
        const int d = ((a - b) % 8 + 8) % 8;
        return std::min(d, 8 - d);
    }

    bool isInfantryArchetype(int archetypeId)
    {
        return archetypeId >= 0 && archetypeId < 31 && (16447 & (1 << archetypeId)) != 0;
    }

    namespace
    {
        constexpr Faction R = Faction::Resistance;
        constexpr Faction C = Faction::Confederation;
        const ArchetypeInfo kArchetypes[kArchetypeCount] = {
            {0, "Rifleman", R, UnitType::ResistanceRifleman, -1},
            {1, "Rifleman", C, UnitType::ConfederationRifleman, -1},
            {2, "Grenadier", R, UnitType::Grenadier, -1},
            {3, "Heavy Assault", C, UnitType::HeavyAssault, -1},
            {4, "Sniper", R, UnitType::Sniper, -1},
            {5, "Flamethrower", C, UnitType::Flamethrower, -1},
            {6, "Coyote", R, UnitType::Coyote, -1},
            {7, "TM-40 Fortress", C, UnitType::Fortress, -1},
            {8, "Armadillo", R, UnitType::Armadillo, -1},
            {9, "T-21 Hammer", C, UnitType::Hammer, -1},
            {10, "Rhino", R, UnitType::Rhino, -1},
            {11, "T-22 Zeus", C, UnitType::Zeus, -1},
            {12, "Porcupine", R, UnitType::Porcupine, -1},
            {13, "Grad MLRS", C, UnitType::Grad, -1},
            {14, "Sniper (prone)", R, UnitType::Sniper, 4},
            {15, "Rhino (siege)", R, UnitType::Rhino, 10},
            {16, "Grad (deployed)", C, UnitType::Grad, 13},
            {17, "T-22 Zeus (base)", C, UnitType::Zeus, 11},
            {18, "TM-40 Fortress (base)", C, UnitType::Fortress, 7},
        };
        const std::vector<BuildingKindInfo> kBuildingKinds = {
            {0, "Command Center", C, UnitType::Headquarters},
            {1, "Power Generator", C, UnitType::PowerPlant},
            {2, "Infantry Center", C, UnitType::Barracks},
            {3, "Vehicle Factory", C, UnitType::Factory},
            {4, "Tech Center", C, UnitType::ResearchLab},
            {5, "Bunker", C, UnitType::Bunker},
            {6, "Radar Locator", C, UnitType::Radar},
            {7, "Missile Turret", C, UnitType::Tower},
            {8, "Headquarters", R, UnitType::Headquarters},
            {9, "Power Plant", R, UnitType::PowerPlant},
            {10, "Barracks", R, UnitType::Barracks},
            {11, "Factory", R, UnitType::Factory},
            {12, "Research Lab", R, UnitType::ResearchLab},
            {13, "Bunker", R, UnitType::Bunker},
            {14, "Guard Tower", R, UnitType::Tower},
            {15, "Concrete Wall", C, UnitType::Wall},
            {25, "Wall", R, UnitType::Wall},
        };
    } // namespace

    const ArchetypeInfo *archetypeInfo(int archetypeId)
    {
        return (archetypeId >= 0 && archetypeId < kArchetypeCount) ? &kArchetypes[archetypeId] : nullptr;
    }

    const char *unitClassName(int archetypeId)
    {
        return isInfantryArchetype(archetypeId) ? "infantry" : "vehicle";
    }

    const char *producedIn(int archetypeId)
    {
        return isInfantryArchetype(archetypeId) ? "barracks" : "factory";
    }

    float defaultTurnStepDelay(int archetypeId)
    {
        if(isInfantryArchetype(archetypeId))
            return 0.0f;
        switch(archetypeId)
        {
            case 6:
            case 8:
            case 10:
                return 0.08f; // light / medium hulls (2 ticks per step)
            case 7:
            case 9:
            case 11:
            case 12:
            case 17:
            case 18:
                return 0.16f; // heavy hulls (4 ticks)
            case 13:
                return 0.24f; // MLRS (6 ticks)
            default:
                return 0.10f;
        }
    }

    const BuildingKindInfo *buildingKindInfo(int buildingIndex)
    {
        for(const auto &k : kBuildingKinds)
        {
            if(k.index == buildingIndex)
                return &k;
        }
        return nullptr;
    }

    const std::vector<BuildingKindInfo> &buildingKinds()
    {
        return kBuildingKinds;
    }

    const char *unitTypeName(UnitType type)
    {
        switch(type)
        {
            case UnitType::ResistanceRifleman:
                return "ResistanceRifleman";
            case UnitType::Grenadier:
                return "Grenadier";
            case UnitType::Sniper:
                return "Sniper";
            case UnitType::Coyote:
                return "Coyote";
            case UnitType::Armadillo:
                return "Armadillo";
            case UnitType::Rhino:
                return "Rhino";
            case UnitType::Porcupine:
                return "Porcupine";
            case UnitType::ConfederationRifleman:
                return "ConfederationRifleman";
            case UnitType::HeavyAssault:
                return "HeavyAssault";
            case UnitType::Flamethrower:
                return "Flamethrower";
            case UnitType::Fortress:
                return "Fortress";
            case UnitType::Hammer:
                return "Hammer";
            case UnitType::Zeus:
                return "Zeus";
            case UnitType::Grad:
                return "Grad";
            case UnitType::Headquarters:
                return "Headquarters";
            case UnitType::PowerPlant:
                return "PowerPlant";
            case UnitType::Barracks:
                return "Barracks";
            case UnitType::Factory:
                return "Factory";
            case UnitType::ResearchLab:
                return "ResearchLab";
            case UnitType::Tower:
                return "Tower";
            case UnitType::Bunker:
                return "Bunker";
            case UnitType::Wall:
                return "Wall";
            case UnitType::Radar:
                return "Radar";
            default:
                return "Unknown";
        }
    }

    Faction parseFaction(std::string_view text, Faction fallback)
    {
        const std::string s = toLower(text);
        if(s == "resistance" || s == "res" || s == "red")
            return Faction::Resistance;
        if(s == "confederation" || s == "confederacy" || s == "conf" || s == "blue")
            return Faction::Confederation;
        if(s == "neutral" || s == "green")
            return Faction::Neutral;
        return fallback;
    }

    const char *factionName(Faction faction)
    {
        switch(faction)
        {
            case Faction::Resistance:
                return "Resistance";
            case Faction::Confederation:
                return "Confederation";
            default:
                return "Neutral";
        }
    }

    const char *factionKey(Faction faction)
    {
        switch(faction)
        {
            case Faction::Resistance:
                return "resistance";
            case Faction::Confederation:
                return "confederation";
            default:
                return "neutral";
        }
    }

    std::string toLower(std::string_view text)
    {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

} // namespace OpenArtOfWar::Utility
