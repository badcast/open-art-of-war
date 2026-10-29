#include "EditorUI.hpp"
#include "SmartGenerator.hpp"
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

namespace OpenArtOfWar {

namespace fs = std::filesystem;

namespace {

ImTextureID spriteTextureId(SDL_Texture* texture) {
    return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture));
}

void drawSpriteFrame(ImDrawList* drawList, TextureManager& textureManager, int spriteId,
                     float x, float y, float scale, Faction faction, int depth = 0) {
    if (depth > 6) return;
    const SpriteFrame* frame = textureManager.getSpriteFrame(spriteId);
    if (!frame || frame->width <= 0 || frame->height <= 0) return;

    int page = frame->page;
    if (page == 8 && faction == Faction::Resistance) page = 9;
    SDL_Texture* sheet = textureManager.getSpriteSheet(page);
    if (!sheet) return;

    static std::unordered_map<SDL_Texture*, ImVec2> sheetSizes;
    auto size = sheetSizes.find(sheet);
    if (size == sheetSizes.end()) {
        float width = 0.0f;
        float height = 0.0f;
        SDL_GetTextureSize(sheet, &width, &height);
        size = sheetSizes.emplace(sheet, ImVec2(static_cast<float>(width), static_cast<float>(height))).first;
    }
    if (size->second.x <= 0.0f || size->second.y <= 0.0f) return;

    const float sheetScale = textureManager.getSheetScale(page);
    const ImVec2 uv0(frame->x * sheetScale / size->second.x,
                     frame->y * sheetScale / size->second.y);
    const ImVec2 uv1((frame->x + frame->width) * sheetScale / size->second.x,
                     (frame->y + frame->height) * sheetScale / size->second.y);
    const ImVec2 min(x, y);
    const ImVec2 max(x + frame->width * scale, y + frame->height * scale);
    drawList->AddImage(spriteTextureId(sheet), min, max, uv0, uv1);
}

void drawCompositeSprite(ImDrawList* drawList, TextureManager& textureManager, int compositeId,
                         float x, float y, float scale, Faction faction, int depth = 0) {
    if (depth > 6) return;
    const IndexRange range = textureManager.getCompositeRange(compositeId);
    if (range.end < range.start) return;

    for (int index = range.start; index <= range.end; ++index) {
        const CompositePart* part = textureManager.getCompositePart(index);
        if (!part) continue;
        const float childX = x + part->ox * scale;
        const float childY = y + part->oy * scale;
        if (part->isComp == 0) {
            drawSpriteFrame(drawList, textureManager, part->child, childX, childY, scale, faction);
        } else {
            drawCompositeSprite(drawList, textureManager, part->child, childX, childY,
                                scale, faction, depth + 1);
        }
    }
}

struct SpriteBounds {
    float left{std::numeric_limits<float>::max()};
    float top{std::numeric_limits<float>::max()};
    float right{std::numeric_limits<float>::lowest()};
    float bottom{std::numeric_limits<float>::lowest()};
    bool valid{false};

    void include(float x, float y, float width, float height) {
        if (width <= 0.0f || height <= 0.0f) return;
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x + width);
        bottom = std::max(bottom, y + height);
        valid = true;
    }
};

void includeSpriteBounds(TextureManager& textureManager, int spriteId, float x, float y,
                         SpriteBounds& bounds) {
    const SpriteFrame* frame = textureManager.getSpriteFrame(spriteId);
    if (frame) bounds.include(x, y, static_cast<float>(frame->width), static_cast<float>(frame->height));
}

void includeCompositeBounds(TextureManager& textureManager, int compositeId, float x, float y,
                            SpriteBounds& bounds, int depth = 0) {
    if (depth > 6) return;
    const IndexRange range = textureManager.getCompositeRange(compositeId);
    for (int index = range.start; index <= range.end; ++index) {
        const CompositePart* part = textureManager.getCompositePart(index);
        if (!part) continue;
        const float partX = x + part->ox;
        const float partY = y + part->oy;
        if (part->isComp == 0) includeSpriteBounds(textureManager, part->child, partX, partY, bounds);
        else includeCompositeBounds(textureManager, part->child, partX, partY, bounds, depth + 1);
    }
}

struct ThumbnailTransform {
    ImVec2 origin;
    float scale{1.0f};
};

ThumbnailTransform fitThumbnail(const SpriteBounds& bounds, const ImVec2& itemMin,
                                const ImVec2& itemMax) {
    constexpr float slotWidth = 48.0f;
    const ImVec2 slotMin(itemMin.x + 4.0f, itemMin.y + 4.0f);
    const ImVec2 slotMax(itemMin.x + slotWidth, itemMax.y - 4.0f);
    const float boundsWidth = std::max(1.0f, bounds.right - bounds.left);
    const float boundsHeight = std::max(1.0f, bounds.bottom - bounds.top);
    const float availableWidth = std::max(1.0f, slotMax.x - slotMin.x - 4.0f);
    const float availableHeight = std::max(1.0f, slotMax.y - slotMin.y - 4.0f);
    const float scale = bounds.valid
        ? std::min(availableWidth / boundsWidth, availableHeight / boundsHeight) : 1.0f;
    const float x = slotMin.x + (availableWidth - boundsWidth * scale) * 0.5f - bounds.left * scale;
    const float y = slotMin.y + (availableHeight - boundsHeight * scale) * 0.5f - bounds.top * scale;
    return {ImVec2(x, y), scale};
}

void drawThumbnailFrame(ImDrawList* drawList, const ImVec2& itemMin, const ImVec2& itemMax) {
    drawList->AddRect(ImVec2(itemMin.x + 4.0f, itemMin.y + 4.0f),
                      ImVec2(itemMin.x + 48.0f, itemMax.y - 4.0f), IM_COL32(255, 255, 255, 55));
}

void drawDecorThumbnail(ImDrawList* drawList, TextureManager& textureManager,
                        int tilesetId, int decorType, const ImVec2& min, const ImVec2& max) {
    const int compositeId = textureManager.getDecorCompositeId(tilesetId, decorType);
    SpriteBounds bounds;
    includeCompositeBounds(textureManager, compositeId, 0.0f, 0.0f, bounds);
    const ThumbnailTransform transform = fitThumbnail(bounds, min, max);
    drawList->PushClipRect(ImVec2(min.x + 4.0f, min.y + 4.0f),
                           ImVec2(min.x + 48.0f, max.y - 4.0f), true);
    drawCompositeSprite(drawList, textureManager, compositeId,
                        transform.origin.x, transform.origin.y, transform.scale, Faction::Resistance);
    drawList->PopClipRect();
    drawThumbnailFrame(drawList, min, max);
}

