// Saving configs keeps what the editor does not know: fields added on disk, comments, removed keys stay removed
#include "ConfigMerge.hpp"
#include "CommentedYaml.hpp"
#include <cstdio>
#include <fstream>
using namespace CharEditor;
int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)
int main(int, char** argv) {
    // 1. Field added on disk after load (new game version), editor changed another field
    YAML::Node base = YAML::Load("stats: {max_hp: 30, move_speed: 1.5}\ncombat: {weapons: [{damage: 5}]}\n");
    YAML::Node mine = YAML::Clone(base);
    mine["stats"]["max_hp"] = 45;
    YAML::Node disk = YAML::Load("stats: {max_hp: 30, move_speed: 1.5, armor_class: heavy}\ncombat: {weapons: [{damage: 5}]}\nnew_section: {x: 1}\n");
    YAML::Node m = mergeYamlEdits(base, mine, disk);
    CHECK(m["stats"]["max_hp"].as<int>() == 45);
    CHECK(m["stats"]["armor_class"].as<std::string>() == "heavy");
    CHECK(m["new_section"]["x"].as<int>() == 1);
    // 2. Editor removed a key (cooldown -> random): removed; a key changed on disk meanwhile is kept
    YAML::Node b2 = YAML::Load("w: {cooldown: 1.0, damage: 3}\n");
    YAML::Node m2 = YAML::Load("w: {cooldown_min: 1.0, cooldown_max: 1.5, damage: 3}\n");
    YAML::Node d2 = YAML::Load("w: {cooldown: 1.0, damage: 7, crit: 2}\n");
    YAML::Node r2 = mergeYamlEdits(b2, m2, d2);
    CHECK(!r2["w"]["cooldown"]);
    CHECK(r2["w"]["cooldown_min"].as<float>() == 1.0f);
    CHECK(r2["w"]["damage"].as<int>() == 7);
    CHECK(r2["w"]["crit"].as<int>() == 2);
    // 3. Untouched document: disk wins entirely
    YAML::Node r3 = mergeYamlEdits(base, YAML::Clone(base), disk);
    CHECK(YAML::Dump(r3) == YAML::Dump(disk));
    // 4. JSON environment
    nlohmann::ordered_json jb = {{"fog", {{"alpha", 140}}}};
    nlohmann::ordered_json jm = {{"fog", {{"alpha", 100}}}};
    nlohmann::ordered_json jd = {{"fog", {{"alpha", 140}, {"rounding", 0.5}}}, {"radar_alert", {{"wave_count", 3}}}};
    auto jr = mergeJsonEdits(jb, jm, jd);
    CHECK(jr["fog"]["alpha"] == 100 && jr["fog"]["rounding"] == 0.5 && jr["radar_alert"]["wave_count"] == 3);
    // 5. CommentedYaml: comments and lines added on disk survive a save
    const std::string path = std::string(argv[0]) + ".commented.yaml";
    { std::ofstream o(path); o << "# Easy level\neasy:\n  income: 10 # per second\n  waves: 3\n"; }
    CommentedYaml y; std::string err;
    CHECK(y.load(path, err));
    CHECK(y.set("easy.income", "12"));
    { std::ofstream o(path); o << "# Easy level\neasy:\n  income: 10 # per second\n  waves: 3\n  # added later\n  tactics: rush\n"; }
    CHECK(y.save(err));
    std::ifstream in(path); std::string txt((std::istreambuf_iterator<char>(in)), {});
    CHECK(txt.find("income: 12  # per second") != std::string::npos);
    CHECK(txt.find("tactics: rush") != std::string::npos && txt.find("# added later") != std::string::npos);
    std::printf("%s\n", fails ? "FAILED" : "all merge checks passed");
    return fails ? 1 : 0;
}
