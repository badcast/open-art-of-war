#include "ui/MapPreview.hpp"
#include "core/AssetStore.hpp"
#include "core/ResourceManager.hpp"
#include "game/TileCollision.hpp"
#include "graphics/TextureManager.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar
{

MapPreview::~MapPreview()
{
    BindResourceManager(nullptr);
    Clear();
}

void MapPreview::BindResourceManager(ResourceManager *manager, std::string cacheName)
{
    if (m_resourceManager)
        m_resourceManager->UnregisterCache(m_cacheName);
    m_resourceManager = manager;
    m_cacheName = std::move(cacheName);
    if (!m_resourceManager)
        return;

    m_resourceManager->RegisterCache(
        m_cacheName,
        [this](std::uint64_t nowMs, bool forceAllUnpinned) {
            return m_cache.EvictUnused(nowMs, forceAllUnpinned ? 0 : 30000, 32 * 1024 * 1024);
        },
        [this]() {
            return m_cache.Stats();
        });
}

void MapPreview::Clear()
{
    m_cache.Clear();
}

SDL_Texture *MapPreview::Get(
    SDL_Renderer *renderer,
    TextureManager &textures,
    std::string_view key,
    std::string_view mapPath,
    const std::vector<PreviewMarker> &markers,
    int &width,
    int &height)
{
    const std::uint64_t nowMs = SDL_GetTicks();
    if (Entry *cached = m_cache.Find(key, nowMs))
    {
        width = cached->width;
        height = cached->height;
        return cached->texture;
    }

    auto entry = std::make_unique<Entry>();
    std::size_t bytes = 0;
    std::string text;
    nlohmann::json doc;
    if (Assets::ReadText(mapPath, text))
        doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_object() && doc.contains("width") && doc.contains("height") && doc.contains("terrainRle"))
    {
        const int w = std::clamp(doc["width"].get<int>(), 1, 256);
        const int h = std::clamp(doc["height"].get<int>(), 1, 256);
        const int tileset = doc.value("tilesetId", 0);
        SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
        if (surface)
        {
            auto put = [&](int x, int y, SDL_Color c) {
                if (x < 0 || y < 0 || x >= w || y >= h)
                    return;
                std::uint8_t *px = static_cast<std::uint8_t *>(surface->pixels) + y * surface->pitch + x * 4;
                px[0] = c.r;
                px[1] = c.g;
                px[2] = c.b;
                px[3] = 255;
            };
            SDL_ClearSurface(surface, 0.0f, 0.0f, 0.0f, 1.0f);

            int cx = 0, cy = 0;
            for (const auto &span : doc["terrainRle"])
            {
                const int run = span.value("runLength", 0);
                const int tile = span.value("tileType", 0);
                const SDL_Color color = textures.GetTileColor(tileset, tile);
                for (int i = 0; i < run && cy < h; ++i)
                {
                    put(cx, cy, color);
                    if (++cx >= w)
                    {
                        cx = 0;
                        ++cy;
                    }
                }
            }
            // Trees, cliffs and landmarks as dark spots
            if (doc.contains("decorations"))
            {
                for (const auto &d : doc["decorations"])
                {
                    put(d.value("x", -1), d.value("y", -1), {28, 52, 30, 255});
                }
            }
            for (const PreviewMarker &m : markers)
            {
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                        put(m.x + dx + 1, m.y + dy + 1, m.color);
                }
            }

            entry->texture = SDL_CreateTextureFromSurface(renderer, surface);
            if (entry->texture)
            {
                SDL_SetTextureScaleMode(entry->texture, SDL_SCALEMODE_NEAREST);
                bytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
            }
            entry->width = w;
            entry->height = h;
            SDL_DestroySurface(surface);
        }
    }
    width = entry->width;
    height = entry->height;
    SDL_Texture *tex = entry->texture;
    m_cache.Insert(key, std::move(entry), bytes, nowMs);
    return tex;
}

} // namespace OpenArtOfWar
