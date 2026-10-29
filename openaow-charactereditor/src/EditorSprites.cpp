#include "CharacterEditor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace CharEditor {

namespace {

// Called by SDL (possibly from another thread) when the folder dialog closes: the result waits for the main loop
void SDLCALL onFolderChosen(void* userdata, const char* const* list, int /*filter*/) {
    auto* pick = static_cast<FolderPick*>(userdata);
    std::lock_guard<std::mutex> lock(pick->mutex);
    if (list && list[0]) {
        pick->path = list[0];
        pick->ready = true;
    }
}

const char* const kGroupLabels[kSpriteGroupCount] = {"Units", "Buildings", "Terrain", "Other"};

} // namespace

void CharacterEditor::ensureSpriteCatalog() {
    if (spriteCatalogTried) return;
    spriteCatalogTried = true;
    std::string error;
    spriteCatalogOk = spriteCatalog.load(assetsRoot, error);
    if (!spriteCatalogOk) {
        spriteReport = error;
        status = error;
        return;
    }
    // The picture of an object in the list
    spriteThumbIds = pickObjectThumbnails(spriteCatalog, assetsRoot);
    if (spriteOutDir[0] == '\0') std::strncpy(spriteOutDir.data(), "out-sprites", spriteOutDir.size() - 1);
}

// A sprite of the sheets in a box x box slot (whole-number magnification when it fits)
void CharacterEditor::drawSpriteImage(int spriteId, float box) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(box, box));
    drawSpriteImageAt(pos, spriteId, box);
}

void CharacterEditor::drawSpriteImageAt(const ImVec2& pos, int spriteId, float box) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + box, pos.y + box), IM_COL32(28, 34, 30, 255));
    if (spriteId < 0 || spriteId >= static_cast<int>(spriteCatalog.sprites().size())) return;
    const RawSprite& s = spriteCatalog.sprites()[static_cast<size_t>(spriteId)];
    SDL_Texture* tex = textures->getSpriteSheet(s.page);
    if (!tex || s.w <= 0 || s.h <= 0) return;
    float tw = 1.0f, th = 1.0f;
    SDL_GetTextureSize(tex, &tw, &th);
    const float scale = textures->getSheetScale(s.page);
    float zoom = std::min(box / s.w, box / s.h);
    if (zoom >= 1.0f) zoom = std::floor(zoom);
    const ImVec2 size(s.w * zoom, s.h * zoom);
    const ImVec2 p0(pos.x + (box - size.x) * 0.5f, pos.y + (box - size.y) * 0.5f);
    dl->AddImage(reinterpret_cast<ImTextureID>(tex), p0, ImVec2(p0.x + size.x, p0.y + size.y),
                 ImVec2(s.x * scale / tw, s.y * scale / th), ImVec2((s.x + s.w) * scale / tw, (s.y + s.h) * scale / th));
}

