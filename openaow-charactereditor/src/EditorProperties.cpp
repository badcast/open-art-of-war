#include "CharacterEditor.hpp"
#include "game/Projectiles.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace CharEditor {

void CharacterEditor::DrawEditor() {
    ImGuiIO& io = ImGui::GetIO();
    const float fullWidth = io.DisplaySize.x;
    const float fullHeight = io.DisplaySize.y;
    constexpr float top = 24.0f;
    const float panelHeight = std::max(70.0f, fullHeight - top);

    constexpr float kMinPanelWidth = 70.0f;
    leftPanelWidth = std::clamp(leftPanelWidth, kMinPanelWidth, std::max(kMinPanelWidth, fullWidth - rightPanelWidth - kMinPanelWidth));
    rightPanelWidth = std::clamp(rightPanelWidth, kMinPanelWidth, std::max(kMinPanelWidth, fullWidth - leftPanelWidth - kMinPanelWidth));

    float leftWidth = leftPanelWidth;
    float rightWidth = rightPanelWidth;
    const float minRightWidth = kMinPanelWidth;
    float maxRightWidth = std::max(minRightWidth, fullWidth - leftWidth - kMinPanelWidth);

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(_("File"))) {
            if (ImGui::MenuItem(_("Save"), "Ctrl+S", false, document.IsDefined())) SaveFile();
            if (ImGui::MenuItem(_("Load selected"), nullptr, false, !files.empty())) {
                if (selectedIndex >= 0 && selectedIndex < static_cast<int>(files.size())) {
                    loadFile(files[static_cast<size_t>(selectedIndex)].path);
                }
            }
            if (ImGui::MenuItem(_("Refresh list"))) LoadCatalog();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(_("Language"))) {
            const bool russian = OpenArtOfWar::Localization::GetLanguage() == "ru";
            if (ImGui::MenuItem(_("English"), nullptr, !russian)) {
                OpenArtOfWar::Localization::SetLanguage("en");
            }
            if (ImGui::MenuItem(_("Russian"), nullptr, russian)) {
                OpenArtOfWar::Localization::SetLanguage("ru");
            }
            ImGui::EndMenu();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status.c_str());
        ImGui::EndMainMenuBar();
    }

    ImGui::SetNextWindowPos(ImVec2(0.0f, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(leftWidth, panelHeight), ImGuiCond_Always);
    ImGui::Begin(_("Archetypes"), nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::Text(_("%zu configurations"), files.size());
    ImGui::SetNextItemWidth(-1.0f);
    Hinted(ImGui::InputTextWithHint("##search", _("Search by name, ID or faction"), searchBuffer.data(), searchBuffer.size()),
           "Search by name, ID or faction");

    static int filterCategory = 0; // 0=All, 1=Units, 2=Buildings
    Hinted(ImGui::RadioButton(_("All"), &filterCategory, 0), "All");
    ImGui::SameLine();
    Hinted(ImGui::RadioButton(_("Units"), &filterCategory, 1), "Units");
    ImGui::SameLine();
    Hinted(ImGui::RadioButton(_("Buildings"), &filterCategory, 2), "Buildings");

    ImGui::BeginChild("archetype-list", ImVec2(0.0f, 0.0f), true);

    auto matchesSearch = [&](const CharacterFile& f) -> bool {
        if (searchBuffer[0] == '\0') return true;
        std::string needle(searchBuffer.data());
        std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
        std::string haystack = f.name + " " + f.stringId + " " + f.faction + " " +
                               std::to_string(f.numericId) + " " + f.path.stem().string();
        std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
        return haystack.find(needle) != std::string::npos;
    };

    int itemCounter = 0;
    auto renderItem = [&](const CharacterFile& file) {
        ImGui::PushID(itemCounter++);
        const bool selected = (file.path == activePath);
        char idTag[32];
        if (file.type == "building") {
            std::snprintf(idTag, sizeof(idTag), "[B#%d]", file.numericId);
        } else {
            std::snprintf(idTag, sizeof(idTag), "[#%d]", file.numericId);
        }
        std::string label = std::string(idTag) + " " + file.name;
        if (ImGui::Selectable(label.c_str(), selected)) {
            loadFile(file.path);
        }
        ImGui::TextDisabled("  %s (%s)", file.stringId.c_str(), file.faction.c_str());
        ImGui::PopID();
        ImGui::Separator();
    };

    struct FactionGroup {
        std::string title;
        std::vector<CharacterFile> units;
        std::vector<CharacterFile> buildings;
    };
    FactionGroup confGroup{_("Confederation"), {}, {}};
    FactionGroup resGroup{_("Resistance"), {}, {}};
    FactionGroup otherGroup{_("Other"), {}, {}};

    for (const auto& file : files) {
        if (!matchesSearch(file)) continue;
        if (filterCategory == 1 && file.type != "unit") continue;
        if (filterCategory == 2 && file.type != "building") continue;

        FactionGroup* target = &otherGroup;
        const OpenArtOfWar::Faction fileFaction = OpenArtOfWar::Utility::ParseFaction(file.faction, OpenArtOfWar::Faction::Neutral);
        if (fileFaction == OpenArtOfWar::Faction::Confederation) {
            target = &confGroup;
        } else if (fileFaction == OpenArtOfWar::Faction::Resistance) {
            target = &resGroup;
        }

        if (file.type == "building") {
            target->buildings.push_back(file);
        } else {
            target->units.push_back(file);
        }
    }

    auto renderGroup = [&](FactionGroup& group) {
        if (group.units.empty() && group.buildings.empty()) return;
        size_t totalCount = group.units.size() + group.buildings.size();
        std::string headerLabel = group.title + " (" + std::to_string(totalCount) + ")";
        if (ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!group.units.empty()) {
                std::string uHeader = std::string(_("Units")) + " (" + std::to_string(group.units.size()) + ")";
                if (ImGui::TreeNodeEx(uHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (const auto& f : group.units) {
                        renderItem(f);
                    }
                    ImGui::TreePop();
                }
            }
            if (!group.buildings.empty()) {
                std::string bHeader = std::string(_("Buildings")) + " (" + std::to_string(group.buildings.size()) + ")";
                if (ImGui::TreeNodeEx(bHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (const auto& f : group.buildings) {
                        renderItem(f);
                    }
                    ImGui::TreePop();
                }
            }
        }
    };

    renderGroup(confGroup);
    renderGroup(resGroup);
    renderGroup(otherGroup);

    ImGui::EndChild();
    ImGui::End();

    const float splitterThickness = 8.0f;

    // Draggable vertical splitter to resize Archetypes tab panel (left)
    const float splitterLeftX = leftWidth - splitterThickness * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(splitterLeftX, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(splitterThickness, panelHeight), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##vsplitter_left", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::InvisibleButton("##split_left_btn", ImVec2(splitterThickness, panelHeight));
    const bool isLeftSplitHovered = ImGui::IsItemHovered();
    const bool isLeftSplitActive = ImGui::IsItemActive();
    if (isLeftSplitHovered || isLeftSplitActive) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (isLeftSplitActive) {
        leftPanelWidth += io.MouseDelta.x;
        leftPanelWidth = std::clamp(leftPanelWidth, kMinPanelWidth, std::max(kMinPanelWidth, fullWidth - rightPanelWidth - kMinPanelWidth));
        leftWidth = leftPanelWidth;
        maxRightWidth = std::max(minRightWidth, fullWidth - leftWidth - kMinPanelWidth);
    }
    if (isLeftSplitHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        leftPanelWidth = std::clamp(fullWidth * 0.22f, kMinPanelWidth, 320.0f);
        leftWidth = leftPanelWidth;
        maxRightWidth = std::max(minRightWidth, fullWidth - leftWidth - kMinPanelWidth);
    }
    if (isLeftSplitHovered || isLeftSplitActive) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pMin = ImGui::GetItemRectMin();
        ImVec2 pMax = ImGui::GetItemRectMax();
        dl->AddRectFilled(pMin, pMax, isLeftSplitActive ? IM_COL32(70, 190, 160, 220) : IM_COL32(55, 140, 115, 160));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::SetNextWindowPos(ImVec2(fullWidth - rightWidth, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(rightWidth, panelHeight), ImGuiCond_Always);
    ImGui::Begin(_("Properties"), nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (editorMode == EditorMode::Research) DrawResearchProperties();
    else if (editorMode == EditorMode::SpriteTools) DrawSpriteProperties();
    else if (editorMode == EditorMode::Explosions) DrawExplosionProperties();
    else if (editorMode == EditorMode::Wrecks) DrawWreckTabProperties();
    else if (editorMode == EditorMode::Projectiles) DrawProjectileProperties();
    else if (editorMode == EditorMode::GameUi) DrawGameUiProperties();
    else if (editorMode == EditorMode::Environment) DrawEnvironmentProperties();
    else if (editorMode == EditorMode::AiDifficulty) DrawAiProperties();
    else if (editorMode == EditorMode::Animator) DrawAnimatorProperties();
    else if (document.IsDefined()) DrawProperties();
    else ImGui::TextWrapped(_("Select a YAML configuration from the list."));
    ImGui::End();

    // Draggable vertical splitter to resize Properties tab panel (right)
    const float splitterRightX = fullWidth - rightWidth - splitterThickness * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(splitterRightX, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(splitterThickness, panelHeight), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##vsplitter_right", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::InvisibleButton("##split_h", ImVec2(splitterThickness, panelHeight));
    const bool isSplitterHovered = ImGui::IsItemHovered();
    const bool isSplitterActive = ImGui::IsItemActive();
    if (isSplitterHovered || isSplitterActive) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (isSplitterActive) {
        rightPanelWidth -= io.MouseDelta.x;
        rightPanelWidth = std::clamp(rightPanelWidth, minRightWidth, maxRightWidth);
        rightWidth = rightPanelWidth;
    }
    if (isSplitterHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        rightPanelWidth = std::clamp(fullWidth * 0.35f, kMinPanelWidth, 520.0f);
        rightWidth = rightPanelWidth;
    }
    if (isSplitterHovered || isSplitterActive) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pMin = ImGui::GetItemRectMin();
        ImVec2 pMax = ImGui::GetItemRectMax();
        dl->AddRectFilled(pMin, pMax, isSplitterActive ? IM_COL32(70, 190, 160, 220) : IM_COL32(55, 140, 115, 160));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    const float centerWidth = std::max(kMinPanelWidth, fullWidth - leftWidth - rightWidth);
    ImGui::SetNextWindowPos(ImVec2(leftWidth, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(centerWidth, panelHeight), ImGuiCond_Always);
    ImGui::Begin(_("Preview"), nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    DrawPreviewControls();
    if (editorMode == EditorMode::Research || editorMode == EditorMode::SpriteTools || editorMode == EditorMode::GameUi ||
        editorMode == EditorMode::Environment || editorMode == EditorMode::AiDifficulty) {
        previewRect = {0, 0, 0, 0};
        if (editorMode == EditorMode::Research) DrawResearchTree();
        else if (editorMode == EditorMode::GameUi) DrawGameUi();
        else if (editorMode == EditorMode::Environment) DrawEnvironment();
        else if (editorMode == EditorMode::AiDifficulty) DrawAiDifficulty();
        else DrawSpriteTools();
        ImGui::End();
    } else {
    ImGui::BeginChild("preview-canvas", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoMove);
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    previewRect = {static_cast<int>(canvasMin.x), static_cast<int>(canvasMin.y),
                   std::max(1, static_cast<int>(canvasSize.x)),
                   std::max(1, static_cast<int>(canvasSize.y))};
    DrawCanvasGuides(canvasMin, canvasSize);

    ImGui::InvisibleButton("canvas-interaction", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool canvasActive = ImGui::IsItemActive();
    const bool canvasHovered = ImGui::IsItemHovered();

    // 1. Panning canvas with MMB (Middle Mouse Button) or Space + Drag
    if (canvasActive && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f) ||
                         (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)))) {
        const ImVec2 delta = io.MouseDelta;
        cameraPanX -= delta.x / previewZoom;
        cameraPanY -= delta.y / previewZoom;
    }

    // 2. Right Mouse Button (RMB) -> Turn Unit Hull toward point (step-by-step)
    else if (editorMode != EditorMode::Simulation && canvasActive && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        const ImVec2 mousePos = io.MousePos;
        float mWorldX = 0.0f, mWorldY = 0.0f;
        camera->ScreenToWorld(mousePos.x, mousePos.y, mWorldX, mWorldY);

        float dx = mWorldX;
        float dy = mWorldY;
        int dir = OpenArtOfWar::Utility::Dir8FromVector(dx, dy);
        desiredHullFacingDir = (dir % 8 + 8) % 8;

        const bool hasRotTurret = YamlBool(document["combat"], "has_rotating_turret", false);
        if (!hasRotTurret || linkTurretToHull) {
            desiredTurretFacingDir = desiredHullFacingDir;
            targetWorldX = mWorldX;
            targetWorldY = mWorldY;
            targetCustomized = true;
        }
    }

    // 3. Left Mouse Button (LMB) -> Turn Turret / Aim (step-by-step)
    else if (editorMode != EditorMode::Simulation && canvasActive && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
        const ImVec2 mousePos = io.MousePos;
        camera->ScreenToWorld(mousePos.x, mousePos.y, targetWorldX, targetWorldY);
        targetCustomized = true;

        const bool hasRotTurret = YamlBool(document["combat"], "has_rotating_turret", false);
        float tPivotX = hasRotTurret ? turretOffsetX : 0.0f;
        float tPivotY = hasRotTurret ? turretOffsetY : 0.0f;
        float dx = targetWorldX - tPivotX;
        float dy = targetWorldY - tPivotY;
        int dir = OpenArtOfWar::Utility::Dir8FromVector(dx, dy);
        desiredTurretFacingDir = (dir % 8 + 8) % 8;
        if (!hasRotTurret) {
            desiredHullFacingDir = desiredTurretFacingDir;
        }
    }

    // 4. Double click MMB to reset view
    if (canvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
        if (editorMode == EditorMode::Simulation && battle.Active()) {
            FrameBattleCamera();
        } else {
            cameraPanX = 0.0f;
            cameraPanY = 0.0f;
            previewZoom = 2.0f;
        }
    }

    ImGui::EndChild();
    ImGui::End();
    }
}

bool CharacterEditor::DrawIdentity(bool isBuilding) {
    bool changed = false;
    if (isBuilding) {
        const int current = YamlInt(document, "building_index", -1);
        const auto* kind = OpenArtOfWar::Utility::buildingKindInfo(current);
        std::string preview = kind ? ("#" + std::to_string(current) + " " + _(kind->name) + " - " +
                                      _(OpenArtOfWar::Utility::FactionName(kind->faction)))
                                   : std::string(_("(unknown)"));
        if (Hinted(ImGui::BeginCombo(_("Building"), preview.c_str()), "Building")) {
            for (const auto& k : OpenArtOfWar::Utility::BuildingKinds()) {
                const std::string label = "#" + std::to_string(k.index) + " " + _(k.name) + " - " +
                                          _(OpenArtOfWar::Utility::FactionName(k.faction));
                if (ImGui::Selectable(label.c_str(), k.index == current)) {
                    document["building_index"] = k.index;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (kind) {
            ImGui::TextDisabled(_("Faction: %s   Structure type: %s"), _(OpenArtOfWar::Utility::FactionName(kind->faction)),
                                OpenArtOfWar::Utility::UnitTypeName(kind->type));
        }
        return changed;
    }

    const int current = YamlInt(document, "archetype_id", -1);
    const auto* arch = OpenArtOfWar::Utility::archetypeInfo(current);
    auto label = [](const OpenArtOfWar::Utility::ArchetypeInfo& a) {
        return "#" + std::to_string(a.id) + " " + _(a.name) + " - " + _(OpenArtOfWar::Utility::FactionName(a.faction));
    };
    if (Hinted(ImGui::BeginCombo(_("Archetype"), arch ? label(*arch).c_str() : _("(unknown)")), "Archetype")) {
        for (int i = 0; i < OpenArtOfWar::Utility::kArchetypeCount; ++i) {
            const auto* a = OpenArtOfWar::Utility::archetypeInfo(i);
            if (a->baseFormId >= 0) continue; // siege / baseline forms are set up under Abilities
            if (ImGui::Selectable(label(*a).c_str(), a->id == current)) {
                document["archetype_id"] = a->id;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (arch) {
        ImGui::TextDisabled(_("Faction: %s   Class: %s   Internal type: %s"), _(OpenArtOfWar::Utility::FactionName(arch->faction)),
                            _(OpenArtOfWar::Utility::UnitClassName(arch->id)), OpenArtOfWar::Utility::UnitTypeName(arch->type));
    }
    return changed;
}

bool CharacterEditor::DrawIconPicker(YAML::Node production) {
    bool changed = false;
    const int current = YamlInt(production, "icon_composite", 0);
    ImGui::Text(_("Build menu icon: %d"), current);
    const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 74.0f));
    int n = 0;
    for (int comp = 2064; comp <= 2081; ++comp) {
        if (n++ % perRow != 0) ImGui::SameLine();
        ImGui::PushID(comp);
        const bool selected = (comp == current);
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
        SDL_Texture* thumb = CompositeThumbnail(comp, false);
        const bool clicked = thumb ? ImGui::ImageButton("icon", reinterpret_cast<ImTextureID>(thumb), ImVec2(64, 42))
                                   : ImGui::Button(std::to_string(comp).c_str(), ImVec2(64, 42));
        if (selected) ImGui::PopStyleColor();
        if (clicked) {
            production["icon_composite"] = comp;
            changed = true;
        }
        ImGui::PopID();
    }
    return changed;
}

bool CharacterEditor::DrawWreckProperties(bool isBuilding) {
    bool changed = false;
    YAML::Node animations = Section("animations");
    const bool infantry = !isBuilding && OpenArtOfWar::Utility::IsInfantryArchetype(YamlInt(document, "archetype_id", 0));
    if (infantry) {
        ImGui::TextWrapped("%s", _("Infantry leaves a corpse instead of a wreck: the last frame of its death sequence "
                                   "(death_sequences / flame_death_sequences in Animation)."));
        if (Hinted(ImGui::Button(_("Preview corpse")), "Preview corpse")) SetMode(PreviewMode::Wreck);
        return false;
    }

    int composite = 0, stages = 0;
    WreckOf(document, composite, stages);
    const bool custom = animations["wreck_composite"].IsDefined();
    ImGui::Text(_("Wreck composite %d, %d stage(s)%s"), composite, stages, custom ? "" : _(" (default)"));

    // Family list with pictures; click to use it
    const float cell = 104.0f;
    const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
    int n = 0;
    for (const WreckFamily& f : kWreckFamilies) {
        if (n++ % perRow != 0) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushID(f.composite);
        const bool current = (f.composite == composite);
        if (current) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
        SDL_Texture* thumb = WreckThumbnail(f.composite);
        const bool clicked = thumb ? ImGui::ImageButton("thumb", reinterpret_cast<ImTextureID>(thumb), ImVec2(96, 64))
                                   : ImGui::Button("?", ImVec2(96, 64));
        if (current) ImGui::PopStyleColor();
        if (clicked) {
            animations["wreck_composite"] = f.composite;
            animations["wreck_stages"] = f.stages;
            SetMode(PreviewMode::Wreck);
            changed = true;
        }
        ImGui::TextDisabled("%s", _(f.label));
        ImGui::PopID();
        ImGui::EndGroup();
    }

    int st = stages;
    ImGui::SetNextItemWidth(120.0f);
    if (Hinted(ImGui::SliderInt(_("Stages"), &st, 1, 8), "Stages")) {
        animations["wreck_stages"] = st;
        changed = true;
    }
    ImGui::SetNextItemWidth(160.0f);
    Hinted(ImGui::SliderInt(_("Preview stage (-1 = play)"), &wreckPreviewStage, -1, stages - 1), "Preview stage (-1 = play)");
    if (Hinted(ImGui::Button(_("Preview wreck")), "Preview wreck")) SetMode(PreviewMode::Wreck);
    ImGui::SameLine();
    if (custom && Hinted(ImGui::Button(_("Use default wreck")), "Use default wreck")) {
        animations.remove("wreck_composite");
        animations.remove("wreck_stages");
        changed = true;
    }
    ImGui::TextDisabled("%s", _("In the game the wreck stays 120 s and sinks one stage at a time."));
    return changed;
}

void CharacterEditor::DrawProperties() {
    if (dirty) ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f), _("Unsaved changes"));

    // Panel Width quick controls
    ImGui::SetNextItemWidth(75.0f);
    if (Hinted(ImGui::DragFloat(_("Width##rpanel"), &rightPanelWidth, 2.0f, 70.0f, 900.0f, "%.0f px"), "Width##rpanel")) {
        rightPanelWidth = std::clamp(rightPanelWidth, 70.0f, 900.0f);
    }
    ImGui::SameLine();
    if (Hinted(ImGui::SmallButton(_("Min")), "Min")) {
        rightPanelWidth = 70.0f;
    }
    ImGui::SameLine();
    if (Hinted(ImGui::SmallButton(_("Compact")), "Compact")) {
        rightPanelWidth = 360.0f;
    }
    ImGui::SameLine();
    if (Hinted(ImGui::SmallButton(_("Standard")), "Standard")) {
        rightPanelWidth = 470.0f;
    }
    ImGui::SameLine();
    if (Hinted(ImGui::SmallButton(_("Wide")), "Wide")) {
        rightPanelWidth = 620.0f;
    }
    ImGui::Separator();

    ImGui::TextWrapped("%s", activePath.string().c_str());
    bool changed = false;

    const bool isBuilding = (YamlString(document, "type") == "building");

    if (Hinted(ImGui::CollapsingHeader(_("General"), ImGuiTreeNodeFlags_DefaultOpen), "General")) {
        changed |= EditString(document, "id", "ID", "root");
        changed |= EditString(document, "name", "Name", "root");
        // Description: the translation of "<id>.description" in po/*.po, not a field of this file
        {
            const std::string key = OpenArtOfWar::EntityManager::DescriptionKey(YamlString(document, "id"));
            const std::string text = _(key);
            ImGui::TextDisabled(_("Description (po key %s):"), key.c_str());
            ImGui::TextWrapped("%s", text == key ? _("(no translation yet - add it to po/en.po and po/ru.po)") : text.c_str());
        }
        changed |= DrawIdentity(isBuilding);
    }

    if (Hinted(ImGui::CollapsingHeader(_("Size and stats"), ImGuiTreeNodeFlags_DefaultOpen), "Size and stats")) {
        if (isBuilding) {
            YAML::Node size = Section("size");
            changed |= EditInt(size, "width", "Width (tiles)", 1);
            changed |= EditInt(size, "height", "Height (tiles)", 1);
        }
        YAML::Node stats = Section("stats");
        changed |= EditInt(stats, "max_hp", "Maximum health", 100);
        changed |= EditInt(stats, "sight_radius", "Sight radius (tiles)", 5);
        if (isBuilding) {
            const std::string buildingId = YamlString(document, "id");
            if (stats["build_radius"] || buildingId.find("headquarters") != std::string::npos
                || buildingId.find("command_center") != std::string::npos) {
                changed |= EditInt(stats, "build_radius", "Build radius (tiles)", 16);
            }
            if (stats["radar_range"] || buildingId.find("radar") != std::string::npos) {
                changed |= EditInt(stats, "radar_range", "Radar range (tiles)", 15);
            }
        }
        if (!isBuilding) {
            changed |= EditFloat(stats, "move_speed", "Move speed (pixels per tick)", 1.4f);
        }
    }

    if (Hinted(ImGui::CollapsingHeader(_("Production")), "Production")) {
        YAML::Node production = Section("production");
        // Legacy price keys are folded into the single "cost" field
        for (const char* legacyKey : {"credit", "credit_cost"}) {
            if (production[legacyKey]) {
                if (!production["cost"]) production["cost"] = production[legacyKey];
                production.remove(legacyKey);
                changed = true;
            }
        }
        changed |= EditInt(production, "cost", "Cost (credits)", 100);
        if (isBuilding) {
            changed |= EditInt(production, "power", "Power (+/-)", 0);
        }
        changed |= EditFloat(production, "build_time", "Build time", 4.0f);
        if (!isBuilding) {
            ImGui::TextDisabled(_("Produced in: %s (follows from the archetype)"),
                                _(OpenArtOfWar::Utility::ProducedIn(YamlInt(document, "archetype_id", 0))));
        } else {
            changed |= DrawIconPicker(production);
        }
    }

    if (Hinted(ImGui::CollapsingHeader(_("Combat"), ImGuiTreeNodeFlags_DefaultOpen), "Combat")) {
        YAML::Node combat = Section("combat");
        if (isBuilding) {
            changed |= EditBool(combat, "can_attack", "Can attack", false);
        }
        const bool canAttack = !isBuilding || YamlBool(combat, "can_attack", false);

        if (!canAttack) {
            ImGui::TextDisabled("%s", _("Combat is disabled for this archetype."));
        }

        // Damage, range, delay, projectile and muzzle belong to each weapon (Weapons below);
        // combat itself only holds what is shared by all guns
        if (EditFloat(combat, "bullet_speed", "Bullet / Projectile Speed (px/s)", previewBulletSpeed, 10.0f)) {
            previewBulletSpeed = YamlFloat(combat, "bullet_speed", previewBulletSpeed);
            changed = true;
        }
        if (EditFloat(combat, "fx_speed", "Special Effects / FX Speed Multiplier", previewFxSpeed, 0.05f)) {
            previewFxSpeed = YamlFloat(combat, "fx_speed", previewFxSpeed);
            changed = true;
        }
        float curSpread = YamlFloat(combat, "spread_angle", -1.0f);
        if (curSpread < 0.0f) {
            std::string ptype = YamlString(MainWeapon(combat), "projectile", "bullet");
            // The default of the projectile kind (Projectiles tab)
            curSpread = ptype == "none" ? 3.0f
                                        : OpenArtOfWar::Projectiles::of(OpenArtOfWar::Projectiles::FromKey(ptype)).spread * 180.0f /
                                              static_cast<float>(M_PI);
        }
        if (Hinted(ImGui::DragFloat(_("Shot Spread (deg, 0=pinpoint)"), &curSpread, 0.1f, 0.0f, 15.0f, "%.1f deg"), "Shot Spread (deg, 0=pinpoint)")) {
            combat["spread_angle"] = curSpread;
            changed = true;
        }
        if (curSpread <= 0.001f) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%s", _("Shot spread: 0.0 (High Precision Pinpoint Accuracy)"));
        }
        changed |= EditBool(combat, "has_rotating_turret", "Rotating turret");
        if (isBuilding) {
            changed |= EditBool(combat, "has_rotating_radar", "Rotating radar");
        }

        ImGui::Spacing();
        const bool hasRotTurret = YamlBool(combat, "has_rotating_turret", false);
        if (hasRotTurret) {
            float domePos[2] = {turretOffsetX, turretOffsetY};
            if (Hinted(ImGui::DragFloat2(_("Dome Offset (X, Y)"), domePos, 0.5f, -100.0f, 100.0f, "%.1f px"), "Dome Offset (X, Y)")) {
                turretOffsetX = domePos[0];
                turretOffsetY = domePos[1];
                hasCustomTurretOffset = true;
                YAML::Node seq(YAML::NodeType::Sequence);
                seq.push_back(turretOffsetX);
                seq.push_back(turretOffsetY);
                combat["turret_offset"] = seq;
                changed = true;
            }
            ImGui::SameLine();
            if (Hinted(ImGui::Button(_("Default Dome")), "Default Dome")) {
                const int aId = isBuilding ? YamlInt(document, "building_index", -1) : YamlInt(document, "archetype_id", -1);
                float defTx = 0.0f, defTy = 0.0f;
                GetDefaultTurretOffset(aId, isBuilding, defTx, defTy);
                turretOffsetX = defTx;
                turretOffsetY = defTy;
                hasCustomTurretOffset = false;
                combat.remove("turret_offset");
                combat.remove("dome_offset");
                combat.remove("turret_pivot");
                changed = true;
            }
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", _("Dome Offset: Turret rotation center relative to hull center [X, Y]."));
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%s", _("localePosition: Projectile exit point attached to rotating turret: [X=Forward, Y=Lateral]."));
            if (!isBuilding) {
                const int arch = YamlInt(document, "archetype_id", 0);
                const float defTDelay = document["turn_step_delay"].IsDefined()
                    ? YamlFloat(document, "turn_step_delay", OpenArtOfWar::Utility::DefaultTurretTurnStepDelay(arch))
                    : OpenArtOfWar::Utility::DefaultTurretTurnStepDelay(arch);
                changed |= EditFloat(combat, "turret_turn_step_delay", "Turret turn delay per 45 deg (s)", defTDelay, 0.01f);
            }
        } else {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%s", _("localePosition: Projectile exit point: [X=Forward along facing, Y=Lateral]."));
        }

        // Weapons Array (Multiple Gun Types)
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", _("Weapons System (Multiple Gun Types):"));

        YAML::Node weapons = combat["weapons"];
        if (weapons && weapons.IsSequence() && weapons.size() > 0) {
            for (size_t wi = 0; wi < weapons.size(); ++wi) {
                YAML::Node w = weapons[wi];
                if (!w.IsMap()) continue;
                std::string wTitle = "Weapon " + std::to_string(wi + 1) + ": " + YamlString(w, "name", wi == 0 ? "Primary" : "Secondary");
                ImGui::PushID(static_cast<int>(wi));
                if (ImGui::TreeNodeEx(wTitle.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    bool wEnabled = YamlBool(w, "enabled", true);
                    if (Hinted(ImGui::Checkbox(_("Enabled (Tested in Combat)"), &wEnabled), "Enabled (Tested in Combat)")) {
                        w["enabled"] = wEnabled;
                        changed = true;
                    }
                    if (wi > 0) {
                        ImGui::SameLine();
                        if (Hinted(ImGui::Button(_("Remove Weapon")), "Remove Weapon")) {
                            YAML::Node newWeapons(YAML::NodeType::Sequence);
                            for (size_t k = 0; k < weapons.size(); ++k) {
                                if (k != wi) newWeapons.push_back(weapons[k]);
                            }
                            combat["weapons"] = newWeapons;
                            changed = true;
                            ImGui::TreePop();
                            ImGui::PopID();
                            break;
                        }
                    }

                    changed |= EditString(w, "name", "Weapon Name", "combat/w" + std::to_string(wi));
                    changed |= EditProjectileCombo(w);
                    changed |= EditInt(w, "damage", "Damage per shot", 10);
                    changed |= EditFloat(w, "range", "Attack range (tiles)", 4.0f);
                    changed |= EditFloat(w, "min_range", "Minimum range (tiles)", 0.0f);
                    changed |= EditWeaponDelay(w);
                    changed |= EditInt(w, "burst_count", "Burst count", 1);
                    changed |= EditFloat(w, "burst_delay", "Burst delay (seconds)", 0.1f);
                    changed |= EditInt(w, "splash_percent", "Splash Damage (%) [2x2 tile area]", 0);

                    float wLocPos[2] = {0.0f, 0.0f};
                    if (w["localePosition"] && w["localePosition"].IsSequence() && w["localePosition"].size() >= 2) {
                        try {
                            wLocPos[0] = w["localePosition"][0].as<float>(0.0f);
                            wLocPos[1] = w["localePosition"][1].as<float>(0.0f);
                        } catch (...) {}
                    }
                    if (Hinted(ImGui::DragFloat2(_("Weapon Muzzle (X, Y)"), wLocPos, 0.5f, -100.0f, 100.0f, "%.1f px"), "Weapon Muzzle (X, Y)")) {
                        YAML::Node seq(YAML::NodeType::Sequence);
                        seq.push_back(wLocPos[0]);
                        seq.push_back(wLocPos[1]);
                        w["localePosition"] = seq;
                        if (wi == 0) { // the main gun's muzzle is the one the preview fires from
                            muzzleOffsetX = wLocPos[0];
                            muzzleOffsetY = wLocPos[1];
                        }
                        changed = true;
                    }
                    ImGui::TextDisabled("%s", hasRotTurret ? _("Muzzle: exit point on the rotating turret [X=Forward, Y=Lateral].")
                                                           : _("Muzzle: exit point [X=Forward along facing, Y=Lateral]."));

                    if (wi > 0) {
                        changed |= EditBool(w, "requires_research", "Requires Laboratory Research");
                        changed |= EditFloat(w, "max_distance", "Max distance trigger (tiles, <=1.5 for flame)", 1.5f);
                    }

                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            if (weapons.size() < 3) {
                if (Hinted(ImGui::Button(_("+ Add Auxiliary Weapon (e.g. Flame Tower)")), "+ Add Auxiliary Weapon (e.g. Flame Tower)")) {
                    YAML::Node newW(YAML::NodeType::Map);
                    newW["name"] = "Flame Tower";
                    newW["projectile"] = "flame";
                    newW["damage"] = 12;
                    newW["range"] = 1.5f;
                    newW["min_range"] = 0.0f;
                    newW["cooldown"] = 0.25f;
                    newW["burst_count"] = 1;
                    newW["burst_delay"] = 0.1f;
                    newW["enabled"] = true;
                    newW["requires_research"] = true;
                    newW["max_distance"] = 1.5f;
                    YAML::Node mPos(YAML::NodeType::Sequence);
                    mPos.push_back(muzzleOffsetX);
                    mPos.push_back(muzzleOffsetY);
                    newW["localePosition"] = mPos;
                    weapons.push_back(newW);
                    changed = true;
                }
            }
        } else if (canAttack) {
            if (Hinted(ImGui::Button(_("+ Add Weapon")), "+ Add Weapon")) {
                EnsureMainWeapon(combat);
                changed = true;
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "%s", _("Multi-Weapon Simulation & Testing:"));
        Hinted(ImGui::Checkbox(_("Simulate Laboratory Research Unlocked"), &simLabUpgradeUnlocked), "Simulate Laboratory Research Unlocked");
        ImGui::SameLine();
        const char* testModes[] = {"Auto (Distance / Condition)", "Force Weapon 1", "Force Weapon 2"};
        ImGui::SetNextItemWidth(180.0f);
        Hinted(ImGui::Combo(_("Test Weapon"), &testWeaponSelectMode, testModes, 3), "Test Weapon");
        if (activeFiringWeaponIndex == 1) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.2f, 1.0f), _("Active Firing Weapon: Weapon 2 (Flame Tower - Close Range <= 1 tile)"));
        } else {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), _("Active Firing Weapon: Weapon 1 (Primary Gun)"));
        }

        // Live Combat Analytics Box
        const YAML::Node gun = MainWeapon(combat);
        const int damage = YamlInt(gun, "damage", 0);
        const float range = YamlFloat(gun, "range", 0.0f);
        const float minRange = YamlFloat(gun, "min_range", 0.0f);
        const float cooldown = std::max(0.05f, WeaponCooldown(gun, false));
        const int burstCount = std::max(1, YamlInt(gun, "burst_count", 1));
        const float burstDelay = std::max(0.01f, YamlFloat(gun, "burst_delay", 0.1f));
        const int totalBurstDmg = damage * burstCount;
        const float burstSpan = static_cast<float>(burstCount - 1) * burstDelay;
        const float cycleTime = cooldown + burstSpan;
        const float dps = (cycleTime > 0.01f) ? (static_cast<float>(totalBurstDmg) / cycleTime) : 0.0f;

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.15f, 0.16f, 0.95f));
        ImGui::BeginChild("combat-analytics", ImVec2(0.0f, burstCount > 1 ? 122.0f : 100.0f), true);
        ImGui::TextColored(ImVec4(0.4f, 0.88f, 0.72f, 1.0f), "%s", _("Combat Metrics & Burst Info:"));
        if (burstCount > 1) {
            ImGui::Text(_("Burst damage: %d dmg (%d shots x %d dmg)"), totalBurstDmg, burstCount, damage);
            ImGui::Text(_("Attack cycle: %.2fs (Burst: %.2fs + Cooldown: %.2fs)"), cycleTime, burstSpan, cooldown);
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.5f, 1.0f), _("Burst fire: %d shots fired every %.2fs"), burstCount, burstDelay);
        } else {
            ImGui::Text(_("Damage per shot: %d dmg (single shot)"), damage);
            ImGui::Text(_("Attack cycle: %.2fs cooldown"), cycleTime);
        }
        ImGui::Text(_("Effective DPS: %.1f dmg/s"), dps);
        if (WeaponRandomDelay(gun)) {
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.5f, 1.0f), _("Random delay %.2f - %.2f s (average used above)"),
                               YamlFloat(gun, "cooldown_min", 0.0f), YamlFloat(gun, "cooldown_max", 0.0f));
        }
        if (minRange > 0.0f) {
            ImGui::Text(_("Range: %.1f - %.1f tiles (%d - %d px)"), minRange, range,
                        static_cast<int>(minRange * 30.0f), static_cast<int>(range * 30.0f));
        } else {
            ImGui::Text(_("Range: %.1f tiles (%d px)"), range, static_cast<int>(range * 30.0f));
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        if (!isBuilding) {
            const int arch = YamlInt(document, "archetype_id", -1);
            if (arch == 7 || arch == 18) {
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f),
                                   _("Fortress feature: auxiliary flamethrower when target <= 1 tile away."));
            }
            if (YamlBool(document["abilities"], "siege_mode", false)) {
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1.0f),
                                   _("Siege unit: transforms into stationary siege form with enhanced range & damage."));
            }
        }
    }

    if (Hinted(ImGui::CollapsingHeader(_("Animation")), "Animation")) {
        // Sprites come from the game data by archetype / building index: nothing to type in here
        if (isBuilding) {
            ImGui::TextDisabled(_("Sprites: building %d of the game data"), YamlInt(document, "building_index", -1));
        } else {
            const int arch = YamlInt(document, "archetype_id", 0);
            ImGui::TextDisabled(_("Sprites: archetype %d - idle sequence %d, move sequence %d"), arch,
                                textures ? textures->GetUnitBaseSeq(arch, static_cast<int>(OpenArtOfWar::ActionState::Idle)) : -1,
                                textures ? textures->GetUnitBaseSeq(arch, static_cast<int>(OpenArtOfWar::ActionState::Move)) : -1);
            if (!OpenArtOfWar::Utility::IsInfantryArchetype(arch)) {
                changed |= EditFloat(document, "turn_step_delay", "Hull turn delay per 45 deg (s)",
                                     OpenArtOfWar::Utility::DefaultTurnStepDelay(arch), 0.01f);
                if (YamlBool(document["combat"], "has_rotating_turret", false)) {
                    YAML::Node combat = Section("combat");
                    const float defTDelay = document["turn_step_delay"].IsDefined()
                        ? YamlFloat(document, "turn_step_delay", OpenArtOfWar::Utility::DefaultTurretTurnStepDelay(arch))
                        : OpenArtOfWar::Utility::DefaultTurretTurnStepDelay(arch);
                    changed |= EditFloat(combat, "turret_turn_step_delay", "Turret turn delay per 45 deg (s)", defTDelay, 0.01f);
                }
            }
        }
    }

    if (Hinted(ImGui::CollapsingHeader(_("Wreck")), "Wreck")) {
        changed |= DrawWreckProperties(isBuilding);
    }

    if (!isBuilding && Hinted(ImGui::CollapsingHeader(_("Abilities")), "Abilities")) {
        YAML::Node abilities = Section("abilities");
        {
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "%s", _("Siege Mode / Alternate Form Configuration:"));
            if (Hinted(ImGui::Button(testSiegeMode ? _("Exit Siege Mode (Undeploy) [G]") : _("Test Siege Mode (Deploy) [G]")),
                       testSiegeMode ? "Exit Siege Mode (Undeploy) [G]" : "Test Siege Mode (Deploy) [G]")) {
                testSiegeMode = !testSiegeMode;
            }
            ImGui::SameLine();
            ImGui::Text(testSiegeMode ? _("[DEPLOYED]") : _("[MOBILE]"));

            changed |= EditBool(abilities, "siege_mode", "Siege mode capable");
            changed |= EditInt(abilities, "siege_archetype_id", "Siege archetype ID", -1);
            changed |= EditInt(abilities, "parent_archetype_id", "Parent archetype ID", -1);

            if (abilities["siege"] && abilities["siege"].IsMap()) {
                if (Hinted(ImGui::TreeNodeEx(_("Embedded Siege Configuration"), ImGuiTreeNodeFlags_DefaultOpen), "Embedded Siege Configuration")) {
                    bool sEn = YamlBool(abilities["siege"], "enabled", true);
                    if (Hinted(ImGui::Checkbox(_("Siege Enabled"), &sEn), "Siege Enabled")) {
                        abilities["siege"]["enabled"] = sEn;
                        changed = true;
                    }
                    changed |= EditInt(abilities["siege"], "archetype_id", "Siege Form Archetype ID", 15);
                    {
                        // When the unit goes into siege mode by itself (the computer always; a person's units on attack-move)
                        static const char *const kModes[] = {"off", "far", "always"};
                        const std::string cur = YamlString(abilities["siege"], "auto_deploy", "far");
                        int idx = cur == "off" ? 0 : (cur == "always" ? 2 : 1);
                        const char *labels = "Off (by hand only)\0Far (enemy beyond the normal range)\0Always (any enemy in siege range)\0";
                        if (Hinted(ImGui::Combo(_("Auto deploy"), &idx, labels), "Auto deploy")) {
                            abilities["siege"]["auto_deploy"] = kModes[idx];
                            changed = true;
                        }
                    }
                    changed |= EditString(abilities["siege"], "name", "Siege Name", "abilities/siege");
                    {
                        const std::string key = OpenArtOfWar::EntityManager::DescriptionKey(YamlString(document, "id") + "_siege");
                        const std::string text = _(key);
                        ImGui::TextDisabled(_("Siege description (po key %s):"), key.c_str());
                        ImGui::TextWrapped("%s", text == key ? _("(no translation yet - add it to po/en.po and po/ru.po)") : text.c_str());
                    }

                    YAML::Node sStats = abilities["siege"]["stats"];
                    if (sStats.IsMap()) {
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", _("Siege Stats:"));
                        changed |= EditInt(sStats, "sight_radius", "Sight radius", 5);
                        changed |= EditFloat(sStats, "move_speed", "Move speed while deployed", 0.0f);
                    }
                    YAML::Node sCombat = abilities["siege"]["combat"];
                    if (sCombat.IsMap()) {
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", _("Siege Combat:"));
                        YAML::Node sGun = EnsureMainWeapon(sCombat);
                        ImGui::PushID("siege-gun");
                        changed |= EditInt(sGun, "damage", "Damage", 100);
                        changed |= EditFloat(sGun, "range", "Attack range", 7.0f);
                        changed |= EditWeaponDelay(sGun);
                        changed |= EditInt(sGun, "burst_count", "Burst count", 1);
                        changed |= EditFloat(sGun, "burst_delay", "Burst delay", 0.1f);
                        changed |= EditInt(sGun, "splash_percent", "Splash Damage (%) [2x2 tile area]", 0);
                        changed |= EditProjectileCombo(sGun);
                        ImGui::PopID();
                        changed |= EditBool(sCombat, "has_rotating_turret", "Rotating turret");
                    }
                    ImGui::TreePop();
                }
            } else {
                if (Hinted(ImGui::Button(_("+ Add Full Embedded Siege Configuration (Rhino/Grad style)")), "+ Add Full Embedded Siege Configuration (Rhino/Grad style)")) {
                    YAML::Node s(YAML::NodeType::Map);
                    s["enabled"] = true;
                    int curArch = YamlInt(document, "archetype_id", 0);
                    s["archetype_id"] = (curArch == 8) ? 16 : 15;
                    s["name"] = YamlString(document, "name", "Unit") + " (Siege Mode)";
                    YAML::Node sSt(YAML::NodeType::Map);
                    sSt["max_hp"] = YamlInt(document["stats"], "max_hp", 500);
                    sSt["sight_radius"] = YamlInt(document["stats"], "sight_radius", 8);
                    sSt["move_speed"] = 0.0f;
                    s["stats"] = sSt;
                    YAML::Node sCb(YAML::NodeType::Map);
                    const YAML::Node baseGun = MainWeapon(document["combat"]);
                    YAML::Node sGun(YAML::NodeType::Map);
                    sGun["name"] = YamlString(baseGun, "name", "Siege Gun");
                    sGun["projectile"] = "shell";
                    sGun["damage"] = YamlInt(baseGun, "damage", 50) * 2;
                    sGun["range"] = YamlFloat(baseGun, "range", 5.0f) + 4.0f;
                    sGun["cooldown"] = 1.0f;
                    sGun["burst_count"] = 2;
                    sGun["burst_delay"] = 0.1f;
                    sGun["enabled"] = true;
                    sCb["has_rotating_turret"] = true;
                    sCb["weapons"].push_back(sGun);
                    s["combat"] = sCb;
                    abilities["siege"] = s;
                    abilities["siege_mode"] = true;
                    changed = true;
                }
            }
            if (abilities["upgrade"] && abilities["upgrade"].IsMap()) {
                if (Hinted(ImGui::TreeNode(_("Embedded Upgrade / Baseline Configuration")), "Embedded Upgrade / Baseline Configuration")) {
                    YAML::Node uCombat = abilities["upgrade"]["combat"];
                    if (uCombat.IsMap()) {
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", _("Baseline Combat:"));
                        YAML::Node uGun = EnsureMainWeapon(uCombat);
                        ImGui::PushID("baseline-gun");
                        changed |= EditInt(uGun, "damage", "Damage", 100);
                        changed |= EditFloat(uGun, "range", "Attack range", 7.0f);
                        changed |= EditWeaponDelay(uGun);
                        changed |= EditInt(uGun, "burst_count", "Burst count", 1);
                        changed |= EditFloat(uGun, "burst_delay", "Burst delay", 0.1f);
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
            }
        }
    }

    if (changed) dirty = true;
    ImGui::Separator();
    if (Hinted(ImGui::Button(_("Save"), ImVec2(-1.0f, 0.0f)), "Save")) SaveFile();
    ImGui::SetNextItemWidth(-1.0f);
    Hinted(ImGui::InputTextWithHint("##save-as", _("Filename for Save As"), savePath.data(), savePath.size()),
           "Filename for Save As");
    if (Hinted(ImGui::Button(_("Save As"), ImVec2(-1.0f, 0.0f)), "Save As")) SaveFileAs();
    if (Hinted(ImGui::Button(_("Reload file"), ImVec2(-1.0f, 0.0f)), "Reload file") && !activePath.empty()) {
        loadFile(activePath);
    }
}

} // namespace CharEditor
