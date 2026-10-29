#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace CharEditor {

namespace fs = std::filesystem;

// What the single sprite export splits the sprites into; a sprite belongs to the first group that uses it
enum class SpriteGroup { Units = 0, Buildings, Terrain, Other };
constexpr int kSpriteGroupCount = 4;
const char* spriteGroupName(SpriteGroup group); // gettext msgid, e.g. "Units"
const char* spriteGroupSlug(SpriteGroup group); // file name part, e.g. "units"

// One rectangle of a sprite sheet (assets/characters/sprites.json)
struct RawSprite {
    int id{0};
    int page{0};
    int x{0};
    int y{0};
    int w{0};
    int h{0};
    int palette{0};
};

enum class SpriteObjectKind { Unit, Building, Tileset, Misc };

// One thing of the game with everything it is drawn from: the roots of its dependency tree and the closure of
// what they use. Sprites, composites and animations may be shared by several objects.
struct SpriteObject {
    SpriteObjectKind kind{SpriteObjectKind::Misc};
    SpriteGroup group{SpriteGroup::Other};
    std::string kindName;  // "unit" / "building" / "tileset" / "misc"
    std::string faction;   // "Resistance" / "Confederation" / "Neutral"
    int id{0};             // archetype id / building index / tileset number / serial of a misc object
    std::string name;
    std::string slug;
    std::vector<int> rootSequences, rootComposites, rootAnims, rootBuildings;
    int thumbSequence{-1};  // units: the sequence that shows the object best (standing, facing the viewer)
    std::set<int> sequences, bodies, composites, anims, buildings, sprites;
};

// A line of the dependency tree shown in the editor
struct DepNode {
    std::string label;
    std::string kind; // "sequence" "step" "body" "layer" "part" "composite" "anim" "building" "sprite"
    int id{-1};
    int ox{0};
    int oy{0};
    std::vector<DepNode> children;
};

// The sprite data of the game (sprites.json + graphics_data.json) grouped into objects. No SDL, no ImGui:
// it can be checked without a window.
class SpriteCatalog {
public:
    // `assetsRoot` is the assets folder (with characters/, data/, sprites/)
    bool load(const fs::path& assetsRoot, std::string& error);

    const std::vector<RawSprite>& sprites() const { return rawSprites; }
    const std::vector<SpriteObject>& objects() const { return objs; }
    SpriteGroup groupOfSprite(int spriteId) const;
    // The sprites one unit sequence is drawn from
    std::set<int> spritesOfSequence(int sequenceId) const;

    // Dependency tree of an object (depth is limited; shared parts are shown where they are used)
    DepNode tree(const SpriteObject& object) const;
    // Everything the object is made of as JSON: roots and the raw tables of sequences, bodies, composites,
    // animations and buildings, enough to rebuild the graphics data of the object
    nlohmann::json describe(const SpriteObject& object) const;

private:
    void collectComposite(int id, SpriteObject& out, int depth) const;
    void collectAnim(int id, SpriteObject& out, int depth) const;
    void collectBody(int id, SpriteObject& out) const;
    void collectSequence(int id, SpriteObject& out) const;
    void collectBuilding(int id, SpriteObject& out) const;
    void collectChild(int kind, int id, SpriteObject& out, int depth) const;
    void finish(SpriteObject& object) const;

    DepNode compositeNode(int id, int depth) const;
    DepNode animNode(int id) const;
    DepNode childNode(int kind, int id, int ox, int oy, int depth) const;

    nlohmann::json gd;                       // graphics_data.json
    std::vector<RawSprite> rawSprites;
    std::vector<SpriteObject> objs;
    std::vector<uint8_t> spriteGroup;        // per sprite id
    int compCount{0}, animCount{0}, bodyCount{0}, seqCount{0}, buildingCount{0};
};

// Digest of one entry of the graphics tables as exported (without its "digest" field): the import applies an entry
// only when its content no longer matches it, so entries left alone never undo an edit of the same entry elsewhere
std::string graphicsDigest(nlohmann::json entry);

// Lower case letters and digits, the rest becomes '_' ("TM-40 Fortress" -> "tm_40_fortress")
std::string spriteSlug(const std::string& text);

} // namespace CharEditor
