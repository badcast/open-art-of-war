#include "EditorUI.hpp"
#include "core/LoaderResources.hpp"
#include "MapGenerator.hpp"
#include "game/TileCollision.hpp"
#include "core/AssetStore.hpp"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <limits>

namespace OpenArtOfWar
{

namespace fs = std::filesystem;

namespace
{

ImTextureID SpriteTextureId(SDL_Texture *texture)
{
    return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture));
}

void DrawSpriteFrame(
    ImDrawList *drawList,
    TextureManager &textureManager,
    int spriteId,
    float x,
    float y,
    float scale,
    Faction faction,
    int depth = 0)
{
    if (depth > 6)
        return;
    const SpriteFrame *frame = textureManager.GetSpriteFrame(spriteId);
    if (!frame || frame->width <= 0 || frame->height <= 0)
        return;

    int page = frame->page;
    if (page == 8 && faction == Faction::Resistance)
        page = 9;
    SDL_Texture *sheet = textureManager.GetSpriteSheet(page);
    if (!sheet)
        return;

    static std::unordered_map<SDL_Texture *, ImVec2> sheetSizes;
    auto size = sheetSizes.find(sheet);
    if (size == sheetSizes.end())
    {
        float width = 0.0f;
        float height = 0.0f;
        SDL_GetTextureSize(sheet, &width, &height);
        size = sheetSizes.emplace(sheet, ImVec2(static_cast<float>(width), static_cast<float>(height))).first;
    }
    if (size->second.x <= 0.0f || size->second.y <= 0.0f)
        return;

    const float sheetScale = textureManager.GetSheetScale(page);
    const ImVec2 uv0(frame->x * sheetScale / size->second.x, frame->y * sheetScale / size->second.y);
    const ImVec2 uv1(
        (frame->x + frame->width) * sheetScale / size->second.x,
        (frame->y + frame->height) * sheetScale / size->second.y);
    const ImVec2 min(x, y);
    const ImVec2 max(x + frame->width * scale, y + frame->height * scale);
    drawList->AddImage(SpriteTextureId(sheet), min, max, uv0, uv1);
}

void DrawCompositeSprite(
    ImDrawList *drawList,
    TextureManager &textureManager,
    int compositeId,
    float x,
    float y,
    float scale,
    Faction faction,
    int depth = 0)
{
    if (depth > 6)
        return;
    const IndexRange range = textureManager.GetCompositeRange(compositeId);
    if (range.end < range.start)
        return;

    for (int index = range.start; index <= range.end; ++index)
    {
        const CompositePart *part = textureManager.GetCompositePart(index);
        if (!part)
            continue;
        const float childX = x + part->ox * scale;
        const float childY = y + part->oy * scale;
        if (part->isComp == 0)
        {
            DrawSpriteFrame(drawList, textureManager, part->child, childX, childY, scale, faction);
        }
        else
        {
            DrawCompositeSprite(drawList, textureManager, part->child, childX, childY, scale, faction, depth + 1);
        }
    }
}

struct SpriteBounds
{
    float left {std::numeric_limits<float>::max()};
    float top {std::numeric_limits<float>::max()};
    float right {std::numeric_limits<float>::lowest()};
    float bottom {std::numeric_limits<float>::lowest()};
    bool valid {false};

    void Include(float x, float y, float width, float height)
    {
        if (width <= 0.0f || height <= 0.0f)
            return;
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x + width);
        bottom = std::max(bottom, y + height);
        valid = true;
    }
};

void IncludeSpriteBounds(TextureManager &textureManager, int spriteId, float x, float y, SpriteBounds &bounds)
{
    const SpriteFrame *frame = textureManager.GetSpriteFrame(spriteId);
    if (frame)
        bounds.Include(x, y, static_cast<float>(frame->width), static_cast<float>(frame->height));
}

void IncludeCompositeBounds(
    TextureManager &textureManager, int compositeId, float x, float y, SpriteBounds &bounds, int depth = 0)
{
    if (depth > 6)
        return;
    const IndexRange range = textureManager.GetCompositeRange(compositeId);
    for (int index = range.start; index <= range.end; ++index)
    {
        const CompositePart *part = textureManager.GetCompositePart(index);
        if (!part)
            continue;
        const float partX = x + part->ox;
        const float partY = y + part->oy;
        if (part->isComp == 0)
            IncludeSpriteBounds(textureManager, part->child, partX, partY, bounds);
        else
            IncludeCompositeBounds(textureManager, part->child, partX, partY, bounds, depth + 1);
    }
}

struct ThumbnailTransform
{
    ImVec2 origin;
    float scale {1.0f};
};

ThumbnailTransform FitThumbnail(const SpriteBounds &bounds, const ImVec2 &itemMin, const ImVec2 &itemMax)
{
    constexpr float slotWidth = 48.0f;
    const ImVec2 slotMin(itemMin.x + 4.0f, itemMin.y + 4.0f);
    const ImVec2 slotMax(itemMin.x + slotWidth, itemMax.y - 4.0f);
    const float boundsWidth = std::max(1.0f, bounds.right - bounds.left);
    const float boundsHeight = std::max(1.0f, bounds.bottom - bounds.top);
    const float availableWidth = std::max(1.0f, slotMax.x - slotMin.x - 4.0f);
    const float availableHeight = std::max(1.0f, slotMax.y - slotMin.y - 4.0f);
    const float scale = bounds.valid ? std::min(availableWidth / boundsWidth, availableHeight / boundsHeight) : 1.0f;
    const float x = slotMin.x + (availableWidth - boundsWidth * scale) * 0.5f - bounds.left * scale;
    const float y = slotMin.y + (availableHeight - boundsHeight * scale) * 0.5f - bounds.top * scale;
    return {ImVec2(x, y), scale};
}

void DrawThumbnailFrame(ImDrawList *drawList, const ImVec2 &itemMin, const ImVec2 &itemMax)
{
    drawList->AddRect(
        ImVec2(itemMin.x + 4.0f, itemMin.y + 4.0f),
        ImVec2(itemMin.x + 48.0f, itemMax.y - 4.0f),
        IM_COL32(255, 255, 255, 55));
}

void DrawDecorThumbnail(
    ImDrawList *drawList,
    TextureManager &textureManager,
    int tilesetId,
    int decorType,
    const ImVec2 &min,
    const ImVec2 &max)
{
    const int compositeId = textureManager.GetDecorCompositeId(tilesetId, decorType);
    SpriteBounds bounds;
    IncludeCompositeBounds(textureManager, compositeId, 0.0f, 0.0f, bounds);
    const ThumbnailTransform transform = FitThumbnail(bounds, min, max);
    drawList->PushClipRect(ImVec2(min.x + 4.0f, min.y + 4.0f), ImVec2(min.x + 48.0f, max.y - 4.0f), true);
    DrawCompositeSprite(
        drawList,
        textureManager,
        compositeId,
        transform.origin.x,
        transform.origin.y,
        transform.scale,
        Faction::Resistance);
    drawList->PopClipRect();
    DrawThumbnailFrame(drawList, min, max);
}

