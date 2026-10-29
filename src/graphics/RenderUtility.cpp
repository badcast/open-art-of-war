#include "graphics/RenderUtility.hpp"

#include "graphics/Camera.hpp"
#include "graphics/Renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace OpenArtOfWar::RenderUtility
{

    void DrawRadius(Renderer &renderer, const Camera &camera, float worldX, float worldY, float radiusX, float radiusY, SDL_Color color, float thickness)
    {
        float sx = 0.0f, sy = 0.0f;
        camera.worldToScreen(worldX, worldY, sx, sy);
        const float zoom = camera.getZoom();
        const float rx = radiusX * zoom, ry = radiusY * zoom;
        // A segment about 5 px long keeps even a screen-wide ring round
        const int segments = static_cast<int>(std::ceil(2.0f * static_cast<float>(M_PI) * std::max(rx, ry) / 5.0f));
        renderer.drawRingScreen(sx, sy, rx, ry, thickness, color, std::clamp(segments, 48, 720));
    }

    void DrawRadius(Renderer &renderer, const Camera &camera, float worldX, float worldY, float radius, SDL_Color color, float thickness)
    {
        DrawRadius(renderer, camera, worldX, worldY, radius, radius, color, thickness);
    }

    void DrawTileRadius(Renderer &renderer, const Camera &camera, float worldX, float worldY, float tiles, SDL_Color color, float thickness)
    {
        DrawRadius(renderer, camera, worldX, worldY, tiles * static_cast<float>(TILE_WIDTH), tiles * static_cast<float>(TILE_HEIGHT), color, thickness);
    }

    void DrawLine(Renderer &renderer, const Camera &camera, float x0, float y0, float x1, float y1, SDL_Color color, float widthPx)
    {
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        camera.worldToScreen(x0, y0, ax, ay);
        camera.worldToScreen(x1, y1, bx, by);
        renderer.drawThickLineScreen(ax, ay, bx, by, widthPx, color);
    }

    SDL_Color ParseHexColor(const std::string &text, SDL_Color fallback)
    {
        std::string s = text;
        if(!s.empty() && s[0] == '#')
            s.erase(0, 1);
        if(s.size() != 6 && s.size() != 8)
            return fallback;
        for(char c : s)
        {
            if(!std::isxdigit(static_cast<unsigned char>(c)))
                return fallback;
        }
        auto byte = [&](size_t i) { return static_cast<Uint8>(std::stoi(s.substr(i, 2), nullptr, 16)); };
        return {byte(0), byte(2), byte(4), s.size() == 8 ? byte(6) : static_cast<Uint8>(255)};
    }

} // namespace OpenArtOfWar::RenderUtility
