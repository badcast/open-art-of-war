#include "CharacterEditor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>

namespace CharEditor
{

namespace
{

// Called by SDL (possibly from another thread) when the folder dialog closes: the result waits for the main loop
void SDLCALL OnFolderChosen(void *userdata, const char *const *list, int /*filter*/)
{
    auto *pick = static_cast<FolderPick *>(userdata);
    std::lock_guard<std::mutex> lock(pick->mutex);
    if (list && list[0])
    {
        pick->path = list[0];
        pick->ready = true;
    }
}

const char *const kGroupLabels[kSpriteGroupCount] = {"Units", "Buildings", "Terrain", "Other"};

} // namespace

void CharacterEditor::EnsureSpriteCatalog()
{
    if (spriteCatalogTried)
        return;
    spriteCatalogTried = true;
    std::string error;
    spriteCatalogOk = spriteCatalog.load(assetsRoot, error);
    if (!spriteCatalogOk)
    {
        spriteReport = error;
        status = error;
        return;
    }
    // The picture of an object in the list
    spriteThumbIds = PickObjectThumbnails(spriteCatalog, assetsRoot);
    if (spriteOutDir[0] == '\0')
        std::strncpy(spriteOutDir.data(), "out-sprites", spriteOutDir.size() - 1);

    // Build reverse lookup: spriteId -> list of object indices that use it
    spriteToObjects.assign(spriteCatalog.Sprites().size(), {});
    const auto &objs = spriteCatalog.Objects();
    for (std::size_t oi = 0; oi < objs.size(); ++oi)
    {
        for (int sid : objs[oi].sprites)
        {
            if (sid >= 0 && sid < static_cast<int>(spriteToObjects.size()))
            {
                spriteToObjects[static_cast<std::size_t>(sid)].push_back(static_cast<int>(oi));
            }
        }
    }
}

// A sprite of the sheets in a box x box slot (whole-number magnification when it fits)
void CharacterEditor::DrawSpriteImage(int spriteId, float box)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(box, box));
    DrawSpriteImageAt(pos, spriteId, box);
}

void CharacterEditor::DrawSpriteImageAt(const ImVec2 &pos, int spriteId, float box)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + box, pos.y + box), IM_COL32(28, 34, 30, 255));
    if (spriteId < 0 || spriteId >= static_cast<int>(spriteCatalog.Sprites().size()))
        return;
    const RawSprite &s = spriteCatalog.Sprites()[static_cast<std::size_t>(spriteId)];
    SDL_Texture *tex = textures->GetSpriteSheet(s.page);
    if (!tex || s.w <= 0 || s.h <= 0)
        return;
    float tw = 1.0f, th = 1.0f;
    SDL_GetTextureSize(tex, &tw, &th);
    const float scale = textures->GetSheetScale(s.page);
    float zoom = std::min(box / s.w, box / s.h);
    if (zoom >= 1.0f)
        zoom = std::floor(zoom);
    const ImVec2 size(s.w * zoom, s.h * zoom);
    const ImVec2 p0(pos.x + (box - size.x) * 0.5f, pos.y + (box - size.y) * 0.5f);
    dl->AddImage(
        reinterpret_cast<ImTextureID>(tex),
        p0,
        ImVec2(p0.x + size.x, p0.y + size.y),
        ImVec2(s.x * scale / tw, s.y * scale / th),
        ImVec2((s.x + s.w) * scale / tw, (s.y + s.h) * scale / th));
}

void CharacterEditor::DrawDepNode(const DepNode &node, int depth)
{
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node.children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    else
        flags |= ImGuiTreeNodeFlags_OpenOnArrow;
    if (depth == 0)
        flags |= ImGuiTreeNodeFlags_DefaultOpen;

    std::string label = node.label;
    if (node.ox != 0 || node.oy != 0)
        label += "   (" + std::to_string(node.ox) + ", " + std::to_string(node.oy) + ")";
    ImGui::PushID(&node);
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemVisible())
    {
        if (node.kind == "sprite")
        {
            ImGui::SameLine();
            DrawSpriteImage(node.id, 24.0f);
        }
        else if (node.kind == "composite")
        {
            if (SDL_Texture *thumb = CompositeThumbnail(node.id, true))
            {
                ImGui::SameLine();
                ImGui::Image(reinterpret_cast<ImTextureID>(thumb), ImVec2(48.0f, 32.0f));
            }
        }
    }
    if (open && !node.children.empty())
    {
        for (const DepNode &child : node.children)
            DrawDepNode(child, depth + 1);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void CharacterEditor::DrawSpriteTools()
{
    EnsureSpriteCatalog();
    if (!spriteCatalogOk)
    {
        ImGui::TextWrapped(
            "%s", spriteReport.empty() ? _("The sprite data could not be loaded.") : spriteReport.c_str());
        return;
    }
    const auto &objects = spriteCatalog.Objects();

    // Group filter
    static const char *const kFilterKeys[5]
        = {"All##sprf", "Units##sprf", "Buildings##sprf", "Terrain##sprf", "Other##sprf"};
    for (int i = 0; i < 5; ++i)
    {
        if (i)
            ImGui::SameLine();
        const std::string label = std::string(i == 0 ? _("All") : _(kGroupLabels[i - 1])) + "##sprf";
        if (Hinted(ImGui::RadioButton(label.c_str(), spriteGroupFilter == i - 1), kFilterKeys[i]))
            spriteGroupFilter = i - 1;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    Hinted(
        ImGui::InputTextWithHint(
            "##spritesearch", _("Search sprite objects"), spriteSearch.data(), spriteSearch.size()),
        "Search sprite objects");

    int shown = 0;
    std::string needle(spriteSearch.data());
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return std::tolower(c); });
    auto visible = [&](const SpriteObject &o) {
        if (spriteGroupFilter >= 0 && static_cast<int>(o.group) != spriteGroupFilter)
            return false;
        if (needle.empty())
            return true;
        std::string hay = o.name + " " + o.faction + " " + o.kindName + " " + std::to_string(o.id);
        std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return std::tolower(c); });
        return hay.find(needle) != std::string::npos;
    };
    for (const auto &o : objects)
        shown += visible(o) ? 1 : 0;
    ImGui::TextDisabled(_("%d objects, %zu sprites"), shown, spriteCatalog.Sprites().size());
    std::vector<int> shownIdx;
    for (std::size_t i = 0; i < objects.size(); ++i)
    {
        if (visible(objects[i]))
            shownIdx.push_back(static_cast<int>(i));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(_("Select all shown")))
    {
        for (int idx : shownIdx)
            spriteChecked.insert(idx);
        if (!shownIdx.empty())
        {
            spriteSelected = shownIdx.front();
            spriteAnchor = spriteSelected;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(_("Clear selection")))
    {
        spriteChecked.clear();
        spriteSelected = -1;
        spriteAnchor = -1;
    }
    ImGui::TextDisabled(_("Selected: %zu (Ctrl+click: add, Shift+click: range)"), spriteChecked.size());

    const float availW = ImGui::GetContentRegionAvail().x;
    constexpr float kMinSpritePanel = 70.0f;
    const float sprSplitterW = 6.0f;
    const float sprSpacing = ImGui::GetStyle().ItemSpacing.x * 2.0f + sprSplitterW;
    const float maxListWidth = std::max(kMinSpritePanel, availW - kMinSpritePanel - sprSpacing);
    spriteListWidth = std::clamp(spriteListWidth, kMinSpritePanel, maxListWidth);
    const float listWidth = spriteListWidth;
    ImGui::BeginChild("sprite-objects", ImVec2(listWidth, 0.0f), true);
    int lastGroup = -1;
    constexpr float rowH = 40.0f;
    for (std::size_t i = 0; i < objects.size(); ++i)
    {
        const SpriteObject &o = objects[i];
        if (!visible(o))
            continue;
        if (static_cast<int>(o.group) != lastGroup)
        {
            lastGroup = static_cast<int>(o.group);
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", _(kGroupLabels[lastGroup]));
            ImGui::Separator();
        }
        ImGui::PushID(static_cast<int>(i));
        const int index = static_cast<int>(i);
        const bool selected = spriteChecked.count(index) > 0;
        if (ImGui::Selectable("##obj", selected, 0, ImVec2(0.0f, rowH)))
        {
            const ImGuiIO &io = ImGui::GetIO();
            if (io.KeyShift && spriteAnchor >= 0)
            {
                auto a = std::find(shownIdx.begin(), shownIdx.end(), spriteAnchor);
                auto b = std::find(shownIdx.begin(), shownIdx.end(), index);
                if (a != shownIdx.end() && b != shownIdx.end())
                {
                    if (a > b)
                        std::swap(a, b);
                    if (!io.KeyCtrl)
                        spriteChecked.clear();
                    for (auto it = a; it <= b; ++it)
                        spriteChecked.insert(*it);
                }
                else
                {
                    spriteChecked.insert(index);
                }
                spriteSelected = index;
                OnSpriteObjectSelected(index);
            }
            else if (io.KeyCtrl)
            {
                if (spriteChecked.erase(index) == 0)
                {
                    spriteChecked.insert(index);
                    spriteSelected = index;
                    OnSpriteObjectSelected(index);
                }
                else if (spriteSelected == index)
                {
                    spriteSelected = spriteChecked.empty() ? -1 : *spriteChecked.begin();
                    if (spriteSelected >= 0)
                        OnSpriteObjectSelected(spriteSelected);
                }
                spriteAnchor = index;
            }
            else
            {
                spriteChecked.clear();
                spriteChecked.insert(index);
                spriteSelected = index;
                spriteAnchor = index;
                OnSpriteObjectSelected(index);
            }
        }
        const ImVec2 min = ImGui::GetItemRectMin();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        // The picture, then name and details
        DrawSpriteImageAt(
            ImVec2(min.x + 2.0f, min.y + 2.0f), i < spriteThumbIds.size() ? spriteThumbIds[i] : -1, rowH - 4.0f);
        const std::string title = (o.kind == SpriteObjectKind::Unit       ? "[#"
                                   : o.kind == SpriteObjectKind::Building ? "[B#"
                                                                          : "[")
                                  + std::to_string(o.id) + "] " + o.name;
        dl->AddText(ImVec2(min.x + rowH + 6.0f, min.y + 3.0f), IM_COL32(230, 235, 232, 255), title.c_str());
        const std::string detail = o.faction + ", " + std::to_string(o.sprites.size()) + " sprites";
        dl->AddText(ImVec2(min.x + rowH + 6.0f, min.y + 21.0f), IM_COL32(150, 175, 168, 255), detail.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    // Draggable vertical splitter inside Sprite Tools
    ImGui::InvisibleButton("##spr_vsplitter", ImVec2(sprSplitterW, -1.0f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    const ImGuiIO &sprIo = ImGui::GetIO();
    if (ImGui::IsItemActive())
    {
        spriteListWidth += sprIo.MouseDelta.x;
        spriteListWidth = std::clamp(spriteListWidth, kMinSpritePanel, maxListWidth);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        spriteListWidth = std::clamp(availW * 0.38f, kMinSpritePanel, maxListWidth);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 pMin = ImGui::GetItemRectMin();
        ImVec2 pMax = ImGui::GetItemRectMax();
        dl->AddRectFilled(
            pMin, pMax, ImGui::IsItemActive() ? IM_COL32(70, 190, 160, 220) : IM_COL32(55, 140, 115, 160));
    }

    ImGui::SameLine();
    ImGui::BeginChild("sprite-detail", ImVec2(0.0f, 0.0f), true);

    const bool isPointEditor = (spriteDetailTab == SpriteDetailTab::PointEditor);
    const bool isTree = (spriteDetailTab == SpriteDetailTab::Tree);
    const bool isPreview = (spriteDetailTab == SpriteDetailTab::ExportPreview);
    const bool isLoaded = (spriteDetailTab == SpriteDetailTab::LoadedSprites);

    if (isPointEditor)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.35f, 1.0f));
    if (Hinted(ImGui::Button(_("Point Editor"), ImVec2(120.0f, 26.0f)), "Point Editor"))
    {
        spriteDetailTab = SpriteDetailTab::PointEditor;
    }
    if (isPointEditor)
        ImGui::PopStyleColor();

    ImGui::SameLine();
    if (isTree)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.35f, 1.0f));
    if (Hinted(ImGui::Button(_("Dependencies"), ImVec2(120.0f, 26.0f)), "Dependencies"))
    {
        spriteDetailTab = SpriteDetailTab::Tree;
    }
    if (isTree)
        ImGui::PopStyleColor();

    ImGui::SameLine();
    if (isPreview)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.35f, 1.0f));
    if (Hinted(ImGui::Button(_("Export Preview"), ImVec2(130.0f, 26.0f)), "Export Preview"))
    {
        spriteDetailTab = SpriteDetailTab::ExportPreview;
        if (previewSprites.empty()
            || (previewSourceMode == PreviewSourceMode::Object && previewForObject != spriteSelected))
        {
            if (spriteSelected >= 0)
                InitPreviewFromObject(spriteSelected);
            else
                InitPreviewFromSingleSprites();
        }
    }
    if (isPreview)
        ImGui::PopStyleColor();

    ImGui::SameLine();
    if (isLoaded)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.35f, 1.0f));
    if (Hinted(ImGui::Button(_("Loaded Sprites"), ImVec2(130.0f, 26.0f)), "Loaded Sprites"))
    {
        spriteDetailTab = SpriteDetailTab::LoadedSprites;
    }
    if (isLoaded)
        ImGui::PopStyleColor();

    if (spritesJsonDirty)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "%s", _("● sprites.json modified"));
        ImGui::SameLine();
        if (Hinted(ImGui::Button(_("Save sprites.json")), "Save sprites.json"))
        {
            SaveSpritesJson();
        }
    }

    ImGui::Separator();

    if (spriteDetailTab == SpriteDetailTab::PointEditor)
    {
        DrawPointEditor();
    }
    else if (spriteDetailTab == SpriteDetailTab::Tree)
    {
        if (spriteSelected < 0 || spriteSelected >= static_cast<int>(objects.size()))
        {
            ImGui::TextWrapped(
                "%s",
                _("Pick an object in the list: its dependencies (sequences, composites, animations, sprites) are shown "
                  "here."));
        }
        else
        {
            const SpriteObject &o = objects[static_cast<std::size_t>(spriteSelected)];
            if (spriteTreeFor != spriteSelected)
            {
                spriteTree = spriteCatalog.tree(o);
                spriteTreeFor = spriteSelected;
            }
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", o.name.c_str());
            ImGui::TextDisabled("%s", (o.faction + " / " + o.kindName + " #" + std::to_string(o.id)).c_str());
            ImGui::Text(
                _("%zu sprites, %zu composites, %zu animations, %zu sequences, %zu buildings"),
                o.sprites.size(),
                o.composites.size(),
                o.anims.size(),
                o.sequences.size(),
                o.buildings.size());
            ImGui::Separator();
            DrawDepNode(spriteTree, 0);
        }
    }
    else if (spriteDetailTab == SpriteDetailTab::LoadedSprites)
    {
        DrawLoadedSprites();
    }
    else
    {
        if (previewSourceMode == PreviewSourceMode::Object && spriteSelected >= 0 && previewForObject != spriteSelected)
        {
            InitPreviewFromObject(spriteSelected);
        }
        DrawSpritePreview();
    }
    ImGui::EndChild();
}

