#include "MapEditorApp.hpp"
#include "core/AssetStore.hpp"
#include <cstring>
#include "core/Localization.hpp"
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace OpenArtOfWar {

MapEditorApp::MapEditorApp() {
}

MapEditorApp::~MapEditorApp() {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (m_sdlRenderer) SDL_DestroyRenderer(m_sdlRenderer);
    if (m_window) SDL_DestroyWindow(m_window);
    SDL_Quit();
}

bool MapEditorApp::init(int width, int height, const std::string& initialMap) {
    m_windowW = width;
    m_windowH = height;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::cerr << "[MapEditorApp] SDL_Init Error: " << SDL_GetError() << std::endl;
        return false;
    }

    Localization::init("assets/locale", "en");

    m_window = SDL_CreateWindow(
        _("Open Art Of War Map Editor - [openaow-mapeditor]"),
        m_windowW, m_windowH,
        SDL_WINDOW_RESIZABLE
    );
    if (!m_window) {
        std::cerr << "[MapEditorApp] SDL_CreateWindow Error: " << SDL_GetError() << std::endl;
        return false;
    }

    m_sdlRenderer = SDL_CreateRenderer(m_window, nullptr);
    if (!m_sdlRenderer) {
        std::cerr << "[MapEditorApp] SDL_CreateRenderer Error: " << SDL_GetError() << std::endl;
        return false;
    }
    SDL_SetRenderVSync(m_sdlRenderer, 1);

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    std::vector<uint8_t> fontData;
    if (Assets::readBytes("assets/fonts/Roboto-Regular.ttf", fontData) && !fontData.empty()) {
        // ImGui owns (and frees) the buffer it is given
        void* fontCopy = IM_ALLOC(fontData.size());
        std::memcpy(fontCopy, fontData.data(), fontData.size());
        if (!io.Fonts->AddFontFromMemoryTTF(fontCopy, static_cast<int>(fontData.size()), 16.0f, nullptr,
                                            io.Fonts->GetGlyphRangesCyrillic())) {
            std::cerr << "[MapEditorApp] Failed to load UI font" << std::endl;
        }
    }

    // Professional Dark Theme with military RTS accents
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.14f, 0.17f, 0.94f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.35f, 0.28f, 0.85f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.45f, 0.35f, 0.95f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.32f, 0.26f, 0.85f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.48f, 0.36f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.58f, 0.42f, 1.0f);

    ImGui_ImplSDL3_InitForSDLRenderer(m_window, m_sdlRenderer);
    ImGui_ImplSDLRenderer3_Init(m_sdlRenderer);

    // Initialize Camera, TextureManager, and Renderer
    m_camera.setViewport(m_windowW, m_windowH);
    m_texManager = std::make_unique<TextureManager>(m_sdlRenderer);
    m_doc.setTextureManager(m_texManager.get());
    m_renderer = std::make_unique<Renderer>(m_sdlRenderer, m_texManager.get());

    std::string assetRoot = "assets";
    if (!m_texManager->loadAllAssets(assetRoot)) {
        assetRoot = "../assets";
        if (!m_texManager->loadAllAssets(assetRoot)) {
            assetRoot = "../../assets";
            m_texManager->loadAllAssets(assetRoot);
        }
    }

    // Load initial map or create default
    if (!initialMap.empty()) {
        if (!m_doc.loadMap(initialMap)) {
            m_doc.newMap(60, 40, 0, 0);
        }
    } else {
        if (!m_doc.loadMap("assets/maps/m0.json")) {
            m_doc.newMap(60, 40, 0, 0);
        }
    }

    // Center camera on map
    float mw = m_doc.getWidth() * TILE_WIDTH * 0.5f;
    float mh = m_doc.getHeight() * TILE_HEIGHT * 0.5f;
    m_camera.centerOn(mw, mh);

    m_running = true;
    std::cout << "[MapEditorApp] Initialized successfully. Map: " << m_doc.getName()
              << " (" << m_doc.getWidth() << "x" << m_doc.getHeight() << ")" << std::endl;
    return true;
}

