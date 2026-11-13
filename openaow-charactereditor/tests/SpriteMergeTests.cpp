// Merging sprite objects: names, the merged meta and the references in other files
#include "SpriteMerge.hpp"
#include "SpriteMetaEdit.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

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

    // Assemble: a body is put together as the game draws it, a layer over another goes right beside its place
    {
        const YAML::Node meta = YAML::Load(R"(
frames:
  hull: [0, 0, 20, 10]
  tur: [0, 0, 8, 8]
  shadow: [0, 0, 20, 4]
  fire0: [0, 0, 4, 4]
  fire1: [0, 0, 4, 4]
  loose: [0, 0, 5, 5]
parts:
  hull_p: [ {frame: hull, at: [0, 0]} ]
  tur_p: [ {frame: tur, at: [0, 0]} ]
  f0: [ {frame: fire0} ]
  f1: [ {frame: fire1} ]
anims:
  fire: [ {part: f0}, {part: f1} ]
bodies:
  b0: [ {frame: shadow, at: [0, 12]}, {part: hull_p, at: [0, 0]}, {part: tur_p, at: [6, 1]}, {anim: fire, at: [30, 0]} ]
)");
        const std::map<std::string, int> idx
            = {{"hull", 0}, {"tur", 1}, {"shadow", 2}, {"fire0", 3}, {"fire1", 4}, {"loose", 5}};
        const auto groups = AssemblyGroupsFromMeta(meta, "obj", idx, 1.0f);
        CHECK(!groups.empty() && groups[0].size() == 4); // shadow, hull, turret, first fire picture: one body
        const std::vector<std::array<int, 2>> sz = {{20, 10}, {8, 8}, {20, 4}, {4, 4}, {4, 4}, {5, 5}};
        const auto at = AssemblePack(sz, groups, 1);
        CHECK(at[2][0] == at[0][0] && at[2][1] == at[0][1] + 12); // the shadow under the hull, as in the game
        CHECK(at[3][0] == at[0][0] + 30 && at[3][1] == at[0][1]); // the fire beside it
        // The turret is drawn over the hull: it may not share pixels, it stands right next to its place
        const int tx = at[1][0] - at[0][0], ty = at[1][1] - at[0][1];
        CHECK(std::abs(tx - 6) + std::abs(ty - 1) <= 12);
        bool apart = true;
        for (std::size_t i = 0; i < sz.size(); ++i)
            for (std::size_t j = i + 1; j < sz.size(); ++j)
                if (!(at[i][0] + sz[i][0] <= at[j][0] || at[j][0] + sz[j][0] <= at[i][0]
                      || at[i][1] + sz[i][1] <= at[j][1] || at[j][1] + sz[j][1] <= at[i][1]))
                    apart = false;
        CHECK(apart);
        // A building: its parts and the first picture of its animation in one block
        const YAML::Node bmeta = YAML::Load(R"(
frames: {base: [0, 0, 30, 20], light: [0, 0, 3, 3]}
parts: {bp: [ {frame: base} ], lp: [ {frame: light} ]}
anims: {blink: [ {part: lp} ]}
buildings: {main: {size: [2, 2], parts: [ {part: bp, at: [0, 0]}, {anim: blink, at: [40, 2]} ]}}
)");
        const auto bg = AssemblyGroupsFromMeta(bmeta, "b", {{"base", 0}, {"light", 1}}, 1.0f);
        CHECK(!bg.empty() && bg[0].size() == 2);
        const auto bat = AssemblePack({{30, 20}, {3, 3}}, bg, 1);
        CHECK(bat[1][0] - bat[0][0] == 40 && bat[1][1] - bat[0][1] == 2);

        // Per-frame ppu overrides stale header `scale`, state==0 precedes state>0, and <=3px seam overlaps nudge 1D
        const YAML::Node smeta = YAML::Load(R"(
scale: 1.0
frames:
  t0: { rect: [0, 0, 50, 40], size: [5.0, 4.0] }
  t1: { rect: [50, 0, 50, 40], size: [5.0, 4.0] }
  dmg: { rect: [0, 40, 20, 20], size: [2.0, 2.0] }
parts:
  p_dmg: [ {frame: dmg} ]
buildings:
  main:
    parts:
      - { part: p_dmg, at: [0, 0], state: 1 }
      - { frame: t0, at: [0, 0], state: 0 }
      - { frame: t1, at: [4.9, 0], state: 0 }
)");
        const auto sg = AssemblyGroupsFromMeta(smeta, "s", {{"t0", 0}, {"t1", 1}, {"dmg", 2}}, 1.0f);
        CHECK(!sg.empty() && sg[0].size() == 3);
        CHECK(sg[0][0].frame == 0 && sg[0][1].frame == 1 && sg[0][2].frame == 2); // state==0 base tiles before state>0
        CHECK(sg[0][1].x == 49 && sg[0][1].y == 0);
        const auto sat = AssemblePack({{50, 40}, {50, 40}, {20, 20}}, sg, 4);
        // 1px seam overlap nudges t1 to x = t0.x + 50 (no extra padding gap) and keeps y aligned
        CHECK(sat[1][0] - sat[0][0] == 50 && sat[1][1] == sat[0][1]);
    }

    // SplitMeta: move subset of frames + exclusive configs into a new sprite object cleanly
    {
        const YAML::Node src = YAML::Load(R"(
frames:
  keep_f: [0, 0, 10, 10]
  move_f: [10, 0, 12, 12]
parts:
  p_keep: [ {frame: keep_f} ]
  p_move: [ {frame: move_f} ]
  p_mix:  [ {frame: keep_f}, {frame: move_f} ]
buildings:
  b_move: { parts: [ {part: p_move} ] }
  b_stay: { parts: [ {part: p_keep}, {part: p_move} ] }
)");
        const auto split = SplitMeta(
            "src",
            src,
            {"move_f"},
            {{{"src", "move_f"}, {0, 0}}},
            "dst",
            "sprites/sprite_dst.png",
            "",
            12,
            12);
        CHECK(!split.movedEverything);
        CHECK(!split.remainingSourceMeta["frames"]["move_f"]);
        CHECK(split.remainingSourceMeta["frames"]["keep_f"]);
        CHECK(split.newObjectMeta["frames"]["move_f"]);
        CHECK(!split.newObjectMeta["frames"]["keep_f"]);
        // p_move & b_move moved exclusively; p_mix & b_stay remained and qualify cross-references
        CHECK(!split.remainingSourceMeta["parts"]["p_move"]);
        CHECK(split.newObjectMeta["parts"]["p_move"]);
        const std::string mixFrame = split.remainingSourceMeta["parts"]["p_mix"][1]["frame"].as<std::string>("");
        CHECK(mixFrame == "dst.move_f");
        CHECK(!split.remainingSourceMeta["buildings"]["b_move"]);
        const std::string movePart = split.newObjectMeta["buildings"]["b_move"]["parts"][0]["part"].as<std::string>("");
        CHECK(movePart == "p_move");
        const std::string stayPart
            = split.remainingSourceMeta["buildings"]["b_stay"]["parts"][1]["part"].as<std::string>("");
        CHECK(stayPart == "dst.p_move");

        int rPartial = 0;
        NameMap nm;
        for (const std::string &m : split.movedItems)
            nm["src"][m] = m;
        const std::string rewritten = RewriteText(
            "use: src.p_move\nkeep: src.p_keep\nsprite: src\nbare: p_move\n", nm, "dst", rPartial, false);
        CHECK(rPartial == 1);
        CHECK(rewritten == "use: dst.p_move\nkeep: src.p_keep\nsprite: src\nbare: p_move\n");
    }

    // ScaleGroupRects: proportional scaling preserves group layout, expands canvas, or relocates cleanly on collision
    {
        const std::vector<std::array<int, 4>> sel {{10, 10, 20, 10}, {32, 10, 10, 10}};
        const auto noHit = ScaleGroupRects(sel, {{0, 100, 30, 30}}, 2.0f, 2.0f, 10, 10, 64, 140, 2);
        CHECK(!noHit.relocated);
        CHECK(noHit.rects[0] == (std::array<int, 4> {10, 10, 40, 20}));
        CHECK(noHit.rects[1] == (std::array<int, 4> {54, 10, 20, 20}));
        CHECK(noHit.canvasW == 74 && noHit.canvasH == 140);

        // Collision with unselected frame forces clean relocation below unselected
        const auto hit = ScaleGroupRects({{0, 0, 20, 20}}, {{25, 0, 20, 30}}, 2.0f, 2.0f, 0, 0, 64, 64, 2);
        CHECK(hit.relocated);
        CHECK(hit.rects[0][1] == 32 && hit.rects[0][2] == 40 && hit.rects[0][3] == 40);
        CHECK(hit.canvasH == 72);
    }

    // SpriteMetaEdit: turret pivot recentering & paired firing/rot layer sync
    {
        std::string meta =
            "# keep comment\n"
            "parts:\n"
            "  c731:\n"
            "  - {frame: s393, at: [55, 36]}\n"
            "  c739:\n"
            "  - {frame: s393, at: [55, 36]}\n"
            "  - {frame: flash, at: [44, 34]}\n"
            "  other:\n"
            "  - {frame: s393, at: [10, 10]}\n"
            "bodies:\n"
            "  b16:\n"
            "  - {part: c691, at: [-29, -20], flag: 1}\n"
            "  - {anim: 'rot_a125[0]', at: [-50, -37], flag: 4}\n"
            "  - {part: 'rot_c731[0]', at: [-50, -37]}\n"
            "families:\n"
            "  rot_c731: {parts: [c731]}\n";

        // Shift c731 so local (61.5, 43) becomes [0, 0]; paired c739 shifts by (-61.5, -43), unrelated 'other' stays
        CHECK(CharEditor::ShiftPartPivot(meta, "c731", 61.5f, 43.0f, true));
        const auto c731 = CharEditor::ReadMetaItemLayers(meta, "parts", "c731");
        const auto c739 = CharEditor::ReadMetaItemLayers(meta, "parts", "c739");
        const auto other = CharEditor::ReadMetaItemLayers(meta, "parts", "other");
        CHECK(c731.size() == 1 && std::abs(c731[0].x - (-6.5f)) < 0.01f && std::abs(c731[0].y - (-7.0f)) < 0.01f);
        CHECK(c739.size() == 2 && std::abs(c739[0].x - (-6.5f)) < 0.01f && std::abs(c739[1].x - (-17.5f)) < 0.01f);
        CHECK(other.size() == 1 && std::abs(other[0].x - 10.0f) < 0.01f);
        CHECK(meta.find("# keep comment") != std::string::npos);

        // Shifting rot_c731[0] on body b16 syncs sibling rot_a125[0]
        CHECK(CharEditor::ShiftBodyLayer(meta, "b16", 2, 61.5f, 43.0f, true));
        const auto b16 = CharEditor::ReadMetaItemLayers(meta, "bodies", "b16");
        CHECK(b16.size() == 3 && std::abs(b16[1].x - 11.5f) < 0.01f && std::abs(b16[2].x - 11.5f) < 0.01f);
        CHECK(std::abs(b16[2].y - 6.0f) < 0.01f);
        CHECK(CharEditor::ReadMetaFamilyMembers(meta, "rot_c731", "parts").size() == 1);
    }

    if (fails)
        std::printf("%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
