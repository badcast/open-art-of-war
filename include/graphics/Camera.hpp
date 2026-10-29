#pragma once

#include "core/Config.hpp"
#include <algorithm>

namespace OpenArtOfWar
{

    class Camera
    {
    public:
        Camera(int viewportW = DEFAULT_WINDOW_WIDTH, int viewportH = DEFAULT_WINDOW_HEIGHT);

        void setViewport(int w, int h);
        void setPosition(float x, float y);
        void pan(float dx, float dy);
        void setZoom(float newZoom, float pivotX = -1.0f, float pivotY = -1.0f);
        // Range the zoom stays in (the mouse wheel and setZoom); the current zoom is moved into it
        void setZoomLimits(float minZoom, float maxZoom);
        // The same view at 1/divisor of the resolution (viewport and zoom divided, zoom limits do not apply)
        Camera scaledDown(int divisor) const;
        float getMinZoom() const
        {
            return m_minZoom;
        }
        float getMaxZoom() const
        {
            return m_maxZoom;
        }
        void centerOn(float targetWorldX, float targetWorldY);
        void clampToBounds(int mapWidthTiles, int mapHeightTiles);

        void worldToScreen(float wx, float wy, float &sx, float &sy) const;
        void screenToWorld(float sx, float sy, float &wx, float &wy) const;

        float getX() const
        {
            return m_x;
        }
        float getY() const
        {
            return m_y;
        }
        float getZoom() const
        {
            return m_zoom;
        }
        int getViewportWidth() const
        {
            return m_viewportW;
        }
        int getViewportHeight() const
        {
            return m_viewportH;
        }

    private:
        int m_viewportW {DEFAULT_WINDOW_WIDTH};
        int m_viewportH {DEFAULT_WINDOW_HEIGHT};
        float m_x {0.0f};
        float m_y {0.0f};
        float m_zoom {2.0f};
        float m_minZoom {0.5f};
        float m_maxZoom {4.0f};
    };

} // namespace OpenArtOfWar
