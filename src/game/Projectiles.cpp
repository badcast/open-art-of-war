#include "game/Projectiles.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"

#include <yaml-cpp/yaml.h>
#include <array>
#include <iostream>

namespace OpenArtOfWar
{

    namespace
    {

        std::vector<int> range(int first, int last)
        {
            std::vector<int> v;
            for(int i = first; i <= last; ++i)
                v.push_back(i);
            return v;
        }

        // The game's own values (index = ProjectileType)
        std::array<ProjectileSpec, Projectiles::kCount> defaults()
        {
            std::array<ProjectileSpec, Projectiles::kCount> d;
            ProjectileSpec &bullet = d[static_cast<int>(ProjectileType::Bullet)];
            bullet.key = "bullet";

            ProjectileSpec &shell = d[static_cast<int>(ProjectileType::Shell)];
            shell.key = "shell";
            shell.hitscan = false;
            shell.sprite = 1341;
            shell.directions = 32;
            shell.speed = 13.5f;
            shell.spread = 0.035f;
            shell.impact = {15, 16, 17};
            shell.groundMarks = range(1789, 1794);

            ProjectileSpec &rocket = d[static_cast<int>(ProjectileType::Rocket)];
            rocket.key = "rocket";
            rocket.hitscan = false;
            rocket.sprite = 1405;
            rocket.directions = 16;
            rocket.speed = 11.0f;
            rocket.spread = 0.08f;
            rocket.impact = {18, 19, 20};
            rocket.smokeTrail = true;
            rocket.groundMarks = range(1795, 1800);

            ProjectileSpec &flame = d[static_cast<int>(ProjectileType::Flame)];
            flame.key = "flame";
            flame.hitscan = false;
            flame.sprite = 0; // the jet is its own effect
            flame.speed = 22.0f;
            flame.spread = 0.12f;
            flame.impact = {};

            ProjectileSpec &sniper = d[static_cast<int>(ProjectileType::Sniper)];
            sniper.key = "sniper";
            sniper.sprite = 1373; // the long double tracer
            sniper.speed = 48.0f;
            sniper.spread = 0.02f;
            sniper.impact = {51, 52, 53, 54}; // bigger sparks than a bullet's
            sniper.impactWall = 51;
            return d;
        }

        std::array<ProjectileSpec, Projectiles::kCount> &specs()
        {
            static std::array<ProjectileSpec, Projectiles::kCount> s = []
            {
                auto d = defaults();
                return d;
            }();
            static bool loaded = false;
            if(!loaded)
            {
                loaded = true;
                std::string text;
                if(Assets::readText(LoaderResources::projectiles(), text))
                    Projectiles::loadText(text);
            }
            return s;
        }

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
                if(!n || !n.IsMap() || !n[key])
                    return;
                std::vector<int> v;
                if(n[key].IsSequence())
                {
                    for(const auto &item : n[key])
                        v.push_back(item.as<int>());
                }
                else
                {
                    v.push_back(n[key].as<int>());
                }
                out = v;
            }
            catch(...)
            {
            }
        }

    } // namespace

    int ProjectileSpec::pickImpact(int seed) const
    {
        if(impact.empty())
            return -1;
        const int n = static_cast<int>(impact.size());
        return impact[static_cast<size_t>(((seed % n) + n) % n)];
    }

    int ProjectileSpec::pickGroundMark(int seed) const
    {
        if(groundMarks.empty())
            return -1;
        const int n = static_cast<int>(groundMarks.size());
        return groundMarks[static_cast<size_t>(((seed % n) + n) % n)];
    }

    namespace Projectiles
    {

        const ProjectileSpec &of(ProjectileType type)
        {
            const int i = static_cast<int>(type);
            return specs()[static_cast<size_t>(i >= 0 && i < kCount ? i : 0)];
        }

        const char *keyOf(ProjectileType type)
        {
            return of(type).key;
        }

        ProjectileType fromKey(const std::string &key)
        {
            for(int i = 0; i < kCount; ++i)
            {
                if(key == specs()[static_cast<size_t>(i)].key)
                    return static_cast<ProjectileType>(i);
            }
            return ProjectileType::Bullet;
        }

        bool loadText(const std::string &yamlText)
        {
            try
            {
                const YAML::Node root = YAML::Load(yamlText);
                const YAML::Node all = root["projectiles"];
                if(!all || !all.IsMap())
                    return false;
                for(ProjectileSpec &s : specs())
                {
                    const YAML::Node n = all[s.key];
                    if(!n || !n.IsMap())
                        continue;
                    read(n, "hitscan", s.hitscan);
                    read(n, "sprite", s.sprite);
                    read(n, "directions", s.directions);
                    s.directions = s.directions <= 16 ? 16 : 32;
                    read(n, "speed", s.speed);
                    s.speed = std::max(0.5f, s.speed);
                    read(n, "spread", s.spread);
                    readList(n, "impact", s.impact);
                    read(n, "impact_wall", s.impactWall);
                    read(n, "wall_destroyed", s.wallDestroyed);
                    read(n, "smoke_trail", s.smokeTrail);
                    readList(n, "ground_marks", s.groundMarks);
                    read(n, "ground_mark_seconds", s.groundMarkSeconds);
                }
                return true;
            }
            catch(const std::exception &e)
            {
                std::cerr << "[Projectiles] projectiles.yaml: " << e.what() << std::endl;
                return false;
            }
        }

        void reload()
        {
            specs() = defaults();
            std::string text;
            if(Assets::readText(LoaderResources::projectiles(), text))
                loadText(text);
        }

        void apply(const std::string &yamlText)
        {
            specs() = defaults();
            loadText(yamlText);
        }

    } // namespace Projectiles

} // namespace OpenArtOfWar
