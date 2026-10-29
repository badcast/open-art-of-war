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

    void InputManager::resetFrameEvents()
    {
        // Note: Do not clear click/action events here, as simulation updates run on fixed timesteps
        // and events must persist until consumed by the update logic.
    }

    void InputManager::updateKeystates()
    {
        if(m_keystates)
        {
            std::memcpy(m_prevKeystates, m_keystates, SDL_SCANCODE_COUNT);
        }
    }

    void InputManager::updateCamera(float frameDeltaTime, Camera &camera, int mapWidthTiles, int mapHeightTiles)
    {
        float panSpeed = 650.0f * frameDeltaTime; // pixels/sec in world space
        float curX = camera.getX();
        float curY = camera.getY();
        if(m_keystates[SDL_SCANCODE_W] || m_keystates[SDL_SCANCODE_UP])
        {
            curY -= panSpeed;
        }
        if(m_keystates[SDL_SCANCODE_S] || m_keystates[SDL_SCANCODE_DOWN])
        {
            curY += panSpeed;
        }
        if(m_keystates[SDL_SCANCODE_A] || m_keystates[SDL_SCANCODE_LEFT])
        {
            curX -= panSpeed;
        }
        if(m_keystates[SDL_SCANCODE_D] || m_keystates[SDL_SCANCODE_RIGHT])
        {
            curX += panSpeed;
        }
        camera.setPosition(curX, curY);
        camera.clampToBounds(mapWidthTiles, mapHeightTiles);
    }

    void InputManager::update(float /*deltaTime*/, Camera &camera)
    {
        // Update mouse world position
        float mouseX = 0.0f, mouseY = 0.0f;
        SDL_GetMouseState(&mouseX, &mouseY);
        m_mouseScreenX = static_cast<int>(mouseX);
        m_mouseScreenY = static_cast<int>(mouseY);
        camera.screenToWorld(static_cast<float>(m_mouseScreenX), static_cast<float>(m_mouseScreenY), m_mouseWorldX, m_mouseWorldY);

        if(m_selectionBox.active)
        {
            m_selectionBox.currentScreenX = m_mouseScreenX;
            m_selectionBox.currentScreenY = m_mouseScreenY;
            m_selectionBox.currentWorldX = m_mouseWorldX;
            m_selectionBox.currentWorldY = m_mouseWorldY;
        }

        updateKeystates();
    }

    bool InputManager::processEvent(const SDL_Event &event, Camera &camera)
    {
        switch(event.type)
        {
            case SDL_EVENT_TEXT_INPUT:
                if(m_typed.size() < 256)
                    m_typed += event.text.text;
                break;

            case SDL_EVENT_KEY_DOWN:
            {
                if(event.key.scancode == SDL_SCANCODE_BACKSPACE)
                    ++m_backspaces;
                if(event.key.repeat == 0)
                {
                    SDL_Scancode sc = event.key.scancode;
                    if(sc >= 0 && sc < SDL_SCANCODE_COUNT)
                    {
                        m_keysJustPressed[sc] = 1;
                    }
                }
                break;
            }

            case SDL_EVENT_KEY_UP:
            {
                SDL_Scancode sc = event.key.scancode;
                if(sc >= 0 && sc < SDL_SCANCODE_COUNT)
                {
                    m_keysJustPressed[sc] = 0;
                }
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            {
                if(event.button.button == SDL_BUTTON_LEFT)
                {
                    m_leftMouseDown = true;
                    m_leftPressed = true;
                    m_leftPressX = static_cast<int>(event.button.x);
                    m_leftPressY = static_cast<int>(event.button.y);
                    m_leftClickScreenX = static_cast<int>(event.button.x);
                    m_leftClickScreenY = static_cast<int>(event.button.y);
                    camera.screenToWorld(static_cast<int>(event.button.x), static_cast<int>(event.button.y), m_leftClickWorldX, m_leftClickWorldY);

                    // Immediate click dispatch for the UI sidebar (right of the map viewport) and the top debug bar
                    if(static_cast<int>(event.button.x) >= camera.getViewportWidth() || static_cast<int>(event.button.y) <= 40)
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
                else if(event.button.button == SDL_BUTTON_RIGHT)
                {
                    m_rightClickIssued = true;
                    m_rightClickScreenX = static_cast<int>(event.button.x);
                    m_rightClickScreenY = static_cast<int>(event.button.y);
                    camera.screenToWorld(static_cast<int>(event.button.x), static_cast<int>(event.button.y), m_rightClickWorldX, m_rightClickWorldY);
                    m_selectionBox.active = false;
                }
                else if(event.button.button == SDL_BUTTON_MIDDLE)
                {
                    m_middleMouseDown = true;
                    m_lastMouseX = static_cast<int>(event.button.x);
                    m_lastMouseY = static_cast<int>(event.button.y);
                }
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                if(event.button.button == SDL_BUTTON_LEFT)
                {
                    m_leftMouseDown = false;
                    if(m_selectionBox.active)
                    {
                        if(!m_selectionBox.hasSignificantArea())
                        {
                            m_leftClickIssued = true;
                            m_leftClickScreenX = static_cast<int>(event.button.x);
                            m_leftClickScreenY = static_cast<int>(event.button.y);
                            camera.screenToWorld(static_cast<int>(event.button.x), static_cast<int>(event.button.y), m_leftClickWorldX, m_leftClickWorldY);
                        }
                        m_selectionBox.active = false;
                    }
                }
                else if(event.button.button == SDL_BUTTON_MIDDLE)
                {
                    m_middleMouseDown = false;
                }
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
            {
                if(m_middleMouseDown)
                {
                    float dx = static_cast<float>(m_lastMouseX - static_cast<int>(event.motion.x));
                    float dy = static_cast<float>(m_lastMouseY - static_cast<int>(event.motion.y));
                    camera.pan(dx, dy);
                    m_lastMouseX = static_cast<int>(event.motion.x);
                    m_lastMouseY = static_cast<int>(event.motion.y);
                }
                break;
            }

            case SDL_EVENT_MOUSE_WHEEL:
            {
                m_wheel += (event.wheel.y > 0.0f) ? 1 : ((event.wheel.y < 0.0f) ? -1 : 0);
                if(m_mouseScreenX >= camera.getViewportWidth())
                {
                    m_uiWheel += (event.wheel.y > 0.0f) ? 1 : ((event.wheel.y < 0.0f) ? -1 : 0);
                    break;
                }
                if(event.wheel.y == 0.0f)
                    break;
                float zoomDelta = (event.wheel.y > 0.0f) ? 1.15f : (1.0f / 1.15f);
                camera.setZoom(camera.getZoom() * zoomDelta, static_cast<float>(m_mouseScreenX), static_cast<float>(m_mouseScreenY));
                break;
            }
        }
        return true;
    }

    bool InputManager::wasRightClickIssued(float &worldX, float &worldY)
    {
        if(m_rightClickIssued)
        {
            worldX = m_rightClickWorldX;
            worldY = m_rightClickWorldY;
            m_rightClickIssued = false;
            return true;
        }
        return false;
    }

    bool InputManager::wasRightClickIssued(float &worldX, float &worldY, int &screenX, int &screenY)
    {
        if(m_rightClickIssued)
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

    bool InputManager::wasLeftClickIssued(float &worldX, float &worldY)
    {
        if(m_leftClickIssued)
        {
            worldX = m_leftClickWorldX;
            worldY = m_leftClickWorldY;
            m_leftClickIssued = false;
            return true;
        }
        return false;
    }

    bool InputManager::wasLeftClickIssued(float &worldX, float &worldY, int &screenX, int &screenY)
    {
        if(m_leftClickIssued)
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

    bool InputManager::isKeyPressed(SDL_Scancode scancode) const
    {
        if(!m_keystates || scancode < 0 || scancode >= SDL_SCANCODE_COUNT)
            return false;
        return m_keystates[scancode] != 0;
    }

    bool InputManager::isKeyJustPressed(SDL_Scancode scancode)
    {
        if(scancode < 0 || scancode >= SDL_SCANCODE_COUNT)
            return false;
        if(m_keysJustPressed[scancode])
        {
            m_keysJustPressed[scancode] = 0; // Strictly consume key event!
            return true;
        }
        return false;
    }

    bool InputManager::isConfirmJustPressed()
    {
        bool returnPressed = isKeyJustPressed(SDL_SCANCODE_RETURN);
        bool spacePressed = isKeyJustPressed(SDL_SCANCODE_SPACE);
        return returnPressed || spacePressed;
    }

} // namespace OpenArtOfWar