void drawBuildingThumbnail(ImDrawList* drawList, TextureManager& textureManager,
                           int buildingType, Faction faction, const ImVec2& min, const ImVec2& max) {
    const BuildingDef* def = textureManager.getBuildingDef(buildingType);
    if (!def || def->end < def->start) return;
    SpriteBounds bounds;
    for (int index = def->start; index <= def->end; ++index) {
        const BuildingPart* part = textureManager.getBuildingPart(index);
        if (!part || part->state != 0) continue;
        if (part->isComp == 0) includeSpriteBounds(textureManager, part->child, part->ox, part->oy, bounds);
        else if (part->isComp == 1) includeCompositeBounds(textureManager, part->child, part->ox, part->oy, bounds);
        else {
            const IndexRange range = textureManager.getAnimRange(part->child);
            if (range.end < range.start) continue;
            const AnimFrame* frame = textureManager.getAnimFrame(range.start);
            if (!frame) continue;
            const float x = part->ox + frame->ox;
            const float y = part->oy + frame->oy;
            if (frame->isComp == 0) includeSpriteBounds(textureManager, frame->child, x, y, bounds);
            else includeCompositeBounds(textureManager, frame->child, x, y, bounds);
        }
    }
    const ThumbnailTransform transform = fitThumbnail(bounds, min, max);
    drawList->PushClipRect(ImVec2(min.x + 4.0f, min.y + 4.0f),
                           ImVec2(min.x + 48.0f, max.y - 4.0f), true);
    for (int index = def->start; index <= def->end; ++index) {
        const BuildingPart* part = textureManager.getBuildingPart(index);
        if (!part || part->state != 0) continue;
        const float x = transform.origin.x + part->ox * transform.scale;
        const float y = transform.origin.y + part->oy * transform.scale;
        if (part->isComp == 0) {
            drawSpriteFrame(drawList, textureManager, part->child, x, y, transform.scale, faction);
        } else if (part->isComp == 1) {
            drawCompositeSprite(drawList, textureManager, part->child, x, y, transform.scale, faction);
        } else {
            const IndexRange range = textureManager.getAnimRange(part->child);
            if (range.end >= range.start) {
                const AnimFrame* frame = textureManager.getAnimFrame(range.start);
                if (frame) {
                    const float frameX = x + frame->ox * transform.scale;
                    const float frameY = y + frame->oy * transform.scale;
                    if (frame->isComp == 0) {
                        drawSpriteFrame(drawList, textureManager, frame->child, frameX, frameY, transform.scale, faction);
                    } else {
                        drawCompositeSprite(drawList, textureManager, frame->child, frameX, frameY, transform.scale, faction);
                    }
                }
            }
        }
    }
    drawList->PopClipRect();
    drawThumbnailFrame(drawList, min, max);
}

void drawUnitThumbnail(ImDrawList* drawList, TextureManager& textureManager,
                       int unitType, Faction faction, const ImVec2& min, const ImVec2& max) {
    const int sequence = textureManager.getUnitBaseSeq(unitType, static_cast<int>(ActionState::Idle));
    const IndexRange sequenceRange = textureManager.getUnitSequenceRange(sequence);
    if (sequenceRange.end < sequenceRange.start) return;
    const UnitSequenceStep* step = textureManager.getUnitSequenceStep(sequenceRange.start);
    if (!step) return;
    const IndexRange bodyRange = textureManager.getUnitBodyRange(step->bodyId);
    SpriteBounds bounds;
    for (int index = bodyRange.start; index <= bodyRange.end; ++index) {
        const UnitLayer* layer = textureManager.getUnitLayer(index);
        if (!layer || layer->flag > 2) continue;
        const float x = step->ox + layer->ox;
        const float y = step->oy + layer->oy;
        if (layer->isComp == 0) includeSpriteBounds(textureManager, layer->child, x, y, bounds);
        else includeCompositeBounds(textureManager, layer->child, x, y, bounds);
    }
    const ThumbnailTransform transform = fitThumbnail(bounds, min, max);
    drawList->PushClipRect(ImVec2(min.x + 4.0f, min.y + 4.0f),
                           ImVec2(min.x + 48.0f, max.y - 4.0f), true);
    for (int index = bodyRange.start; index <= bodyRange.end; ++index) {
        const UnitLayer* layer = textureManager.getUnitLayer(index);
        if (!layer || layer->flag > 2) continue;
        const float x = transform.origin.x + (step->ox + layer->ox) * transform.scale;
        const float y = transform.origin.y + (step->oy + layer->oy) * transform.scale;
        if (layer->isComp == 0) {
            drawSpriteFrame(drawList, textureManager, layer->child, x, y, transform.scale, faction);
        } else {
            drawCompositeSprite(drawList, textureManager, layer->child, x, y, transform.scale, faction);
        }
    }
    drawList->PopClipRect();
    drawThumbnailFrame(drawList, min, max);
}

} // namespace

EditorUI::EditorUI() {
    init();
}

void EditorUI::init() {
    m_genParams.width = 60;
    m_genParams.height = 40;
    m_genParams.tilesetId = 0;
    m_genParams.biome = GeneratorBiome::RiverValley;
    m_genParams.symmetry = GeneratorSymmetry::Rotational180;
    m_genParams.addRiver = true;
    m_genParams.riverBridges = 2;
    m_genParams.generateBases = true;
    m_genParams.decorDensity = 1;
    m_genParams.randomSeed = true;

    scanAvailableMaps();
}

void EditorUI::showToast(const std::string& message, float duration) {
    m_toastMessage = message;
    m_toastTimer = duration;
}

void EditorUI::openSaveAsDialog(const MapDocument& doc) {
    std::string base = doc.getName();
    if (base.empty() || base == "untitled" || base == "new_map") {
        base = "custom_map_01";
    }
    std::snprintf(m_saveMapName, sizeof(m_saveMapName), "%s", base.c_str());
    std::snprintf(m_saveFilename, sizeof(m_saveFilename), "%s.json", base.c_str());
    m_openSaveModalReq = true;
}

void EditorUI::openOpenDialog() {
    scanAvailableMaps();
    m_openOpenModalReq = true;
}

void EditorUI::openNewMapDialog() {
    m_openNewModalReq = true;
}

void EditorUI::openGeneratorDialog() {
    m_openGenModalReq = true;
}

void EditorUI::requestClose(const MapDocument& doc) {
    if (!doc.isModified()) {
        m_shouldClose = true;
        return;
    }
    m_openCloseConfirmationReq = true;
}

void EditorUI::scanAvailableMaps() {
    m_availableMaps.clear();
    const std::string searchDirs[] = {"assets/maps"};

    for (const auto& sdir : searchDirs) {
        for (const std::string& mapPath : OpenArtOfWar::Assets::list(sdir, ".json")) {
            {
                MapFileInfo info;
                info.filename = fs::path(mapPath).filename().string();
                info.fullPath = mapPath;

                // Quick metadata peek
                std::string text;
                if (OpenArtOfWar::Assets::readText(info.fullPath, text)) {
                    info.fileSize = text.size();
                    try {
                        nlohmann::json j = nlohmann::json::parse(text);
                        info.width = j.value("width", 0);
                        info.height = j.value("height", 0);
                        info.tilesetId = j.value("tilesetId", 0);
                    } catch (...) {}
                }

                // Avoid duplicates
                bool exists = false;
                for (const auto& m : m_availableMaps) {
                    if (m.filename == info.filename) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    m_availableMaps.push_back(info);
                }
            }
        }
    }

    std::sort(m_availableMaps.begin(), m_availableMaps.end(), [](const MapFileInfo& a, const MapFileInfo& b) {
        return a.filename < b.filename;
    });
}