void CharacterEditor::DrawSpriteProperties()
{
    EnsureSpriteCatalog();
    if (!spriteCatalogOk)
    {
        ImGui::TextWrapped(
            "%s", spriteReport.empty() ? _("The sprite data could not be loaded.") : spriteReport.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(folderPick.mutex);
        if (folderPick.ready)
        {
            std::strncpy(spriteOutDir.data(), folderPick.path.c_str(), spriteOutDir.size() - 1);
            spriteOutDir.back() = '\0';
            folderPick.ready = false;
        }
    }
    const fs::path outDir = fs::path(spriteOutDir.data());

    if (spritesJsonDirty)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "%s", _("● Unsaved changes in sprites.json"));
        if (Hinted(ImGui::Button(_("Save sprites.json##sp"), ImVec2(140.0f, 26.0f)), "Save sprites.json"))
            SaveSpritesJson();
        ImGui::SameLine();
        if (Hinted(ImGui::Button(_("Revert##sp_rev"), ImVec2(80.0f, 26.0f)), "Revert sprites.json"))
            RevertSpritesJson();
        ImGui::Separator();
    }

    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", _("Export objects"));
    ImGui::TextWrapped(
        "%s", _("One PNG atlas and one JSON per object (unit, building, tileset) with all the sprites it depends on."));
    ImGui::SetNextItemWidth(-130.0f);
    Hinted(ImGui::InputText(_("Output folder"), spriteOutDir.data(), spriteOutDir.size()), "Output folder");
    if (Hinted(ImGui::Button(_("Choose folder...")), "Choose folder..."))
    {
        SDL_ShowOpenFolderDialog(OnFolderChosen, &folderPick, window, nullptr, false);
    }
    ImGui::SameLine();
    const bool haveDir = spriteOutDir[0] != '\0';
    if (Hinted(ImGui::Button(_("Export all objects")), "Export all objects"))
    {
        if (!haveDir)
        {
            spriteReport = _("Choose an output folder first.");
        }
        else
        {
            std::vector<int> all(spriteCatalog.Objects().size());
            std::iota(all.begin(), all.end(), 0);
            ExportSpriteObjects(spriteCatalog, assetsRoot, outDir, all, spriteReport);
            status = spriteReport;
        }
    }
    const bool hasSelection = !spriteChecked.empty();
    ImGui::BeginDisabled(!hasSelection);
    if (Hinted(ImGui::Button(_("Export selected object")), "Export selected object"))
    {
        if (!haveDir)
        {
            spriteReport = _("Choose an output folder first.");
        }
        else
        {
            ExportSpriteObjects(
                spriteCatalog,
                assetsRoot,
                outDir,
                std::vector<int>(spriteChecked.begin(), spriteChecked.end()),
                spriteReport);
            status = spriteReport;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(_("Selected: %zu"), spriteChecked.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(spriteSelected < 0);
    if (Hinted(ImGui::Button(_("Preview Object")), "Preview Object"))
    {
        spriteDetailTab = SpriteDetailTab::ExportPreview;
        InitPreviewFromObject(spriteSelected);
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", _("SINGLE SPRITE EXPORT"));
    ImGui::TextWrapped("%s", _("Every sprite in the order of its id, in rows and columns of one big PNG per group."));
    static const char *const kExportKeys[kSpriteGroupCount]
        = {"Units##sprx", "Buildings##sprx", "Terrain##sprx", "Other##sprx"};
    bool singleChanged = false;
    for (int g = 0; g < kSpriteGroupCount; ++g)
    {
        if (g)
            ImGui::SameLine();
        const std::string label = std::string(_(kGroupLabels[g])) + "##sprx";
        if (Hinted(ImGui::Checkbox(label.c_str(), &singleOptions.groups[g]), kExportKeys[g]))
            singleChanged = true;
    }
    ImGui::SetNextItemWidth(120.0f);
    if (Hinted(ImGui::InputInt(_("Columns"), &singleOptions.columns), "Columns"))
    {
        singleOptions.columns = std::clamp(singleOptions.columns, 1, kMaxExportColumns);
        singleChanged = true;
    }
    if (Hinted(ImGui::Checkbox(_("All in one file"), &singleOptions.allInOne), "All in one file"))
        singleChanged = true;
    if (Hinted(ImGui::Checkbox(_("Compress sprites"), &singleOptions.compress), "Compress sprites"))
        singleChanged = true;
    if (Hinted(ImGui::Checkbox(_("Packed atlas"), &singleOptions.packed), "Packed atlas"))
        singleChanged = true;
    if (singleChanged && spriteDetailTab == SpriteDetailTab::ExportPreview
        && previewSourceMode == PreviewSourceMode::SingleSprites)
    {
        InitPreviewFromSingleSprites();
    }
    if (Hinted(ImGui::Button(_("Export sprites")), "Export sprites"))
    {
        if (!haveDir)
        {
            spriteReport = _("Choose an output folder first.");
        }
        else
        {
            ExportSingleSprites(spriteCatalog, assetsRoot, outDir, singleOptions, spriteReport);
            status = spriteReport;
        }
    }
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Preview Sprites")), "Preview Sprites"))
    {
        spriteDetailTab = SpriteDetailTab::ExportPreview;
        InitPreviewFromSingleSprites();
    }

    // Import: the exports above back into the assets
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", _("IMPORT SPRITES"));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.35f, 1.0f));
    ImGui::TextWrapped(
        "%s",
        _("WARNING: the import writes into assets/sprites and assets/data/sprites_graphics.json. Your data there may "
          "be overwritten."));
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "%s",
        _("Reads the exported object files and single sprite files of a folder: the pixels of the sprites you edited "
          "go back to the sheets, edited table rows to sprites_graphics.json."));
    {
        std::lock_guard<std::mutex> lock(importPick.mutex);
        if (importPick.ready)
        {
            std::strncpy(spriteInDir.data(), importPick.path.c_str(), spriteInDir.size() - 1);
            spriteInDir.back() = '\0';
            importPick.ready = false;
        }
    }
    ImGui::SetNextItemWidth(-130.0f);
    Hinted(ImGui::InputText(_("Import folder"), spriteInDir.data(), spriteInDir.size()), "Import folder");
    if (Hinted(ImGui::Button(_("Choose import folder...")), "Choose import folder..."))
    {
        SDL_ShowOpenFolderDialog(OnFolderChosen, &importPick, window, nullptr, false);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(spriteInDir[0] == '\0');
    if (Hinted(ImGui::Button(_("Import sprites")), "Import sprites"))
        importConfirm = true;
    ImGui::EndDisabled();
    if (importConfirm)
    {
        ImGui::OpenPopup(_("Overwrite assets?"));
        importConfirm = false;
    }
    if (ImGui::BeginPopupModal(_("Overwrite assets?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped(
            "%s",
            _("The import overwrites the sprite sheets in assets/sprites and the tables in "
              "assets/data/sprites_graphics.json with what is in the folder. Your data there may be lost."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", spriteInDir.data());
        ImGui::Spacing();
        if (ImGui::Button(_("Import and overwrite")))
        {
            if (ImportSpriteFolder(assetsRoot, fs::path(spriteInDir.data()), spriteReport))
            {
                if (textures)
                    textures->LoadAllAssets();
                for (auto &[k, t] : wreckThumbs)
                    if (t)
                        SDL_DestroyTexture(t);
                wreckThumbs.clear();
                for (auto &[k, t] : animThumbs)
                    if (t)
                        SDL_DestroyTexture(t);
                animThumbs.clear();
                for (auto &[k, t] : zoomThumbs)
                    if (t)
                        SDL_DestroyTexture(t);
                zoomThumbs.clear();
                spriteCatalogTried = false;
                EnsureSpriteCatalog();
                if (spriteDetailTab == SpriteDetailTab::ExportPreview)
                {
                    if (previewSourceMode == PreviewSourceMode::Object && spriteSelected >= 0)
                        InitPreviewFromObject(spriteSelected);
                    else
                        InitPreviewFromSingleSprites();
                }
            }
            status = spriteReport;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Cancel")))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (!spriteReport.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", spriteReport.c_str());
    }
}

void CharacterEditor::InitPreviewFromObject(int objectIndex)
{
    EnsureSpriteCatalog();
    if (!spriteCatalogOk || objectIndex < 0 || objectIndex >= static_cast<int>(spriteCatalog.Objects().size()))
    {
        previewSprites.clear();
        previewForObject = -1;
        UpdatePreviewCollisions();
        return;
    }
    previewSourceMode = PreviewSourceMode::Object;
    previewForObject = objectIndex;
    previewShowObjectGroups = false;
    const SpriteObject &o = spriteCatalog.Objects()[static_cast<std::size_t>(objectIndex)];

    // Map each sprite of the object to its first sub-component in the dependency tree
    std::map<int, std::pair<int, std::string>> spriteSubGroup;
    const DepNode root = spriteCatalog.tree(o);
    const std::vector<DepNode> *topNodes = &root.children;
    if (root.children.size() == 1 && root.children[0].kind == "building" && !root.children[0].children.empty())
    {
        topNodes = &root.children[0].children;
    }
    auto collectLeafSprites = [&](auto &self, const DepNode &node, int gid, const std::string &glabel) -> void {
        if (node.kind == "sprite" && node.id >= 0)
        {
            spriteSubGroup.try_emplace(node.id, gid, glabel);
        }
        for (const DepNode &ch : node.children)
        {
            self(self, ch, gid, glabel);
        }
    };
    for (std::size_t cIdx = 0; cIdx < topNodes->size(); ++cIdx)
    {
        const DepNode &comp = (*topNodes)[cIdx];
        collectLeafSprites(collectLeafSprites, comp, static_cast<int>(cIdx), o.name + " - " + comp.label);
    }

    previewSprites.clear();
    for (int id : o.sprites)
    {
        if (id < 0 || id >= static_cast<int>(spriteCatalog.Sprites().size()))
            continue;
        const RawSprite &s = spriteCatalog.Sprites()[static_cast<std::size_t>(id)];
        if (s.w <= 0 || s.h <= 0)
            continue;
        SDL_Texture *tex = textures ? textures->GetSpriteSheet(s.page) : nullptr;
        if (!tex)
            continue;
        float tw = 1.0f, th = 1.0f;
        SDL_GetTextureSize(tex, &tw, &th);
        const int scale = std::max(1, static_cast<int>(std::round(textures->GetSheetScale(s.page))));
        const int x0 = std::clamp(s.x * scale, 0, static_cast<int>(tw));
        const int y0 = std::clamp(s.y * scale, 0, static_cast<int>(th));
        const int x1 = std::clamp((s.x + s.w) * scale, 0, static_cast<int>(tw));
        const int y1 = std::clamp((s.y + s.h) * scale, 0, static_cast<int>(th));
        if (x1 <= x0 || y1 <= y0)
            continue;

        PreviewSprite ps;
        ps.id = id;
        ps.page = s.page;
        ps.src = SDL_Rect {x0, y0, x1 - x0, y1 - y0};
        ps.x = 0;
        ps.y = 0;
        ps.w = x1 - x0;
        ps.h = y1 - y0;
        auto it = spriteSubGroup.find(id);
        if (it != spriteSubGroup.end())
        {
            ps.groupId = it->second.first;
            ps.groupLabel = it->second.second;
        }
        else
        {
            ps.groupId = 1000 + s.page;
            ps.groupLabel = o.name + " - Sheet " + std::to_string(s.page);
        }
        ps.colliding = false;
        ps.selected = false;
        previewSprites.push_back(ps);
    }
    LayoutPreviewPack(previewPadding);
    FitPreviewView(previewLastCanvasSize);
}

void CharacterEditor::InitPreviewFromSingleSprites()
{
    EnsureSpriteCatalog();
    if (!spriteCatalogOk)
    {
        previewSprites.clear();
        previewForObject = -1;
        UpdatePreviewCollisions();
        return;
    }
    previewSourceMode = PreviewSourceMode::SingleSprites;
    previewForObject = -1;
    previewShowObjectGroups = false;
    previewSprites.clear();

    // Map each sprite ID to the first SpriteObject that owns it
    const auto &objs = spriteCatalog.Objects();
    std::vector<int> ownerObj(spriteCatalog.Sprites().size(), -1);
    for (std::size_t oi = 0; oi < objs.size(); ++oi)
    {
        for (int sid : objs[oi].sprites)
        {
            if (sid >= 0 && sid < static_cast<int>(ownerObj.size()) && ownerObj[static_cast<std::size_t>(sid)] < 0)
            {
                ownerObj[static_cast<std::size_t>(sid)] = static_cast<int>(oi);
            }
        }
    }

    std::vector<int> byGroup[kSpriteGroupCount];
    for (const RawSprite &s : spriteCatalog.Sprites())
    {
        byGroup[static_cast<int>(spriteCatalog.GroupOfSprite(s.id))].push_back(s.id);
    }
    std::vector<int> ids;
    for (int g = 0; g < kSpriteGroupCount; ++g)
    {
        if (singleOptions.groups[g])
            ids.insert(ids.end(), byGroup[g].begin(), byGroup[g].end());
    }
    for (int id : ids)
    {
        if (id < 0 || id >= static_cast<int>(spriteCatalog.Sprites().size()))
            continue;
        const RawSprite &s = spriteCatalog.Sprites()[static_cast<std::size_t>(id)];
        if (s.w <= 0 || s.h <= 0)
            continue;
        SDL_Texture *tex = textures ? textures->GetSpriteSheet(s.page) : nullptr;
        if (!tex)
            continue;
        float tw = 1.0f, th = 1.0f;
        SDL_GetTextureSize(tex, &tw, &th);
        const int scale = std::max(1, static_cast<int>(std::round(textures->GetSheetScale(s.page))));
        const int x0 = std::clamp(s.x * scale, 0, static_cast<int>(tw));
        const int y0 = std::clamp(s.y * scale, 0, static_cast<int>(th));
        const int x1 = std::clamp((s.x + s.w) * scale, 0, static_cast<int>(tw));
        const int y1 = std::clamp((s.y + s.h) * scale, 0, static_cast<int>(th));
        if (x1 <= x0 || y1 <= y0)
            continue;

        PreviewSprite ps;
        ps.id = id;
        ps.page = s.page;
        ps.src = SDL_Rect {x0, y0, x1 - x0, y1 - y0};
        ps.x = 0;
        ps.y = 0;
        ps.w = x1 - x0;
        ps.h = y1 - y0;
        const int oi = ownerObj[static_cast<std::size_t>(id)];
        if (oi >= 0 && oi < static_cast<int>(objs.size()))
        {
            const SpriteObject &o = objs[static_cast<std::size_t>(oi)];
            ps.groupId = oi;
            ps.groupLabel = (o.kind == SpriteObjectKind::Unit       ? "[#"
                             : o.kind == SpriteObjectKind::Building ? "[B#"
                                                                    : "[")
                            + std::to_string(o.id) + "] " + o.name;
        }
        else
        {
            ps.groupId = 9999;
            ps.groupLabel = "Unassigned";
        }
        ps.colliding = false;
        ps.selected = false;
        previewSprites.push_back(ps);
    }
    previewGridCols = std::clamp(singleOptions.columns, 1, kMaxExportColumns);
    if (singleOptions.packed)
    {
        LayoutPreviewPack(previewPadding);
    }
    else
    {
        LayoutPreviewGrid(previewGridCols, previewPadding, singleOptions.compress);
    }
    FitPreviewView(previewLastCanvasSize);
}

void CharacterEditor::UpdatePreviewCollisions()
{
    previewCollisionCount = 0;
    previewCollidingSpritesCount = 0;
    previewSelectedCount = 0;
    previewCollisionDetail.clear();
    int maxW = 0, maxH = 0;
    for (auto &s : previewSprites)
    {
        s.colliding = false;
        if (s.selected)
            ++previewSelectedCount;
        maxW = std::max(maxW, s.x + s.w);
        maxH = std::max(maxH, s.y + s.h);
    }
    previewAtlasW = maxW;
    previewAtlasH = maxH;
    if (previewSprites.size() < 2)
        return;

    int shownPairs = 0;
    for (std::size_t i = 0; i < previewSprites.size(); ++i)
    {
        auto &a = previewSprites[i];
        for (std::size_t j = i + 1; j < previewSprites.size(); ++j)
        {
            auto &b = previewSprites[j];
            if (!(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y))
            {
                a.colliding = true;
                b.colliding = true;
                ++previewCollisionCount;
                if (shownPairs < 3)
                {
                    if (!previewCollisionDetail.empty())
                        previewCollisionDetail += ", ";
                    previewCollisionDetail += "#" + std::to_string(a.id) + " <-> #" + std::to_string(b.id);
                    ++shownPairs;
                }
            }
        }
    }
    if (previewCollisionCount > shownPairs)
    {
        previewCollisionDetail += " (+" + std::to_string(previewCollisionCount - shownPairs) + ")";
    }
    for (const auto &s : previewSprites)
    {
        if (s.colliding)
            ++previewCollidingSpritesCount;
    }
}

void CharacterEditor::LayoutPreviewLine(bool horizontal, int pad)
{
    if (previewSprites.empty())
        return;
    int cursor = 0;
    for (auto &s : previewSprites)
    {
        if (horizontal)
        {
            s.x = cursor;
            s.y = 0;
            cursor += s.w + pad;
        }
        else
        {
            s.x = 0;
            s.y = cursor;
            cursor += s.h + pad;
        }
    }
    UpdatePreviewCollisions();
}

void CharacterEditor::LayoutPreviewGrid(int cols, int pad, bool compress)
{
    if (previewSprites.empty())
        return;
    cols = std::clamp(cols, 1, static_cast<int>(previewSprites.size()));
    const int rows = (static_cast<int>(previewSprites.size()) + cols - 1) / cols;
    if (!compress)
    {
        int maxW = 1, maxH = 1;
        for (const auto &s : previewSprites)
        {
            maxW = std::max(maxW, s.w);
            maxH = std::max(maxH, s.h);
        }
        const int cellW = maxW + pad;
        const int cellH = maxH + pad;
        for (std::size_t i = 0; i < previewSprites.size(); ++i)
        {
            const int col = static_cast<int>(i) % cols;
            const int row = static_cast<int>(i) / cols;
            previewSprites[i].x = col * cellW;
            previewSprites[i].y = row * cellH;
        }
    }
    else
    {
        std::vector<int> rowHeights(static_cast<std::size_t>(rows), 0);
        for (std::size_t i = 0; i < previewSprites.size(); ++i)
        {
            const std::size_t r = i / static_cast<std::size_t>(cols);
            rowHeights[r] = std::max(rowHeights[r], previewSprites[i].h);
        }
        std::vector<int> rowY(static_cast<std::size_t>(rows), 0);
        int sumY = 0;
        for (std::size_t r = 0; r < rowHeights.size(); ++r)
        {
            rowY[r] = sumY;
            sumY += rowHeights[r] + pad;
        }
        int cursorX = 0;
        for (std::size_t i = 0; i < previewSprites.size(); ++i)
        {
            const int col = static_cast<int>(i) % cols;
            const std::size_t row = i / static_cast<std::size_t>(cols);
            if (col == 0)
                cursorX = 0;
            previewSprites[i].x = cursorX;
            previewSprites[i].y = rowY[row];
            cursorX += previewSprites[i].w + pad;
        }
    }
    UpdatePreviewCollisions();
}

void CharacterEditor::LayoutPreviewPack(int pad)
{
    if (previewSprites.empty())
        return;
    std::vector<CustomAtlasItem> items;
    items.reserve(previewSprites.size());
    for (const auto &s : previewSprites)
    {
        CustomAtlasItem it;
        it.id = s.id;
        it.w = s.w;
        it.h = s.h;
        it.x = 0;
        it.y = 0;
        items.push_back(it);
    }
    int w = 1, h = 1;
    items = PackCustomMaxRects(std::move(items), w, h, pad);
    for (std::size_t i = 0; i < previewSprites.size() && i < items.size(); ++i)
    {
        previewSprites[i].x = items[i].x;
        previewSprites[i].y = items[i].y;
    }
    UpdatePreviewCollisions();
}

void CharacterEditor::LayoutPreviewByObject(int pad)
{
    EnsureSpriteCatalog();
    if (!spriteCatalogOk)
        return;
    const auto &objs = spriteCatalog.Objects();

    // If multiple objects are selected in the left list while in Object mode, load all selected objects so each object
    // and its sprites are grouped
    if (previewSourceMode == PreviewSourceMode::Object && spriteChecked.size() > 1)
    {
        previewSprites.clear();
        std::set<int> addedIds;
        for (int objIdx : spriteChecked)
        {
            if (objIdx < 0 || objIdx >= static_cast<int>(objs.size()))
                continue;
            const SpriteObject &o = objs[static_cast<std::size_t>(objIdx)];
            const std::string gLabel = (o.kind == SpriteObjectKind::Unit       ? "[#"
                                        : o.kind == SpriteObjectKind::Building ? "[B#"
                                                                               : "[")
                                       + std::to_string(o.id) + "] " + o.name;
            for (int id : o.sprites)
            {
                if (!addedIds.insert(id).second)
                    continue;
                if (id < 0 || id >= static_cast<int>(spriteCatalog.Sprites().size()))
                    continue;
                const RawSprite &s = spriteCatalog.Sprites()[static_cast<std::size_t>(id)];
                if (s.w <= 0 || s.h <= 0)
                    continue;
                SDL_Texture *tex = textures ? textures->GetSpriteSheet(s.page) : nullptr;
                if (!tex)
                    continue;
                float tw = 1.0f, th = 1.0f;
                SDL_GetTextureSize(tex, &tw, &th);
                const int scale = std::max(1, static_cast<int>(std::round(textures->GetSheetScale(s.page))));
                const int x0 = std::clamp(s.x * scale, 0, static_cast<int>(tw));
                const int y0 = std::clamp(s.y * scale, 0, static_cast<int>(th));
                const int x1 = std::clamp((s.x + s.w) * scale, 0, static_cast<int>(tw));
                const int y1 = std::clamp((s.y + s.h) * scale, 0, static_cast<int>(th));
                if (x1 <= x0 || y1 <= y0)
                    continue;

                PreviewSprite ps;
                ps.id = id;
                ps.page = s.page;
                ps.src = SDL_Rect {x0, y0, x1 - x0, y1 - y0};
                ps.x = 0;
                ps.y = 0;
                ps.w = x1 - x0;
                ps.h = y1 - y0;
                ps.groupId = objIdx;
                ps.groupLabel = gLabel;
                ps.colliding = false;
                ps.selected = false;
                previewSprites.push_back(ps);
            }
        }
    }

    if (previewSprites.empty())
        return;

    // Group sprites by their assigned groupId (Object or Object sub-component) in stable order
    struct ObjGroup
    {
        int groupId {-1};
        std::string label;
        std::vector<std::size_t> indices;
        int w {0};
        int h {0};
        std::vector<SDL_Point> localPos;
    };
    std::vector<ObjGroup> groups;
    std::map<int, std::size_t> groupMap;
    for (std::size_t i = 0; i < previewSprites.size(); ++i)
    {
        const int gid = previewSprites[i].groupId;
        auto it = groupMap.find(gid);
        if (it == groupMap.end())
        {
            std::size_t newIdx = groups.size();
            groupMap[gid] = newIdx;
            ObjGroup g;
            g.groupId = gid;
            g.label = previewSprites[i].groupLabel.empty() ? ("Group #" + std::to_string(gid))
                                                           : previewSprites[i].groupLabel;
            g.indices.push_back(i);
            groups.push_back(std::move(g));
        }
        else
        {
            groups[it->second].indices.push_back(i);
        }
    }

    // Arrange sprites inside each object group in a neat sub-grid
    const int maxCols = std::max(1, previewGridCols);
    int maxGroupW = 256;
    for (auto &g : groups)
    {
        const int n = static_cast<int>(g.indices.size());
        const int cols = std::clamp(maxCols, 1, n);
        const int rows = (n + cols - 1) / cols;
        std::vector<int> rowHeights(static_cast<std::size_t>(rows), 0);
        for (int k = 0; k < n; ++k)
        {
            const int r = k / cols;
            rowHeights[static_cast<std::size_t>(r)]
                = std::max(rowHeights[static_cast<std::size_t>(r)], previewSprites[g.indices[static_cast<std::size_t>(k)]].h);
        }
        std::vector<int> rowY(static_cast<std::size_t>(rows), 0);
        int sumY = 0;
        for (int r = 0; r < rows; ++r)
        {
            rowY[static_cast<std::size_t>(r)] = sumY;
            sumY += rowHeights[static_cast<std::size_t>(r)] + pad;
        }
        g.localPos.resize(static_cast<std::size_t>(n));
        int cursorX = 0;
        int gw = 0;
        for (int k = 0; k < n; ++k)
        {
            const int c = k % cols;
            const int r = k / cols;
            if (c == 0)
                cursorX = 0;
            const auto &sp = previewSprites[g.indices[static_cast<std::size_t>(k)]];
            g.localPos[static_cast<std::size_t>(k)] = SDL_Point {cursorX, rowY[static_cast<std::size_t>(r)]};
            cursorX += sp.w + pad;
            gw = std::max(gw, cursorX - pad);
        }
        const int minLabelW = static_cast<int>(g.label.size()) * 7 + 44;
        g.w = std::max(gw, minLabelW);
        g.h = std::max(1, sumY - pad);
        maxGroupW = std::max(maxGroupW, g.w);
    }

    // Place object groups cleanly: vertical stack when <= 10 groups, multi-column shelf when many objects
    const int headerH = 20;
    const int gapX = std::max(20, pad * 6 + 12);
    const int gapY = std::max(28, headerH + pad * 4 + 8);
    const int targetRowWidth = (groups.size() <= 10) ? maxGroupW : std::max(maxGroupW, 1400);

    int curX = 6;
    int curY = headerH + 4;
    int shelfH = 0;
    for (const auto &g : groups)
    {
        if (curX > 6 && curX + g.w > targetRowWidth)
        {
            curX = 6;
            curY += shelfH + gapY;
            shelfH = 0;
        }
        for (std::size_t k = 0; k < g.indices.size(); ++k)
        {
            auto &sp = previewSprites[g.indices[k]];
            sp.x = curX + g.localPos[k].x;
            sp.y = curY + g.localPos[k].y;
        }
        curX += g.w + gapX;
        shelfH = std::max(shelfH, g.h);
    }

    previewShowObjectGroups = true;
    UpdatePreviewCollisions();
}

void CharacterEditor::LayoutPreviewScatter()
{
    if (previewSprites.empty())
        return;
    const int cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(previewSprites.size()) * 1.4)));
    int maxW = 48, maxH = 48;
    for (const auto &s : previewSprites)
    {
        maxW = std::max(maxW, s.w);
        maxH = std::max(maxH, s.h);
    }
    const int pad = std::max(16, previewPadding * 4);
    const int cellW = maxW + pad;
    const int cellH = maxH + pad;
    const int jitter = std::max(2, pad / 3);
    const int span = jitter * 2 + 1;
    for (std::size_t i = 0; i < previewSprites.size(); ++i)
    {
        const int col = static_cast<int>(i) % cols;
        const int row = static_cast<int>(i) / cols;
        const int jx = ((previewSprites[i].id * 37 + static_cast<int>(i) * 13) % span) - jitter;
        const int jy = ((previewSprites[i].id * 53 + static_cast<int>(i) * 29) % span) - jitter;
        previewSprites[i].x = std::max(0, col * cellW + jx);
        previewSprites[i].y = std::max(0, row * cellH + jy);
    }
    UpdatePreviewCollisions();
}

void CharacterEditor::LayoutPreviewSeparateOverlaps(int pad)
{
    if (previewSprites.size() < 2)
        return;
    for (int iter = 0; iter < 150; ++iter)
    {
        bool anyOverlap = false;
        for (std::size_t i = 0; i < previewSprites.size(); ++i)
        {
            auto &a = previewSprites[i];
            for (std::size_t j = i + 1; j < previewSprites.size(); ++j)
            {
                auto &b = previewSprites[j];
                const int ox1 = std::max(a.x, b.x);
                const int oy1 = std::max(a.y, b.y);
                const int ox2 = std::min(a.x + a.w, b.x + b.w);
                const int oy2 = std::min(a.y + a.h, b.y + b.h);
                if (ox1 < ox2 && oy1 < oy2)
                {
                    anyOverlap = true;
                    const int overlapX = (ox2 - ox1) + pad;
                    const int overlapY = (oy2 - oy1) + pad;
                    const int caX = a.x * 2 + a.w;
                    const int cbX = b.x * 2 + b.w;
                    const int caY = a.y * 2 + a.h;
                    const int cbY = b.y * 2 + b.h;
                    if (overlapX <= overlapY)
                    {
                        const int shift = (overlapX + 1) / 2;
                        if (caX < cbX || (caX == cbX && i < j))
                        {
                            a.x -= shift;
                            b.x += shift;
                        }
                        else
                        {
                            a.x += shift;
                            b.x -= shift;
                        }
                    }
                    else
                    {
                        const int shift = (overlapY + 1) / 2;
                        if (caY < cbY || (caY == cbY && i < j))
                        {
                            a.y -= shift;
                            b.y += shift;
                        }
                        else
                        {
                            a.y += shift;
                            b.y -= shift;
                        }
                    }
                }
            }
        }
        if (!anyOverlap)
            break;
    }
    int minX = 0, minY = 0;
    for (const auto &s : previewSprites)
    {
        minX = std::min(minX, s.x);
        minY = std::min(minY, s.y);
    }
    if (minX < 0 || minY < 0)
    {
        for (auto &s : previewSprites)
        {
            s.x = std::max(0, s.x - minX);
            s.y = std::max(0, s.y - minY);
        }
    }
    UpdatePreviewCollisions();

    // Guaranteed fallback if dense clusters still have residual collisions after relaxation
    if (previewCollisionCount > 0)
    {
        auto overlapsAny = [&](std::size_t idx, int x, int y) {
            const int w = previewSprites[idx].w;
            const int h = previewSprites[idx].h;
            for (std::size_t k = 0; k < previewSprites.size(); ++k)
            {
                if (k == idx)
                    continue;
                const auto &o = previewSprites[k];
                if (!(x + w + pad <= o.x || o.x + o.w + pad <= x || y + h + pad <= o.y || o.y + o.h + pad <= y))
                {
                    return true;
                }
            }
            return false;
        };
        for (std::size_t i = 0; i < previewSprites.size(); ++i)
        {
            if (!previewSprites[i].colliding)
                continue;
            if (!overlapsAny(i, previewSprites[i].x, previewSprites[i].y))
                continue;
            int step = std::max(4, pad + 2);
            bool placed = false;
            for (int radius = step; radius < 4096 && !placed; radius += step)
            {
                for (int dy = -radius; dy <= radius && !placed; dy += step)
                {
                    for (int dx = -radius; dx <= radius && !placed; dx += step)
                    {
                        if (std::abs(dx) != radius && std::abs(dy) != radius)
                            continue;
                        int nx = std::max(0, previewSprites[i].x + dx);
                        int ny = std::max(0, previewSprites[i].y + dy);
                        if (!overlapsAny(i, nx, ny))
                        {
                            previewSprites[i].x = nx;
                            previewSprites[i].y = ny;
                            placed = true;
                        }
                    }
                }
            }
        }
        UpdatePreviewCollisions();
    }
}

void CharacterEditor::LayoutPreviewAlign(int mode)
{
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < previewSprites.size(); ++i)
    {
        if (previewSprites[i].selected)
            indices.push_back(i);
    }
    if (indices.empty())
    {
        for (std::size_t i = 0; i < previewSprites.size(); ++i)
            indices.push_back(i);
    }
    if (indices.empty())
        return;

    if (mode == 0)
    { // Align Left
        int minX = previewSprites[indices[0]].x;
        for (std::size_t idx : indices)
            minX = std::min(minX, previewSprites[idx].x);
        minX = std::max(0, minX);
        for (std::size_t idx : indices)
            previewSprites[idx].x = minX;
    }
    else if (mode == 1)
    { // Align Top
        int minY = previewSprites[indices[0]].y;
        for (std::size_t idx : indices)
            minY = std::min(minY, previewSprites[idx].y);
        minY = std::max(0, minY);
        for (std::size_t idx : indices)
            previewSprites[idx].y = minY;
    }
    else if (mode == 2)
    { // Align Right
        int maxX = previewSprites[indices[0]].x + previewSprites[indices[0]].w;
        for (std::size_t idx : indices)
            maxX = std::max(maxX, previewSprites[idx].x + previewSprites[idx].w);
        for (std::size_t idx : indices)
            previewSprites[idx].x = std::max(0, maxX - previewSprites[idx].w);
    }
    else if (mode == 3)
    { // Align Bottom
        int maxY = previewSprites[indices[0]].y + previewSprites[indices[0]].h;
        for (std::size_t idx : indices)
            maxY = std::max(maxY, previewSprites[idx].y + previewSprites[idx].h);
        for (std::size_t idx : indices)
            previewSprites[idx].y = std::max(0, maxY - previewSprites[idx].h);
    }
    else if (mode == 4 && indices.size() >= 2)
    { // Distribute H (accounting for sprite widths)
        std::sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
            if (previewSprites[a].x != previewSprites[b].x)
                return previewSprites[a].x < previewSprites[b].x;
            return previewSprites[a].id < previewSprites[b].id;
        });
        int minX = previewSprites[indices.front()].x;
        int maxRight = previewSprites[indices.front()].x + previewSprites[indices.front()].w;
        int totalW = 0;
        for (std::size_t idx : indices)
        {
            minX = std::min(minX, previewSprites[idx].x);
            maxRight = std::max(maxRight, previewSprites[idx].x + previewSprites[idx].w);
            totalW += previewSprites[idx].w;
        }
        const int span = maxRight - minX;
        const float gap = (span > totalW + static_cast<int>(indices.size() - 1) * previewPadding)
                              ? static_cast<float>(span - totalW) / static_cast<float>(indices.size() - 1)
                              : static_cast<float>(previewPadding);
        float cursor = static_cast<float>(std::max(0, minX));
        for (std::size_t idx : indices)
        {
            previewSprites[idx].x = static_cast<int>(std::round(cursor));
            cursor += static_cast<float>(previewSprites[idx].w) + gap;
        }
    }
    else if (mode == 5 && indices.size() >= 2)
    { // Distribute V (accounting for sprite heights)
        std::sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
            if (previewSprites[a].y != previewSprites[b].y)
                return previewSprites[a].y < previewSprites[b].y;
            return previewSprites[a].id < previewSprites[b].id;
        });
        int minY = previewSprites[indices.front()].y;
        int maxBottom = previewSprites[indices.front()].y + previewSprites[indices.front()].h;
        int totalH = 0;
        for (std::size_t idx : indices)
        {
            minY = std::min(minY, previewSprites[idx].y);
            maxBottom = std::max(maxBottom, previewSprites[idx].y + previewSprites[idx].h);
            totalH += previewSprites[idx].h;
        }
        const int span = maxBottom - minY;
        const float gap = (span > totalH + static_cast<int>(indices.size() - 1) * previewPadding)
                              ? static_cast<float>(span - totalH) / static_cast<float>(indices.size() - 1)
                              : static_cast<float>(previewPadding);
        float cursor = static_cast<float>(std::max(0, minY));
        for (std::size_t idx : indices)
        {
            previewSprites[idx].y = static_cast<int>(std::round(cursor));
            cursor += static_cast<float>(previewSprites[idx].h) + gap;
        }
    }
    UpdatePreviewCollisions();
}