void DrawBuildingThumbnail(
    ImDrawList *drawList,
    TextureManager &textureManager,
    int buildingType,
    Faction faction,
    const ImVec2 &min,
    const ImVec2 &max)
{
    const BuildingDef *def = textureManager.GetBuildingDef(buildingType);
    if (!def || def->end < def->start)
        return;
    SpriteBounds bounds;
    for (int index = def->start; index <= def->end; ++index)
    {
        const BuildingPart *part = textureManager.GetBuildingPart(index);
        if (!part || part->state != 0)
            continue;
        if (part->isComp == 0)
            IncludeSpriteBounds(textureManager, part->child, part->ox, part->oy, bounds);
        else if (part->isComp == 1)
            IncludeCompositeBounds(textureManager, part->child, part->ox, part->oy, bounds);
        else
        {
            const IndexRange range = textureManager.GetAnimRange(part->child);
            if (range.end < range.start)
                continue;
            const AnimFrame *frame = textureManager.GetAnimFrame(range.start);
            if (!frame)
                continue;
            const float x = part->ox + frame->ox;
            const float y = part->oy + frame->oy;
            if (frame->isComp == 0)
                IncludeSpriteBounds(textureManager, frame->child, x, y, bounds);
            else
                IncludeCompositeBounds(textureManager, frame->child, x, y, bounds);
        }
    }
    const ThumbnailTransform transform = FitThumbnail(bounds, min, max);
    drawList->PushClipRect(ImVec2(min.x + 4.0f, min.y + 4.0f), ImVec2(min.x + 48.0f, max.y - 4.0f), true);
    for (int index = def->start; index <= def->end; ++index)
    {
        const BuildingPart *part = textureManager.GetBuildingPart(index);
        if (!part || part->state != 0)
            continue;
        const float x = transform.origin.x + part->ox * transform.scale;
        const float y = transform.origin.y + part->oy * transform.scale;
        if (part->isComp == 0)
        {
            DrawSpriteFrame(drawList, textureManager, part->child, x, y, transform.scale, faction);
        }
        else if (part->isComp == 1)
        {
            DrawCompositeSprite(drawList, textureManager, part->child, x, y, transform.scale, faction);
        }
        else
        {
            const IndexRange range = textureManager.GetAnimRange(part->child);
            if (range.end >= range.start)
            {
                const AnimFrame *frame = textureManager.GetAnimFrame(range.start);
                if (frame)
                {
                    const float frameX = x + frame->ox * transform.scale;
                    const float frameY = y + frame->oy * transform.scale;
                    if (frame->isComp == 0)
                    {
                        DrawSpriteFrame(
                            drawList, textureManager, frame->child, frameX, frameY, transform.scale, faction);
                    }
                    else
                    {
                        DrawCompositeSprite(
                            drawList, textureManager, frame->child, frameX, frameY, transform.scale, faction);
                    }
                }
            }
        }
    }
    drawList->PopClipRect();
    DrawThumbnailFrame(drawList, min, max);
}

void DrawUnitThumbnail(
    ImDrawList *drawList,
    TextureManager &textureManager,
    int unitType,
    Faction faction,
    const ImVec2 &min,
    const ImVec2 &max)
{
    const int sequence = textureManager.GetUnitBaseSeq(unitType, static_cast<int>(ActionState::Idle));
    const IndexRange sequenceRange = textureManager.GetUnitSequenceRange(sequence);
    if (sequenceRange.end < sequenceRange.start)
        return;
    const UnitSequenceStep *step = textureManager.GetUnitSequenceStep(sequenceRange.start);
    if (!step)
        return;
    const IndexRange bodyRange = textureManager.GetUnitBodyRange(step->bodyId);
    SpriteBounds bounds;
    for (int index = bodyRange.start; index <= bodyRange.end; ++index)
    {
        const UnitLayer *layer = textureManager.GetUnitLayer(index);
        if (!layer || layer->flag > 2)
            continue;
        const float x = step->ox + layer->ox;
        const float y = step->oy + layer->oy;
        if (layer->isComp == 0)
            IncludeSpriteBounds(textureManager, layer->child, x, y, bounds);
        else
            IncludeCompositeBounds(textureManager, layer->child, x, y, bounds);
    }
    const ThumbnailTransform transform = FitThumbnail(bounds, min, max);
    drawList->PushClipRect(ImVec2(min.x + 4.0f, min.y + 4.0f), ImVec2(min.x + 48.0f, max.y - 4.0f), true);
    for (int index = bodyRange.start; index <= bodyRange.end; ++index)
    {
        const UnitLayer *layer = textureManager.GetUnitLayer(index);
        if (!layer || layer->flag > 2)
            continue;
        const float x = transform.origin.x + (step->ox + layer->ox) * transform.scale;
        const float y = transform.origin.y + (step->oy + layer->oy) * transform.scale;
        if (layer->isComp == 0)
        {
            DrawSpriteFrame(drawList, textureManager, layer->child, x, y, transform.scale, faction);
        }
        else
        {
            DrawCompositeSprite(drawList, textureManager, layer->child, x, y, transform.scale, faction);
        }
    }
    drawList->PopClipRect();
    DrawThumbnailFrame(drawList, min, max);
}

} // namespace

EditorUI::EditorUI()
{
    Init();
}

void EditorUI::Init()
{
    m_genParams.width = 60;
    m_genParams.height = 40;
    m_genParams.tilesetId = 0;
    m_genParams.symmetry = GeneratorSymmetry::Rotational180;
    m_genParams.randomSeed = true;

    ScanAvailableMaps();
}

void EditorUI::ShowToast(const std::string &message, float duration)
{
    m_toastMessage = message;
    m_toastTimer = duration;
}

void EditorUI::OpenSaveAsDialog(const MapDocument &doc)
{
    std::string base = doc.GetName();
    if (base.empty() || base == "untitled" || base == "new_map")
    {
        base = "custom_map_01";
    }
    std::snprintf(m_saveMapName, sizeof(m_saveMapName), "%s", base.c_str());
    std::snprintf(m_saveFilename, sizeof(m_saveFilename), "%s.json", base.c_str());
    m_openSaveModalReq = true;
}

void EditorUI::OpenOpenDialog()
{
    ScanAvailableMaps();
    m_openOpenModalReq = true;
}

void EditorUI::OpenNewMapDialog()
{
    m_openNewModalReq = true;
}

void EditorUI::OpenGeneratorDialog()
{
    m_openGenModalReq = true;
}

void EditorUI::RequestClose(const MapDocument &doc)
{
    if (!doc.IsModified())
    {
        m_shouldClose = true;
        return;
    }
    m_openCloseConfirmationReq = true;
}

void EditorUI::ScanAvailableMaps()
{
    m_availableMaps.clear();
    const std::string searchDirs[] = {OpenArtOfWar::LoaderResources::MapsDir()};

    for (const auto &sdir : searchDirs)
    {
        for (const std::string &mapPath : OpenArtOfWar::Assets::List(sdir, ".json"))
        {
            {
                MapFileInfo info;
                info.filename = fs::path(mapPath).filename().string();
                info.fullPath = mapPath;

                // Quick metadata peek
                std::string text;
                if (OpenArtOfWar::Assets::ReadText(info.fullPath, text))
                {
                    info.fileSize = text.size();
                    try
                    {
                        nlohmann::json j = nlohmann::json::parse(text);
                        info.width = j.value("width", 0);
                        info.height = j.value("height", 0);
                        info.tilesetId = j.value("tilesetId", 0);
                    }
                    catch (...)
                    {}
                }

                // Avoid duplicates
                bool exists = false;
                for (const auto &m : m_availableMaps)
                {
                    if (m.filename == info.filename)
                    {
                        exists = true;
                        break;
                    }
                }
                if (!exists)
                {
                    m_availableMaps.push_back(info);
                }
            }
        }
    }

    std::sort(m_availableMaps.begin(), m_availableMaps.end(), [](const MapFileInfo &a, const MapFileInfo &b) {
        return a.filename < b.filename;
    });
}

void EditorUI::render(MapDocument &doc, Camera &camera, TextureManager &texManager, Renderer & /*renderer*/)
{
    // Top menu bar
    RenderMenuBar(doc, camera);

    // Left toolbar
    RenderToolbar(doc, texManager);

    // Right palette panel
    RenderPalettePanel(doc, texManager, camera);

    // Minimap window
    if (m_showMiniMap)
    {
        RenderMiniMapWindow(doc, camera);
    }

    // Context Menu
    RenderContextMenu(doc, camera);

    // Bottom status bar
    RenderStatusBar(doc, camera);

    // Modals
    RenderSaveAsModal(doc);
    RenderOpenModal(doc);
    RenderNewMapModal(doc);
    RenderGeneratorModal(doc);
    RenderCloseConfirmation(doc);
    RenderGeneratorConfirmation(doc);

    // Toast
    RenderToastNotification();
}