void EditorUI::render(MapDocument& doc, Camera& camera, TextureManager& texManager, Renderer& /*renderer*/) {
    // Top menu bar
    renderMenuBar(doc, camera);

    // Left toolbar
    renderToolbar(doc, texManager);

    // Right palette panel
    renderPalettePanel(doc, texManager);

    // Minimap window
    if (m_showMiniMap) {
        renderMiniMapWindow(doc, camera);
    }

    // Bottom status bar
    renderStatusBar(doc, camera);

    // Modals
    renderSaveAsModal(doc);
    renderOpenModal(doc);
    renderNewMapModal(doc);
    renderGeneratorModal(doc);
    renderCloseConfirmation(doc);

    // Toast
    renderToastNotification();
}

void EditorUI::renderMenuBar(MapDocument& doc, Camera& camera) {
    if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("Файл")) {
            if (ImGui::MenuItem("Новая карта...", "Ctrl+N")) {
                openNewMapDialog();
            }
            if (ImGui::MenuItem("Открыть карту...", "Ctrl+O")) {
                openOpenDialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Сохранить", "Ctrl+S")) {
                if (doc.getFilePath().empty()) {
                    openSaveAsDialog(doc);
                } else {
                    doc.saveMap(doc.getFilePath());
                    showToast("Карта сохранена: " + doc.getFilePath());
                }
            }
            if (ImGui::MenuItem("Сохранить карту как...", "Ctrl+Shift+S")) {
                openSaveAsDialog(doc);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Выход", "Alt+F4")) {
                SDL_Event quitEv;
                quitEv.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quitEv);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Правка")) {
            if (ImGui::MenuItem("Отменить", "Ctrl+Z", false, doc.canUndo())) {
                doc.undo();
                showToast("Отмена действия");
            }
            if (ImGui::MenuItem("Повторить", "Ctrl+Y", false, doc.canRedo())) {
                doc.redo();
                showToast("Повтор действия");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Залить карту выбранным тайлом")) {
                doc.pushUndoSnapshot();
                doc.fillAll(static_cast<uint8_t>(m_selectedTile));
                showToast("Вся карта залита тайлом #" + std::to_string(m_selectedTile));
            }
            if (ImGui::MenuItem("Пересчитать проходимость")) {
                doc.autoRecalculatePassability();
                showToast("Сетка проходимости обновлена");
            }
            if (ImGui::MenuItem("Очистить всё")) {
                doc.pushUndoSnapshot();
                doc.clearAll();
                showToast("Карта очищена");
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Генератор")) {
            if (ImGui::MenuItem("Автогенератор карт...", "Ctrl+G")) {
                openGeneratorDialog();
            }
            if (ImGui::MenuItem("Быстрая случайная карта (1 клик!)")) {
                doc.pushUndoSnapshot();
                MapAutoGenerator::generateQuickRandom(doc);
                showToast("Сгенерирована случайная карта (" + std::to_string(doc.getWidth()) + "x" + std::to_string(doc.getHeight()) + ")");
            }
            if (ImGui::MenuItem("Smart-карта (1 клик, по образцу оригинала)")) {
                GeneratorParams params;
                params.width = doc.getWidth();
                params.height = doc.getHeight();
                params.tilesetId = doc.getTilesetId();
                doc.pushUndoSnapshot();
                SmartGenerator::generate(doc, params);
                showToast("Сгенерирована Smart-карта (" + std::to_string(doc.getWidth()) + "x" + std::to_string(doc.getHeight()) + ")");
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Вид")) {
            ImGui::MenuItem("Сетка тайлов (G)", nullptr, &m_showGrid);
            ImGui::MenuItem("Коллизии / Проходимость (C)", nullptr, &m_showPassability);
            ImGui::MenuItem("Миникарта (M)", nullptr, &m_showMiniMap);
            ImGui::Separator();
            if (ImGui::MenuItem("Центрировать камеру (F)")) {
                float mw = doc.getWidth() * TILE_WIDTH * 0.5f;
                float mh = doc.getHeight() * TILE_HEIGHT * 0.5f;
                camera.centerOn(mw, mh);
            }
            if (ImGui::MenuItem("Масштаб 100% (1:1)")) {
                camera.setZoom(1.0f);
            }
            ImGui::EndMenu();
        }

        // Center the primary actions in the menu bar.
        constexpr float actionGroupWidth = 390.0f;
        const float centeredActionX = (ImGui::GetWindowWidth() - actionGroupWidth) * 0.5f;
        const float actionGroupX = std::max(ImGui::GetCursorPosX() + 12.0f, centeredActionX);
        ImGui::SameLine(actionGroupX);
        if (ImGui::Button("Случайная карта")) {
            doc.pushUndoSnapshot();
            MapAutoGenerator::generateQuickRandom(doc);
            showToast("Сгенерирована случайная карта!");
        }
        ImGui::SameLine();
        if (ImGui::Button("Сохранить в assets/maps/")) {
            openSaveAsDialog(doc);
        }
        ImGui::SameLine();
        if (ImGui::Button("Открыть")) {
            openOpenDialog();
        }

        ImGui::EndMainMenuBar();
    }
}

void EditorUI::renderToolbar(MapDocument& doc, TextureManager& texManager) {
    const ImVec2 viewport = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(8.0f, 30.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(150.0f, std::max(240.0f, viewport.y - 66.0f)), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("Инструменты##Toolbar", nullptr, flags)) {
        ImGui::Text("Выбранный тайл: #%d", m_selectedTile);
        const ImVec2 previewPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("selected-tile-preview", ImVec2(96.0f, 42.0f));
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 previewMin = ImGui::GetItemRectMin();
        const ImVec2 previewMax = ImGui::GetItemRectMax();
        drawList->PushClipRect(previewMin, previewMax, true);
        const int tileComposite = texManager.getTileCompositeId(doc.getTilesetId(), m_selectedTile);
        drawCompositeSprite(drawList, texManager, tileComposite,
                            previewPos.x + 20.0f, previewPos.y + 8.0f, 0.85f,
                            Faction::Resistance);
        drawList->PopClipRect();
        ImGui::Separator();

        auto toolBtn = [&](const char* label, EditorTool tool, const char* tooltip) {
            bool active = (m_activeTool == tool);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.3f, 1.0f));
            }
            if (ImGui::Button(label, ImVec2(96, 32))) {
                m_activeTool = tool;
                switch (tool) {
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
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tooltip);
            }
            if (active) {
                ImGui::PopStyleColor();
            }
        };

        toolBtn("Кисть тайлов", EditorTool::TileBrush, "Кисть тайлов (Pencil / Brush)");
        toolBtn("Заливка", EditorTool::BucketFill, "Заливка области тайлом (Bucket Fill)");
        toolBtn("Декорации", EditorTool::PlaceDecor, "Размещение декораций (деревья, скалы)");
        toolBtn("Стены", EditorTool::PlaceWall, "Размещение защитных стен");
        toolBtn("Постройки", EditorTool::PlaceBuilding, "Размещение построек баз");
        toolBtn("Юниты", EditorTool::PlaceUnit, "Размещение стартовых юнитов");
        toolBtn("Старт игрока", EditorTool::SetPlayerStart, "Установка точки старта игрока (HQ)");
        toolBtn("Старт врага", EditorTool::SetEnemyStart, "Установка точки старта врага (HQ)");
        toolBtn("Ещё старт", EditorTool::AddStartPosition,
                "Дополнительные точки старта (игроки 3-8 сетевой игры): клик добавляет, клик по точке убирает. "
                "Карта вмещает столько игроков, сколько на ней точек старта"),
        toolBtn("Проходимость", EditorTool::PassabilityBrush, "Кисть проходимости (блок/проход)");
        toolBtn("Ластик", EditorTool::Eraser, "Ластик (удаление объекта/стены/декора)");
        toolBtn("Пипетка", EditorTool::Eyedropper, "Пипетка (выбрать тайл под курсором)");
        toolBtn("Выделение", EditorTool::Select, "Выделить объект; Shift добавляет или убирает из выделения");

        ImGui::Separator();
        ImGui::Text("Размер кисти:");
        const char* brushSizes[] = {"1x1", "2x2", "3x3"};
        int brushIndex = std::clamp(m_brushSize, 1, 3) - 1;
        if (ImGui::Combo("##BrushSize", &brushIndex, brushSizes, IM_ARRAYSIZE(brushSizes))) {
            m_brushSize = brushIndex + 1;
        }

        ImGui::Separator();
        if (ImGui::Button("Случайная карта", ImVec2(96, 30))) {
            doc.pushUndoSnapshot();
            MapAutoGenerator::generateQuickRandom(doc);
            showToast("Сгенерирована случайная карта!");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Быстрая автогенерация карты в 1 клик");
        }
    }
    ImGui::End();
}

