#pragma once

#include <vector>
#include <string>
#if OAOW_TOUCH
#include <map>
#endif

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

    bool HasSignificantArea() const
    {
#if OAOW_TOUCH
        constexpr int threshold = 12;
#else
        constexpr int threshold = 6;
#endif
        return std::abs(currentScreenX - startScreenX) > threshold
               || std::abs(currentScreenY - startScreenY) > threshold;
    }
};

class InputManager
{
public:
    InputManager();
#if OAOW_TOUCH
    void SetRenderer(SDL_Renderer *renderer)
    {
        m_touchRenderer = renderer;
    }
#endif

    // Touch HUD: drag to pan, or hold before dragging to select.
    void SetTouchMode(bool on)
    {
        m_touchMode = on;
    }
    void SetTouchPan(bool on)
    {
        m_touchPan = on;
    }
    // A finger drag in a menu or window scrolls it by wheel steps; a window that scrolls by itself (the technology
    // tree: two directions, it follows the pointer) turns that off
    void SetTouchWheelScroll(bool on)
    {
        m_touchWheelScroll = on;
    }
    void SetTouchScrollPanel(SDL_Rect rect)
    {
        m_touchScrollPanel = rect;
    }
    void SetTouchPlacementGhost(SDL_Rect rect)
    {
        m_touchPlacementGhost = rect;
    }

    // Touch interface panels over the map (screen pixels): a finger that starts on one never pans the map
    void SetTouchUiRects(std::vector<SDL_Rect> rects)
    {
        m_touchUiRects = std::move(rects);
    }

    void Update(float deltaTime, Camera &camera);
    void UpdateCamera(
        float frameDeltaTime, Camera &camera, int mapWidthTiles, int mapHeightTiles, bool menuOpen = false);
    void UpdateKeystates();
    bool ProcessEvent(const SDL_Event &event, Camera &camera);

    void SetEdgePanMargin(int margin)
    {
        m_edgePanMargin = std::clamp(margin, 5, 30);
    }
    int GetEdgePanMargin() const
    {
        return m_edgePanMargin;
    }

    void SetMenuOpen(bool open)
    {
        m_menuOpen = open;
    }
    bool IsMenuOpen() const
    {
        return m_menuOpen;
    }

    void SetMouseScreenPosition(int x, int y)
    {
        m_mouseScreenX = x;
        m_mouseScreenY = y;
    }

    // Queries
    bool IsKeyPressed(SDL_Scancode scancode) const;
    inline bool isKeyPressed(SDL_Scancode scancode) const
    {
        return IsKeyPressed(scancode);
    }
    bool IsKeyJustPressed(SDL_Scancode scancode);

    const SelectionBox &GetSelectionBox() const
    {
        return m_selectionBox;
    }
    // Non-placing TouchHUD: true once a map press has held >= kTouchHoldSelectMs so drag switches from pan to box
    // selection.
    bool IsTouchSelectionHoldReady(int &screenX, int &screenY) const;
    void CancelSelectionBox()
    {
        m_selectionBox.active = false;
    }
    bool WasRightClickIssued(float &worldX, float &worldY);
    bool WasRightClickIssued(float &worldX, float &worldY, int &screenX, int &screenY);
    bool WasLeftClickIssued(float &worldX, float &worldY);
    bool WasLeftClickIssued(float &worldX, float &worldY, int &screenX, int &screenY);
    bool IsLeftMouseDown() const
    {
        return m_leftMouseDown;
    }
    int GetLeftPressX() const
    {
        return m_leftPressX;
    }
    int GetLeftPressY() const
    {
        return m_leftPressY;
    }
    // The left button went down since the last call (a click only fires on release), for grabbing scroll and seek bars
    bool ConsumeLeftPress(int &screenX, int &screenY)
    {
        if (!m_leftPressed)
            return false;
        m_leftPressed = false;
        screenX = m_leftPressX;
        screenY = m_leftPressY;
        return true;
    }

    int GetLeftClickScreenX() const
    {
        return m_leftClickScreenX;
    }
    int GetLeftClickScreenY() const
    {
        return m_leftClickScreenY;
    }