void EditorUI::RenderMenuBar(MapDocument &doc, Camera &camera)
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New map...", "Ctrl+N"))
            {
                OpenNewMapDialog();
            }
            if (ImGui::MenuItem("Open map...", "Ctrl+O"))
            {
                OpenOpenDialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S"))
            {
                if (doc.GetFilePath().empty())
                {
                    OpenSaveAsDialog(doc);
                }
                else
                {
                    doc.SaveMap(doc.GetFilePath());
                    ShowToast("Map saved: " + doc.GetFilePath());
                }
            }
            if (ImGui::MenuItem("Save map as...", "Ctrl+Shift+S"))
            {
                OpenSaveAsDialog(doc);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
            {
                SDL_Event quitEv;
                quitEv.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quitEv);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, doc.CanUndo()))
            {
                doc.Undo();
                ShowToast("Action undone");
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, doc.CanRedo()))
            {
                doc.Redo();
                ShowToast("Action redone");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Fill map with the selected tile"))
            {
                doc.PushUndoSnapshot();
                doc.FillAll(static_cast<std::uint8_t>(m_selectedTile));
                ShowToast("Whole map filled with tile #" + std::to_string(m_selectedTile));
            }
            if (ImGui::MenuItem("Recompute passability"))
            {
                doc.AutoRecalculatePassability();
                ShowToast("Passability grid updated");
            }
            if (ImGui::MenuItem("Clear all"))
            {
                doc.PushUndoSnapshot();
                doc.ClearAll();
                ShowToast("Map cleared");
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Generator"))
        {
            if (ImGui::MenuItem("Map generator...", "Ctrl+G"))
            {
                OpenGeneratorDialog();
            }
            if (ImGui::MenuItem("Generate map (1 click, modelled on the original)"))
            {
                RequestGenerate(doc, false);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Tile grid (G)", nullptr, &m_showGrid);
            ImGui::MenuItem("Collisions / Passability (C)", nullptr, &m_showPassability);
            ImGui::MenuItem("Minimap (M)", nullptr, &m_showMiniMap);
            ImGui::Separator();
            if (ImGui::MenuItem("Center camera (F)"))
            {
                float mw = doc.GetWidth() * TILE_WIDTH * 0.5f;
                float mh = doc.GetHeight() * TILE_HEIGHT * 0.5f;
                camera.CenterOn(mw, mh);
            }
            if (ImGui::MenuItem("Zoom 100% (1:1)"))
            {
                camera.SetZoom(1.0f);
            }
            ImGui::EndMenu();
        }

        // Center the primary actions in the menu bar.
        constexpr float actionGroupWidth = 390.0f;
        const float centeredActionX = (ImGui::GetWindowWidth() - actionGroupWidth) * 0.5f;
        const float actionGroupX = std::max(ImGui::GetCursorPosX() + 12.0f, centeredActionX);
        ImGui::SameLine(actionGroupX);
        if (ImGui::Button("Generate map"))
        {
            RequestGenerate(doc, false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save to assets/maps/"))
        {
            OpenSaveAsDialog(doc);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open"))
        {
            OpenOpenDialog();
        }

        ImGui::EndMainMenuBar();
    }
}

void EditorUI::RenderToolbar(MapDocument &doc, TextureManager &texManager)
{
    const ImVec2 viewport = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(8.0f, 30.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(150.0f, std::max(240.0f, viewport.y - 66.0f)), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("Tools##Toolbar", nullptr, flags))
    {
        ImGui::Text("Selected tile: #%d", m_selectedTile);
        const ImVec2 previewPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("selected-tile-preview", ImVec2(96.0f, 42.0f));
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 previewMin = ImGui::GetItemRectMin();
        const ImVec2 previewMax = ImGui::GetItemRectMax();
        drawList->PushClipRect(previewMin, previewMax, true);
        const int tileComposite = texManager.GetTileCompositeId(doc.GetTilesetId(), m_selectedTile);
        DrawCompositeSprite(
            drawList, texManager, tileComposite, previewPos.x + 20.0f, previewPos.y + 8.0f, 0.85f, Faction::Resistance);
        drawList->PopClipRect();
        ImGui::Separator();

        auto toolBtn = [&](const char *label, EditorTool tool, const char *tooltip) {
            bool active = (m_activeTool == tool);
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.3f, 1.0f));
            }
            if (ImGui::Button(label, ImVec2(96, 32)))
            {
                m_activeTool = tool;
                switch (tool)
                {
                    case EditorTool::TileBrush:
                    case EditorTool::BucketFill:
                    case EditorTool::Eyedropper:
                    case EditorTool::PassabilityBrush:
                        m_paletteTabToSelect = 0;
                        break;
                    case EditorTool::PlaceDecor:
                        m_paletteTabToSelect = 1;
                        break;
                    case EditorTool::PlaceWall:
                        m_paletteTabToSelect = 2;
                        break;
                    case EditorTool::PlaceBuilding:
                        m_paletteTabToSelect = 3;
                        break;
                    case EditorTool::PlaceUnit:
                    case EditorTool::Select:
                        m_paletteTabToSelect = 4;
                        break;
                    case EditorTool::SetPlayerStart:
                    case EditorTool::SetEnemyStart:
                    case EditorTool::AddStartPosition:
                        m_paletteTabToSelect = 5;
                        break;
                    case EditorTool::Eraser:
                        break;
                }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", tooltip);
            }
            if (active)
            {
                ImGui::PopStyleColor();
            }
        };

        toolBtn("Tile brush", EditorTool::TileBrush, "Tile brush (Pencil / Brush)");
        toolBtn("Fill", EditorTool::BucketFill, "Fill an area with a tile (Bucket Fill)");
        toolBtn("Decorations", EditorTool::PlaceDecor, "Place decorations (trees, rocks)");
        toolBtn("Walls", EditorTool::PlaceWall, "Place defensive walls");
        toolBtn("Buildings", EditorTool::PlaceBuilding, "Place base buildings");
        toolBtn("Units", EditorTool::PlaceUnit, "Place starting units");
        toolBtn(
            "Player starts",
            EditorTool::PlayerStarts,
            "Manage and place player starting positions (HQ) for up to 8 players");
        toolBtn("Passability", EditorTool::PassabilityBrush, "Passability brush (block/pass)");
        toolBtn("Eraser", EditorTool::Eraser, "Eraser (removes an object/wall/decoration)");
        toolBtn("Eyedropper", EditorTool::Eyedropper, "Eyedropper (pick the tile under the cursor)");
        toolBtn("Select", EditorTool::Select, "Select an object; Shift adds or removes from the selection");

        ImGui::Separator();
        ImGui::Text("Brush size:");
        const char *brushSizes[] = {"1x1", "2x2", "3x3"};
        int brushIndex = std::clamp(m_brushSize, 1, 3) - 1;
        if (ImGui::Combo("##BrushSize", &brushIndex, brushSizes, IM_ARRAYSIZE(brushSizes)))
        {
            m_brushSize = brushIndex + 1;
        }

        ImGui::Separator();
        if (ImGui::Button("Generate map", ImVec2(110, 30)))
        {
            RequestGenerate(doc, false);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "A map modelled on the original ones in 1 click (Ctrl+G: the generator with its settings)");
        }
    }
    ImGui::End();
}

