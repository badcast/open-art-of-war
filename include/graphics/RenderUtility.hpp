#pragma once

#include <SDL3/SDL.h>
#include <string>

namespace OpenArtOfWar
{

    class Renderer;
    class Camera;

    // Shared drawing helpers of the game world. Every radius / range on the map (build radius of a headquarters,
    // firing range of a tower, attack range of a unit, squad radius) is drawn through DrawRadius.
    namespace RenderUtility
    {

        // Smooth outline of an ellipse around a world point: radiusX / radiusY are world pixels (radiusY < radiusX for a
        // radius counted in map tiles, which are wider than tall). `thickness` is in screen pixels.
        void DrawRadius(Renderer &renderer, const Camera &camera, float worldX, float worldY, float radiusX, float radiusY, SDL_Color color, float thickness = 2.0f);

        // A true circle of `radius` world pixels
        void DrawRadius(Renderer &renderer, const Camera &camera, float worldX, float worldY, float radius, SDL_Color color, float thickness = 2.0f);

        // A radius counted in map tiles, drawn as the ground ellipse of the map view (tiles are wider than tall). The build
        // radius of a headquarters and the firing range of a structure both use it, so they look alike.
        void DrawTileRadius(Renderer &renderer, const Camera &camera, float worldX, float worldY, float tiles, SDL_Color color, float thickness = 2.0f);

        // A line between two world points, `widthPx` screen pixels wide
        void DrawLine(Renderer &renderer, const Camera &camera, float x0, float y0, float x1, float y1, SDL_Color color, float widthPx = 1.0f);

        // "#RRGGBB" or "#RRGGBBAA" (the # is optional); `fallback` when the text is not a color
        SDL_Color ParseHexColor(const std::string &text, SDL_Color fallback);

    } // namespace RenderUtility

} // namespace OpenArtOfWar
