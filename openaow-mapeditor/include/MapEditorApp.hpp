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

    bool init(int width = 1280, int height = 720, const std::string& initialMap = "");
    void run();

private:
    void processEvents();
    void update(float deltaTime);
    void render();
    void screenToTile(float screenX, float screenY, int& tileX, int& tileY) const;
    void updateWindowTitle();
    void eraseAtTile(int tileX, int tileY);

    void applyToolAt(int tileX, int tileY, bool isDrag = false);

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

    float m_globalAnimTimer{0.0f};
};

} // namespace OpenArtOfWar