void EditorUI::RenderPalettePanel(MapDocument &doc, TextureManager &texManager, Camera &camera)
{
    ImVec2 mainViewport = ImGui::GetMainViewport()->Size;
    float panelW = 410.0f;
    ImGui::SetNextWindowPos(ImVec2(std::max(158.0f, mainViewport.x - panelW - 8.0f), 30.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, std::max(240.0f, mainViewport.y - 66.0f)), ImGuiCond_Always);

    if (ImGui::Begin(
            "Palette and settings##Palettes",
            nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse))
    {
        if (ImGui::BeginTabBar("PaletteTabs"))
        {
            // Tab 1: Terrain Tiles
            const ImGuiTabItemFlags tileTabFlags
                = (m_paletteTabToSelect == 0) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Tiles", nullptr, tileTabFlags))
            {
                int tileset = doc.GetTilesetId();
                ImGui::Text("Tileset: %s (ID %d)", (tileset == 1 ? "Desert / Canyon" : "Jungle / Peru"), tileset);

                ImGui::Text("Selected tile: #%d", m_selectedTile);
                ImGui::SameLine();
                if (ImGui::Button("Fill all"))
                {
                    doc.PushUndoSnapshot();
                    doc.FillAll(static_cast<std::uint8_t>(m_selectedTile));
                }

                ImGui::Separator();
                ImGui::Text("Quick categories:");
                if (ImGui::Button("Grass (0)"))
                    m_selectedTile = 0;
                ImGui::SameLine();
                if (ImGui::Button("Meadow (10)"))
                    m_selectedTile = 10;
                ImGui::SameLine();
                if (ImGui::Button("Road (18)"))
                    m_selectedTile = 18;
                ImGui::SameLine();
                if (ImGui::Button("Dirt (26)"))
                    m_selectedTile = 26;
                ImGui::SameLine();
                if (ImGui::Button("Thicket (235)"))
                    m_selectedTile = (tileset == 1 ? 238 : 235);

                ImGui::Separator();
                ImGui::Text("Grid of all tiles (red corner - the sprite is impassable):");

                ImGui::BeginChild("TileGridScroll", ImVec2(0, 360), true);
                int totalTiles = 245;
                int cols = 5;
                for (int t = 0; t < totalTiles; ++t)
                {
                    bool selected = (m_selectedTile == t);
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d", t);

                    if (selected)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.4f, 1.0f));
                    }
                    const bool clicked = ImGui::Button((std::string("##Tile") + buf).c_str(), ImVec2(68, 54));
                    const ImVec2 itemMin = ImGui::GetItemRectMin();
                    const ImVec2 itemMax = ImGui::GetItemRectMax();
                    ImDrawList *drawList = ImGui::GetWindowDrawList();
                    drawList->PushClipRect(itemMin, itemMax, true);
                    const int compositeId = texManager.GetTileCompositeId(tileset, t);
                    DrawCompositeSprite(
                        drawList,
                        texManager,
                        compositeId,
                        itemMin.x + 17.0f,
                        itemMin.y + 4.0f,
                        1.0f,
                        Faction::Resistance);
                    char tileLabel[16];
                    std::snprintf(tileLabel, sizeof(tileLabel), "%d", t);
                    const ImVec2 labelSize = ImGui::CalcTextSize(tileLabel);
                    drawList->AddText(
                        ImVec2(itemMin.x + (68.0f - labelSize.x) * 0.5f, itemMax.y - labelSize.y - 3.0f),
                        ImGui::GetColorU32(ImGuiCol_Text),
                        tileLabel);
                    drawList->PopClipRect();
                    if (TileCollision::IsBlocked(tileset, t))
                    {
                        // Corner mark: this sprite blocks movement
                        drawList->AddTriangleFilled(
                            ImVec2(itemMax.x - 14.0f, itemMin.y),
                            ImVec2(itemMax.x, itemMin.y),
                            ImVec2(itemMax.x, itemMin.y + 14.0f),
                            IM_COL32(220, 50, 40, 255));
                    }
                    if (selected)
                    {
                        drawList->AddRect(itemMin, itemMax, IM_COL32(255, 220, 80, 255), 0.0f, 0, 3.0f);
                    }
                    if (clicked)
                    {
                        m_selectedTile = t;
                        m_activeTool = EditorTool::TileBrush;
                    }
                    if (selected)
                    {
                        ImGui::PopStyleColor();
                    }

                    if ((t + 1) % cols != 0 && t + 1 < totalTiles)
                    {
                        ImGui::SameLine();
                    }
                }
                ImGui::EndChild();

                ImGui::EndTabItem();
            }

            // Tab 2: Decorations
            const ImGuiTabItemFlags decorTabFlags
                = (m_paletteTabToSelect == 1) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Decorations", nullptr, decorTabFlags))
            {
                ImGui::Text("Authentic map objects:");
                ImGui::Text("Selected decoration: type %d", m_selectedDecorType);

                const char *decorNames[]
                    = {"0: Small palm",
                       "1: Medium palm",
                       "2: Tall palm",
                       "3: Jungle tree",
                       "4: Tropical bush",
                       "5: Vine thicket",
                       "6: Granite boulder",
                       "7: Rock ridge",
                       "8: Scree",
                       "14: Inca ruins",
                       "15: Wooden bridge",
                       "16: Stone bridge",
                       "20: Shell crater",
                       "21: Helicopter wreck",
                       "22: Abandoned bunker",
                       "25: Water rapids",
                       "28: Tropical flower",
                       "30: Tree trunk"};

                ImGui::BeginChild("DecorListScroll", ImVec2(0, 360), true);
                for (std::size_t i = 0; i < sizeof(decorNames) / sizeof(decorNames[0]); ++i)
                {
                    int dtype = 0;
                    std::sscanf(decorNames[i], "%d", &dtype);
                    bool selected = (m_selectedDecorType == dtype);

                    const std::string itemId = "##Decor" + std::to_string(dtype);
                    if (ImGui::Selectable(itemId.c_str(), selected, 0, ImVec2(0.0f, 48.0f)))
                    {
                        m_selectedDecorType = dtype;
                        m_activeTool = EditorTool::PlaceDecor;
                    }
                    const ImVec2 itemMin = ImGui::GetItemRectMin();
                    const ImVec2 itemMax = ImGui::GetItemRectMax();
                    ImDrawList *drawList = ImGui::GetWindowDrawList();
                    DrawDecorThumbnail(drawList, texManager, doc.GetTilesetId(), dtype, itemMin, itemMax);
                    drawList->AddText(
                        ImVec2(itemMin.x + 54.0f, itemMin.y + 15.0f), ImGui::GetColorU32(ImGuiCol_Text), decorNames[i]);
                }

                ImGui::Separator();
                ImGui::Text("Any decoration number (0..95):");
                ImGui::SliderInt("Type##DecorSlider", &m_selectedDecorType, 0, 95);
                if (ImGui::Button("Pick this decoration"))
                {
                    m_activeTool = EditorTool::PlaceDecor;
                }
                ImGui::EndChild();

                ImGui::EndTabItem();
            }

            // Tab 3: Walls
            const ImGuiTabItemFlags wallTabFlags
                = (m_paletteTabToSelect == 2) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Walls", nullptr, wallTabFlags))
            {
                ImGui::Text("Defensive fortifications:");

                int wallFacInt = (m_selectedWallFaction == Faction::Resistance) ? 0 : 1;
                if (ImGui::RadioButton("Resistance (Red 1101)", wallFacInt == 0))
                {
                    m_selectedWallFaction = Faction::Resistance;
                    m_activeTool = EditorTool::PlaceWall;
                }
                if (ImGui::RadioButton("Confederation (Blue 1001)", wallFacInt == 1))
                {
                    m_selectedWallFaction = Faction::Confederation;
                    m_activeTool = EditorTool::PlaceWall;
                }

                for (const auto &wall :
                     {std::pair<const char *, int> {"Wall (Resistance)", 25},
                      std::pair<const char *, int> {"Wall (Confederation)", 15}})
                {
                    const bool selected = (wall.second == 25 && m_selectedWallFaction == Faction::Resistance)
                                          || (wall.second == 15 && m_selectedWallFaction == Faction::Confederation);
                    const std::string id = std::string("##Wall") + std::to_string(wall.second);
                    if (ImGui::Selectable(id.c_str(), selected, 0, ImVec2(0.0f, 48.0f)))
                    {
                        m_selectedWallFaction = wall.second == 25 ? Faction::Resistance : Faction::Confederation;
                        m_activeTool = EditorTool::PlaceWall;
                    }
                    const ImVec2 itemMin = ImGui::GetItemRectMin();
                    const ImVec2 itemMax = ImGui::GetItemRectMax();
                    DrawBuildingThumbnail(
                        ImGui::GetWindowDrawList(),
                        texManager,
                        wall.second,
                        wall.second == 25 ? Faction::Resistance : Faction::Confederation,
                        itemMin,
                        itemMax);
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(itemMin.x + 54.0f, itemMin.y + 15.0f), ImGui::GetColorU32(ImGuiCol_Text), wall.first);
                }
                if (ImGui::Button("Activate the wall brush", ImVec2(200, 30)))
                {
                    m_activeTool = EditorTool::PlaceWall;
                }
                ImGui::EndTabItem();
            }

            // Tab 4: Buildings
            const ImGuiTabItemFlags buildingTabFlags
                = (m_paletteTabToSelect == 3) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Buildings", nullptr, buildingTabFlags))
            {
                ImGui::Text("Building faction:");
                int bFacInt = (m_selectedBuildingFaction == Faction::Resistance)
                                  ? 0
                                  : (m_selectedBuildingFaction == Faction::Confederation ? 1 : 2);
                if (ImGui::RadioButton("Resistance##BFac", bFacInt == 0))
                {
                    m_selectedBuildingFaction = Faction::Resistance;
                    m_selectedBuildingType = 8;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Confederation##BFac", bFacInt == 1))
                {
                    m_selectedBuildingFaction = Faction::Confederation;
                    m_selectedBuildingType = 0;
                }

                ImGui::Separator();
                struct BuildingDef
                {
                    int resType;
                    int confType;
                    const char *resName;
                    const char *confName;
                    int resWidth;
                    int resHeight;
                    int confWidth;
                    int confHeight;
                };

                const BuildingDef bdefs[]
                    = {{8, 0, "Headquarters (Resistance)", "Headquarters (Confederation)", 3, 4, 4, 4},
                       {10, 2, "Barracks (Resistance)", "Barracks (Confederation)", 3, 3, 3, 3},
                       {11, 3, "Factory (Resistance)", "Factory (Confederation)", 3, 3, 4, 3},
                       {12, 4, "Laboratory (Resistance)", "Tech center (Confederation)", 3, 3, 3, 3},
                       {14, 7, "Tower (Resistance)", "Missile turret (Confederation)", 2, 2, 2, 2},
                       {13, 5, "Bunker (Resistance)", "Bunker (Confederation)", 2, 2, 2, 2},
                       {9, 1, "Power plant (Resistance)", "Power plant (Confederation)", 3, 3, 3, 3},
                       {-1, 6, "", "Radar (Confederation)", 0, 0, 2, 2}};

                for (const auto &bd : bdefs)
                {
                    int btype = (m_selectedBuildingFaction == Faction::Resistance) ? bd.resType : bd.confType;
                    if (btype < 0)
                        continue;
                    bool sel = (m_selectedBuildingType == btype && m_activeTool == EditorTool::PlaceBuilding);

                    char label[64];
                    int width = (m_selectedBuildingFaction == Faction::Resistance) ? bd.resWidth : bd.confWidth;
                    int height = (m_selectedBuildingFaction == Faction::Resistance) ? bd.resHeight : bd.confHeight;
                    const std::string itemId = "##Building" + std::to_string(btype);
                    if (ImGui::Selectable(itemId.c_str(), sel, 0, ImVec2(0.0f, 50.0f)))
                    {
                        m_selectedBuildingType = btype;
                        m_activeTool = EditorTool::PlaceBuilding;
                    }
                    const ImVec2 itemMin = ImGui::GetItemRectMin();
                    const ImVec2 itemMax = ImGui::GetItemRectMax();
                    DrawBuildingThumbnail(
                        ImGui::GetWindowDrawList(), texManager, btype, m_selectedBuildingFaction, itemMin, itemMax);
                    std::snprintf(
                        label,
                        sizeof(label),
                        "%s (%dx%d)",
                        m_selectedBuildingFaction == Faction::Resistance ? bd.resName : bd.confName,
                        width,
                        height);
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(itemMin.x + 54.0f, itemMin.y + 16.0f), ImGui::GetColorU32(ImGuiCol_Text), label);
                }
                ImGui::EndTabItem();
            }

            // Tab 5: Units
            const ImGuiTabItemFlags unitTabFlags
                = (m_paletteTabToSelect == 4) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Units", nullptr, unitTabFlags))
            {
                ImGui::Text("Unit faction:");
                int uFacInt = (m_selectedUnitFaction == Faction::Resistance) ? 0 : 1;
                if (ImGui::RadioButton("Resistance##UFac", uFacInt == 0))
                {
                    m_selectedUnitFaction = Faction::Resistance;
                    m_selectedUnitArchetype = 0;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Confederation##UFac", uFacInt == 1))
                {
                    m_selectedUnitFaction = Faction::Confederation;
                    m_selectedUnitArchetype = 1;
                }

                ImGui::Separator();
                struct UnitDef
                {
                    int resistanceId;
                    int confederationId;
                    const char *resistanceName;
                    const char *confederationName;
                };

                const UnitDef udefs[]
                    = {{0, 1, "Rifleman (Resistance)", "Rifleman (Confederation)"},
                       {2, 5, "Grenadier (Resistance)", "Flamethrower (Confederation)"},
                       {4, 3, "Sniper (Resistance)", "Assault trooper (Confederation)"},
                       {6, 7, "Coyote (Resistance)", "Fortress (Confederation)"},
                       {8, 9, "Assault tank (Resistance)", "Hammer (Confederation)"},
                       {10, 11, "Heavy tank (Resistance)", "Zeus (Confederation)"},
                       {12, 13, "MLRS (Resistance)", "Grad (Confederation)"},
                       {14, -1, "Sniper in siege mode", ""},
                       {15, -1, "Tank in siege mode", ""},
                       {16, -1, "MLRS in siege mode", ""}};

                for (const auto &ud : udefs)
                {
                    const bool resistance = m_selectedUnitFaction == Faction::Resistance;
                    const int archetypeId = resistance ? ud.resistanceId : ud.confederationId;
                    if (archetypeId < 0)
                        continue;
                    const bool sel = (m_selectedUnitArchetype == archetypeId && m_activeTool == EditorTool::PlaceUnit);
                    const std::string itemId = "##Unit" + std::to_string(archetypeId);
                    if (ImGui::Selectable(itemId.c_str(), sel, 0, ImVec2(0.0f, 50.0f)))
                    {
                        m_selectedUnitArchetype = archetypeId;
                        m_activeTool = EditorTool::PlaceUnit;
                    }
                    const ImVec2 itemMin = ImGui::GetItemRectMin();
                    const ImVec2 itemMax = ImGui::GetItemRectMax();
                    DrawUnitThumbnail(
                        ImGui::GetWindowDrawList(), texManager, archetypeId, m_selectedUnitFaction, itemMin, itemMax);
                    const char *unitName = resistance ? ud.resistanceName : ud.confederationName;
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(itemMin.x + 54.0f, itemMin.y + 17.0f), ImGui::GetColorU32(ImGuiCol_Text), unitName);
                }

                ImGui::Separator();
                const int selectedUnits = doc.CountSelectedUnits();
                if (selectedUnits > 0)
                {
                    for (const auto &entity : doc.GetEntities())
                    {
                        if (entity.selected && !entity.isBuilding)
                        {
                            m_selectedUnitFacing = entity.facing;
                            break;
                        }
                    }
                }
                ImGui::Text("Selected units: %d", selectedUnits);
                ImGui::Text(selectedUnits > 0 ? "Facing of the selected and new units:" : "Facing of new units:");
                const char *dirs[]
                    = {"East (Right)",
                       "South-East",
                       "South (Down)",
                       "South-West",
                       "West (Left)",
                       "North-West",
                       "North (Up)",
                       "North-East"};
                if (ImGui::Combo("Facing", &m_selectedUnitFacing, dirs, IM_ARRAYSIZE(dirs))
                    && doc.CountSelectedUnits() > 0)
                {
                    doc.PushUndoSnapshot();
                    doc.SetSelectedUnitFacing(m_selectedUnitFacing);
                }
                ImGui::EndTabItem();
            }

            // Tab 6: Players & Starts
            const ImGuiTabItemFlags playersTabFlags
                = (m_paletteTabToSelect == 5) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Players", nullptr, playersTabFlags))
            {
                static const ImU32 kPlayerColors[8] = {
                    IM_COL32(235, 40, 40, 255),  // P1: Red (Resistance)
                    IM_COL32(40, 110, 240, 255), // P2: Blue (Confederation)
                    IM_COL32(40, 200, 80, 255),  // P3: Green
                    IM_COL32(240, 200, 40, 255), // P4: Yellow
                    IM_COL32(180, 60, 220, 255), // P5: Purple
                    IM_COL32(40, 210, 220, 255), // P6: Cyan
                    IM_COL32(240, 130, 40, 255), // P7: Orange
                    IM_COL32(230, 110, 170, 255) // P8: Pink
                };
                static const char *kPlayerNames[8]
                    = {"Player 1 (Resistance HQ)",
                       "Player 2 (Confederation HQ)",
                       "Player 3",
                       "Player 4",
                       "Player 5",
                       "Player 6",
                       "Player 7",
                       "Player 8"};

                const int playerCount = doc.GetPlayerCount();
                ImGui::Text("Player starts on map: %d / 8", playerCount);
                ImGui::TextDisabled("Drag markers on map, right-click tile, or edit below:");
                ImGui::Separator();

                for (int p = 0; p < playerCount; ++p)
                {
                    ImGui::PushID(p);
                    const auto [sx, sy] = doc.GetStartPos(p);

                    // Color swatch
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    ImVec2 curPos = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(curPos, ImVec2(curPos.x + 16, curPos.y + 16), kPlayerColors[p], 3.0f);
                    dl->AddRect(curPos, ImVec2(curPos.x + 16, curPos.y + 16), IM_COL32(255, 255, 255, 220), 3.0f);
                    ImGui::Dummy(ImVec2(20, 16));
                    ImGui::SameLine();

                    // Name and active placement indicator
                    const bool isPicking = (m_activeTool == EditorTool::PlayerStarts && m_selectedPlayerSlot == p);
                    if (isPicking)
                    {
                        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "%s [Placing]", kPlayerNames[p]);
                    }
                    else
                    {
                        ImGui::Text("%s", kPlayerNames[p]);
                    }

                    // Coordinates input
                    int coords[2] = {sx, sy};
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::InputInt2("##pos", coords))
                    {
                        const int newX = std::clamp(coords[0], 0, std::max(0, doc.GetWidth() - 1));
                        const int newY = std::clamp(coords[1], 0, std::max(0, doc.GetHeight() - 1));
                        if (newX != sx || newY != sy)
                        {
                            doc.PushUndoSnapshot();
                            doc.SetStartPos(p, newX, newY);
                        }
                    }
                    ImGui::SameLine();

                    if (ImGui::Button("Focus"))
                    {
                        camera.CenterOn(sx * TILE_WIDTH + TILE_WIDTH * 0.5f, sy * TILE_HEIGHT + TILE_HEIGHT * 0.5f);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("Center camera on this start position");
                    }
                    ImGui::SameLine();

                    if (ImGui::Button("Place"))
                    {
                        m_selectedPlayerSlot = p;
                        m_activeTool = EditorTool::PlayerStarts;
                        ShowToast(std::string("Click on map to place ") + kPlayerNames[p]);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("Click on map to set this player's position");
                    }

                    if (p >= 2)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("X"))
                        {
                            doc.PushUndoSnapshot();
                            doc.RemoveStartPos(p);
                            ShowToast(std::string("Removed ") + kPlayerNames[p]);
                            ImGui::PopID();
                            break;
                        }
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("Remove this player start point");
                        }
                    }

                    ImGui::Separator();
                    ImGui::PopID();
                }

                if (playerCount < 8)
                {
                    if (ImGui::Button("+ Add Player Start", ImVec2(220, 28)))
                    {
                        const int newX = std::clamp(doc.GetWidth() / 2 + (playerCount - 2) * 2, 0, doc.GetWidth() - 1);
                        const int newY
                            = std::clamp(doc.GetHeight() / 2 + (playerCount - 2) * 2, 0, doc.GetHeight() - 1);
                        doc.PushUndoSnapshot();
                        if (doc.AddStartPos(newX, newY))
                        {
                            m_selectedPlayerSlot = playerCount;
                            m_activeTool = EditorTool::PlayerStarts;
                            ShowToast("Player start added! Click map or edit coordinates.");
                        }
                    }
                }

                ImGui::EndTabItem();
            }

            // Tab 7: Map Properties
            const ImGuiTabItemFlags propertiesTabFlags
                = (m_paletteTabToSelect == 6) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Properties", nullptr, propertiesTabFlags))
            {
                ImGui::Text("Map: %s", doc.GetName().c_str());
                ImGui::Text("Size: %d x %d cells", doc.GetWidth(), doc.GetHeight());
                ImGui::Text("Decorations: %zu", doc.GetTileMap().GetDecorations().size());
                ImGui::Text("Objects/Units: %zu", doc.GetEntities().size());
                ImGui::Text("Player start: (%d, %d)", doc.GetPlayerStartX(), doc.GetPlayerStartY());
                ImGui::Text("Enemy start: (%d, %d)", doc.GetEnemyStartX(), doc.GetEnemyStartY());

                ImGui::Separator();
                if (ImGui::Button("Recompute passability (Auto)", ImVec2(240, 30)))
                {
                    doc.AutoRecalculatePassability();
                    ShowToast("Passability grid recomputed!");
                }
                if (ImGui::Button("Save to assets/maps/", ImVec2(240, 30)))
                {
                    OpenSaveAsDialog(doc);
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
            m_paletteTabToSelect = -1;
        }
    }
    ImGui::End();
}