void CharacterEditor::FitPreviewView(const ImVec2 &canvasSize)
{
    if (previewSprites.empty() || canvasSize.x <= 20.0f || canvasSize.y <= 20.0f)
        return;
    int maxW = std::max(1, previewAtlasW);
    int maxH = std::max(1, previewAtlasH);
    const float pad = 24.0f;
    const float availX = std::max(16.0f, canvasSize.x - pad * 2.0f);
    const float availY = std::max(16.0f, canvasSize.y - pad * 2.0f);
    const float zoomX = availX / static_cast<float>(maxW);
    const float zoomY = availY / static_cast<float>(maxH);
    previewZoomLevel = std::clamp(std::min(zoomX, zoomY), 0.05f, 4.0f);
    previewPanX = std::max(8.0f, (canvasSize.x - static_cast<float>(maxW) * previewZoomLevel) * 0.5f);
    previewPanY = std::max(20.0f, (canvasSize.y - static_cast<float>(maxH) * previewZoomLevel) * 0.5f);
}

bool CharacterEditor::ExportCurrentPreview(std::string &report)
{
    if (previewSprites.empty())
    {
        report = _("No sprites to export.");
        return false;
    }
    const fs::path outDir = fs::path(spriteOutDir.data());
    if (spriteOutDir[0] == '\0')
    {
        report = _("Choose an output folder first in Properties panel.");
        return false;
    }

    std::vector<CustomAtlasItem> items;
    items.reserve(previewSprites.size());
    for (const auto &s : previewSprites)
    {
        CustomAtlasItem it;
        it.id = s.id;
        it.x = s.x;
        it.y = s.y;
        it.w = s.w;
        it.h = s.h;
        items.push_back(it);
    }

    std::string stem;
    std::string title;
    nlohmann::json extraData = nlohmann::json::object();

    if (previewSourceMode == PreviewSourceMode::Object && previewForObject >= 0
        && previewForObject < static_cast<int>(spriteCatalog.Objects().size()))
    {
        const SpriteObject &o = spriteCatalog.Objects()[static_cast<std::size_t>(previewForObject)];
        std::string faction = o.faction;
        std::transform(
            faction.begin(), faction.end(), faction.begin(), [](unsigned char c) { return std::tolower(c); });
        char buf[256];
        std::snprintf(
            buf,
            sizeof buf,
            "%03d_%s_%s_%s_id%d_custom",
            previewForObject + 1,
            o.kindName.c_str(),
            faction.c_str(),
            o.slug.c_str(),
            o.id);
        stem = buf;
        title = o.name + " (Custom Layout)";

        extraData["object"]
            = {{"kind", o.kindName}, {"id", o.id}, {"faction", o.faction}, {"name", o.name}, {"slug", o.slug}};
        if (o.kind == SpriteObjectKind::Unit)
            extraData["object"]["archetype_id"] = o.id;
        if (o.kind == SpriteObjectKind::Building)
            extraData["object"]["building_index"] = o.id;
        if (o.kind == SpriteObjectKind::Tileset)
            extraData["object"]["tileset"] = o.id;
        extraData["graphics"] = spriteCatalog.Describe(o);
    }
    else
    {
        stem = "sprites_custom";
        title = "Custom Sprites Layout";
    }

    bool ok = ExportCustomAtlas(
        spriteCatalog,
        assetsRoot,
        outDir,
        stem,
        title,
        items,
        previewAtlasW,
        previewAtlasH,
        extraData.empty() ? nullptr : &extraData,
        report);
    status = report;
    previewExportReport = report;
    return ok;
}

