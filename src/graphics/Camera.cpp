#include "graphics/Camera.hpp"

namespace OpenArtOfWar
{

    Camera::Camera(int viewportW, int viewportH) : m_viewportW(viewportW), m_viewportH(viewportH), m_zoom(2.0f)
    {
    }

    void Camera::SetViewport(int w, int h)
    {
        m_viewportW = std::max(1, w);
        m_viewportH = std::max(1, h);
    }

    void Camera::SetPosition(float x, float y)
    {
        m_x = x;
        m_y = y;
    }

    void Camera::Pan(float dx, float dy)
    {
        m_x += dx / m_zoom;
        m_y += dy / m_zoom;
    }

    void Camera::SetZoom(float newZoom, float pivotX, float pivotY)
    {
        float clamped = std::clamp(newZoom, m_minZoom, m_maxZoom);
        if(clamped == m_zoom)
            return;

        if(pivotX < 0 || pivotY < 0)
        {
            pivotX = m_viewportW * 0.5f;
            pivotY = m_viewportH * 0.5f;
        }

        float worldPivotX, worldPivotY;
        ScreenToWorld(pivotX, pivotY, worldPivotX, worldPivotY);

        m_zoom = clamped;

        m_x = worldPivotX - (pivotX / m_zoom);
        m_y = worldPivotY - (pivotY / m_zoom);
    }

    void Camera::SetZoomLimits(float minZoom, float maxZoom)
    {
        m_minZoom = std::min(minZoom, maxZoom);
        m_maxZoom = std::max(minZoom, maxZoom);
        SetZoom(m_zoom);
    }

    Camera Camera::ScaledDown(int divisor) const
    {
        Camera c = *this;
        divisor = std::max(1, divisor);
        c.m_viewportW = (m_viewportW + divisor - 1) / divisor;
        c.m_viewportH = (m_viewportH + divisor - 1) / divisor;
        c.m_zoom = m_zoom / divisor;
        c.m_minZoom = m_minZoom / divisor;
        c.m_maxZoom = m_maxZoom / divisor;
        return c;
    }

    void Camera::CenterOn(float targetWorldX, float targetWorldY)
    {
        m_x = targetWorldX - (m_viewportW * 0.5f / m_zoom);
        m_y = targetWorldY - (m_viewportH * 0.5f / m_zoom);
    }

    void Camera::ClampToBounds(int mapWidthTiles, int mapHeightTiles)
    {
        float maxWorldW = static_cast<float>(mapWidthTiles * TILE_WIDTH);
        float maxWorldH = static_cast<float>(mapHeightTiles * TILE_HEIGHT);

        float viewWorldW = m_viewportW / m_zoom;
        float viewWorldH = m_viewportH / m_zoom;

        // Allow panning past the map boundaries, but only up to the center of the screen
        // (At (0,0) camera position, screen center is at world (0,0): half black screen, half map)
        float minX = -viewWorldW * 0.5f;
        float maxX = maxWorldW - viewWorldW * 0.5f;
        float minY = -viewWorldH * 0.5f;
        float maxY = maxWorldH - viewWorldH * 0.5f;

        m_x = std::clamp(m_x, minX, maxX);
        m_y = std::clamp(m_y, minY, maxY);
    }

    void Camera::WorldToScreen(float wx, float wy, float &sx, float &sy) const
    {
        sx = (wx - m_x) * m_zoom;
        sy = (wy - m_y) * m_zoom;
    }

    void Camera::ScreenToWorld(float sx, float sy, float &wx, float &wy) const
    {
        wx = m_x + (sx / m_zoom);
        wy = m_y + (sy / m_zoom);
    }

} // namespace OpenArtOfWar
