#pragma once

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
        int x {0};
        int y {0};
        int width {0};
        int height {0};
        int palette {0};
    };

    struct IndexRange
    {
        int16_t start {0};
        int16_t end {-1};
    };

    struct CompositePart
    {
        int16_t child {0};
        int16_t ox {0};
        int16_t oy {0};
        uint8_t isComp {0};
    };

    struct AnimFrame
    {
        int16_t child {0};
        int8_t ox {0};
        int8_t oy {0};
        uint8_t isComp {0};
    };

    struct UnitLayer
    {
        int16_t child {0};
        int8_t ox {0};
        int8_t oy {0};
        uint8_t isComp {0};
        uint8_t flag {0};
    };

    struct UnitSequenceStep
    {
        int16_t bodyId {0};
        int8_t ox {0};
        int8_t oy {0};
    };

    struct BuildingPart
    {
        int16_t child {0};
        int16_t ox {0};
        int8_t oy {0};
        uint8_t isComp {0};
        uint8_t layer {0};
        uint8_t state {0};
    };

    struct BuildingDef
    {
        int16_t start {0};
        int16_t end {-1};
        uint8_t width {1};
        uint8_t height {1};
    };

    class TextureManager
    {
    public:
        TextureManager(SDL_Renderer *renderer);
        ~TextureManager();

        bool loadAllAssets(const std::string &assetRoot = "assets");
        SDL_Texture *loadTexture(const std::string &filePath);

        // Map graphics (sprite sheets and tilesets): smooth when scaled (true) or hard pixel edges (false).
        // `sharpPixels` (pixel-art filter of the shader effects) needs linear samples on the sprite sheets but
        // keeps the terrain sheets as they are: their tiles touch each other in the atlas, and linear samples
        // at a tile edge would pull in the neighbouring tile as a faint grid line.
        void setSpriteFiltering(bool linear, bool sharpPixels = false);
        SDL_Texture *getSpriteSheet(int page) const;
        float getSheetScale(int page) const;
        // Width of a sprite sheet of the original game (the sheets may be enlarged: scale = width / this); 0 when unknown
        static int baseSheetWidth(int page);
        SDL_Texture *getTileset(int index) const;
        SDL_Texture *getUiTexture(const std::string &name) const;
        const SpriteFrame *getSpriteFrame(int spriteId) const;

        // Average color of a map tile's picture (for minimap-like previews); computed from the sprite sheet
        // pixels on first use and cached
        SDL_Color getTileColor(int tilesetId, int tileIdx);

        // Graphics data lookups
        IndexRange getCompositeRange(int compId) const;
        const CompositePart *getCompositePart(int index) const;

        IndexRange getAnimRange(int animId) const;
        const AnimFrame *getAnimFrame(int index) const;

        IndexRange getUnitBodyRange(int bodyId) const;
        const UnitLayer *getUnitLayer(int index) const;

        IndexRange getUnitSequenceRange(int seqId) const;
        const UnitSequenceStep *getUnitSequenceStep(int index) const;

        const BuildingDef *getBuildingDef(int buildingType) const;
        const BuildingPart *getBuildingPart(int index) const;

        int getTileCompositeId(int tilesetId, int tileIdx) const;
        int getDecorCompositeId(int tilesetId, int decorType) const;
        int getDecorBuildingId(int tilesetId, int decorType) const;
        int getUnitBaseSeq(int unitType, int actionState) const;
        // Where the rotating turret layer sits for a hull facing (idle sequence, first step), relative to
        // the unit's draw origin; false when the unit has no turret layer
        bool getTurretAnchor(int unitType, int hullDir8, float &outX, float &outY) const;
        const std::vector<int16_t> &getDecorAnims() const
        {
            return m_decorAnims;
        }

        void cleanup();

    private:
        bool loadGraphicsData(const std::string &jsonPath);
        SDL_Surface *sheetSurface(int page);
        void sumCompositeColor(int compId, int depth, long long &r, long long &g, long long &b, long long &weight);

        SDL_Renderer *m_renderer {nullptr};
        std::unordered_map<std::string, SDL_Texture *> m_textureCache;
        std::vector<SDL_Texture *> m_spriteSheets;
        std::vector<float> m_sheetScales;
        std::string m_assetRoot;
        std::vector<SDL_Surface *> m_sheetSurfaces; // decoded pages, loaded by getTileColor only
        std::unordered_map<int, SDL_Color> m_tileColors;
        std::vector<SDL_Texture *> m_tilesets;
        std::vector<bool> m_terrainPages; // sheet pages that hold terrain tiles (worked out on first use)
        void collectTerrainPages();
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

        std::vector<std::vector<int16_t>> m_tilesetsList;
        std::vector<int16_t> m_decorOffsets;
        std::vector<std::vector<int16_t>> m_unitArchetypes;
        std::vector<int16_t> m_decorAnims;
    };

} // namespace OpenArtOfWar