void CharacterEditor::DrawSpritePreview()
{
    // 1. Source and reload
    bool modeObj = previewSourceMode == PreviewSourceMode::Object;
    if (ImGui::RadioButton(_("Selected Object"), modeObj))
    {
        if (!modeObj)
        {
            InitPreviewFromObject(spriteSelected);
        }
    }
    ImGui::SameLine();
    bool modeSingle = previewSourceMode == PreviewSourceMode::SingleSprites;
    if (ImGui::RadioButton(_("Single Sprites Set"), modeSingle))
    {
        if (!modeSingle)
        {
            InitPreviewFromSingleSprites();
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(_("↻ Reload")))
    {
        if (previewSourceMode == PreviewSourceMode::Object)
            InitPreviewFromObject(spriteSelected);
        else
            InitPreviewFromSingleSprites();
    }

    ImGui::SameLine();
    if (previewCollisionCount > 0)
    {
        ImGui::TextColored(
            ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            _("⚠️ %d Collisions (%d sprites): %s"),
            previewCollisionCount,
            previewCollidingSpritesCount,
            previewCollisionDetail.c_str());
    }
    else
    {
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), _("✓ No overlaps"));
    }

    ImGui::SameLine();
    ImGui::TextDisabled(
        _("| %zu sprites, Atlas: %dx%d px | Zoom: %.1fx"),
        previewSprites.size(),
        previewAtlasW,
        previewAtlasH,
        previewZoomLevel);

    // 2. Toolbar: Layout actions
    if (Hinted(ImGui::Button(_("Line (Strip)")), "Line (Strip)"))
    {
        previewShowObjectGroups = false;
        LayoutPreviewLine(true, previewPadding);
    }
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Grid")), "Grid"))
    {
        previewShowObjectGroups = false;
        LayoutPreviewGrid(previewGridCols, previewPadding, true);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(65.0f);
    if (ImGui::InputInt("##previewcols", &previewGridCols, 1, 8))
    {
        previewGridCols = std::clamp(previewGridCols, 1, kMaxExportColumns);
    }
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Pack (MaxRects)")), "Pack (MaxRects)"))
    {
        previewShowObjectGroups = false;
        LayoutPreviewPack(previewPadding);
    }
    ImGui::SameLine();
    if (previewShowObjectGroups)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.50f, 0.42f, 1.0f));
    }
    if (Hinted(ImGui::Button(_("By Object")), "By Object"))
    {
        LayoutPreviewByObject(previewPadding);
    }
    if (previewShowObjectGroups)
    {
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Scatter")), "Scatter"))
    {
        previewShowObjectGroups = false;
        LayoutPreviewScatter();
    }
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Separate Overlaps")), "Separate Overlaps"))
    {
        LayoutPreviewSeparateOverlaps(previewPadding);
    }
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Fit View")), "Fit View"))
    {
        FitPreviewView(previewLastCanvasSize);
    }

    // 3. Second Toolbar Row: Align, Snap, Padding, Export
    ImGui::TextUnformatted(_("Pad:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(45.0f);
    if (ImGui::InputInt("##pad", &previewPadding, 1))
    {
        previewPadding = std::clamp(previewPadding, 0, 128);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(_("Snap:"));
    ImGui::SameLine();
    static const int kSnapValues[] = {1, 2, 4, 8, 16};
    static const char *const kSnapLabels[] = {"Off", "2px", "4px", "8px", "16px"};
    int snapIdx = 0;
    for (int k = 0; k < 5; ++k)
    {
        if (previewSnapGrid == kSnapValues[k])
        {
            snapIdx = k;
            break;
        }
    }
    ImGui::SetNextItemWidth(65.0f);
    if (ImGui::Combo("##snapgrid", &snapIdx, kSnapLabels, 5))
    {
        previewSnapGrid = kSnapValues[snapIdx];
    }
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Align L")), "Align L"))
        LayoutPreviewAlign(0);
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Align R")), "Align R"))
        LayoutPreviewAlign(2);
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Align T")), "Align T"))
        LayoutPreviewAlign(1);
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Align B")), "Align B"))
        LayoutPreviewAlign(3);
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Distrib H")), "Distrib H"))
        LayoutPreviewAlign(4);
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Distrib V")), "Distrib V"))
        LayoutPreviewAlign(5);

    ImGui::SameLine();
    if (previewCollisionCount > 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.20f, 0.20f, 1.0f));
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
    }
    if (Hinted(ImGui::Button(_("Export Layout")), "Export Layout"))
    {
        if (previewCollisionCount > 0)
        {
            previewExportReport = _("ERROR: Overlapping sprites detected! Fix collisions or confirm export.");
            status = previewExportReport;
            previewExportConfirm = true;
        }
        else
        {
            ExportCurrentPreview(previewExportReport);
        }
    }
    ImGui::PopStyleColor();

    if (previewExportConfirm)
    {
        ImGui::OpenPopup(_("Overlapping Sprites Error"));
        previewExportConfirm = false;
    }
    if (ImGui::BeginPopupModal(_("Overlapping Sprites Error"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::Text(
            _("ERROR: %d overlap(s) across %d sprite(s) (%s)!"),
            previewCollisionCount,
            previewCollidingSpritesCount,
            previewCollisionDetail.c_str());
        ImGui::PopStyleColor();
        ImGui::TextWrapped(
            "%s",
            _("Exporting overlapping sprites will cause sprites to overwrite each other in the PNG atlas and corrupt "
              "re-imported graphics."));
        ImGui::Spacing();
        if (ImGui::Button(_("Separate Overlaps & Export")))
        {
            LayoutPreviewSeparateOverlaps(previewPadding);
            ExportCurrentPreview(previewExportReport);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Export Anyway")))
        {
            ExportCurrentPreview(previewExportReport);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Cancel")))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!previewExportReport.empty())
    {
        ImGui::SameLine();
        const ImVec4 repColor = (previewCollisionCount > 0 && previewExportReport.find("ERROR") != std::string::npos)
                                    ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                                    : ImVec4(0.9f, 0.85f, 0.4f, 1.0f);
        ImGui::TextColored(repColor, "%s", previewExportReport.c_str());
    }

    // 4. Canvas Viewport
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    const float footerH = 28.0f;
    const ImVec2 drawSize(std::max(10.0f, canvasSize.x), std::max(10.0f, canvasSize.y - footerH));
    previewLastCanvasSize = drawSize;
    DrawSpritePreviewCanvas(drawSize);

    // 5. Footer info
    ImGui::TextDisabled(
        _("LMB: Select | Drag: Move | Space+LMB / MMB / RMB: Pan | Wheel: Zoom | Arrows: Nudge (Shift=10px) | "
          "Selected: %d"),
        previewSelectedCount);
    if (previewSelectedCount == 1)
    {
        for (auto &s : previewSprites)
        {
            if (s.selected)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "[Sprite #%d: %dx%d]", s.id, s.w, s.h);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(65.0f);
                if (ImGui::InputInt("X##spx", &s.x))
                {
                    s.x = std::max(0, s.x);
                    UpdatePreviewCollisions();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(65.0f);
                if (ImGui::InputInt("Y##spy", &s.y))
                {
                    s.y = std::max(0, s.y);
                    UpdatePreviewCollisions();
                }
                break;
            }
        }
    }
}

void CharacterEditor::DrawSpritePreviewCanvas(const ImVec2 &size)
{
    ImGui::BeginChild(
        "sprite-preview-canvas",
        size,
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Checkered background
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(22, 26, 24, 255));
    constexpr float checkSize = 16.0f;
    const int checksX = static_cast<int>(size.x / checkSize) + 1;
    const int checksY = static_cast<int>(size.y / checkSize) + 1;
    for (int cy = 0; cy < checksY; ++cy)
    {
        for (int cx = 0; cx < checksX; ++cx)
        {
            if ((cx + cy) % 2 == 1)
            {
                const ImVec2 c0(origin.x + cx * checkSize, origin.y + cy * checkSize);
                const ImVec2 c1(
                    std::min(origin.x + size.x, c0.x + checkSize), std::min(origin.y + size.y, c0.y + checkSize));
                dl->AddRectFilled(c0, c1, IM_COL32(28, 33, 30, 255));
            }
        }
    }

    // Atlas bounding rectangle
    const ImVec2 atlasMin(origin.x + previewPanX, origin.y + previewPanY);
    const ImVec2 atlasMax(atlasMin.x + previewAtlasW * previewZoomLevel, atlasMin.y + previewAtlasH * previewZoomLevel);
    const ImU32 atlasBorderCol = previewCollisionCount > 0 ? IM_COL32(230, 65, 65, 220) : IM_COL32(80, 140, 100, 180);
    dl->AddRect(atlasMin, atlasMax, atlasBorderCol, 0.0f, 0, 1.5f);
    dl->AddText(
        ImVec2(atlasMin.x + 4.0f, atlasMin.y - 18.0f),
        previewCollisionCount > 0 ? IM_COL32(255, 95, 95, 255) : IM_COL32(120, 180, 140, 255),
        (std::to_string(previewAtlasW) + " x " + std::to_string(previewAtlasH) + " px").c_str());

    // Canvas interaction
    ImGui::InvisibleButton(
        "canvas_click_target",
        size,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool canvasHovered = ImGui::IsItemHovered();
    const bool canvasActive = ImGui::IsItemActive();
    const ImGuiIO &io = ImGui::GetIO();

    // Zooming with mouse wheel
    if (canvasHovered && io.MouseWheel != 0.0f)
    {
        const float oldZoom = previewZoomLevel;
        const float factor = io.MouseWheel > 0 ? 1.15f : (1.0f / 1.15f);
        previewZoomLevel = std::clamp(previewZoomLevel * factor, 0.05f, 16.0f);
        const float relX = io.MousePos.x - (origin.x + previewPanX);
        const float relY = io.MousePos.y - (origin.y + previewPanY);
        previewPanX = io.MousePos.x - origin.x - relX * (previewZoomLevel / oldZoom);
        previewPanY = io.MousePos.y - origin.y - relY * (previewZoomLevel / oldZoom);
    }

    // Panning
    const bool isSpaceDown = ImGui::IsKeyDown(ImGuiKey_Space);
    const bool isPanning = ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f)
                           || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f)
                           || (isSpaceDown && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f));
    if (canvasActive && isPanning)
    {
        previewPanX += io.MouseDelta.x;
        previewPanY += io.MouseDelta.y;
    }

    const float mouseAtlasX = (io.MousePos.x - origin.x - previewPanX) / previewZoomLevel;
    const float mouseAtlasY = (io.MousePos.y - origin.y - previewPanY) / previewZoomLevel;

    // Compute dynamic object group bounds when By Object grouping is active
    struct GroupBounds
    {
        int groupId {-1};
        std::string label;
        int minX {0}, minY {0}, maxX {0}, maxY {0};
        int count {0};
        bool anySelected {false};
        ImVec2 headerMin {0.0f, 0.0f};
        ImVec2 headerMax {0.0f, 0.0f};
    };
    std::vector<GroupBounds> groupBounds;
    int hoveredGroupHeader = -1;
    if (previewShowObjectGroups && !previewSprites.empty())
    {
        std::map<int, std::size_t> gMap;
        for (const auto &s : previewSprites)
        {
            auto it = gMap.find(s.groupId);
            if (it == gMap.end())
            {
                std::size_t idx = groupBounds.size();
                gMap[s.groupId] = idx;
                GroupBounds gb;
                gb.groupId = s.groupId;
                gb.label = s.groupLabel.empty() ? ("Group #" + std::to_string(s.groupId)) : s.groupLabel;
                gb.minX = s.x;
                gb.minY = s.y;
                gb.maxX = s.x + s.w;
                gb.maxY = s.y + s.h;
                gb.count = 1;
                gb.anySelected = s.selected;
                groupBounds.push_back(std::move(gb));
            }
            else
            {
                auto &gb = groupBounds[it->second];
                gb.minX = std::min(gb.minX, s.x);
                gb.minY = std::min(gb.minY, s.y);
                gb.maxX = std::max(gb.maxX, s.x + s.w);
                gb.maxY = std::max(gb.maxY, s.y + s.h);
                gb.count++;
                if (s.selected)
                    gb.anySelected = true;
            }
        }
        for (std::size_t gi = 0; gi < groupBounds.size(); ++gi)
        {
            auto &gb = groupBounds[gi];
            const ImVec2 g0(
                origin.x + previewPanX + (gb.minX - 3) * previewZoomLevel,
                origin.y + previewPanY + (gb.minY - 3) * previewZoomLevel);
            const ImVec2 g1(
                origin.x + previewPanX + (gb.maxX + 3) * previewZoomLevel,
                origin.y + previewPanY + (gb.maxY + 3) * previewZoomLevel);
            const std::string title = gb.label + " (" + std::to_string(gb.count) + ")";
            const ImVec2 tSz = ImGui::CalcTextSize(title.c_str());
            gb.headerMin = ImVec2(g0.x, g0.y - 17.0f);
            gb.headerMax = ImVec2(std::max(g1.x, g0.x + tSz.x + 10.0f), g0.y);

            if (canvasHovered && !isSpaceDown && !isPanning && !previewMarquee && io.MousePos.x >= gb.headerMin.x
                && io.MousePos.x <= gb.headerMax.x && io.MousePos.y >= gb.headerMin.y
                && io.MousePos.y <= gb.headerMax.y)
            {
                hoveredGroupHeader = static_cast<int>(gi);
            }
        }
    }

    // Hover detection (top-most in reverse order)
    int hoveredIdx = -1;
    if (canvasHovered && !isSpaceDown && !isPanning && !previewMarquee)
    {
        for (int i = static_cast<int>(previewSprites.size()) - 1; i >= 0; --i)
        {
            const auto &s = previewSprites[static_cast<std::size_t>(i)];
            if (mouseAtlasX >= s.x && mouseAtlasX <= s.x + s.w && mouseAtlasY >= s.y && mouseAtlasY <= s.y + s.h)
            {
                hoveredIdx = i;
                break;
            }
        }
    }

    // Double-click on a sprite selects all sprites of the same Object / group
    if (canvasHovered && hoveredIdx >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !isSpaceDown)
    {
        const int targetGroup = previewSprites[static_cast<std::size_t>(hoveredIdx)].groupId;
        if (!io.KeyCtrl)
        {
            for (auto &s : previewSprites)
                s.selected = false;
        }
        for (auto &s : previewSprites)
        {
            if (s.groupId == targetGroup)
                s.selected = true;
        }
        previewClickedIndex = -1;
        UpdatePreviewCollisions();
    }

    // Left click handling: selection and drag start
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !isSpaceDown)
    {
        if (hoveredIdx >= 0)
        {
            previewClickedIndex = -1;
            if (io.KeyCtrl)
            {
                previewSprites[static_cast<std::size_t>(hoveredIdx)].selected
                    = !previewSprites[static_cast<std::size_t>(hoveredIdx)].selected;
            }
            else if (!previewSprites[static_cast<std::size_t>(hoveredIdx)].selected)
            {
                for (auto &s : previewSprites)
                    s.selected = false;
                previewSprites[static_cast<std::size_t>(hoveredIdx)].selected = true;
            }
            else
            {
                // Clicked an already-selected sprite without Ctrl: if user releases without dragging, select only this
                // one
                previewClickedIndex = hoveredIdx;
            }
            previewDragging = previewSprites[static_cast<std::size_t>(hoveredIdx)].selected;
            previewDragMoved = false;
            previewDragStartMouse = io.MousePos;
            previewOriginalPositions.clear();
            for (const auto &s : previewSprites)
            {
                previewOriginalPositions.push_back(ImVec2(static_cast<float>(s.x), static_cast<float>(s.y)));
            }
            UpdatePreviewCollisions();
        }
        else if (hoveredGroupHeader >= 0 && hoveredGroupHeader < static_cast<int>(groupBounds.size()))
        {
            // Clicking an Object group header selects all sprites of that object and allows dragging the whole object
            // at once
            const int targetGroup = groupBounds[static_cast<std::size_t>(hoveredGroupHeader)].groupId;
            if (!io.KeyCtrl)
            {
                for (auto &s : previewSprites)
                    s.selected = false;
            }
            for (auto &s : previewSprites)
            {
                if (s.groupId == targetGroup)
                    s.selected = true;
            }
            previewClickedIndex = -1;
            previewDragging = true;
            previewDragMoved = false;
            previewDragStartMouse = io.MousePos;
            previewOriginalPositions.clear();
            for (const auto &s : previewSprites)
            {
                previewOriginalPositions.push_back(ImVec2(static_cast<float>(s.x), static_cast<float>(s.y)));
            }
            UpdatePreviewCollisions();
        }
        else
        {
            if (!io.KeyCtrl)
            {
                for (auto &s : previewSprites)
                    s.selected = false;
                UpdatePreviewCollisions();
            }
            previewMarquee = true;
            previewMarqueeStart = io.MousePos;
            previewMarqueeEnd = io.MousePos;
        }
    }

    // Dragging
    if (previewDragging)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const float rawDx = io.MousePos.x - previewDragStartMouse.x;
            const float rawDy = io.MousePos.y - previewDragStartMouse.y;
            if (std::abs(rawDx) > 1.5f || std::abs(rawDy) > 1.5f)
            {
                previewDragMoved = true;
            }
            if (previewDragMoved)
            {
                const float dx = rawDx / previewZoomLevel;
                const float dy = rawDy / previewZoomLevel;
                for (std::size_t i = 0; i < previewSprites.size(); ++i)
                {
                    if (previewSprites[i].selected && i < previewOriginalPositions.size())
                    {
                        int newX = static_cast<int>(std::round(previewOriginalPositions[i].x + dx));
                        int newY = static_cast<int>(std::round(previewOriginalPositions[i].y + dy));
                        if (previewSnapGrid > 1)
                        {
                            newX = (newX / previewSnapGrid) * previewSnapGrid;
                            newY = (newY / previewSnapGrid) * previewSnapGrid;
                        }
                        previewSprites[i].x = std::max(0, newX);
                        previewSprites[i].y = std::max(0, newY);
                    }
                }
                UpdatePreviewCollisions();
            }
        }
        else
        {
            if (!previewDragMoved && previewClickedIndex >= 0
                && previewClickedIndex < static_cast<int>(previewSprites.size()))
            {
                for (auto &s : previewSprites)
                    s.selected = false;
                previewSprites[static_cast<std::size_t>(previewClickedIndex)].selected = true;
                UpdatePreviewCollisions();
            }
            previewDragging = false;
            previewDragMoved = false;
            previewClickedIndex = -1;
        }
    }

    // Marquee selection
    if (previewMarquee)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            previewMarqueeEnd = io.MousePos;
        }
        else
        {
            previewMarquee = false;
            const float mx0 = std::min(previewMarqueeStart.x, io.MousePos.x);
            const float my0 = std::min(previewMarqueeStart.y, io.MousePos.y);
            const float mx1 = std::max(previewMarqueeStart.x, io.MousePos.x);
            const float my1 = std::max(previewMarqueeStart.y, io.MousePos.y);
            const float ax0 = (mx0 - origin.x - previewPanX) / previewZoomLevel;
            const float ay0 = (my0 - origin.y - previewPanY) / previewZoomLevel;
            const float ax1 = (mx1 - origin.x - previewPanX) / previewZoomLevel;
            const float ay1 = (my1 - origin.y - previewPanY) / previewZoomLevel;
            if (std::abs(mx1 - mx0) > 4.0f || std::abs(my1 - my0) > 4.0f)
            {
                for (auto &s : previewSprites)
                {
                    if (!(s.x + s.w < ax0 || s.x > ax1 || s.y + s.h < ay0 || s.y > ay1))
                    {
                        s.selected = true;
                    }
                }
                UpdatePreviewCollisions();
            }
        }
    }

    // Keyboard controls
    if (canvasActive || canvasHovered)
    {
        int nudgeX = 0, nudgeY = 0;
        const int step = io.KeyShift ? 10 : 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            nudgeX -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            nudgeX += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            nudgeY -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            nudgeY += step;
        if (nudgeX != 0 || nudgeY != 0)
        {
            for (auto &s : previewSprites)
            {
                if (s.selected)
                {
                    s.x = std::max(0, s.x + nudgeX);
                    s.y = std::max(0, s.y + nudgeY);
                }
            }
            UpdatePreviewCollisions();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))
        {
            for (auto &s : previewSprites)
                s.selected = true;
            UpdatePreviewCollisions();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            for (auto &s : previewSprites)
                s.selected = false;
            UpdatePreviewCollisions();
        }
    }

    // Draw Object Group frames & clickable headers when "By Object" mode is active
    if (previewShowObjectGroups && !groupBounds.empty())
    {
        for (std::size_t gi = 0; gi < groupBounds.size(); ++gi)
        {
            const auto &gb = groupBounds[gi];
            const ImVec2 g0(
                origin.x + previewPanX + (gb.minX - 3) * previewZoomLevel,
                origin.y + previewPanY + (gb.minY - 3) * previewZoomLevel);
            const ImVec2 g1(
                origin.x + previewPanX + (gb.maxX + 3) * previewZoomLevel,
                origin.y + previewPanY + (gb.maxY + 3) * previewZoomLevel);
            if (g1.x < origin.x || g0.x > origin.x + size.x || g1.y < origin.y || gb.headerMin.y > origin.y + size.y)
                continue;

            const bool isHdrHovered = (hoveredGroupHeader == static_cast<int>(gi));
            const ImU32 boxFill = gb.anySelected ? IM_COL32(55, 85, 70, 50) : IM_COL32(35, 52, 44, 40);
            const ImU32 boxBorder = gb.anySelected
                                        ? IM_COL32(235, 195, 65, 200)
                                        : (isHdrHovered ? IM_COL32(110, 210, 175, 220) : IM_COL32(75, 130, 105, 160));
            const ImU32 hdrFill = gb.anySelected
                                      ? IM_COL32(80, 115, 60, 220)
                                      : (isHdrHovered ? IM_COL32(60, 115, 95, 230) : IM_COL32(42, 72, 60, 210));
            dl->AddRectFilled(g0, g1, boxFill, 3.0f);
            dl->AddRect(g0, g1, boxBorder, 3.0f, 0, 1.2f);
            dl->AddRectFilled(gb.headerMin, gb.headerMax, hdrFill, 3.0f);
            dl->AddRect(gb.headerMin, gb.headerMax, boxBorder, 3.0f, 0, 1.0f);

            const std::string title = gb.label + " (" + std::to_string(gb.count) + ")";
            dl->AddText(
                ImVec2(gb.headerMin.x + 5.0f, gb.headerMin.y + 1.0f),
                gb.anySelected ? IM_COL32(255, 240, 160, 255) : IM_COL32(210, 235, 225, 255),
                title.c_str());
        }
    }

    // Draw Sprites
    for (std::size_t i = 0; i < previewSprites.size(); ++i)
    {
        const auto &s = previewSprites[i];
        const ImVec2 sp0(
            origin.x + previewPanX + s.x * previewZoomLevel, origin.y + previewPanY + s.y * previewZoomLevel);
        const ImVec2 sp1(sp0.x + s.w * previewZoomLevel, sp0.y + s.h * previewZoomLevel);

        // Viewport culling
        if (sp1.x < origin.x || sp0.x > origin.x + size.x || sp1.y < origin.y || sp0.y > origin.y + size.y)
            continue;

        dl->AddRectFilled(sp0, sp1, IM_COL32(32, 38, 34, 230));

        SDL_Texture *tex = textures ? textures->GetSpriteSheet(s.page) : nullptr;
        if (tex && s.w > 0 && s.h > 0)
        {
            float tw = 1.0f, th = 1.0f;
            SDL_GetTextureSize(tex, &tw, &th);
            const ImVec2 uv0(s.src.x / tw, s.src.y / th);
            const ImVec2 uv1((s.src.x + s.src.w) / tw, (s.src.y + s.src.h) / th);
            dl->AddImage(reinterpret_cast<ImTextureID>(tex), sp0, sp1, uv0, uv1);
        }

        // Collision or selection border
        if (s.colliding)
        {
            // Overlapping error: bold red border and red translucent fill
            dl->AddRectFilled(sp0, sp1, IM_COL32(255, 30, 30, 85));
            dl->AddRect(sp0, sp1, s.selected ? IM_COL32(255, 190, 0, 255) : IM_COL32(255, 45, 45, 255), 0.0f, 0, 2.2f);
            dl->AddLine(sp0, sp1, IM_COL32(255, 50, 50, 180), 1.5f);
            dl->AddLine(ImVec2(sp1.x, sp0.y), ImVec2(sp0.x, sp1.y), IM_COL32(255, 50, 50, 180), 1.5f);
            dl->AddCircleFilled(ImVec2(sp0.x + 5.0f, sp0.y + 5.0f), 4.0f, IM_COL32(255, 40, 40, 255));
        }
        else if (s.selected)
        {
            // Selected: bright gold border
            dl->AddRect(sp0, sp1, IM_COL32(255, 215, 0, 255), 0.0f, 0, 2.0f);
            dl->AddRectFilled(ImVec2(sp0.x - 2, sp0.y - 2), ImVec2(sp0.x + 3, sp0.y + 3), IM_COL32(255, 215, 0, 255));
        }
        else if (hoveredIdx == static_cast<int>(i))
        {
            // Hovered: cyan border
            dl->AddRect(sp0, sp1, IM_COL32(100, 200, 255, 255), 0.0f, 0, 1.5f);
        }
        else
        {
            // Normal: subtle dark border
            dl->AddRect(sp0, sp1, IM_COL32(70, 85, 75, 180), 0.0f, 0, 1.0f);
        }
    }

    // Draw Marquee selection box
    if (previewMarquee)
    {
        const ImVec2 m0(
            std::min(previewMarqueeStart.x, previewMarqueeEnd.x), std::min(previewMarqueeStart.y, previewMarqueeEnd.y));
        const ImVec2 m1(
            std::max(previewMarqueeStart.x, previewMarqueeEnd.x), std::max(previewMarqueeStart.y, previewMarqueeEnd.y));
        dl->AddRectFilled(m0, m1, IM_COL32(100, 200, 255, 40));
        dl->AddRect(m0, m1, IM_COL32(100, 200, 255, 200), 0.0f, 0, 1.0f);
    }

    // Floating collision error banner at top of canvas when overlaps exist
    if (previewCollisionCount > 0)
    {
        const std::string banner = "ERROR: " + std::to_string(previewCollisionCount) + " overlap(s) ("
                                   + previewCollisionDetail + ") — Drag apart or click 'Separate Overlaps'";
        const ImVec2 textSz = ImGui::CalcTextSize(banner.c_str());
        const ImVec2 b0(origin.x + 8.0f, origin.y + 8.0f);
        const ImVec2 b1(std::min(origin.x + size.x - 8.0f, b0.x + textSz.x + 16.0f), b0.y + textSz.y + 10.0f);
        dl->AddRectFilled(b0, b1, IM_COL32(140, 20, 20, 230), 4.0f);
        dl->AddRect(b0, b1, IM_COL32(255, 80, 80, 255), 4.0f, 0, 1.5f);
        dl->AddText(ImVec2(b0.x + 8.0f, b0.y + 5.0f), IM_COL32(255, 235, 235, 255), banner.c_str());
    }

    // Tooltip for hovered sprite or hovered object group header
    if (hoveredIdx >= 0 && hoveredIdx < static_cast<int>(previewSprites.size()))
    {
        const auto &s = previewSprites[static_cast<std::size_t>(hoveredIdx)];
        ImGui::BeginTooltip();
        ImGui::Text("Sprite #%d (Sheet %02d)", s.id, s.page);
        if (!s.groupLabel.empty())
        {
            ImGui::TextColored(ImVec4(0.65f, 0.90f, 0.80f, 1.0f), "Object: %s", s.groupLabel.c_str());
        }
        ImGui::Text("Size: %dx%d px | Atlas Pos: (%d, %d)", s.w, s.h, s.x, s.y);
        ImGui::TextDisabled("%s", _("Double-click to select all sprites of this object/group"));
        if (s.colliding)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::Text("%s", _("⚠️ ERROR: OVERLAPPING with another sprite!"));
            ImGui::PopStyleColor();
        }
        ImGui::EndTooltip();
    }
    else if (hoveredGroupHeader >= 0 && hoveredGroupHeader < static_cast<int>(groupBounds.size()))
    {
        const auto &gb = groupBounds[static_cast<std::size_t>(hoveredGroupHeader)];
        ImGui::BeginTooltip();
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", gb.label.c_str());
        ImGui::Text(_("Sprites: %d | Bounds: (%d, %d) - (%d, %d)"), gb.count, gb.minX, gb.minY, gb.maxX, gb.maxY);
        ImGui::TextDisabled("%s", _("LMB + Drag header to move this entire object and all its sprites"));
        ImGui::EndTooltip();
    }

    ImGui::EndChild();
}

