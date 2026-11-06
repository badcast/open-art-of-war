#include "MapEditorApp.hpp"
#include "core/LoaderResources.hpp"
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

namespace OpenArtOfWar
{

MapEditorApp::MapEditorApp() {}

MapEditorApp::~MapEditorApp()
{
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (m_sdlRenderer)
        SDL_DestroyRenderer(m_sdlRenderer);
    if (m_window)
        SDL_DestroyWindow(m_window);
    SDL_Quit();
}

bool MapEditorApp::Init(int width, int height, const std::string &initialMap)
{
    m_windowW = width;
    m_windowH = height;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        std::cerr << "[MapEditorApp] SDL_Init Error: " << SDL_GetError() << std::endl;
        return false;
    }

    Localization::Init(LoaderResources::LocaleDir(), LoaderResources::DefaultLanguage());

    m_window = SDL_CreateWindow(
        _("Open Art Of War Map Editor - [openaow-mapeditor]"), m_windowW, m_windowH, SDL_WINDOW_RESIZABLE);
    if (!m_window)
    {
        std::cerr << "[MapEditorApp] SDL_CreateWindow Error: " << SDL_GetError() << std::endl;
        return false;
    }

    m_sdlRenderer = SDL_CreateRenderer(m_window, nullptr);
    if (!m_sdlRenderer)
    {
        std::cerr << "[MapEditorApp] SDL_CreateRenderer Error: " << SDL_GetError() << std::endl;
        return false;
    }
    SDL_SetRenderVSync(m_sdlRenderer, 1);

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    std::vector<uint8_t> fontData;
    if (Assets::ReadBytes(LoaderResources::FontRegular(), fontData) && !fontData.empty())
    {
        // ImGui owns (and frees) the buffer it is given
        void *fontCopy = IM_ALLOC(fontData.size());
        std::memcpy(fontCopy, fontData.data(), fontData.size());
        if (!io.Fonts->AddFontFromMemoryTTF(
                fontCopy, static_cast<int>(fontData.size()), 16.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic()))
        {
            std::cerr << "[MapEditorApp] Failed to load UI font" << std::endl;
        }
    }

    // Professional Dark Theme with military RTS accents
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
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
    m_camera.SetViewport(m_windowW, m_windowH);
    m_texManager = std::make_unique<TextureManager>(m_sdlRenderer);
    m_doc.SetTextureManager(m_texManager.get());
    m_renderer = std::make_unique<Renderer>(m_sdlRenderer, m_texManager.get());

    std::string assetRoot = LoaderResources::kDefaultRoot;
    if (!m_texManager->LoadAllAssets(assetRoot))
    {
        assetRoot = "../assets";
        if (!m_texManager->LoadAllAssets(assetRoot))
        {
            assetRoot = "../../assets";
            m_texManager->LoadAllAssets(assetRoot);
        }
    }

    // Load initial map or create default
    if (!initialMap.empty())
    {
        if (!m_doc.LoadMap(initialMap))
        {
            m_doc.NewMap(60, 40, 0, 0);
        }
    }
    else
    {
        if (!m_doc.LoadMap(LoaderResources::MapPath("m0")))
        {
            m_doc.NewMap(60, 40, 0, 0);
        }
    }

    // Center camera on map
    float mw = m_doc.GetWidth() * TILE_WIDTH * 0.5f;
    float mh = m_doc.GetHeight() * TILE_HEIGHT * 0.5f;
    m_camera.CenterOn(mw, mh);

    m_running = true;
    std::cout << "[MapEditorApp] Initialized successfully. Map: " << m_doc.GetName() << " (" << m_doc.GetWidth() << "x"
              << m_doc.GetHeight() << ")" << std::endl;
    return true;
}

void MapEditorApp::run()
{
    Uint64 lastTime = SDL_GetPerformanceCounter();

    while (m_running)
    {
        Uint64 currentTime = SDL_GetPerformanceCounter();
        float deltaTime = static_cast<float>(currentTime - lastTime) / SDL_GetPerformanceFrequency();
        lastTime = currentTime;
        deltaTime = std::clamp(deltaTime, 0.001f, 0.1f);

        ProcessEvents();
        Update(deltaTime);
        render();
    }
}