void EditorUI::RenderMiniMapWindow(MapDocument &doc, Camera &camera)
{
    ImVec2 mainViewport = ImGui::GetMainViewport()->Size;
    float mw = 220.0f;
    float mh = 180.0f;
    constexpr float leftPanelWidth = 150.0f;
    constexpr float rightPanelWidth = 410.0f;
    const float centerX = leftPanelWidth + (mainViewport.x - leftPanelWidth - rightPanelWidth - mw) * 0.5f;
    ImGui::SetNextWindowPos(
        ImVec2(std::max(leftPanelWidth + 8.0f, centerX), mainViewport.y - mh - 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Minimap##MiniMap", &m_showMiniMap, ImGuiWindowFlags_NoScrollbar))
    {
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();

        int mapW = doc.GetWidth();
        int mapH = doc.GetHeight();
        if (mapW > 0 && mapH > 0 && canvasSize.x > 10 && canvasSize.y > 10)
        {
            ImDrawList *drawList = ImGui::GetWindowDrawList();

            // Background
            drawList->AddRectFilled(
                canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(20, 25, 30, 255));

            float scaleX = canvasSize.x / mapW;
            float scaleY = canvasSize.y / mapH;

            // Draw coarse map terrain & water
            int step = std::max(1, mapW / 60);

            for (int y = 0; y < mapH; y += step)
            {
                for (int x = 0; x < mapW; x += step)
                {
                    std::uint8_t t = doc.GetTileMap().GetBaseTile(x, y);
                    ImU32 col = TileCollision::IsBlocked(doc.GetTilesetId(), t)
                                    ? IM_COL32(40, 70, 40, 255)
                                    : (t >= 26 ? IM_COL32(110, 95, 80, 255)
                                               : (t >= 18 ? IM_COL32(150, 125, 90, 255)
                                                          : (doc.GetTilesetId() == 1 ? IM_COL32(190, 160, 105, 255)
                                                                                     : IM_COL32(65, 120, 50, 255))));

                    if (doc.GetTileMap().GetWall(x, y) > 0)
                    {
                        col = IM_COL32(220, 200, 50, 255);
                    }

                    float px = canvasPos.x + x * scaleX;
                    float py = canvasPos.y + y * scaleY;
                    drawList->AddRectFilled(ImVec2(px, py), ImVec2(px + scaleX * step, py + scaleY * step), col);
                }
            }

            // Draw player & enemy base flags
            float ppx = canvasPos.x + doc.GetPlayerStartX() * scaleX;
            float ppy = canvasPos.y + doc.GetPlayerStartY() * scaleY;
            drawList->AddCircleFilled(ImVec2(ppx, ppy), 4.0f, IM_COL32(240, 40, 40, 255));

            float epx = canvasPos.x + doc.GetEnemyStartX() * scaleX;
            float epy = canvasPos.y + doc.GetEnemyStartY() * scaleY;
            drawList->AddCircleFilled(ImVec2(epx, epy), 4.0f, IM_COL32(50, 100, 240, 255));

            // Draw camera view rectangle
            float viewW = (camera.GetViewportWidth() / TILE_WIDTH) * scaleX / camera.GetZoom();
            float viewH = (camera.GetViewportHeight() / TILE_HEIGHT) * scaleY / camera.GetZoom();
            float viewX = canvasPos.x + (camera.GetX() / TILE_WIDTH) * scaleX;
            float viewY = canvasPos.y + (camera.GetY() / TILE_HEIGHT) * scaleY;
            drawList->AddRect(
                ImVec2(viewX, viewY),
                ImVec2(viewX + viewW, viewY + viewH),
                IM_COL32(255, 255, 255, 240),
                0.0f,
                0,
                1.5f);

            // Handle minimap click navigation
            if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                ImVec2 mousePos = ImGui::GetMousePos();
                float relX = (mousePos.x - canvasPos.x) / canvasSize.x;
                float relY = (mousePos.y - canvasPos.y) / canvasSize.y;
                float worldTargetX = relX * mapW * TILE_WIDTH;
                float worldTargetY = relY * mapH * TILE_HEIGHT;
                camera.CenterOn(worldTargetX, worldTargetY);
            }
        }
    }
    ImGui::End();
}

void EditorUI::RenderStatusBar(const MapDocument &doc, const Camera &camera)
{
    ImVec2 mainViewport = ImGui::GetMainViewport()->Size;
    float h = 26.0f;
    ImGui::SetNextWindowPos(ImVec2(0, mainViewport.y - h));
    ImGui::SetNextWindowSize(ImVec2(mainViewport.x, h));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove
                             | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##StatusBar", nullptr, flags))
    {
        const char *toolNames[]
            = {"Tile brush",
               "Fill",
               "Decoration",
               "Wall",
               "Building",
               "Unit",
               "Player base",
               "Enemy base",
               "Passability",
               "Eraser",
               "Eyedropper",
               "Select"};
        int toolIdx = static_cast<int>(m_activeTool);
        const char *tname = (toolIdx >= 0 && toolIdx < IM_ARRAYSIZE(toolNames)) ? toolNames[toolIdx] : "None";

        if (m_hoverTileX >= 0 && m_hoverTileY >= 0 && m_hoverTileX < doc.GetWidth() && m_hoverTileY < doc.GetHeight())
        {
            std::uint8_t t = doc.GetTileMap().GetBaseTile(m_hoverTileX, m_hoverTileY);
            bool pass = doc.GetTileMap().IsPassable(m_hoverTileX, m_hoverTileY);
            std::uint16_t w = doc.GetTileMap().GetWall(m_hoverTileX, m_hoverTileY);

            ImGui::Text(
                "Cell: (%d, %d) | Tile #%d [%s] | Wall: %s | Tool: %s | Map: %dx%d | Zoom: %d%%",
                m_hoverTileX,
                m_hoverTileY,
                t,
                pass ? "Passable" : "Blocked",
                w > 0 ? "Yes" : "No",
                tname,
                doc.GetWidth(),
                doc.GetHeight(),
                static_cast<int>(camera.GetZoom() * 100.0f));
        }
        else
        {
            ImGui::Text(
                "Map: %dx%d | Tool: %s | Zoom: %d%% | [RMB/MMB: Pan, Wheel: Zoom]",
                doc.GetWidth(),
                doc.GetHeight(),
                tname,
                static_cast<int>(camera.GetZoom() * 100.0f));
        }
    }
    ImGui::End();
}

void EditorUI::RenderToastNotification()
{
    if (m_toastTimer > 0.0f && !m_toastMessage.empty())
    {
        m_toastTimer -= ImGui::GetIO().DeltaTime;
        ImVec2 mainViewport = ImGui::GetMainViewport()->Size;

        ImGui::SetNextWindowPos(ImVec2(mainViewport.x * 0.5f - 200, mainViewport.y - 80), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400, 40), ImGuiCond_Always);

        ImGuiWindowFlags flags
            = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.45f, 0.2f, 0.9f));
        if (ImGui::Begin("##Toast", nullptr, flags))
        {
            ImGui::TextWrapped("%s", m_toastMessage.c_str());
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }
}