void EditorUI::renderPalettePanel(MapDocument& doc, TextureManager& texManager) {
    ImVec2 mainViewport = ImGui::GetMainViewport()->Size;
    float panelW = 410.0f;
    ImGui::SetNextWindowPos(ImVec2(std::max(158.0f, mainViewport.x - panelW - 8.0f), 30.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, std::max(240.0f, mainViewport.y - 66.0f)), ImGuiCond_Always);

    if (ImGui::Begin("Палитра и настройки##Palettes", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("PaletteTabs")) {
            // Tab 1: Terrain Tiles
            const ImGuiTabItemFlags tileTabFlags = (m_paletteTabToSelect == 0)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Тайлы", nullptr, tileTabFlags)) {
                int tileset = doc.getTilesetId();
                ImGui::Text("Тайлсет: %s (ID %d)", (tileset == 1 ? "Пустыня / Каньон" : "Джунгли / Перу"), tileset);

                ImGui::Text("Выбранный тайл: #%d", m_selectedTile);
                ImGui::SameLine();
                if (ImGui::Button("Залить всё")) {
                    doc.pushUndoSnapshot();
                    doc.fillAll(static_cast<uint8_t>(m_selectedTile));
                }

                ImGui::Separator();
                ImGui::Text("Быстрые категории:");
                if (ImGui::Button("Трава (0)")) m_selectedTile = 0;
                ImGui::SameLine();
                if (ImGui::Button("Луг (10)")) m_selectedTile = 10;
                ImGui::SameLine();
                if (ImGui::Button("Дорога (18)")) m_selectedTile = 18;
                ImGui::SameLine();
                if (ImGui::Button("Земля (26)")) m_selectedTile = 26;
                ImGui::SameLine();
                if (ImGui::Button("Заросли (235)")) m_selectedTile = (tileset == 1 ? 238 : 235);

                ImGui::Separator();
                ImGui::Text("Сетка всех тайлов (красный уголок - спрайт непроходим):");

                ImGui::BeginChild("TileGridScroll", ImVec2(0, 360), true);
                int totalTiles = 245;
                int cols = 5;
                for (int t = 0; t < totalTiles; ++t) {
                    bool selected = (m_selectedTile == t);
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d", t);

                    if (selected) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.4f, 1.0f));
                    }
                    const bool clicked = ImGui::Button((std::string("##Tile") + buf).c_str(), ImVec2(68, 54));
                    const ImVec2 itemMin = ImGui::GetItemRectMin();
                    const ImVec2 itemMax = ImGui::GetItemRectMax();
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->PushClipRect(itemMin, itemMax, true);
                    const int compositeId = texManager.getTileCompositeId(tileset, t);
                    drawCompositeSprite(drawList, texManager, compositeId,
                                        itemMin.x + 17.0f, itemMin.y + 4.0f, 1.0f,
                                        Faction::Resistance);
                    char tileLabel[16];
                    std::snprintf(tileLabel, sizeof(tileLabel), "%d", t);
                    const ImVec2 labelSize = ImGui::CalcTextSize(tileLabel);
                    drawList->AddText(ImVec2(itemMin.x + (68.0f - labelSize.x) * 0.5f,
                                            itemMax.y - labelSize.y - 3.0f),
                                      ImGui::GetColorU32(ImGuiCol_Text), tileLabel);
                    drawList->PopClipRect();
                    if (TileCollision::isBlocked(tileset, t)) {
                        // Corner mark: this sprite blocks movement
                        drawList->AddTriangleFilled(ImVec2(itemMax.x - 14.0f, itemMin.y), ImVec2(itemMax.x, itemMin.y),
                                                    ImVec2(itemMax.x, itemMin.y + 14.0f), IM_COL32(220, 50, 40, 255));
                    }
                    if (selected) {
                        drawList->AddRect(itemMin, itemMax, IM_COL32(255, 220, 80, 255), 0.0f, 0, 3.0f);
                    }
                    if (clicked) {
                        m_selectedTile = t;
                        m_activeTool = EditorTool::TileBrush;
                    }
                    if (selected) {
                        ImGui::PopStyleColor();
                    }

                    if ((t + 1) % cols != 0 && t + 1 < totalTiles) {
                        ImGui::SameLine();
                    }
                }
                ImGui::EndChild();

                ImGui::EndTabItem();
            }

            // Tab 2: Decorations
            const ImGuiTabItemFlags decorTabFlags = (m_paletteTabToSelect == 1)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Декорации", nullptr, decorTabFlags)) {
                ImGui::Text("Аутентичные объекты карты:");
                ImGui::Text("Выбран декор: тип %d", m_selectedDecorType);

                const char* decorNames[] = {
                    "0: Пальма малая", "1: Пальма средняя", "2: Пальма высокая",
                    "3: Джунгли дерево", "4: Тропический куст", "5: Заросли лиан",
                    "6: Валун гранитный", "7: Каменная гряда", "8: Осыпь",
                    "14: Руины инков", "15: Деревянный мост", "16: Каменный мост",
                    "20: Кратер от снаряда", "21: Остов вертолета", "22: Заброшенный бункер",
                    "25: Водный перекат", "28: Тропический цветок", "30: Ствол дерева"
                };

                ImGui::BeginChild("DecorListScroll", ImVec2(0, 360), true);
                for (size_t i = 0; i < sizeof(decorNames) / sizeof(decorNames[0]); ++i) {
                    int dtype = 0;
                    std::sscanf(decorNames[i], "%d", &dtype);
                    bool selected = (m_selectedDecorType == dtype);

                    const std::string itemId = "##Decor" + std::to_string(dtype);
                    if (ImGui::Selectable(itemId.c_str(), selected, 0, ImVec2(0.0f, 48.0f))) {
                        m_selectedDecorType = dtype;
                        m_activeTool = EditorTool::PlaceDecor;
                    }
                    const ImVec2 itemMin = ImGui::GetItemRectMin();
                    const ImVec2 itemMax = ImGui::GetItemRectMax();
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawDecorThumbnail(drawList, texManager, doc.getTilesetId(), dtype, itemMin, itemMax);
                    drawList->AddText(ImVec2(itemMin.x + 54.0f, itemMin.y + 15.0f),
                                      ImGui::GetColorU32(ImGuiCol_Text), decorNames[i]);
                }

                ImGui::Separator();
                ImGui::Text("Любой номер декора (0..95):");
                ImGui::SliderInt("Тип##DecorSlider", &m_selectedDecorType, 0, 95);
                if (ImGui::Button("Выбрать этот декор")) {
                    m_activeTool = EditorTool::PlaceDecor;
                }
                ImGui::EndChild();

                ImGui::EndTabItem();
            }

            // Tab 3: Walls
            const ImGuiTabItemFlags wallTabFlags = (m_paletteTabToSelect == 2)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Стены", nullptr, wallTabFlags)) {
                ImGui::Text("Оборонительные укрепления:");

                int wallFacInt = (m_selectedWallFaction == Faction::Resistance) ? 0 : 1;
                if (ImGui::RadioButton("Сопротивление (Красные 1101)", wallFacInt == 0)) {
                    m_selectedWallFaction = Faction::Resistance;
                    m_activeTool = EditorTool::PlaceWall;
                }
                if (ImGui::RadioButton("Конфедерация (Синие 1001)", wallFacInt == 1)) {
                    m_selectedWallFaction = Faction::Confederation;
                    m_activeTool = EditorTool::PlaceWall;
                }

                for (const auto& wall : {std::pair<const char*, int>{"Стена (Сопротивление)", 25},
                                         std::pair<const char*, int>{"Стена (Конфедерация)", 15}}) {
                    const bool selected = (wall.second == 25 && m_selectedWallFaction == Faction::Resistance) ||
                                          (wall.second == 15 && m_selectedWallFaction == Faction::Confederation);
                    const std::string id = std::string("##Wall") + std::to_string(wall.second);
                    if (ImGui::Selectable(id.c_str(), selected, 0, ImVec2(0.0f, 48.0f))) {
                        m_selectedWallFaction = wall.second == 25 ? Faction::Resistance : Faction::Confederation;
                        m_activeTool = EditorTool::PlaceWall;
                    }
                    const ImVec2 itemMin = ImGui::GetItemRectMin();
                    const ImVec2 itemMax = ImGui::GetItemRectMax();
                    drawBuildingThumbnail(ImGui::GetWindowDrawList(), texManager, wall.second,
                                          wall.second == 25 ? Faction::Resistance : Faction::Confederation,
                                          itemMin, itemMax);
                    ImGui::GetWindowDrawList()->AddText(ImVec2(itemMin.x + 54.0f, itemMin.y + 15.0f),
                                                       ImGui::GetColorU32(ImGuiCol_Text), wall.first);
                }
                if (ImGui::Button("Активировать кисть стен", ImVec2(200, 30))) {
                    m_activeTool = EditorTool::PlaceWall;
                }
                ImGui::EndTabItem();
            }

            // Tab 4: Buildings
            const ImGuiTabItemFlags buildingTabFlags = (m_paletteTabToSelect == 3)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Постройки", nullptr, buildingTabFlags)) {
                ImGui::Text("Фракция постройки:");
                int bFacInt = (m_selectedBuildingFaction == Faction::Resistance) ? 0 : (m_selectedBuildingFaction == Faction::Confederation ? 1 : 2);
                if (ImGui::RadioButton("Сопротивление##BFac", bFacInt == 0)) {
                    m_selectedBuildingFaction = Faction::Resistance;
                    m_selectedBuildingType = 8;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Конфедерация##BFac", bFacInt == 1)) {
                    m_selectedBuildingFaction = Faction::Confederation;
                    m_selectedBuildingType = 0;
                }

                ImGui::Separator();
                struct BuildingDef {
                    int resType;
                    int confType;
                    const char* resName;
                    const char* confName;
                    int resWidth;
                    int resHeight;
                    int confWidth;
                    int confHeight;
                };

                const BuildingDef bdefs[] = {
                    {8, 0, "Штаб-квартира (Сопротивление)", "Штаб-квартира (Конфедерация)", 3, 4, 4, 4},
                    {10, 2, "Казармы (Сопротивление)", "Казармы (Конфедерация)", 3, 3, 3, 3},
                    {11, 3, "Завод (Сопротивление)", "Завод (Конфедерация)", 3, 3, 4, 3},
                    {12, 4, "Лаборатория (Сопротивление)", "Техцентр (Конфедерация)", 3, 3, 3, 3},
                    {14, 7, "Башня (Сопротивление)", "Ракетная турель (Конфедерация)", 2, 2, 2, 2},
                    {13, 5, "Бункер (Сопротивление)", "Бункер (Конфедерация)", 2, 2, 2, 2},
                    {9, 1, "Электростанция (Сопротивление)", "Электростанция (Конфедерация)", 3, 3, 3, 3},
                    {-1, 6, "", "РЛС (Конфедерация)", 0, 0, 2, 2}
                };

                for (const auto& bd : bdefs) {
                    int btype = (m_selectedBuildingFaction == Faction::Resistance) ? bd.resType : bd.confType;
                    if (btype < 0) continue;
                    bool sel = (m_selectedBuildingType == btype && m_activeTool == EditorTool::PlaceBuilding);

                    char label[64];
                    int width = (m_selectedBuildingFaction == Faction::Resistance) ? bd.resWidth : bd.confWidth;
                    int height = (m_selectedBuildingFaction == Faction::Resistance) ? bd.resHeight : bd.confHeight;
                    const std::string itemId = "##Building" + std::to_string(btype);
                    if (ImGui::Selectable(itemId.c_str(), sel, 0, ImVec2(0.0f, 50.0f))) {
                        m_selectedBuildingType = btype;
                        m_activeTool = EditorTool::PlaceBuilding;
                    }
                    const ImVec2 itemMin = ImGui::GetItemRectMin();
                    const ImVec2 itemMax = ImGui::GetItemRectMax();
                    drawBuildingThumbnail(ImGui::GetWindowDrawList(), texManager, btype,
                                          m_selectedBuildingFaction, itemMin, itemMax);
                    std::snprintf(label, sizeof(label), "%s (%dx%d)",
                                  m_selectedBuildingFaction == Faction::Resistance ? bd.resName : bd.confName,
                                  width, height);
                    ImGui::GetWindowDrawList()->AddText(ImVec2(itemMin.x + 54.0f, itemMin.y + 16.0f),
                                                       ImGui::GetColorU32(ImGuiCol_Text), label);
                }
                ImGui::EndTabItem();
            }

            // Tab 5: Units
            const ImGuiTabItemFlags unitTabFlags = (m_paletteTabToSelect == 4)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Юниты", nullptr, unitTabFlags)) {
                ImGui::Text("Фракция юнита:");
                int uFacInt = (m_selectedUnitFaction == Faction::Resistance) ? 0 : 1;
                if (ImGui::RadioButton("Сопротивление##UFac", uFacInt == 0)) {
                    m_selectedUnitFaction = Faction::Resistance;
                    m_selectedUnitArchetype = 0;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Конфедерация##UFac", uFacInt == 1)) {
                    m_selectedUnitFaction = Faction::Confederation;
                    m_selectedUnitArchetype = 1;
                }

                ImGui::Separator();
                struct UnitDef {
                    int resistanceId;
                    int confederationId;
                    const char* resistanceName;
                    const char* confederationName;
                };

                const UnitDef udefs[] = {
                    {0, 1, "Стрелок (Сопротивление)", "Стрелок (Конфедерация)"},
                    {2, 5, "Гренадер (Сопротивление)", "Огнемётчик (Конфедерация)"},
                    {4, 3, "Снайпер (Сопротивление)", "Штурмовик (Конфедерация)"},
                    {6, 7, "Койот (Сопротивление)", "Крепость (Конфедерация)"},
                    {8, 9, "Штурмовой танк (Сопротивление)", "Хаммер (Конфедерация)"},
                    {10, 11, "Тяжёлый танк (Сопротивление)", "Зевс (Конфедерация)"},
                    {12, 13, "РСЗО (Сопротивление)", "Град (Конфедерация)"},
                    {14, -1, "Снайпер в осадном режиме", ""},
                    {15, -1, "Танк в осадном режиме", ""},
                    {16, -1, "РСЗО в осадном режиме", ""}
                };

                for (const auto& ud : udefs) {
                    const bool resistance = m_selectedUnitFaction == Faction::Resistance;
                    const int archetypeId = resistance ? ud.resistanceId : ud.confederationId;
                    if (archetypeId < 0) continue;
                    const bool sel = (m_selectedUnitArchetype == archetypeId && m_activeTool == EditorTool::PlaceUnit);
                    const std::string itemId = "##Unit" + std::to_string(archetypeId);
                    if (ImGui::Selectable(itemId.c_str(), sel, 0, ImVec2(0.0f, 50.0f))) {
                        m_selectedUnitArchetype = archetypeId;
                        m_activeTool = EditorTool::PlaceUnit;
                    }
                    const ImVec2 itemMin = ImGui::GetItemRectMin();
                    const ImVec2 itemMax = ImGui::GetItemRectMax();
                    drawUnitThumbnail(ImGui::GetWindowDrawList(), texManager, archetypeId,
                                      m_selectedUnitFaction, itemMin, itemMax);
                    const char* unitName = resistance ? ud.resistanceName : ud.confederationName;
                    ImGui::GetWindowDrawList()->AddText(ImVec2(itemMin.x + 54.0f, itemMin.y + 17.0f),
                                                       ImGui::GetColorU32(ImGuiCol_Text), unitName);
                }

                ImGui::Separator();
                const int selectedUnits = doc.countSelectedUnits();
                if (selectedUnits > 0) {
                    for (const auto& entity : doc.getEntities()) {
                        if (entity.selected && !entity.isBuilding) {
                            m_selectedUnitFacing = entity.facing;
                            break;
                        }
                    }
                }
                ImGui::Text("Выделено юнитов: %d", selectedUnits);
                ImGui::Text(selectedUnits > 0 ? "Направление выбранных и новых:" : "Направление новых юнитов:");
                const char* dirs[] = {"Восток (Вправо)", "Юго-Восток", "Юг (Вниз)", "Юго-Запад", "Запад (Влево)", "Северо-Запад", "Север (Вверх)", "Северо-Восток"};
                if (ImGui::Combo("Взгляд", &m_selectedUnitFacing, dirs, IM_ARRAYSIZE(dirs)) && doc.countSelectedUnits() > 0) {
                    doc.pushUndoSnapshot();
                    doc.setSelectedUnitFacing(m_selectedUnitFacing);
                }
                ImGui::EndTabItem();
            }

            // Tab 6: Map Properties
            const ImGuiTabItemFlags propertiesTabFlags = (m_paletteTabToSelect == 5)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Свойства", nullptr, propertiesTabFlags)) {
                ImGui::Text("Карта: %s", doc.getName().c_str());
                ImGui::Text("Размер: %d x %d ячеек", doc.getWidth(), doc.getHeight());
                ImGui::Text("Декораций: %zu", doc.getTileMap().getDecorations().size());
                ImGui::Text("Объектов/Юнитов: %zu", doc.getEntities().size());
                ImGui::Text("Старт игрока: (%d, %d)", doc.getPlayerStartX(), doc.getPlayerStartY());
                ImGui::Text("Старт врага: (%d, %d)", doc.getEnemyStartX(), doc.getEnemyStartY());

                ImGui::Separator();
                if (ImGui::Button("Пересчитать проходимость (Auto)", ImVec2(240, 30))) {
                    doc.autoRecalculatePassability();
                    showToast("Сетка проходимости успешно пересчитана!");
                }
                if (ImGui::Button("Сохранить в assets/maps/", ImVec2(240, 30))) {
                    openSaveAsDialog(doc);
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
            m_paletteTabToSelect = -1;
        }
    }
    ImGui::End();
}