void MapEditorApp::ProcessEvents()
{
    SDL_Event event;
    ImGuiIO &io = ImGui::GetIO();

    while (SDL_PollEvent(&event))
    {
        ImGui_ImplSDL3_ProcessEvent(&event);

        if (event.type == SDL_EVENT_QUIT)
        {
            m_ui.RequestClose(m_doc);
        }
        else if (
            event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED
            || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        {
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED)
            {
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
                m_windowW = (pw > 0) ? pw : event.window.data1;
                m_windowH = (ph > 0) ? ph : event.window.data2;
                m_camera.SetViewport(m_windowW, m_windowH);
            }
            else
            {
                m_ui.RequestClose(m_doc);
            }
        }
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        {
            if (!io.WantCaptureMouse)
            {
                if (event.button.button == SDL_BUTTON_RIGHT)
                {
                    m_rightClickStartX = event.button.x;
                    m_rightClickStartY = event.button.y;
                    m_rightClickMoved = false;
                    m_isRightDeleting = false;
                    m_lastDeletedTileX = -1;
                    m_lastDeletedTileY = -1;
                }
                else if (event.button.button == SDL_BUTTON_MIDDLE)
                {
                    m_isPanning = true;
                    m_lastMouseX = static_cast<int>(event.button.x);
                    m_lastMouseY = static_cast<int>(event.button.y);
                }
                else if (event.button.button == SDL_BUTTON_LEFT)
                {
                    int tx, ty;
                    ScreenToTile(event.button.x, event.button.y, tx, ty);

                    const int pIdx = m_doc.FindStartPosAt(tx, ty);
                    if (pIdx >= 0)
                    {
                        m_isDraggingPlayer = true;
                        m_draggingPlayerIndex = pIdx;
                        m_draggedTileX = tx;
                        m_draggedTileY = ty;
                        m_selectedPlayerIndex = pIdx;
                        m_ui.SetSelectedPlayerSlot(pIdx);
                    }
                    else
                    {
                        if (m_ui.GetActiveTool() != EditorTool::Select
                            && m_ui.GetActiveTool() != EditorTool::Eyedropper)
                        {
                            m_doc.PushUndoSnapshot();
                        }
                        m_isPainting = true;
                        m_lastPaintedTileX = tx;
                        m_lastPaintedTileY = ty;
                        ApplyToolAt(tx, ty, false);
                    }
                }
            }
        }
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
        {
            if (event.button.button == SDL_BUTTON_MIDDLE)
            {
                m_isPanning = false;
            }
            else if (event.button.button == SDL_BUTTON_RIGHT)
            {
                if (!m_rightClickMoved && !io.WantCaptureMouse)
                {
                    int tx, ty;
                    ScreenToTile(event.button.x, event.button.y, tx, ty);
                    if (tx >= 0 && tx < m_doc.GetWidth() && ty >= 0 && ty < m_doc.GetHeight())
                    {
                        m_ui.OpenContextMenu(tx, ty);
                    }
                }
                m_isRightDeleting = false;
                m_rightDeleteUndoPushed = false;
                m_lastDeletedTileX = -1;
                m_lastDeletedTileY = -1;
            }
            else if (event.button.button == SDL_BUTTON_LEFT)
            {
                if (m_isDraggingPlayer && m_draggingPlayerIndex >= 0)
                {
                    int tx, ty;
                    ScreenToTile(event.button.x, event.button.y, tx, ty);
                    const auto [origX, origY] = m_doc.GetStartPos(m_draggingPlayerIndex);
                    if (tx >= 0 && tx < m_doc.GetWidth() && ty >= 0 && ty < m_doc.GetHeight())
                    {
                        if (tx != origX || ty != origY)
                        {
                            m_doc.PushUndoSnapshot();
                            m_doc.SetStartPos(m_draggingPlayerIndex, tx, ty);
                            m_ui.ShowToast(
                                std::string("Moved Player ") + std::to_string(m_draggingPlayerIndex + 1) + " start to ("
                                + std::to_string(tx) + ", " + std::to_string(ty) + ")");
                        }
                    }
                    m_isDraggingPlayer = false;
                    m_draggingPlayerIndex = -1;
                }
                m_isPainting = false;
                m_lastPaintedTileX = -1;
                m_lastPaintedTileY = -1;
            }
        }
        else if (event.type == SDL_EVENT_MOUSE_MOTION)
        {
            if (m_isPanning)
            {
                float dx = static_cast<float>(event.motion.x - m_lastMouseX);
                float dy = static_cast<float>(event.motion.y - m_lastMouseY);
                m_camera.Pan(-dx, -dy);
                m_lastMouseX = static_cast<int>(event.motion.x);
                m_lastMouseY = static_cast<int>(event.motion.y);
            }
            else if (m_isDraggingPlayer && !io.WantCaptureMouse)
            {
                int tx, ty;
                ScreenToTile(event.motion.x, event.motion.y, tx, ty);
                m_draggedTileX = tx;
                m_draggedTileY = ty;
            }
            else if (m_isPainting && !io.WantCaptureMouse)
            {
                int tx, ty;
                ScreenToTile(event.motion.x, event.motion.y, tx, ty);

                if (tx != m_lastPaintedTileX || ty != m_lastPaintedTileY)
                {
                    m_lastPaintedTileX = tx;
                    m_lastPaintedTileY = ty;
                    ApplyToolAt(tx, ty, true);
                }
            }
            else if (!io.WantCaptureMouse && (event.motion.state & SDL_BUTTON_RMASK))
            {
                float dx = event.motion.x - m_rightClickStartX;
                float dy = event.motion.y - m_rightClickStartY;
                if (dx * dx + dy * dy > 36.0f)
                {
                    m_rightClickMoved = true;
                    m_isRightDeleting = true;
                    int tx, ty;
                    ScreenToTile(event.motion.x, event.motion.y, tx, ty);
                    if (tx != m_lastDeletedTileX || ty != m_lastDeletedTileY)
                    {
                        EraseAtTile(tx, ty);
                    }
                }
            }
        }
        else if (event.type == SDL_EVENT_MOUSE_WHEEL)
        {
            if (!io.WantCaptureMouse)
            {
                float mouseX = 0.0f, mouseY = 0.0f;
                SDL_GetMouseState(&mouseX, &mouseY);
                float oldZoom = m_camera.GetZoom();
                float newZoom = oldZoom * (event.wheel.y > 0 ? 1.15f : 0.87f);
                newZoom = std::clamp(newZoom, 0.35f, 3.5f);
                m_camera.SetZoom(newZoom, static_cast<float>(mouseX), static_cast<float>(mouseY));
            }
        }
        else if (event.type == SDL_EVENT_KEY_DOWN)
        {
            if (!io.WantCaptureKeyboard)
            {
                SDL_Keymod mod = SDL_GetModState();
                bool ctrl = (mod & SDL_KMOD_CTRL) != 0;

                if (ctrl && event.key.key == SDLK_Z)
                {
                    m_doc.Undo();
                    m_ui.ShowToast(_("Undo action"));
                }
                else if (ctrl && event.key.key == SDLK_Y)
                {
                    m_doc.Redo();
                    m_ui.ShowToast(_("Redo action"));
                }
                else if (ctrl && event.key.key == SDLK_S)
                {
                    if (m_doc.GetFilePath().empty())
                    {
                        m_ui.OpenSaveAsDialog(m_doc);
                    }
                    else
                    {
                        m_doc.SaveMap(m_doc.GetFilePath());
                        m_ui.ShowToast(std::string(_("Map saved: ")) + m_doc.GetFilePath());
                    }
                }
                else if (ctrl && event.key.key == SDLK_O)
                {
                    m_ui.OpenOpenDialog();
                }
                else if (ctrl && event.key.key == SDLK_N)
                {
                    m_ui.OpenNewMapDialog();
                }
                else if (ctrl && event.key.key == SDLK_G)
                {
                    m_ui.OpenGeneratorDialog();
                }
                else if (event.key.key == SDLK_F)
                {
                    float mw = m_doc.GetWidth() * TILE_WIDTH * 0.5f;
                    float mh = m_doc.GetHeight() * TILE_HEIGHT * 0.5f;
                    m_camera.CenterOn(mw, mh);
                }
            }
        }
    }
}

