#pragma once

#include "core/Config.hpp"
#include "game/Players.hpp"

#include <string>

namespace OpenArtOfWar
{

    class EntityManager;
    class Unit;

    // What a technology does. Each faction has its own catalog (24 technologies, as in the original game); the
    // same effect can appear in both of them.
    enum class ResearchEffect : int
    {
        InfantryArmor = 0,    // value: armor points of all infantry
        InfantryRegen,        // infantry regenerates health 3 times faster
        FireRate,             // value: +% rate of fire of archetypes archA / archB
        Damage,               // value: +% damage of archA / archB
        InfantrySpeed,        // value: +speed points of all infantry
        VehicleSpeed,         // value: +speed points of all vehicles
        VisionRange,          // value: +tiles of vision and range, value2: +% damage of archA
        RangePercent,         // value: +% range, value2: +% damage of archA
        VehicleArmor,         // value: armor points of all vehicles
        RepairSpeed,          // vehicles repair 3 times faster
        Reload,               // value: -% reload time of archA / archB
        UnlockUnit,           // archA can be trained
        FortressFlamethrower, // the Fortress gets its heavy flamethrower
        SiegeMode,            // archA (Rhino) may switch to siege mode
        RocketTurret,         // the Zeus gets a rocket launcher turret
        PowerBonus,           // value: +% power output
        BuildingArmor,        // value: +% armor of all structures
        WallStrength,         // value: +% durability of walls
        UnlockRadar,          // radar locators can be built
        TrainTime,            // value: -% infantry training time
        VehicleTime,          // value: -% vehicle production time
        Income,               // value: +% income
        KillReward,           // value: +% reward for a destroyed enemy unit
        BuildRadius,          // value: +% base building radius
        Mine,                 // value: 0 = Frog mine, 1 = Monitor Lizard mine
        Count
    };

    /**
     * @brief One technology of a faction's research tree (Tech Center / Laboratory).
     *
     * The AI walks the catalog from the first entry to the last, the player picks freely (respecting `needs`).
     */
    struct ResearchDef
    {
        int compId;       // cameo composite for the sidebar
        const char *name; // gettext msgid
        int cost;
        const char *desc; // gettext msgid
        ResearchEffect effect;
        int value;
        int value2;
        int archA; // archetypes the effect is about (siege / baseline forms count as their base), -1 = none
        int archB;
        int needs;       // index of the technology that has to be researched first, -1 = none
        bool playerOnly; // effect only makes sense for the player (e.g. HQ build radius)
    };

    constexpr int kResearchCount = 24; // technologies per faction

    // YAML name of an effect (assets/characters/research_<faction>.yaml): `infantry_armor`, `fire_rate`, ...
    const char *researchEffectName(ResearchEffect effect);
    bool parseResearchEffect(const std::string &name, ResearchEffect &out);
    // Cameo composites the research icon can be picked from
    constexpr int kResearchIconFirst = 2064;
    constexpr int kResearchIconLast = 2132;
    // File of the faction's tree, relative to the asset root
    const char *researchFile(Faction faction);

    // The catalog is read from the faction's research YAML; entries it lacks keep the built-in values
    const ResearchDef &researchDef(Faction faction, int index);

    // Research is only possible while the player owns a finished Tech Center / Laboratory
    bool hasResearchBuilding(PlayerId player, const EntityManager &entities);

    // Completed technologies per player (reset when a mission starts). The technologies on offer are those
    // of the player's faction (researchDef(Players::factionOf(player), i)).
    void resetResearch();
    void resetResearch(PlayerId player);
    bool isResearched(PlayerId player, int index);
    // The technology's own requirement is done
    bool isResearchAvailable(PlayerId player, int index);

    // Marks the technology done and applies it to every unit of the player plus its global effects
    void completeResearch(PlayerId player, int index, EntityManager &entities);

    // Gives a freshly trained unit every technology its owner has completed
    void applyCompletedResearch(Unit &unit);
    // Firing range of a unit form after the completed range technologies (Hornet-10, Grad-10, ...)
    float researchedAttackRange(PlayerId player, int archetypeId, float range);

    // Player-wide numbers of the completed technologies: the sum of `value` of every one with that effect
    int researchTotal(PlayerId player, ResearchEffect effect);
    // A completed technology with that effect exists
    bool isEffectResearched(PlayerId player, ResearchEffect effect);
    // Units that need a technology (flamethrower, sniper, Grad, Porcupine) can only be trained once it is done
    bool isUnitUnlocked(PlayerId player, int archetype);
    // Index of the technology that unlocks the unit, -1 when it needs none
    int unitUnlockResearch(Faction faction, int archetype);
    bool isRadarUnlocked(PlayerId player);

    // Armor: damage taken by `target` after the armor of its owner's technologies (at least 1)
    int reduceDamageByArmor(int damage, const class Entity &target);
    // A wall of `player` takes this much of `damage`
    int reduceWallDamage(int damage, PlayerId player);

} // namespace OpenArtOfWar
