#include "game/Projectiles.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "graphics/SpriteRegistry.hpp"

#include <yaml-cpp/yaml.h>
#include <array>
#include <iostream>

namespace OpenArtOfWar
{

namespace
{

int Named(const std::string &name, SpriteKind kind)
{
    return SpriteHandle(name, kind, "projectile defaults");
}

// "<object>.<prefix>0" .. "<object>.<prefix>(n-1)"
std::vector<int> NamedList(const std::string &prefix, int first, int count, SpriteKind kind)
{
    std::vector<int> v;
    for (int i = first; i < first + count; ++i)
    {
        const int h = Named(prefix + std::to_string(i), kind);
        if (h >= 0)
            v.push_back(h);
    }
    return v;
}

// The first member of a family (projectile directions)
int FamilyStart(const std::string &object, const std::string &family)
{
    const SpriteFamily *f = Sprites().FindFamily(object, family);
    return f && f->count > 0 ? f->start : -1;
}

// The game's own values (index = ProjectileType)
std::array<ProjectileSpec, Projectiles::kCount> Defaults()
{
    std::array<ProjectileSpec, Projectiles::kCount> d;
    ProjectileSpec &bullet = d[static_cast<int>(ProjectileType::Bullet)];
    bullet.key = "bullet";
    bullet.sprite = FamilyStart("projectile_bullet", "dirs");
    bullet.impact = NamedList("fx_impacts.bullet_", 0, 4, SpriteKind::Anim);
    const int bulletWall = Named("fx_impacts.bullet_0", SpriteKind::Anim);
    const int wallDown = Named("fx_impacts.wall_destroyed", SpriteKind::Anim);
    for (ProjectileSpec &spec : d)
    {
        spec.impactWall = bulletWall;
        spec.wallDestroyed = wallDown;
    }

    ProjectileSpec &shell = d[static_cast<int>(ProjectileType::Shell)];
    shell.key = "shell";
    shell.hitscan = false;
    shell.sprite = FamilyStart("projectile_shell", "dirs");
    shell.directions = 32;
    shell.speed = 13.5f;
    shell.spread = 0.035f;
    shell.impact = NamedList("fx_impacts.shell_", 0, 3, SpriteKind::Anim);
    shell.groundMarks = NamedList("fx_craters.mark_", 0, 6, SpriteKind::Part);

    ProjectileSpec &rocket = d[static_cast<int>(ProjectileType::Rocket)];
    rocket.key = "rocket";
    rocket.hitscan = false;
    rocket.sprite = FamilyStart("projectile_rocket", "dirs");
    rocket.directions = 16;
    rocket.speed = 11.0f;
    rocket.spread = 0.08f;
    rocket.impact = NamedList("fx_impacts.rocket_", 0, 3, SpriteKind::Anim);
    rocket.smokeTrail = true;
    rocket.groundMarks = NamedList("fx_craters.mark_", 6, 6, SpriteKind::Part);

    ProjectileSpec &flame = d[static_cast<int>(ProjectileType::Flame)];
    flame.key = "flame";
    flame.hitscan = false;
    flame.sprite = -1; // the jet is its own effect
    flame.speed = 22.0f;
    flame.spread = 0.12f;
    flame.impact = {};

    ProjectileSpec &sniper = d[static_cast<int>(ProjectileType::Sniper)];
    sniper.key = "sniper";
    sniper.sprite = FamilyStart("projectile_sniper", "dirs"); // the long double tracer
    sniper.speed = 48.0f;
    sniper.spread = 0.02f;
    sniper.impact = NamedList("fx_impacts.sniper_", 0, 4, SpriteKind::Anim); // bigger sparks than a bullet's
    sniper.impactWall = Named("fx_impacts.sniper_0", SpriteKind::Anim);
    return d;
}

std::array<ProjectileSpec, Projectiles::kCount> &Specs()
{
    static std::array<ProjectileSpec, Projectiles::kCount> s = [] {
        auto d = Defaults();
        return d;
    }();
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        std::string text;
        if (Assets::ReadText(LoaderResources::projectiles(), text))
            Projectiles::LoadText(text);
    }
    return s;
}

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

} // namespace

int ProjectileSpec::PickImpact(int seed) const
{
    if (impact.empty())
        return -1;
    const int n = static_cast<int>(impact.size());
    return impact[static_cast<std::size_t>(((seed % n) + n) % n)];
}

int ProjectileSpec::PickGroundMark(int seed) const
{
    if (groundMarks.empty())
        return -1;
    const int n = static_cast<int>(groundMarks.size());
    return groundMarks[static_cast<std::size_t>(((seed % n) + n) % n)];
}

namespace Projectiles
{

const ProjectileSpec &Of(ProjectileType type)
{
    const int i = static_cast<int>(type);
    return Specs()[static_cast<std::size_t>(i >= 0 && i < kCount ? i : 0)];
}

const char *KeyOf(ProjectileType type)
{
    return Of(type).key;
}

ProjectileType FromKey(const std::string &key)
{
    for (int i = 0; i < kCount; ++i)
    {
        if (key == Specs()[static_cast<std::size_t>(i)].key)
            return static_cast<ProjectileType>(i);
    }
    return ProjectileType::Bullet;
}

bool LoadText(const std::string &yamlText)
{
    try
    {
        const YAML::Node root = YAML::Load(yamlText);
        const YAML::Node all = root["projectiles"];
        if (!all || !all.IsMap())
            return false;
        for (ProjectileSpec &s : Specs())
        {
            const YAML::Node n = all[s.key];
            if (!n || !n.IsMap())
                continue;
            Read(n, "hitscan", s.hitscan);
            if (n["sprite"])
            {
                // "<object>.<family of directions>" (or "none")
                const std::string ref = n["sprite"].as<std::string>("");
                const auto dot = ref.find('.');
                const SpriteFamily *f = dot == std::string::npos
                                            ? nullptr
                                            : Sprites().FindFamily(ref.substr(0, dot), ref.substr(dot + 1));
                s.sprite
                    = f ? f->start
                        : (ref.empty() || ref == "none"
                               ? -1
                               : SpriteHandle(n["sprite"], SpriteKind::Part, std::string("projectiles.yaml ") + s.key));
            }
            Read(n, "directions", s.directions);
            s.directions = s.directions <= 16 ? 16 : 32;
            Read(n, "speed", s.speed);
            s.speed = std::max(0.5f, s.speed);
            Read(n, "spread", s.spread);
            const std::string where = std::string("projectiles.yaml ") + s.key;
            if (n["impact"])
                s.impact = SpriteHandles(n["impact"], SpriteKind::Anim, where);
            if (n["impact_wall"])
                s.impactWall = SpriteHandle(n["impact_wall"], SpriteKind::Anim, where);
            if (n["wall_destroyed"])
                s.wallDestroyed = SpriteHandle(n["wall_destroyed"], SpriteKind::Anim, where);
            Read(n, "smoke_trail", s.smokeTrail);
            if (n["ground_marks"])
                s.groundMarks = SpriteHandles(n["ground_marks"], SpriteKind::Part, where);
            Read(n, "ground_mark_seconds", s.groundMarkSeconds);
        }
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Projectiles] projectiles.yaml: " << e.what() << std::endl;
        return false;
    }
}

void Reload()
{
    Specs() = Defaults();
    std::string text;
    if (Assets::ReadText(LoaderResources::projectiles(), text))
        LoadText(text);
}

void Apply(const std::string &yamlText)
{
    Specs() = Defaults();
    LoadText(yamlText);
}

} // namespace Projectiles

} // namespace OpenArtOfWar
