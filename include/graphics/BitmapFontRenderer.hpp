#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "core/Config.hpp"

namespace OpenArtOfWar
{

    struct TextCacheKey
    {
        std::string text;
        int scale {2};
        uint32_t colorRgba {0xFFFFFFFF};

        bool operator==(const TextCacheKey &o) const
        {
            return scale == o.scale && colorRgba == o.colorRgba && text == o.text;
        }
    };

    struct TextCacheKeyHash
    {
        std::size_t operator()(const TextCacheKey &k) const
        {
            std::size_t h1 = std::hash<std::string> {}(k.text);
            std::size_t h2 = std::hash<int> {}(k.scale);
            std::size_t h3 = std::hash<uint32_t> {}(k.colorRgba);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    struct CachedText
    {
        SDL_Texture *texture {nullptr};
        int width {0};
        int height {0};
        uint32_t lastUsedFrame {0};
    };

    // Pixel sizes of the font scales (section "font" of game_environment.json)
    struct FontSizes
    {
        float small {13.0f};  // scale 1 and 5
        float medium {16.0f}; // scale 2 and 6
        float large {22.0f};  // scale 3
        float title {30.0f};  // scale 4
        float hero {48.0f};   // scale 7
    };

    class BitmapFontRenderer
    {
    public:
        BitmapFontRenderer(SDL_Renderer *renderer, const std::string &regularFontPath = "assets/fonts/Roboto-Regular.ttf", const std::string &boldFontPath = "assets/fonts/Roboto-Bold.ttf", const FontSizes &sizes = {});
        ~BitmapFontRenderer();

        void drawString(const std::string &text, int x, int y, int scale = 2, SDL_Color color = {255, 255, 255, 255}, bool dropShadow = true, int anchor = Anchor::TOP | Anchor::LEFT);

        // Returns the height of the drawn block in pixels
        int drawTextWrapped(const std::string &text, int x, int y, int maxLineWidth, int scale = 2, SDL_Color color = {255, 255, 255, 255}, bool dropShadow = true);

        int getTextWidth(const std::string &text, int scale = 2) const;
        int getTextHeight(int scale = 2) const;

        void clearCache();

    private:
        TTF_Font *getFontForScale(int scale) const;
        CachedText getOrCreateTexture(const std::string &text, int scale, SDL_Color color);

        SDL_Renderer *m_renderer {nullptr};
        TTF_Font *m_fontSmall {nullptr};        // scale 1 (~13px)
        TTF_Font *m_fontMedium {nullptr};       // scale 2 (~16px)
        TTF_Font *m_fontLarge {nullptr};        // scale 3 (~22px bold)
        TTF_Font *m_fontTitle {nullptr};        // scale 4 (~30px bold)
        TTF_Font *m_fontSmallBold {nullptr};    // scale 5 (13px bold)
        TTF_Font *m_fontMediumBold {nullptr};   // scale 6 (16px bold)
        std::vector<uint8_t> m_fontDataRegular; // font files from data.blob, read by m_font* via SDL IO
        std::vector<uint8_t> m_fontDataBold;
        TTF_Font *m_fontHero {nullptr}; // scale 7 (48px bold)

        std::unordered_map<TextCacheKey, CachedText, TextCacheKeyHash> m_cache;
        uint32_t m_currentFrame {0};
    };

    // Modern alias
    using FontRenderer = BitmapFontRenderer;

} // namespace OpenArtOfWar