void CharacterEditor::drawDepNode(const DepNode& node, int depth) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    else flags |= ImGuiTreeNodeFlags_OpenOnArrow;
    if (depth == 0) flags |= ImGuiTreeNodeFlags_DefaultOpen;

    std::string label = node.label;
    if (node.ox != 0 || node.oy != 0) label += "   (" + std::to_string(node.ox) + ", " + std::to_string(node.oy) + ")";
    ImGui::PushID(&node);
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemVisible()) {
        if (node.kind == "sprite") {
            ImGui::SameLine();
            drawSpriteImage(node.id, 24.0f);
        } else if (node.kind == "composite") {
            if (SDL_Texture* thumb = compositeThumbnail(node.id, true)) {
                ImGui::SameLine();
                ImGui::Image(reinterpret_cast<ImTextureID>(thumb), ImVec2(48.0f, 32.0f));
            }
        }
    }
    if (open && !node.children.empty()) {
        for (const DepNode& child : node.children) drawDepNode(child, depth + 1);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void CharacterEditor::drawSpriteTools() {
    ensureSpriteCatalog();
    if (!spriteCatalogOk) {
        ImGui::TextWrapped("%s", spriteReport.empty() ? _("The sprite data could not be loaded.") : spriteReport.c_str());
        return;
    }
    const auto& objects = spriteCatalog.objects();

    // Group filter
    static const char* const kFilterKeys[5] = {"All##sprf", "Units##sprf", "Buildings##sprf", "Terrain##sprf", "Other##sprf"};
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        const std::string label = std::string(i == 0 ? _("All") : _(kGroupLabels[i - 1])) + "##sprf";
        if (hinted(ImGui::RadioButton(label.c_str(), spriteGroupFilter == i - 1), kFilterKeys[i])) spriteGroupFilter = i - 1;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    hinted(ImGui::InputTextWithHint("##spritesearch", _("Search sprite objects"), spriteSearch.data(), spriteSearch.size()),
           "Search sprite objects");

    int shown = 0;
    std::string needle(spriteSearch.data());
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return std::tolower(c); });
    auto visible = [&](const SpriteObject& o) {
        if (spriteGroupFilter >= 0 && static_cast<int>(o.group) != spriteGroupFilter) return false;
        if (needle.empty()) return true;
        std::string hay = o.name + " " + o.faction + " " + o.kindName + " " + std::to_string(o.id);
        std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return std::tolower(c); });
        return hay.find(needle) != std::string::npos;
    };
    for (const auto& o : objects) shown += visible(o) ? 1 : 0;
    ImGui::TextDisabled(_("%d objects, %zu sprites"), shown, spriteCatalog.sprites().size());
    std::vector<int> shownIdx;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (visible(objects[i])) shownIdx.push_back(static_cast<int>(i));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(_("Select all shown"))) {
        for (int idx : shownIdx) spriteChecked.insert(idx);
        if (!shownIdx.empty()) { spriteSelected = shownIdx.front(); spriteAnchor = spriteSelected; }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(_("Clear selection"))) {
        spriteChecked.clear();
        spriteSelected = -1;
        spriteAnchor = -1;
    }
    ImGui::TextDisabled(_("Selected: %zu (Ctrl+click: add, Shift+click: range)"), spriteChecked.size());

    const float listWidth = std::max(220.0f, ImGui::GetContentRegionAvail().x * 0.38f);
    ImGui::BeginChild("sprite-objects", ImVec2(listWidth, 0.0f), true);
    int lastGroup = -1;
    constexpr float rowH = 40.0f;
    for (size_t i = 0; i < objects.size(); ++i) {
        const SpriteObject& o = objects[i];
        if (!visible(o)) continue;
        if (static_cast<int>(o.group) != lastGroup) {
            lastGroup = static_cast<int>(o.group);
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", _(kGroupLabels[lastGroup]));
            ImGui::Separator();
        }
        ImGui::PushID(static_cast<int>(i));
        const int index = static_cast<int>(i);
        const bool selected = spriteChecked.count(index) > 0;
        if (ImGui::Selectable("##obj", selected, 0, ImVec2(0.0f, rowH))) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && spriteAnchor >= 0) {
                auto a = std::find(shownIdx.begin(), shownIdx.end(), spriteAnchor);
                auto b = std::find(shownIdx.begin(), shownIdx.end(), index);
                if (a != shownIdx.end() && b != shownIdx.end()) {
                    if (a > b) std::swap(a, b);
                    if (!io.KeyCtrl) spriteChecked.clear();
                    for (auto it = a; it <= b; ++it) spriteChecked.insert(*it);
                } else {
                    spriteChecked.insert(index);
                }
                spriteSelected = index;
            } else if (io.KeyCtrl) {
                if (spriteChecked.erase(index) == 0) {
                    spriteChecked.insert(index);
                    spriteSelected = index;
                } else if (spriteSelected == index) {
                    spriteSelected = spriteChecked.empty() ? -1 : *spriteChecked.begin();
                }
                spriteAnchor = index;
            } else {
                spriteChecked.clear();
                spriteChecked.insert(index);
                spriteSelected = index;
                spriteAnchor = index;
            }
        }
        const ImVec2 min = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // The picture, then name and details
        drawSpriteImageAt(ImVec2(min.x + 2.0f, min.y + 2.0f), i < spriteThumbIds.size() ? spriteThumbIds[i] : -1, rowH - 4.0f);
        const std::string title = (o.kind == SpriteObjectKind::Unit ? "[#" : o.kind == SpriteObjectKind::Building ? "[B#" : "[") +
                                  std::to_string(o.id) + "] " + o.name;
        dl->AddText(ImVec2(min.x + rowH + 6.0f, min.y + 3.0f), IM_COL32(230, 235, 232, 255), title.c_str());
        const std::string detail = o.faction + ", " + std::to_string(o.sprites.size()) + " sprites";
        dl->AddText(ImVec2(min.x + rowH + 6.0f, min.y + 21.0f), IM_COL32(150, 175, 168, 255), detail.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("sprite-detail", ImVec2(0.0f, 0.0f), true);
    if (spriteSelected < 0 || spriteSelected >= static_cast<int>(objects.size())) {
        ImGui::TextWrapped("%s", _("Pick an object in the list: its dependencies (sequences, composites, animations, sprites) are shown here."));
    } else {
        const SpriteObject& o = objects[static_cast<size_t>(spriteSelected)];
        if (spriteTreeFor != spriteSelected) {
            spriteTree = spriteCatalog.tree(o);
            spriteTreeFor = spriteSelected;
        }
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", o.name.c_str());
        ImGui::TextDisabled("%s", (o.faction + " / " + o.kindName + " #" + std::to_string(o.id)).c_str());
        ImGui::Text(_("%zu sprites, %zu composites, %zu animations, %zu sequences, %zu buildings"), o.sprites.size(),
                    o.composites.size(), o.anims.size(), o.sequences.size(), o.buildings.size());
        ImGui::Separator();
        drawDepNode(spriteTree, 0);
    }
    ImGui::EndChild();
}

