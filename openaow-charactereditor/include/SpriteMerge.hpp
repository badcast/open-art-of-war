#pragma once
// Merging sprite objects (assets/sprites/sprite_<name>_meta.yaml) into one: the names of the items, the merged meta
// and the references in the other asset files. No ImGui / SDL here, so the tests can use it.

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace CharEditor::SpriteMerge
{

struct Source
{
    std::string object;
    YAML::Node meta;
};

// Per source object: every name it defines (items of every kind and families) -> its name in the merged object.
// The first source keeps all its names; a later one gets "<object>_<name>" (plus a number) where a name is taken.
using NameMap = std::map<std::string, std::map<std::string, std::string>>;

// A meta whose "image_size" differs from the image's real size (`w` x `h`): the frame rectangles to the real pixels,
// the drawn sizes kept (evenly scaled: the scale follows; otherwise each frame gets its size); image_size = w, h
void FitMetaToImage(YAML::Node meta, int w, int h);

// The names an object defines: keys of frames / parts / anims / bodies / sequences / buildings / families
std::vector<std::string> DefinedNames(const YAML::Node &meta);
NameMap BuildNameMap(const std::vector<Source> &sources);

// A reference as written in the meta of `fromObject` ("item", "item[i]", "object.item[i]"), inside the merged
// object: references into a merged source lose their prefix and follow the renames; others stay as they are
std::string RewriteMetaRef(const std::string &ref, const std::string &fromObject, const NameMap &names);

// Where each frame goes in the merged image: (object, frame) -> x, y
using FramePlaces = std::map<std::pair<std::string, std::string>, std::array<int, 2>>;

// The merged meta: image `image` of imageW x imageH (and `resistanceImage` when not empty); frames at their places,
// with the size and recolor they had (written out where the merged object's defaults differ); every other section
// copied with its references rewritten. Scale, recolor and terrain come from the first source (fitted to its image).
YAML::Node MergeMetas(
    const std::vector<Source> &sources,
    const NameMap &names,
    const FramePlaces &places,
    const std::string &image,
    const std::string &resistanceImage,
    int imageW,
    int imageH);
std::string EmitMeta(const YAML::Node &meta, const std::string &header);

// References in another asset file: "<source>.<item>" (and "<source>.<item>[i]") -> "<merged>.<new item>" where the
// item is one of the source's names; a whole line "sprite: <source>" / "reinforced_sprite: <source>" -> the merged
// object. `count` gets the number of replacements.
std::string RewriteText(const std::string &text, const NameMap &names, const std::string &merged, int &count);

// True when a line of `text` is "sprite: <object>" (units bind their object by its bare name)
bool BindsBare(const std::string &text, const std::string &object);

// Shelf packing, largest first: the top left corner of each size, `padding` pixels apart
std::vector<std::array<int, 2>> ShelfPack(const std::vector<std::array<int, 2>> &sizes, int padding);

// The same sizes in a random order (rows filled in it); a new seed gives another arrangement
std::vector<std::array<int, 2>> ShufflePack(const std::vector<std::array<int, 2>> &sizes, int padding, unsigned seed);

// A tidy grid: equal cells, the sizes' order kept, each frame centered in its cell, close to a square
std::vector<std::array<int, 2>> GridPack(const std::vector<std::array<int, 2>> &sizes, int padding);

// Width and height of the image that holds the frames at `pos`
std::array<int, 2> PackExtent(const std::vector<std::array<int, 2>> &sizes, const std::vector<std::array<int, 2>> &pos);

} // namespace CharEditor::SpriteMerge
