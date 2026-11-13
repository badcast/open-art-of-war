#include "input/InputManager.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace OpenArtOfWar
{

namespace
{

bool IsDirectPointerMouseEvent(SDL_MouseID which)
{
#if OAOW_TOUCH
    return which != SDL_TOUCH_MOUSEID;
#else
    (void) which;
    return true;
#endif
}

} // namespace

void InputManager::TouchMapDragState::Begin(float x, float y, Uint64 ticks)
{
    active = true;
    moved = false;
    panning = false;
    selecting = false;
    startTicks = ticks;
    originX = x;
    originY = y;
    prevX = x;
    prevY = y;
}

void InputManager::TouchMapDragState::Reset()
{
    active = false;
    moved = false;
    panning = false;
    selecting = false;
}

bool InputManager::TouchMapDragState::HoldReady(Uint64 nowTicks, bool menuOpen, bool panOnly) const
{
    return active && !menuOpen && !panOnly && !panning && (selecting || (nowTicks - startTicks >= kTouchHoldSelectMs));
}

void InputManager::UpdateTouchMapDrag(
    TouchMapDragState &drag, float x, float y, Camera &camera, bool clearLeftDownOnPan)
{
    drag.moved = drag.moved || std::hypot(x - drag.originX, y - drag.originY) > kTouchDragSlopPx;
    const float dx = drag.prevX - x;
    const float dy = drag.prevY - y;
    drag.prevX = x;
    drag.prevY = y;
    if (!drag.moved)
        return;

    if (!drag.panning && !drag.selecting)
    {
        drag.selecting = !m_touchPan && (SDL_GetTicks() - drag.startTicks >= kTouchHoldSelectMs);
        drag.panning = !drag.selecting;
        if (drag.selecting)
        {
            m_selectionBox.active = true;
            m_selectionBox.startScreenX = static_cast<int>(drag.originX);
            m_selectionBox.startScreenY = static_cast<int>(drag.originY);
            camera.ScreenToWorld(drag.originX, drag.originY, m_selectionBox.startWorldX, m_selectionBox.startWorldY);
        }
    }
    if (drag.panning)
    {
        m_selectionBox.active = false;
        if (clearLeftDownOnPan)
            m_leftMouseDown = false;
        camera.Pan(dx, dy);
    }
    else if (drag.selecting)
    {
        m_selectionBox.currentScreenX = static_cast<int>(x);
        m_selectionBox.currentScreenY = static_cast<int>(y);
        camera.ScreenToWorld(x, y, m_selectionBox.currentWorldX, m_selectionBox.currentWorldY);
    }
}

bool InputManager::IsTouchSelectionHoldReady(int &screenX, int &screenY) const
{
    if (!m_touchMode)
        return false;
    const Uint64 nowTicks = SDL_GetTicks();
    if (m_touchMouseDrag.HoldReady(nowTicks, m_menuOpen, m_touchPan))
    {
        screenX = static_cast<int>(m_touchMouseDrag.originX);
        screenY = static_cast<int>(m_touchMouseDrag.originY);
        return true;
    }
#if OAOW_TOUCH
    if (!m_touchGesture && m_fingers.size() == 1 && !m_touchOnGhost && !m_touchMoved
        && m_touchDrag.HoldReady(nowTicks, m_menuOpen, m_touchPan))
    {
        screenX = static_cast<int>(m_touchDrag.originX);
        screenY = static_cast<int>(m_touchDrag.originY);
        return true;
    }
#endif
    return false;
}

InputManager::InputManager()
{
    m_keystates = SDL_GetKeyboardState(nullptr);
    std::memset(m_prevKeystates, 0, sizeof(m_prevKeystates));
    std::memset(m_keysJustPressed, 0, sizeof(m_keysJustPressed));
}

void InputManager::ResetFrameEvents()
{
    // Note: Do not clear click/action events here, as simulation updates run on fixed timesteps
    // and events must persist until consumed by the update logic.
}

void InputManager::UpdateKeystates()
{
    if (m_keystates)
    {
        std::memcpy(m_prevKeystates, m_keystates, SDL_SCANCODE_COUNT);
    }
}

void InputManager::UpdateCamera(
    float frameDeltaTime, Camera &camera, int mapWidthTiles, int mapHeightTiles, bool menuOpen)
{
    if (menuOpen || m_menuOpen)
    {
        return;
    }
#if OAOW_TOUCH
    // A lifted finger leaves a stale pointer at the edge: never edge-scroll on touch devices.
    camera.ClampToBounds(mapWidthTiles, mapHeightTiles);
    return;
#endif
    float panSpeed = 650.0f * frameDeltaTime; // pixels/sec in world space
    float curX = camera.GetX();
    float curY = camera.GetY();
    const int margin = m_edgePanMargin;
    if ((m_keystates && (m_keystates[SDL_SCANCODE_W] || m_keystates[SDL_SCANCODE_UP])) || m_mouseScreenY <= margin)
    {
        curY -= panSpeed;
    }
    if ((m_keystates && (m_keystates[SDL_SCANCODE_S] || m_keystates[SDL_SCANCODE_DOWN]))
        || m_mouseScreenY >= camera.GetViewportHeight() - margin)
    {
        curY += panSpeed;
    }
    if ((m_keystates && (m_keystates[SDL_SCANCODE_A] || m_keystates[SDL_SCANCODE_LEFT])) || m_mouseScreenX <= margin)
    {
        curX -= panSpeed;
    }
    if ((m_keystates && (m_keystates[SDL_SCANCODE_D] || m_keystates[SDL_SCANCODE_RIGHT]))
        || (m_mouseScreenX >= camera.GetViewportWidth() - margin && m_mouseScreenX < camera.GetViewportWidth()))
    {
        curX += panSpeed;
    }
    camera.SetPosition(curX, curY);
    camera.ClampToBounds(mapWidthTiles, mapHeightTiles);
}

void InputManager::Update(float /*deltaTime*/, Camera &camera)
{
    // Update mouse world position
    float mouseX = 0.0f, mouseY = 0.0f;
#if OAOW_TOUCH
    if (m_touchPointer)
    {
        mouseX = static_cast<float>(m_mouseScreenX);
        mouseY = static_cast<float>(m_mouseScreenY);
    }
    else if (m_touchMouseDrag.active)
    {
        mouseX = m_touchMouseDrag.prevX;
        mouseY = m_touchMouseDrag.prevY;
    }
    else
    {
        SDL_GetMouseState(&mouseX, &mouseY);
        if (m_touchRenderer)
            SDL_RenderCoordinatesFromWindow(m_touchRenderer, mouseX, mouseY, &mouseX, &mouseY);
    }
#else
    if (m_touchMouseDrag.active)
    {
        mouseX = m_touchMouseDrag.prevX;
        mouseY = m_touchMouseDrag.prevY;
    }
    else
    {
        SDL_GetMouseState(&mouseX, &mouseY);
    }
#endif
    m_mouseScreenX = static_cast<int>(mouseX);
    m_mouseScreenY = static_cast<int>(mouseY);
    camera.ScreenToWorld(
        static_cast<float>(m_mouseScreenX), static_cast<float>(m_mouseScreenY), m_mouseWorldX, m_mouseWorldY);

    if (m_menuOpen)
    {
        m_touchMouseDrag.Reset();
#if OAOW_TOUCH
        m_touchDrag.Reset();
#endif
    }

    if (m_selectionBox.active)
    {
        m_selectionBox.currentScreenX = m_mouseScreenX;
        m_selectionBox.currentScreenY = m_mouseScreenY;
        m_selectionBox.currentWorldX = m_mouseWorldX;
        m_selectionBox.currentWorldY = m_mouseWorldY;
    }

    UpdateKeystates();
}

bool InputManager::ProcessEvent(const SDL_Event &event, Camera &camera)
{
#if OAOW_TOUCH
    if (event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_UP
        || event.type == SDL_EVENT_FINGER_MOTION || event.type == SDL_EVENT_FINGER_CANCELED)
        return ProcessTouch(event, camera);
    if ((event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which != SDL_TOUCH_MOUSEID)
        || ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
            && event.button.which != SDL_TOUCH_MOUSEID))
        m_touchPointer = false;
#endif
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
    {
#if OAOW_TOUCH
        m_fingers.clear();
        m_touchGesture = false;
        m_touchDrag.Reset();
#endif
        m_touchMouseDrag.Reset();
        m_mouseScrollPanelPressed = m_mouseGhostPressed = false;
        m_leftMouseDown = m_leftPressed = m_leftClickIssued = m_rightClickIssued = m_middleMouseDown = false;
        m_selectionBox.active = false;
    }
    switch (event.type)
    {
        case SDL_EVENT_TEXT_INPUT:
            if (m_typed.size() < 256)
                m_typed += event.text.text;
            break;

        case SDL_EVENT_KEY_DOWN:
        {
            if (event.key.scancode == SDL_SCANCODE_BACKSPACE)
                ++m_backspaces;
            if (event.key.repeat == 0)
            {
                SDL_Scancode sc = event.key.scancode;
#if OAOW_TOUCH
                if (sc == SDL_SCANCODE_AC_BACK)
                    sc = SDL_SCANCODE_ESCAPE;
#endif
                if (sc >= 0 && sc < SDL_SCANCODE_COUNT)
                {
                    m_keysJustPressed[sc] = 1;
                }
            }
            break;
        }

        case SDL_EVENT_KEY_UP:
        {
            SDL_Scancode sc = event.key.scancode;
            if (sc >= 0 && sc < SDL_SCANCODE_COUNT)
            {
                m_keysJustPressed[sc] = 0;
            }
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        {
            if (event.button.button == SDL_BUTTON_LEFT)
            {
                m_leftMouseDown = true;
                m_leftPressed = true;
                m_leftPressX = static_cast<int>(event.button.x);
                m_leftPressY = static_cast<int>(event.button.y);
                m_leftClickScreenX = static_cast<int>(event.button.x);
                m_leftClickScreenY = static_cast<int>(event.button.y);
                camera.ScreenToWorld(
                    static_cast<int>(event.button.x),
                    static_cast<int>(event.button.y),
                    m_leftClickWorldX,
                    m_leftClickWorldY);

                m_mouseScrollPanelPressed = m_touchMode && !m_menuOpen && event.button.which != SDL_TOUCH_MOUSEID
                                            && event.button.x >= m_touchScrollPanel.x
                                            && event.button.x < m_touchScrollPanel.x + m_touchScrollPanel.w
                                            && event.button.y >= m_touchScrollPanel.y
                                            && event.button.y < m_touchScrollPanel.y + m_touchScrollPanel.h;
                m_mouseGhostPressed = m_touchMode && !m_menuOpen && event.button.which != SDL_TOUCH_MOUSEID
                                      && m_touchPlacementGhost.w > 0 && event.button.x >= m_touchPlacementGhost.x
                                      && event.button.x < m_touchPlacementGhost.x + m_touchPlacementGhost.w
                                      && event.button.y >= m_touchPlacementGhost.y
                                      && event.button.y < m_touchPlacementGhost.y + m_touchPlacementGhost.h;
                if (m_mouseGhostPressed)
                    m_touchGhostAtPress = m_touchPlacementGhost;
                // Immediate click dispatch for the UI sidebar (right of the map viewport), touch HUD panels, and debug
                // bar
                const int px = static_cast<int>(event.button.x);
                const int py = static_cast<int>(event.button.y);
                bool inTouchPanel = false;
                for (const SDL_Rect &r : m_touchUiRects)
                {
                    if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h)
                    {
                        inTouchPanel = true;
                        break;
                    }
                }
                if (px >= camera.GetViewportWidth() || (!m_touchMode && py <= 40) || inTouchPanel || m_menuOpen)
                {
                    m_leftClickIssued = !m_mouseScrollPanelPressed && !m_mouseGhostPressed;
                    m_selectionBox.active = false;
                    m_touchMouseDrag.Reset();
                }
                else if (
                    m_touchMode && !m_mouseScrollPanelPressed && !m_mouseGhostPressed
                    && IsDirectPointerMouseEvent(event.button.which))
                {
                    m_selectionBox.active = false;
                    m_leftClickIssued = false;
                    m_touchMouseDrag.Begin(event.button.x, event.button.y, SDL_GetTicks());
                }
                else if (m_touchMode && (m_mouseScrollPanelPressed || m_mouseGhostPressed))
                {
                    m_selectionBox.active = false;
                    m_touchMouseDrag.Reset();
                }
                else
                {
                    m_selectionBox.active = true;
                    m_selectionBox.startScreenX = static_cast<int>(event.button.x);
                    m_selectionBox.startScreenY = static_cast<int>(event.button.y);
                    m_selectionBox.currentScreenX = static_cast<int>(event.button.x);
                    m_selectionBox.currentScreenY = static_cast<int>(event.button.y);
                    m_selectionBox.startWorldX = m_leftClickWorldX;
                    m_selectionBox.startWorldY = m_leftClickWorldY;
                    m_selectionBox.currentWorldX = m_leftClickWorldX;
                    m_selectionBox.currentWorldY = m_leftClickWorldY;
                }
            }
            else if (event.button.button == SDL_BUTTON_RIGHT)
            {
                m_rightClickIssued = true;
                m_rightClickScreenX = static_cast<int>(event.button.x);
                m_rightClickScreenY = static_cast<int>(event.button.y);
                camera.ScreenToWorld(
                    static_cast<int>(event.button.x),
                    static_cast<int>(event.button.y),
                    m_rightClickWorldX,
                    m_rightClickWorldY);
                m_selectionBox.active = false;
                m_touchMouseDrag.Reset();
            }
            else if (event.button.button == SDL_BUTTON_MIDDLE)
            {
                m_middleMouseDown = true;
                m_lastMouseX = event.button.x;
                m_lastMouseY = event.button.y;
            }
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            if (event.button.button == SDL_BUTTON_LEFT)
            {
                m_leftMouseDown = false;
                if (m_touchMouseDrag.active && IsDirectPointerMouseEvent(event.button.which))
                {
                    const bool selecting = m_touchMouseDrag.selecting;
                    if (!m_touchMouseDrag.moved
                        && (m_touchPan || SDL_GetTicks() - m_touchMouseDrag.startTicks < kTouchHoldSelectMs))
                    {
                        m_leftClickIssued = true;
                        m_leftClickScreenX = static_cast<int>(event.button.x);
                        m_leftClickScreenY = static_cast<int>(event.button.y);
                        camera.ScreenToWorld(event.button.x, event.button.y, m_leftClickWorldX, m_leftClickWorldY);
                    }
                    if (!selecting || !m_selectionBox.HasSignificantArea())
                        m_selectionBox.active = false;
                    m_touchMouseDrag.Reset();
                    break;
                }
                if (m_mouseGhostPressed)
                {
                    const bool sameGhost = m_touchGhostAtPress.x == m_touchPlacementGhost.x
                                           && m_touchGhostAtPress.y == m_touchPlacementGhost.y
                                           && m_touchGhostAtPress.w == m_touchPlacementGhost.w
                                           && m_touchGhostAtPress.h == m_touchPlacementGhost.h;
                    const bool insideGhost = event.button.x >= m_touchPlacementGhost.x
                                             && event.button.x < m_touchPlacementGhost.x + m_touchPlacementGhost.w
                                             && event.button.y >= m_touchPlacementGhost.y
                                             && event.button.y < m_touchPlacementGhost.y + m_touchPlacementGhost.h;
                    if (sameGhost && insideGhost)
                    {
                        m_leftClickIssued = true;
                        m_leftClickScreenX = static_cast<int>(event.button.x);
                        m_leftClickScreenY = static_cast<int>(event.button.y);
                        camera.ScreenToWorld(event.button.x, event.button.y, m_leftClickWorldX, m_leftClickWorldY);
                    }
                    m_mouseGhostPressed = false;
                }
                if (m_mouseScrollPanelPressed)
                {
                    const float dx = event.button.x - m_leftPressX;
                    const float dy = event.button.y - m_leftPressY;
                    if (std::hypot(dx, dy) <= kTouchDragSlopPx)
                    {
                        m_leftClickIssued = true;
                        m_leftClickScreenX = static_cast<int>(event.button.x);
                        m_leftClickScreenY = static_cast<int>(event.button.y);
                        camera.ScreenToWorld(event.button.x, event.button.y, m_leftClickWorldX, m_leftClickWorldY);
                    }
                    m_mouseScrollPanelPressed = false;
                }
                if (m_selectionBox.active)
                {
                    if (!m_selectionBox.HasSignificantArea()
#if OAOW_TOUCH
                        && !m_touchDrag.selecting
#endif
                    )
                    {
                        m_leftClickIssued = true;
                        m_leftClickScreenX = static_cast<int>(event.button.x);
                        m_leftClickScreenY = static_cast<int>(event.button.y);
                        camera.ScreenToWorld(
                            static_cast<int>(event.button.x),
                            static_cast<int>(event.button.y),
                            m_leftClickWorldX,
                            m_leftClickWorldY);
                    }
#if OAOW_TOUCH
                    if (!m_touchDrag.selecting || !m_selectionBox.HasSignificantArea())
#endif
                        m_selectionBox.active = false;
                }
            }
            else if (event.button.button == SDL_BUTTON_MIDDLE)
            {
                m_middleMouseDown = false;
            }
            break;
        }

        case SDL_EVENT_MOUSE_MOTION:
        {
            if (m_middleMouseDown && !m_menuOpen)
            {
                const float dx = m_lastMouseX - event.motion.x;
                const float dy = m_lastMouseY - event.motion.y;
                camera.Pan(dx, dy);
                m_lastMouseX = event.motion.x;
                m_lastMouseY = event.motion.y;
            }
            else if (
                m_touchMode && m_touchMouseDrag.active && !m_menuOpen && IsDirectPointerMouseEvent(event.motion.which))
            {
                UpdateTouchMapDrag(m_touchMouseDrag, event.motion.x, event.motion.y, camera, true);
            }
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL:
        {
            // Only counted here: the game decides who gets the steps (a list, the sidebar, or the camera zoom when
            // the cursor is on the open map and no window is in the way - GameplayController)
            const float stepX = event.wheel.x;
            const float stepY = event.wheel.y;
            m_wheelX += stepX;
            m_wheel += stepY;
            bool overTouchPanel = false;
            if (m_touchMode)
            {
                for (const SDL_Rect &r : m_touchUiRects)
                {
                    if (event.wheel.mouse_x >= r.x && event.wheel.mouse_x < r.x + r.w && event.wheel.mouse_y >= r.y
                        && event.wheel.mouse_y < r.y + r.h)
                    {
                        overTouchPanel = true;
                        break;
                    }
                }
            }
            if (overTouchPanel || event.wheel.mouse_x >= static_cast<float>(camera.GetViewportWidth()))
                m_uiWheel += stepY;
            else
                m_mapWheel += stepY;
            break;
        }
    }
    return true;
}

#if OAOW_TOUCH
bool InputManager::ProcessTouch(const SDL_Event &event, Camera &camera)
{
    // Application has already converted normalized SDL touch positions to logical pixels.
    const auto &finger = event.tfinger;
    const SDL_FPoint point {finger.x, finger.y};
    m_touchPointer = true;
    auto mouse = [&](Uint32 type, Uint8 button) {
        SDL_Event e {};
        e.type = type;
        e.button.which = SDL_TOUCH_MOUSEID;
        e.button.button = button;
        e.button.x = point.x;
        e.button.y = point.y;
        ProcessEvent(e, camera);
    };
    if (event.type == SDL_EVENT_FINGER_DOWN)
    {
        m_fingers[finger.fingerID] = point;
        if (m_fingers.size() == 1)
        {
            m_touchGesture = false;
            m_touchMoved = false;
            m_touchDrag.Reset();
            m_touchOnGhost
                = m_touchMode && !m_menuOpen && m_touchPlacementGhost.w > 0 && point.x >= m_touchPlacementGhost.x
                  && point.x < m_touchPlacementGhost.x + m_touchPlacementGhost.w && point.y >= m_touchPlacementGhost.y
                  && point.y < m_touchPlacementGhost.y + m_touchPlacementGhost.h;
            m_touchOnScrollPanel
                = m_touchMode && !m_menuOpen && m_touchScrollPanel.w > 0 && point.x >= m_touchScrollPanel.x
                  && point.x < m_touchScrollPanel.x + m_touchScrollPanel.w && point.y >= m_touchScrollPanel.y
                  && point.y < m_touchScrollPanel.y + m_touchScrollPanel.h;
            m_touchGhostAtPress = m_touchPlacementGhost;
            m_touchMap = !m_menuOpen && point.x < camera.GetViewportWidth();
            for (const SDL_Rect &r : m_touchUiRects)
            {
                if (point.x >= r.x && point.x < r.x + r.w && point.y >= r.y && point.y < r.y + r.h)
                    m_touchMap = false; // a panel of the touch interface: a drag there is not a map drag
            }
            m_touchOrigin = point;
            m_touchStart = SDL_GetTicks();
            if (m_touchMode && m_touchMap)
                m_touchDrag.Begin(point.x, point.y, m_touchStart);
            m_touchScroll = 0.0f;
            SetMouseScreenPosition(static_cast<int>(point.x), static_cast<int>(point.y));
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
            // Touch UI clicks fire on release, allowing a hold to cancel a production order.
            m_leftClickIssued = false;
            // Never start a selection box on a HUD/radar panel, menu, or when single-finger camera pan is active
            if (!m_touchMap || m_touchPan || m_touchMode)
                m_selectionBox.active = false;
        }
        else
        {
            m_touchGesture = true;
            m_touchDrag.Reset();
            m_leftMouseDown = m_leftPressed = m_leftClickIssued = false;
            m_selectionBox.active = false;
        }
        return true;
    }
    auto it = m_fingers.find(finger.fingerID);
    if (it == m_fingers.end())
        return true;
    if (event.type == SDL_EVENT_FINGER_CANCELED)
    {
        m_fingers.erase(it);
        m_touchGesture = !m_fingers.empty();
        m_touchDrag.Reset();
        m_leftMouseDown = m_leftPressed = m_leftClickIssued = m_rightClickIssued = false;
        m_selectionBox.active = false;
        return true;
    }
    if (event.type == SDL_EVENT_FINGER_MOTION)
    {
        if (m_touchGesture && m_fingers.size() == 2 && m_touchMap && !m_menuOpen)
        {
            auto other = m_fingers.begin();
            if (other == it)
                ++other;
            const SDL_FPoint a = it->second, b = other->second;
            const float before = std::hypot(a.x - b.x, a.y - b.y);
            const float after = std::hypot(point.x - b.x, point.y - b.y);
            camera.Pan((a.x - point.x) * 0.5f, (a.y - point.y) * 0.5f);
            if (before > 16.0f && after > 16.0f)
                camera.SetZoom(camera.GetZoom() * after / before, (point.x + b.x) * 0.5f, (point.y + b.y) * 0.5f);
        }
        const SDL_FPoint previous = it->second;
        it->second = point;
        if (!m_touchGesture)
        {
            SetMouseScreenPosition(static_cast<int>(point.x), static_cast<int>(point.y));
            m_touchMoved
                = m_touchMoved || std::hypot(point.x - m_touchOrigin.x, point.y - m_touchOrigin.y) > kTouchDragSlopPx;
            if (m_menuOpen && m_touchMoved)
            {
                // A menu, a list, a window: the finger scrolls it. Every 32 px of travel is a wheel step (content
                // follows the finger: dragging up goes to the later rows), so every list that takes the wheel works.
                constexpr float kStepPx = 32.0f;
                if (m_touchWheelScroll)
                    m_touchScroll += point.y - previous.y;
                while (m_touchWheelScroll && std::fabs(m_touchScroll) >= kStepPx)
                {
                    SDL_Event wheel {};
                    wheel.type = SDL_EVENT_MOUSE_WHEEL;
                    wheel.wheel.y = m_touchScroll < 0.0f ? -1.0f : 1.0f;
                    wheel.wheel.mouse_x = point.x;
                    wheel.wheel.mouse_y = point.y;
                    ProcessEvent(wheel, camera);
                    m_touchScroll += m_touchScroll < 0.0f ? kStepPx : -kStepPx;
                }
                // what the touch began (a pressed button, a selection box) is dropped: it was a scroll
                m_touchDrag.Reset();
                m_selectionBox.active = false;
            }
            if (m_touchMode && !m_menuOpen && !m_touchMap && !m_touchOnScrollPanel && m_touchMoved)
            {
                m_touchScroll += point.y - previous.y;
                while (std::fabs(m_touchScroll) >= 48.0f)
                {
                    m_uiWheel += m_touchScroll < 0.0f ? -1 : 1;
                    m_touchScroll += m_touchScroll < 0.0f ? 48.0f : -48.0f;
                }
            }
            if (m_touchMode && m_touchMap && !m_menuOpen)
            {
                UpdateTouchMapDrag(m_touchDrag, point.x, point.y, camera, true);
            }
            else if (!m_touchMode && m_touchPan && m_touchMap && m_touchMoved)
            {
                m_selectionBox.active = false;
                m_leftMouseDown = false;
                camera.Pan(previous.x - point.x, previous.y - point.y);
            }
        }
        return true;
    }
    m_fingers.erase(it);
    if (!m_touchGesture)
    {
        SetMouseScreenPosition(static_cast<int>(point.x), static_cast<int>(point.y));
        if (!m_touchMoved && !m_touchOnGhost && !m_menuOpen && SDL_GetTicks() - m_touchStart >= kTouchHoldSelectMs)
        {
            // Hold: move/attack/rally on the map, cancel an order in the production grid.
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT);
        }
        else if (
            (!m_touchMoved && !m_touchOnGhost)
            || (m_touchOnGhost && m_touchGhostAtPress.x == m_touchPlacementGhost.x
                && m_touchGhostAtPress.y == m_touchPlacementGhost.y && m_touchGhostAtPress.w == m_touchPlacementGhost.w
                && m_touchGhostAtPress.h == m_touchPlacementGhost.h && point.x >= m_touchPlacementGhost.x
                && point.x < m_touchPlacementGhost.x + m_touchPlacementGhost.w && point.y >= m_touchPlacementGhost.y
                && point.y < m_touchPlacementGhost.y + m_touchPlacementGhost.h))
        {
            m_leftClickIssued = true;
            m_leftClickScreenX = static_cast<int>(point.x);
            m_leftClickScreenY = static_cast<int>(point.y);
            camera.ScreenToWorld(point.x, point.y, m_leftClickWorldX, m_leftClickWorldY);
        }
        else
        {
            // A finger that dragged past the tap slop must never turn into a stray tap click on release
            if (!m_touchDrag.selecting)
                m_selectionBox.active = false;
        }
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        m_touchDrag.Reset();
    }
    if (m_fingers.empty())
        m_touchGesture = false;
    return true;
}
#endif

