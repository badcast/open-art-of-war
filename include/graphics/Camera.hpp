#pragma once

#include "core/Config.hpp"
#include <algorithm>

namespace OpenArtOfWar
{

    class Camera
    {
    public:
        Camera(int viewportW = DEFAULT_WINDOW_WIDTH, int viewportH = DEFAULT_WINDOW_HEIGHT);

        void SetViewport(int w, int h);
        void SetPosition(float x, float y);
        void Pan(float dx, float dy);
        void SetZoom(float newZoom, float pivotX = -1.0f, float pivotY = -1.0f);
        // Range the zoom stays in (the mouse wheel and setZoom); the current zoom is moved into it
        void SetZoomLimits(float minZoom, float maxZoom);
        // The same view at 1/divisor of the resolution (viewport and zoom divided, zoom limits do not apply)
        Camera ScaledDown(int divisor) const;
        float GetMinZoom() const
        {
            return m_minZoom;
        }
        float GetMaxZoom() const
        {
            return m_maxZoom;
        }
        void CenterOn(float targetWorldX, float targetWorldY);
        void ClampToBounds(int mapWidthTiles, int mapHeightTiles);

        void WorldToScreen(float wx, float wy, float &sx, float &sy) const;
        void ScreenToWorld(float sx, float sy, float &wx, float &wy) const;

        float GetX() const
        {
            return m_x;
        }
        float GetY() const
        {
            return m_y;
        }
        float GetZoom() const
        {
            return m_zoom;
        }
        int GetViewportWidth() const
        {
            return m_viewportW;
        }
        int GetViewportHeight() const
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
