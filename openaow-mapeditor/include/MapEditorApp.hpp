#pragma once

#include <SDL3/SDL.h>
#include <string>
#include <memory>
#include "MapDocument.hpp"
#include "EditorUI.hpp"
#include "graphics/TextureManager.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/Camera.hpp"

namespace OpenArtOfWar {

class MapEditorApp {
public:
    MapEditorApp();
    ~MapEditorApp();

    bool Init(int width = 1280, int height = 720, const std::string& initialMap = "");
    void run();

private:
    void ProcessEvents();
    void Update(float deltaTime);
    void render();
    void ScreenToTile(float screenX, float screenY, int& tileX, int& tileY) const;
    void UpdateWindowTitle();
    void EraseAtTile(int tileX, int tileY);

    void ApplyToolAt(int tileX, int tileY, bool isDrag = false);

    SDL_Window* m_window{nullptr};
    SDL_Renderer* m_sdlRenderer{nullptr};
    bool m_running{false};

    int m_windowW{1280};
    int m_windowH{720};

    std::unique_ptr<TextureManager> m_texManager;
    std::unique_ptr<Renderer> m_renderer;
    Camera m_camera;
    MapDocument m_doc;
    EditorUI m_ui;

    bool m_isPanning{false};
    std::string m_windowTitle;
    int m_lastMouseX{0};
    int m_lastMouseY{0};

    bool m_isPainting{false};
    int m_lastPaintedTileX{-1};
    int m_lastPaintedTileY{-1};
    bool m_isRightDeleting{false};
    bool m_rightDeleteUndoPushed{false};
    int m_lastDeletedTileX{-1};
    int m_lastDeletedTileY{-1};

    bool m_isDraggingPlayer{false};
    int m_draggingPlayerIndex{-1};
    int m_draggedTileX{-1};
    int m_draggedTileY{-1};
    int m_selectedPlayerIndex{-1};

    float m_rightClickStartX{0.0f};
    float m_rightClickStartY{0.0f};
    bool m_rightClickMoved{false};

    float m_globalAnimTimer{0.0f};
};

} // namespace OpenArtOfWar