// "<map file>[*] "<map title>" - <app name>": the open map's file name first, * while it has unsaved changes
void MapEditorApp::UpdateWindowTitle()
{
    const std::string &path = m_doc.GetFilePath();
    const std::string file = path.empty() ? m_doc.GetName() + ".json (" + _("unsaved") + ")"
                                          : std::filesystem::path(path).filename().string();
    const std::string mapTitle = m_doc.GetTitle();
    const std::string title = file + (m_doc.IsModified() ? "*" : "") + (mapTitle.empty() ? "" : " \"" + mapTitle + "\"")
                              + " - " + _("Open Art Of War Map Editor - [openaow-mapeditor]");
    if (title != m_windowTitle)
    {
        m_windowTitle = title;
        SDL_SetWindowTitle(m_window, title.c_str());
    }
}

void MapEditorApp::Update(float deltaTime)
{
    UpdateWindowTitle();
    m_globalAnimTimer += deltaTime;

    // Update hovered tile
    float mouseX = 0.0f, mouseY = 0.0f;
    SDL_GetMouseState(&mouseX, &mouseY);
    ImGuiIO &io = ImGui::GetIO();
    if (!io.WantCaptureMouse)
    {
        int tx, ty;
        ScreenToTile(mouseX, mouseY, tx, ty);
        m_ui.SetHoveredTile(tx, ty);
    }
    else
    {
        m_ui.SetHoveredTile(-1, -1);
    }

    // WASD Camera Panning
    if (!io.WantCaptureKeyboard)
    {
        const bool *state = SDL_GetKeyboardState(nullptr);
        float panSpeed = 600.0f * deltaTime;
        if (state[SDL_SCANCODE_W] || state[SDL_SCANCODE_UP])
            m_camera.Pan(0, -panSpeed);
        if (state[SDL_SCANCODE_S] || state[SDL_SCANCODE_DOWN])
            m_camera.Pan(0, panSpeed);
        if (state[SDL_SCANCODE_A] || state[SDL_SCANCODE_LEFT])
            m_camera.Pan(-panSpeed, 0);
        if (state[SDL_SCANCODE_D] || state[SDL_SCANCODE_RIGHT])
            m_camera.Pan(panSpeed, 0);
    }
}