    float GetMouseWorldX() const
    {
        return m_mouseWorldX;
    }
    float GetMouseWorldY() const
    {
        return m_mouseWorldY;
    }
    int GetMouseScreenX() const
    {
        return m_mouseScreenX;
    }
    int GetMouseScreenY() const
    {
        return m_mouseScreenY;
    }

    // ENTER or SPACE (consumes both keys)
    bool IsConfirmJustPressed();

    // Mouse wheel steps scrolled over the UI sidebar since the last call (positive = up)
    // Mouse wheel steps anywhere in the window since the last call (positive = up), for menus and lists
    int ConsumeWheel()
    {
        int v = m_wheel;
        m_wheel = 0;
        return v;
    }
    int ConsumeUiWheel()
    {
        int v = m_uiWheel;
        m_uiWheel = 0;
        return v;
    }
    // Mouse wheel steps scrolled over the map viewport (left of the sidebar) since the last call: camera zoom
    int ConsumeMapWheel()
    {
        int v = m_mapWheel;
        m_mapWheel = 0;
        return v;
    }

    // Text typed since the last call (UTF-8) and the Backspace presses (key repeat included), while
    // SDL text input is on (SDL_StartTextInput)
    std::string TakeTypedText()
    {
        std::string t;
        t.swap(m_typed);
        return t;
    }
    int TakeBackspaces()
    {
        int n = m_backspaces;
        m_backspaces = 0;
        return n;
    }

    void ResetFrameEvents();

private:
    SDL_Rect m_touchPlacementGhost {0, 0, 0, 0};
    SDL_Rect m_touchGhostAtPress {0, 0, 0, 0};
    SDL_Rect m_touchScrollPanel {0, 0, 0, 0};
    bool m_mouseScrollPanelPressed {false};
    bool m_mouseGhostPressed {false};
    bool m_touchMode {false};
    bool m_touchPan {false};
    bool m_touchWheelScroll {true};
    float m_touchScroll {0.0f}; // finger travel not yet turned into wheel steps (menus scroll by swiping)
    std::vector<SDL_Rect> m_touchUiRects;
    static constexpr Uint64 kTouchHoldSelectMs = 200;
    static constexpr float kTouchDragSlopPx = 12.0f;
    struct TouchMapDragState
    {
        bool active {false};
        bool moved {false};
        bool panning {false};
        bool selecting {false};
        Uint64 startTicks {0};
        float originX {0.0f};
        float originY {0.0f};
        float prevX {0.0f};
        float prevY {0.0f};

        void Begin(float x, float y, Uint64 ticks);
        void Reset();
        bool HoldReady(Uint64 nowTicks, bool menuOpen, bool panOnly) const;
    };
    void UpdateTouchMapDrag(TouchMapDragState &drag, float x, float y, Camera &camera, bool clearLeftDownOnPan);
    TouchMapDragState m_touchMouseDrag;
#if OAOW_TOUCH
    bool ProcessTouch(const SDL_Event &event, Camera &camera);
    SDL_Renderer *m_touchRenderer {nullptr};
    std::map<SDL_FingerID, SDL_FPoint> m_fingers;
    bool m_touchPointer {false};
    bool m_touchGesture {false};
    bool m_touchMap {false};
    bool m_touchMoved {false};
    bool m_touchOnGhost {false};
    bool m_touchOnScrollPanel {false};
    TouchMapDragState m_touchDrag;
    Uint64 m_touchStart {0};
    SDL_FPoint m_touchOrigin {};
#endif
    const bool *m_keystates {nullptr};
    std::uint8_t m_prevKeystates[SDL_SCANCODE_COUNT] {0};
    std::uint8_t m_keysJustPressed[SDL_SCANCODE_COUNT] {0};

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
    int m_mapWheel {0};
    int m_wheel {0};
    std::string m_typed;
    int m_backspaces {0};
    int m_edgePanMargin {5};
    bool m_menuOpen {false};
};

} // namespace OpenArtOfWar
