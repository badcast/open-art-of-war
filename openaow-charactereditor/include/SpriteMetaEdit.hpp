#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace CharEditor
{

struct MetaLayerRef
{
    int index {0};
    std::string kind;   // "frame", "part", "anim", "seq"
    std::string target; // raw reference without quotes (e.g. "s393", "rot_c731[0]")
    float x {0.0f};
    float y {0.0f};
    bool hasAt {false};
    int flag {0};
    int state {0};
};

// Reads layer entries for `(section, itemName)` from meta YAML text.
// `section` may be "parts", "bodies", "anims", or "buildings" (reads `buildings[itemName].parts`).
std::vector<MetaLayerRef> ReadMetaItemLayers(
    const std::string &metaText, std::string_view section, std::string_view itemName);

// Resolves family members (e.g. "rot_c731", "parts" -> ["c731", ..., "c738"]).
std::vector<std::string> ReadMetaFamilyMembers(
    const std::string &metaText, std::string_view familyName, std::string_view memberKey);

// Updates `at: [x, y]` for `layerIndex` in `(section, itemName)` while preserving comments and layout.
bool SetMetaLayerAt(
    std::string &metaText,
    std::string_view section,
    std::string_view itemName,
    int layerIndex,
    float x,
    float y);

// Shifts all layers in `partName` by `(-pivotX, -pivotY)` so local `(pivotX, pivotY)` sits at `[0, 0]`.
// When `syncMatchingParts` is true, any other part containing a primary layer from `partName` at the
// same pre-shift `at:` offset is shifted uniformly so firing/overlay variants stay locked to it.
bool ShiftPartPivot(
    std::string &metaText,
    std::string_view partName,
    float pivotX,
    float pivotY,
    bool syncMatchingParts = true);

// Shifts `layerIndex` in `bodyName` by `(dx, dy)`. When `syncSiblingRot` is true and the target layer
// is a `rot_*[slot]` layer, sibling `rot_*[slot]` layers (e.g. firing `rot_a125[i]` paired with `rot_c731[i]`)
// are shifted by the same `(dx, dy)`.
bool ShiftBodyLayer(
    std::string &metaText,
    std::string_view bodyName,
    int layerIndex,
    float dx,
    float dy,
    bool syncSiblingRot = true);

} // namespace CharEditor
