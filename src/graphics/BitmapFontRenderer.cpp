#include "graphics/BitmapFontRenderer.hpp"
#include "core/AssetStore.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace OpenArtOfWar
{

    namespace
    {

        namespace fs = std::filesystem;

        // Finds a font file: first the bundled one (relative to the working directory and to the
        // executable, so the game starts from any directory), then platform system fonts.
        std::string resolveFontPath(const std::string &bundled, bool bold)
        {
            std::vector<fs::path> candidates;
            const fs::path rel(bundled);
            candidates.push_back(rel);
            candidates.push_back(fs::path("..") / rel);
            if(const char *base = SDL_GetBasePath())
            {
                candidates.push_back(fs::path(base) / rel);
                candidates.push_back(fs::path(base) / ".." / rel);
                candidates.push_back(fs::path(base) / ".." / "share" / "openaow" / rel);
            }

#if defined(_WIN32)
            const char *winDir = std::getenv("WINDIR");
            const fs::path fonts = fs::path(winDir ? winDir : "C:\\Windows") / "Fonts";
            candidates.push_back(fonts / (bold ? "segoeuib.ttf" : "segoeui.ttf"));
            candidates.push_back(fonts / (bold ? "arialbd.ttf" : "arial.ttf"));
#elif defined(__APPLE__)
            candidates.push_back(bold ? "/System/Library/Fonts/Supplemental/Arial Bold.ttf" : "/System/Library/Fonts/Supplemental/Arial.ttf");
            candidates.push_back(bold ? "/Library/Fonts/Arial Bold.ttf" : "/Library/Fonts/Arial.ttf");
#else
            const char *robotoFile = bold ? "Roboto-Bold.ttf" : "Roboto-Regular.ttf";
            for(const char *dir : {"/usr/share/fonts/TTF", "/usr/share/fonts/truetype/roboto/unhinted/RobotoTTF", "/usr/share/fonts/truetype/roboto", "/usr/share/fonts/google-roboto", "/usr/local/share/fonts"})
            {
                candidates.push_back(fs::path(dir) / robotoFile);
            }
            const char *dejavuFile = bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf";
            for(const char *dir : {"/usr/share/fonts/TTF", "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/dejavu", "/usr/share/fonts/dejavu-sans-fonts"})
            {
                candidates.push_back(fs::path(dir) / dejavuFile);
            }
            candidates.push_back(bold ? "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf" : "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
#endif

            std::error_code ec;
            for(const auto &c : candidates)
            {
                if(fs::is_regular_file(c, ec))
                    return c.string();
            }
            return bundled;
        }

    } // namespace

    BitmapFontRenderer::BitmapFontRenderer(SDL_Renderer *renderer, const std::string &regularFontPath, const std::string &boldFontPath, const FontSizes &sizes) : m_renderer(renderer)
    {
        if(!TTF_WasInit())
        {
            if(!TTF_Init())
            {
                std::cerr << "[BitmapFontRenderer] Failed to initialize SDL_ttf: " << SDL_GetError() << std::endl;
                return;
            }
        }

        std::string regPath = resolveFontPath(regularFontPath, false);
        std::string bldPath = resolveFontPath(boldFontPath, true);

        // Fonts packed in data.blob are opened from memory (the buffers stay alive with the fonts)
        const bool packedRegular = Assets::isBlob() && Assets::readBytes(regularFontPath, m_fontDataRegular);
        const bool packedBold = Assets::isBlob() && Assets::readBytes(boldFontPath, m_fontDataBold);
        auto openFont = [&](bool packed, bool bold, const std::string &path, float size)
        {
            if(!packed)
                return TTF_OpenFont(path.c_str(), size);
            const std::vector<uint8_t> &data = bold ? m_fontDataBold : m_fontDataRegular;
            return TTF_OpenFontIO(SDL_IOFromConstMem(data.data(), data.size()), true, size);
        };

        // Load Roboto fonts for scales 1..4, plus bold UI label sizes (5, 6) and the hero title (7)
        m_fontSmall = openFont(packedRegular, false, regPath, sizes.small);
        m_fontMedium = openFont(packedRegular, false, regPath, sizes.medium);
        m_fontLarge = openFont(packedBold, true, bldPath, sizes.large);
        m_fontTitle = openFont(packedBold, true, bldPath, sizes.title);
        m_fontSmallBold = openFont(packedBold, true, bldPath, sizes.small);
        m_fontMediumBold = openFont(packedBold, true, bldPath, sizes.medium);
        m_fontHero = openFont(packedBold, true, bldPath, sizes.hero);

        if(!m_fontSmall || !m_fontMedium || !m_fontLarge || !m_fontTitle)
        {
            std::cerr << "[BitmapFontRenderer] Warning: Could not open font " << regPath << ": " << SDL_GetError() << std::endl;
        }
        else
        {
            std::cout << "[BitmapFontRenderer] Loaded fonts: " << regPath << " / " << bldPath << std::endl;
        }
    }

    BitmapFontRenderer::~BitmapFontRenderer()
    {
        clearCache();

        if(m_fontSmall)
        {
            TTF_CloseFont(m_fontSmall);
            m_fontSmall = nullptr;
        }
        if(m_fontMedium)
        {
            TTF_CloseFont(m_fontMedium);
            m_fontMedium = nullptr;
        }
        if(m_fontLarge)
        {
            TTF_CloseFont(m_fontLarge);
            m_fontLarge = nullptr;
        }
        if(m_fontTitle)
        {
            TTF_CloseFont(m_fontTitle);
            m_fontTitle = nullptr;
        }
        if(m_fontSmallBold)
        {
            TTF_CloseFont(m_fontSmallBold);
            m_fontSmallBold = nullptr;
        }
        if(m_fontMediumBold)
        {
            TTF_CloseFont(m_fontMediumBold);
            m_fontMediumBold = nullptr;
        }
        if(m_fontHero)
        {
            TTF_CloseFont(m_fontHero);
            m_fontHero = nullptr;
        }
    }

    void BitmapFontRenderer::clearCache()
    {
        for(auto &pair : m_cache)
        {
            if(pair.second.texture)
            {
                SDL_DestroyTexture(pair.second.texture);
            }
        }
        m_cache.clear();
    }

    TTF_Font *BitmapFontRenderer::getFontForScale(int scale) const
    {
        if(scale == 5)
            return m_fontSmallBold ? m_fontSmallBold : m_fontSmall;
        if(scale == 6)
            return m_fontMediumBold ? m_fontMediumBold : m_fontMedium;
        if(scale >= 7)
            return m_fontHero ? m_fontHero : m_fontTitle;
        if(scale <= 1)
            return m_fontSmall ? m_fontSmall : m_fontMedium;
        if(scale == 2)
            return m_fontMedium ? m_fontMedium : m_fontSmall;
        if(scale == 3)
            return m_fontLarge ? m_fontLarge : m_fontMedium;
        return m_fontTitle ? m_fontTitle : (m_fontLarge ? m_fontLarge : m_fontMedium);
    }

    CachedText BitmapFontRenderer::getOrCreateTexture(const std::string &text, int scale, SDL_Color color)
    {
        if(text.empty() || !m_renderer)
        {
            return {nullptr, 0, 0, 0};
        }

        uint32_t colorKey = (static_cast<uint32_t>(color.r) << 24) | (static_cast<uint32_t>(color.g) << 16) | (static_cast<uint32_t>(color.b) << 8) | (static_cast<uint32_t>(color.a));
        TextCacheKey key {text, scale, colorKey};

        auto it = m_cache.find(key);
        if(it != m_cache.end())
        {
            it->second.lastUsedFrame = m_currentFrame;
            return it->second;
        }

        TTF_Font *font = getFontForScale(scale);
        if(!font)
        {
            return {nullptr, 0, 0, 0};
        }

        // Render text to surface with anti-aliasing
        SDL_Surface *surface = TTF_RenderText_Blended(font, text.c_str(), text.size(), color);
        if(!surface)
        {
            return {nullptr, 0, 0, 0};
        }

        SDL_Texture *texture = SDL_CreateTextureFromSurface(m_renderer, surface);
        int w = surface->w;
        int h = surface->h;
        SDL_DestroySurface(surface);

        if(!texture)
        {
            return {nullptr, 0, 0, 0};
        }

        // Prune cache if too large (keep under 512 entries)
        if(m_cache.size() >= 512)
        {
            auto oldest = m_cache.begin();
            for(auto iter = m_cache.begin(); iter != m_cache.end(); ++iter)
            {
                if(iter->second.lastUsedFrame < oldest->second.lastUsedFrame)
                {
                    oldest = iter;
                }
            }
            if(oldest != m_cache.end())
            {
                if(oldest->second.texture)
                {
                    SDL_DestroyTexture(oldest->second.texture);
                }
                m_cache.erase(oldest);
            }
        }

        CachedText cached {texture, w, h, m_currentFrame};
        m_cache[key] = cached;
        return cached;
    }

    int BitmapFontRenderer::getTextWidth(const std::string &text, int scale) const
    {
        if(text.empty())
            return 0;
        TTF_Font *font = getFontForScale(scale);
        if(!font)
            return static_cast<int>(text.size()) * (scale <= 1 ? 8 : (scale * 8));

        int w = 0, h = 0;
        TTF_GetStringSize(font, text.c_str(), text.size(), &w, &h);
        return w;
    }

    int BitmapFontRenderer::getTextHeight(int scale) const
    {
        TTF_Font *font = getFontForScale(scale);
        if(!font)
            return (scale <= 1 ? 12 : (scale * 8));
        return TTF_GetFontHeight(font);
    }

    void BitmapFontRenderer::drawString(const std::string &text, int x, int y, int scale, SDL_Color color, bool dropShadow, int anchor)
    {
        if(text.empty() || !m_renderer)
            return;

        m_currentFrame++;

        int totalW = getTextWidth(text, scale);
        int totalH = getTextHeight(scale);

        if(anchor & Anchor::HCENTER)
        {
            x -= totalW / 2;
        }
        else if(anchor & Anchor::RIGHT)
        {
            x -= totalW;
        }

        if(anchor & Anchor::VCENTER)
        {
            y -= totalH / 2;
        }
        else if(anchor & Anchor::BOTTOM)
        {
            y -= totalH;
        }

        // Shadow pass
        if(dropShadow)
        {
            SDL_Color shadowCol = {0, 0, 0, 210};
            CachedText shadow = getOrCreateTexture(text, scale, shadowCol);
            if(shadow.texture)
            {
                int shadowOffset = (scale == 3 || scale == 4 || scale >= 7) ? 2 : 1;
                SDL_FRect dstRect = {static_cast<float>(x + shadowOffset), static_cast<float>(y + shadowOffset), static_cast<float>(shadow.width), static_cast<float>(shadow.height)};
                SDL_RenderTexture(m_renderer, shadow.texture, nullptr, &dstRect);
            }
        }

        // Main text pass
        CachedText mainText = getOrCreateTexture(text, scale, color);
        if(mainText.texture)
        {
            SDL_FRect dstRect = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(mainText.width), static_cast<float>(mainText.height)};
            SDL_RenderTexture(m_renderer, mainText.texture, nullptr, &dstRect);
        }
    }

    int BitmapFontRenderer::drawTextWrapped(const std::string &text, int x, int y, int maxLineWidth, int scale, SDL_Color color, bool dropShadow)
    {
        if(text.empty() || !m_renderer)
            return 0;

        int lineHeight = getTextHeight(scale) + 3;
        int curY = y;

        std::istringstream stream(text);
        std::string line;

        // Process newlines first
        while(std::getline(stream, line))
        {
            if(line.empty())
            {
                curY += lineHeight;
                continue;
            }

            std::istringstream lineStream(line);
            std::string word;
            std::string currentLine;

            while(lineStream >> word)
            {
                std::string testLine = currentLine.empty() ? word : (currentLine + " " + word);
                int testW = getTextWidth(testLine, scale);

                if(testW > maxLineWidth && !currentLine.empty())
                {
                    drawString(currentLine, x, curY, scale, color, dropShadow);
                    curY += lineHeight;
                    currentLine = word;
                }
                else
                {
                    currentLine = testLine;
                }
            }

            if(!currentLine.empty())
            {
                drawString(currentLine, x, curY, scale, color, dropShadow);
                curY += lineHeight;
            }
        }
        return curY - y;
    }

} // namespace OpenArtOfWar
