#pragma once

#include "core/LoaderResources.hpp"

#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "core/Config.hpp"

namespace OpenArtOfWar
{

struct SpriteFrame
{
    int id {0};
    int page {0};
    // In the units of the original sheet (GetSheetScale pixels each on an HD sheet): fractional, so a rectangle can
    // sit on any pixel of the sheet as it is
    float x {0.0f};
    float y {0.0f};
    float width {0.0f};
    float height {0.0f};
    int palette {0};
    // The pixels in the image (per-object sprites): srcW > 0 means use these instead of x/y/width/height * sheet scale
    float srcX {0.0f};
    float srcY {0.0f};
    float srcW {0.0f};
    float srcH {0.0f};
    // Team armour on this frame the army colour recolours: 0 none, 1 blue (Confederation), 2 red (Resistance)
    std::uint8_t recolor {0};
};

struct IndexRange
{
    std::int16_t start {0};
    std::int16_t end {-1};
};

struct CompositePart
{
    std::int16_t child {0};
    std::int16_t ox {0};
    std::int16_t oy {0};
    std::uint8_t isComp {0};
};

struct AnimFrame
{
    std::int16_t child {0};
    std::int8_t ox {0};
    std::int8_t oy {0};
    std::uint8_t isComp {0};
};

struct UnitLayer
{
    std::int16_t child {0};
    std::int8_t ox {0};
    std::int8_t oy {0};
    std::uint8_t isComp {0};
    std::uint8_t flag {0};
};

struct UnitSequenceStep
{
    std::int16_t bodyId {0};
    std::int8_t ox {0};
    std::int8_t oy {0};
};

struct BuildingPart
{
    std::int16_t child {0};
    std::int16_t ox {0};
    std::int8_t oy {0};
    std::uint8_t isComp {0};
    std::uint8_t layer {0};
    std::uint8_t state {0};
};

// How a building's parts are drawn (it used to follow from the building's position in the table)
enum class BuildingStyle : std::uint8_t
{
    Structure, // faction buildings: damage states, rotating turret / muzzle, power icon, animated parts
    Wall,      // walls and the construction scaffolding: state 0 parts by layer, animated parts, army colour
    Decor,     // map decorations: state 0 parts by layer, no animation
    DecorFlat  // map decorations drawn in one pass (every part with layer 0)
};

struct BuildingDef
{
    std::int16_t start {0};
    std::int16_t end {-1};
    std::uint8_t width {1};
    std::uint8_t height {1};
    BuildingStyle style {BuildingStyle::Structure};
    bool tall {false}; // decorations that rise above their rows (drawn later so units pass behind them)
};

class TextureManager
{
public:
    TextureManager(SDL_Renderer *renderer);
    ~TextureManager();

    bool LoadAllAssets();
    SDL_Texture *LoadTexture(const std::string &filePath);

    // Map graphics (sprite sheets and tilesets): smooth when scaled (true) or hard pixel edges (false).
    // `sharpPixels` (pixel-art filter of the shader effects) needs linear samples on the sprite sheets but
    // keeps the terrain sheets as they are: their tiles touch each other in the atlas, and linear samples
    // at a tile edge would pull in the neighbouring tile as a faint grid line.
    // Smoothing of atlas pages uses the pixel-art mode (SDL 3.4+): plain linear samples bleed the neighbouring
    // frames and the transparent borders into every sprite and tile, which shows as seams and cut lines.
    void SetSpriteFiltering(bool linear, bool sharpPixels = false);
    // Scale mode of "smooth" scaling of pixel graphics: pixel-art when SDL has it, linear otherwise
    static SDL_ScaleMode SmoothPixelScaleMode();
    SDL_Texture *GetSpriteSheet(int page) const;
    // Sprite sheet `page` with team armour (team 1: blue, team 2: red) recoloured to `color` on CPU and cached
    SDL_Texture *GetRecoloredSpriteSheet(int page, int team, SDL_Color color);
    float GetSheetScale(int page) const;
    // Width of a sprite sheet of the original game (the sheets may be enlarged: scale = width / this); 0 when unknown
    static int BaseSheetWidth(int page);
    SDL_Texture *GetTileset(int index) const;
    SDL_Texture *GetUiTexture(const std::string &name) const;
    const SpriteFrame *GetSpriteFrame(int spriteId) const;
    bool UpdateSpriteFrame(int spriteId, int page, float x, float y, float width, float height, int palette = -1);

