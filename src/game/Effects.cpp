#include "game/Effects.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/Config.hpp"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <iostream>

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

void ReadWreck(const YAML::Node &n, const char *key, EffectsConfig::Wreck &w)
{
    const YAML::Node s = n ? n[key] : YAML::Node();
    Read(s, "composite", w.composite);
    Read(s, "stages", w.stages);
    w.stages = std::max(1, w.stages);
}

bool Contains(const std::vector<int> &v, int x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}

} // namespace

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
    if (animId >= 48 && animId <= 50)
    {
        // Infantry bullet impact (blood / sparks)
        outOx = -13.5f;
        outOy = 0.0f;
    }
    else if (animId >= 51 && animId <= 54)
    {
        // Bullet ricochets
        outOx = -14.5f;
        outOy = -7.5f;
    }
    else if (animId >= 55 && animId <= 58)
    {
        // Vehicle/armor bullet impact sparks
        outOx = -16.0f;
        outOy = -6.0f;
    }
    else if (animId == 42)
    {
        // Small bullet hit flash / wall hit
        outOx = -16.0f;
        outOy = -8.0f;
    }
    else if (animId >= 15 && animId <= 17)
    {
        // Cannon shell explosions
        outOx = -16.0f;
        outOy = 0.0f;
    }
    else if (animId >= 18 && animId <= 20)
    {
        // Rocket explosions
        outOx = -16.0f;
        outOy = -2.0f;
    }
    else
    {
        outOx = 0.0f;
        outOy = 0.0f;
    }
}

bool EffectsConfig::LoadText(const std::string &yamlText)
{
    try
    {
        const YAML::Node root = YAML::Load(yamlText);
        if (!root.IsMap())
            return false;
        const YAML::Node ex = root["explosions"];
        Read(ex, "frame_time", frameTime);
        frameTime = std::max(0.005f, frameTime);
        const YAML::Node veh = ex ? ex["vehicle"] : YAML::Node();
        ReadList(veh, "main", vehicleMain);
        ReadList(veh, "second", vehicleSecond);
        ReadPair(veh, "second_delay", vehicleSecondDelayMin, vehicleSecondDelayMax);
        Read(veh, "second_spread_x", vehicleSecondSpreadX);
        ReadPair(veh, "second_spread_y", vehicleSecondSpreadYMin, vehicleSecondSpreadYMax);
        const YAML::Node b = ex ? ex["building"] : YAML::Node();
        Read(b, "small", buildingSmallMain);
        Read(b, "power", buildingPowerMain);
        Read(b, "large", buildingLargeMain);
        Read(b, "extra_min", buildingExtraMin);
        Read(b, "extra_max", buildingExtraMax);
        buildingExtraMax = std::max(buildingExtraMax, buildingExtraMin);
        ReadList(b, "extra_anims", buildingExtraAnims);
        Read(b, "big_anim", buildingBigAnim);
        Read(b, "big_every", buildingBigEvery);
        buildingBigEvery = std::max(1, buildingBigEvery);
        Read(b, "delay_start", buildingDelayStart);
        Read(b, "delay_step", buildingDelayStep);
        Read(b, "delay_jitter", buildingDelayJitter);

        const YAML::Node w = root["wrecks"];
        Read(w, "burial_seconds", burialSeconds);
        burialSeconds = std::max(1.0f, burialSeconds);
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

void EffectsConfig::Apply(const std::string &yamlText)
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