void MapEditorApp::run() {
    Uint64 lastTime = SDL_GetPerformanceCounter();

    while (m_running) {
        Uint64 currentTime = SDL_GetPerformanceCounter();
        float deltaTime = static_cast<float>(currentTime - lastTime) / SDL_GetPerformanceFrequency();
        lastTime = currentTime;
        deltaTime = std::clamp(deltaTime, 0.001f, 0.1f);

        processEvents();
        update(deltaTime);
        render();
    }
}

void MapEditorApp::processEvents() {
    SDL_Event event;
    ImGuiIO& io = ImGui::GetIO();

    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);

        if (event.type == SDL_EVENT_QUIT) {
            m_ui.requestClose(m_doc);
        } else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                m_windowW = event.window.data1;
                m_windowH = event.window.data2;
                m_camera.setViewport(m_windowW, m_windowH);
            } else {
                m_ui.requestClose(m_doc);
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (!io.WantCaptureMouse) {
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    int tx, ty;
                    screenToTile(event.button.x, event.button.y, tx, ty);
                    m_isRightDeleting = true;
                    m_rightDeleteUndoPushed = false;
                    m_lastDeletedTileX = -1;
                    m_lastDeletedTileY = -1;
                    eraseAtTile(tx, ty);
                } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                    m_isPanning = true;
                    m_lastMouseX = event.button.x;
                    m_lastMouseY = event.button.y;
                } else if (event.button.button == SDL_BUTTON_LEFT) {
                    int tx, ty;
                    screenToTile(event.button.x, event.button.y, tx, ty);

                    if (m_ui.getActiveTool() != EditorTool::Select &&
                        m_ui.getActiveTool() != EditorTool::Eyedropper) {
                        m_doc.pushUndoSnapshot();
                    }
                    m_isPainting = true;
                    m_lastPaintedTileX = tx;
                    m_lastPaintedTileY = ty;
                    applyToolAt(tx, ty, false);
                }
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            if (event.button.button == SDL_BUTTON_MIDDLE) {
                m_isPanning = false;
            } else if (event.button.button == SDL_BUTTON_RIGHT) {
                m_isRightDeleting = false;
                m_rightDeleteUndoPushed = false;
                m_lastDeletedTileX = -1;
                m_lastDeletedTileY = -1;
            } else if (event.button.button == SDL_BUTTON_LEFT) {
                m_isPainting = false;
                m_lastPaintedTileX = -1;
                m_lastPaintedTileY = -1;
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            if (m_isPanning) {
                float dx = static_cast<float>(event.motion.x - m_lastMouseX);
                float dy = static_cast<float>(event.motion.y - m_lastMouseY);
                m_camera.pan(-dx, -dy);
                m_lastMouseX = event.motion.x;
                m_lastMouseY = event.motion.y;
            } else if (m_isPainting && !io.WantCaptureMouse) {
                int tx, ty;
                screenToTile(event.motion.x, event.motion.y, tx, ty);

                if (tx != m_lastPaintedTileX || ty != m_lastPaintedTileY) {
                    m_lastPaintedTileX = tx;
                    m_lastPaintedTileY = ty;
                    applyToolAt(tx, ty, true);
                }
            } else if (m_isRightDeleting && !io.WantCaptureMouse) {
                int tx, ty;
                screenToTile(event.motion.x, event.motion.y, tx, ty);
                if (tx != m_lastDeletedTileX || ty != m_lastDeletedTileY) {
                    eraseAtTile(tx, ty);
                }
            }
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            if (!io.WantCaptureMouse) {
                float mouseX = 0.0f, mouseY = 0.0f;
                SDL_GetMouseState(&mouseX, &mouseY);
                float oldZoom = m_camera.getZoom();
                float newZoom = oldZoom * (event.wheel.y > 0 ? 1.15f : 0.87f);
                newZoom = std::clamp(newZoom, 0.35f, 3.5f);
                m_camera.setZoom(newZoom, static_cast<float>(mouseX), static_cast<float>(mouseY));
            }
        } else if (event.type == SDL_EVENT_KEY_DOWN) {
            if (!io.WantCaptureKeyboard) {
                SDL_Keymod mod = SDL_GetModState();
                bool ctrl = (mod & SDL_KMOD_CTRL) != 0;

                if (ctrl && event.key.key == SDLK_Z) {
                    m_doc.undo();
                    m_ui.showToast(_("Undo action"));
                } else if (ctrl && event.key.key == SDLK_Y) {
                    m_doc.redo();
                    m_ui.showToast(_("Redo action"));
                } else if (ctrl && event.key.key == SDLK_S) {
                    if (m_doc.getFilePath().empty()) {
                        m_ui.openSaveAsDialog(m_doc);
                    } else {
                        m_doc.saveMap(m_doc.getFilePath());
                        m_ui.showToast(std::string(_("Map saved: ")) + m_doc.getFilePath());
                    }
                } else if (ctrl && event.key.key == SDLK_O) {
                    m_ui.openOpenDialog();
                } else if (ctrl && event.key.key == SDLK_N) {
                    m_ui.openNewMapDialog();
                } else if (ctrl && event.key.key == SDLK_G) {
                    m_ui.openGeneratorDialog();
                } else if (event.key.key == SDLK_F) {
                    float mw = m_doc.getWidth() * TILE_WIDTH * 0.5f;
                    float mh = m_doc.getHeight() * TILE_HEIGHT * 0.5f;
                    m_camera.centerOn(mw, mh);
                }
            }
        }
    }
}