void CharacterEditor::DrawLoadedSprites()
{
    EnsureSpriteCatalog();
    if (!spriteCatalogOk)
        return;

    auto &allSprites = spriteCatalog.SpritesMut();
    const auto &objects = spriteCatalog.Objects();

    // Helper lambda to scan an export/import folder for external sprite mappings
    auto scanFolderMappings = [&](const fs::path &dir) {
        spriteExternalSource.clear();
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            return;
        std::vector<fs::path> jsonFiles;
        for (const auto &entry : fs::recursive_directory_iterator(dir, ec))
        {
            if (ec || !entry.is_regular_file())
                continue;
            const std::string fn = entry.path().filename().string();
            if (entry.path().extension() == ".json" && fn != "index.json" && fn != "sprites_index.json")
            {
                jsonFiles.push_back(entry.path());
            }
        }
        std::sort(jsonFiles.begin(), jsonFiles.end());
        for (const fs::path &jf : jsonFiles)
        {
            try
            {
                std::ifstream in(jf);
                if (!in)
                    continue;
                nlohmann::json doc = nlohmann::json::parse(in);
                if (!doc.contains("sprites") || !doc["sprites"].is_array())
                    continue;
                const std::string imgName = doc.contains("atlas") ? doc["atlas"].value("file", std::string())
                                                                  : doc.value("file", std::string());
                const fs::path relPath = fs::relative(jf.parent_path() / imgName, assetsRoot.parent_path(), ec);
                const std::string dispFile = (!ec && !relPath.empty()) ? relPath.generic_string()
                                                                       : (jf.parent_path() / imgName).generic_string();
                for (const auto &s : doc["sprites"])
                {
                    const int sid = s.value("id", -1);
                    if (sid < 0)
                        continue;
                    const auto &rect = s.contains("atlas_rect") ? s["atlas_rect"] : s["cell_rect"];
                    std::string info = dispFile;
                    if (rect.is_object())
                    {
                        info += " @ (" + std::to_string(rect.value("x", 0)) + "," + std::to_string(rect.value("y", 0))
                                + ")";
                    }
                    spriteExternalSource[sid] = info;
                }
            }
            catch (...)
            {}
        }
    };

    // 1. Loaded Asset Sources Summary Card
    int loadedSheetsCount = 0;
    for (int p = 0; p < 42; ++p)
    {
        if (textures && textures->GetSpriteSheet(p) != nullptr)
            ++loadedSheetsCount;
    }
    const fs::path spritesJsonPath = assetsRoot / "characters" / "sprites.json";
    const fs::path graphicsJsonPath = assetsRoot / "data" / "sprites_graphics.json";
    const fs::path sheetsDirPath = assetsRoot / "sprites";

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.14f, 0.15f, 0.95f));
    ImGui::BeginChild("loaded-sources-banner", ImVec2(0.0f, 68.0f), true);
    ImGui::TextColored(ImVec4(0.45f, 0.92f, 0.75f, 1.0f), "%s", _("Loaded Sprite Sources (Active Game Assets):"));
    ImGui::SameLine();
    if (ImGui::SmallButton(_("Scan Export/Import Folder")))
    {
        const fs::path scanDir
            = (spriteInDir[0] != '\0') ? fs::path(spriteInDir.data()) : fs::path(spriteOutDir.data());
        scanFolderMappings(scanDir);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "%s", _("Scans JSON files in the Import/Output folder to show external atlas file mappings per sprite."));
    }
    if (spritesJsonDirty)
    {
        ImGui::SameLine(0.0f, 25.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "%s", _("● sprites.json modified"));
        ImGui::SameLine();
        if (ImGui::Button(_("Save sprites.json##banner")))
        {
            SaveSpritesJson();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Revert##banner")))
        {
            RevertSpritesJson();
        }
    }
    ImGui::Text(_("• Rect Definitions: %s (%zu sprites)"), spritesJsonPath.generic_string().c_str(), allSprites.size());
    ImGui::SameLine(0.0f, 18.0f);
    ImGui::Text(_("• Dependency Tables: %s (%zu objects)"), graphicsJsonPath.generic_string().c_str(), objects.size());
    ImGui::Text(
        _("• Texture Sheets: %s/sheet_00.png .. sheet_41.png (%d/42 loaded in GPU)"),
        sheetsDirPath.generic_string().c_str(),
        loadedSheetsCount);
    if (!spriteExternalSource.empty())
    {
        ImGui::SameLine(0.0f, 18.0f);
        ImGui::TextColored(
            ImVec4(0.95f, 0.85f, 0.35f, 1.0f),
            _("• External Atlas Mappings: %zu sprites"),
            spriteExternalSource.size());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // 2. Filter Bar (Scope, Sheet Selector, Search, Visual Map Toggle)
    ImGui::TextDisabled("%s", _("Scope:"));
    ImGui::SameLine();
    if (ImGui::RadioButton(_("All Sprites"), loadedSpritesScope == 0))
        loadedSpritesScope = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton(_("Selected Object(s)"), loadedSpritesScope == 1))
        loadedSpritesScope = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton(_("By Sheet"), loadedSpritesScope == 2))
    {
        loadedSpritesScope = 2;
        if (loadedSpritesSheetFilter < 0)
            loadedSpritesSheetFilter = 0;
    }

    if (loadedSpritesScope == 2)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(210.0f);
        char sheetPreviewBuf[64];
        if (loadedSpritesSheetFilter < 0)
        {
            std::snprintf(sheetPreviewBuf, sizeof(sheetPreviewBuf), "%s", _("All Sheets (0..41)"));
        }
        else
        {
            SDL_Texture *st = textures ? textures->GetSpriteSheet(loadedSpritesSheetFilter) : nullptr;
            float tw = 0.0f, th = 0.0f;
            if (st)
                SDL_GetTextureSize(st, &tw, &th);
            std::snprintf(
                sheetPreviewBuf,
                sizeof(sheetPreviewBuf),
                "sheet_%02d.png (%.0fx%.0f)",
                loadedSpritesSheetFilter,
                tw,
                th);
        }
        if (ImGui::BeginCombo("##sheet_filter", sheetPreviewBuf))
        {
            if (ImGui::Selectable(_("All Sheets (0..41)"), loadedSpritesSheetFilter == -1))
            {
                loadedSpritesSheetFilter = -1;
            }
            for (int p = 0; p < 42; ++p)
            {
                SDL_Texture *st = textures ? textures->GetSpriteSheet(p) : nullptr;
                float tw = 0.0f, th = 0.0f;
                if (st)
                    SDL_GetTextureSize(st, &tw, &th);
                int countOnPage = 0;
                for (const auto &rs : allSprites)
                    if (rs.page == p)
                        ++countOnPage;
                char itemBuf[96];
                std::snprintf(
                    itemBuf,
                    sizeof(itemBuf),
                    "sheet_%02d.png — %d sprites (%.0fx%.0f px, %.0fx)",
                    p,
                    countOnPage,
                    tw,
                    th,
                    textures ? textures->GetSheetScale(p) : 1.0f);
                if (ImGui::Selectable(itemBuf, loadedSpritesSheetFilter == p))
                {
                    loadedSpritesSheetFilter = p;
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint(
        "##loaded_search", _("Filter #id, sheet, object..."), loadedSpritesSearch.data(), loadedSpritesSearch.size());
    ImGui::SameLine();
    ImGui::Checkbox(_("Show Sheet Map"), &loadedSpritesShowSheetMap);

    // Build filtered sprite list
    std::set<int> allowedByObject;
    if (loadedSpritesScope == 1)
    {
        std::vector<int> objIndices;
        if (!spriteChecked.empty())
        {
            objIndices.assign(spriteChecked.begin(), spriteChecked.end());
        }
        else if (spriteSelected >= 0 && spriteSelected < static_cast<int>(objects.size()))
        {
            objIndices.push_back(spriteSelected);
        }
        for (int oi : objIndices)
        {
            if (oi >= 0 && oi < static_cast<int>(objects.size()))
            {
                const auto &o = objects[static_cast<std::size_t>(oi)];
                allowedByObject.insert(o.sprites.begin(), o.sprites.end());
            }
        }
    }

    std::string needle(loadedSpritesSearch.data());
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    std::vector<int> filteredIds;
    filteredIds.reserve(allSprites.size());
    for (const RawSprite &s : allSprites)
    {
        if (loadedSpritesScope == 1 && !allowedByObject.empty() && allowedByObject.count(s.id) == 0)
            continue;
        if (loadedSpritesScope == 1 && allowedByObject.empty() && spriteSelected < 0 && spriteChecked.empty())
        {
            // No object selected: show all so table isn't blank
        }
        if (loadedSpritesScope == 2 && loadedSpritesSheetFilter >= 0 && s.page != loadedSpritesSheetFilter)
            continue;
        if (spriteGroupFilter >= 0 && static_cast<int>(spriteCatalog.GroupOfSprite(s.id)) != spriteGroupFilter)
            continue;

        if (!needle.empty())
        {
            char sheetName[32];
            std::snprintf(sheetName, sizeof(sheetName), "sheet_%02d.png", s.page);
            std::string hay = "#" + std::to_string(s.id) + " " + sheetName + " "
                              + SpriteGroupName(spriteCatalog.GroupOfSprite(s.id));
            if (s.id >= 0 && s.id < static_cast<int>(spriteToObjects.size()))
            {
                for (int oi : spriteToObjects[static_cast<std::size_t>(s.id)])
                {
                    if (oi >= 0 && oi < static_cast<int>(objects.size()))
                    {
                        hay += " " + objects[static_cast<std::size_t>(oi)].name;
                    }
                }
            }
            auto extIt = spriteExternalSource.find(s.id);
            if (extIt != spriteExternalSource.end())
                hay += " " + extIt->second;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (hay.find(needle) == std::string::npos)
                continue;
        }
        filteredIds.push_back(s.id);
    }

    if (!filteredIds.empty()
        && std::find(filteredIds.begin(), filteredIds.end(), loadedSpriteSelectedId) == filteredIds.end())
    {
        loadedSpriteSelectedId = filteredIds.front();
    }

    // 3. Visual Sheet Cutout Map for the currently selected sprite
    if (loadedSpritesShowSheetMap && loadedSpriteSelectedId >= 0
        && loadedSpriteSelectedId < static_cast<int>(allSprites.size()))
    {
        RawSprite &sel = allSprites[static_cast<std::size_t>(loadedSpriteSelectedId)];
        SDL_Texture *sheetTex = textures ? textures->GetSpriteSheet(sel.page) : nullptr;
        float tw = 1.0f, th = 1.0f;
        if (sheetTex)
            SDL_GetTextureSize(sheetTex, &tw, &th);
        const float sheetScale = textures ? textures->GetSheetScale(sel.page) : 1.0f;
        char relSheet[64];
        std::snprintf(relSheet, sizeof(relSheet), "assets/sprites/sheet_%02d.png", sel.page);
        const fs::path fullSheetPath
            = assetsRoot / "sprites"
              / (std::string("sheet_") + (sel.page < 10 ? "0" : "") + std::to_string(sel.page) + ".png");

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.10f, 0.10f, 1.0f));
        ImGui::BeginChild("loaded-sheet-map", ImVec2(0.0f, 195.0f), true);

        // Left: zoomed sprite preview + exact provenance details
        ImGui::BeginGroup();
        DrawSpriteImage(sel.id, 64.0f);
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(
            ImVec4(1.0f, 0.88f, 0.30f, 1.0f), _("Sprite #%d  —  Loaded from %s (Page %d)"), sel.id, relSheet, sel.page);
        ImGui::TextDisabled(_("Full File Path: %s"), fullSheetPath.generic_string().c_str());
        ImGui::Text(
            _("sprites.json Rect: x=%d, y=%d, w=%d, h=%d (palette=%d)  |  PNG Pixel Rect (%.0fx): [%d, %d, %d, %d] on "
              "%.0fx%.0f px texture"),
            sel.x,
            sel.y,
            sel.w,
            sel.h,
            sel.palette,
            sheetScale,
            static_cast<int>(std::round(sel.x * sheetScale)),
            static_cast<int>(std::round(sel.y * sheetScale)),
            static_cast<int>(std::round(sel.w * sheetScale)),
            static_cast<int>(std::round(sel.h * sheetScale)),
            tw,
            th);
        if (sel.id >= 0 && sel.id < static_cast<int>(spriteToObjects.size())
            && !spriteToObjects[static_cast<std::size_t>(sel.id)].empty())
        {
            std::string usedStr;
            const auto &uList = spriteToObjects[static_cast<std::size_t>(sel.id)];
            for (std::size_t k = 0; k < uList.size() && k < 6; ++k)
            {
                if (k > 0)
                    usedStr += ", ";
                const auto &o = objects[static_cast<std::size_t>(uList[k])];
                usedStr += "[#" + std::to_string(o.id) + "] " + o.name;
            }
            if (uList.size() > 6)
                usedStr += " (+" + std::to_string(uList.size() - 6) + " more)";
            ImGui::TextColored(ImVec4(0.55f, 0.90f, 0.78f, 1.0f), _("Used by: %s"), usedStr.c_str());
        }
        else
        {
            ImGui::TextDisabled("%s", _("Used by: Unreferenced / Misc raw sprite"));
        }
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 15.0f);
        ImGui::BeginGroup();
        ImGui::Text("%s", _("Position:"));
        ImGui::SameLine();
        if (ImGui::Button("-1##ls_x")) { sel.x = std::max(0, sel.x - 1); OnSpriteModified(sel); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        if (ImGui::InputInt("##ls_xv", &sel.x, 0)) { sel.x = std::max(0, sel.x); OnSpriteModified(sel); }
        ImGui::SameLine();
        if (ImGui::Button("+1##ls_x")) { sel.x += 1; OnSpriteModified(sel); }
        ImGui::SameLine(0.0f, 10.0f);
        if (ImGui::Button("-1##ls_y")) { sel.y = std::max(0, sel.y - 1); OnSpriteModified(sel); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        if (ImGui::InputInt("##ls_yv", &sel.y, 0)) { sel.y = std::max(0, sel.y); OnSpriteModified(sel); }
        ImGui::SameLine();
        if (ImGui::Button("+1##ls_y")) { sel.y += 1; OnSpriteModified(sel); }
        if (spritesJsonDirty)
        {
            ImGui::SameLine(0.0f, 15.0f);
            if (ImGui::Button(_("Save sprites.json##ls")))
            {
                SaveSpritesJson();
            }
        }
        ImGui::EndGroup();

        // Bottom strip inside child: miniature interactive map of sheet_XX.png showing all cutouts & highlighting sel
        if (sheetTex && tw > 0.0f && th > 0.0f)
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float mapH = std::max(48.0f, avail.y - 2.0f);
            const float fitScale = std::min(avail.x / tw, mapH / th);
            const ImVec2 drawSz(tw * fitScale, th * fitScale);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const ImVec2 p1(p0.x + drawSz.x, p0.y + drawSz.y);

            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1, IM_COL32(20, 26, 26, 255));
            dl->AddImage(reinterpret_cast<ImTextureID>(sheetTex), p0, p1);
            dl->AddRect(p0, p1, IM_COL32(90, 120, 110, 200));

            // Draw all sprites on this page
            for (const RawSprite &rs : allSprites)
            {
                if (rs.page != sel.page)
                    continue;
                const ImVec2 r0(p0.x + rs.x * sheetScale * fitScale, p0.y + rs.y * sheetScale * fitScale);
                const ImVec2 r1(
                    p0.x + (rs.x + rs.w) * sheetScale * fitScale, p0.y + (rs.y + rs.h) * sheetScale * fitScale);
                if (rs.id == sel.id)
                    continue;
                dl->AddRect(r0, r1, IM_COL32(60, 180, 140, 110));
            }
            // Highlight selected sprite on the sheet
            const ImVec2 sr0(p0.x + sel.x * sheetScale * fitScale, p0.y + sel.y * sheetScale * fitScale);
            const ImVec2 sr1(
                p0.x + (sel.x + sel.w) * sheetScale * fitScale, p0.y + (sel.y + sel.h) * sheetScale * fitScale);
            dl->AddRectFilled(sr0, sr1, IM_COL32(255, 230, 50, 55));
            dl->AddRect(sr0, sr1, IM_COL32(255, 230, 50, 255), 0.0f, 0, 2.0f);

            ImGui::InvisibleButton("##sheet_map_click", drawSz);
            if (ImGui::IsItemHovered())
            {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                const float sheetX = (mp.x - p0.x) / (sheetScale * fitScale);
                const float sheetY = (mp.y - p0.y) / (sheetScale * fitScale);
                int hoveredSprite = -1;
                for (const RawSprite &rs : allSprites)
                {
                    if (rs.page != sel.page)
                        continue;
                    if (sheetX >= rs.x && sheetX < rs.x + rs.w && sheetY >= rs.y && sheetY < rs.y + rs.h)
                    {
                        hoveredSprite = rs.id;
                        break;
                    }
                }
                if (hoveredSprite >= 0)
                {
                    ImGui::SetTooltip(
                        "Sprite #%d (%dx%d at %d,%d) — Click to inspect",
                        hoveredSprite,
                        allSprites[static_cast<std::size_t>(hoveredSprite)].w,
                        allSprites[static_cast<std::size_t>(hoveredSprite)].h,
                        allSprites[static_cast<std::size_t>(hoveredSprite)].x,
                        allSprites[static_cast<std::size_t>(hoveredSprite)].y);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        loadedSpriteSelectedId = hoveredSprite;
                        loadedSpritesScrollToSelected = true;
                    }
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    ImGui::TextDisabled(
        _("Showing %zu / %zu loaded sprites (Click a row to highlight on source sheet, click an object badge to jump "
          "to it)"),
        filteredIds.size(),
        allSprites.size());

    // 4. Table of Loaded Sprites and their Provenance
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable
                                       | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("loaded-sprites-table", 6, tableFlags, ImVec2(0.0f, 0.0f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(_("Preview"), ImGuiTableColumnFlags_WidthFixed, 38.0f);
        ImGui::TableSetupColumn(_("Sprite ID"), ImGuiTableColumnFlags_WidthFixed, 68.0f);
        ImGui::TableSetupColumn(_("Loaded From (Sheet File)"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn(_("Source Rect (sprites.json / PNG)"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(_("Group"), ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableSetupColumn(_("Used By Objects (sprites_graphics.json)"), ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(filteredIds.size()), 34.0f);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const int sid = filteredIds[static_cast<std::size_t>(row)];
                const RawSprite &s = allSprites[static_cast<std::size_t>(sid)];
                const bool isSelected = (sid == loadedSpriteSelectedId);

                ImGui::TableNextRow(ImGuiTableRowFlags_None, 34.0f);
                ImGui::PushID(sid);

                // Col 0: Thumbnail
                ImGui::TableSetColumnIndex(0);
                DrawSpriteImage(sid, 28.0f);

                // Col 1: Sprite ID (Selectable spanning row or cell)
                ImGui::TableSetColumnIndex(1);
                char idLabel[32];
                std::snprintf(idLabel, sizeof(idLabel), "#%d", sid);
                if (ImGui::Selectable(idLabel, isSelected, ImGuiSelectableFlags_None, ImVec2(0.0f, 28.0f)))
                {
                    loadedSpriteSelectedId = sid;
                }
                if (loadedSpritesScrollToSelected && isSelected)
                {
                    ImGui::SetScrollHereY(0.5f);
                    loadedSpritesScrollToSelected = false;
                }

                // Col 2: Loaded From (Sheet File + Status + External Override if any)
                ImGui::TableSetColumnIndex(2);
                SDL_Texture *sheetTex = textures ? textures->GetSpriteSheet(s.page) : nullptr;
                float tw = 0.0f, th = 0.0f;
                if (sheetTex)
                    SDL_GetTextureSize(sheetTex, &tw, &th);
                const float scale = textures ? textures->GetSheetScale(s.page) : 1.0f;
                char sheetFile[64];
                std::snprintf(sheetFile, sizeof(sheetFile), "assets/sprites/sheet_%02d.png", s.page);
                if (sheetTex)
                {
                    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.70f, 1.0f), "%s", sheetFile);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%.0fx%.0f, %.0fx)", tw, th, scale);
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.40f, 0.40f, 1.0f), "%s [MISSING]", sheetFile);
                }
                auto extIt = spriteExternalSource.find(sid);
                if (extIt != spriteExternalSource.end())
                {
                    ImGui::TextColored(ImVec4(0.95f, 0.82f, 0.35f, 1.0f), "↳ %s", extIt->second.c_str());
                }

                // Col 3: Source Rect
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("(%d, %d) %dx%d px", s.x, s.y, s.w, s.h);
                if (std::abs(scale - 1.0f) > 0.01f)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled(
                        "-> [%d,%d %dx%d]",
                        static_cast<int>(std::round(s.x * scale)),
                        static_cast<int>(std::round(s.y * scale)),
                        static_cast<int>(std::round(s.w * scale)),
                        static_cast<int>(std::round(s.h * scale)));
                }

                // Col 4: Group
                ImGui::TableSetColumnIndex(4);
                const SpriteGroup grp = spriteCatalog.GroupOfSprite(sid);
                ImGui::TextUnformatted(_(SpriteGroupName(grp)));

                // Col 5: Used By Objects
                ImGui::TableSetColumnIndex(5);
                if (sid >= 0 && sid < static_cast<int>(spriteToObjects.size())
                    && !spriteToObjects[static_cast<std::size_t>(sid)].empty())
                {
                    const auto &uList = spriteToObjects[static_cast<std::size_t>(sid)];
                    const std::size_t maxBadges = std::min<std::size_t>(uList.size(), 3);
                    for (std::size_t k = 0; k < maxBadges; ++k)
                    {
                        if (k > 0)
                            ImGui::SameLine();
                        const int oi = uList[k];
                        const auto &o = objects[static_cast<std::size_t>(oi)];
                        const std::string btnLbl = (o.kind == SpriteObjectKind::Unit       ? "[#"
                                                    : o.kind == SpriteObjectKind::Building ? "[B#"
                                                                                           : "[")
                                                   + std::to_string(o.id) + "] " + o.name + "##jump"
                                                   + std::to_string(k);
                        if (ImGui::SmallButton(btnLbl.c_str()))
                        {
                            spriteSelected = oi;
                            spriteAnchor = oi;
                            spriteChecked.clear();
                            spriteChecked.insert(oi);
                            loadedSpriteSelectedId = sid;
                        }
                    }
                    if (uList.size() > maxBadges)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("+%zu", uList.size() - maxBadges);
                    }
                }
                else
                {
                    ImGui::TextDisabled("%s", _("Unused / Raw"));
                }

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

int CharacterEditor::FindSpriteObjectForActiveCharacter() const
{
    if (!document.IsDefined())
        return -1;
    const auto &objs = spriteCatalog.Objects();
    const std::string type = YamlString(document, "type", "");
    const bool isBuilding = (type == "building");
    const int numId = isBuilding ? YamlInt(document, "building_index", -1) : YamlInt(document, "archetype_id", -1);
    const std::string name = YamlString(document, "name", "");

    if (numId >= 0)
    {
        for (std::size_t i = 0; i < objs.size(); ++i)
        {
            if (isBuilding && objs[i].kind == SpriteObjectKind::Building && objs[i].id == numId)
                return static_cast<int>(i);
            if (!isBuilding && objs[i].kind == SpriteObjectKind::Unit && objs[i].id == numId)
                return static_cast<int>(i);
        }
    }
    if (!name.empty())
    {
        for (std::size_t i = 0; i < objs.size(); ++i)
        {
            if (objs[i].name == name)
                return static_cast<int>(i);
        }
    }
    return -1;
}

void CharacterEditor::SyncSpriteObjectFromActive()
{
    EnsureSpriteCatalog();
    if (!spriteCatalogOk)
        return;
    int objIdx = FindSpriteObjectForActiveCharacter();
    if (objIdx >= 0)
    {
        spriteSelected = objIdx;
        spriteChecked.clear();
        spriteChecked.insert(objIdx);
        spriteAnchor = objIdx;
        OnSpriteObjectSelected(objIdx);
    }
}

void CharacterEditor::OnSpriteObjectSelected(int objIdx)
{
    if (objIdx < 0 || objIdx >= static_cast<int>(spriteCatalog.Objects().size()))
        return;
    const auto &o = spriteCatalog.Objects()[static_cast<std::size_t>(objIdx)];
    if (!o.sprites.empty())
    {
        if (pointEditorSpriteId < 0 || o.sprites.count(pointEditorSpriteId) == 0)
        {
            pointEditorSpriteId = *o.sprites.begin();
        }
        loadedSpriteSelectedId = pointEditorSpriteId;
        loadedSpritesScope = 1; // Filter loaded sprites to selected object
        pointEditorFramedSpriteId = -1;
    }
}

void CharacterEditor::OnSpriteModified(const RawSprite &s)
{
    if (textures)
    {
        textures->UpdateSpriteFrame(s.id, s.page, s.x, s.y, s.w, s.h, s.palette);
    }
    spritesJsonDirty = true;
}

bool CharacterEditor::SaveSpritesJson()
{
    EnsureSpriteCatalog();
    std::string err;
    if (spriteCatalog.SaveSpritesJson(assetsRoot, err))
    {
        spritesJsonDirty = false;
        status = _("Saved to assets/characters/sprites.json");
        return true;
    }
    else
    {
        status = std::string(_("Failed to save sprites.json: ")) + err;
        return false;
    }
}

bool CharacterEditor::RevertSpritesJson()
{
    EnsureSpriteCatalog();
    std::string err;
    if (spriteCatalog.ReloadSpritesJson(assetsRoot, err))
    {
        SyncTextureFramesFromCatalog();
        spritesJsonDirty = false;
        status = _("Reverted sprites.json from disk.");
        return true;
    }
    else
    {
        status = std::string(_("Failed to reload sprites.json: ")) + err;
        return false;
    }
}

void CharacterEditor::SyncTextureFramesFromCatalog()
{
    if (!textures)
        return;
    const auto &sprites = spriteCatalog.Sprites();
    for (const auto &s : sprites)
    {
        textures->UpdateSpriteFrame(s.id, s.page, s.x, s.y, s.w, s.h, s.palette);
    }
}

void CharacterEditor::DrawPointEditor()
{
    EnsureSpriteCatalog();
    if (!spriteCatalogOk)
    {
        ImGui::TextWrapped(
            "%s", spriteReport.empty() ? _("The sprite data could not be loaded.") : spriteReport.c_str());
        return;
    }
    const auto &objects = spriteCatalog.Objects();
    if (spriteSelected < 0 || spriteSelected >= static_cast<int>(objects.size()))
    {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", _("Point Editor"));
        ImGui::TextWrapped(
            "%s",
            _("Select an object from the left panel (character / building) or from the Sprite Tools list to inspect and edit its sprites."));
        return;
    }

    const SpriteObject &obj = objects[static_cast<std::size_t>(spriteSelected)];
    auto &allSprites = spriteCatalog.SpritesMut();

    // 1. Top Control Bar
    const std::string objTag = (obj.kind == SpriteObjectKind::Unit ? "[#" : obj.kind == SpriteObjectKind::Building ? "[B#" : "[")
                               + std::to_string(obj.id) + "] " + obj.name;
    ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.30f, 1.0f), "%s", objTag.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s / %s, %zu sprites)", obj.faction.c_str(), obj.kindName.c_str(), obj.sprites.size());

    ImGui::SameLine(0.0f, 20.0f);
    if (spritesJsonDirty)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.20f, 1.0f), "%s", _("● Unsaved changes"));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.60f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.75f, 0.44f, 1.0f));
        if (Hinted(ImGui::Button(_("Save (sprites.json)"), ImVec2(150.0f, 24.0f)), "Save sprites.json"))
        {
            SaveSpritesJson();
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (Hinted(ImGui::Button(_("Revert"), ImVec2(80.0f, 24.0f)), "Revert sprites.json"))
        {
            RevertSpritesJson();
        }
    }
    else
    {
        ImGui::TextDisabled("%s", _("✔ sprites.json up to date"));
        ImGui::SameLine();
        ImGui::BeginDisabled(true);
        ImGui::Button(_("Save (sprites.json)"), ImVec2(150.0f, 24.0f));
        ImGui::EndDisabled();
    }

    ImGui::SameLine(0.0f, 15.0f);
    if (ImGui::Button(_("Shift All Sprites...")))
    {
        ImGui::OpenPopup("ShiftAllSpritesPopup");
    }
    if (ImGui::BeginPopup("ShiftAllSpritesPopup"))
    {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s", _("Shift All Sprites of Object"));
        ImGui::TextDisabled(_("Offsets (x, y) for all %zu sprites of %s:"), obj.sprites.size(), obj.name.c_str());
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt(_("ΔX (px)"), &pointEditorShiftAllDx);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt(_("ΔY (px)"), &pointEditorShiftAllDy);
        if (ImGui::Button(_("Apply Shift")) && (pointEditorShiftAllDx != 0 || pointEditorShiftAllDy != 0))
        {
            for (int sid : obj.sprites)
            {
                if (sid >= 0 && sid < static_cast<int>(allSprites.size()))
                {
                    RawSprite &curS = allSprites[static_cast<std::size_t>(sid)];
                    curS.x = std::max(0, curS.x + pointEditorShiftAllDx);
                    curS.y = std::max(0, curS.y + pointEditorShiftAllDy);
                    OnSpriteModified(curS);
                }
            }
            pointEditorShiftAllDx = 0;
            pointEditorShiftAllDy = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Cancel")))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();

    if (obj.sprites.empty())
    {
        ImGui::TextDisabled("%s", _("This object does not reference any sprites directly."));
        return;
    }

    std::vector<int> sortedObjSprites(obj.sprites.begin(), obj.sprites.end());
    if (pointEditorSpriteId < 0 || obj.sprites.count(pointEditorSpriteId) == 0)
    {
        pointEditorSpriteId = sortedObjSprites.front();
        loadedSpriteSelectedId = pointEditorSpriteId;
    }

    // 2. Left Subpanel: List of Sprites belonging to this Object
    const float leftColW = 270.0f;
    ImGui::BeginChild("point-editor-sprite-list", ImVec2(leftColW, 0.0f), true);

    auto itCur = std::find(sortedObjSprites.begin(), sortedObjSprites.end(), pointEditorSpriteId);
    int curPos = (itCur != sortedObjSprites.end()) ? static_cast<int>(itCur - sortedObjSprites.begin()) : 0;
    ImGui::BeginDisabled(curPos <= 0);
    if (ImGui::SmallButton("◄"))
    {
        if (curPos > 0)
        {
            pointEditorSpriteId = sortedObjSprites[static_cast<std::size_t>(curPos - 1)];
            loadedSpriteSelectedId = pointEditorSpriteId;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("%d / %zu", curPos + 1, sortedObjSprites.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(curPos + 1 >= static_cast<int>(sortedObjSprites.size()));
    if (ImGui::SmallButton("►"))
    {
        if (curPos + 1 < static_cast<int>(sortedObjSprites.size()))
        {
            pointEditorSpriteId = sortedObjSprites[static_cast<std::size_t>(curPos + 1)];
            loadedSpriteSelectedId = pointEditorSpriteId;
        }
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    for (int sid : sortedObjSprites)
    {
        if (sid < 0 || sid >= static_cast<int>(allSprites.size()))
            continue;
        const RawSprite &s = allSprites[static_cast<std::size_t>(sid)];
        const bool isSelected = (sid == pointEditorSpriteId);
        ImGui::PushID(sid);

        constexpr float rowHeight = 36.0f;
        if (ImGui::Selectable("##srow", isSelected, 0, ImVec2(0.0f, rowHeight)))
        {
            pointEditorSpriteId = sid;
            loadedSpriteSelectedId = sid;
        }
        const ImVec2 rMin = ImGui::GetItemRectMin();
        DrawSpriteImageAt(ImVec2(rMin.x + 2.0f, rMin.y + 2.0f), sid, 32.0f);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        char line1[48];
        std::snprintf(line1, sizeof(line1), "Sprite #%d", sid);
        dl->AddText(
            ImVec2(rMin.x + 38.0f, rMin.y + 2.0f),
            isSelected ? IM_COL32(255, 230, 80, 255) : IM_COL32(230, 235, 230, 255),
            line1);
        char line2[48];
        std::snprintf(line2, sizeof(line2), "P%d: (%d, %d) %dx%d", s.page, s.x, s.y, s.w, s.h);
        dl->AddText(ImVec2(rMin.x + 38.0f, rMin.y + 18.0f), IM_COL32(145, 175, 165, 255), line2);

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // 3. Right Subpanel: Coordinate Controls & Interactive Sheet Map for Selected Sprite
    ImGui::BeginChild("point-editor-detail", ImVec2(0.0f, 0.0f), true);

    if (pointEditorSpriteId >= 0 && pointEditorSpriteId < static_cast<int>(allSprites.size()))
    {
        RawSprite &sel = allSprites[static_cast<std::size_t>(pointEditorSpriteId)];
        const float sheetScale = textures ? textures->GetSheetScale(sel.page) : 1.0f;
        SDL_Texture *sheetTex = textures ? textures->GetSpriteSheet(sel.page) : nullptr;
        float tw = 1.0f, th = 1.0f;
        if (sheetTex)
            SDL_GetTextureSize(sheetTex, &tw, &th);

        ImGui::BeginGroup();
        const ImVec2 thumbPos = ImGui::GetCursorScreenPos();
        constexpr float thumbBox = 76.0f;
        DrawSpriteImage(sel.id, thumbBox);
        ImGui::SetCursorScreenPos(thumbPos);
        ImGui::InvisibleButton("##sprite_thumb_drag", ImVec2(thumbBox, thumbBox));
        const bool thumbHovered = ImGui::IsItemHovered();
        const bool thumbActive = ImGui::IsItemActive();
        if (thumbHovered && ImGui::GetIO().MouseWheel != 0.0f)
        {
            const float factor = (ImGui::GetIO().MouseWheel > 0.0f) ? 1.25f : 0.8f;
            pointEditorZoom = std::clamp(pointEditorZoom * factor, 0.15f, 24.0f);
        }
        if (thumbActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            if (std::abs(delta.x) >= 1.0f || std::abs(delta.y) >= 1.0f)
            {
                sel.x = std::max(0, sel.x + static_cast<int>(std::round(delta.x)));
                sel.y = std::max(0, sel.y + static_cast<int>(std::round(delta.y)));
                OnSpriteModified(sel);
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        if (thumbHovered)
        {
            ImGui::SetTooltip(_("Sprite #%d\nLMB + Drag: adjust (x, y)\nMouse Wheel: zoom sheet (%.0f%%)"), sel.id, pointEditorZoom * 100.0f);
        }
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 15.0f);
        ImGui::BeginGroup();
        char headerText[96];
        std::snprintf(
            headerText, sizeof(headerText), "Sprite #%d  —  sheet_%02d.png (Page %d)", sel.id, sel.page, sel.page);
        ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.30f, 1.0f), "%s", headerText);

        // Position X Controls
        ImGui::Text("%s", _("Position X:"));
        ImGui::SameLine(95.0f);
        if (ImGui::Button("-10##px")) { sel.x = std::max(0, sel.x - 10); OnSpriteModified(sel); }
        ImGui::SameLine();
        if (ImGui::Button("-1##px")) { sel.x = std::max(0, sel.x - 1); OnSpriteModified(sel); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(75.0f);
        if (ImGui::InputInt("##val_px", &sel.x, 0)) { sel.x = std::max(0, sel.x); OnSpriteModified(sel); }
        ImGui::SameLine();
        if (ImGui::Button("+1##px")) { sel.x += 1; OnSpriteModified(sel); }
        ImGui::SameLine();
        if (ImGui::Button("+10##px")) { sel.x += 10; OnSpriteModified(sel); }

        // Position Y Controls
        ImGui::Text("%s", _("Position Y:"));
        ImGui::SameLine(95.0f);
        if (ImGui::Button("-10##py")) { sel.y = std::max(0, sel.y - 10); OnSpriteModified(sel); }
        ImGui::SameLine();
        if (ImGui::Button("-1##py")) { sel.y = std::max(0, sel.y - 1); OnSpriteModified(sel); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(75.0f);
        if (ImGui::InputInt("##val_py", &sel.y, 0)) { sel.y = std::max(0, sel.y); OnSpriteModified(sel); }
        ImGui::SameLine();
        if (ImGui::Button("+1##py")) { sel.y += 1; OnSpriteModified(sel); }
        ImGui::SameLine();
        if (ImGui::Button("+10##py")) { sel.y += 10; OnSpriteModified(sel); }

        // Size W x H Controls
        ImGui::Text("%s", _("Size (WxH):"));
        ImGui::SameLine(95.0f);
        if (ImGui::Button("-1##pw")) { sel.w = std::max(1, sel.w - 1); OnSpriteModified(sel); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(55.0f);
        if (ImGui::InputInt("##val_pw", &sel.w, 0)) { sel.w = std::max(1, sel.w); OnSpriteModified(sel); }
        ImGui::SameLine();
        if (ImGui::Button("+1##pw")) { sel.w += 1; OnSpriteModified(sel); }
        ImGui::SameLine();
        ImGui::Text("x");
        ImGui::SameLine();
        if (ImGui::Button("-1##ph")) { sel.h = std::max(1, sel.h - 1); OnSpriteModified(sel); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(55.0f);
        if (ImGui::InputInt("##val_ph", &sel.h, 0)) { sel.h = std::max(1, sel.h); OnSpriteModified(sel); }
        ImGui::SameLine();
        if (ImGui::Button("+1##ph")) { sel.h += 1; OnSpriteModified(sel); }
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 25.0f);
        // Nudge D-Pad
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", _("Nudge:"));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 26.0f);
        if (ImGui::Button("▲##nudge_up", ImVec2(24.0f, 22.0f)))
        {
            sel.y = std::max(0, sel.y - pointEditorNudgeStep);
            OnSpriteModified(sel);
        }
        if (ImGui::Button("◄##nudge_l", ImVec2(24.0f, 22.0f)))
        {
            sel.x = std::max(0, sel.x - pointEditorNudgeStep);
            OnSpriteModified(sel);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d", pointEditorNudgeStep);
        ImGui::SameLine();
        if (ImGui::Button("►##nudge_r", ImVec2(24.0f, 22.0f)))
        {
            sel.x += pointEditorNudgeStep;
            OnSpriteModified(sel);
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 26.0f);
        if (ImGui::Button("▼##nudge_dn", ImVec2(24.0f, 22.0f)))
        {
            sel.y += pointEditorNudgeStep;
            OnSpriteModified(sel);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 2.0f));
        if (ImGui::RadioButton("1px", pointEditorNudgeStep == 1)) pointEditorNudgeStep = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("5px", pointEditorNudgeStep == 5)) pointEditorNudgeStep = 5;
        ImGui::SameLine();
        if (ImGui::RadioButton("10px", pointEditorNudgeStep == 10)) pointEditorNudgeStep = 10;
        ImGui::PopStyleVar();
        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();

        // Canvas Toolbar
        if (Hinted(ImGui::SmallButton(_("Focus Sprite")), "Focus Sprite"))
        {
            pointEditorFramedSpriteId = -1; // triggers auto-focus
        }
        ImGui::SameLine();
        if (Hinted(ImGui::SmallButton(_("Fit Sheet")), "Fit Sheet"))
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float fit = std::min(avail.x / tw, (avail.y - 10.0f) / th);
            pointEditorZoom = std::max(0.1f, fit);
            pointEditorPan = ImVec2(0.0f, 0.0f);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("100%")) { pointEditorZoom = 1.0f; }
        ImGui::SameLine();
        if (ImGui::SmallButton("200%")) { pointEditorZoom = 2.0f; }
        ImGui::SameLine();
        if (ImGui::SmallButton("400%")) { pointEditorZoom = 4.0f; }
        ImGui::SameLine();
        if (ImGui::SmallButton("-##zm")) { pointEditorZoom = std::max(0.2f, pointEditorZoom / 1.25f); }
        ImGui::SameLine();
        ImGui::Text("%.0f%%", pointEditorZoom * 100.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("+##zm")) { pointEditorZoom = std::min(24.0f, pointEditorZoom * 1.25f); }
        ImGui::SameLine();
        ImGui::Checkbox(_("Grid"), &pointEditorShowGrid);
        ImGui::SameLine(0.0f, 15.0f);
        ImGui::TextDisabled("%s", _("LMB: Move / Resize | Wheel: Zoom | MMB / Space+Drag: Pan"));

        // Interactive Sheet Canvas
        if (sheetTex && tw > 0.0f && th > 0.0f)
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float canvasW = std::max(100.0f, avail.x);
            const float canvasH = std::max(150.0f, avail.y - 6.0f);
            const ImVec2 canvasSize(canvasW, canvasH);

            if (pointEditorFramedSpriteId != sel.id)
            {
                pointEditorFramedSpriteId = sel.id;
                const float targetW = static_cast<float>(std::max(16, sel.w));
                const float targetH = static_cast<float>(std::max(16, sel.h));
                const float idealZoom = std::clamp(std::min(canvasW * 0.40f / targetW, canvasH * 0.40f / targetH), 1.5f, 8.0f);
                pointEditorZoom = std::round(idealZoom);
                if (pointEditorZoom < 1.0f)
                    pointEditorZoom = idealZoom;
                const float cx = sel.x + sel.w * 0.5f;
                const float cy = sel.y + sel.h * 0.5f;
                pointEditorPan.x = canvasW * 0.5f - cx * sheetScale * pointEditorZoom;
                pointEditorPan.y = canvasH * 0.5f - cy * sheetScale * pointEditorZoom;
            }

            const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
            const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);

            ImGui::InvisibleButton(
                "##sheet_canvas_interactive",
                canvasSize,
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);

            const bool isHovered = ImGui::IsItemHovered();
            const bool isActive = ImGui::IsItemActive();
            const ImGuiIO &io = ImGui::GetIO();
            const ImVec2 mp = io.MousePos;

            // Texture coordinates under mouse cursor
            const float mouseSheetX = (mp.x - canvasMin.x - pointEditorPan.x) / (sheetScale * pointEditorZoom);
            const float mouseSheetY = (mp.y - canvasMin.y - pointEditorPan.y) / (sheetScale * pointEditorZoom);

            // 1. Zoom with Mouse Wheel (anchored around mouse pointer)
            if (isHovered && io.MouseWheel != 0.0f)
            {
                const float factor = (io.MouseWheel > 0.0f) ? 1.25f : 0.8f;
                const float newZoom = std::clamp(pointEditorZoom * factor, 0.15f, 24.0f);
                if (newZoom != pointEditorZoom)
                {
                    const float relX = mp.x - canvasMin.x;
                    const float relY = mp.y - canvasMin.y;
                    pointEditorPan.x = relX - mouseSheetX * sheetScale * newZoom;
                    pointEditorPan.y = relY - mouseSheetY * sheetScale * newZoom;
                    pointEditorZoom = newZoom;
                }
            }

            // 2. Pan with MMB, RMB, or Space + LMB drag
            const bool isPanning = isActive && (
                ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)
                || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)
                || (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)));

            if (isPanning)
            {
                pointEditorPan.x += io.MouseDelta.x;
                pointEditorPan.y += io.MouseDelta.y;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }

            // Sprite coordinates on screen
            const ImVec2 sr0(
                canvasMin.x + pointEditorPan.x + sel.x * sheetScale * pointEditorZoom,
                canvasMin.y + pointEditorPan.y + sel.y * sheetScale * pointEditorZoom);
            const ImVec2 sr1(
                sr0.x + sel.w * sheetScale * pointEditorZoom,
                sr0.y + sel.h * sheetScale * pointEditorZoom);

            const float hTol = 7.0f; // handle hover radius in screen pixels
            auto nearPt = [&](const ImVec2 &pt) {
                return std::abs(mp.x - pt.x) <= hTol && std::abs(mp.y - pt.y) <= hTol;
            };

            const ImVec2 ptNW(sr0.x, sr0.y);
            const ImVec2 ptNE(sr1.x, sr0.y);
            const ImVec2 ptSW(sr0.x, sr1.y);
            const ImVec2 ptSE(sr1.x, sr1.y);
            const ImVec2 ptN(0.5f * (sr0.x + sr1.x), sr0.y);
            const ImVec2 ptS(0.5f * (sr0.x + sr1.x), sr1.y);
            const ImVec2 ptW(sr0.x, 0.5f * (sr0.y + sr1.y));
            const ImVec2 ptE(sr1.x, 0.5f * (sr0.y + sr1.y));

            // Mouse cursor on hover
            if (!isPanning && pointEditorDragMode == PointEditorDragMode::None && isHovered)
            {
                if (nearPt(ptNW) || nearPt(ptSE))
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                else if (nearPt(ptNE) || nearPt(ptSW))
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
                else if (nearPt(ptN) || nearPt(ptS) || (mp.x >= sr0.x && mp.x <= sr1.x && (std::abs(mp.y - sr0.y) <= hTol || std::abs(mp.y - sr1.y) <= hTol)))
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                else if (nearPt(ptW) || nearPt(ptE) || (mp.y >= sr0.y && mp.y <= sr1.y && (std::abs(mp.x - sr0.x) <= hTol || std::abs(mp.x - sr1.x) <= hTol)))
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                else if (mp.x >= sr0.x && mp.x <= sr1.x && mp.y >= sr0.y && mp.y <= sr1.y)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }

            // Mouse click start
            if (!isPanning && isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_Space))
            {
                pointEditorDragStartMouse = mp;
                pointEditorOrigX = sel.x;
                pointEditorOrigY = sel.y;
                pointEditorOrigW = sel.w;
                pointEditorOrigH = sel.h;

                if (nearPt(ptNW))
                    pointEditorDragMode = PointEditorDragMode::ResizeNW;
                else if (nearPt(ptNE))
                    pointEditorDragMode = PointEditorDragMode::ResizeNE;
                else if (nearPt(ptSW))
                    pointEditorDragMode = PointEditorDragMode::ResizeSW;
                else if (nearPt(ptSE))
                    pointEditorDragMode = PointEditorDragMode::ResizeSE;
                else if (nearPt(ptN) || (mp.x >= sr0.x && mp.x <= sr1.x && std::abs(mp.y - sr0.y) <= hTol))
                    pointEditorDragMode = PointEditorDragMode::ResizeN;
                else if (nearPt(ptS) || (mp.x >= sr0.x && mp.x <= sr1.x && std::abs(mp.y - sr1.y) <= hTol))
                    pointEditorDragMode = PointEditorDragMode::ResizeS;
                else if (nearPt(ptW) || (mp.y >= sr0.y && mp.y <= sr1.y && std::abs(mp.x - sr0.x) <= hTol))
                    pointEditorDragMode = PointEditorDragMode::ResizeW;
                else if (nearPt(ptE) || (mp.y >= sr0.y && mp.y <= sr1.y && std::abs(mp.x - sr1.x) <= hTol))
                    pointEditorDragMode = PointEditorDragMode::ResizeE;
                else if (mp.x >= sr0.x && mp.x <= sr1.x && mp.y >= sr0.y && mp.y <= sr1.y)
                    pointEditorDragMode = PointEditorDragMode::MoveRect;
                else
                {
                    // Check if clicked another sprite on this page
                    int clickedSpriteId = -1;
                    for (int sid : obj.sprites)
                    {
                        if (sid < 0 || sid >= static_cast<int>(allSprites.size()) || sid == sel.id)
                            continue;
                        const RawSprite &other = allSprites[static_cast<std::size_t>(sid)];
                        if (other.page == sel.page && mouseSheetX >= other.x && mouseSheetX <= other.x + other.w
                            && mouseSheetY >= other.y && mouseSheetY <= other.y + other.h)
                        {
                            clickedSpriteId = sid;
                            break;
                        }
                    }
                    if (clickedSpriteId < 0)
                    {
                        for (const RawSprite &other : allSprites)
                        {
                            if (other.page == sel.page && other.id != sel.id
                                && mouseSheetX >= other.x && mouseSheetX <= other.x + other.w
                                && mouseSheetY >= other.y && mouseSheetY <= other.y + other.h)
                            {
                                clickedSpriteId = other.id;
                                break;
                            }
                        }
                    }

                    if (clickedSpriteId >= 0)
                    {
                        pointEditorSpriteId = clickedSpriteId;
                        loadedSpriteSelectedId = clickedSpriteId;
                        pointEditorDragMode = PointEditorDragMode::None;
                    }
                    else
                    {
                        pointEditorDragMode = PointEditorDragMode::DrawBox;
                    }
                }
            }

            // Mouse dragging
            if (pointEditorDragMode != PointEditorDragMode::None)
            {
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    pointEditorDragMode = PointEditorDragMode::None;
                }
                else
                {
                    const float deltaSheetX = (mp.x - pointEditorDragStartMouse.x) / (sheetScale * pointEditorZoom);
                    const float deltaSheetY = (mp.y - pointEditorDragStartMouse.y) / (sheetScale * pointEditorZoom);
                    const int idx = static_cast<int>(std::round(deltaSheetX));
                    const int idy = static_cast<int>(std::round(deltaSheetY));

                    int newX = pointEditorOrigX;
                    int newY = pointEditorOrigY;
                    int newW = pointEditorOrigW;
                    int newH = pointEditorOrigH;

                    switch (pointEditorDragMode)
                    {
                        case PointEditorDragMode::MoveRect:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                            newX = std::max(0, pointEditorOrigX + idx);
                            newY = std::max(0, pointEditorOrigY + idy);
                            break;
                        case PointEditorDragMode::ResizeE:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                            newW = std::max(1, pointEditorOrigW + idx);
                            break;
                        case PointEditorDragMode::ResizeW:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                            if (pointEditorOrigW - idx >= 1)
                            {
                                newX = std::max(0, pointEditorOrigX + idx);
                                newW = pointEditorOrigW - idx;
                            }
                            break;
                        case PointEditorDragMode::ResizeS:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                            newH = std::max(1, pointEditorOrigH + idy);
                            break;
                        case PointEditorDragMode::ResizeN:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                            if (pointEditorOrigH - idy >= 1)
                            {
                                newY = std::max(0, pointEditorOrigY + idy);
                                newH = pointEditorOrigH - idy;
                            }
                            break;
                        case PointEditorDragMode::ResizeSE:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                            newW = std::max(1, pointEditorOrigW + idx);
                            newH = std::max(1, pointEditorOrigH + idy);
                            break;
                        case PointEditorDragMode::ResizeNW:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                            if (pointEditorOrigW - idx >= 1) { newX = std::max(0, pointEditorOrigX + idx); newW = pointEditorOrigW - idx; }
                            if (pointEditorOrigH - idy >= 1) { newY = std::max(0, pointEditorOrigY + idy); newH = pointEditorOrigH - idy; }
                            break;
                        case PointEditorDragMode::ResizeNE:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
                            newW = std::max(1, pointEditorOrigW + idx);
                            if (pointEditorOrigH - idy >= 1) { newY = std::max(0, pointEditorOrigY + idy); newH = pointEditorOrigH - idy; }
                            break;
                        case PointEditorDragMode::ResizeSW:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
                            if (pointEditorOrigW - idx >= 1) { newX = std::max(0, pointEditorOrigX + idx); newW = pointEditorOrigW - idx; }
                            newH = std::max(1, pointEditorOrigH + idy);
                            break;
                        case PointEditorDragMode::DrawBox:
                        {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                            const float sX0 = (pointEditorDragStartMouse.x - canvasMin.x - pointEditorPan.x) / (sheetScale * pointEditorZoom);
                            const float sY0 = (pointEditorDragStartMouse.y - canvasMin.y - pointEditorPan.y) / (sheetScale * pointEditorZoom);
                            const int ix0 = static_cast<int>(std::round(sX0));
                            const int iy0 = static_cast<int>(std::round(sY0));
                            const int ix1 = static_cast<int>(std::round(mouseSheetX));
                            const int iy1 = static_cast<int>(std::round(mouseSheetY));
                            newX = std::max(0, std::min(ix0, ix1));
                            newY = std::max(0, std::min(iy0, iy1));
                            newW = std::max(1, std::abs(ix1 - ix0));
                            newH = std::max(1, std::abs(iy1 - iy0));
                            break;
                        }
                        default:
                            break;
                    }

                    if (newX != sel.x || newY != sel.y || newW != sel.w || newH != sel.h)
                    {
                        sel.x = newX;
                        sel.y = newY;
                        sel.w = newW;
                        sel.h = newH;
                        OnSpriteModified(sel);
                    }
                }
            }

            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(canvasMin, canvasMax, true);

            // Dark canvas background
            dl->AddRectFilled(canvasMin, canvasMax, IM_COL32(18, 22, 22, 255));

            // Sheet Texture
            const ImVec2 sheetP0(canvasMin.x + pointEditorPan.x, canvasMin.y + pointEditorPan.y);
            const ImVec2 sheetP1(sheetP0.x + tw * pointEditorZoom, sheetP0.y + th * pointEditorZoom);

            const float visMinX = std::max(canvasMin.x, sheetP0.x);
            const float visMaxX = std::min(canvasMax.x, sheetP1.x);
            const float visMinY = std::max(canvasMin.y, sheetP0.y);
            const float visMaxY = std::min(canvasMax.y, sheetP1.y);
            if (visMaxX > visMinX && visMaxY > visMinY)
            {
                dl->AddRectFilled(ImVec2(visMinX, visMinY), ImVec2(visMaxX, visMaxY), IM_COL32(28, 32, 30, 255));
            }

            dl->AddImage(reinterpret_cast<ImTextureID>(sheetTex), sheetP0, sheetP1);
            dl->AddRect(sheetP0, sheetP1, IM_COL32(90, 130, 115, 220), 0.0f, 0, 1.2f);

            // Pixel Grid when zoomed in
            if (pointEditorShowGrid && pointEditorZoom >= 4.0f)
            {
                const float pixStep = sheetScale * pointEditorZoom;
                const int startX = std::max(0, static_cast<int>(std::floor((canvasMin.x - sheetP0.x) / pixStep)));
                const int endX = std::min(static_cast<int>(tw / sheetScale), static_cast<int>(std::ceil((canvasMax.x - sheetP0.x) / pixStep)));
                const int startY = std::max(0, static_cast<int>(std::floor((canvasMin.y - sheetP0.y) / pixStep)));
                const int endY = std::min(static_cast<int>(th / sheetScale), static_cast<int>(std::ceil((canvasMax.y - sheetP0.y) / pixStep)));

                if ((endX - startX) * (endY - startY) <= 80000)
                {
                    for (int x = startX; x <= endX; ++x)
                    {
                        const float gx = sheetP0.x + x * pixStep;
                        dl->AddLine(ImVec2(gx, std::max(canvasMin.y, sheetP0.y)), ImVec2(gx, std::min(canvasMax.y, sheetP1.y)), IM_COL32(255, 255, 255, 16));
                    }
                    for (int y = startY; y <= endY; ++y)
                    {
                        const float gy = sheetP0.y + y * pixStep;
                        dl->AddLine(ImVec2(std::max(canvasMin.x, sheetP0.x), gy), ImVec2(std::min(canvasMax.x, sheetP1.x), gy), IM_COL32(255, 255, 255, 16));
                    }
                }
            }

            // Draw other sprites on this page
            for (const RawSprite &rs : allSprites)
            {
                if (rs.page != sel.page || rs.id == sel.id)
                    continue;
                const bool belongsToObj = (obj.sprites.count(rs.id) > 0);
                const ImVec2 r0(
                    canvasMin.x + pointEditorPan.x + rs.x * sheetScale * pointEditorZoom,
                    canvasMin.y + pointEditorPan.y + rs.y * sheetScale * pointEditorZoom);
                const ImVec2 r1(
                    r0.x + rs.w * sheetScale * pointEditorZoom,
                    r0.y + rs.h * sheetScale * pointEditorZoom);

                if (r1.x < canvasMin.x || r0.x > canvasMax.x || r1.y < canvasMin.y || r0.y > canvasMax.y)
                    continue;

                if (belongsToObj)
                {
                    dl->AddRect(r0, r1, IM_COL32(60, 220, 190, 200), 0.0f, 0, 1.2f);
                }
                else
                {
                    dl->AddRect(r0, r1, IM_COL32(50, 110, 90, 75));
                }
            }

            // Selected active sprite highlight
            dl->AddRectFilled(sr0, sr1, IM_COL32(255, 230, 50, 45));
            dl->AddRect(sr0, sr1, IM_COL32(255, 230, 50, 255), 0.0f, 0, 2.0f);

            // Center crosshair
            const ImVec2 center(0.5f * (sr0.x + sr1.x), 0.5f * (sr0.y + sr1.y));
            dl->AddLine(ImVec2(sr0.x - 4.0f, center.y), ImVec2(sr1.x + 4.0f, center.y), IM_COL32(255, 240, 120, 220), 1.0f);
            dl->AddLine(ImVec2(center.x, sr0.y - 4.0f), ImVec2(center.x, sr1.y + 4.0f), IM_COL32(255, 240, 120, 220), 1.0f);

            // 8 Interactive Handles
            const float hr = 3.5f;
            auto drawHandle = [&](const ImVec2 &c) {
                dl->AddRectFilled(ImVec2(c.x - hr, c.y - hr), ImVec2(c.x + hr, c.y + hr), IM_COL32(255, 255, 255, 255));
                dl->AddRect(ImVec2(c.x - hr, c.y - hr), ImVec2(c.x + hr, c.y + hr), IM_COL32(25, 25, 25, 255));
            };
            drawHandle(ptNW);
            drawHandle(ptNE);
            drawHandle(ptSW);
            drawHandle(ptSE);
            drawHandle(ptN);
            drawHandle(ptS);
            drawHandle(ptW);
            drawHandle(ptE);

            // Top-left HUD badge on canvas
            char hudBuf[128];
            std::snprintf(hudBuf, sizeof(hudBuf), "Sprite #%d: (%d, %d) %dx%d  |  Zoom: %.0f%%", sel.id, sel.x, sel.y, sel.w, sel.h, pointEditorZoom * 100.0f);
            const ImVec2 hudSz = ImGui::CalcTextSize(hudBuf);
            const ImVec2 hb0(canvasMin.x + 8.0f, canvasMin.y + 8.0f);
            const ImVec2 hb1(hb0.x + hudSz.x + 12.0f, hb0.y + hudSz.y + 6.0f);
            dl->AddRectFilled(hb0, hb1, IM_COL32(10, 15, 12, 220), 3.0f);
            dl->AddRect(hb0, hb1, IM_COL32(60, 120, 95, 200), 3.0f);
            dl->AddText(ImVec2(hb0.x + 6.0f, hb0.y + 3.0f), IM_COL32(255, 235, 120, 255), hudBuf);

            dl->PopClipRect();

            if (isHovered && pointEditorDragMode == PointEditorDragMode::None)
            {
                ImGui::SetTooltip(
                    _("Sheet (%.0f, %.0f)  |  Sprite #%d: (%d, %d) %dx%d\nLMB Drag: Move/Resize | MMB / Space+Drag: Pan | Wheel: Zoom | Click sprite: Select"),
                    mouseSheetX, mouseSheetY, sel.id, sel.x, sel.y, sel.w, sel.h);
            }
        }
    }
    ImGui::EndChild();
}

} // namespace CharEditor