void EditorUI::renderMiniMapWindow(MapDocument& doc, Camera& camera) {
    ImVec2 mainViewport = ImGui::GetMainViewport()->Size;
    float mw = 220.0f;
    float mh = 180.0f;
    constexpr float leftPanelWidth = 150.0f;
    constexpr float rightPanelWidth = 410.0f;
    const float centerX = leftPanelWidth + (mainViewport.x - leftPanelWidth - rightPanelWidth - mw) * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(std::max(leftPanelWidth + 8.0f, centerX), mainViewport.y - mh - 40.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Миникарта##MiniMap", &m_showMiniMap, ImGuiWindowFlags_NoScrollbar)) {
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();

        int mapW = doc.getWidth();
        int mapH = doc.getHeight();
        if (mapW > 0 && mapH > 0 && canvasSize.x > 10 && canvasSize.y > 10) {
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // Background
            drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(20, 25, 30, 255));

            float scaleX = canvasSize.x / mapW;
            float scaleY = canvasSize.y / mapH;

            // Draw coarse map terrain & water
            int step = std::max(1, mapW / 60);

            for (int y = 0; y < mapH; y += step) {
                for (int x = 0; x < mapW; x += step) {
                    uint8_t t = doc.getTileMap().getBaseTile(x, y);
                    ImU32 col = TileCollision::isBlocked(doc.getTilesetId(), t) ? IM_COL32(40, 70, 40, 255) :
                                (t >= 26 ? IM_COL32(110, 95, 80, 255) :
                                (t >= 18 ? IM_COL32(150, 125, 90, 255) :
                                (doc.getTilesetId() == 1 ? IM_COL32(190, 160, 105, 255) : IM_COL32(65, 120, 50, 255))));

                    if (doc.getTileMap().getWall(x, y) > 0) {
                        col = IM_COL32(220, 200, 50, 255);
                    }

                    float px = canvasPos.x + x * scaleX;
                    float py = canvasPos.y + y * scaleY;
                    drawList->AddRectFilled(ImVec2(px, py), ImVec2(px + scaleX * step, py + scaleY * step), col);
                }
            }

            // Draw player & enemy base flags
            float ppx = canvasPos.x + doc.getPlayerStartX() * scaleX;
            float ppy = canvasPos.y + doc.getPlayerStartY() * scaleY;
            drawList->AddCircleFilled(ImVec2(ppx, ppy), 4.0f, IM_COL32(240, 40, 40, 255));

            float epx = canvasPos.x + doc.getEnemyStartX() * scaleX;
            float epy = canvasPos.y + doc.getEnemyStartY() * scaleY;
            drawList->AddCircleFilled(ImVec2(epx, epy), 4.0f, IM_COL32(50, 100, 240, 255));

            // Draw camera view rectangle
            float viewW = (camera.getViewportWidth() / TILE_WIDTH) * scaleX / camera.getZoom();
            float viewH = (camera.getViewportHeight() / TILE_HEIGHT) * scaleY / camera.getZoom();
            float viewX = canvasPos.x + (camera.getX() / TILE_WIDTH) * scaleX;
            float viewY = canvasPos.y + (camera.getY() / TILE_HEIGHT) * scaleY;
            drawList->AddRect(ImVec2(viewX, viewY), ImVec2(viewX + viewW, viewY + viewH), IM_COL32(255, 255, 255, 240), 0.0f, 0, 1.5f);

            // Handle minimap click navigation
            if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                ImVec2 mousePos = ImGui::GetMousePos();
                float relX = (mousePos.x - canvasPos.x) / canvasSize.x;
                float relY = (mousePos.y - canvasPos.y) / canvasSize.y;
                float worldTargetX = relX * mapW * TILE_WIDTH;
                float worldTargetY = relY * mapH * TILE_HEIGHT;
                camera.centerOn(worldTargetX, worldTargetY);
            }
        }
    }
    ImGui::End();
}

