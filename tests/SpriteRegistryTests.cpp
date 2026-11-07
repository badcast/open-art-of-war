// SpriteRegistry: names resolve, families are contiguous, a removed object breaks only the references to it
#include "graphics/SpriteRegistry.hpp"

#include <cstdio>

using namespace OpenArtOfWar;

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

static const char *kShadow = R"(
scale: 2
frames:
  s: [0, 0, 10, 4]
parts:
  shadow: [ {frame: s, at: [-5, -2]} ]
)";

static const char *kTank = R"(
recolor: red
frames:
  h0: [0, 0, 30, 20]
  h1: {rect: [30, 0, 30, 20], palette: 4}
  t0: [0, 20, 16, 16]
  t1: [16, 20, 16, 16]
parts:
  hull_0: [ {part: fx_shadow.shadow, at: [0, 4]}, {frame: h0, at: [-15, -10]} ]
  hull_1: [ {frame: h1, at: [-15, -10]} ]
  tur_1: [ {frame: t1} ]
  tur_0: [ {frame: t0} ]
anims:
  fire: [ {part: hull_0}, {part: hull_1, at: [1, 0]} ]
bodies:
  b0: [ {part: hull_0, flag: 1}, {part: turret, flag: 2}, {anim: fire, flag: 3} ]
  b1: [ {part: hull_1, at: [-29, -20]} ]
sequences:
  idle_1: [ {body: b1} ]
  idle_0: [ {body: b0} ]
buildings:
  wreck: { size: [2, 1], parts: [ {part: hull_0, layer: 0, state: 4}, {anim: fire, layer: 1, state: 5} ] }
families:
  idle: { sequences: [idle_0, idle_1] }
  turret: { parts: [tur_0, tur_1] }
)";

static void Build(SpriteRegistry &reg, bool withShadow)
{
    reg.Clear();
    if (withShadow)
        reg.AddObject("fx_shadow", YAML::Load(kShadow), "assets/sprites");
    reg.AddObject("tank_conf", YAML::Load(kTank), "assets/sprites");
    reg.Compile();
}

int main()
{
    SpriteRegistry reg;
    Build(reg, true);
    CHECK(reg.Errors().empty());
    for (const auto &e : reg.Errors())
        std::printf("  error: %s\n", e.c_str());
    const SpriteTables &t = reg.Tables();

    // Images: one per object, path and settings from the meta
    CHECK(t.images.size() == 2);
    const int tankImg = reg.ImageOf("tank_conf");
    CHECK(tankImg >= 0 && t.images[tankImg].path == "assets/sprites/sprite_tank_conf.png");
    CHECK(t.images[tankImg].recolor == SpriteRecolor::Red);
    CHECK(t.images[reg.ImageOf("fx_shadow")].scale == 2.0f);

    // Frames
    const SpriteRef h1 = reg.Resolve("tank_conf.h1");
    CHECK(h1.kind == SpriteKind::Frame);
    CHECK(t.frames[h1.handle].page == tankImg && t.frames[h1.handle].x == 30 && t.frames[h1.handle].palette == 4);

    // A composite referencing another object
    const SpriteRef hull0 = reg.Resolve("tank_conf.hull_0", SpriteKind::Part);
    CHECK(hull0.Valid());
    const IndexRange r = t.compositeRanges[hull0.handle];
    CHECK(r.end - r.start == 1);
    CHECK(t.compositeParts[r.start].isComp == 1);
    CHECK(t.compositeParts[r.start].child == reg.Resolve("fx_shadow.shadow").handle);
    CHECK(t.compositeParts[r.start].oy == 4);
    CHECK(t.compositeParts[r.start + 1].isComp == 0);
    CHECK(t.compositeParts[r.start + 1].ox == -15);

    // Families are contiguous in the order of the family, whatever the file order of the items
    const SpriteFamily *idle = reg.FindFamily("tank_conf", "idle");
    CHECK(idle && idle->kind == SpriteKind::Sequence && idle->count == 2);
    if (idle)
    {
        const IndexRange s0 = t.sequenceRanges[idle->start];
        const IndexRange s1 = t.sequenceRanges[idle->start + 1];
        CHECK(t.sequenceSteps[s0.start].bodyId == reg.Resolve("tank_conf.b0").handle);
        CHECK(t.sequenceSteps[s1.start].bodyId == reg.Resolve("tank_conf.b1").handle);
        CHECK(reg.NameOf({SpriteKind::Sequence, idle->start + 1}) == "tank_conf.idle[1]");
    }
    // A family referenced by name gives its first member (turret base + direction)
    const SpriteRef turret = reg.Resolve("tank_conf.turret");
    const SpriteFamily *tf = reg.FindFamily("tank_conf", "turret");
    CHECK(tf && turret.kind == SpriteKind::Part && turret.handle == tf->start);
    {
        const IndexRange b = t.bodyRanges[reg.Resolve("tank_conf.b0").handle];
        CHECK(b.end - b.start == 2);
        CHECK(t.bodyLayers[b.start + 1].child == tf->start && t.bodyLayers[b.start + 1].flag == 2);
        CHECK(t.bodyLayers[b.start + 2].isComp == 2);
        const IndexRange t1 = t.compositeRanges[tf->start + 1];
        CHECK(t.compositeParts[t1.start].child == reg.Resolve("tank_conf.t1").handle);
    }

    // Buildings
    const SpriteRef wreck = reg.Resolve("tank_conf.wreck");
    CHECK(wreck.kind == SpriteKind::Building);
    const BuildingDef &bd = t.buildings[wreck.handle];
    CHECK(bd.width == 2 && bd.height == 1 && bd.end - bd.start == 1);
    CHECK(t.buildingParts[bd.start + 1].isComp == 2 && t.buildingParts[bd.start + 1].state == 5);

    // Names round-trip
    CHECK(reg.NameOf(hull0) == "tank_conf.hull_0");
    CHECK(!reg.Resolve("tank_conf.nothing").Valid());
    CHECK(!reg.Resolve("nothing.hull_0").Valid());

    // Removing an object: only the reference to it fails, with a message naming it; the rest is intact
    Build(reg, false);
    CHECK(reg.Errors().size() == 1);
    CHECK(!reg.Errors().empty() && reg.Errors()[0].find("fx_shadow") != std::string::npos);
    const SpriteRef h1b = reg.Resolve("tank_conf.h1");
    CHECK(h1b.Valid() && reg.Tables().frames[h1b.handle].x == 30);
    const SpriteFamily *idle2 = reg.FindFamily("tank_conf", "idle");
    CHECK(idle2 && idle2->count == 2);
    {
        const IndexRange rr = reg.Tables().compositeRanges[reg.Resolve("tank_conf.hull_0").handle];
        CHECK(reg.Tables().compositeParts[rr.start].child == -1);
        CHECK(reg.Tables().compositeParts[rr.start + 1].ox == -15);
    }

    // Bad input is reported, not fatal
    reg.Clear();
    reg.AddObject("bad", YAML::Load("frames: {f: [1, 2]}\nparts: {p: [ {frame: f, part: g} ]}\nrecolor: green\n"), "");
    reg.AddObject("bad", YAML::Load("{}"), "");
    reg.Compile();
    CHECK(reg.Errors().size() == 4);

    std::printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