void CharacterEditor::drawSpriteProperties() {
    ensureSpriteCatalog();
    if (!spriteCatalogOk) {
        ImGui::TextWrapped("%s", spriteReport.empty() ? _("The sprite data could not be loaded.") : spriteReport.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(folderPick.mutex);
        if (folderPick.ready) {
            std::strncpy(spriteOutDir.data(), folderPick.path.c_str(), spriteOutDir.size() - 1);
            spriteOutDir.back() = '\0';
            folderPick.ready = false;
        }
    }
    const fs::path outDir = fs::path(spriteOutDir.data());

    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", _("Export objects"));
    ImGui::TextWrapped("%s", _("One PNG atlas and one JSON per object (unit, building, tileset) with all the sprites it depends on."));
    ImGui::SetNextItemWidth(-130.0f);
    hinted(ImGui::InputText(_("Output folder"), spriteOutDir.data(), spriteOutDir.size()), "Output folder");
    if (hinted(ImGui::Button(_("Choose folder...")), "Choose folder...")) {
        SDL_ShowOpenFolderDialog(onFolderChosen, &folderPick, window, nullptr, false);
    }
    ImGui::SameLine();
    const bool haveDir = spriteOutDir[0] != '\0';
    if (hinted(ImGui::Button(_("Export all objects")), "Export all objects")) {
        if (!haveDir) {
            spriteReport = _("Choose an output folder first.");
        } else {
            std::vector<int> all(spriteCatalog.objects().size());
            std::iota(all.begin(), all.end(), 0);
            exportSpriteObjects(spriteCatalog, assetsRoot, outDir, all, spriteReport);
            status = spriteReport;
        }
    }
    const bool hasSelection = !spriteChecked.empty();
    ImGui::BeginDisabled(!hasSelection);
    if (hinted(ImGui::Button(_("Export selected object")), "Export selected object")) {
        if (!haveDir) {
            spriteReport = _("Choose an output folder first.");
        } else {
            exportSpriteObjects(spriteCatalog, assetsRoot, outDir,
                                std::vector<int>(spriteChecked.begin(), spriteChecked.end()), spriteReport);
            status = spriteReport;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(_("Selected: %zu"), spriteChecked.size());

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", _("SINGLE SPRITE EXPORT"));
    ImGui::TextWrapped("%s", _("Every sprite in the order of its id, in rows and columns of one big PNG per group."));
    static const char* const kExportKeys[kSpriteGroupCount] = {"Units##sprx", "Buildings##sprx", "Terrain##sprx", "Other##sprx"};
    for (int g = 0; g < kSpriteGroupCount; ++g) {
        if (g) ImGui::SameLine();
        const std::string label = std::string(_(kGroupLabels[g])) + "##sprx";
        hinted(ImGui::Checkbox(label.c_str(), &singleOptions.groups[g]), kExportKeys[g]);
    }
    ImGui::SetNextItemWidth(120.0f);
    if (hinted(ImGui::InputInt(_("Columns"), &singleOptions.columns), "Columns")) {
        singleOptions.columns = std::clamp(singleOptions.columns, 1, kMaxExportColumns);
    }
    hinted(ImGui::Checkbox(_("All in one file"), &singleOptions.allInOne), "All in one file");
    hinted(ImGui::Checkbox(_("Compress sprites"), &singleOptions.compress), "Compress sprites");
    hinted(ImGui::Checkbox(_("Packed atlas"), &singleOptions.packed), "Packed atlas");
    if (hinted(ImGui::Button(_("Export sprites")), "Export sprites")) {
        if (!haveDir) {
            spriteReport = _("Choose an output folder first.");
        } else {
            exportSingleSprites(spriteCatalog, assetsRoot, outDir, singleOptions, spriteReport);
            status = spriteReport;
        }
    }

    // Import: the exports above back into the assets
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", _("IMPORT SPRITES"));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.35f, 1.0f));
    ImGui::TextWrapped("%s", _("WARNING: the import writes into assets/sprites and assets/data/graphics_data.json. Your data there may be overwritten."));
    ImGui::PopStyleColor();
    ImGui::TextWrapped("%s", _("Reads the exported object files and single sprite files of a folder: the pixels of the sprites you edited go back to the sheets, edited table rows to graphics_data.json."));
    {
        std::lock_guard<std::mutex> lock(importPick.mutex);
        if (importPick.ready) {
            std::strncpy(spriteInDir.data(), importPick.path.c_str(), spriteInDir.size() - 1);
            spriteInDir.back() = '\0';
            importPick.ready = false;
        }
    }
    ImGui::SetNextItemWidth(-130.0f);
    hinted(ImGui::InputText(_("Import folder"), spriteInDir.data(), spriteInDir.size()), "Import folder");
    if (hinted(ImGui::Button(_("Choose import folder...")), "Choose import folder...")) {
        SDL_ShowOpenFolderDialog(onFolderChosen, &importPick, window, nullptr, false);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(spriteInDir[0] == '\0');
    if (hinted(ImGui::Button(_("Import sprites")), "Import sprites")) importConfirm = true;
    ImGui::EndDisabled();
    if (importConfirm) {
        ImGui::OpenPopup(_("Overwrite assets?"));
        importConfirm = false;
    }
    if (ImGui::BeginPopupModal(_("Overwrite assets?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", _("The import overwrites the sprite sheets in assets/sprites and the tables in assets/data/graphics_data.json with what is in the folder. Your data there may be lost."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", spriteInDir.data());
        ImGui::Spacing();
        if (ImGui::Button(_("Import and overwrite"))) {
            importSpriteFolder(assetsRoot, fs::path(spriteInDir.data()), spriteReport);
            status = spriteReport;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (!spriteReport.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", spriteReport.c_str());
    }
}

} // namespace CharEditor
