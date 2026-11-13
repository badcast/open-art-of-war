#include "game/Effects.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/Config.hpp"
#include "graphics/SpriteRegistry.hpp"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>

namespace OpenArtOfWar
{

namespace
{

template <typename T>
void Read(const YAML::Node &n, const char *key, T &out)
{
    try
    {
        if (n && n.IsMap() && n[key])
            out = n[key].as<T>();
    }
    catch (...)
    {}
}

void ReadList(const YAML::Node &n, const char *key, std::vector<int> &out)
{
    try
    {
        if (!n || !n.IsMap() || !n[key] || !n[key].IsSequence())
            return;
        std::vector<int> v;
        for (const auto &item : n[key])
            v.push_back(item.as<int>());
        out = v; // an empty list is a choice too (no archetype leaves a light wreck)
    }
    catch (...)
    {}
}

// An animation by name
void ReadAnim(const YAML::Node &n, const char *key, int &out)
{
    if (n && n.IsMap() && n[key])
        out = SpriteHandle(n[key], SpriteKind::Anim, std::string("effects.yaml ") + key);
}

void ReadAnims(const YAML::Node &n, const char *key, std::vector<int> &out)
{
    if (n && n.IsMap() && n[key] && n[key].IsSequence())
        out = SpriteHandles(n[key], SpriteKind::Anim, std::string("effects.yaml ") + key);
}

EffectsConfig::Wreck WreckNamed(const char *family)
{
    EffectsConfig::Wreck w;
    if (const char *dot = std::strchr(family, '.'))
    {
        if (const SpriteFamily *f = Sprites().FindFamily(std::string(family, dot), dot + 1))
        {
            w.composite = f->start;
            w.stages = std::max(1, f->count);
            return w;
        }
    }
    w.composite = SpriteHandle(family, SpriteKind::Part, "effects");
    return w;
}

void ReadPair(const YAML::Node &n, const char *key, float &a, float &b)
{
    try
    {
        if (n && n.IsMap() && n[key] && n[key].IsSequence() && n[key].size() >= 2)
        {
            a = n[key][0].as<float>();
            b = n[key][1].as<float>();
        }
    }
    catch (...)
    {}
}

// {sprite: <object>.<family of stages>, stages: n}: the stage count defaults to the family's size
void ReadWreck(const YAML::Node &n, const char *key, EffectsConfig::Wreck &w)
{
    const YAML::Node s = n ? n[key] : YAML::Node();
    if (s && s.IsMap() && s["sprite"] && s["sprite"].IsScalar())
        w = WreckNamed(s["sprite"].as<std::string>().c_str());
    Read(s, "stages", w.stages);
    w.stages = std::max(1, w.stages);
}

bool Contains(const std::vector<int> &v, int x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}

} // namespace

EffectsConfig::EffectsConfig()
{
    auto anim = [](const char *name) {
        return SpriteHandle(name, SpriteKind::Anim, "effects defaults");
    };
    const std::vector<int> blasts {
        anim("fx_explosions.blast_0"),
        anim("fx_explosions.blast_1"),
        anim("fx_explosions.blast_2"),
        anim("fx_explosions.blast_3")};
    vehicleMain = blasts;
    vehicleSecond = blasts;
    buildingExtraAnims = blasts;
    buildingSmallMain = anim("fx_explosions.building_small");
    buildingPowerMain = anim("fx_explosions.building_power");
    buildingLargeMain = anim("fx_explosions.building_large");
    buildingBigAnim = buildingSmallMain;
    vehicle = WreckNamed("fx_wreck_vehicle.stages");
    light = WreckNamed("fx_wreck_light.stages");
    heavy = WreckNamed("fx_wreck_heavy.stages");
    buildingSmall = WreckNamed("fx_rubble_small.stages");
    buildingMedium = WreckNamed("fx_rubble_medium.stages");
    buildingLarge = WreckNamed("fx_rubble_large.stages");
}

EffectsConfig::Wreck EffectsConfig::WreckOfVehicle(int archetype) const
{
    if (Contains(lightArchetypes, archetype))
        return light;
    if (Contains(heavyArchetypes, archetype))
        return heavy;
    return vehicle;
}

EffectsConfig::Wreck EffectsConfig::WreckOfBuilding(int width, int height) const
{
    if (width >= 4 || height >= 4)
        return buildingLarge;
    if (width == 3 || height == 3)
        return buildingMedium;
    return buildingSmall;
}

void ExplosionCentering(int animId, float &outOx, float &outOy)
{
    struct Centering
    {
        const char *names;
        float ox, oy;
    };
    // The impacts of fx_impacts by group
    static const Centering kGroups[] = {
        {"a48 a49 a50", -13.5f, 0.0f},                          // infantry bullet impact (blood / sparks)
        {"sniper_0 sniper_1 sniper_2 sniper_3", -14.5f, -7.5f}, // bullet ricochets
        {"bullet_0 bullet_1 bullet_2 bullet_3", -16.0f, -6.0f}, // vehicle / armour bullet impact sparks
        {"wall_destroyed", -16.0f, -8.0f},                      // small bullet hit flash / wall hit
        {"shell_0 shell_1 shell_2", -16.0f, 0.0f},              // cannon shell explosions
        {"rocket_0 rocket_1 rocket_2", -16.0f, -2.0f},          // rocket explosions
    };
    static int generation = -1;
    static std::map<int, std::pair<float, float>> table;
    if (generation != SpritesGeneration())
    {
        generation = SpritesGeneration();
        table = [] {
            std::map<int, std::pair<float, float>> t;
            for (const Centering &c : kGroups)
            {
                std::string names = c.names;
                std::size_t pos = 0;
                while (pos < names.size())
                {
                    std::size_t end = names.find(' ', pos);
                    if (end == std::string::npos)
                        end = names.size();
                    const int h
                        = SpriteHandle("fx_impacts." + names.substr(pos, end - pos), SpriteKind::Anim, "centering");
                    if (h >= 0)
                        t.emplace(h, std::make_pair(c.ox, c.oy));
                    pos = end + 1;
                }
            }
            return t;
        }();
    }
    const auto it = table.find(animId);
    outOx = it != table.end() ? it->second.first : 0.0f;
    outOy = it != table.end() ? it->second.second : 0.0f;
}

bool EffectsConfig::LoadText(std::string_view yamlText)
{
    try
    {
        const YAML::Node root = YAML::Load(std::string(yamlText));
        if (!root.IsMap())
            return false;
        const YAML::Node ex = root["explosions"];
        Read(ex, "frame_time", frameTime);
        frameTime = std::max(0.005f, frameTime);
        const YAML::Node veh = ex ? ex["vehicle"] : YAML::Node();
        ReadAnims(veh, "main", vehicleMain);
        ReadAnims(veh, "second", vehicleSecond);
        ReadPair(veh, "second_delay", vehicleSecondDelayMin, vehicleSecondDelayMax);
        Read(veh, "second_spread_x", vehicleSecondSpreadX);
        ReadPair(veh, "second_spread_y", vehicleSecondSpreadYMin, vehicleSecondSpreadYMax);
        const YAML::Node b = ex ? ex["building"] : YAML::Node();
        ReadAnim(b, "small", buildingSmallMain);
        ReadAnim(b, "power", buildingPowerMain);
        ReadAnim(b, "large", buildingLargeMain);
        Read(b, "extra_min", buildingExtraMin);
        Read(b, "extra_max", buildingExtraMax);
        buildingExtraMax = std::max(buildingExtraMax, buildingExtraMin);
        ReadAnims(b, "extra_anims", buildingExtraAnims);
        ReadAnim(b, "big_anim", buildingBigAnim);
        Read(b, "big_every", buildingBigEvery);
        buildingBigEvery = std::max(1, buildingBigEvery);
        Read(b, "delay_start", buildingDelayStart);
        Read(b, "delay_step", buildingDelayStep);
        Read(b, "delay_jitter", buildingDelayJitter);

        const YAML::Node w = root["wrecks"];
        Read(w, "burial_seconds", burialSeconds);
        burialSeconds = std::max(1.0f, burialSeconds);
        Read(w, "fade_seconds", fadeSeconds);
        fadeSeconds = std::max(0.0f, fadeSeconds);
        ReadWreck(w, "vehicle", vehicle);
        ReadWreck(w, "light", light);
        ReadWreck(w, "heavy", heavy);
        ReadList(w, "light_archetypes", lightArchetypes);
        ReadList(w, "heavy_archetypes", heavyArchetypes);
        ReadWreck(w, "building_small", buildingSmall);
        ReadWreck(w, "building_medium", buildingMedium);
        ReadWreck(w, "building_large", buildingLarge);
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Effects] effects.yaml: " << e.what() << std::endl;
        return false;
    }
}

EffectsConfig &EffectsConfig::Get()
{
    static EffectsConfig cfg = [] {
        EffectsConfig c;
        std::string text;
        if (Assets::ReadText(LoaderResources::Effects(), text))
            c.LoadText(text);
        return c;
    }();
    return cfg;
}

void EffectsConfig::Apply(std::string_view yamlText)
{
    EffectsConfig fresh;
    fresh.LoadText(yamlText);
    Get() = fresh;
}

void EffectsConfig::reload()
{
    EffectsConfig fresh;
    std::string text;
    if (Assets::ReadText(LoaderResources::Effects(), text))
        fresh.LoadText(text);
    Get() = fresh;
}

} // namespace OpenArtOfWar