bool InputManager::WasRightClickIssued(float &worldX, float &worldY)
{
    if (m_rightClickIssued)
    {
        worldX = m_rightClickWorldX;
        worldY = m_rightClickWorldY;
        m_rightClickIssued = false;
        return true;
    }
    return false;
}

bool InputManager::WasRightClickIssued(float &worldX, float &worldY, int &screenX, int &screenY)
{
    if (m_rightClickIssued)
    {
        worldX = m_rightClickWorldX;
        worldY = m_rightClickWorldY;
        screenX = m_rightClickScreenX;
        screenY = m_rightClickScreenY;
        m_rightClickIssued = false;
        return true;
    }
    return false;
}

bool InputManager::WasLeftClickIssued(float &worldX, float &worldY)
{
    if (m_leftClickIssued)
    {
        worldX = m_leftClickWorldX;
        worldY = m_leftClickWorldY;
        m_leftClickIssued = false;
        return true;
    }
    return false;
}

bool InputManager::WasLeftClickIssued(float &worldX, float &worldY, int &screenX, int &screenY)
{
    if (m_leftClickIssued)
    {
        worldX = m_leftClickWorldX;
        worldY = m_leftClickWorldY;
        screenX = m_leftClickScreenX;
        screenY = m_leftClickScreenY;
        m_leftClickIssued = false;
        return true;
    }
    return false;
}

bool InputManager::IsKeyPressed(SDL_Scancode scancode) const
{
    if (!m_keystates || scancode < 0 || scancode >= SDL_SCANCODE_COUNT)
        return false;
    return m_keystates[scancode] != 0;
}

bool InputManager::IsKeyJustPressed(SDL_Scancode scancode)
{
    if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT)
        return false;
    if (m_keysJustPressed[scancode])
    {
        m_keysJustPressed[scancode] = 0; // Strictly consume key event!
        return true;
    }
    return false;
}

bool InputManager::IsConfirmJustPressed()
{
    bool returnPressed = IsKeyJustPressed(SDL_SCANCODE_RETURN);
    bool spacePressed = IsKeyJustPressed(SDL_SCANCODE_SPACE);
    return returnPressed || spacePressed;
}

} // namespace OpenArtOfWar
