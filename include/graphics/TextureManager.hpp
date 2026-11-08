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

// What a unit archetype is drawn from: families of its sprite object (families "<set>idle", "<set>move", ...)
struct UnitVisual
{
    int idle {-1}; // first sequence; 8 facings when idleCount is 8, a fixed hull when it is 1
    int idleCount {0};
    int attack {-1}; // firing pose by facing (infantry); -1: fires from the idle sequences
    int move {-1};
    int moveCount {0};
    int die {-1};
    int transform {-1}; // siege transformation; -1: none
    int icon {-1};      // cameo composite
};

class TextureManager
{
public:
    TextureManager(SDL_Renderer *renderer);
    ~TextureManager();

    bool LoadAllAssets();
    // The sprites again from disk (metas and images; the editors after a change), with the bindings of the characters
    void ReloadSprites();
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
    // The image of a sprite object (SpriteFrame::page); pages from ImageCount() on are the Resistance images
    SDL_Texture *GetSpriteSheet(int page) const;
    // Image `page` with team armour (team 1: blue, team 2: red) recoloured to `color` on CPU and cached
    SDL_Texture *GetRecoloredSpriteSheet(int page, int team, SDL_Color color);
    int ImageCount() const
    {
        return m_imageCount;
    }
    // The page to draw `page` from for the Resistance: its Resistance image when the object has one
    int ResistancePage(int page) const;
    SDL_Texture *GetTileset(int index) const;
    SDL_Texture *GetUiTexture(const std::string &name) const;
    const SpriteFrame *GetSpriteFrame(int spriteId) const;

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

    // Tilesets are the sprite objects "tileset_<id>" (families "tiles" and "decor")
    int GetTileCompositeId(int tilesetId, int tileIdx) const;
    int GetDecorCompositeId(int tilesetId, int decorType) const;
    int GetDecorBuildingId(int tilesetId, int decorType) const;
    int GetTileCount(int tilesetId) const;
    int GetDecorCount(int tilesetId) const;
    // Unit archetypes and building types are bound to sprites by the character files ("sprite:")
    const UnitVisual *GetUnitVisual(int archetype) const;
    int GetUnitBaseSeq(int unitType, int actionState) const;
    // The building sprite of a building type (0-14 structures, 15-29 walls by damage); -1 unknown
    int GetBuildingSprite(int buildingType) const;
    // Where the rotating turret layer sits for a hull facing (idle sequence, first step), relative to
    // the unit's draw origin; false when the unit has no turret layer
    bool GetTurretAnchor(int unitType, int hullDir8, float &outX, float &outY) const;

    void Cleanup();

private:
    void LoadSprites();
    void LoadSpriteBindings();
    SDL_Surface *SheetSurface(int page);
    void SumCompositeColor(int compId, int depth, long long &r, long long &g, long long &b, long long &weight);

    SDL_Renderer *m_renderer {nullptr};
    std::unordered_map<std::string, SDL_Texture *> m_textureCache;
    std::vector<SDL_Texture *> m_spriteSheets;
    std::unordered_map<std::uint64_t, SDL_Texture *> m_recoloredSheets;
    SDL_ScaleMode m_terrainScaleMode {SDL_SCALEMODE_NEAREST};
    SDL_ScaleMode m_spriteScaleMode {SDL_SCALEMODE_NEAREST};
    std::vector<std::string> m_imagePaths; // per page (Resistance pages included)
    int m_imageCount {0};
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

    struct TilesetSprites
    {
        int tiles {-1};
        int tileCount {0};
        int decor {-1};
        int decorCount {0};
    };
    std::vector<TilesetSprites> m_tilesetSprites;
    std::vector<UnitVisual> m_unitVisuals; // by archetype
    std::vector<int> m_buildingSprites;    // by building type
};

} // namespace OpenArtOfWar