void MapEditorApp::ScreenToTile(float screenX, float screenY, int &tileX, int &tileY) const
{
    float wx, wy;
    m_camera.ScreenToWorld(screenX, screenY, wx, wy);
    tileX = static_cast<int>(std::floor(wx / TILE_WIDTH));
    tileY = static_cast<int>(std::floor(wy / TILE_HEIGHT));
}

void MapEditorApp::EraseAtTile(int tx, int ty)
{
    if (tx < 0 || tx >= m_doc.GetWidth() || ty < 0 || ty >= m_doc.GetHeight())
        return;
    m_lastDeletedTileX = tx;
    m_lastDeletedTileY = ty;
    if (!m_doc.HasObjectsInBrush(tx, ty, m_ui.GetBrushSize()))
        return;
    if (!m_rightDeleteUndoPushed)
    {
        m_doc.PushUndoSnapshot();
        m_rightDeleteUndoPushed = true;
    }
    m_doc.RemoveObjectsBrush(tx, ty, m_ui.GetBrushSize());
}

void MapEditorApp::ApplyToolAt(int tx, int ty, bool isDrag)
{
    if (tx < 0 || tx >= m_doc.GetWidth() || ty < 0 || ty >= m_doc.GetHeight())
        return;

    EditorTool tool = m_ui.GetActiveTool();
    switch (tool)
    {
        case EditorTool::TileBrush:
            m_doc.SetTileBrush(tx, ty, static_cast<uint8_t>(m_ui.GetSelectedTile()), m_ui.GetBrushSize());
            break;
        case EditorTool::BucketFill:
            if (!isDrag)
            {
                m_doc.FloodFill(tx, ty, static_cast<uint8_t>(m_ui.GetSelectedTile()));
            }
            break;
        case EditorTool::PlaceDecor:
            if (!isDrag)
            {
                m_doc.AddDecoration(m_ui.GetSelectedDecorType(), tx, ty);
            }
            break;
        case EditorTool::PlaceWall:
            if (!m_doc.SetWallBrush(tx, ty, m_ui.GetSelectedWallFaction(), m_ui.GetBrushSize()))
            {
                m_ui.ShowToast("Cannot place a wall: the spot is occupied or blocked");
            }
            break;
        case EditorTool::PlaceBuilding:
            if (!isDrag)
            {
                const int type = m_ui.GetSelectedBuildingType();
                if (!m_doc.AddEntity(type, true, m_ui.GetSelectedBuildingFaction(), tx, ty))
                {
                    m_ui.ShowToast("Cannot place a building: the spot is occupied or blocked");
                }
            }
            break;
        case EditorTool::PlaceUnit:
            if (!isDrag)
            {
                const int type = m_ui.GetSelectedUnitArchetype();
                if (!m_doc.AddEntity(type, false, m_ui.GetSelectedUnitFaction(), tx, ty, m_ui.GetSelectedUnitFacing()))
                {
                    m_ui.ShowToast("Cannot place a unit: the tile is occupied or blocked");
                }
            }
            break;
        case EditorTool::SetPlayerStart:
            if (!isDrag)
            {
                const int slot = m_ui.GetSelectedPlayerSlot();
                if (slot < m_doc.GetPlayerCount())
                {
                    m_doc.SetStartPos(slot, tx, ty);
                    m_ui.ShowToast(
                        std::string(_("Player start point (HQ): ")) + "P" + std::to_string(slot + 1) + " ("
                        + std::to_string(tx) + ", " + std::to_string(ty) + ")");
                }
                else if (m_doc.GetPlayerCount() < 8)
                {
                    m_doc.AddStartPos(tx, ty);
                    m_ui.ShowToast(
                        std::string(_("Start position added: ")) + "P" + std::to_string(m_doc.GetPlayerCount()) + " ("
                        + std::to_string(tx) + ", " + std::to_string(ty) + ")");
                }
            }
            break;
        case EditorTool::SetEnemyStart:
            m_doc.setEnemyStart(tx, ty);
            m_ui.ShowToast(
                std::string(_("Enemy start point (HQ): ")) + "(" + std::to_string(tx) + ", " + std::to_string(ty)
                + ")");
            break;
        case EditorTool::AddStartPosition:
            if (!isDrag)
            {
                const auto result = m_doc.ToggleExtraStart(tx, ty);
                const std::string where = "(" + std::to_string(tx) + ", " + std::to_string(ty) + ")";
                if (result == MapDocument::StartToggle::Full)
                {
                    m_ui.ShowToast(_("A map holds 8 players at most: no more start positions"));
                }
                else
                {
                    const size_t players = 2 + m_doc.GetExtraStarts().size();
                    m_ui.ShowToast(
                        std::string(
                            result == MapDocument::StartToggle::Added ? _("Start position added: ")
                                                                      : _("Start position removed: "))
                        + where + " - " + std::to_string(players) + " " + _("players"));
                }
            }
            break;
        case EditorTool::PassabilityBrush:
            m_doc.SetPassable(tx, ty, !m_doc.GetTileMap().IsPassable(tx, ty));
            break;
        case EditorTool::Eraser:
            m_doc.RemoveObjectsBrush(tx, ty, m_ui.GetBrushSize());
            break;
        case EditorTool::Eyedropper:
            if (!isDrag)
            {
                uint8_t sampled = m_doc.GetTileMap().GetBaseTile(tx, ty);
                m_ui.SelectTileFromEyedropper(sampled);
                m_ui.ShowToast(std::string(_("Eyedropper: selected tile #")) + std::to_string(sampled));
            }
            break;
        case EditorTool::Select:
            if (!isDrag)
            {
                m_doc.SelectEntityAt(tx, ty, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
            }
            break;
    }
}

void MapEditorApp::render()
{
    SDL_SetRenderDrawColor(m_sdlRenderer, 15, 18, 22, 255);
    SDL_RenderClear(m_sdlRenderer);

    int globalAnimTick = static_cast<int>(m_globalAnimTimer * 5.0f);
    int mapW = m_doc.GetWidth();
    int mapH = m_doc.GetHeight();
    int tilesetId = m_doc.GetTilesetId();

    float startWorldX, startWorldY, endWorldX, endWorldY;
    m_camera.ScreenToWorld(0, 0, startWorldX, startWorldY);
    m_camera.ScreenToWorld(m_camera.GetViewportWidth(), m_camera.GetViewportHeight(), endWorldX, endWorldY);
    int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, mapW - 1);
    int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, mapW);
    int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, mapH - 1);
    int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, mapH);

    // 1. Render Base Terrain Tiles
    for (int ty = startTileY; ty < endTileY; ++ty)
    {
        for (int tx = startTileX; tx < endTileX; ++tx)
        {
            uint8_t tileIdx = m_doc.GetTileMap().GetBaseTile(tx, ty);
            m_renderer->DrawTileWorld(tilesetId, tileIdx, tx * TILE_WIDTH, ty * TILE_HEIGHT, m_camera, globalAnimTick);

            // Optional Grid lines
            if (m_ui.IsShowGrid())
            {
                m_renderer->DrawRectWorld(
                    tx * TILE_WIDTH, ty * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera, {255, 255, 255, 30});
            }
        }
    }

    // 2. Render Wall Rows (Depth-sorted)
    for (int ty = startTileY; ty < endTileY; ++ty)
    {
        m_doc.GetTileMap().RenderWallRow(ty, *m_renderer, m_camera, false, globalAnimTick, startTileX, endTileX);
    }

    // 3. Render Map Decorations
    for (const auto &dec : m_doc.GetTileMap().GetDecorations())
    {
        if (dec.x >= startTileX - 3 && dec.x <= endTileX + 3 && dec.y >= startTileY - 3 && dec.y <= endTileY + 3)
        {
            m_renderer->DrawDecorWorld(
                tilesetId, dec.type, dec.x * TILE_WIDTH, dec.y * TILE_HEIGHT, m_camera, globalAnimTick);
        }
    }

    // 4. Render Placed Buildings
    for (const auto &ent : m_doc.GetEntities())
    {
        if (!ent.isBuilding)
            continue;
        float bx = ent.tileX * TILE_WIDTH;
        float by = ent.tileY * TILE_HEIGHT;
        m_renderer->DrawBuildingWorld(ent.type, bx, by, 0, m_camera, ent.faction, globalAnimTick, 1.0f, false);
        m_renderer->DrawBuildingWorld(ent.type, bx, by, 1, m_camera, ent.faction, globalAnimTick, 1.0f, false);

        // Selection bracket / footprint
        const SDL_Color footprintColor
            = ent.selected
                  ? SDL_Color {40, 255, 90, 255}
                  : (ent.faction == Faction::Resistance ? SDL_Color {255, 50, 50, 200} : SDL_Color {50, 100, 255, 200});
        m_renderer->DrawRectWorld(
            bx, by, ent.buildWidth * TILE_WIDTH, ent.buildHeight * TILE_HEIGHT, m_camera, footprintColor);
    }

    // 5. Render Placed Units
    for (const auto &ent : m_doc.GetEntities())
    {
        if (ent.isBuilding)
            continue;
        float ux = ent.tileX * TILE_WIDTH;
        float uy = ent.tileY * TILE_HEIGHT;
        m_renderer->DrawUnitWorld(ent.type, 0, ent.facing, 0, ux, uy, m_camera, ent.faction);
        const SDL_Color footprintColor = ent.selected
                                             ? SDL_Color {40, 255, 90, 255}
                                             : (ent.faction == Faction::Resistance ? SDL_Color {255, 100, 100, 160}
                                                                                   : SDL_Color {100, 150, 255, 160});
        m_renderer->DrawRectWorld(
            ent.tileX * TILE_WIDTH, ent.tileY * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera, footprintColor);
    }

    // 6. Starting Base Position Markers (Flags)
    static const SDL_Color kPlayerMarkerColors[8] = {
        {235, 40, 40, 150},  // P1
        {40, 110, 240, 150}, // P2
        {40, 200, 80, 150},  // P3
        {240, 200, 40, 150}, // P4
        {180, 60, 220, 150}, // P5
        {40, 210, 220, 150}, // P6
        {240, 130, 40, 150}, // P7
        {230, 110, 170, 150} // P8
    };

    const int totalPlayers = m_doc.GetPlayerCount();
    for (int p = 0; p < totalPlayers; ++p)
    {
        const auto [sx, sy] = m_doc.GetStartPos(p);
        const float wx = sx * TILE_WIDTH;
        const float wy = sy * TILE_HEIGHT;
        m_renderer->FillRectWorld(wx, wy, TILE_WIDTH, TILE_HEIGHT, m_camera, kPlayerMarkerColors[p]);
        const SDL_Color outline
            = (p == m_selectedPlayerIndex) ? SDL_Color {255, 255, 40, 255} : SDL_Color {255, 255, 255, 220};
        m_renderer->DrawRectWorld(wx, wy, TILE_WIDTH, TILE_HEIGHT, m_camera, outline);
    }

    // Ghost preview if dragging a player marker
    if (m_isDraggingPlayer && m_draggingPlayerIndex >= 0 && m_draggingPlayerIndex < 8)
    {
        const float dwx = m_draggedTileX * TILE_WIDTH;
        const float dwy = m_draggedTileY * TILE_HEIGHT;
        SDL_Color dragColor = kPlayerMarkerColors[m_draggingPlayerIndex];
        dragColor.a = 180;
        m_renderer->FillRectWorld(dwx, dwy, TILE_WIDTH, TILE_HEIGHT, m_camera, dragColor);
        m_renderer->DrawRectWorld(dwx, dwy, TILE_WIDTH, TILE_HEIGHT, m_camera, {255, 255, 100, 255});
    }

    // 7. Collision Passability Overlay (if toggled)
    if (m_ui.IsShowPassability())
    {
        for (int ty = startTileY; ty < endTileY; ++ty)
        {
            for (int tx = startTileX; tx < endTileX; ++tx)
            {
                if (!m_doc.GetTileMap().IsPassable(tx, ty))
                {
                    m_renderer->FillRectWorld(
                        tx * TILE_WIDTH + 2,
                        ty * TILE_HEIGHT + 2,
                        TILE_WIDTH - 4,
                        TILE_HEIGHT - 4,
                        m_camera,
                        {220, 30, 30, 110});
                }
            }
        }
    }

    // 8. Tool Ghost Hover Preview
    float mouseX = 0.0f, mouseY = 0.0f;
    SDL_GetMouseState(&mouseX, &mouseY);
    ImGuiIO &io = ImGui::GetIO();
    if (!io.WantCaptureMouse)
    {
        int htx, hty;
        ScreenToTile(mouseX, mouseY, htx, hty);

        if (htx >= 0 && htx < mapW && hty >= 0 && hty < mapH)
        {
            EditorTool t = m_ui.GetActiveTool();
            if (t == EditorTool::TileBrush)
            {
                int bs = m_ui.GetBrushSize();
                int start = -(bs - 1) / 2;
                m_renderer->DrawRectWorld(
                    (htx + start) * TILE_WIDTH,
                    (hty + start) * TILE_HEIGHT,
                    bs * TILE_WIDTH,
                    bs * TILE_HEIGHT,
                    m_camera,
                    {0, 255, 120, 220});
            }
            else if (t == EditorTool::PlaceWall)
            {
                const int brushSize = m_ui.GetBrushSize();
                const int start = -(brushSize - 1) / 2;
                const bool canPlace = m_doc.CanPlaceWallBrush(htx, hty, m_ui.GetSelectedWallFaction(), brushSize);
                const SDL_Color fillColor = canPlace ? SDL_Color {255, 220, 0, 120} : SDL_Color {255, 40, 40, 110};
                const SDL_Color borderColor = canPlace ? SDL_Color {255, 255, 255, 200} : SDL_Color {255, 40, 40, 255};
                const int drawX = htx + start;
                const int drawY = hty + start;
                m_renderer->FillRectWorld(
                    drawX * TILE_WIDTH,
                    drawY * TILE_HEIGHT,
                    brushSize * TILE_WIDTH,
                    brushSize * TILE_HEIGHT,
                    m_camera,
                    fillColor);
                m_renderer->DrawRectWorld(
                    drawX * TILE_WIDTH,
                    drawY * TILE_HEIGHT,
                    brushSize * TILE_WIDTH,
                    brushSize * TILE_HEIGHT,
                    m_camera,
                    borderColor);
            }
            else if (t == EditorTool::PlaceBuilding)
            {
                int bw = 1;
                int bh = 1;
                const BuildingDef *buildingDef = m_texManager->GetBuildingDef(m_ui.GetSelectedBuildingType());
                if (buildingDef)
                {
                    bw = buildingDef->width;
                    bh = buildingDef->height;
                }
                const bool canPlace = m_doc.CanPlaceEntity(m_ui.GetSelectedBuildingType(), true, htx, hty);
                const SDL_Color previewColor = canPlace ? SDL_Color {50, 200, 255, 90} : SDL_Color {255, 40, 40, 110};
                const SDL_Color borderColor = canPlace ? SDL_Color {255, 255, 255, 200} : SDL_Color {255, 40, 40, 255};
                m_renderer->FillRectWorld(
                    htx * TILE_WIDTH, hty * TILE_HEIGHT, bw * TILE_WIDTH, bh * TILE_HEIGHT, m_camera, previewColor);
                m_renderer->DrawRectWorld(
                    htx * TILE_WIDTH, hty * TILE_HEIGHT, bw * TILE_WIDTH, bh * TILE_HEIGHT, m_camera, borderColor);
            }
            else if (t == EditorTool::PlaceUnit)
            {
                const bool canPlace = m_doc.CanPlaceEntity(m_ui.GetSelectedUnitArchetype(), false, htx, hty);
                const SDL_Color previewColor = canPlace ? SDL_Color {100, 255, 100, 120} : SDL_Color {255, 40, 40, 110};
                const SDL_Color borderColor = canPlace ? SDL_Color {255, 255, 255, 200} : SDL_Color {255, 40, 40, 255};
                m_renderer->FillRectWorld(
                    htx * TILE_WIDTH, hty * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera, previewColor);
                m_renderer->DrawRectWorld(
                    htx * TILE_WIDTH, hty * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera, borderColor);
            }
            else if (t == EditorTool::Eraser)
            {
                m_renderer->FillRectWorld(
                    htx * TILE_WIDTH, hty * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera, {255, 0, 0, 140});
            }
            else
            {
                m_renderer->DrawRectWorld(
                    htx * TILE_WIDTH, hty * TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT, m_camera, {255, 255, 255, 160});
            }
        }
    }

    // 9. ImGui UI Overlay
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    m_ui.render(m_doc, m_camera, *m_texManager, *m_renderer);
    if (m_ui.ShouldClose())
    {
        m_running = false;
    }

    // Draw on-screen badge indicators (P1..P8)
    ImDrawList *fgDraw = ImGui::GetForegroundDrawList();
    for (int p = 0; p < totalPlayers; ++p)
    {
        const auto [sx, sy] = m_doc.GetStartPos(p);
        float scX = 0.0f, scY = 0.0f;
        m_camera.WorldToScreen(sx * TILE_WIDTH + 2, sy * TILE_HEIGHT + 2, scX, scY);
        if (scX >= -50.0f && scX <= m_windowW + 50.0f && scY >= -50.0f && scY <= m_windowH + 50.0f)
        {
            char badge[16];
            snprintf(badge, sizeof(badge), "P%d", p + 1);
            fgDraw->AddRectFilled(ImVec2(scX, scY), ImVec2(scX + 20.0f, scY + 15.0f), IM_COL32(10, 10, 15, 210), 2.0f);
            fgDraw->AddText(ImVec2(scX + 3.0f, scY), IM_COL32(255, 255, 255, 240), badge);
        }
    }
    if (m_isDraggingPlayer && m_draggingPlayerIndex >= 0)
    {
        float scX = 0.0f, scY = 0.0f;
        m_camera.WorldToScreen(m_draggedTileX * TILE_WIDTH + 2, m_draggedTileY * TILE_HEIGHT + 2, scX, scY);
        char badge[32];
        snprintf(badge, sizeof(badge), "P%d -> (%d, %d)", m_draggingPlayerIndex + 1, m_draggedTileX, m_draggedTileY);
        fgDraw->AddRectFilled(
            ImVec2(scX, scY - 18.0f), ImVec2(scX + 115.0f, scY - 2.0f), IM_COL32(20, 20, 25, 220), 3.0f);
        fgDraw->AddText(ImVec2(scX + 4.0f, scY - 18.0f), IM_COL32(255, 255, 100, 255), badge);
    }

    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_sdlRenderer);

    SDL_RenderPresent(m_sdlRenderer);
}

} // namespace OpenArtOfWar
