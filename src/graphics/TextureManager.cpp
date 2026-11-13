#include "graphics/TextureManager.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "graphics/SpriteRegistry.hpp"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>

namespace OpenArtOfWar
{

TextureManager::TextureManager(SDL_Renderer *renderer)
    : m_renderer(renderer)
{}

TextureManager::~TextureManager()
{
    Cleanup();
}

void TextureManager::Cleanup()
{
    for (auto &[path, tex] : m_textureCache)
    {
        if (tex)
        {
            SDL_DestroyTexture(tex);
        }
    }
    m_textureCache.clear();
    for (auto &[key, tex] : m_recoloredSheets)
    {
        if (tex)
        {
            SDL_DestroyTexture(tex);
        }
    }
    m_recoloredSheets.clear();
    for (SDL_Surface *surface : m_sheetSurfaces)
    {
        if (surface)
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
    m_tilesetSprites.clear();
    m_unitVisuals.clear();
    m_buildingSprites.clear();
    m_imagePaths.clear();
    m_imageCount = 0;
    m_terrainPages.clear();
}

SDL_Texture *TextureManager::LoadTexture(std::string_view filePath)
{
    auto it = m_textureCache.find(filePath);
    if (it != m_textureCache.end())
    {
        return it->second;
    }

    // SDL3 decodes PNG natively; all game art is PNG
    std::vector<std::uint8_t> pngData;
    SDL_Surface *surface = nullptr;
    if (Assets::ReadBytes(filePath, pngData) && !pngData.empty())
    {
        surface = SDL_LoadPNG_IO(SDL_IOFromConstMem(pngData.data(), pngData.size()), true);
    }
    if (!surface)
    {
        std::cerr << "[TextureManager] Failed to load image: " << filePath << " (" << SDL_GetError() << ")"
                  << std::endl;
        return nullptr;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    SDL_DestroySurface(surface);
    if (texture)
    {
        // Pixel art: keep hard pixel edges when the camera zooms (SDL3 defaults to linear filtering)
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    }

    if (!texture)
    {
        std::cerr << "[TextureManager] Failed to create texture: " << filePath << " (" << SDL_GetError() << ")"
                  << std::endl;
        return nullptr;
    }

    m_textureCache[std::string(filePath)] = texture;
    return texture;
}

bool TextureManager::LoadAllAssets()
{
    std::cout << "[TextureManager] Loading game assets from: " << LoaderResources::AssetRoot() << "..." << std::endl;

    LoadSprites();

    // 4. Load UI screens
    for (const auto &[name, path] : LoaderResources::UiTextures())
        m_uiTextures[name] = LoadTexture(path);

    std::cout << "[TextureManager] Asset textures loaded successfully." << std::endl;
    return true;
}

void TextureManager::ReloadSprites()
{
    // Forget every texture and surface made from the sprite images, then read them again
    for (const std::string &path : m_imagePaths)
    {
        const auto it = m_textureCache.find(path);
        if (it != m_textureCache.end())
        {
            if (it->second)
                SDL_DestroyTexture(it->second);
            m_textureCache.erase(it);
        }
    }
    for (auto &[key, tex] : m_recoloredSheets)
        if (tex)
            SDL_DestroyTexture(tex);
    m_recoloredSheets.clear();
    for (SDL_Surface *surface : m_sheetSurfaces)
        if (surface)
            SDL_DestroySurface(surface);
    m_sheetSurfaces.clear();
    m_tileColors.clear();
    OpenArtOfWar::ReloadSprites();
    LoadSprites();
    SDL_ScaleMode terrain = m_terrainScaleMode, sprite = m_spriteScaleMode;
    for (std::size_t i = 0; i < m_spriteSheets.size(); ++i)
        if (m_spriteSheets[i])
            SDL_SetTextureScaleMode(m_spriteSheets[i], m_terrainPages[i] ? terrain : sprite);
}

void TextureManager::LoadSprites()
{
    // 1. The sprites: one image and one meta per object, compiled into the drawing tables
    const SpriteRegistry &reg = Sprites();
    const SpriteTables &t = reg.Tables();
    m_spriteFrames = t.frames;
    m_compositeRanges = t.compositeRanges;
    m_compositeParts = t.compositeParts;
    m_animRanges = t.animRanges;
    m_animFrames = t.animFrames;
    m_unitBodyRanges = t.bodyRanges;
    m_unitLayers = t.bodyLayers;
    m_unitSequenceRanges = t.sequenceRanges;
    m_unitSteps = t.sequenceSteps;
    m_buildings = t.buildings;
    m_buildingParts = t.buildingParts;

    // 2. Their images; the Resistance images of objects that have one follow as pages ImageCount() + i
    m_imageCount = static_cast<int>(t.images.size());
    m_imagePaths.assign(2 * m_imageCount, std::string());
    m_spriteSheets.assign(2 * m_imageCount, nullptr);
    m_terrainPages.assign(2 * m_imageCount, false);
    for (int i = 0; i < m_imageCount; ++i)
    {
        const SpriteImage &img = t.images[i];
        bool hasFrames = false;
        for (const SpriteFrame &f : m_spriteFrames)
        {
            if (f.page == i && f.srcW > 0 && f.srcH > 0)
            {
                hasFrames = true;
                break;
            }
        }
        for (int alt = 0; alt < 2; ++alt)
        {
            const std::string &path = alt ? img.resistancePath : img.path;
            const int page = i + alt * m_imageCount;
            m_terrainPages[page] = img.terrain;
            if (path.empty() || !hasFrames)
                continue;
            m_imagePaths[page] = path;
            m_spriteSheets[page] = LoadTexture(path);
        }
    }
    std::cout << "[TextureManager] " << m_imageCount << " sprite objects, " << m_spriteFrames.size() << " frames, "
              << m_compositeRanges.size() << " composites, " << m_animRanges.size() << " animations, "
              << m_buildings.size() << " buildings." << std::endl;

    // 3. What the game's units, buildings and tilesets are drawn from
    LoadSpriteBindings();
}

namespace
{
// A family of a sprite object; {start, count} or {-1, 0}
std::pair<int, int> FamilyOf(const std::string &object, const std::string &family)
{
    const SpriteFamily *f = Sprites().FindFamily(object, family);
    if (!f || f->count <= 0)
        return {-1, 0};
    return {f->start, f->count};
}

std::string Scalar(const YAML::Node &n, const char *key)
{
    try
    {
        if (n && n.IsMap() && n[key] && n[key].IsScalar())
            return n[key].as<std::string>();
    }
    catch (...)
    {}
    return {};
}
} // namespace

void TextureManager::LoadSpriteBindings()
{
    m_unitVisuals.assign(64, UnitVisual {});
    m_buildingSprites.assign(64, -1);

    // A unit archetype: the families "<set>idle" / "attack" / "move" / "die" / "transform" of its object
    auto bindUnit = [&](int arch,
                        const std::string &object,
                        const std::string &set,
                        const std::string &icon,
                        const std::string &file) {
        if (arch < 0 || arch >= static_cast<int>(m_unitVisuals.size()) || object.empty())
            return;
        if (!Sprites().HasObject(object))
        {
            std::cerr << "[TextureManager] " << file << ": no sprite object '" << object << "'" << std::endl;
            return;
        }
        const std::string pre = set.empty() ? std::string() : set + "_";
        UnitVisual v;
        std::tie(v.idle, v.idleCount) = FamilyOf(object, pre + "idle");
        v.attack = FamilyOf(object, pre + "attack").first;
        std::tie(v.move, v.moveCount) = FamilyOf(object, pre + "move");
        v.die = FamilyOf(object, pre + "die").first;
        v.transform = FamilyOf(object, pre + "transform").first;
        if (v.idle < 0)
            std::cerr << "[TextureManager] " << file << ": sprite '" << object << "' has no family '" << pre << "idle'"
                      << std::endl;
        v.icon = icon.empty() ? m_unitVisuals[arch].icon : SpriteHandle(icon, SpriteKind::Part, file);
        m_unitVisuals[arch] = v;
    };
    // A building type, or a family of them (walls by damage)
    auto bindBuilding = [&](int type, std::string_view ref, std::string_view file) {
        if (type < 0 || ref.empty())
            return;
        const auto dot = ref.find('.');
        const SpriteFamily *fam
            = dot == std::string::npos ? nullptr : Sprites().FindFamily(ref.substr(0, dot), ref.substr(dot + 1));
        if (fam && fam->kind == SpriteKind::Building)
        {
            for (int i = 0; i < fam->count && type + i < static_cast<int>(m_buildingSprites.size()); ++i)
                m_buildingSprites[type + i] = fam->start + i;
            return;
        }
        if (type < static_cast<int>(m_buildingSprites.size()))
            m_buildingSprites[type] = SpriteHandle(ref, SpriteKind::Building, file);
    };

    for (const std::string &path : LoaderResources::CharacterFiles())
    {
        std::string text;
        if (!Assets::ReadText(path, text))
            continue;
        YAML::Node doc;
        try
        {
            doc = YAML::Load(text);
        }
        catch (const std::exception &)
        {
            continue;
        }
        if (!doc.IsMap())
            continue;
        const std::string sprite = Scalar(doc, "sprite");
        if (doc["archetype_id"])
        {
            const int arch = doc["archetype_id"].as<int>(-1);
            const std::string icon = Scalar(doc["production"], "icon_composite");
            bindUnit(arch, sprite, Scalar(doc, "sprite_set"), icon, path);
            // Other forms of the unit (siege mode, the baseline before an upgrade): "<form>: {archetype_id,
            // sprite_set}"
            std::vector<YAML::Node> nodes {doc};
            if (doc["abilities"] && doc["abilities"].IsMap())
                for (const auto &kv : doc["abilities"])
                    nodes.push_back(kv.second);
            for (const YAML::Node &n : nodes)
            {
                if (!n.IsMap())
                    continue;
                if (&n != &nodes.front() && n["archetype_id"] && n["sprite_set"])
                    bindUnit(
                        n["archetype_id"].as<int>(-1),
                        Scalar(n, "sprite").empty() ? sprite : Scalar(n, "sprite"),
                        Scalar(n, "sprite_set"),
                        {},
                        path);
                if (n["baseline_archetype_id"] && n["baseline_sprite_set"])
                    bindUnit(
                        n["baseline_archetype_id"].as<int>(-1), sprite, Scalar(n, "baseline_sprite_set"), {}, path);
            }
        }
        else if (doc["building_index"])
        {
            const int type = doc["building_index"].as<int>(-1);
            bindBuilding(type, sprite, path);
            // Walls: the reinforced walls (research) are the five types after the plain ones
            if (doc["reinforced_sprite"])
                bindBuilding(type + 5, Scalar(doc, "reinforced_sprite"), path);
        }
    }

    // Tilesets: "tileset_<id>"
    m_tilesetSprites.clear();
    for (int id = 0;; ++id)
    {
        const std::string object = "tileset_" + std::to_string(id);
        if (!Sprites().HasObject(object))
            break;
        TilesetSprites ts;
        std::tie(ts.tiles, ts.tileCount) = FamilyOf(object, "tiles");
        std::tie(ts.decor, ts.decorCount) = FamilyOf(object, "decor");
        m_tilesetSprites.push_back(ts);
    }
}

void TextureManager::CollectTerrainPages()
{
    // Terrain images say so in their meta (LoadAllAssets); nothing to work out
    m_terrainPages.resize(m_spriteSheets.size(), false);
}

SDL_ScaleMode TextureManager::SmoothPixelScaleMode()
{
#if SDL_VERSION_ATLEAST(3, 4, 0)
    return SDL_SCALEMODE_PIXELART;
#else
    return SDL_SCALEMODE_LINEAR;
#endif
}

void TextureManager::SetSpriteFiltering(bool linear, bool sharpPixels)
{
    if (m_terrainPages.size() != m_spriteSheets.size())
        CollectTerrainPages();
    const SDL_ScaleMode smooth = SmoothPixelScaleMode();
    m_terrainScaleMode = linear ? smooth : SDL_SCALEMODE_NEAREST;
    // The sharp-pixel shader does its own pixel-art filtering and needs plain linear samples
    m_spriteScaleMode = sharpPixels ? SDL_SCALEMODE_LINEAR : m_terrainScaleMode;
    const SDL_ScaleMode mode = m_terrainScaleMode;
    const SDL_ScaleMode spriteMode = m_spriteScaleMode;
    for (std::size_t i = 0; i < m_spriteSheets.size(); ++i)
    {
        if (m_spriteSheets[i])
            SDL_SetTextureScaleMode(m_spriteSheets[i], m_terrainPages[i] ? mode : spriteMode);
    }
    for (auto &[key, tex] : m_recoloredSheets)
    {
        if (!tex)
            continue;
        const std::size_t page = static_cast<std::size_t>(key >> 32);
        const bool terrain = page < m_terrainPages.size() && m_terrainPages[page];
        SDL_SetTextureScaleMode(tex, terrain ? mode : spriteMode);
    }
    for (SDL_Texture *texture : m_tilesets)
    {
        if (texture)
            SDL_SetTextureScaleMode(texture, mode);
    }
}

namespace
{

float Smoothstep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void RgbToHsv(float r, float g, float b, float &h, float &s, float &v)
{
    const float maxC = std::max({r, g, b});
    const float minC = std::min({r, g, b});
    const float d = maxC - minC;
    v = maxC;
    s = maxC > 1.0e-10f ? d / maxC : 0.0f;
    if (d <= 1.0e-10f)
    {
        h = 0.0f;
        return;
    }
    if (maxC == r)
        h = (g - b) / (6.0f * d) + (g < b ? 1.0f : 0.0f);
    else if (maxC == g)
        h = (b - r) / (6.0f * d) + (1.0f / 3.0f);
    else
        h = (r - g) / (6.0f * d) + (2.0f / 3.0f);
}

void HsvToRgb(float h, float s, float v, float &r, float &g, float &b)
{
    auto fract = [](float x) {
        return x - std::floor(x);
    };
    const float px = std::abs(fract(h + 1.0f) * 6.0f - 3.0f);
    const float py = std::abs(fract(h + 2.0f / 3.0f) * 6.0f - 3.0f);
    const float pz = std::abs(fract(h + 1.0f / 3.0f) * 6.0f - 3.0f);
    r = v * (1.0f + s * (std::clamp(px - 1.0f, 0.0f, 1.0f) - 1.0f));
    g = v * (1.0f + s * (std::clamp(py - 1.0f, 0.0f, 1.0f) - 1.0f));
    b = v * (1.0f + s * (std::clamp(pz - 1.0f, 0.0f, 1.0f) - 1.0f));
}

} // namespace

SDL_Texture *TextureManager::GetRecoloredSpriteSheet(int page, int team, SDL_Color color)
{
    if (team != 1 && team != 2)
        return GetSpriteSheet(page);
    if (page < 0 || page >= static_cast<int>(m_spriteSheets.size()))
        return nullptr;

    const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(page)) << 32)
                              | (static_cast<std::uint64_t>(static_cast<std::uint8_t>(team)) << 24)
                              | (static_cast<std::uint64_t>(color.r) << 16) | (static_cast<std::uint64_t>(color.g) << 8)
                              | static_cast<std::uint64_t>(color.b);
    auto it = m_recoloredSheets.find(key);
    if (it != m_recoloredSheets.end())
        return it->second;

    SDL_Surface *base = SheetSurface(page);
    if (!base)
        return GetSpriteSheet(page);
    SDL_Surface *copy = SDL_DuplicateSurface(base);
    if (!copy)
        return GetSpriteSheet(page);

    float teamH = 0.0f, teamS = 0.0f, teamV = 0.0f;
    RgbToHsv(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, teamH, teamS, teamV);
    const float gain = team == 2 ? 1.7f : 1.0f;
    const float satFactor = gain * (0.6f + 0.5f * teamS);

    for (int y = 0; y < copy->h; ++y)
    {
        std::uint8_t *row = static_cast<std::uint8_t *>(copy->pixels) + y * copy->pitch;
        for (int x = 0; x < copy->w; ++x)
        {
            std::uint8_t *px = row + x * 4; // RGBA32
            if (px[3] == 0)
                continue;
            const float cr = px[0] / 255.0f;
            const float cg = px[1] / 255.0f;
            const float cb = px[2] / 255.0f;
            float h = 0.0f, s = 0.0f, v = 0.0f;
            RgbToHsv(cr, cg, cb, h, s, v);
            const float deg = h * 360.0f;
            float w = 0.0f;
            if (team == 1)
            {
                w = Smoothstep(160.0f, 185.0f, deg) * (1.0f - Smoothstep(235.0f, 255.0f, deg))
                    * Smoothstep(0.18f, 0.40f, s);
            }
            else
            {
                const float hueDist = deg < 180.0f ? deg : 360.0f - deg;
                const float redness = 1.0f - Smoothstep(22.0f, 36.0f, hueDist);
                w = redness * Smoothstep(0.10f, 0.24f, s) * Smoothstep(0.005f, 0.02f, v);
            }
            if (w <= 0.001f)
                continue;
            float rr = 0.0f, rg = 0.0f, rb = 0.0f;
            HsvToRgb(teamH, std::min(1.0f, s * satFactor), v, rr, rg, rb);
            const float outR = cr + (rr - cr) * w;
            const float outG = cg + (rg - cg) * w;
            const float outB = cb + (rb - cb) * w;
            px[0] = static_cast<std::uint8_t>(std::clamp(outR * 255.0f + 0.5f, 0.0f, 255.0f));
            px[1] = static_cast<std::uint8_t>(std::clamp(outG * 255.0f + 0.5f, 0.0f, 255.0f));
            px[2] = static_cast<std::uint8_t>(std::clamp(outB * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(m_renderer, copy);
    SDL_DestroySurface(copy);
    if (tex)
    {
        if (m_terrainPages.size() != m_spriteSheets.size())
            CollectTerrainPages();
        const bool terrain = page < static_cast<int>(m_terrainPages.size()) && m_terrainPages[page];
        SDL_SetTextureScaleMode(tex, terrain ? m_terrainScaleMode : m_spriteScaleMode);
    }
    m_recoloredSheets[key] = tex;
    return tex ? tex : GetSpriteSheet(page);
}

SDL_Texture *TextureManager::GetSpriteSheet(int page) const
{
    if (page >= 0 && page < static_cast<int>(m_spriteSheets.size()))
    {
        return m_spriteSheets[page];
    }
    return nullptr;
}

SDL_Texture *TextureManager::GetTileset(int index) const
{
    if (index >= 0 && index < static_cast<int>(m_tilesets.size()))
    {
        return m_tilesets[index];
    }
    return nullptr;
}

SDL_Texture *TextureManager::GetUiTexture(std::string_view name) const
{
    auto it = m_uiTextures.find(name);
    if (it != m_uiTextures.end())
    {
        return it->second;
    }
    return nullptr;
}

const SpriteFrame *TextureManager::GetSpriteFrame(int spriteId) const
{
    if (spriteId >= 0 && spriteId < static_cast<int>(m_spriteFrames.size()))
    {
        return &m_spriteFrames[spriteId];
    }
    return nullptr;
}

IndexRange TextureManager::GetCompositeRange(int compId) const
{
    if (compId >= 0 && compId < static_cast<int>(m_compositeRanges.size()))
    {
        return m_compositeRanges[compId];
    }
    return {0, -1};
}

const CompositePart *TextureManager::GetCompositePart(int index) const
{
    if (index >= 0 && index < static_cast<int>(m_compositeParts.size()))
    {
        return &m_compositeParts[index];
    }
    return nullptr;
}

IndexRange TextureManager::GetAnimRange(int animId) const
{
    if (animId >= 0 && animId < static_cast<int>(m_animRanges.size()))
    {
        return m_animRanges[animId];
    }
    return {0, -1};
}

const AnimFrame *TextureManager::GetAnimFrame(int index) const
{
    if (index >= 0 && index < static_cast<int>(m_animFrames.size()))
    {
        return &m_animFrames[index];
    }
    return nullptr;
}

IndexRange TextureManager::GetUnitBodyRange(int bodyId) const
{
    if (bodyId >= 0 && bodyId < static_cast<int>(m_unitBodyRanges.size()))
    {
        return m_unitBodyRanges[bodyId];
    }
    return {0, -1};
}

const UnitLayer *TextureManager::GetUnitLayer(int index) const
{
    if (index >= 0 && index < static_cast<int>(m_unitLayers.size()))
    {
        return &m_unitLayers[index];
    }
    return nullptr;
}

IndexRange TextureManager::GetUnitSequenceRange(int seqId) const
{
    if (seqId >= 0 && seqId < static_cast<int>(m_unitSequenceRanges.size()))
    {
        return m_unitSequenceRanges[seqId];
    }
    return {0, -1};
}

const UnitSequenceStep *TextureManager::GetUnitSequenceStep(int index) const
{
    if (index >= 0 && index < static_cast<int>(m_unitSteps.size()))
    {
        return &m_unitSteps[index];
    }
    return nullptr;
}

const BuildingDef *TextureManager::GetBuildingDef(int buildingType) const
{
    if (buildingType >= 0 && buildingType < static_cast<int>(m_buildings.size()))
    {
        return &m_buildings[buildingType];
    }
    return nullptr;
}

const BuildingPart *TextureManager::GetBuildingPart(int index) const
{
    if (index >= 0 && index < static_cast<int>(m_buildingParts.size()))
    {
        return &m_buildingParts[index];
    }
    return nullptr;
}

SDL_Surface *TextureManager::SheetSurface(int page)
{
    if (page < 0 || page >= static_cast<int>(m_imagePaths.size()) || m_imagePaths[page].empty())
        return nullptr;
    if (m_sheetSurfaces.size() < m_imagePaths.size())
        m_sheetSurfaces.resize(m_imagePaths.size(), nullptr);
    if (!m_sheetSurfaces[page])
    {
        std::vector<std::uint8_t> png;
        if (!Assets::ReadBytes(m_imagePaths[page], png) || png.empty())
            return nullptr;
        SDL_Surface *loaded = SDL_LoadPNG_IO(SDL_IOFromConstMem(png.data(), png.size()), true);
        if (!loaded)
            return nullptr;
        m_sheetSurfaces[page] = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(loaded);
    }
    return m_sheetSurfaces[page];
}

int TextureManager::ResistancePage(int page) const
{
    if (page >= 0 && page < m_imageCount && m_spriteSheets[page + m_imageCount])
        return page + m_imageCount;
    return page;
}

void TextureManager::SumCompositeColor(
    int compId, int depth, long long &r, long long &g, long long &b, long long &weight)
{
    if (depth > 6)
        return;
    const IndexRange range = GetCompositeRange(compId);
    for (int p = range.start; p <= range.end; ++p)
    {
        const CompositePart *part = GetCompositePart(p);
        if (!part)
            continue;
        if (part->isComp)
        {
            SumCompositeColor(part->child, depth + 1, r, g, b, weight);
            continue;
        }
        const SpriteFrame *frame = GetSpriteFrame(part->child);
        if (!frame || frame->width <= 0 || frame->height <= 0)
            continue;
        SDL_Surface *surface = SheetSurface(frame->page);
        if (!surface)
            continue;
        const int x0 = static_cast<int>(frame->srcX), y0 = static_cast<int>(frame->srcY);
        const int x1 = std::min(surface->w, static_cast<int>(frame->srcX + frame->srcW));
        const int y1 = std::min(surface->h, static_cast<int>(frame->srcY + frame->srcH));
        for (int y = y0; y < y1; ++y)
        {
            const std::uint8_t *row = static_cast<const std::uint8_t *>(surface->pixels) + y * surface->pitch;
            for (int x = x0; x < x1; ++x)
            {
                const std::uint8_t *px = row + x * 4; // RGBA32: bytes are R, G, B, A
                if (px[3] < 128)
                    continue;
                r += px[0];
                g += px[1];
                b += px[2];
                ++weight;
            }
        }
    }
}

SDL_Color TextureManager::GetTileColor(int tilesetId, int tileIdx)
{
    const int key = tilesetId * 4096 + tileIdx;
    const auto it = m_tileColors.find(key);
    if (it != m_tileColors.end())
        return it->second;

    long long r = 0, g = 0, b = 0, weight = 0;
    SumCompositeColor(GetTileCompositeId(tilesetId, tileIdx), 0, r, g, b, weight);
    SDL_Color color {90, 90, 90, 255};
    if (weight > 0)
    {
        color = {static_cast<Uint8>(r / weight), static_cast<Uint8>(g / weight), static_cast<Uint8>(b / weight), 255};
    }
    m_tileColors[key] = color;
    return color;
}

int TextureManager::GetTileCompositeId(int tilesetId, int tileIdx) const
{
    if (m_tilesetSprites.empty())
        return -1;
    if (tilesetId < 0 || tilesetId >= static_cast<int>(m_tilesetSprites.size()))
        tilesetId = 0;
    const TilesetSprites &ts = m_tilesetSprites[tilesetId];
    return tileIdx >= 0 && tileIdx < ts.tileCount ? ts.tiles + tileIdx : -1;
}

int TextureManager::GetTileCount(int tilesetId) const
{
    return tilesetId >= 0 && tilesetId < static_cast<int>(m_tilesetSprites.size())
               ? m_tilesetSprites[tilesetId].tileCount
               : 0;
}

int TextureManager::GetDecorCount(int tilesetId) const
{
    return tilesetId >= 0 && tilesetId < static_cast<int>(m_tilesetSprites.size())
               ? m_tilesetSprites[tilesetId].decorCount
               : 0;
}

int TextureManager::GetDecorCompositeId(int tilesetId, int decorType) const
{
    return GetDecorBuildingId(tilesetId, decorType);
}

int TextureManager::GetDecorBuildingId(int tilesetId, int decorType) const
{
    if (m_tilesetSprites.empty())
        return -1;
    if (tilesetId < 0 || tilesetId >= static_cast<int>(m_tilesetSprites.size()))
        tilesetId = 0;
    const TilesetSprites &ts = m_tilesetSprites[tilesetId];
    return decorType >= 0 && decorType < ts.decorCount ? ts.decor + decorType : -1;
}

bool TextureManager::GetTurretAnchor(int unitType, int hullDir8, float &outX, float &outY) const
{
    const UnitVisual *v = GetUnitVisual(unitType);
    if (!v || v->idle < 0)
        return false;
    const int seq = v->idle + ((hullDir8 % 8 + 8) % 8);
    IndexRange r = GetUnitSequenceRange(seq);
    if (r.end < r.start)
        return false;
    const UnitSequenceStep *step = GetUnitSequenceStep(r.start);
    if (!step)
        return false;
    IndexRange br = GetUnitBodyRange(step->bodyId);
    for (int lay = br.start; lay <= br.end; ++lay)
    {
        const UnitLayer *layer = GetUnitLayer(lay);
        // Turret layers are the idle weapon slots (flags 0 and 2), the ones the renderer rotates
        if (layer && (layer->flag == 0 || layer->flag == 2))
        {
            outX = static_cast<float>(step->ox + layer->ox);
            outY = static_cast<float>(step->oy + layer->oy);
            return true;
        }
    }
    return false;
}

const UnitVisual *TextureManager::GetUnitVisual(int archetype) const
{
    if (archetype < 0 || archetype >= static_cast<int>(m_unitVisuals.size()) || m_unitVisuals[archetype].idle < 0)
        return nullptr;
    return &m_unitVisuals[archetype];
}

int TextureManager::GetUnitBaseSeq(int unitType, int actionState) const
{
    const UnitVisual *v = GetUnitVisual(unitType);
    if (!v)
        return -1;
    if (actionState == static_cast<int>(ActionState::Move))
        return v->move >= 0 ? v->move : v->idle;
    if (actionState == static_cast<int>(ActionState::Die))
        return v->die >= 0 ? v->die : v->idle;
    if (actionState == static_cast<int>(ActionState::Transform))
        return v->transform >= 0 ? v->transform : v->idle;
    return v->idle;
}

int TextureManager::GetBuildingSprite(int buildingType) const
{
    if (buildingType >= 0 && buildingType < static_cast<int>(m_buildingSprites.size()))
        return m_buildingSprites[buildingType];
    return -1;
}

} // namespace OpenArtOfWar