void EditorUI::renderStatusBar(const MapDocument& doc, const Camera& camera) {
    ImVec2 mainViewport = ImGui::GetMainViewport()->Size;
    float h = 26.0f;
    ImGui::SetNextWindowPos(ImVec2(0, mainViewport.y - h));
    ImGui::SetNextWindowSize(ImVec2(mainViewport.x, h));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##StatusBar", nullptr, flags)) {
        const char* toolNames[] = {"Кисть тайлов", "Заливка", "Декорация", "Стена", "Постройка", "Юнит", "База игрока", "База врага", "Проходимость", "Ластик", "Пипетка", "Выделение"};
        int toolIdx = static_cast<int>(m_activeTool);
        const char* tname = (toolIdx >= 0 && toolIdx < IM_ARRAYSIZE(toolNames)) ? toolNames[toolIdx] : "None";

        if (m_hoverTileX >= 0 && m_hoverTileY >= 0 && m_hoverTileX < doc.getWidth() && m_hoverTileY < doc.getHeight()) {
            uint8_t t = doc.getTileMap().getBaseTile(m_hoverTileX, m_hoverTileY);
            bool pass = doc.getTileMap().isPassable(m_hoverTileX, m_hoverTileY);
            uint16_t w = doc.getTileMap().getWall(m_hoverTileX, m_hoverTileY);

            ImGui::Text("Ячейка: (%d, %d) | Тайл #%d [%s] | Стена: %s | Инструмент: %s | Карта: %dx%d | Зум: %d%%",
                        m_hoverTileX, m_hoverTileY, t, pass ? "Проходимо" : "Блок",
                        w > 0 ? "Да" : "Нет", tname, doc.getWidth(), doc.getHeight(),
                        static_cast<int>(camera.getZoom() * 100.0f));
        } else {
            ImGui::Text("Карта: %dx%d | Инструмент: %s | Зум: %d%% | [ПКМ/СКМ: Панорама, Колесо: Зум]",
                        doc.getWidth(), doc.getHeight(), tname, static_cast<int>(camera.getZoom() * 100.0f));
        }
    }
    ImGui::End();
}

