#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "graphics/TextureManager.hpp"

namespace OpenArtOfWar
{

// One object = one sprite: "sprites/sprite_<name>.png" with every frame of the object (all directions, animations,
// states) and "sprites/sprite_<name>_meta.yaml" next to it describing them. Everything refers to sprites by name
// ("<object>.<item>"), never by position, so removing an object only breaks the references to that object, with a
// readable error at load time.
//
// The registry compiles all metas into the flat tables the renderer draws from (frames, composites, anims, unit
// bodies, unit sequences, buildings). The handles it hands out are indices into those tables; they are valid for
// one load only and are never written anywhere.
//
// Meta format (all sections optional):
//
//   image: sprite_tank_conf.png      # default: sprite_<name>.png next to the meta
//   scale: 4                         # default PNG pixels per drawing unit (frames without their own "size")
//   terrain: true                    # map tiles (filtered without bleeding between tiles)
//   recolor: red                     # none | blue | red: team armour the army colour recolours (frames may override)
//   resistance_image: sprite_tank_conf_resistance.png  # same layout, drawn for the Resistance (optional)
//   frames:                          # pixel rectangles in the image: [x, y, w, h], or
//     f0: [0, 0, 120, 80]            # {rect: [x, y, w, h], size: [w, h], recolor: none, palette: n}
//                                    # "size": drawn size in game units (default: rect size / scale); offsets
//                                    # ("at") are game units as well
//   parts:                           # composites: lists of entries
//     hull_0: [ {frame: f0, at: [-15, -10]}, {part: other_object.shadow} ]
//   anims:                           # animations: one entry per frame
//     muzzle: [ {part: m0}, {part: m1, at: [1, 0]} ]
//   bodies:                          # unit bodies: entries with a visibility flag (0..5)
//     b0: [ {part: hull_0, at: [-29, -20], flag: 1}, {anim: muzzle, flag: 3} ]
//   sequences:                       # unit sequences: steps of bodies
//     idle_0: [ {body: b0} ]
//   buildings:                       # building parts by layer and damage state
//     main: { size: [3, 2], style: structure, parts: [ {part: base, layer: 0, state: 4}, {anim: lights, layer: 1} ] }
//                                    # style: structure | wall | decor | decor_flat; tall: true for high decorations
//   families:                        # ordered lists the game indexes by direction / stage / tile slot
//     idle: { sequences: [idle_0, idle_1, ...] }
//     turret: { parts: [t0, t1, ...] }
//
// An entry names exactly one of frame / part / anim / body; "at" is the offset (default [0, 0]). A name is "item",
// "family" (its first member), "family[i]" (its i-th member), each optionally prefixed with "<object>.".
// A family is laid out contiguously in the compiled tables (its members are copies), so "start + direction" works
// whatever order the items have in the file.
enum class SpriteKind : std::uint8_t
{
    None,
    Frame,
    Part,
    Anim,
    Body,
    Sequence,
    Building
};

const char *SpriteKindName(SpriteKind kind);

struct SpriteRef
{
    SpriteKind kind {SpriteKind::None};
    int handle {-1};
    bool Valid() const
    {
        return kind != SpriteKind::None && handle >= 0;
    }
};

struct SpriteFamily
{
    SpriteKind kind {SpriteKind::None};
    int start {-1};
    int count {0};
    SpriteRef At(int i) const
    {
        return (i >= 0 && i < count) ? SpriteRef {kind, start + i} : SpriteRef {};
    }
};

enum class SpriteRecolor : std::uint8_t
{
    None,
    Blue,
    Red
};

struct SpriteImage
{
    std::string object;
    std::string path;           // asset path of the PNG
    std::string resistancePath; // the same layout for the Resistance; empty: `path` for everyone
    float scale {1.0f};
    bool terrain {false}; // map tiles: kept sharp-edged when scaled (neighbouring tiles must not bleed in)
    SpriteRecolor recolor {SpriteRecolor::None};
};

// The compiled tables, in the layout TextureManager / Renderer use. SpriteFrame::page indexes `images`.
struct SpriteTables
{
    std::vector<SpriteImage> images;
    std::vector<SpriteFrame> frames;
    std::vector<IndexRange> compositeRanges;
    std::vector<CompositePart> compositeParts;
    std::vector<IndexRange> animRanges;
    std::vector<AnimFrame> animFrames;
    std::vector<IndexRange> bodyRanges;
    std::vector<UnitLayer> bodyLayers;
    std::vector<IndexRange> sequenceRanges;
    std::vector<UnitSequenceStep> sequenceSteps;
    std::vector<BuildingDef> buildings;
    std::vector<BuildingPart> buildingParts;
};

class SpriteRegistry
{
public:
    // Every "sprite_<name>_meta.yaml" in `dir` (an asset path such as "assets/sprites"); then Compile()
    bool LoadDirectory(const std::string &dir);
    // One object from an already parsed meta; `dir` is where its image lives
    void AddObject(const std::string &name, const YAML::Node &meta, const std::string &dir);
    // Resolves every name; false when something did not resolve (see Errors(), the rest is still usable)
    bool Compile();
    void Clear();

