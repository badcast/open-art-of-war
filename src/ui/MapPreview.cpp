#include "ui/MapPreview.hpp"
#include "core/AssetStore.hpp"
#include "game/TileCollision.hpp"
#include "graphics/TextureManager.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar
{

    MapPreview::~MapPreview()
    {
        clear();
    }

    void MapPreview::clear()
    {
        for(auto &[key, entry] : m_cache)
        {
            if(entry.texture)
                SDL_DestroyTexture(entry.texture);
        }
        m_cache.clear();
    }

    SDL_Texture *MapPreview::get(SDL_Renderer *renderer, TextureManager &textures, const std::string &key, const std::string &mapPath, const std::vector<PreviewMarker> &markers, int &width, int &height)
    {
        const auto cached = m_cache.find(key);
        if(cached != m_cache.end())
        {
            width = cached->second.width;
            height = cached->second.height;
            return cached->second.texture;
        }

        Entry entry;
        std::string text;
        nlohmann::json doc;
        if(Assets::readText(mapPath, text))
            doc = nlohmann::json::parse(text, nullptr, false);
        if(doc.is_object() && doc.contains("width") && doc.contains("height") && doc.contains("terrainRle"))
        {
            const int w = std::clamp(doc["width"].get<int>(), 1, 256);
            const int h = std::clamp(doc["height"].get<int>(), 1, 256);
            const int tileset = doc.value("tilesetId", 0);
            SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
            if(surface)
            {
                auto put = [&](int x, int y, SDL_Color c)
                {
                    if(x < 0 || y < 0 || x >= w || y >= h)
                        return;
                    uint8_t *px = static_cast<uint8_t *>(surface->pixels) + y * surface->pitch + x * 4;
                    px[0] = c.r;
                    px[1] = c.g;
                    px[2] = c.b;
                    px[3] = 255;
                };
                SDL_ClearSurface(surface, 0.0f, 0.0f, 0.0f, 1.0f);

                int cx = 0, cy = 0;
                for(const auto &span : doc["terrainRle"])
                {
                    const int run = span.value("runLength", 0);
                    const int tile = span.value("tileType", 0);
                    const SDL_Color color = textures.getTileColor(tileset, tile);
                    for(int i = 0; i < run && cy < h; ++i)
                    {
                        put(cx, cy, color);
                        if(++cx >= w)
                        {
                            cx = 0;
                            ++cy;
                        }
                    }
                }
                // Trees, cliffs and landmarks as dark spots
                if(doc.contains("decorations"))
                {
                    for(const auto &d : doc["decorations"])
                    {
                        put(d.value("x", -1), d.value("y", -1), {28, 52, 30, 255});
                    }
                }
                for(const PreviewMarker &m : markers)
                {
                    for(int dy = -1; dy <= 1; ++dy)
                    {
                        for(int dx = -1; dx <= 1; ++dx)
                            put(m.x + dx + 1, m.y + dy + 1, m.color);
                    }
                }

                entry.texture = SDL_CreateTextureFromSurface(renderer, surface);
                if(entry.texture)
                    SDL_SetTextureScaleMode(entry.texture, SDL_SCALEMODE_NEAREST);
                entry.width = w;
                entry.height = h;
                SDL_DestroySurface(surface);
            }
        }
        m_cache[key] = entry;
        width = entry.width;
        height = entry.height;
        return entry.texture;
    }

} // namespace OpenArtOfWar
