#include "input/InputManager.hpp"
#include <cstring>
#include <cmath>

namespace OpenArtOfWar
{

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
#if ANDROID
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
#if ANDROID
    if (m_touchPointer)
    {
        mouseX = static_cast<float>(m_mouseScreenX);
        mouseY = static_cast<float>(m_mouseScreenY);
    }
    else
    {
        SDL_GetMouseState(&mouseX, &mouseY);
        if (m_touchRenderer)
            SDL_RenderCoordinatesFromWindow(m_touchRenderer, mouseX, mouseY, &mouseX, &mouseY);
    }
#else
    SDL_GetMouseState(&mouseX, &mouseY);
#endif
    m_mouseScreenX = static_cast<int>(mouseX);
    m_mouseScreenY = static_cast<int>(mouseY);
    camera.ScreenToWorld(
        static_cast<float>(m_mouseScreenX), static_cast<float>(m_mouseScreenY), m_mouseWorldX, m_mouseWorldY);

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
#if ANDROID
    if (event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_UP
        || event.type == SDL_EVENT_FINGER_MOTION || event.type == SDL_EVENT_FINGER_CANCELED)
        return ProcessTouch(event, camera);
    if ((event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which != SDL_TOUCH_MOUSEID)
        || ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
            && event.button.which != SDL_TOUCH_MOUSEID))
        m_touchPointer = false;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
    {
        m_fingers.clear();
        m_touchGesture = false;
        m_leftMouseDown = m_leftPressed = m_leftClickIssued = m_rightClickIssued = m_middleMouseDown = false;
        m_selectionBox.active = false;
    }
#endif
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
#if ANDROID
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

                // Immediate click dispatch for the UI sidebar (right of the map viewport) and the top debug bar
                if (static_cast<int>(event.button.x) >= camera.GetViewportWidth()
                    || static_cast<int>(event.button.y) <= 40)
                {
                    m_leftClickIssued = true;
                    m_selectionBox.active = false;
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
            }
            else if (event.button.button == SDL_BUTTON_MIDDLE)
            {
                m_middleMouseDown = true;
                m_lastMouseX = static_cast<int>(event.button.x);
                m_lastMouseY = static_cast<int>(event.button.y);
            }
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            if (event.button.button == SDL_BUTTON_LEFT)
            {
                m_leftMouseDown = false;
                if (m_selectionBox.active)
                {
                    if (!m_selectionBox.HasSignificantArea())
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
                float dx = static_cast<float>(m_lastMouseX - static_cast<int>(event.motion.x));
                float dy = static_cast<float>(m_lastMouseY - static_cast<int>(event.motion.y));
                camera.Pan(dx, dy);
                m_lastMouseX = static_cast<int>(event.motion.x);
                m_lastMouseY = static_cast<int>(event.motion.y);
            }
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL:
        {
            // Only counted here: the game decides who gets the steps (a list, the sidebar, or the camera zoom when
            // the cursor is on the open map and no window is in the way - GameplayController)
            const int step = (event.wheel.y > 0.0f) ? 1 : ((event.wheel.y < 0.0f) ? -1 : 0);
            m_wheel += step;
            if (event.wheel.mouse_x >= static_cast<float>(camera.GetViewportWidth()))
                m_uiWheel += step;
            else
                m_mapWheel += step;
            break;
        }
    }
    return true;
}

#if ANDROID
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
            m_touchMap = !m_menuOpen && point.x < camera.GetViewportWidth();
            m_touchOrigin = point;
            m_touchStart = SDL_GetTicks();
            SetMouseScreenPosition(static_cast<int>(point.x), static_cast<int>(point.y));
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
            // Touch UI clicks fire on release, allowing a hold to cancel a production order.
            m_leftClickIssued = false;
        }
        else
        {
            m_touchGesture = true;
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
        it->second = point;
        if (!m_touchGesture)
        {
            SetMouseScreenPosition(static_cast<int>(point.x), static_cast<int>(point.y));
            m_touchMoved = m_touchMoved || std::hypot(point.x - m_touchOrigin.x, point.y - m_touchOrigin.y) > 12.0f;
        }
        return true;
    }
    m_fingers.erase(it);
    if (!m_touchGesture)
    {
        SetMouseScreenPosition(static_cast<int>(point.x), static_cast<int>(point.y));
        if (!m_touchMoved && !m_menuOpen && SDL_GetTicks() - m_touchStart >= 500)
        {
            // Hold: move/attack/rally on the map, cancel an order in the production grid.
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT);
        }
        else if (!m_touchMoved)
        {
            m_leftClickIssued = true;
            m_leftClickScreenX = static_cast<int>(point.x);
            m_leftClickScreenY = static_cast<int>(point.y);
            camera.ScreenToWorld(point.x, point.y, m_leftClickWorldX, m_leftClickWorldY);
        }
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
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
