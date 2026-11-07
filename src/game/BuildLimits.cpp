#include "game/BuildLimits.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "game/Building.hpp"
#include "game/EntityManager.hpp"
#include "game/TileMap.hpp"

#include <yaml-cpp/yaml.h>

#include <array>
#include <iostream>
#include <map>

namespace OpenArtOfWar::BuildLimits
{

namespace
{

constexpr int kKinds = static_cast<int>(Kind::Count);

struct Table
{
    std::array<int, kKinds> byKind;
    std::map<std::string, int> byId; // exact building ids
    Table()
    {
        byKind.fill(kUnlimited);
    }
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

int Normalized(int v)
{
    return v < 0 ? kUnlimited : v;
}

const std::string *IdOf(const EntityManager &entities, int buildingIndex)
{
    const BuildingConfig *cfg = entities.GetBuildingConfig(buildingIndex);
    return cfg && !cfg->id.empty() ? &cfg->id : nullptr;
}

// The limit's own id when the building has one in the file, else none (the kind counts)
const std::string *OwnId(const EntityManager &entities, int buildingIndex)
{
    const std::string *id = IdOf(entities, buildingIndex);
    return id && Current().byId.count(*id) ? id : nullptr;
}

} // namespace

Kind KindOf(int buildingIndex)
{
    switch (buildingIndex)
    {
        case 0: // Command Center
        case 8: // Headquarters
            return Kind::Headquarters;
        case 1: // Power Generator
        case 9: // Power Plant
            return Kind::Power;
        case 2:  // Infantry Center
        case 10: // Barracks
            return Kind::Barracks;
        case 3:  // Vehicle Factory
        case 11: // Factory
            return Kind::Factory;
        case 4:  // Tech Center
        case 12: // Research Lab
            return Kind::Research;
        case 6: // Radar Locator
            return Kind::Radar;
        case 7:  // Missile Turret
        case 14: // Guard Tower
            return Kind::Turret;
        case 5:  // Confederation Bunker
        case 13: // Resistance Bunker
            return Kind::Bunker;
        default:
            // 15..29: wall segments
            return buildingIndex >= 15 && buildingIndex <= 29 ? Kind::Wall : Kind::Other;
    }
}

const char *KeyOf(Kind kind)
{
    static const char *const kKeys[kKinds]
        = {"headquarters", "power", "barracks", "factory", "research", "radar", "turret", "bunker", "wall", "other"};
    const int i = static_cast<int>(kind);
    return i >= 0 && i < kKinds ? kKeys[i] : "other";
}

int LimitOf(const EntityManager &entities, int buildingIndex)
{
    const Table &t = Current();
    if (const std::string *id = OwnId(entities, buildingIndex))
        return t.byId.at(*id);
    return t.byKind[static_cast<std::size_t>(KindOf(buildingIndex))];
}

int CountOwned(const EntityManager &entities, const TileMap &map, PlayerId player, int buildingIndex)
{
    const Kind kind = KindOf(buildingIndex);
    if (kind == Kind::Wall)
    {
        int walls = 0;
        for (int y = 0; y < map.GetHeight(); ++y)
            for (int x = 0; x < map.GetWidth(); ++x)
                if (map.GetWall(x, y) > 0 && map.GetWallOwner(x, y) == player)
                    ++walls;
        return walls;
    }
    const std::string *id = OwnId(entities, buildingIndex);
    int count = 0;
    for (const Entity &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (!b || !b->alive || b->state == ActionState::Die || b->owner != player)
            continue;
        if (id)
        {
            const std::string *other = IdOf(entities, b->rawBuildingType);
            if (other && *other == *id)
                ++count;
        }
        else if (KindOf(b->rawBuildingType) == kind && !OwnId(entities, b->rawBuildingType))
        {
            ++count; // (one with its own limit counts only for that)
        }
    }
    return count;
}

bool CanBuildMore(const EntityManager &entities, const TileMap &map, PlayerId player, int buildingIndex)
{
    const int limit = LimitOf(entities, buildingIndex);
    if (limit < 0)
        return true;
    if (limit == 0)
        return false;
    return CountOwned(entities, map, player, buildingIndex) < limit;
}

bool LoadText(const std::string &yamlText)
{
    Table t;
    try
    {
        const YAML::Node root = YAML::Load(yamlText);
        if (!root || !root.IsMap())
            return false;
        if (const YAML::Node d = root["default"])
            t.byKind.fill(Normalized(d.as<int>()));
        if (const YAML::Node all = root["buildings"]; all && all.IsMap())
        {
            for (const auto &entry : all)
            {
                const std::string key = entry.first.as<std::string>();
                const int value = Normalized(entry.second.as<int>());
                bool isKind = false;
                for (int k = 0; k < kKinds; ++k)
                {
                    if (key == KeyOf(static_cast<Kind>(k)))
                    {
                        t.byKind[static_cast<std::size_t>(k)] = value;
                        isKind = true;
                    }
                }
                if (!isKind)
                    t.byId[key] = value;
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[BuildLimits] limits.yaml: " << e.what() << std::endl;
        return false;
    }
    Storage() = std::move(t);
    Loaded() = true;
    return true;
}

void Reload()
{
    Storage() = Table();
    Loaded() = true;
    std::string text;
    if (Assets::ReadText(LoaderResources::Limits(), text))
        LoadText(text);
}

} // namespace OpenArtOfWar::BuildLimits
