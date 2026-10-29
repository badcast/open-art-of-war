#pragma once

#include <SDL3/SDL.h>
#include <map>
#include <string>
#include <vector>

namespace OpenArtOfWar
{

    class TextureManager;

    // A building or base shown on the preview
    struct PreviewMarker
    {
        int x {0};
        int y {0};
        SDL_Color color {255, 255, 255, 255};
    };

    // Small picture of a map for the selection dialogs: one pixel per tile, colored like the tile's artwork,
    // with decorations (trees, cliffs) darkened and optional markers. Built from the map JSON on first use.
    class MapPreview
    {
    public:
        MapPreview() = default;
        ~MapPreview();
        MapPreview(const MapPreview &) = delete;
        MapPreview &operator=(const MapPreview &) = delete;

        // The preview of `mapPath` ("assets/maps/m37.json"); `key` tells the cached pictures apart (the same map
        // with other markers needs another key). Returns nullptr when the map cannot be read.
        SDL_Texture *get(SDL_Renderer *renderer, TextureManager &textures, const std::string &key, const std::string &mapPath, const std::vector<PreviewMarker> &markers, int &width, int &height);
        void clear();

    private:
        struct Entry
        {
            SDL_Texture *texture {nullptr};
            int width {0};
            int height {0};
        };
        std::map<std::string, Entry> m_cache;
    };

} // namespace OpenArtOfWar