void EditorUI::renderToastNotification() {
    if (m_toastTimer > 0.0f && !m_toastMessage.empty()) {
        m_toastTimer -= ImGui::GetIO().DeltaTime;
        ImVec2 mainViewport = ImGui::GetMainViewport()->Size;

        ImGui::SetNextWindowPos(ImVec2(mainViewport.x * 0.5f - 200, mainViewport.y - 80), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400, 40), ImGuiCond_Always);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.45f, 0.2f, 0.9f));
        if (ImGui::Begin("##Toast", nullptr, flags)) {
            ImGui::TextWrapped("%s", m_toastMessage.c_str());
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }
}

void EditorUI::renderSaveAsModal(MapDocument& doc) {
    if (m_openSaveModalReq) {
        ImGui::OpenPopup("Сохранить карту как... (Save Map As)##SaveModal");
        m_openSaveModalReq = false;
        scanAvailableMaps();
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Сохранить карту как... (Save Map As)##SaveModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Сохранение карты в каталог assets/maps/:");
        ImGui::Separator();

        ImGui::InputText("Имя файла (.json)", m_saveFilename, sizeof(m_saveFilename));
        ImGui::InputText("Название карты", m_saveMapName, sizeof(m_saveMapName));

        ImGui::Text("Существующие карты в папке:");
        ImGui::BeginChild("ExistingMapsSaveList", ImVec2(0, 180), true);
        for (const auto& m : m_availableMaps) {
            if (ImGui::Selectable(m.filename.c_str())) {
                std::snprintf(m_saveFilename, sizeof(m_saveFilename), "%s", m.filename.c_str());
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button("Сохранить карту в assets/maps/", ImVec2(220, 36))) {
            std::string fname = m_saveFilename;
            if (fname.find(".json") == std::string::npos) {
                fname += ".json";
            }
            std::string savePath = OpenArtOfWar::Assets::writablePath("assets/maps/" + fname);
            doc.setName(m_saveMapName);
            if (doc.saveMap(savePath)) {
                showToast("Карта успешно сохранена в " + savePath);
                if (m_closeAfterSave) {
                    m_closeAfterSave = false;
                    m_shouldClose = true;
                }
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(120, 36))) {
            m_closeAfterSave = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void EditorUI::renderOpenModal(MapDocument& doc) {
    if (m_openOpenModalReq) {
        ImGui::OpenPopup("Открыть карту (Open Map)##OpenModal");
        m_openOpenModalReq = false;
        scanAvailableMaps();
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 440), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Открыть карту (Open Map)##OpenModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Выберите карту из каталога assets/maps/:");
        ImGui::InputText("Поиск / Фильтр", m_openSearchFilter, sizeof(m_openSearchFilter));

        ImGui::BeginChild("OpenMapsList", ImVec2(0, 240), true);
        for (size_t i = 0; i < m_availableMaps.size(); ++i) {
            const auto& m = m_availableMaps[i];
            if (m_openSearchFilter[0] != '\0') {
                if (m.filename.find(m_openSearchFilter) == std::string::npos) continue;
            }

            bool selected = (m_selectedMapIndex == static_cast<int>(i));
            char entryLabel[128];
            std::snprintf(entryLabel, sizeof(entryLabel), "%s  [%dx%d, %s, %zu KB]",
                          m.filename.c_str(), m.width, m.height,
                          (m.tilesetId == 1 ? "Пустыня" : "Джунгли"),
                          m.fileSize / 1024);

            if (ImGui::Selectable(entryLabel, selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                m_selectedMapIndex = static_cast<int>(i);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    doc.loadMap(m.fullPath);
                    showToast("Загружена карта: " + m.filename);
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button("Открыть", ImVec2(140, 36))) {
            if (m_selectedMapIndex >= 0 && m_selectedMapIndex < static_cast<int>(m_availableMaps.size())) {
                doc.loadMap(m_availableMaps[m_selectedMapIndex].fullPath);
                showToast("Загружена карта: " + m_availableMaps[m_selectedMapIndex].filename);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(120, 36))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void EditorUI::renderNewMapModal(MapDocument& doc) {
    if (m_openNewModalReq) {
        ImGui::OpenPopup("Создать новую карту (New Map)##NewModal");
        m_openNewModalReq = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Создать новую карту (New Map)##NewModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Параметры новой карты:");
        ImGui::Separator();

        ImGui::SliderInt("Ширина (Width)", &m_newMapWidth, 20, 128);
        ImGui::SliderInt("Высота (Height)", &m_newMapHeight, 20, 128);

        ImGui::Text("Пресеты размеров:");
        if (ImGui::Button("40x40")) { m_newMapWidth = 40; m_newMapHeight = 40; }
        ImGui::SameLine();
        if (ImGui::Button("56x44 (Полигон)")) { m_newMapWidth = 56; m_newMapHeight = 44; }
        ImGui::SameLine();
        if (ImGui::Button("60x40 (Стандарт)")) { m_newMapWidth = 60; m_newMapHeight = 40; }
        ImGui::SameLine();
        if (ImGui::Button("80x60")) { m_newMapWidth = 80; m_newMapHeight = 60; }
        ImGui::SameLine();
        if (ImGui::Button("80x80")) { m_newMapWidth = 80; m_newMapHeight = 80; }

        ImGui::Separator();
        ImGui::Text("Тайлсет ландшафта:");
        ImGui::RadioButton("0: Джунгли / Зеленые равнины Перу", &m_newMapTileset, 0);
        ImGui::RadioButton("1: Пустыня / Песчаный каньон", &m_newMapTileset, 1);

        ImGui::Separator();
        if (ImGui::Button("Создать карту", ImVec2(160, 36))) {
            doc.pushUndoSnapshot();
            doc.newMap(m_newMapWidth, m_newMapHeight, m_newMapTileset, static_cast<uint8_t>(m_newMapFillTile));
            showToast("Создана новая карта " + std::to_string(m_newMapWidth) + "x" + std::to_string(m_newMapHeight));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(120, 36))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void EditorUI::renderGeneratorModal(MapDocument& doc) {
    if (m_openGenModalReq) {
        ImGui::OpenPopup("Автогенератор карт (Map Generator)##GenModal");
        m_openGenModalReq = false;
        m_genParams.width = doc.getWidth();
        m_genParams.height = doc.getHeight();
        m_genParams.tilesetId = doc.getTilesetId();
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Автогенератор карт (Map Generator)##GenModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Процедурная генерация карты с базами и ландшафтом:");
        ImGui::Separator();

        ImGui::SliderInt("Ширина", &m_genParams.width, 30, 128);
        ImGui::SliderInt("Высота", &m_genParams.height, 30, 128);

        const char* modeNames[] = {"Классический (биомы и река)", "Smart (по образцу оригинальных карт)"};
        ImGui::Combo("Генератор", &m_genMode, modeNames, 2);
        const bool smart = m_genMode == 1;

        if (smart) {
            const char* tilesetNames[] = {"Джунгли", "Второй тайлсет"};
            ImGui::Combo("Тайлсет", &m_genParams.tilesetId, tilesetNames, 2);
        } else {
            const char* biomeNames[] = {
                "Зеленые равнины и луга",
                "Речная долина с мостами",
                "Дикие джунгли Перу",
                "Пустынный каньон и дюны",
                "Островной архипелаг"
            };
            int biomeInt = static_cast<int>(m_genParams.biome);
            if (ImGui::Combo("Биом ландшафта", &biomeInt, biomeNames, 5)) {
                m_genParams.biome = static_cast<GeneratorBiome>(biomeInt);
                m_genParams.tilesetId = (m_genParams.biome == GeneratorBiome::DesertCanyon) ? 1 : 0;
            }
        }

        const char* symNames[] = {
            "Естественная асимметрия",
            "Ротационная 180° (Турнирная RTS)",
            "Диагональная",
            "Зеркальная по горизонтали",
            "Зеркальная по вертикали"
        };
        int symInt = static_cast<int>(m_genParams.symmetry);
        if (ImGui::Combo("Симметрия карты", &symInt, symNames, 5)) {
            m_genParams.symmetry = static_cast<GeneratorSymmetry>(symInt);
        }

        if (smart) {
            const char* levelNames[] = {"Мало", "Средне", "Много"};
            ImGui::Combo("Земля между травой", &m_genParams.dirtLevel, levelNames, 3);
            ImGui::Combo("Непроходимые заросли", &m_genParams.obstacleLevel, levelNames, 3);
            ImGui::Checkbox("Лесная стена по краю карты", &m_genParams.borderForest);
        } else {
            ImGui::Checkbox("Расчистить стартовые площадки игроков", &m_genParams.generateBases);
            ImGui::Checkbox("Добавить извилистую реку и мосты", &m_genParams.addRiver);
            if (m_genParams.addRiver) {
                ImGui::SliderInt("Количество мостов", &m_genParams.riverBridges, 1, 4);
            }
        }

        const char* densityNames[] = {"Низкая (редкие деревья)", "Средняя (сбалансированная)", "Высокая (густые рощи)"};
        ImGui::Combo("Плотность декораций", &m_genParams.decorDensity, densityNames, 3);

        ImGui::Checkbox("Случайный Seed", &m_genParams.randomSeed);
        if (!m_genParams.randomSeed) {
            int seedInt = static_cast<int>(m_genParams.seed);
            if (ImGui::InputInt("Seed", &seedInt)) {
                m_genParams.seed = static_cast<uint32_t>(seedInt);
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Сгенерировать карту", ImVec2(220, 36))) {
            doc.pushUndoSnapshot();
            if (smart) {
                SmartGenerator::generate(doc, m_genParams);
            } else {
                MapAutoGenerator::generate(doc, m_genParams);
            }
            showToast("Карта успешно сгенерирована!");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(120, 36))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void EditorUI::renderCloseConfirmation(MapDocument& doc) {
    if (m_openCloseConfirmationReq) {
        ImGui::OpenPopup("Несохранённые изменения##CloseConfirmation");
        m_openCloseConfirmationReq = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Несохранённые изменения##CloseConfirmation", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("В карте есть несохранённые изменения. Сохранить их перед закрытием?");
        ImGui::Separator();
        if (ImGui::Button("Сохранить", ImVec2(140, 36))) {
            if (doc.getFilePath().empty()) {
                m_closeAfterSave = true;
                openSaveAsDialog(doc);
                ImGui::CloseCurrentPopup();
            } else if (doc.saveMap(doc.getFilePath())) {
                m_shouldClose = true;
                ImGui::CloseCurrentPopup();
            } else {
                showToast("Не удалось сохранить карту");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Не сохранять", ImVec2(140, 36))) {
            m_shouldClose = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(120, 36))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace OpenArtOfWar
