#include "game/Capabilities.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/Utility.hpp"
#include "game/EntityManager.hpp"
#include "game/Research.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <iostream>
#include <set>

namespace OpenArtOfWar::Capabilities
{

namespace
{

constexpr int kSlots = 64; // building indexes / archetypes looked at

// What one level allows one faction, as written in the file
struct FactionList
{
    std::set<std::string> builds;
    std::set<std::string> units;
    std::set<int> researches;
};

struct Level
{
    std::string name;
    std::array<FactionList, 2> factions; // by Faction (Resistance 0, Confederation 1)
};

struct Table
{
    std::vector<std::string> names;
    std::vector<Level> levels;
    std::string defaultLevel;
};

// What one player may use, resolved by index
struct Allowed
{
    bool restricted {false};
    std::string level;
    std::bitset<kSlots> buildings;
    std::bitset<kSlots> units;
    std::bitset<kResearchCount> researches;
};

Table &Storage()
{
    static Table t;
    return t;
}

bool &Loaded()
{
    static bool loaded = false;
    return loaded;
}

const Table &Current()
{
    if (!Loaded())
        Reload(); // the file is read on first use
    return Storage();
}

std::array<Allowed, kMaxPlayers> &PlayerTable()
{
    static std::array<Allowed, kMaxPlayers> players;
    return players;
}

int FactionSlot(Faction f)
{
    return f == Faction::Confederation ? 1 : 0;
}

int BaseArchetype(int archetype)
{
    const Utility::ArchetypeInfo *info = Utility::archetypeInfo(archetype);
    return info && info->baseFormId >= 0 ? info->baseFormId : archetype;
}

// Walls are one building with many segment indexes: 15..24 Confederation, 25..29 Resistance
int CanonicalBuilding(int buildingIndex)
{
    if (buildingIndex >= 15 && buildingIndex <= 24)
        return 15;
    if (buildingIndex >= 25 && buildingIndex <= 29)
        return 25;
    return buildingIndex;
}

void ReadList(const YAML::Node &node, std::set<std::string> &out)
{
    if (node && node.IsSequence())
        for (const auto &v : node)
            out.insert(v.as<std::string>());
}

const Level *FindLevel(std::string_view name)
{
    for (const Level &l : Current().levels)
        if (l.name == name)
            return &l;
    return nullptr;
}

} // namespace

const std::vector<std::string> &Levels()
{
    return Current().names;
}

int LevelIndex(std::string_view name)
{
    const auto &names = Levels();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name)
            return static_cast<int>(i);
    return -1;
}

const std::string &DefaultLevel()
{
    return Current().defaultLevel;
}

std::string ResolveLevel(int matchLevel, int slotLevel, std::string_view preset)
{
    const auto &names = Levels();
    if (slotLevel >= 0 && slotLevel < static_cast<int>(names.size()))
        return names[static_cast<std::size_t>(slotLevel)];
    if (matchLevel >= 0 && matchLevel < static_cast<int>(names.size()))
        return names[static_cast<std::size_t>(matchLevel)];
    if (!preset.empty())
    {
        if (LevelIndex(preset) >= 0)
            return std::string(preset);
        std::cerr << "[Capabilities] the map names an unknown level '" << preset << "'" << std::endl;
    }
    return DefaultLevel();
}

void ApplyLevels(int matchLevel, std::string_view preset, const EntityManager &entities)
{
    ResetLevels();
    for (int p = 0; p < kMaxPlayers; ++p)
    {
        const auto id = static_cast<PlayerId>(p);
        if (Players::IsActive(id))
            SetPlayerLevel(id, ResolveLevel(matchLevel, Players::Slot(id).techLevel, preset), entities);
    }
}

