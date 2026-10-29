#pragma once

#include "core/Config.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace OpenArtOfWar::Utility
{

    // ---------------------------------------------------------------------------
    // Random numbers: one fast xorshift32 generator for gameplay noise (shot spread, damage rolls,
    // death variants, AI cooldown jitter). Not for anything security related.
    // ---------------------------------------------------------------------------
    class Random
    {
    public:
        using result_type = uint32_t;
        static constexpr result_type min()
        {
            return 0;
        }
        static constexpr result_type max()
        {
            return UINT32_MAX;
        }

        explicit Random(uint32_t seed = 0x9e3779b9u)
        {
            reseed(seed);
        }

        void reseed(uint32_t seed)
        {
            m_state = seed ? seed : 0x9e3779b9u;
        }

        result_type next()
        {
            m_state ^= m_state << 13;
            m_state ^= m_state >> 17;
            m_state ^= m_state << 5;
            return m_state;
        }

        // [0, 1)
        float unit()
        {
            return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f);
        }
        // [-1, 1]
        float signedUnit()
        {
            return unit() * 2.0f - 1.0f;
        }
        // [lo, hi] inclusive
        int range(int lo, int hi)
        {
            if(hi <= lo)
                return lo;
            const uint64_t span = static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo) + 1u;
            return lo + static_cast<int>((static_cast<uint64_t>(next()) * span) >> 32);
        }
        // [lo, hi]
        float range(float lo, float hi)
        {
            return hi <= lo ? lo : lo + (hi - lo) * unit();
        }

        result_type operator()()
        {
            return next();
        }

    private:
        result_type m_state {0x9e3779b9u};
    };

    // The shared gameplay generator
    Random &rng();

    inline int randomInt(int lo, int hi)
    {
        return rng().range(lo, hi);
    }
    inline float randomFloat(float lo, float hi)
    {
        return rng().range(lo, hi);
    }
    inline float randomSigned()
    {
        return rng().signedUnit();
    }

    template <typename T>
    const T &pick(const std::vector<T> &items)
    {
        return items[static_cast<size_t>(randomInt(0, static_cast<int>(items.size()) - 1))];
    }

    // ---------------------------------------------------------------------------
    // Directions. Facing 0..7 runs clockwise on screen starting east: 0 E, 1 SE, 2 S, 3 SW, 4 W, 5 NW, 6 N, 7 NE.
    // ---------------------------------------------------------------------------
    extern const float kDir8X[8];
    extern const float kDir8Y[8];

    // Facing (0..7) of a screen vector (y down)
    int dir8FromVector(float dx, float dy);
    // Sprite direction 0..steps-1 counted clockwise from north (projectile sprite sets with 16 / 32 frames)
    int dirFromNorth(float dx, float dy, int steps);
    // One rotation step (-1, 0, +1) from one facing toward another: the short way round, and a half turn
    // (180 degrees) always swings through "down" (south)
    int turnStep8(int from, int to);
    // Number of 45-degree steps between two facings (0..4)
    int dir8Distance(int a, int b);

    // ---------------------------------------------------------------------------
    // Unit archetypes and building kinds: everything that follows from the id alone. Character files only
    // name the id (archetype_id / building_index); faction, class, internal type, where it is produced, etc.
    // come from here so they can never contradict each other.
    // ---------------------------------------------------------------------------
    // Infantry archetypes: 0..5 (both factions' soldiers) and 14 (prone sniper)
    bool isInfantryArchetype(int archetypeId);

    struct ArchetypeInfo
    {
        int id;
        const char *name; // sprite set name (gettext msgid)
        Faction faction;
        UnitType type;  // internal unit type
        int baseFormId; // for siege / baseline forms: the archetype they belong to, otherwise -1
    };
    constexpr int kArchetypeCount = 19;
    const ArchetypeInfo *archetypeInfo(int archetypeId); // nullptr for an unknown id
    const char *unitClassName(int archetypeId);          // "infantry" / "vehicle"
    const char *producedIn(int archetypeId);             // "barracks" / "factory"
    float defaultTurnStepDelay(int archetypeId);         // seconds per 45-degree hull turn (0 for infantry)

    struct BuildingKindInfo
    {
        int index;
        const char *name; // gettext msgid
        Faction faction;
        UnitType type;
    };
    const BuildingKindInfo *buildingKindInfo(int buildingIndex); // nullptr for an unknown index
    const std::vector<BuildingKindInfo> &buildingKinds();
    const char *unitTypeName(UnitType type);

    // ---------------------------------------------------------------------------
    // Factions. Data files spell them "Resistance" and "Confederation"; parsing accepts the usual
    // variants in any letter case (resistance / red / res, confederation / confederacy / blue / conf).
    // ---------------------------------------------------------------------------
    Faction parseFaction(std::string_view text, Faction fallback = Faction::Resistance);
    const char *factionName(Faction faction); // "Resistance", "Confederation", "Neutral"
    const char *factionKey(Faction faction);  // "resistance", "confederation", "neutral"

    // ---------------------------------------------------------------------------
    // Strings
    // ---------------------------------------------------------------------------
    std::string toLower(std::string_view text);

} // namespace OpenArtOfWar::Utility
