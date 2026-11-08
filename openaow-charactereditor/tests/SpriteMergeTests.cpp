// Merging sprite objects: names, the merged meta and the references in other files
#include "SpriteMerge.hpp"

#include <cstdio>

using namespace CharEditor::SpriteMerge;

static int fails = 0;
#define CHECK(c)                                                    \
    do                                                              \
    {                                                               \
        if (!(c))                                                   \
        {                                                           \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++fails;                                                \
        }                                                           \
    } while (0)

int main()
{
    const std::vector<Source> sources = {
        {"tank",
         YAML::Load(
             "image: sprite_tank.png\nscale: 2\nrecolor: blue\n"
             "frames:\n  f0: [0, 0, 10, 8]\n  f1: {rect: [10, 0, 4, 4], palette: 2}\n"
             "parts:\n  hull: [{frame: f0, at: [1, 2]}, {frame: fx.f0}]\n"
             "families:\n  idle: {parts: [hull, hull]}\n")},
        {"fx",
         YAML::Load(
             "image: sprite_fx.png\nframes:\n  f0: [0, 0, 6, 6]\n  spark: {rect: [6, 0, 2, 2]}\n"
             "anims:\n  boom: [{frame: f0}, {frame: spark, at: [1, 0]}, {part: tank.hull}, "
             "{part: other.x}, {part: \"tank.idle[1]\"}]\n")},
    };

    // Names: the first source keeps its names, a taken one gets the object prefix
    const NameMap names = BuildNameMap(sources);
    CHECK(names.at("tank").at("f0") == "f0");
    CHECK(names.at("tank").at("idle") == "idle");
    CHECK(names.at("fx").at("f0") == "fx_f0");
    CHECK(names.at("fx").at("spark") == "spark");

    // References inside the merged meta
    CHECK(RewriteMetaRef("f0", "fx", names) == "fx_f0");
    CHECK(RewriteMetaRef("fx.f0", "tank", names) == "fx_f0");
    CHECK(RewriteMetaRef("tank.idle[1]", "fx", names) == "idle[1]");
    CHECK(RewriteMetaRef("other.x", "fx", names) == "other.x");

    // The merged meta
    FramePlaces places
        = {{{"tank", "f0"}, {0, 0}}, {{"tank", "f1"}, {12, 0}}, {{"fx", "f0"}, {0, 10}}, {{"fx", "spark"}, {8, 10}}};
    const YAML::Node m = MergeMetas(sources, names, places, "sprite_merged.png", "", 16, 16);
    CHECK(m["image_size"][0].as<int>() == 16);
    CHECK(m["scale"].as<int>() == 2);
    CHECK(m["recolor"].as<std::string>() == "blue");
    CHECK(m["frames"]["f1"]["rect"][0].as<int>() == 12);
    CHECK(m["frames"]["f1"]["palette"].as<int>() == 2);
    CHECK(!m["frames"]["f0"]["size"]);
    // fx: scale 1 and no recolor -> its own size and recolor
    CHECK(m["frames"]["fx_f0"]["rect"][1].as<int>() == 10);
    CHECK(m["frames"]["fx_f0"]["size"][0].as<float>() == 6.0f);
    CHECK(m["frames"]["fx_f0"]["recolor"].as<std::string>() == "none");
    CHECK(m["parts"]["hull"][1]["frame"].as<std::string>() == "fx_f0");
    CHECK(m["anims"]["boom"][0]["frame"].as<std::string>() == "fx_f0");
    CHECK(m["anims"]["boom"][1]["frame"].as<std::string>() == "spark");
    CHECK(m["anims"]["boom"][2]["part"].as<std::string>() == "hull");
    CHECK(m["anims"]["boom"][3]["part"].as<std::string>() == "other.x");
    CHECK(m["anims"]["boom"][4]["part"].as<std::string>() == "idle[1]");
    CHECK(m["families"]["idle"]["parts"][1].as<std::string>() == "hull");
    // Round trip through the text
    const YAML::Node again = YAML::Load(EmitMeta(m, "# merged\n"));
    CHECK(again["frames"]["spark"]["rect"][0].as<int>() == 8);
    CHECK(again["anims"]["boom"][1]["at"][0].as<int>() == 1);

    // An image scaled x4 since its rectangles were written: they follow, the scale too, the drawn size stays
    {
        YAML::Node meta = YAML::Load(
            "image_size: [20, 10]\nscale: 2\nframes:\n  a: [2, 1, 4, 6]\n"
            "  b: {rect: [10, 0, 8, 8], size: [3, 3]}\n");
        FitMetaToImage(meta, 80, 40);
        CHECK(meta["scale"].as<int>() == 8);
        CHECK(meta["frames"]["a"][0].as<int>() == 8 && meta["frames"]["a"][3].as<int>() == 24);
        CHECK(meta["frames"]["b"]["rect"][2].as<int>() == 32 && meta["frames"]["b"]["size"][0].as<int>() == 3);
        CHECK(meta["image_size"][0].as<int>() == 80);
        // Unevenly: each frame keeps its drawn size
        YAML::Node u = YAML::Load("image_size: [20, 10]\nframes:\n  a: [2, 1, 4, 6]\n");
        FitMetaToImage(u, 40, 30);
        CHECK(!u["scale"]);
        CHECK(u["frames"]["a"]["rect"][1].as<int>() == 3 && u["frames"]["a"]["size"][1].as<int>() == 6);
        // No image_size: only written
        YAML::Node n = YAML::Load("frames:\n  a: [2, 1, 4, 6]\n");
        FitMetaToImage(n, 40, 30);
        CHECK(n["frames"]["a"][0].as<int>() == 2 && n["image_size"][1].as<int>() == 30);
    }

    // Other files
    int count = 0;
    const std::string text = "sprite: tank\nicon: fx.f0\nlist: [tank.idle[2], fx.spark, fx.unknown, other.f0]\n"
                             "image: sprite_fx.png\nx: notfx.f0\n";
    const std::string out = RewriteText(text, names, "merged", count);
    CHECK(
        out
        == "sprite: merged\nicon: merged.fx_f0\nlist: [merged.idle[2], merged.spark, fx.unknown, other.f0]\n"
           "image: sprite_fx.png\nx: notfx.f0\n");
    CHECK(count == 4);
    CHECK(BindsBare("id: x\nsprite: tank\n", "tank"));
    CHECK(!BindsBare("sprite: tank.main\n", "tank"));

    // Packing: nothing overlaps
    const std::vector<std::array<int, 2>> sizes = {{10, 4}, {3, 9}, {5, 5}, {7, 2}};
    const auto pos = ShelfPack(sizes, 1);
    for (std::size_t a = 0; a < sizes.size(); ++a)
        for (std::size_t b = a + 1; b < sizes.size(); ++b)
        {
            const bool apart = pos[a][0] + sizes[a][0] <= pos[b][0] || pos[b][0] + sizes[b][0] <= pos[a][0]
                               || pos[a][1] + sizes[a][1] <= pos[b][1] || pos[b][1] + sizes[b][1] <= pos[a][1];
            CHECK(apart);
        }

    if (fails)
        std::printf("%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
