#include "graphics/TextureManager.hpp"
#include "core/AssetStore.hpp"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar
{

    TextureManager::TextureManager(SDL_Renderer *renderer) : m_renderer(renderer)
    {
    }

    TextureManager::~TextureManager()
    {
        cleanup();
    }

    void TextureManager::cleanup()
    {
        for(auto &[path, tex] : m_textureCache)
        {
            if(tex)
            {
                SDL_DestroyTexture(tex);
            }
        }
        m_textureCache.clear();
        for(SDL_Surface *surface : m_sheetSurfaces)
        {
            if(surface)
                SDL_DestroySurface(surface);
        }
        m_sheetSurfaces.clear();
        m_tileColors.clear();
        m_spriteSheets.clear();
        m_tilesets.clear();
        m_uiTextures.clear();
        m_spriteFrames.clear();

        m_compositeRanges.clear();
        m_compositeParts.clear();
        m_animRanges.clear();
        m_animFrames.clear();
        m_unitBodyRanges.clear();
        m_unitLayers.clear();
        m_unitSequenceRanges.clear();
        m_unitSteps.clear();
        m_buildings.clear();
        m_buildingParts.clear();
        m_tilesetsList.clear();
        m_decorOffsets.clear();
        m_unitArchetypes.clear();
        m_decorAnims.clear();
    }

    SDL_Texture *TextureManager::loadTexture(const std::string &filePath)
    {
        auto it = m_textureCache.find(filePath);
        if(it != m_textureCache.end())
        {
            return it->second;
        }

        // SDL3 decodes PNG natively; all game art is PNG
        std::vector<uint8_t> pngData;
        SDL_Surface *surface = nullptr;
        if(Assets::readBytes(filePath, pngData) && !pngData.empty())
        {
            surface = SDL_LoadPNG_IO(SDL_IOFromConstMem(pngData.data(), pngData.size()), true);
        }
        if(!surface)
        {
            std::cerr << "[TextureManager] Failed to load image: " << filePath << " (" << SDL_GetError() << ")" << std::endl;
            return nullptr;
        }

        SDL_Texture *texture = SDL_CreateTextureFromSurface(m_renderer, surface);
        SDL_DestroySurface(surface);
        if(texture)
        {
            // Pixel art: keep hard pixel edges when the camera zooms (SDL3 defaults to linear filtering)
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
        }

        if(!texture)
        {
            std::cerr << "[TextureManager] Failed to create texture: " << filePath << " (" << SDL_GetError() << ")" << std::endl;
            return nullptr;
        }

        m_textureCache[filePath] = texture;
        return texture;
    }

    bool TextureManager::loadGraphicsData(const std::string &jsonPath)
    {
        std::string jtext;
        if(!Assets::readText(jsonPath, jtext))
        {
            std::cerr << "[TextureManager] Error: could not open " << jsonPath << std::endl;
            return false;
        }

        try
        {
            nlohmann::json j;
            j = nlohmann::json::parse(jtext);

            // 1. Composite sprites
            const auto &comp = j["composite_sprites"];
            for(const auto &r : comp["ranges"])
            {
                m_compositeRanges.push_back({r[0].get<int16_t>(), r[1].get<int16_t>()});
            }
            for(const auto &p : comp["parts"])
            {
                m_compositeParts.push_back({p["child"].get<int16_t>(), p["ox"].get<int16_t>(), p["oy"].get<int16_t>(), static_cast<uint8_t>(p["is_comp"].get<int>())});
            }

            // 2. Animation sequences
            const auto &anim = j["anim_sequences"];
            for(const auto &r : anim["ranges"])
            {
                m_animRanges.push_back({r[0].get<int16_t>(), r[1].get<int16_t>()});
            }
            for(const auto &f : anim["frames"])
            {
                m_animFrames.push_back({f["child"].get<int16_t>(), static_cast<int8_t>(f["ox"].get<int>()), static_cast<int8_t>(f["oy"].get<int>()), static_cast<uint8_t>(f["is_comp"].get<int>())});
            }

            // 3. Unit bodies
            const auto &ub = j["unit_bodies"];
            for(const auto &r : ub["ranges"])
            {
                m_unitBodyRanges.push_back({r[0].get<int16_t>(), r[1].get<int16_t>()});
            }
            for(const auto &l : ub["layers"])
            {
                m_unitLayers.push_back({l["child"].get<int16_t>(), static_cast<int8_t>(l["ox"].get<int>()), static_cast<int8_t>(l["oy"].get<int>()), static_cast<uint8_t>(l["is_comp"].get<int>()), static_cast<uint8_t>(l["flag"].get<int>())});
            }

            // 4. Unit sequences
            const auto &us = j["unit_sequences"];
            for(const auto &r : us["ranges"])
            {
                m_unitSequenceRanges.push_back({r[0].get<int16_t>(), r[1].get<int16_t>()});
            }
            for(const auto &s : us["steps"])
            {
                m_unitSteps.push_back({s["body"].get<int16_t>(), static_cast<int8_t>(s["ox"].get<int>()), static_cast<int8_t>(s["oy"].get<int>())});
            }

            // 5. Buildings
            const auto &bd = j["buildings"];
            const auto &ranges = bd["ranges"];
            const auto &widths = bd["widths"];
            const auto &heights = bd["heights"];
            size_t bcount = ranges.size();
            for(size_t i = 0; i < bcount; ++i)
            {
                BuildingDef bdef;
                bdef.start = ranges[i][0].get<int16_t>();
                bdef.end = ranges[i][1].get<int16_t>();
                bdef.width = (i < widths.size()) ? static_cast<uint8_t>(widths[i].get<int>()) : 1;
                bdef.height = (i < heights.size()) ? static_cast<uint8_t>(heights[i].get<int>()) : 1;
                m_buildings.push_back(bdef);
            }
            for(const auto &p : bd["parts"])
            {
                m_buildingParts.push_back(
                    {p["child"].get<int16_t>(), p["ox"].get<int16_t>(), static_cast<int8_t>(p["oy"].get<int>()), static_cast<uint8_t>(p["is_comp"].get<int>()), static_cast<uint8_t>(p["layer"].get<int>()), p.contains("state") ? static_cast<uint8_t>(p["state"].get<int>()) : uint8_t {0}});
            }

            // 6. Tilesets
            for(const auto &ts : j["tilesets"])
            {
                std::vector<int16_t> tlist;
                for(const auto &val : ts)
                {
                    tlist.push_back(val.get<int16_t>());
                }
                m_tilesetsList.push_back(std::move(tlist));
            }

            if(j.contains("decor_offsets"))
            {
                for(const auto &val : j["decor_offsets"])
                {
                    m_decorOffsets.push_back(val.get<int16_t>());
                }
            }

            // 7. Unit archetypes
            for(const auto &row : j["unit_archetypes"])
            {
                std::vector<int16_t> rlist;
                for(const auto &val : row)
                {
                    rlist.push_back(val.get<int16_t>());
                }
                m_unitArchetypes.push_back(std::move(rlist));
            }

            // 8. Decor animations
            for(const auto &val : j["decor_anims"])
            {
                m_decorAnims.push_back(val.get<int16_t>());
            }

            std::cout << "[TextureManager] Loaded graphics data: " << m_compositeRanges.size() << " composites, " << m_animRanges.size() << " anims, " << m_buildings.size() << " buildings, " << m_tilesetsList.size() << " tilesets." << std::endl;
            return true;
        }
        catch(const std::exception &e)
        {
            std::cerr << "[TextureManager] Exception parsing graphics data: " << e.what() << std::endl;
            return false;
        }
    }

    bool TextureManager::loadAllAssets(const std::string &assetRoot)
    {
        std::cout << "[TextureManager] Loading game assets from: " << assetRoot << "..." << std::endl;

        // 1. Load sprite frame definitions from sprites.json
        std::string jsonPath = assetRoot + "/characters/sprites.json";
        std::string sprites;
        if(Assets::readText(jsonPath, sprites))
        {
            nlohmann::json j = nlohmann::json::parse(sprites);
            m_spriteFrames.reserve(j.size());
            for(const auto &item : j)
            {
                SpriteFrame f;
                f.id = item["id"].get<int>();
                f.page = item["page"].get<int>();
                f.x = item["x"].get<int>();
                f.y = item["y"].get<int>();
                f.width = item["width"].get<int>();
                f.height = item["height"].get<int>();
                f.palette = item["palette"].get<int>();
                m_spriteFrames.push_back(f);
            }
            std::cout << "[TextureManager] Loaded " << m_spriteFrames.size() << " sprite definitions." << std::endl;
        }
        else
        {
            std::cerr << "[TextureManager] Warning: could not open " << jsonPath << std::endl;
        }

        // 2. Load all 42 sprite sheets and compute dynamic HD resolution scaling

        m_assetRoot = assetRoot;
        m_spriteSheets.resize(42, nullptr);
        m_sheetScales.assign(42, 1.0f);
        for(int i = 0; i < 42; ++i)
        {
            std::ostringstream ss;
            ss << assetRoot << "/sprites/sheet_" << (i < 10 ? "0" : "") << i << ".png";
            m_spriteSheets[i] = loadTexture(ss.str());
            if(m_spriteSheets[i])
            {
                float tw = 0.0f, th = 0.0f;
                SDL_GetTextureSize(m_spriteSheets[i], &tw, &th);
                if(baseSheetWidth(i) > 0)
                {
                    m_sheetScales[i] = tw / static_cast<float>(baseSheetWidth(i));
                }
            }
        }

        // 3. Load UI screens
        m_uiTextures["logo"] = loadTexture(assetRoot + "/ui/logo.png");
        m_uiTextures["title"] = loadTexture(assetRoot + "/ui/title.png");
        m_uiTextures["icon"] = loadTexture(assetRoot + "/ui/icon.png");

        // 4. Load comprehensive graphics data (composites, animations, units, buildings, tilesets)
        std::string gdataPath = assetRoot + "/data/graphics_data.json";
        loadGraphicsData(gdataPath);

        std::cout << "[TextureManager] Asset textures loaded successfully." << std::endl;
        return true;
    }

    void TextureManager::collectTerrainPages()
    {
        m_terrainPages.assign(m_spriteSheets.size(), false);
        // Sprites of a composite, nested composites included
        std::vector<int> stack;
        for(const auto &tiles : m_tilesetsList)
        {
            for(int16_t comp : tiles)
                stack.push_back(comp);
        }
        std::vector<bool> seen;
        while(!stack.empty())
        {
            const int comp = stack.back();
            stack.pop_back();
            if(comp < 0)
                continue;
            if(comp >= static_cast<int>(seen.size()))
                seen.resize(comp + 1, false);
            if(seen[comp])
                continue;
            seen[comp] = true;
            const IndexRange r = getCompositeRange(comp);
            for(int p = r.start; p <= r.end; ++p)
            {
                const CompositePart *part = getCompositePart(p);
                if(!part)
                    continue;
                if(part->isComp)
                {
                    stack.push_back(part->child);
                }
                else if(const auto *frame = getSpriteFrame(part->child))
                {
                    if(frame->page >= 0 && frame->page < static_cast<int>(m_terrainPages.size()))
                        m_terrainPages[frame->page] = true;
                }
            }
        }
    }

    void TextureManager::setSpriteFiltering(bool linear, bool sharpPixels)
    {
        if(m_terrainPages.size() != m_spriteSheets.size())
            collectTerrainPages();
        const SDL_ScaleMode mode = linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
        const SDL_ScaleMode spriteMode = (linear || sharpPixels) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
        for(size_t i = 0; i < m_spriteSheets.size(); ++i)
        {
            if(m_spriteSheets[i])
                SDL_SetTextureScaleMode(m_spriteSheets[i], m_terrainPages[i] ? mode : spriteMode);
        }
        for(SDL_Texture *texture : m_tilesets)
        {
            if(texture)
                SDL_SetTextureScaleMode(texture, mode);
        }
    }

    SDL_Texture *TextureManager::getSpriteSheet(int page) const
    {
        if(page >= 0 && page < static_cast<int>(m_spriteSheets.size()))
        {
            return m_spriteSheets[page];
        }
        return nullptr;
    }

    int TextureManager::baseSheetWidth(int page)
    {
        static const int kWidths[42] = {240, 179, 206, 229, 200, 274, 268, 240, 77, 77, 107, 153, 91, 112, 223, 154, 193, 158, 135, 73, 65, 121, 91, 94, 106, 169, 68, 78, 126, 231, 272, 45, 166, 123, 122, 1, 61, 88, 50, 63, 98, 33};
        return page >= 0 && page < 42 ? kWidths[page] : 0;
    }

    float TextureManager::getSheetScale(int page) const
    {
        if(page >= 0 && page < static_cast<int>(m_sheetScales.size()))
        {
            return m_sheetScales[page];
        }
        return 1.0f;
    }

    SDL_Texture *TextureManager::getTileset(int index) const
    {
        if(index >= 0 && index < static_cast<int>(m_tilesets.size()))
        {
            return m_tilesets[index];
        }
        return nullptr;
    }

    SDL_Texture *TextureManager::getUiTexture(const std::string &name) const
    {
        auto it = m_uiTextures.find(name);
        if(it != m_uiTextures.end())
        {
            return it->second;
        }
        return nullptr;
    }

    const SpriteFrame *TextureManager::getSpriteFrame(int spriteId) const
    {
        if(spriteId >= 0 && spriteId < static_cast<int>(m_spriteFrames.size()))
        {
            return &m_spriteFrames[spriteId];
        }
        return nullptr;
    }

    IndexRange TextureManager::getCompositeRange(int compId) const
    {
        if(compId >= 0 && compId < static_cast<int>(m_compositeRanges.size()))
        {
            return m_compositeRanges[compId];
        }
        return {0, -1};
    }

    const CompositePart *TextureManager::getCompositePart(int index) const
    {
        if(index >= 0 && index < static_cast<int>(m_compositeParts.size()))
        {
            return &m_compositeParts[index];
        }
        return nullptr;
    }

    IndexRange TextureManager::getAnimRange(int animId) const
    {
        if(animId >= 0 && animId < static_cast<int>(m_animRanges.size()))
        {
            return m_animRanges[animId];
        }
        return {0, -1};
    }

    const AnimFrame *TextureManager::getAnimFrame(int index) const
    {
        if(index >= 0 && index < static_cast<int>(m_animFrames.size()))
        {
            return &m_animFrames[index];
        }
        return nullptr;
    }

    IndexRange TextureManager::getUnitBodyRange(int bodyId) const
    {
        if(bodyId >= 0 && bodyId < static_cast<int>(m_unitBodyRanges.size()))
        {
            return m_unitBodyRanges[bodyId];
        }
        return {0, -1};
    }

    const UnitLayer *TextureManager::getUnitLayer(int index) const
    {
        if(index >= 0 && index < static_cast<int>(m_unitLayers.size()))
        {
            return &m_unitLayers[index];
        }
        return nullptr;
    }

    IndexRange TextureManager::getUnitSequenceRange(int seqId) const
    {
        if(seqId >= 0 && seqId < static_cast<int>(m_unitSequenceRanges.size()))
        {
            return m_unitSequenceRanges[seqId];
        }
        return {0, -1};
    }

    const UnitSequenceStep *TextureManager::getUnitSequenceStep(int index) const
    {
        if(index >= 0 && index < static_cast<int>(m_unitSteps.size()))
        {
            return &m_unitSteps[index];
        }
        return nullptr;
    }

    const BuildingDef *TextureManager::getBuildingDef(int buildingType) const
    {
        if(buildingType >= 0 && buildingType < static_cast<int>(m_buildings.size()))
        {
            return &m_buildings[buildingType];
        }
        return nullptr;
    }

    const BuildingPart *TextureManager::getBuildingPart(int index) const
    {
        if(index >= 0 && index < static_cast<int>(m_buildingParts.size()))
        {
            return &m_buildingParts[index];
        }
        return nullptr;
    }

    SDL_Surface *TextureManager::sheetSurface(int page)
    {
        if(page < 0 || page >= 42)
            return nullptr;
        if(m_sheetSurfaces.empty())
            m_sheetSurfaces.assign(42, nullptr);
        if(!m_sheetSurfaces[page])
        {
            std::ostringstream ss;
            ss << m_assetRoot << "/sprites/sheet_" << (page < 10 ? "0" : "") << page << ".png";
            std::vector<uint8_t> png;
            if(!Assets::readBytes(ss.str(), png) || png.empty())
                return nullptr;
            SDL_Surface *loaded = SDL_LoadPNG_IO(SDL_IOFromConstMem(png.data(), png.size()), true);
            if(!loaded)
                return nullptr;
            m_sheetSurfaces[page] = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(loaded);
        }
        return m_sheetSurfaces[page];
    }

    void TextureManager::sumCompositeColor(int compId, int depth, long long &r, long long &g, long long &b, long long &weight)
    {
        if(depth > 6)
            return;
        const IndexRange range = getCompositeRange(compId);
        for(int p = range.start; p <= range.end; ++p)
        {
            const CompositePart *part = getCompositePart(p);
            if(!part)
                continue;
            if(part->isComp)
            {
                sumCompositeColor(part->child, depth + 1, r, g, b, weight);
                continue;
            }
            const SpriteFrame *frame = getSpriteFrame(part->child);
            if(!frame || frame->width <= 0 || frame->height <= 0)
                continue;
            SDL_Surface *surface = sheetSurface(frame->page);
            if(!surface)
                continue;
            const float scale = getSheetScale(frame->page);
            const int x0 = static_cast<int>(frame->x * scale), y0 = static_cast<int>(frame->y * scale);
            const int x1 = std::min(surface->w, static_cast<int>((frame->x + frame->width) * scale));
            const int y1 = std::min(surface->h, static_cast<int>((frame->y + frame->height) * scale));
            for(int y = y0; y < y1; ++y)
            {
                const uint8_t *row = static_cast<const uint8_t *>(surface->pixels) + y * surface->pitch;
                for(int x = x0; x < x1; ++x)
                {
                    const uint8_t *px = row + x * 4; // RGBA32: bytes are R, G, B, A
                    if(px[3] < 128)
                        continue;
                    r += px[0];
                    g += px[1];
                    b += px[2];
                    ++weight;
                }
            }
        }
    }

    SDL_Color TextureManager::getTileColor(int tilesetId, int tileIdx)
    {
        const int key = tilesetId * 4096 + tileIdx;
        const auto it = m_tileColors.find(key);
        if(it != m_tileColors.end())
            return it->second;

        long long r = 0, g = 0, b = 0, weight = 0;
        sumCompositeColor(getTileCompositeId(tilesetId, tileIdx), 0, r, g, b, weight);
        SDL_Color color {90, 90, 90, 255};
        if(weight > 0)
        {
            color = {static_cast<Uint8>(r / weight), static_cast<Uint8>(g / weight), static_cast<Uint8>(b / weight), 255};
        }
        m_tileColors[key] = color;
        return color;
    }

    int TextureManager::getTileCompositeId(int tilesetId, int tileIdx) const
    {
        if(tilesetId < 0 || tilesetId >= static_cast<int>(m_tilesetsList.size()))
        {
            tilesetId = 0;
        }
        if(tilesetId < static_cast<int>(m_tilesetsList.size()))
        {
            const auto &tlist = m_tilesetsList[tilesetId];
            if(tileIdx >= 0 && tileIdx < static_cast<int>(tlist.size()))
            {
                return tlist[tileIdx];
            }
        }
        return 1; // Fallback to basic tile composite
    }

    int TextureManager::getDecorCompositeId(int tilesetId, int decorType) const
    {
        return getDecorBuildingId(tilesetId, decorType);
    }

    int TextureManager::getDecorBuildingId(int tilesetId, int decorType) const
    {
        if(tilesetId < 0 || tilesetId >= static_cast<int>(m_tilesetsList.size()))
        {
            tilesetId = 0;
        }
        int offset = (tilesetId < static_cast<int>(m_decorOffsets.size())) ? m_decorOffsets[tilesetId] : 245;
        int idx = offset + decorType;
        if(tilesetId < static_cast<int>(m_tilesetsList.size()))
        {
            const auto &tlist = m_tilesetsList[tilesetId];
            if(idx >= 0 && idx < static_cast<int>(tlist.size()))
            {
                return tlist[idx];
            }
        }
        return -1;
    }

    bool TextureManager::getTurretAnchor(int unitType, int hullDir8, float &outX, float &outY) const
    {
        const int seq = getUnitBaseSeq(unitType, static_cast<int>(ActionState::Idle)) + ((hullDir8 % 8 + 8) % 8);
        IndexRange r = getUnitSequenceRange(seq);
        if(r.end < r.start)
            return false;
        const UnitSequenceStep *step = getUnitSequenceStep(r.start);
        if(!step)
            return false;
        IndexRange br = getUnitBodyRange(step->bodyId);
        for(int lay = br.start; lay <= br.end; ++lay)
        {
            const UnitLayer *layer = getUnitLayer(lay);
            // Turret layers are the idle weapon slots (flags 0 and 2), the ones the renderer rotates
            if(layer && (layer->flag == 0 || layer->flag == 2))
            {
                outX = static_cast<float>(step->ox + layer->ox);
                outY = static_cast<float>(step->oy + layer->oy);
                return true;
            }
        }
        return false;
    }

    int TextureManager::getUnitBaseSeq(int unitType, int actionState) const
    {
        if(unitType >= 0 && unitType < static_cast<int>(m_unitArchetypes.size()))
        {
            const auto &row = m_unitArchetypes[unitType];
            if(row.empty())
                return 142;

            // Authentic J2ME f1130b[unitType][state] mapping (f.java:10955, i.java:5734):
            // row[0] = Standing / Idle (and base for Attack)
            // row[1] = Moving / Walking
            // row[2] = Dying (231+ for infantry)
            // row[3] = Siege Mode Transformation (4<->14, 10<->15, 13<->16)
            if(actionState == static_cast<int>(ActionState::Idle) || actionState == static_cast<int>(ActionState::Attack))
            {
                return row[0];
            }
            else if(actionState == static_cast<int>(ActionState::Move))
            {
                return (row.size() > 1) ? row[1] : row[0];
            }
            else if(actionState == static_cast<int>(ActionState::Die))
            {
                return (row.size() > 2) ? row[2] : 231;
            }
            else if(actionState == static_cast<int>(ActionState::Transform))
            {
                return (row.size() >= 4) ? row[3] : row[0];
            }
            return row[0];
        }
        return 142; // Fallback rifleman idle
    }

} // namespace OpenArtOfWar