// "<map file>[*] "<map title>" - <app name>": the open map's file name first, * while it has unsaved changes
void MapEditorApp::updateWindowTitle() {
    const std::string& path = m_doc.getFilePath();
    const std::string file = path.empty() ? m_doc.getName() + ".json (" + _("unsaved") + ")"
                                          : std::filesystem::path(path).filename().string();
    const std::string mapTitle = m_doc.getTitle();
    const std::string title = file + (m_doc.isModified() ? "*" : "") + (mapTitle.empty() ? "" : " \"" + mapTitle + "\"") +
                              " - " + _("Open Art Of War Map Editor - [openaow-mapeditor]");
    if (title != m_windowTitle) {
        m_windowTitle = title;
        SDL_SetWindowTitle(m_window, title.c_str());
    }
}

void MapEditorApp::update(float deltaTime) {
    updateWindowTitle();
    m_globalAnimTimer += deltaTime;

    // Update hovered tile
    float mouseX = 0.0f, mouseY = 0.0f;
    SDL_GetMouseState(&mouseX, &mouseY);
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse) {
        int tx, ty;
        screenToTile(mouseX, mouseY, tx, ty);
        m_ui.setHoveredTile(tx, ty);
    } else {
        m_ui.setHoveredTile(-1, -1);
    }

    // WASD Camera Panning
    if (!io.WantCaptureKeyboard) {
        const bool* state = SDL_GetKeyboardState(nullptr);
        float panSpeed = 600.0f * deltaTime;
        if (state[SDL_SCANCODE_W] || state[SDL_SCANCODE_UP]) m_camera.pan(0, -panSpeed);
        if (state[SDL_SCANCODE_S] || state[SDL_SCANCODE_DOWN]) m_camera.pan(0, panSpeed);
        if (state[SDL_SCANCODE_A] || state[SDL_SCANCODE_LEFT]) m_camera.pan(-panSpeed, 0);
        if (state[SDL_SCANCODE_D] || state[SDL_SCANCODE_RIGHT]) m_camera.pan(panSpeed, 0);
    }
}

