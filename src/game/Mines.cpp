#include "game/Mines.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "graphics/SpriteRegistry.hpp"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <iostream>

namespace OpenArtOfWar
{

namespace
{

std::vector<MineSpec> Defaults()
{
    std::vector<MineSpec> d(2);
    MineSpec &frog = d[0];
    frog.key = "frog";
    frog.name = "FROG MINE";
    frog.desc = "Jumps and explodes in the air, hitting infantry and vehicles within 1 tile.";
    frog.researchValue = 0;
    frog.iconComp = SpriteHandle("icons_res.research_frog_mine", SpriteKind::Part, "mine defaults");
    frog.cost = 40;
    frog.armSeconds = 3.0f;
    frog.trigger = MineSpec::Trigger::Any;
    frog.triggerRadius = 0.8f;
    frog.blastRadius = 1.0f;
    frog.damage = 110;
    frog.maxPerPlayer = 30;
    frog.explosion = SpriteHandle("fx_impacts.shell_1", SpriteKind::Anim, "mine defaults");

    MineSpec &lizard = d[1];
    lizard.key = "monitor_lizard";
    lizard.name = "MONITOR LIZARD MINE";
    lizard.desc = "A heavy mine that triggers only when a vehicle drives over it.";
    lizard.researchValue = 1;
    lizard.iconComp = SpriteHandle("icons_res.research_monitor_lizard_mine", SpriteKind::Part, "mine defaults");
    lizard.cost = 70;
    lizard.armSeconds = 4.0f;
    lizard.trigger = MineSpec::Trigger::Vehicle;
    lizard.triggerRadius = 0.7f;
    lizard.blastRadius = 1.5f;
    lizard.damage = 420;
    lizard.maxPerPlayer = 20;
    lizard.explosion = SpriteHandle("fx_explosions.blast_1", SpriteKind::Anim, "mine defaults");
    return d;
}

std::vector<MineSpec> &Storage()
{
    static std::vector<MineSpec> s;
    return s;
}

bool &Loaded()
{
    static bool loaded = false;
    return loaded;
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

bool LoadText(const std::string &yamlText)
{
    try
    {
        const YAML::Node all = YAML::Load(yamlText)["mines"];
        if (!all || !all.IsMap())
            return false;
        std::vector<MineSpec> &specs = Storage();
        for (const auto &kv : all)
        {
            const std::string key = kv.first.as<std::string>();
            const YAML::Node n = kv.second;
            auto it = std::find_if(specs.begin(), specs.end(), [&](const MineSpec &s) { return s.key == key; });
            if (it == specs.end())
            {
                specs.emplace_back();
                specs.back().key = key;
                it = specs.end() - 1;
            }
            MineSpec &s = *it;
            const std::string where = "mines.yaml " + key;
            Read(n, "name", s.name);
            Read(n, "description", s.desc);
            Read(n, "research_value", s.researchValue);
            if (n["icon"])
                s.iconComp = SpriteHandle(n["icon"], SpriteKind::Part, where);
            Read(n, "cost", s.cost);
            Read(n, "arm_seconds", s.armSeconds);
            std::string trigger;
            Read(n, "trigger", trigger);
            if (trigger == "any")
                s.trigger = MineSpec::Trigger::Any;
            else if (trigger == "infantry")
                s.trigger = MineSpec::Trigger::Infantry;
            else if (trigger == "vehicle")
                s.trigger = MineSpec::Trigger::Vehicle;
            Read(n, "trigger_radius", s.triggerRadius);
            Read(n, "blast_radius", s.blastRadius);
            Read(n, "damage", s.damage);
            Read(n, "max_per_player", s.maxPerPlayer);
            if (n["explosion"])
                s.explosion = SpriteHandle(n["explosion"], SpriteKind::Anim, where);
            s.cost = std::max(0, s.cost);
            s.armSeconds = std::max(0.0f, s.armSeconds);
            s.triggerRadius = std::clamp(s.triggerRadius, 0.1f, 4.0f);
            s.blastRadius = std::clamp(s.blastRadius, 0.1f, 6.0f);
            s.damage = std::max(1, s.damage);
            s.maxPerPlayer = std::max(1, s.maxPerPlayer);
        }
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Mines] mines.yaml: " << e.what() << std::endl;
        return false;
    }
}

} // namespace

namespace Mines
{

const std::vector<MineSpec> &Specs()
{
    if (!Loaded())
        Reload();
    return Storage();
}

void Reload()
{
    Loaded() = true;
    Storage() = Defaults();
    std::string text;
    if (Assets::ReadText(LoaderResources::Mines(), text))
        LoadText(text);
}

bool Apply(const std::string &yamlText)
{
    Loaded() = true;
    Storage() = Defaults();
    return LoadText(yamlText);
}

} // namespace Mines

} // namespace OpenArtOfWar
