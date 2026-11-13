#pragma once
// Merging sprite objects (assets/sprites/sprite_<name>_meta.yaml) into one: the names of the items, the merged meta
// and the references in the other asset files. No ImGui / SDL here, so the tests can use it.

#include <array>
#include <map>
#include <set>
#include <string>
#include <string_view>
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
std::string RewriteMetaRef(std::string_view ref, std::string_view fromObject, const NameMap &names);

// Where each frame goes in the merged image: (object, frame) -> x, y
using FramePlaces = std::map<std::pair<std::string, std::string>, std::array<int, 2>>;

// The merged meta: image `image` of imageW x imageH (and `resistanceImage` when not empty); frames at their places,
// with the size and recolor they had (written out where the merged object's defaults differ); every other section
// copied with its references rewritten. Scale, recolor and terrain come from the first source (fitted to its image).
YAML::Node MergeMetas(
    const std::vector<Source> &sources,
    const NameMap &names,
    const FramePlaces &places,
    std::string_view image,
    std::string_view resistanceImage,
    int imageW,
    int imageH);
std::string EmitMeta(const YAML::Node &meta, std::string_view header);

// References in another asset file: "<source>.<item>" (and "<source>.<item>[i]") -> "<merged>.<new item>" where the
// item is one of the source's names; when `rewriteBare` is true, a whole line "sprite: <source>" /
// "reinforced_sprite: <source>" -> `merged`. `count` gets the number of replacements.
std::string RewriteText(
    std::string_view text, const NameMap &names, std::string_view merged, int &count, bool rewriteBare = true);

// Splits `movedFrames` (and any parts/anims/bodies/sequences/buildings/families whose local dependencies lie solely
// inside the moved set) out of `srcMeta` (`srcObject`) into a new meta `newObjectMeta` (`newObject`), rewriting cross
// references cleanly between the two objects.
struct SplitResult
{
    YAML::Node remainingSourceMeta;
    YAML::Node newObjectMeta;
    std::set<std::string> movedItems;
    bool movedEverything {false};
};
SplitResult SplitMeta(
    std::string_view srcObject,
    const YAML::Node &srcMeta,
    const std::set<std::string> &movedFrames,
    const FramePlaces &newPlaces,
    std::string_view newObject,
    std::string_view newImage,
    std::string_view newResistanceImage,
    int newW,
    int newH);

// True when a line of `text` is "sprite: <object>" (units bind their object by its bare name)
bool BindsBare(std::string_view text, std::string_view object);

// Shelf packing, largest first: the top left corner of each size, `padding` pixels apart
std::vector<std::array<int, 2>> ShelfPack(const std::vector<std::array<int, 2>> &sizes, int padding);

// The same sizes in a random order (rows filled in it); a new seed gives another arrangement
std::vector<std::array<int, 2>> ShufflePack(const std::vector<std::array<int, 2>> &sizes, int padding, unsigned seed);

// A tidy grid: equal cells, the sizes' order kept, each frame centered in its cell, close to a square
std::vector<std::array<int, 2>> GridPack(const std::vector<std::array<int, 2>> &sizes, int padding);

// --- Assemble: the frames laid out the way the game puts them together ---
// One composite of the meta ("parts: name: [{frame, at}, ...]"): its frames with their offsets in image pixels
struct AssemblyPiece
{
    int frame {0}; // index into the frame list
    int x {0}, y {0};
};
using AssemblyGroup = std::vector<AssemblyPiece>;
// The pictures of an object's meta as the game puts them together, made of its own frames: every unit body and every
// building first (parts, the first picture of an animation and family members opened up, offsets added), then every
// composite (parts) on its own. `frameIndex`: frame name -> index; `scale`: pixels per game unit for the offsets
std::vector<AssemblyGroup> AssemblyGroupsFromMeta(
    const YAML::Node &meta, std::string_view object, const std::map<std::string, int> &frameIndex, float scale);
// Where each frame goes: every picture becomes one block with its frames at their offsets (a frame goes with the
// first picture that uses it; a frame the game draws over another one goes to the nearest free place beside where it
// is drawn), the blocks and the frames no picture uses are shelf-packed, `padding` pixels apart
std::vector<std::array<int, 2>> AssemblePack(
    const std::vector<std::array<int, 2>> &sizes, const std::vector<AssemblyGroup> &groups, int padding);

// Width and height of the image that holds the frames at `pos`
std::array<int, 2> PackExtent(const std::vector<std::array<int, 2>> &sizes, const std::vector<std::array<int, 2>> &pos);

// --- Image size ---
// The image with its longer side set to `n` pixels, the aspect ratio kept: {width, height}
std::array<int, 2> FitToLongestSide(int w, int h, int n);
// A free size ({newW, newH} typed by hand): with `keepAspect` the side that was not edited follows the other
std::array<int, 2> CustomSize(int w, int h, int newW, int newH, bool keepAspect, bool changedW);

// Scaled layout for a selected subset of frames around `(anchorX, anchorY)`. If scaling causes overlap with
// `unselected` or previously disjoint selected frames, selected frames are shelf-packed cleanly below `unselected`
// and `canvasW`/`canvasH` grow to hold the result.
struct ScaledGroupLayout
{
    std::vector<std::array<int, 4>> rects;
    int canvasW {1};
    int canvasH {1};
    bool relocated {false};
};
ScaledGroupLayout ScaleGroupRects(
    const std::vector<std::array<int, 4>> &selected,
    const std::vector<std::array<int, 4>> &unselected,
    float scaleX,
    float scaleY,
    int anchorX,
    int anchorY,
    int canvasW,
    int canvasH,
    int padding);
// The largest k such that the RGBA image (w x h) is an exact upscale by k: every aligned k x k block of equal
// pixels, and every frame rectangle {x, y, w, h} on that grid. 1 when it is not
int DetectIntegerUpscale(const unsigned char *rgba, int w, int h, const std::vector<std::array<int, 4>> &rects);
// The image reduced k times by taking one pixel of each k x k block (no pixel is invented or lost for an exact upscale)
std::vector<unsigned char> DownscaleInteger(const unsigned char *rgba, int w, int h, int k);

// --- Magnetism ---
// How far to move a span [a0, a1] (a frame's left / right or top / bottom edge) so that one of its edges meets the
// nearest of `targets` (image borders, edges of the other frames), when that is within `threshold` pixels; else 0.
// `snappedTo` gets the target met (when not null and the span snaps)
int SnapDelta(int a0, int a1, const std::vector<int> &targets, int threshold, int *snappedTo = nullptr);

} // namespace CharEditor::SpriteMerge
