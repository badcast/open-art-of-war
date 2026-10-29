#pragma once

#include <string>

#include <SDL3/SDL.h>
#include "core/Config.hpp"
#include "graphics/Camera.hpp"

namespace OpenArtOfWar
{

    struct SelectionBox
    {
        bool active {false};
        float startWorldX {0.0f};
        float startWorldY {0.0f};
        float currentWorldX {0.0f};
        float currentWorldY {0.0f};
        int startScreenX {0};
        int startScreenY {0};
        int currentScreenX {0};
        int currentScreenY {0};

        bool hasSignificantArea() const
        {
            return std::abs(currentScreenX - startScreenX) > 6 || std::abs(currentScreenY - startScreenY) > 6;
        }
    };

    class InputManager
    {
    public:
        InputManager();

        void update(float deltaTime, Camera &camera);
        void updateCamera(float frameDeltaTime, Camera &camera, int mapWidthTiles, int mapHeightTiles);
        void updateKeystates();
        bool processEvent(const SDL_Event &event, Camera &camera);

        // Queries
        bool isKeyPressed(SDL_Scancode scancode) const;
        bool isKeyJustPressed(SDL_Scancode scancode);

        const SelectionBox &getSelectionBox() const
        {
            return m_selectionBox;
        }
        void cancelSelectionBox()
        {
            m_selectionBox.active = false;
        }
        bool wasRightClickIssued(float &worldX, float &worldY);
        bool wasRightClickIssued(float &worldX, float &worldY, int &screenX, int &screenY);
        bool wasLeftClickIssued(float &worldX, float &worldY);
        bool wasLeftClickIssued(float &worldX, float &worldY, int &screenX, int &screenY);
        bool isLeftMouseDown() const
        {
            return m_leftMouseDown;
        }
        // The left button went down since the last call (a click only fires on release), for grabbing scroll and seek bars
        bool consumeLeftPress(int &screenX, int &screenY)
        {
            if(!m_leftPressed)
                return false;
            m_leftPressed = false;
            screenX = m_leftPressX;
            screenY = m_leftPressY;
            return true;
        }

        int getLeftClickScreenX() const
        {
            return m_leftClickScreenX;
        }
        int getLeftClickScreenY() const
        {
            return m_leftClickScreenY;
        }

        float getMouseWorldX() const
        {
            return m_mouseWorldX;
        }
        float getMouseWorldY() const
        {
            return m_mouseWorldY;
        }
        int getMouseScreenX() const
        {
            return m_mouseScreenX;
        }
        int getMouseScreenY() const
        {
            return m_mouseScreenY;
        }

        // ENTER or SPACE (consumes both keys)
        bool isConfirmJustPressed();

        // Mouse wheel steps scrolled over the UI sidebar since the last call (positive = up)
        // Mouse wheel steps anywhere in the window since the last call (positive = up), for menus and lists
        int consumeWheel()
        {
            int v = m_wheel;
            m_wheel = 0;
            return v;
        }
        int consumeUiWheel()
        {
            int v = m_uiWheel;
            m_uiWheel = 0;
            return v;
        }

        // Text typed since the last call (UTF-8) and the Backspace presses (key repeat included), while
        // SDL text input is on (SDL_StartTextInput)
        std::string takeTypedText()
        {
            std::string t;
            t.swap(m_typed);
            return t;
        }
        int takeBackspaces()
        {
            int n = m_backspaces;
            m_backspaces = 0;
            return n;
        }

        void resetFrameEvents();

    private:
        const bool *m_keystates {nullptr};
        uint8_t m_prevKeystates[SDL_SCANCODE_COUNT] {0};
        uint8_t m_keysJustPressed[SDL_SCANCODE_COUNT] {0};

        int m_mouseScreenX {0};
        int m_mouseScreenY {0};
        float m_mouseWorldX {0.0f};
        float m_mouseWorldY {0.0f};

        SelectionBox m_selectionBox;

        bool m_leftMouseDown {false};
        bool m_leftClickIssued {false};
        bool m_leftPressed {false};
        int m_leftPressX {0};
        int m_leftPressY {0};
        float m_leftClickWorldX {0.0f};
        float m_leftClickWorldY {0.0f};
        int m_leftClickScreenX {0};
        int m_leftClickScreenY {0};

        bool m_rightClickIssued {false};
        float m_rightClickWorldX {0.0f};
        float m_rightClickWorldY {0.0f};
        int m_rightClickScreenX {0};
        int m_rightClickScreenY {0};

        bool m_middleMouseDown {false};
        int m_lastMouseX {0};
        int m_lastMouseY {0};

        int m_uiWheel {0};
        int m_wheel {0};
        std::string m_typed;
        int m_backspaces {0};
    };

} // namespace OpenArtOfWar