void MapEditorApp::screenToTile(float screenX, float screenY, int& tileX, int& tileY) const {
    float wx, wy;
    m_camera.screenToWorld(screenX, screenY, wx, wy);
    tileX = static_cast<int>(std::floor(wx / TILE_WIDTH));
    tileY = static_cast<int>(std::floor(wy / TILE_HEIGHT));
}

void MapEditorApp::eraseAtTile(int tx, int ty) {
    if (tx < 0 || tx >= m_doc.getWidth() || ty < 0 || ty >= m_doc.getHeight()) return;
    m_lastDeletedTileX = tx;
    m_lastDeletedTileY = ty;
    if (!m_doc.hasObjectsInBrush(tx, ty, m_ui.getBrushSize())) return;
    if (!m_rightDeleteUndoPushed) {
        m_doc.pushUndoSnapshot();
        m_rightDeleteUndoPushed = true;
    }
    m_doc.removeObjectsBrush(tx, ty, m_ui.getBrushSize());
}

void MapEditorApp::applyToolAt(int tx, int ty, bool isDrag) {
    if (tx < 0 || tx >= m_doc.getWidth() || ty < 0 || ty >= m_doc.getHeight()) return;

    EditorTool tool = m_ui.getActiveTool();
    switch (tool) {
        case EditorTool::TileBrush:
            m_doc.setTileBrush(tx, ty, static_cast<uint8_t>(m_ui.getSelectedTile()), m_ui.getBrushSize());
            break;
        case EditorTool::BucketFill:
            if (!isDrag) {
                m_doc.floodFill(tx, ty, static_cast<uint8_t>(m_ui.getSelectedTile()));
            }
            break;
        case EditorTool::PlaceDecor:
            if (!isDrag) {
                m_doc.addDecoration(m_ui.getSelectedDecorType(), tx, ty);
            }
            break;
        case EditorTool::PlaceWall:
            if (!m_doc.setWallBrush(tx, ty, m_ui.getSelectedWallFaction(), m_ui.getBrushSize())) {
                m_ui.showToast("Нельзя разместить стену: место занято или заблокировано");
            }
            break;
        case EditorTool::PlaceBuilding:
            if (!isDrag) {
                const int type = m_ui.getSelectedBuildingType();
                if (!m_doc.addEntity(type, true, m_ui.getSelectedBuildingFaction(), tx, ty)) {
                    m_ui.showToast("Нельзя разместить постройку: место занято или заблокировано");
                }
            }
            break;
        case EditorTool::PlaceUnit:
            if (!isDrag) {
                const int type = m_ui.getSelectedUnitArchetype();
                if (!m_doc.addEntity(type, false, m_ui.getSelectedUnitFaction(), tx, ty, m_ui.getSelectedUnitFacing())) {
                    m_ui.showToast("Нельзя разместить юнита: клетка занята или заблокирована");
                }
            }
            break;
        case EditorTool::SetPlayerStart:
            m_doc.setPlayerStart(tx, ty);
            m_ui.showToast(std::string(_("Player start point (HQ): ")) +
                           "(" + std::to_string(tx) + ", " + std::to_string(ty) + ")");
            break;
        case EditorTool::SetEnemyStart:
            m_doc.setEnemyStart(tx, ty);
            m_ui.showToast(std::string(_("Enemy start point (HQ): ")) +
                           "(" + std::to_string(tx) + ", " + std::to_string(ty) + ")");
            break;
        case EditorTool::AddStartPosition:
            if (!isDrag) {
                const auto result = m_doc.toggleExtraStart(tx, ty);
                const std::string where = "(" + std::to_string(tx) + ", " + std::to_string(ty) + ")";
                if (result == MapDocument::StartToggle::Full) {
                    m_ui.showToast(_("A map holds 8 players at most: no more start positions"));
                } else {
                    const size_t players = 2 + m_doc.getExtraStarts().size();
                    m_ui.showToast(std::string(result == MapDocument::StartToggle::Added ? _("Start position added: ") : _("Start position removed: ")) +
                                   where + " - " + std::to_string(players) + " " + _("players"));
                }
            }
            break;
        case EditorTool::PassabilityBrush:
            m_doc.setPassable(tx, ty, !m_doc.getTileMap().isPassable(tx, ty));
            break;
        case EditorTool::Eraser:
            m_doc.removeObjectsBrush(tx, ty, m_ui.getBrushSize());
            break;
        case EditorTool::Eyedropper:
            if (!isDrag) {
                uint8_t sampled = m_doc.getTileMap().getBaseTile(tx, ty);
                m_ui.selectTileFromEyedropper(sampled);
                m_ui.showToast(std::string(_("Eyedropper: selected tile #")) + std::to_string(sampled));
            }
            break;
        case EditorTool::Select:
            if (!isDrag) {
                m_doc.selectEntityAt(tx, ty, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
            }
            break;
    }
}

void MapEditorApp::render() {
    SDL_SetRenderDrawColor(m_sdlRenderer, 15, 18, 22, 255);
    SDL_RenderClear(m_sdlRenderer);

    int globalAnimTick = static_cast<int>(m_globalAnimTimer * 5.0f);
    int mapW = m_doc.getWidth();
    int mapH = m_doc.getHeight();
    int tilesetId = m_doc.getTilesetId();

    float startWorldX, startWorldY, endWorldX, endWorldY;
    m_camera.screenToWorld(0, 0, startWorldX, startWorldY);
    m_camera.screenToWorld(m_camera.getViewportWidth(), m_camera.getViewportHeight(), endWorldX, endWorldY);
    int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, mapW - 1);
    int endTileX   = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, mapW);
    int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, mapH - 1);
    int endTileY   = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, mapH);

    // 1. Render Base Terrain Tiles
    for (int ty = startTileY; ty < endTileY; ++ty) {
        for (int tx = startTileX; tx < endTileX; ++tx) {
            uint8_t tileIdx = m_doc.getTileMap().getBaseTile(tx, ty);
            m_renderer->drawTileWorld(tilesetId, tileIdx, tx * TILE_WIDTH, ty * TILE_HEIGHT, m_camera, globalAnimTick);

            // Optional Grid lines
            if (m_ui.isShowGrid()) {
                m_renderer->drawRectWorld(tx * TILE_WIDTH, ty * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera, {255, 255, 255, 30});
            }
        }
    }

    // 2. Render Wall Rows (Depth-sorted)
    for (int ty = startTileY; ty < endTileY; ++ty) {
        m_doc.getTileMap().renderWallRow(ty, *m_renderer, m_camera, false, globalAnimTick, startTileX, endTileX);
    }

    // 3. Render Map Decorations
    for (const auto& dec : m_doc.getTileMap().getDecorations()) {
        if (dec.x >= startTileX - 3 && dec.x <= endTileX + 3 &&
            dec.y >= startTileY - 3 && dec.y <= endTileY + 3) {
            m_renderer->drawDecorWorld(tilesetId, dec.type, dec.x * TILE_WIDTH, dec.y * TILE_HEIGHT, m_camera, globalAnimTick);
        }
    }

    // 4. Render Placed Buildings
    for (const auto& ent : m_doc.getEntities()) {
        if (!ent.isBuilding) continue;
        float bx = ent.tileX * TILE_WIDTH;
        float by = ent.tileY * TILE_HEIGHT;
        m_renderer->drawBuildingWorld(ent.type, bx, by, 0, m_camera, ent.faction, globalAnimTick, 1.0f, false);
        m_renderer->drawBuildingWorld(ent.type, bx, by, 1, m_camera, ent.faction, globalAnimTick, 1.0f, false);

        // Selection bracket / footprint
        const SDL_Color footprintColor = ent.selected ? SDL_Color{40, 255, 90, 255}
            : (ent.faction == Faction::Resistance ? SDL_Color{255, 50, 50, 200} : SDL_Color{50, 100, 255, 200});
        m_renderer->drawRectWorld(bx, by, ent.buildWidth * TILE_WIDTH, ent.buildHeight * TILE_HEIGHT, m_camera,
                                 footprintColor);
    }

    // 5. Render Placed Units
    for (const auto& ent : m_doc.getEntities()) {
        if (ent.isBuilding) continue;
        float ux = ent.tileX * TILE_WIDTH;
        float uy = ent.tileY * TILE_HEIGHT;
        m_renderer->drawUnitWorld(ent.type, 0, ent.facing, 0, ux, uy, m_camera, ent.faction);
        const SDL_Color footprintColor = ent.selected ? SDL_Color{40, 255, 90, 255}
            : (ent.faction == Faction::Resistance ? SDL_Color{255, 100, 100, 160} : SDL_Color{100, 150, 255, 160});
        m_renderer->drawRectWorld(ent.tileX * TILE_WIDTH, ent.tileY * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera,
                                 footprintColor);
    }

    // 6. Starting Base Position Markers (Flags)
    float px = m_doc.getPlayerStartX() * TILE_WIDTH;
    float py = m_doc.getPlayerStartY() * TILE_HEIGHT;
    m_renderer->fillRectWorld(px, py, TILE_WIDTH, TILE_HEIGHT, m_camera, {255, 30, 30, 140});
    m_renderer->drawRectWorld(px, py, TILE_WIDTH, TILE_HEIGHT, m_camera, {255, 255, 255, 255});

    float ex = m_doc.getEnemyStartX() * TILE_WIDTH;
    float ey = m_doc.getEnemyStartY() * TILE_HEIGHT;
    m_renderer->fillRectWorld(ex, ey, TILE_WIDTH, TILE_HEIGHT, m_camera, {30, 80, 255, 140});
    m_renderer->drawRectWorld(ex, ey, TILE_WIDTH, TILE_HEIGHT, m_camera, {255, 255, 255, 255});

    // Extra start positions (players 3..8)
    static const SDL_Color kExtraColors[MapDocument::kMaxExtraStarts] = {
        {40, 200, 80, 150}, {240, 200, 40, 150}, {180, 60, 220, 150}, {40, 210, 220, 150}, {240, 130, 40, 150}, {230, 110, 170, 150}};
    for (size_t i = 0; i < m_doc.getExtraStarts().size(); ++i) {
        const float sx = m_doc.getExtraStarts()[i].first * TILE_WIDTH, sy = m_doc.getExtraStarts()[i].second * TILE_HEIGHT;
        m_renderer->fillRectWorld(sx, sy, TILE_WIDTH, TILE_HEIGHT, m_camera, kExtraColors[i]);
        m_renderer->drawRectWorld(sx, sy, TILE_WIDTH, TILE_HEIGHT, m_camera, {255, 255, 255, 255});
    }

    // 7. Collision Passability Overlay (if toggled)
    if (m_ui.isShowPassability()) {
        for (int ty = startTileY; ty < endTileY; ++ty) {
            for (int tx = startTileX; tx < endTileX; ++tx) {
                if (!m_doc.getTileMap().isPassable(tx, ty)) {
                    m_renderer->fillRectWorld(tx * TILE_WIDTH + 2, ty * TILE_HEIGHT + 2,
                                           TILE_WIDTH - 4, TILE_HEIGHT - 4, m_camera, {220, 30, 30, 110});
                }
            }
        }
    }

    // 8. Tool Ghost Hover Preview
    float mouseX = 0.0f, mouseY = 0.0f;
    SDL_GetMouseState(&mouseX, &mouseY);
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse) {
        int htx, hty;
        screenToTile(mouseX, mouseY, htx, hty);

        if (htx >= 0 && htx < mapW && hty >= 0 && hty < mapH) {
            EditorTool t = m_ui.getActiveTool();
            if (t == EditorTool::TileBrush) {
                int bs = m_ui.getBrushSize();
                int start = -(bs - 1) / 2;
                m_renderer->drawRectWorld((htx + start) * TILE_WIDTH, (hty + start) * TILE_HEIGHT,
                                       bs * TILE_WIDTH, bs * TILE_HEIGHT, m_camera, {0, 255, 120, 220});
            } else if (t == EditorTool::PlaceWall) {
                const int brushSize = m_ui.getBrushSize();
                const int start = -(brushSize - 1) / 2;
                const bool canPlace = m_doc.canPlaceWallBrush(htx, hty, m_ui.getSelectedWallFaction(), brushSize);
                const SDL_Color fillColor = canPlace ? SDL_Color{255, 220, 0, 120} : SDL_Color{255, 40, 40, 110};
                const SDL_Color borderColor = canPlace ? SDL_Color{255, 255, 255, 200} : SDL_Color{255, 40, 40, 255};
                const int drawX = htx + start;
                const int drawY = hty + start;
                m_renderer->fillRectWorld(drawX * TILE_WIDTH, drawY * TILE_HEIGHT,
                                          brushSize * TILE_WIDTH, brushSize * TILE_HEIGHT, m_camera, fillColor);
                m_renderer->drawRectWorld(drawX * TILE_WIDTH, drawY * TILE_HEIGHT,
                                          brushSize * TILE_WIDTH, brushSize * TILE_HEIGHT, m_camera, borderColor);
            } else if (t == EditorTool::PlaceBuilding) {
                int bw = 1;
                int bh = 1;
                const BuildingDef* buildingDef = m_texManager->getBuildingDef(m_ui.getSelectedBuildingType());
                if (buildingDef) {
                    bw = buildingDef->width;
                    bh = buildingDef->height;
                }
                const bool canPlace = m_doc.canPlaceEntity(m_ui.getSelectedBuildingType(), true, htx, hty);
                const SDL_Color previewColor = canPlace ? SDL_Color{50, 200, 255, 90} : SDL_Color{255, 40, 40, 110};
                const SDL_Color borderColor = canPlace ? SDL_Color{255, 255, 255, 200} : SDL_Color{255, 40, 40, 255};
                m_renderer->fillRectWorld(htx * TILE_WIDTH, hty * TILE_HEIGHT, bw * TILE_WIDTH, bh * TILE_HEIGHT, m_camera, previewColor);
                m_renderer->drawRectWorld(htx * TILE_WIDTH, hty * TILE_HEIGHT, bw * TILE_WIDTH, bh * TILE_HEIGHT, m_camera, borderColor);
            } else if (t == EditorTool::PlaceUnit) {
                const bool canPlace = m_doc.canPlaceEntity(m_ui.getSelectedUnitArchetype(), false, htx, hty);
                const SDL_Color previewColor = canPlace ? SDL_Color{100, 255, 100, 120} : SDL_Color{255, 40, 40, 110};
                const SDL_Color borderColor = canPlace ? SDL_Color{255, 255, 255, 200} : SDL_Color{255, 40, 40, 255};
                m_renderer->fillRectWorld(htx * TILE_WIDTH, hty * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera, previewColor);
                m_renderer->drawRectWorld(htx * TILE_WIDTH, hty * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera, borderColor);
            } else if (t == EditorTool::Eraser) {
                m_renderer->fillRectWorld(htx * TILE_WIDTH, hty * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera, {255, 0, 0, 140});
            } else {
                m_renderer->drawRectWorld(htx * TILE_WIDTH, hty * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera, {255, 255, 255, 160});
            }
        }
    }

    // 9. ImGui UI Overlay
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    m_ui.render(m_doc, m_camera, *m_texManager, *m_renderer);
    if (m_ui.shouldClose()) {
        m_running = false;
    }

    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_sdlRenderer);

    SDL_RenderPresent(m_sdlRenderer);
}

} // namespace OpenArtOfWar