    // Average color of a map tile's picture (for minimap-like previews); computed from the sprite sheet
    // pixels on first use and cached
    SDL_Color GetTileColor(int tilesetId, int tileIdx);

    // Graphics data lookups
    IndexRange GetCompositeRange(int compId) const;
    const CompositePart *GetCompositePart(int index) const;

    IndexRange GetAnimRange(int animId) const;
    const AnimFrame *GetAnimFrame(int index) const;

    IndexRange GetUnitBodyRange(int bodyId) const;
    const UnitLayer *GetUnitLayer(int index) const;

    IndexRange GetUnitSequenceRange(int seqId) const;
    const UnitSequenceStep *GetUnitSequenceStep(int index) const;

    const BuildingDef *GetBuildingDef(int buildingType) const;
    const BuildingPart *GetBuildingPart(int index) const;

    int GetTileCompositeId(int tilesetId, int tileIdx) const;
    int GetDecorCompositeId(int tilesetId, int decorType) const;
    int GetDecorBuildingId(int tilesetId, int decorType) const;
    int GetUnitBaseSeq(int unitType, int actionState) const;
    // Where the rotating turret layer sits for a hull facing (idle sequence, first step), relative to
    // the unit's draw origin; false when the unit has no turret layer
    bool GetTurretAnchor(int unitType, int hullDir8, float &outX, float &outY) const;
    const std::vector<std::int16_t> &GetDecorAnims() const
    {
        return m_decorAnims;
    }

    void Cleanup();

private:
    bool LoadGraphicsData(const std::string &jsonPath);
    SDL_Surface *SheetSurface(int page);
    void SumCompositeColor(int compId, int depth, long long &r, long long &g, long long &b, long long &weight);

    SDL_Renderer *m_renderer {nullptr};
    std::unordered_map<std::string, SDL_Texture *> m_textureCache;
    std::vector<SDL_Texture *> m_spriteSheets;
    std::unordered_map<std::uint64_t, SDL_Texture *> m_recoloredSheets;
    SDL_ScaleMode m_terrainScaleMode {SDL_SCALEMODE_NEAREST};
    SDL_ScaleMode m_spriteScaleMode {SDL_SCALEMODE_NEAREST};
    std::vector<float> m_sheetScales;
    std::vector<SDL_Surface *> m_sheetSurfaces; // decoded pages, loaded by getTileColor only
    std::unordered_map<int, SDL_Color> m_tileColors;
    std::vector<SDL_Texture *> m_tilesets;
    std::vector<bool> m_terrainPages; // sheet pages that hold terrain tiles (worked out on first use)
    void CollectTerrainPages();
    std::unordered_map<std::string, SDL_Texture *> m_uiTextures;
    std::vector<SpriteFrame> m_spriteFrames;

    // Decoded graphics tables
    std::vector<IndexRange> m_compositeRanges;
    std::vector<CompositePart> m_compositeParts;

    std::vector<IndexRange> m_animRanges;
    std::vector<AnimFrame> m_animFrames;

    std::vector<IndexRange> m_unitBodyRanges;
    std::vector<UnitLayer> m_unitLayers;

    std::vector<IndexRange> m_unitSequenceRanges;
    std::vector<UnitSequenceStep> m_unitSteps;

    std::vector<BuildingDef> m_buildings;
    std::vector<BuildingPart> m_buildingParts;

    std::vector<std::vector<std::int16_t>> m_tilesetsList;
    std::vector<std::int16_t> m_decorOffsets;
    std::vector<std::vector<std::int16_t>> m_unitArchetypes;
    std::vector<std::int16_t> m_decorAnims;
};

} // namespace OpenArtOfWar
