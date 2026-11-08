// Merging sprite objects: names, the merged meta and the references in other files
#include "SpriteMerge.hpp"

#include <cstdio>
#include <cstring>

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

    // Arrangements: nothing overlaps, the image holds every frame
    auto apartAll = [&](const std::vector<std::array<int, 2>> &pp) {
        for (std::size_t a = 0; a < sizes.size(); ++a)
            for (std::size_t b = a + 1; b < sizes.size(); ++b)
                if (!(pp[a][0] + sizes[a][0] <= pp[b][0] || pp[b][0] + sizes[b][0] <= pp[a][0]
                      || pp[a][1] + sizes[a][1] <= pp[b][1] || pp[b][1] + sizes[b][1] <= pp[a][1]))
                    return false;
        return true;
    };
    for (unsigned seed = 1; seed <= 20; ++seed)
        CHECK(apartAll(ShufflePack(sizes, 2, seed)));
    CHECK(ShufflePack(sizes, 1, 7) == ShufflePack(sizes, 1, 7));
    const auto grid = GridPack(sizes, 2);
    CHECK(apartAll(grid));
    CHECK(grid[0][1] <= grid[3][1]); // the order is kept: rows go down
    const auto ext = PackExtent(sizes, grid);
    for (std::size_t i = 0; i < sizes.size(); ++i)
        CHECK(grid[i][0] + sizes[i][0] <= ext[0] && grid[i][1] + sizes[i][1] <= ext[1]);

    // Image size
    CHECK(FitToLongestSide(300, 200, 1024) == (std::array<int, 2> {1024, 683}));
    CHECK(FitToLongestSide(200, 300, 512) == (std::array<int, 2> {341, 512}));
    CHECK(CustomSize(400, 200, 100, 999, true, true) == (std::array<int, 2> {100, 50}));
    CHECK(CustomSize(400, 200, 999, 100, true, false) == (std::array<int, 2> {200, 100}));
    CHECK(CustomSize(400, 200, 123, 45, false, true) == (std::array<int, 2> {123, 45}));
    {
        // A 4x4 picture of distinct colours scaled up by 3 is detected as x3 and comes back exactly
        const int sw = 4, sh = 4, k = 3, w = sw * k, h = sh * k;
        std::vector<unsigned char> small(sw * sh * 4), big(w * h * 4);
        for (int i = 0; i < sw * sh; ++i)
        {
            small[i * 4] = static_cast<unsigned char>(i * 15 + 1);
            small[i * 4 + 1] = static_cast<unsigned char>(255 - i * 9);
            small[i * 4 + 2] = static_cast<unsigned char>(i * 7);
            small[i * 4 + 3] = 255;
        }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                std::memcpy(&big[(y * w + x) * 4], &small[((y / k) * sw + x / k) * 4], 4);
        CHECK(DetectIntegerUpscale(big.data(), w, h, {}) == k);
        CHECK(DetectIntegerUpscale(big.data(), w, h, {{3, 3, 6, 6}}) == k);
        CHECK(DetectIntegerUpscale(big.data(), w, h, {{1, 3, 6, 6}}) == 1); // a frame off the grid
        CHECK(DownscaleInteger(big.data(), w, h, k) == small);
        big[(5 * w + 5) * 4] ^= 1; // one pixel differs: not an exact upscale any more
        CHECK(DetectIntegerUpscale(big.data(), w, h, {}) == 1);
    }

    // Magnetism: an edge within the threshold meets the nearest target, otherwise nothing moves
    {
        int met = -1;
        const std::vector<int> t = {0, 40, 100};
        CHECK(SnapDelta(3, 20, t, 6, &met) == -3 && met == 0);   // left edge to the border
        CHECK(SnapDelta(50, 97, t, 6, &met) == 3 && met == 100); // right edge to 100
        CHECK(SnapDelta(10, 30, t, 6) == 0);                     // nothing near
        CHECK(SnapDelta(38, 70, t, 6) == 2);                     // 40 is two away
        CHECK(SnapDelta(5, 5, t, 6) == -5);                      // a single point (a dragged corner)
    }

    // Assemble: composites keep their frames at their offsets, overlapping ones become rows, nothing overlaps
    {
        const YAML::Node meta = YAML::Load(R"(
parts:
  hull: [ {frame: a, at: [0, 0]}, {frame: b, at: [10, 0]} ]
  both: [ {part: hull, at: [0, 5]}, {frame: c, at: [0, 5]} ]
  layered: [ {frame: d, at: [0, 0]}, {frame: e, at: [3, 3]} ]
  foreign: [ {frame: other.x, at: [0, 0]} ]
)");
        const std::map<std::string, int> idx = {{"a", 0}, {"b", 1}, {"c", 2}, {"d", 3}, {"e", 4}, {"f", 5}};
        const auto groups = AssemblyGroupsFromMeta(meta, "obj", idx, 1.0f);
        CHECK(groups.size() == 3); // hull, both, layered (foreign has none of ours)
        const std::vector<std::array<int, 2>> sz = {{10, 10}, {10, 10}, {4, 4}, {8, 8}, {8, 8}, {5, 5}};
        const auto at = AssemblePack(sz, groups, 1);
        CHECK(at[1][0] - at[0][0] == 10 && at[1][1] == at[0][1]);              // a, b side by side as in the game
        CHECK(at[4][0] - at[3][0] == 8 + 1 && at[4][1] == at[3][1]);           // d, e overlap: laid in a row
        bool apart = true;
        for (std::size_t i = 0; i < sz.size(); ++i)
            for (std::size_t j = i + 1; j < sz.size(); ++j)
                if (!(at[i][0] + sz[i][0] <= at[j][0] || at[j][0] + sz[j][0] <= at[i][0] || at[i][1] + sz[i][1] <= at[j][1]
                      || at[j][1] + sz[j][1] <= at[i][1]))
                    apart = false;
        CHECK(apart); // a frame goes with the first composite that uses it: c and f are blocks of their own
    }

    if (fails)
        std::printf("%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