void EditorUI::RenderSaveAsModal(MapDocument &doc)
{
    if (m_openSaveModalReq)
    {
        ImGui::OpenPopup("Save Map As##SaveModal");
        m_openSaveModalReq = false;
        ScanAvailableMaps();
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Save Map As##SaveModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Saving the map to assets/maps/:");
        ImGui::Separator();

        ImGui::InputText("File name (.json)", m_saveFilename, sizeof(m_saveFilename));
        ImGui::InputText("Map name", m_saveMapName, sizeof(m_saveMapName));

        ImGui::Text("Existing maps in the folder:");
        ImGui::BeginChild("ExistingMapsSaveList", ImVec2(0, 180), true);
        for (const auto &m : m_availableMaps)
        {
            if (ImGui::Selectable(m.filename.c_str()))
            {
                std::snprintf(m_saveFilename, sizeof(m_saveFilename), "%s", m.filename.c_str());
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button("Save map to assets/maps/", ImVec2(220, 36)))
        {
            std::string fname = m_saveFilename;
            if (fname.find(".json") == std::string::npos)
            {
                fname += ".json";
            }
            std::string savePath
                = OpenArtOfWar::Assets::WritablePath(OpenArtOfWar::LoaderResources::MapsDir() + "/" + fname);
            doc.SetName(m_saveMapName);
            if (doc.SaveMap(savePath))
            {
                ShowToast("Map saved to " + savePath);
                if (m_closeAfterSave)
                {
                    m_closeAfterSave = false;
                    m_shouldClose = true;
                }
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 36)))
        {
            m_closeAfterSave = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void EditorUI::RenderOpenModal(MapDocument &doc)
{
    if (m_openOpenModalReq)
    {
        ImGui::OpenPopup("Open Map##OpenModal");
        m_openOpenModalReq = false;
        ScanAvailableMaps();
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 440), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Open Map##OpenModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Choose a map from assets/maps/:");
        ImGui::InputText("Search / Filter", m_openSearchFilter, sizeof(m_openSearchFilter));

        ImGui::BeginChild("OpenMapsList", ImVec2(0, 240), true);
        for (std::size_t i = 0; i < m_availableMaps.size(); ++i)
        {
            const auto &m = m_availableMaps[i];
            if (m_openSearchFilter[0] != '\0')
            {
                if (m.filename.find(m_openSearchFilter) == std::string::npos)
                    continue;
            }

            bool selected = (m_selectedMapIndex == static_cast<int>(i));
            char entryLabel[128];
            std::snprintf(
                entryLabel,
                sizeof(entryLabel),
                "%s  [%dx%d, %s, %zu KB]",
                m.filename.c_str(),
                m.width,
                m.height,
                (m.tilesetId == 1 ? "Desert" : "Jungle"),
                m.fileSize / 1024);

            if (ImGui::Selectable(entryLabel, selected, ImGuiSelectableFlags_AllowDoubleClick))
            {
                m_selectedMapIndex = static_cast<int>(i);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    doc.LoadMap(m.fullPath);
                    ShowToast("Map loaded: " + m.filename);
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button("Open", ImVec2(140, 36)))
        {
            if (m_selectedMapIndex >= 0 && m_selectedMapIndex < static_cast<int>(m_availableMaps.size()))
            {
                doc.LoadMap(m_availableMaps[m_selectedMapIndex].fullPath);
                ShowToast("Map loaded: " + m_availableMaps[m_selectedMapIndex].filename);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 36)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void EditorUI::RenderNewMapModal(MapDocument &doc)
{
    if (m_openNewModalReq)
    {
        ImGui::OpenPopup("New Map##NewModal");
        m_openNewModalReq = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("New Map##NewModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("New map parameters:");
        ImGui::Separator();

        ImGui::SliderInt("Width", &m_newMapWidth, 20, 128);
        ImGui::SliderInt("Height", &m_newMapHeight, 20, 128);

        ImGui::Text("Size presets:");
        if (ImGui::Button("40x40"))
        {
            m_newMapWidth = 40;
            m_newMapHeight = 40;
        }
        ImGui::SameLine();
        if (ImGui::Button("56x44 (Test range)"))
        {
            m_newMapWidth = 56;
            m_newMapHeight = 44;
        }
        ImGui::SameLine();
        if (ImGui::Button("60x40 (Standard)"))
        {
            m_newMapWidth = 60;
            m_newMapHeight = 40;
        }
        ImGui::SameLine();
        if (ImGui::Button("80x60"))
        {
            m_newMapWidth = 80;
            m_newMapHeight = 60;
        }
        ImGui::SameLine();
        if (ImGui::Button("80x80"))
        {
            m_newMapWidth = 80;
            m_newMapHeight = 80;
        }

        ImGui::Separator();
        ImGui::Text("Terrain tileset:");
        ImGui::RadioButton("0: Jungle / Green plains of Peru", &m_newMapTileset, 0);
        ImGui::RadioButton("1: Desert / Sand canyon", &m_newMapTileset, 1);

        ImGui::Separator();
        if (ImGui::Button("Create map", ImVec2(160, 36)))
        {
            doc.PushUndoSnapshot();
            doc.NewMap(m_newMapWidth, m_newMapHeight, m_newMapTileset, static_cast<std::uint8_t>(m_newMapFillTile));
            ShowToast("New map created " + std::to_string(m_newMapWidth) + "x" + std::to_string(m_newMapHeight));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 36)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void EditorUI::RenderGeneratorModal(MapDocument &doc)
{
    if (m_openGenModalReq)
    {
        ImGui::OpenPopup("Map Generator##GenModal");
        m_openGenModalReq = false;
        m_genParams.width = doc.GetWidth();
        m_genParams.height = doc.GetHeight();
        if (m_genParams.tilesetId != doc.GetTilesetId())
            MapGenerator::OriginalDefaults(m_genParams, doc.GetTilesetId());
        m_genParams.tilesetId = doc.GetTilesetId();
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Map Generator##GenModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("A skirmish map modelled on the original maps:");
        ImGui::Separator();

        ImGui::SliderInt("Width", &m_genParams.width, 30, 128);
        ImGui::SliderInt("Height", &m_genParams.height, 30, 128);

        const char *tilesetNames[] = {"Jungle", "Second tileset"};
        if (ImGui::Combo("Tileset", &m_genParams.tilesetId, tilesetNames, 2))
        {
            MapGenerator::OriginalDefaults(m_genParams, m_genParams.tilesetId);
        }

        const char *symNames[]
            = {"Natural asymmetry",
               "180° rotational (tournament RTS)",
               "Diagonal",
               "Mirrored horizontally",
               "Mirrored vertically"};
        int symInt = static_cast<int>(m_genParams.symmetry);
        if (ImGui::Combo("Map symmetry", &symInt, symNames, 5))
        {
            m_genParams.symmetry = static_cast<GeneratorSymmetry>(symInt);
        }

        ImGui::SliderInt("Players", &m_genParams.players, 2, 8, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Bases (start positions) of the map. 2 follows the symmetry above; more than 2 put the\n"
                "bases on a ring around the centre and make the terrain without symmetry.");
        }

        // Every share in percent; hover a slider for the range measured on the original maps
        ImGui::SeparatorText("Shares, %");
        int settingCount = 0;
        const MapGenerator::Setting *settings = MapGenerator::Settings(settingCount);
        for (int i = 0; i < settingCount; ++i)
        {
            ImGui::SliderInt(
                settings[i].label,
                &(m_genParams.*settings[i].field),
                settings[i].min,
                settings[i].max,
                "%d %%",
                ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", settings[i].hint);
        }
        if (ImGui::Button("Values of the original maps"))
        {
            MapGenerator::OriginalDefaults(m_genParams, m_genParams.tilesetId);
        }

        ImGui::Checkbox("Random seed", &m_genParams.randomSeed);
        if (!m_genParams.randomSeed)
        {
            int seedInt = static_cast<int>(m_genParams.seed);
            if (ImGui::InputInt("Seed", &seedInt))
            {
                m_genParams.seed = static_cast<std::uint32_t>(seedInt);
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Generate map", ImVec2(220, 36)))
        {
            ImGui::CloseCurrentPopup();
            RequestGenerate(doc, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 36)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void EditorUI::RequestGenerate(MapDocument &doc, bool useCustomParams)
{
    if (!doc.IsModified())
    {
        doc.PushUndoSnapshot();
        if (useCustomParams)
        {
            MapGenerator::Generate(doc, m_genParams);
        }
        else
        {
            MapGenerator::GenerateQuick(doc);
        }
        ShowToast("Map generated!");
    }
    else
    {
        m_pendingGenWithCustomParams = useCustomParams;
        m_openGenConfirmationReq = true;
    }
}

void EditorUI::RenderGeneratorConfirmation(MapDocument &doc)
{
    if (m_openGenConfirmationReq)
    {
        ImGui::OpenPopup("Unsaved changes##GenerateConfirmation");
        m_openGenConfirmationReq = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved changes##GenerateConfirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped(
            "The map has unsaved changes that will be lost if you generate a new map.\n"
            "Do you want to proceed and generate a new map?");
        ImGui::Separator();
        if (ImGui::Button("Generate anyway", ImVec2(150, 36)))
        {
            doc.PushUndoSnapshot();
            if (m_pendingGenWithCustomParams)
            {
                MapGenerator::Generate(doc, m_genParams);
            }
            else
            {
                MapGenerator::GenerateQuick(doc);
            }
            ShowToast("Map generated!");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save first...", ImVec2(130, 36)))
        {
            OpenSaveAsDialog(doc);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 36)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorUI::RenderCloseConfirmation(MapDocument &doc)
{
    if (m_openCloseConfirmationReq)
    {
        ImGui::OpenPopup("Unsaved changes##CloseConfirmation");
        m_openCloseConfirmationReq = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved changes##CloseConfirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("The map has unsaved changes. Save them before closing?");
        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(140, 36)))
        {
            if (doc.GetFilePath().empty())
            {
                m_closeAfterSave = true;
                OpenSaveAsDialog(doc);
                ImGui::CloseCurrentPopup();
            }
            else if (doc.SaveMap(doc.GetFilePath()))
            {
                m_shouldClose = true;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                ShowToast("Failed to save the map");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Do not save", ImVec2(140, 36)))
        {
            m_shouldClose = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 36)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorUI::RenderContextMenu(MapDocument &doc, Camera &camera)
{
    if (m_openContextMenuReq)
    {
        ImGui::OpenPopup("MapContextMenu");
        m_openContextMenuReq = false;
    }

    if (ImGui::BeginPopup("MapContextMenu"))
    {
        const int tx = m_contextMenuTileX;
        const int ty = m_contextMenuTileY;
        const int existingP = doc.FindStartPosAt(tx, ty);

        static const char *kPlayerNames[8]
            = {"Player 1 (Resistance HQ)",
               "Player 2 (Confederation HQ)",
               "Player 3",
               "Player 4",
               "Player 5",
               "Player 6",
               "Player 7",
               "Player 8"};

        if (existingP >= 0)
        {
            ImGui::Text("%s", kPlayerNames[existingP]);
            ImGui::TextDisabled("Start position at (%d, %d)", tx, ty);
            ImGui::Separator();

            if (ImGui::MenuItem("Center camera here"))
            {
                camera.CenterOn(tx * TILE_WIDTH + TILE_WIDTH * 0.5f, ty * TILE_HEIGHT + TILE_HEIGHT * 0.5f);
            }

            if (existingP >= 2)
            {
                if (ImGui::MenuItem("Remove this player start"))
                {
                    doc.PushUndoSnapshot();
                    doc.RemoveStartPos(existingP);
                    ShowToast(std::string("Removed ") + kPlayerNames[existingP]);
                }
            }
        }
        else
        {
            ImGui::Text("Tile (%d, %d)", tx, ty);
            ImGui::Separator();

            if (ImGui::MenuItem("Set Player 1 Start (Resistance HQ)"))
            {
                doc.PushUndoSnapshot();
                doc.SetStartPos(0, tx, ty);
                ShowToast("Player 1 start placed at (" + std::to_string(tx) + ", " + std::to_string(ty) + ")");
            }

            if (ImGui::MenuItem("Set Player 2 Start (Confederation HQ)"))
            {
                doc.PushUndoSnapshot();
                doc.SetStartPos(1, tx, ty);
                ShowToast("Player 2 start placed at (" + std::to_string(tx) + ", " + std::to_string(ty) + ")");
            }

            if (doc.GetPlayerCount() < 8)
            {
                std::string addLabel = "Add Player " + std::to_string(doc.GetPlayerCount() + 1) + " Start here";
                if (ImGui::MenuItem(addLabel.c_str()))
                {
                    doc.PushUndoSnapshot();
                    doc.AddStartPos(tx, ty);
                    ShowToast(
                        "Player " + std::to_string(doc.GetPlayerCount()) + " start added at (" + std::to_string(tx)
                        + ", " + std::to_string(ty) + ")");
                }
            }

            if (ImGui::BeginMenu("Move existing player start here..."))
            {
                for (int i = 0; i < doc.GetPlayerCount(); ++i)
                {
                    if (ImGui::MenuItem(kPlayerNames[i]))
                    {
                        doc.PushUndoSnapshot();
                        doc.SetStartPos(i, tx, ty);
                        ShowToast(
                            "Moved " + std::string(kPlayerNames[i]) + " to (" + std::to_string(tx) + ", "
                            + std::to_string(ty) + ")");
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Center camera here"))
            {
                camera.CenterOn(tx * TILE_WIDTH + TILE_WIDTH * 0.5f, ty * TILE_HEIGHT + TILE_HEIGHT * 0.5f);
            }
        }

        ImGui::EndPopup();
    }
}

} // namespace OpenArtOfWar
