#include "graphics/RenderUtility.hpp"
#include "game/AIController.hpp"
#include "AIControllerInternal.hpp"
#include "game/BuildLimits.hpp"
#include "core/DetMath.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/TileMap.hpp"
#include "game/Pathfinder.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <sstream>
#include <cmath>

namespace OpenArtOfWar
{

// Wave plans use the Confederation archetypes (odd ids); each Resistance unit with the same role
// has the even id right below (rifleman 0, grenadier 2, sniper 4, Coyote 6, Armadillo 8, Rhino 10, Porcupine 12)
int SideArchetype(int confederationArch, Faction side)
{
    return side == Faction::Confederation ? confederationArch : confederationArch - 1;
}

bool IsVehicleArchetype(int arch)
{
    return arch == kFortress || arch == kHammer || arch == kZeus || arch == kGrad;
}

int FallbackArchetypeCost(int arch)
{
    switch (arch)
    {
        case kRifleman:
            return 80;
        case kHeavyAssault:
            return 120;
        case kFlamethrower:
            return 150;
        case kFortress:
            return 220;
        case kHammer:
            return 350;
        case kZeus:
            return 420;
        case kGrad:
            return 400;
        default:
            return 80;
    }
}

const char *TierName(AIWaveTier tier)
{
    switch (tier)
    {
        case AIWaveTier::Scout:
            return "Scout";
        case AIWaveTier::Vanguard:
            return "Vanguard";
        case AIWaveTier::Assault:
            return "Assault";
        case AIWaveTier::Siege:
            return "Siege";
    }
    return "?";
}

void ParseTactics(const YAML::Node &node, AITacticsConfig &t)
{
    if (!node || !node.IsMap())
        return;
    auto f = [&](const char *key, float &out) {
        if (node[key])
            out = node[key].as<float>();
    };
    auto i = [&](const char *key, int &out) {
        if (node[key])
            out = node[key].as<int>();
    };
    auto b = [&](const char *key, bool &out) {
        if (node[key])
            out = node[key].as<bool>();
    };
    f("leash_tiles", t.leashTiles);
    f("leash_per_unit", t.leashPerUnit);
    f("corridor_leash_factor", t.corridorLeashFactor);
    b("wait_at_chokepoints", t.waitAtChokepoints);
    f("pre_choke_hold_seconds", t.preChokeHoldSeconds);
    b("sync_to_slowest", t.syncToSlowest);
    b("armor_front", t.armorFront);
    b("melee_ranged_pairs", t.meleeRangedPairs);
    i("wave_base_size", t.waveBaseSize);
    i("wave_growth", t.waveGrowth);
    i("wave_max_size", t.waveMaxSize);
    f("wave_interval_seconds", t.waveIntervalSeconds);
    i("max_wave_tier", t.maxWaveTier);
    i("tier_vanguard_wave", t.vanguardWave);
    i("tier_assault_wave", t.assaultWave);
    i("tier_siege_wave", t.siegeWave);
    f("vehicle_wait_seconds", t.vehicleWaitSeconds);
    f("research_seconds", t.researchSeconds);
    f("straggler_timeout_seconds", t.stragglerTimeoutSeconds);
    i("march_stride_tiles", t.marchStrideTiles);
    i("max_attack_groups", t.maxAttackGroups);
    i("dispatch_group_size", t.dispatchGroupSize);
    i("credits_per_extra_group", t.creditsPerExtraGroup);
    i("late_rifleman_percent", t.lateRiflemanPercent);
    b("build_defenses", t.buildDefenses);
    f("defense_first_wave_seconds", t.defenseFirstWaveSeconds);
    f("defense_wave_gap_seconds", t.defenseWaveGapSeconds);
    i("defense_squad_percent", t.defenseSquadPercent);
    f("defense_time_scale", t.defenseTimeScale);
    t.defenseSquadPercent = std::clamp(t.defenseSquadPercent, 10, 100);
    t.maxAttackGroups = std::clamp(t.maxAttackGroups, 1, 5);
    t.dispatchGroupSize = std::clamp(t.dispatchGroupSize, 1, 12);
}

int FactionBuildingIndex(AIDefenseSite::Kind kind, Faction side)
{
    const bool conf = (side == Faction::Confederation);
    switch (kind)
    {
        case AIDefenseSite::Tower:
            return conf ? 7 : 14;
        case AIDefenseSite::Bunker:
            return conf ? 5 : 13;
        case AIDefenseSite::Lab:
            return conf ? 4 : 12;
        case AIDefenseSite::PowerPlant:
            return conf ? 1 : 9;
        case AIDefenseSite::Barracks:
            return conf ? 2 : 10;
        case AIDefenseSite::Factory:
            return conf ? 3 : 11;
        case AIDefenseSite::Headquarters:
            return conf ? 0 : 8;
        case AIDefenseSite::Wall:
            return -1;
    }
    return -1;
}

// What a structure costs the computer: the cost of the building in its character file, the same price the player
// pays (no handicap and no bonus of a difficulty in prices). The old fixed numbers only stand in when there is no
// character file for it.
int DefenseCost(AIDefenseSite::Kind kind, const EntityManager &entities, Faction side)
{
    const int wallIndex = side == Faction::Confederation ? 15 : 25;
    const int index = kind == AIDefenseSite::Wall ? wallIndex : FactionBuildingIndex(kind, side);
    if (const BuildingConfig *cfg = entities.GetBuildingConfig(index))
        return cfg->creditCost;
    switch (kind)
    {
        case AIDefenseSite::Tower:
            return 250;
        case AIDefenseSite::Bunker:
            return 350;
        case AIDefenseSite::Lab:
            return 450;
        case AIDefenseSite::PowerPlant:
            return 200;
        case AIDefenseSite::Barracks:
            return 350;
        case AIDefenseSite::Factory:
            return 500;
        case AIDefenseSite::Wall:
            return 25;
        case AIDefenseSite::Headquarters:
            return 1500;
    }
    return 250;
}

} // namespace OpenArtOfWar
