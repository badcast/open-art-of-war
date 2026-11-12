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

// The font file named in assets/main.yaml, as it is (the working directory is the folder that holds "assets",
// Assets::Init): no other folder and no system font is looked at
std::string ResolveFontPath(const std::string &path, bool bold)
{
    (void) bold;
    return path;
}

} // namespace

BitmapFontRenderer::BitmapFontRenderer(
    SDL_Renderer *renderer, const std::string &regularFontPath, const std::string &boldFontPath, const FontSizes &sizes)
    : m_renderer(renderer)
{
    if (!TTF_WasInit())
    {
        if (!TTF_Init())
        {
            std::cerr << "[BitmapFontRenderer] Failed to initialize SDL_ttf: " << SDL_GetError() << std::endl;
            return;
        }
    }

    std::string regPath = ResolveFontPath(regularFontPath, false);
    std::string bldPath = ResolveFontPath(boldFontPath, true);

    // Fonts packed in assets.blob are opened from memory (the buffers stay alive with the fonts)
    const bool packedRegular = Assets::IsBlob() && Assets::ReadBytes(regularFontPath, m_fontDataRegular);
    const bool packedBold = Assets::IsBlob() && Assets::ReadBytes(boldFontPath, m_fontDataBold);
    auto openFont = [&](bool packed, bool bold, const std::string &path, float size) {
        if (!packed)
            return TTF_OpenFont(path.c_str(), size);
        const std::vector<std::uint8_t> &data = bold ? m_fontDataBold : m_fontDataRegular;
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

    if (!m_fontSmall || !m_fontMedium || !m_fontLarge || !m_fontTitle)
    {
        const bool regularFailed = !m_fontSmall || !m_fontMedium;
        std::cerr << "[BitmapFontRenderer] Warning: Could not open the " << (regularFailed ? "regular" : "bold")
                  << " font \"" << (regularFailed ? regPath : bldPath)
                  << "\" (fonts in assets/main.yaml): " << SDL_GetError() << std::endl;
    }
    else
    {
        std::cout << "[BitmapFontRenderer] Loaded fonts: " << regPath << " / " << bldPath << std::endl;
    }
}

BitmapFontRenderer::~BitmapFontRenderer()
{
    ClearCache();

    if (m_fontSmall)
    {
        TTF_CloseFont(m_fontSmall);
        m_fontSmall = nullptr;
    }
    if (m_fontMedium)
    {
        TTF_CloseFont(m_fontMedium);
        m_fontMedium = nullptr;
    }
    if (m_fontLarge)
    {
        TTF_CloseFont(m_fontLarge);
        m_fontLarge = nullptr;
    }
    if (m_fontTitle)
    {
        TTF_CloseFont(m_fontTitle);
        m_fontTitle = nullptr;
    }
    if (m_fontSmallBold)
    {
        TTF_CloseFont(m_fontSmallBold);
        m_fontSmallBold = nullptr;
    }
    if (m_fontMediumBold)
    {
        TTF_CloseFont(m_fontMediumBold);
        m_fontMediumBold = nullptr;
    }
    if (m_fontHero)
    {
        TTF_CloseFont(m_fontHero);
        m_fontHero = nullptr;
    }
}

void BitmapFontRenderer::ClearCache()
{
    for (auto &pair : m_cache)
    {
        if (pair.second.texture)
        {
            SDL_DestroyTexture(pair.second.texture);
        }
    }
    m_cache.clear();
}

TTF_Font *BitmapFontRenderer::GetFontForScale(int scale) const
{
    if (scale == 5)
        return m_fontSmallBold ? m_fontSmallBold : m_fontSmall;
    if (scale == 6)
        return m_fontMediumBold ? m_fontMediumBold : m_fontMedium;
    if (scale >= 7)
        return m_fontHero ? m_fontHero : m_fontTitle;
    if (scale <= 1)
        return m_fontSmall ? m_fontSmall : m_fontMedium;
    if (scale == 2)
        return m_fontMedium ? m_fontMedium : m_fontSmall;
    if (scale == 3)
        return m_fontLarge ? m_fontLarge : m_fontMedium;
    return m_fontTitle ? m_fontTitle : (m_fontLarge ? m_fontLarge : m_fontMedium);
}

CachedText BitmapFontRenderer::GetOrCreateTexture(const std::string &text, int scale, SDL_Color color)
{
    if (text.empty() || !m_renderer)
    {
        return {nullptr, 0, 0, 0};
    }

    std::uint32_t colorKey = (static_cast<std::uint32_t>(color.r) << 24) | (static_cast<std::uint32_t>(color.g) << 16)
                             | (static_cast<std::uint32_t>(color.b) << 8) | (static_cast<std::uint32_t>(color.a));
    TextCacheKey key {text, scale, colorKey};

    auto it = m_cache.find(key);
    if (it != m_cache.end())
    {
        it->second.lastUsedFrame = m_currentFrame;
        return it->second;
    }

    TTF_Font *font = GetFontForScale(scale);
    if (!font)
    {
        return {nullptr, 0, 0, 0};
    }

    // Render text to surface with anti-aliasing
    SDL_Surface *surface = TTF_RenderText_Blended(font, text.c_str(), text.size(), color);
    if (!surface)
    {
        return {nullptr, 0, 0, 0};
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    int w = surface->w;
    int h = surface->h;
    SDL_DestroySurface(surface);

    if (!texture)
    {
        return {nullptr, 0, 0, 0};
    }

    // Prune cache if too large (keep under 512 entries)
    if (m_cache.size() >= 512)
    {
        auto oldest = m_cache.begin();
        for (auto iter = m_cache.begin(); iter != m_cache.end(); ++iter)
        {
            if (iter->second.lastUsedFrame < oldest->second.lastUsedFrame)
            {
                oldest = iter;
            }
        }
        if (oldest != m_cache.end())
        {
            if (oldest->second.texture)
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

int BitmapFontRenderer::GetTextWidth(const std::string &text, int scale) const
{
    if (text.empty())
        return 0;
    TTF_Font *font = GetFontForScale(scale);
    if (!font)
        return static_cast<int>(text.size()) * (scale <= 1 ? 8 : (scale * 8));

    int w = 0, h = 0;
    TTF_GetStringSize(font, text.c_str(), text.size(), &w, &h);
    return w;
}

int BitmapFontRenderer::GetTextHeight(int scale) const
{
    TTF_Font *font = GetFontForScale(scale);
    if (!font)
        return (scale <= 1 ? 12 : (scale * 8));
    return TTF_GetFontHeight(font);
}

void BitmapFontRenderer::DrawString(
    const std::string &text, int x, int y, int scale, SDL_Color color, bool dropShadow, int anchor)
{
    if (text.empty() || !m_renderer)
        return;

    m_currentFrame++;

    int totalW = GetTextWidth(text, scale);
    int totalH = GetTextHeight(scale);

    if (anchor & Anchor::HCENTER)
    {
        x -= totalW / 2;
    }
    else if (anchor & Anchor::RIGHT)
    {
        x -= totalW;
    }

    if (anchor & Anchor::VCENTER)
    {
        y -= totalH / 2;
    }
    else if (anchor & Anchor::BOTTOM)
    {
        y -= totalH;
    }

    // Shadow pass
    if (dropShadow)
    {
        SDL_Color shadowCol = {0, 0, 0, 210};
        CachedText shadow = GetOrCreateTexture(text, scale, shadowCol);
        if (shadow.texture)
        {
            int shadowOffset = (scale == 3 || scale == 4 || scale >= 7) ? 2 : 1;
            SDL_FRect dstRect
                = {static_cast<float>(x + shadowOffset),
                   static_cast<float>(y + shadowOffset),
                   static_cast<float>(shadow.width),
                   static_cast<float>(shadow.height)};
            SDL_RenderTexture(m_renderer, shadow.texture, nullptr, &dstRect);
        }
    }

    // Main text pass
    CachedText mainText = GetOrCreateTexture(text, scale, color);
    if (mainText.texture)
    {
        SDL_FRect dstRect
            = {static_cast<float>(x),
               static_cast<float>(y),
               static_cast<float>(mainText.width),
               static_cast<float>(mainText.height)};
        SDL_RenderTexture(m_renderer, mainText.texture, nullptr, &dstRect);
    }
}

int BitmapFontRenderer::DrawTextWrapped(
    const std::string &text, int x, int y, int maxLineWidth, int scale, SDL_Color color, bool dropShadow)
{
    if (text.empty() || !m_renderer)
        return 0;

    int lineHeight = GetTextHeight(scale) + 3;
    int curY = y;

    std::istringstream stream(text);
    std::string line;

    // Process newlines first
    while (std::getline(stream, line))
    {
        if (line.empty())
        {
            curY += lineHeight;
            continue;
        }

        std::istringstream lineStream(line);
        std::string word;
        std::string currentLine;

        while (lineStream >> word)
        {
            std::string testLine = currentLine.empty() ? word : (currentLine + " " + word);
            int testW = GetTextWidth(testLine, scale);

            if (testW > maxLineWidth && !currentLine.empty())
            {
                DrawString(currentLine, x, curY, scale, color, dropShadow);
                curY += lineHeight;
                currentLine = word;
            }
            else
            {
                currentLine = testLine;
            }
        }

        if (!currentLine.empty())
        {
            DrawString(currentLine, x, curY, scale, color, dropShadow);
            curY += lineHeight;
        }
    }
    return curY - y;
}

} // namespace OpenArtOfWar
