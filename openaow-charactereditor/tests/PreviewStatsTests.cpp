#include "EditorCommon.hpp"
#include <cstdio>

int main()
{
    YAML::Node doc = YAML::Load(R"(
type: unit
stats: {max_hp: 100, move_speed: 2, sight_radius: 5}
combat: {weapons: [{damage: 10, range: 4, cooldown: 1}]}
abilities:
  siege:
    stats: {max_hp: 150, move_speed: 0, sight_radius: 8}
    combat: {weapons: [{damage: 30, range: 9, cooldown: 2}]}
)");
    const std::string before = YAML::Dump(doc);
    for (int i = 0; i < 3; ++i)
    {
        const auto deployed = CharEditor::ComputeStats(doc, true);
        const auto mobile = CharEditor::ComputeStats(doc, false);
        if (deployed.damage != 30 || deployed.range != 9 || deployed.hp != 150 || mobile.damage != 10
            || mobile.range != 4 || mobile.hp != 100 || YAML::Dump(doc) != before)
        {
            std::puts("FAIL: previewing siege form changed the character document");
            return 1;
        }
    }
    std::puts("Preview stats preserve the character document");
    return 0;
}