    const SpriteTables &Tables() const
    {
        return m_tables;
    }
    SpriteTables &MutableTables()
    {
        return m_tables;
    }
    const std::vector<std::string> &Errors() const
    {
        return m_errors;
    }

    bool HasObject(const std::string &name) const;
    std::vector<std::string> ObjectNames() const;
    // The items of one kind of an object, in file order; its families
    std::vector<std::string> ItemNames(const std::string &object, SpriteKind kind) const;
    std::vector<std::string> FamilyNames(const std::string &object) const;
    int ImageOf(const std::string &object) const;

    // "<object>.<item>", "<object>.<family>" (its first member) or "<object>.<family>[i]"; `expected` None = any
    SpriteRef Resolve(const std::string &ref, SpriteKind expected = SpriteKind::None) const;
    const SpriteFamily *FindFamily(const std::string &object, const std::string &family) const;
    // Name of the item behind a handle ("<object>.<item>"), for editors and messages; empty when unknown
    std::string NameOf(SpriteRef ref) const;

private:
    struct Entry
    {
        SpriteKind kind {SpriteKind::None};
        std::string ref; // as written: "item" or "object.item"
        int ox {0};
        int oy {0};
        int flag {0};
        int layer {0};
        int state {0};
    };
    struct Item
    {
        SpriteKind kind {SpriteKind::None};
        std::string name;
        // Frame
        float rect[4] {0, 0, 0, 0}; // pixels
        float size[2] {-1, -1};     // game units; < 0: rect / scale
        int palette {0};
        int recolor {-1}; // -1: the object's
        // Building
        BuildingStyle style {BuildingStyle::Structure};
        bool tall {false};
        int width {1};
        int height {1};
        std::vector<Entry> entries;
        int handle {-1};
    };
    struct FamilyDef
    {
        std::string name;
        SpriteKind kind {SpriteKind::None};
        std::vector<std::string> members;
        SpriteFamily compiled;
    };
    struct Object
    {
        std::string name;
        SpriteImage image;
        int imageIndex {-1};
        std::vector<Item> items;
        std::map<std::string, std::size_t> byName[7]; // per SpriteKind
        std::vector<FamilyDef> families;
    };

    const FamilyDef *FindFamilyDef(const Object &obj, const std::string &name) const;
    SpriteRef FamilyMember(const Object &obj, const std::string &name, SpriteKind kind) const;
    // `ref` ("item" or "object.item") as seen from `fromObject`: the item itself, never a family
    const Item *LookupItem(const std::string &fromObject, const std::string &ref, SpriteKind kind) const;
    // An item or, failing that, a family's first member; reports the failure
    SpriteRef LookupRef(const std::string &fromObject, const std::string &ref, SpriteKind kind, const std::string &where);
    int Allocate(SpriteKind kind);
    void Emit(const std::string &object, const Item &item, int handle);
    void Error(const std::string &msg);

    std::map<std::string, Object> m_objects;
    std::vector<std::string> m_errors;
    SpriteTables m_tables;
    std::map<std::pair<int, int>, std::string> m_names; // (kind, handle) -> "<object>.<item>"
};

// The game's sprites (every meta in the sprites folder of the manifest), compiled on first use. Configs resolve their
// sprite names through it; the TextureManager draws from its tables.
SpriteRegistry &Sprites();
// Compiles the sprites folder again (the editors after a save); handles of unchanged files stay the same
void ReloadSprites();
// Counts the loads of the sprites: what keeps handles compares it to know they are still valid
int SpritesGeneration();

// A handle the code names by a fixed sprite name, resolved again after every load of the sprites
class CachedSprite
{
public:
    CachedSprite(const char *ref, SpriteKind kind)
        : m_ref(ref)
        , m_kind(kind)
    {}
    int Get() const;
    operator int() const
    {
        return Get();
    }

private:
    const char *m_ref;
    SpriteKind m_kind;
    mutable int m_handle {-1};
    mutable int m_generation {-1};
};

// A sprite reference of a config ("<object>.<item>", "<object>.<family>", "<object>.<family>[i]") as a handle of
// `kind`; -1 (with one message naming `context`) when it does not resolve. An empty name is -1 without a message.
int SpriteHandle(const std::string &ref, SpriteKind kind, const std::string &context = {});
// The same for a YAML value: a string reference; anything else (a number of the old sheet sprites) is reported
int SpriteHandle(const YAML::Node &node, SpriteKind kind, const std::string &context = {});
// A list of references (a single string is a list of one); unresolved ones are left out
std::vector<int> SpriteHandles(const YAML::Node &node, SpriteKind kind, const std::string &context = {});
// The name to write back into a config for a handle ("" for -1 / unknown)
std::string SpriteName(SpriteKind kind, int handle);

} // namespace OpenArtOfWar