void SetPlayerLevel(PlayerId player, std::string_view level, const EntityManager &entities)
{
    if (player >= kMaxPlayers)
        return;
    Allowed a;
    const Level *l = FindLevel(level);
    if (!l)
    {
        if (!level.empty())
            std::cerr << "[Capabilities] unknown level '" << level << "': nothing restricted" << std::endl;
        PlayerTable()[player] = a;
        return;
    }
    a.restricted = true;
    a.level = level;
    const Faction faction = Players::FactionOf(player);
    const FactionList &list = l->factions[static_cast<std::size_t>(FactionSlot(faction))];

    std::set<std::string> seen;
    for (int i = 0; i < kSlots; ++i)
    {
        const BuildingConfig *cfg = entities.GetBuildingConfig(i);
        if (cfg && !cfg->id.empty() && list.builds.count(cfg->id))
        {
            a.buildings.set(static_cast<std::size_t>(CanonicalBuilding(i)));
            seen.insert(cfg->id);
        }
        const UnitConfig *ucfg = entities.GetUnitConfig(i);
        if (ucfg && !ucfg->id.empty() && list.units.count(ucfg->id))
        {
            a.units.set(static_cast<std::size_t>(BaseArchetype(i)));
            seen.insert(ucfg->id);
        }
    }
    for (const auto *ids : {&list.builds, &list.units})
        for (const std::string &id : *ids)
            if (!seen.count(id))
                std::cerr << "[Capabilities] " << level << ": no building / unit '" << id << "'" << std::endl;

    // A research is out when it unlocks a forbidden unit, or (until nothing changes) when it needs one that is out
    for (int i = 0; i < kResearchCount; ++i)
    {
        const ResearchDef &def = researchDef(faction, i);
        if (list.researches.count(i)
            && !(
                def.effect == ResearchEffect::UnlockUnit && def.archA >= 0
                && !a.units.test(static_cast<std::size_t>(std::clamp(BaseArchetype(def.archA), 0, kSlots - 1)))))
            a.researches.set(static_cast<std::size_t>(i));
    }
    for (bool changed = true; changed;)
    {
        changed = false;
        for (int i = 0; i < kResearchCount; ++i)
        {
            const int needs = researchDef(faction, i).needs;
            if (a.researches.test(static_cast<std::size_t>(i)) && needs >= 0 && needs < kResearchCount
                && !a.researches.test(static_cast<std::size_t>(needs)))
            {
                a.researches.reset(static_cast<std::size_t>(i));
                changed = true;
            }
        }
    }
    PlayerTable()[player] = std::move(a);
}

void ResetLevels()
{
    PlayerTable().fill(Allowed {});
}

const std::string &PlayerLevel(PlayerId player)
{
    static const std::string none;
    return player < kMaxPlayers ? PlayerTable()[player].level : none;
}

bool AllowsBuilding(PlayerId player, int buildingIndex)
{
    if (player >= kMaxPlayers || !PlayerTable()[player].restricted)
        return true;
    const int i = CanonicalBuilding(buildingIndex);
    return i >= 0 && i < kSlots && PlayerTable()[player].buildings.test(static_cast<std::size_t>(i));
}

bool AllowsUnit(PlayerId player, int archetype)
{
    if (player >= kMaxPlayers || !PlayerTable()[player].restricted)
        return true;
    const int i = BaseArchetype(archetype);
    return i >= 0 && i < kSlots && PlayerTable()[player].units.test(static_cast<std::size_t>(i));
}

bool AllowsResearch(PlayerId player, int index)
{
    if (player >= kMaxPlayers || !PlayerTable()[player].restricted)
        return true;
    return index >= 0 && index < kResearchCount
           && PlayerTable()[player].researches.test(static_cast<std::size_t>(index));
}

bool LoadText(std::string_view yamlText)
{
    Table t;
    try
    {
        const YAML::Node root = YAML::Load(std::string(yamlText));
        if (!root || !root.IsMap())
            return false;
        for (const auto &entry : root)
        {
            const std::string key = entry.first.as<std::string>();
            if (key == "default")
            {
                t.defaultLevel = entry.second.as<std::string>();
                continue;
            }
            if (!entry.second.IsMap())
                continue;
            Level level;
            level.name = key;
            for (const auto &side : entry.second)
            {
                const std::string faction = side.first.as<std::string>();
                const int slot = faction == "confederation" ? 1 : faction == "resistance" ? 0 : -1;
                if (slot < 0)
                {
                    std::cerr << "[Capabilities] " << key << ": unknown faction '" << faction << "'" << std::endl;
                    continue;
                }
                FactionList &list = level.factions[static_cast<std::size_t>(slot)];
                ReadList(side.second["builds"], list.builds);
                ReadList(side.second["units"], list.units);
                if (const YAML::Node r = side.second["researches"]; r && r.IsSequence())
                {
                    for (const auto &v : r)
                    {
                        const int index = v.as<int>();
                        if (index < 0 || index >= kResearchCount)
                            std::cerr << "[Capabilities] " << key << ": no research " << index << std::endl;
                        else
                            list.researches.insert(index);
                    }
                }
            }
            t.names.push_back(key);
            t.levels.push_back(std::move(level));
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Capabilities] capabilities.yaml: " << e.what() << std::endl;
        return false;
    }
    if (!t.defaultLevel.empty() && !std::count(t.names.begin(), t.names.end(), t.defaultLevel))
    {
        std::cerr << "[Capabilities] default level '" << t.defaultLevel << "' does not exist" << std::endl;
        t.defaultLevel.clear();
    }
    if (t.defaultLevel.empty() && !t.names.empty())
        t.defaultLevel = t.names.back();
    Storage() = std::move(t);
    Loaded() = true;
    return true;
}

void Reload()
{
    Storage() = Table();
    Loaded() = true;
    std::string text;
    if (Assets::ReadText(LoaderResources::Capabilities(), text))
        LoadText(text);
}

} // namespace OpenArtOfWar::Capabilities
