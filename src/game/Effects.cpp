#include "game/Effects.hpp"
#include "core/AssetStore.hpp"
#include "core/Config.hpp"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <iostream>

namespace OpenArtOfWar
{

    namespace
    {

        template <typename T>
        void read(const YAML::Node &n, const char *key, T &out)
        {
            try
            {
                if(n && n.IsMap() && n[key])
                    out = n[key].as<T>();
            }
            catch(...)
            {
            }
        }

        void readList(const YAML::Node &n, const char *key, std::vector<int> &out)
        {
            try
            {
                if(!n || !n.IsMap() || !n[key] || !n[key].IsSequence())
                    return;
                std::vector<int> v;
                for(const auto &item : n[key])
                    v.push_back(item.as<int>());
                out = v; // an empty list is a choice too (no archetype leaves a light wreck)
            }
            catch(...)
            {
            }
        }

        void readPair(const YAML::Node &n, const char *key, float &a, float &b)
        {
            try
            {
                if(n && n.IsMap() && n[key] && n[key].IsSequence() && n[key].size() >= 2)
                {
                    a = n[key][0].as<float>();
                    b = n[key][1].as<float>();
                }
            }
            catch(...)
            {
            }
        }

        void readWreck(const YAML::Node &n, const char *key, EffectsConfig::Wreck &w)
        {
            const YAML::Node s = n ? n[key] : YAML::Node();
            read(s, "composite", w.composite);
            read(s, "stages", w.stages);
            w.stages = std::max(1, w.stages);
        }

        bool contains(const std::vector<int> &v, int x)
        {
            return std::find(v.begin(), v.end(), x) != v.end();
        }

    } // namespace

    EffectsConfig::Wreck EffectsConfig::wreckOfVehicle(int archetype) const
    {
        if(contains(lightArchetypes, archetype))
            return light;
        if(contains(heavyArchetypes, archetype))
            return heavy;
        return vehicle;
    }

    EffectsConfig::Wreck EffectsConfig::wreckOfBuilding(int width, int height) const
    {
        if(width >= 4 || height >= 4)
            return buildingLarge;
        if(width == 3 || height == 3)
            return buildingMedium;
        return buildingSmall;
    }

    void explosionCentering(int animId, float &outOx, float &outOy)
    {
        if(animId >= 48 && animId <= 50)
        {
            // Infantry bullet impact (blood / sparks)
            outOx = -13.5f;
            outOy = 0.0f;
        }
        else if(animId >= 51 && animId <= 54)
        {
            // Bullet ricochets
            outOx = -14.5f;
            outOy = -7.5f;
        }
        else if(animId >= 55 && animId <= 58)
        {
            // Vehicle/armor bullet impact sparks
            outOx = -16.0f;
            outOy = -6.0f;
        }
        else if(animId == 42)
        {
            // Small bullet hit flash / wall hit
            outOx = -16.0f;
            outOy = -8.0f;
        }
        else if(animId >= 15 && animId <= 17)
        {
            // Cannon shell explosions
            outOx = -16.0f;
            outOy = 0.0f;
        }
        else if(animId >= 18 && animId <= 20)
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

    bool EffectsConfig::loadText(const std::string &yamlText)
    {
        try
        {
            const YAML::Node root = YAML::Load(yamlText);
            if(!root.IsMap())
                return false;
            const YAML::Node ex = root["explosions"];
            read(ex, "frame_time", frameTime);
            frameTime = std::max(0.005f, frameTime);
            const YAML::Node veh = ex ? ex["vehicle"] : YAML::Node();
            readList(veh, "main", vehicleMain);
            readList(veh, "second", vehicleSecond);
            readPair(veh, "second_delay", vehicleSecondDelayMin, vehicleSecondDelayMax);
            read(veh, "second_spread_x", vehicleSecondSpreadX);
            readPair(veh, "second_spread_y", vehicleSecondSpreadYMin, vehicleSecondSpreadYMax);
            const YAML::Node b = ex ? ex["building"] : YAML::Node();
            read(b, "small", buildingSmallMain);
            read(b, "power", buildingPowerMain);
            read(b, "large", buildingLargeMain);
            read(b, "extra_min", buildingExtraMin);
            read(b, "extra_max", buildingExtraMax);
            buildingExtraMax = std::max(buildingExtraMax, buildingExtraMin);
            readList(b, "extra_anims", buildingExtraAnims);
            read(b, "big_anim", buildingBigAnim);
            read(b, "big_every", buildingBigEvery);
            buildingBigEvery = std::max(1, buildingBigEvery);
            read(b, "delay_start", buildingDelayStart);
            read(b, "delay_step", buildingDelayStep);
            read(b, "delay_jitter", buildingDelayJitter);

            const YAML::Node w = root["wrecks"];
            read(w, "burial_seconds", burialSeconds);
            burialSeconds = std::max(1.0f, burialSeconds);
            readWreck(w, "vehicle", vehicle);
            readWreck(w, "light", light);
            readWreck(w, "heavy", heavy);
            readList(w, "light_archetypes", lightArchetypes);
            readList(w, "heavy_archetypes", heavyArchetypes);
            readWreck(w, "building_small", buildingSmall);
            readWreck(w, "building_medium", buildingMedium);
            readWreck(w, "building_large", buildingLarge);
            return true;
        }
        catch(const std::exception &e)
        {
            std::cerr << "[Effects] effects.yaml: " << e.what() << std::endl;
            return false;
        }
    }

    EffectsConfig &EffectsConfig::get()
    {
        static EffectsConfig cfg = []
        {
            EffectsConfig c;
            std::string text;
            if(Assets::readText("assets/characters/effects.yaml", text))
                c.loadText(text);
            return c;
        }();
        return cfg;
    }

    void EffectsConfig::apply(const std::string &yamlText)
    {
        EffectsConfig fresh;
        fresh.loadText(yamlText);
        get() = fresh;
    }

    void EffectsConfig::reload()
    {
        EffectsConfig fresh;
        std::string text;
        if(Assets::readText("assets/characters/effects.yaml", text))
            fresh.loadText(text);
        get() = fresh;
    }

} // namespace OpenArtOfWar
