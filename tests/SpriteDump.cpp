// Compiles a folder of sprite metas with SpriteRegistry and prints the tables as JSON (tools/check_sprites.py
// compares them with the old sheet sprites). Usage: openaow-sprite-dump <sprites dir> [names file]
// The names file lists one reference per line ("<object>.<item>" / "<object>.<family>"); each is resolved.
#include "graphics/SpriteRegistry.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

using namespace OpenArtOfWar;
namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: openaow-sprite-dump <sprites dir> [names file]" << std::endl;
        return 2;
    }
    SpriteRegistry reg;
    std::vector<fs::path> metas;
    for (const auto &e : fs::directory_iterator(argv[1]))
    {
        const std::string f = e.path().filename().string();
        if (f.rfind("sprite_", 0) == 0 && f.size() > 17 && f.compare(f.size() - 10, 10, "_meta.yaml") == 0)
            metas.push_back(e.path());
    }
    std::sort(metas.begin(), metas.end());
    for (const auto &p : metas)
    {
        const std::string f = p.filename().string();
        reg.AddObject(f.substr(7, f.size() - 17), YAML::LoadFile(p.string()), p.parent_path().string());
    }
    reg.Compile();

    const SpriteTables &t = reg.Tables();
    nlohmann::json j;
    j["errors"] = reg.Errors();
    for (const auto &img : t.images)
        j["images"].push_back({{"object", img.object}, {"path", img.path}, {"resistance", img.resistancePath},
                               {"recolor", static_cast<int>(img.recolor)}, {"terrain", img.terrain}});
    for (const auto &f : t.frames)
        j["frames"].push_back({f.page, f.srcX, f.srcY, f.srcW, f.srcH, f.width, f.height, f.recolor});
    auto ranges = [](const std::vector<IndexRange> &v) {
        nlohmann::json a = nlohmann::json::array();
        for (const auto &r : v)
            a.push_back({r.start, r.end});
        return a;
    };
    j["composite_ranges"] = ranges(t.compositeRanges);
    j["anim_ranges"] = ranges(t.animRanges);
    j["body_ranges"] = ranges(t.bodyRanges);
    j["sequence_ranges"] = ranges(t.sequenceRanges);
    for (const auto &p : t.compositeParts)
        j["composite_parts"].push_back({p.child, p.ox, p.oy, p.isComp});
    for (const auto &p : t.animFrames)
        j["anim_frames"].push_back({p.child, p.ox, p.oy, p.isComp});
    for (const auto &p : t.bodyLayers)
        j["body_layers"].push_back({p.child, p.ox, p.oy, p.isComp, p.flag});
    for (const auto &p : t.sequenceSteps)
        j["sequence_steps"].push_back({p.bodyId, p.ox, p.oy});
    for (const auto &b : t.buildings)
        j["buildings"].push_back({b.start, b.end, b.width, b.height, static_cast<int>(b.style), b.tall});
    for (const auto &p : t.buildingParts)
        j["building_parts"].push_back({p.child, p.ox, p.oy, p.isComp, p.layer, p.state});
    j["names"] = nlohmann::json::object();
    if (argc > 2)
    {
        std::ifstream in(argv[2]);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty())
                continue;
            const SpriteRef r = reg.Resolve(line);
            const auto dot = line.find('.');
            const SpriteFamily *fam = reg.FindFamily(line.substr(0, dot), line.substr(dot + 1));
            j["names"][line] = {SpriteKindName(r.kind), r.handle, fam ? fam->count : 0};
        }
    }
    std::cout << j.dump() << std::endl;
    return reg.Errors().empty() ? 0 : 1;
}
