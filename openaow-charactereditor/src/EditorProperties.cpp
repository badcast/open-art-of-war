#include "CharacterEditor.hpp"

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

void CharacterEditor::drawEditor() {
    ImGuiIO& io = ImGui::GetIO();
    const float fullWidth = io.DisplaySize.x;
    const float fullHeight = io.DisplaySize.y;
    const float leftWidth = std::clamp(fullWidth * 0.22f, 240.0f, 320.0f);
    constexpr float top = 24.0f;
    const float panelHeight = std::max(200.0f, fullHeight - top);

    const float minRightWidth = 260.0f;
    const float maxRightWidth = std::max(minRightWidth, fullWidth - leftWidth - 260.0f);
    rightPanelWidth = std::clamp(rightPanelWidth, minRightWidth, maxRightWidth);
    const float rightWidth = rightPanelWidth;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(_("File"))) {
            if (ImGui::MenuItem(_("Save"), "Ctrl+S", false, document.IsDefined())) saveFile();
            if (ImGui::MenuItem(_("Load selected"), nullptr, false, !files.empty())) {
                if (selectedIndex >= 0 && selectedIndex < static_cast<int>(files.size())) {
                    loadFile(files[static_cast<size_t>(selectedIndex)].path);
                }
            }
            if (ImGui::MenuItem(_("Refresh list"))) loadCatalog();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(_("Language"))) {
            const bool russian = OpenArtOfWar::Localization::getLanguage() == "ru";
            if (ImGui::MenuItem(_("English"), nullptr, !russian)) {
                OpenArtOfWar::Localization::setLanguage("en");
            }
            if (ImGui::MenuItem(_("Russian"), nullptr, russian)) {
                OpenArtOfWar::Localization::setLanguage("ru");
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
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::Text(_("%zu configurations"), files.size());
    ImGui::SetNextItemWidth(-1.0f);
    hinted(ImGui::InputTextWithHint("##search", _("Search by name, ID or faction"), searchBuffer.data(), searchBuffer.size()),
           "Search by name, ID or faction");

    static int filterCategory = 0; // 0=All, 1=Units, 2=Buildings
    hinted(ImGui::RadioButton(_("All"), &filterCategory, 0), "All");
    ImGui::SameLine();
    hinted(ImGui::RadioButton(_("Units"), &filterCategory, 1), "Units");
    ImGui::SameLine();
    hinted(ImGui::RadioButton(_("Buildings"), &filterCategory, 2), "Buildings");

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
        const OpenArtOfWar::Faction fileFaction = OpenArtOfWar::Utility::parseFaction(file.faction, OpenArtOfWar::Faction::Neutral);
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

    ImGui::SetNextWindowPos(ImVec2(fullWidth - rightWidth, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(rightWidth, panelHeight), ImGuiCond_Always);
    ImGui::Begin(_("Properties"), nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    if (editorMode == EditorMode::Research) drawResearchProperties();
    else if (editorMode == EditorMode::SpriteTools) drawSpriteProperties();
    else if (editorMode == EditorMode::Explosions) drawExplosionProperties();
    else if (editorMode == EditorMode::Wrecks) drawWreckTabProperties();
    else if (editorMode == EditorMode::GameUi) drawGameUiProperties();
    else if (editorMode == EditorMode::Environment) drawEnvironmentProperties();
    else if (editorMode == EditorMode::AiDifficulty) drawAiProperties();
    else if (document.IsDefined()) drawProperties();
    else ImGui::TextWrapped(_("Select a YAML configuration from the list."));
    ImGui::End();

    // Draggable vertical splitter to resize Properties tab panel
    const float splitterThickness = 8.0f;
    const float splitterX = fullWidth - rightWidth - splitterThickness * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(splitterX, top), ImGuiCond_Always);
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
    }
    if (isSplitterHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        rightPanelWidth = std::clamp(fullWidth * 0.35f, 380.0f, 520.0f);
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

    const float centerWidth = std::max(200.0f, fullWidth - leftWidth - rightWidth);
    ImGui::SetNextWindowPos(ImVec2(leftWidth, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(centerWidth, panelHeight), ImGuiCond_Always);
    ImGui::Begin(_("Preview"), nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    drawPreviewControls();
    if (editorMode == EditorMode::Research || editorMode == EditorMode::SpriteTools || editorMode == EditorMode::GameUi ||
        editorMode == EditorMode::Environment || editorMode == EditorMode::AiDifficulty) {
        previewRect = {0, 0, 0, 0};
        if (editorMode == EditorMode::Research) drawResearchTree();
        else if (editorMode == EditorMode::GameUi) drawGameUi();
        else if (editorMode == EditorMode::Environment) drawEnvironment();
        else if (editorMode == EditorMode::AiDifficulty) drawAiDifficulty();
        else drawSpriteTools();
        ImGui::End();
    } else {
    ImGui::BeginChild("preview-canvas", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoMove);
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    previewRect = {static_cast<int>(canvasMin.x), static_cast<int>(canvasMin.y),
                   std::max(1, static_cast<int>(canvasSize.x)),
                   std::max(1, static_cast<int>(canvasSize.y))};
    drawCanvasGuides(canvasMin, canvasSize);

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

    // 2. Right Mouse Button (RMB) -> Turn Unit Hull toward point
    else if (editorMode != EditorMode::Simulation && canvasActive && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        const ImVec2 mousePos = io.MousePos;
        float mWorldX = 0.0f, mWorldY = 0.0f;
        camera->screenToWorld(mousePos.x, mousePos.y, mWorldX, mWorldY);

        float dx = mWorldX;
        float dy = mWorldY;
        float angle = std::atan2(dy, dx);
        if (angle < 0.0f) angle += 2.0f * static_cast<float>(M_PI);
        int dir = static_cast<int>(std::round(angle / (static_cast<float>(M_PI) / 4.0f))) % 8;
        facing = (dir % 8 + 8) % 8;

        const bool hasRotTurret = yamlBool(document["combat"], "has_rotating_turret", false);
        if (!hasRotTurret || linkTurretToHull) {
            turretFacingDir = facing;
            targetWorldX = mWorldX;
            targetWorldY = mWorldY;
            targetCustomized = true;
        }
    }

    // 3. Left Mouse Button (LMB) -> Turn Turret / Aim
    else if (editorMode != EditorMode::Simulation && canvasActive && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
        const ImVec2 mousePos = io.MousePos;
        camera->screenToWorld(mousePos.x, mousePos.y, targetWorldX, targetWorldY);
        targetCustomized = true;

        const bool hasRotTurret = yamlBool(document["combat"], "has_rotating_turret", false);
        float tPivotX = hasRotTurret ? turretOffsetX : 0.0f;
        float tPivotY = hasRotTurret ? turretOffsetY : 0.0f;
        float dx = targetWorldX - tPivotX;
        float dy = targetWorldY - tPivotY;
        float angle = std::atan2(dy, dx);
        if (angle < 0.0f) angle += 2.0f * static_cast<float>(M_PI);
        int dir = static_cast<int>(std::round(angle / (static_cast<float>(M_PI) / 4.0f))) % 8;
        turretFacingDir = (dir % 8 + 8) % 8;
        if (!hasRotTurret) {
            facing = turretFacingDir;
        }
    }

    // 4. Double click MMB to reset view
    if (canvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
        if (editorMode == EditorMode::Simulation && battle.active()) {
            frameBattleCamera();
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

bool CharacterEditor::drawIdentity(bool isBuilding) {
    bool changed = false;
    if (isBuilding) {
        const int current = yamlInt(document, "building_index", -1);
        const auto* kind = OpenArtOfWar::Utility::buildingKindInfo(current);
        std::string preview = kind ? ("#" + std::to_string(current) + " " + _(kind->name) + " - " +
                                      _(OpenArtOfWar::Utility::factionName(kind->faction)))
                                   : std::string(_("(unknown)"));
        if (hinted(ImGui::BeginCombo(_("Building"), preview.c_str()), "Building")) {
            for (const auto& k : OpenArtOfWar::Utility::buildingKinds()) {
                const std::string label = "#" + std::to_string(k.index) + " " + _(k.name) + " - " +
                                          _(OpenArtOfWar::Utility::factionName(k.faction));
                if (ImGui::Selectable(label.c_str(), k.index == current)) {
                    document["building_index"] = k.index;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (kind) {
            ImGui::TextDisabled(_("Faction: %s   Structure type: %s"), _(OpenArtOfWar::Utility::factionName(kind->faction)),
                                OpenArtOfWar::Utility::unitTypeName(kind->type));
        }
        return changed;
    }

    const int current = yamlInt(document, "archetype_id", -1);
    const auto* arch = OpenArtOfWar::Utility::archetypeInfo(current);
    auto label = [](const OpenArtOfWar::Utility::ArchetypeInfo& a) {
        return "#" + std::to_string(a.id) + " " + _(a.name) + " - " + _(OpenArtOfWar::Utility::factionName(a.faction));
    };
    if (hinted(ImGui::BeginCombo(_("Archetype"), arch ? label(*arch).c_str() : _("(unknown)")), "Archetype")) {
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
        ImGui::TextDisabled(_("Faction: %s   Class: %s   Internal type: %s"), _(OpenArtOfWar::Utility::factionName(arch->faction)),
                            _(OpenArtOfWar::Utility::unitClassName(arch->id)), OpenArtOfWar::Utility::unitTypeName(arch->type));
    }
    return changed;
}

bool CharacterEditor::drawIconPicker(YAML::Node production) {
    bool changed = false;
    const int current = yamlInt(production, "icon_composite", 0);
    ImGui::Text(_("Build menu icon: %d"), current);
    const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 74.0f));
    int n = 0;
    for (int comp = 2064; comp <= 2081; ++comp) {
        if (n++ % perRow != 0) ImGui::SameLine();
        ImGui::PushID(comp);
        const bool selected = (comp == current);
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
        SDL_Texture* thumb = compositeThumbnail(comp, false);
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

bool CharacterEditor::drawWreckProperties(bool isBuilding) {
    bool changed = false;
    YAML::Node animations = section("animations");
    const bool infantry = !isBuilding && OpenArtOfWar::Utility::isInfantryArchetype(yamlInt(document, "archetype_id", 0));
    if (infantry) {
        ImGui::TextWrapped("%s", _("Infantry leaves a corpse instead of a wreck: the last frame of its death sequence "
                                   "(death_sequences / flame_death_sequences in Animation)."));
        if (hinted(ImGui::Button(_("Preview corpse")), "Preview corpse")) setMode(PreviewMode::Wreck);
        return false;
    }

    int composite = 0, stages = 0;
    wreckOf(document, composite, stages);
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
        SDL_Texture* thumb = wreckThumbnail(f.composite);
        const bool clicked = thumb ? ImGui::ImageButton("thumb", reinterpret_cast<ImTextureID>(thumb), ImVec2(96, 64))
                                   : ImGui::Button("?", ImVec2(96, 64));
        if (current) ImGui::PopStyleColor();
        if (clicked) {
            animations["wreck_composite"] = f.composite;
            animations["wreck_stages"] = f.stages;
            setMode(PreviewMode::Wreck);
            changed = true;
        }
        ImGui::TextDisabled("%s", _(f.label));
        ImGui::PopID();
        ImGui::EndGroup();
    }

    int st = stages;
    ImGui::SetNextItemWidth(120.0f);
    if (hinted(ImGui::SliderInt(_("Stages"), &st, 1, 8), "Stages")) {
        animations["wreck_stages"] = st;
        changed = true;
    }
    ImGui::SetNextItemWidth(160.0f);
    hinted(ImGui::SliderInt(_("Preview stage (-1 = play)"), &wreckPreviewStage, -1, stages - 1), "Preview stage (-1 = play)");
    if (hinted(ImGui::Button(_("Preview wreck")), "Preview wreck")) setMode(PreviewMode::Wreck);
    ImGui::SameLine();
    if (custom && hinted(ImGui::Button(_("Use default wreck")), "Use default wreck")) {
        animations.remove("wreck_composite");
        animations.remove("wreck_stages");
        changed = true;
    }
    ImGui::TextDisabled("%s", _("In the game the wreck stays 120 s and sinks one stage at a time."));
    return changed;
}

void CharacterEditor::drawProperties() {
    if (dirty) ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f), _("Unsaved changes"));

    // Panel Width quick controls
    ImGui::SetNextItemWidth(75.0f);
    if (hinted(ImGui::DragFloat(_("Width##rpanel"), &rightPanelWidth, 2.0f, 260.0f, 900.0f, "%.0f px"), "Width##rpanel")) {
        rightPanelWidth = std::clamp(rightPanelWidth, 260.0f, 900.0f);
    }
    ImGui::SameLine();
    if (hinted(ImGui::SmallButton(_("Compact")), "Compact")) {
        rightPanelWidth = 360.0f;
    }
    ImGui::SameLine();
    if (hinted(ImGui::SmallButton(_("Standard")), "Standard")) {
        rightPanelWidth = 470.0f;
    }
    ImGui::SameLine();
    if (hinted(ImGui::SmallButton(_("Wide")), "Wide")) {
        rightPanelWidth = 620.0f;
    }
    ImGui::Separator();

    ImGui::TextWrapped("%s", activePath.string().c_str());
    bool changed = false;

    const bool isBuilding = (yamlString(document, "type") == "building");

    if (hinted(ImGui::CollapsingHeader(_("General"), ImGuiTreeNodeFlags_DefaultOpen), "General")) {
        changed |= editString(document, "id", "ID", "root");
        changed |= editString(document, "name", "Name", "root");
        // Description: the translation of "<id>.description" in po/*.po, not a field of this file
        {
            const std::string key = OpenArtOfWar::EntityManager::descriptionKey(yamlString(document, "id"));
            const std::string text = _(key);
            ImGui::TextDisabled(_("Description (po key %s):"), key.c_str());
            ImGui::TextWrapped("%s", text == key ? _("(no translation yet - add it to po/en.po and po/ru.po)") : text.c_str());
        }
        changed |= drawIdentity(isBuilding);
    }

    if (hinted(ImGui::CollapsingHeader(_("Size and stats"), ImGuiTreeNodeFlags_DefaultOpen), "Size and stats")) {
        if (isBuilding) {
            YAML::Node size = section("size");
            changed |= editInt(size, "width", "Width (tiles)", 1);
            changed |= editInt(size, "height", "Height (tiles)", 1);
        }
        YAML::Node stats = section("stats");
        changed |= editInt(stats, "max_hp", "Maximum health", 100);
        changed |= editInt(stats, "sight_radius", "Sight radius (tiles)", 5);
        if (!isBuilding) {
            changed |= editFloat(stats, "move_speed", "Move speed (pixels per tick)", 1.4f);
        }
    }

    if (hinted(ImGui::CollapsingHeader(_("Production")), "Production")) {
        YAML::Node production = section("production");
        // Legacy price keys are folded into the single "cost" field
        for (const char* legacyKey : {"credit", "credit_cost"}) {
            if (production[legacyKey]) {
                if (!production["cost"]) production["cost"] = production[legacyKey];
                production.remove(legacyKey);
                changed = true;
            }
        }
        changed |= editInt(production, "cost", "Cost (credits)", 100);
        if (isBuilding) {
            changed |= editInt(production, "power", "Power (+/-)", 0);
        }
        changed |= editFloat(production, "build_time", "Build time", 4.0f);
        if (!isBuilding) {
            ImGui::TextDisabled(_("Produced in: %s (follows from the archetype)"),
                                _(OpenArtOfWar::Utility::producedIn(yamlInt(document, "archetype_id", 0))));
        } else {
            changed |= drawIconPicker(production);
        }
    }

    if (hinted(ImGui::CollapsingHeader(_("Combat"), ImGuiTreeNodeFlags_DefaultOpen), "Combat")) {
        YAML::Node combat = section("combat");
        if (isBuilding) {
            changed |= editBool(combat, "can_attack", "Can attack", false);
        }
        const bool canAttack = !isBuilding || yamlBool(combat, "can_attack", false);

        if (!canAttack) {
            ImGui::TextDisabled("%s", _("Combat is disabled for this archetype."));
        }

        // Damage, range, delay, projectile and muzzle belong to each weapon (Weapons below);
        // combat itself only holds what is shared by all guns
        if (editFloat(combat, "bullet_speed", "Bullet / Projectile Speed (px/s)", previewBulletSpeed, 10.0f)) {
            previewBulletSpeed = yamlFloat(combat, "bullet_speed", previewBulletSpeed);
            changed = true;
        }
        if (editFloat(combat, "fx_speed", "Special Effects / FX Speed Multiplier", previewFxSpeed, 0.05f)) {
            previewFxSpeed = yamlFloat(combat, "fx_speed", previewFxSpeed);
            changed = true;
        }
        float curSpread = yamlFloat(combat, "spread_angle", -1.0f);
        if (curSpread < 0.0f) {
            std::string ptype = yamlString(mainWeapon(combat), "projectile", "bullet");
            if (ptype == "bullet") curSpread = 3.7f;
            else if (ptype == "shell") curSpread = 2.0f;
            else if (ptype == "rocket") curSpread = 4.6f;
            else if (ptype == "flame") curSpread = 6.9f;
            else curSpread = 3.0f;
        }
        if (hinted(ImGui::DragFloat(_("Shot Spread (deg, 0=pinpoint)"), &curSpread, 0.1f, 0.0f, 15.0f, "%.1f deg"), "Shot Spread (deg, 0=pinpoint)")) {
            combat["spread_angle"] = curSpread;
            changed = true;
        }
        if (curSpread <= 0.001f) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%s", _("Shot spread: 0.0 (High Precision Pinpoint Accuracy)"));
        }
        changed |= editBool(combat, "has_rotating_turret", "Rotating turret");
        if (isBuilding) {
            changed |= editBool(combat, "has_rotating_radar", "Rotating radar");
        }

        ImGui::Spacing();
        const bool hasRotTurret = yamlBool(combat, "has_rotating_turret", false);
        if (hasRotTurret) {
            float domePos[2] = {turretOffsetX, turretOffsetY};
            if (hinted(ImGui::DragFloat2(_("Dome Offset (X, Y)"), domePos, 0.5f, -100.0f, 100.0f, "%.1f px"), "Dome Offset (X, Y)")) {
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
            if (hinted(ImGui::Button(_("Default Dome")), "Default Dome")) {
                const int aId = isBuilding ? yamlInt(document, "building_index", -1) : yamlInt(document, "archetype_id", -1);
                float defTx = 0.0f, defTy = 0.0f;
                getDefaultTurretOffset(aId, isBuilding, defTx, defTy);
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
                std::string wTitle = "Weapon " + std::to_string(wi + 1) + ": " + yamlString(w, "name", wi == 0 ? "Primary" : "Secondary");
                ImGui::PushID(static_cast<int>(wi));
                if (ImGui::TreeNodeEx(wTitle.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    bool wEnabled = yamlBool(w, "enabled", true);
                    if (hinted(ImGui::Checkbox(_("Enabled (Tested in Combat)"), &wEnabled), "Enabled (Tested in Combat)")) {
                        w["enabled"] = wEnabled;
                        changed = true;
                    }
                    if (wi > 0) {
                        ImGui::SameLine();
                        if (hinted(ImGui::Button(_("Remove Weapon")), "Remove Weapon")) {
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

                    changed |= editString(w, "name", "Weapon Name", "combat/w" + std::to_string(wi));
                    changed |= editProjectileCombo(w);
                    changed |= editInt(w, "damage", "Damage per shot", 10);
                    changed |= editFloat(w, "range", "Attack range (tiles)", 4.0f);
                    changed |= editFloat(w, "min_range", "Minimum range (tiles)", 0.0f);
                    changed |= editWeaponDelay(w);
                    changed |= editInt(w, "burst_count", "Burst count", 1);
                    changed |= editFloat(w, "burst_delay", "Burst delay (seconds)", 0.1f);
                    changed |= editInt(w, "splash_percent", "Splash Damage (%) [2x2 tile area]", 0);

                    float wLocPos[2] = {0.0f, 0.0f};
                    if (w["localePosition"] && w["localePosition"].IsSequence() && w["localePosition"].size() >= 2) {
                        try {
                            wLocPos[0] = w["localePosition"][0].as<float>(0.0f);
                            wLocPos[1] = w["localePosition"][1].as<float>(0.0f);
                        } catch (...) {}
                    }
                    if (hinted(ImGui::DragFloat2(_("Weapon Muzzle (X, Y)"), wLocPos, 0.5f, -100.0f, 100.0f, "%.1f px"), "Weapon Muzzle (X, Y)")) {
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
                        changed |= editBool(w, "requires_research", "Requires Laboratory Research");
                        changed |= editFloat(w, "max_distance", "Max distance trigger (tiles, <=1.5 for flame)", 1.5f);
                    }

                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            if (weapons.size() < 3) {
                if (hinted(ImGui::Button(_("+ Add Auxiliary Weapon (e.g. Flame Tower)")), "+ Add Auxiliary Weapon (e.g. Flame Tower)")) {
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
            if (hinted(ImGui::Button(_("+ Add Weapon")), "+ Add Weapon")) {
                ensureMainWeapon(combat);
                changed = true;
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "%s", _("Multi-Weapon Simulation & Testing:"));
        hinted(ImGui::Checkbox(_("Simulate Laboratory Research Unlocked"), &simLabUpgradeUnlocked), "Simulate Laboratory Research Unlocked");
        ImGui::SameLine();
        const char* testModes[] = {"Auto (Distance / Condition)", "Force Weapon 1", "Force Weapon 2"};
        ImGui::SetNextItemWidth(180.0f);
        hinted(ImGui::Combo(_("Test Weapon"), &testWeaponSelectMode, testModes, 3), "Test Weapon");
        if (activeFiringWeaponIndex == 1) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.2f, 1.0f), _("Active Firing Weapon: Weapon 2 (Flame Tower - Close Range <= 1 tile)"));
        } else {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), _("Active Firing Weapon: Weapon 1 (Primary Gun)"));
        }

        // Live Combat Analytics Box
        const YAML::Node gun = mainWeapon(combat);
        const int damage = yamlInt(gun, "damage", 0);
        const float range = yamlFloat(gun, "range", 0.0f);
        const float minRange = yamlFloat(gun, "min_range", 0.0f);
        const float cooldown = std::max(0.05f, weaponCooldown(gun, false));
        const int burstCount = std::max(1, yamlInt(gun, "burst_count", 1));
        const float burstDelay = std::max(0.01f, yamlFloat(gun, "burst_delay", 0.1f));
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
        if (weaponRandomDelay(gun)) {
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.5f, 1.0f), _("Random delay %.2f - %.2f s (average used above)"),
                               yamlFloat(gun, "cooldown_min", 0.0f), yamlFloat(gun, "cooldown_max", 0.0f));
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
            const int arch = yamlInt(document, "archetype_id", -1);
            if (arch == 7 || arch == 18) {
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f),
                                   _("Fortress feature: auxiliary flamethrower when target <= 1 tile away."));
            }
            if (yamlBool(document["abilities"], "siege_mode", false)) {
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1.0f),
                                   _("Siege unit: transforms into stationary siege form with enhanced range & damage."));
            }
        }
    }

    if (hinted(ImGui::CollapsingHeader(_("Animation")), "Animation")) {
        // Sprites come from the game data by archetype / building index: nothing to type in here
        if (isBuilding) {
            ImGui::TextDisabled(_("Sprites: building %d of the game data"), yamlInt(document, "building_index", -1));
        } else {
            const int arch = yamlInt(document, "archetype_id", 0);
            ImGui::TextDisabled(_("Sprites: archetype %d - idle sequence %d, move sequence %d"), arch,
                                textures ? textures->getUnitBaseSeq(arch, static_cast<int>(OpenArtOfWar::ActionState::Idle)) : -1,
                                textures ? textures->getUnitBaseSeq(arch, static_cast<int>(OpenArtOfWar::ActionState::Move)) : -1);
            if (!OpenArtOfWar::Utility::isInfantryArchetype(arch)) {
                ImGui::TextDisabled(_("Hull turn: %.2f s per 45 degrees"),
                                    yamlFloat(document, "turn_step_delay", OpenArtOfWar::Utility::defaultTurnStepDelay(arch)));
            }
        }
    }

    if (hinted(ImGui::CollapsingHeader(_("Wreck")), "Wreck")) {
        changed |= drawWreckProperties(isBuilding);
    }

    if (!isBuilding && hinted(ImGui::CollapsingHeader(_("Abilities")), "Abilities")) {
        YAML::Node abilities = section("abilities");
        {
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "%s", _("Siege Mode / Alternate Form Configuration:"));
            if (hinted(ImGui::Button(testSiegeMode ? _("Exit Siege Mode (Undeploy) [G]") : _("Test Siege Mode (Deploy) [G]")),
                       testSiegeMode ? "Exit Siege Mode (Undeploy) [G]" : "Test Siege Mode (Deploy) [G]")) {
                testSiegeMode = !testSiegeMode;
            }
            ImGui::SameLine();
            ImGui::Text(testSiegeMode ? _("[DEPLOYED]") : _("[MOBILE]"));

            changed |= editBool(abilities, "siege_mode", "Siege mode capable");
            changed |= editInt(abilities, "siege_archetype_id", "Siege archetype ID", -1);
            changed |= editInt(abilities, "parent_archetype_id", "Parent archetype ID", -1);

            if (abilities["siege"] && abilities["siege"].IsMap()) {
                if (hinted(ImGui::TreeNodeEx(_("Embedded Siege Configuration"), ImGuiTreeNodeFlags_DefaultOpen), "Embedded Siege Configuration")) {
                    bool sEn = yamlBool(abilities["siege"], "enabled", true);
                    if (hinted(ImGui::Checkbox(_("Siege Enabled"), &sEn), "Siege Enabled")) {
                        abilities["siege"]["enabled"] = sEn;
                        changed = true;
                    }
                    changed |= editInt(abilities["siege"], "archetype_id", "Siege Form Archetype ID", 15);
                    changed |= editString(abilities["siege"], "name", "Siege Name", "abilities/siege");
                    {
                        const std::string key = OpenArtOfWar::EntityManager::descriptionKey(yamlString(document, "id") + "_siege");
                        const std::string text = _(key);
                        ImGui::TextDisabled(_("Siege description (po key %s):"), key.c_str());
                        ImGui::TextWrapped("%s", text == key ? _("(no translation yet - add it to po/en.po and po/ru.po)") : text.c_str());
                    }

                    YAML::Node sStats = abilities["siege"]["stats"];
                    if (sStats.IsMap()) {
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", _("Siege Stats:"));
                        changed |= editInt(sStats, "sight_radius", "Sight radius", 5);
                        changed |= editFloat(sStats, "move_speed", "Move speed while deployed", 0.0f);
                    }
                    YAML::Node sCombat = abilities["siege"]["combat"];
                    if (sCombat.IsMap()) {
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", _("Siege Combat:"));
                        YAML::Node sGun = ensureMainWeapon(sCombat);
                        ImGui::PushID("siege-gun");
                        changed |= editInt(sGun, "damage", "Damage", 100);
                        changed |= editFloat(sGun, "range", "Attack range", 7.0f);
                        changed |= editWeaponDelay(sGun);
                        changed |= editInt(sGun, "burst_count", "Burst count", 1);
                        changed |= editFloat(sGun, "burst_delay", "Burst delay", 0.1f);
                        changed |= editInt(sGun, "splash_percent", "Splash Damage (%) [2x2 tile area]", 0);
                        changed |= editProjectileCombo(sGun);
                        ImGui::PopID();
                        changed |= editBool(sCombat, "has_rotating_turret", "Rotating turret");
                    }
                    ImGui::TreePop();
                }
            } else {
                if (hinted(ImGui::Button(_("+ Add Full Embedded Siege Configuration (Rhino/Grad style)")), "+ Add Full Embedded Siege Configuration (Rhino/Grad style)")) {
                    YAML::Node s(YAML::NodeType::Map);
                    s["enabled"] = true;
                    int curArch = yamlInt(document, "archetype_id", 0);
                    s["archetype_id"] = (curArch == 8) ? 16 : 15;
                    s["name"] = yamlString(document, "name", "Unit") + " (Siege Mode)";
                    YAML::Node sSt(YAML::NodeType::Map);
                    sSt["max_hp"] = yamlInt(document["stats"], "max_hp", 500);
                    sSt["sight_radius"] = yamlInt(document["stats"], "sight_radius", 8);
                    sSt["move_speed"] = 0.0f;
                    s["stats"] = sSt;
                    YAML::Node sCb(YAML::NodeType::Map);
                    const YAML::Node baseGun = mainWeapon(document["combat"]);
                    YAML::Node sGun(YAML::NodeType::Map);
                    sGun["name"] = yamlString(baseGun, "name", "Siege Gun");
                    sGun["projectile"] = "shell";
                    sGun["damage"] = yamlInt(baseGun, "damage", 50) * 2;
                    sGun["range"] = yamlFloat(baseGun, "range", 5.0f) + 4.0f;
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
                if (hinted(ImGui::TreeNode(_("Embedded Upgrade / Baseline Configuration")), "Embedded Upgrade / Baseline Configuration")) {
                    YAML::Node uCombat = abilities["upgrade"]["combat"];
                    if (uCombat.IsMap()) {
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", _("Baseline Combat:"));
                        YAML::Node uGun = ensureMainWeapon(uCombat);
                        ImGui::PushID("baseline-gun");
                        changed |= editInt(uGun, "damage", "Damage", 100);
                        changed |= editFloat(uGun, "range", "Attack range", 7.0f);
                        changed |= editWeaponDelay(uGun);
                        changed |= editInt(uGun, "burst_count", "Burst count", 1);
                        changed |= editFloat(uGun, "burst_delay", "Burst delay", 0.1f);
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
            }
        }
    }

    if (changed) dirty = true;
    ImGui::Separator();
    if (hinted(ImGui::Button(_("Save"), ImVec2(-1.0f, 0.0f)), "Save")) saveFile();
    ImGui::SetNextItemWidth(-1.0f);
    hinted(ImGui::InputTextWithHint("##save-as", _("Filename for Save As"), savePath.data(), savePath.size()),
           "Filename for Save As");
    if (hinted(ImGui::Button(_("Save As"), ImVec2(-1.0f, 0.0f)), "Save As")) saveFileAs();
    if (hinted(ImGui::Button(_("Reload file"), ImVec2(-1.0f, 0.0f)), "Reload file") && !activePath.empty()) {
        loadFile(activePath);
    }
}

} // namespace CharEditor
