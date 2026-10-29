#include "CharacterEditor.hpp"

#include "game/Research.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace CharEditor {

namespace {

constexpr const char* kFactionFiles[2] = {"research_confederation.yaml", "research_resistance.yaml"};

constexpr float kNodeW = 210.0f;
constexpr float kNodeH = 52.0f;
constexpr float kGapX = 56.0f;
constexpr float kGapY = 14.0f;

int needsOf(const YAML::Node& list, int index) {
    return yamlInt(list[static_cast<size_t>(index)], "needs", -1);
}

// `candidate` already depends on `index` (directly or through its chain): making `index` need it would loop
bool dependsOn(const YAML::Node& list, int candidate, int index) {
    for (int guard = 0; candidate >= 0 && guard < 64; ++guard) {
        if (candidate == index) return true;
        candidate = needsOf(list, candidate);
    }
    return false;
}

} // namespace

bool CharacterEditor::loadResearch(int faction) {
    faction = std::clamp(faction, 0, 1);
    try {
        researchPath[faction] = charactersDir / kFactionFiles[faction];
        researchDoc[faction] = YAML::LoadFile(researchPath[faction].string());
        const YAML::Node list = researchDoc[faction]["research"];
        if (!list || !list.IsSequence() || list.size() == 0) {
            researchDoc[faction] = YAML::Node();
            status = std::string(_("No research list in ")) + kFactionFiles[faction];
            return false;
        }
        researchDirty[faction] = false;
        return true;
    } catch (const std::exception& error) {
        researchDoc[faction] = YAML::Node();
        status = std::string(_("Could not load YAML: ")) + error.what();
        return false;
    }
}

bool CharacterEditor::saveResearch() {
    const int f = researchFaction;
    if (!researchDoc[f].IsMap()) return false;
    try {
        YAML::Emitter emitter;
        emitter.SetIndent(2);
        emitter << researchDoc[f];
        std::ofstream output(researchPath[f]);
        if (!output) {
            status = std::string(_("Could not open file for writing: ")) + researchPath[f].string();
            return false;
        }
        output << emitter.c_str() << '\n';
        if (!output) {
            status = std::string(_("Failed to write file: ")) + researchPath[f].string();
            return false;
        }
        researchDirty[f] = false;
        status = std::string(_("Saved: ")) + kFactionFiles[f];
        return true;
    } catch (const std::exception& error) {
        status = std::string(_("YAML save error: ")) + error.what();
        return false;
    }
}

void CharacterEditor::drawResearchTree() {
    for (int f = 0; f < 2; ++f) {
        if (researchPath[f].empty()) loadResearch(f);
    }

    const char* factions[2] = {_("Confederation"), _("Resistance")};
    for (int f = 0; f < 2; ++f) {
        if (f) ImGui::SameLine();
        if (hinted(ImGui::RadioButton(factions[f], researchFaction == f), f == 0 ? "Confederation" : "Resistance")) {
            researchFaction = f;
            researchSelected = 0;
        }
    }
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Save research")), "Save research")) saveResearch();
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Reload")), "Reload")) loadResearch(researchFaction);
    if (researchDirty[researchFaction]) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f), "%s", _("Unsaved changes"));
    }

    YAML::Node list = researchDoc[researchFaction]["research"];
    if (!researchDoc[researchFaction].IsMap() || !list || !list.IsSequence()) {
        ImGui::TextWrapped("%s", _("The research file could not be loaded."));
        return;
    }
    const int count = static_cast<int>(list.size());
    researchSelected = std::clamp(researchSelected, 0, count - 1);

    // Layout: columns are the depth in the chain of requirements, rows are leaves in tree order,
    // a parent sits at the middle of its children
    std::vector<int> parent(count), depth(count, 0);
    std::vector<std::vector<int>> children(count);
    std::vector<int> roots;
    for (int i = 0; i < count; ++i) {
        int need = needsOf(list, i);
        if (need < 0 || need >= count || need == i || dependsOn(list, need, i)) need = -1;
        parent[i] = need;
        if (need < 0) roots.push_back(i);
        else children[need].push_back(i);
    }
    std::vector<float> row(count, 0.0f);
    float nextRow = 0.0f;
    std::function<void(int, int)> place = [&](int node, int d) {
        depth[node] = d;
        if (children[node].empty()) {
            row[node] = nextRow;
            nextRow += 1.0f;
            return;
        }
        for (int c : children[node]) place(c, d + 1);
        row[node] = (row[children[node].front()] + row[children[node].back()]) * 0.5f;
    };
    // Single technologies share the first columns compactly, branches follow
    for (int r : roots) place(r, 0);

    int maxDepth = 0;
    for (int d : depth) maxDepth = std::max(maxDepth, d);

    ImGui::BeginChild("research-tree", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float pad = 8.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto nodePos = [&](int i) {
        return ImVec2(origin.x + pad + depth[i] * (kNodeW + kGapX), origin.y + pad + row[i] * (kNodeH + kGapY));
    };

    // Links first, nodes on top of them
    for (int i = 0; i < count; ++i) {
        if (parent[i] < 0) continue;
        const ImVec2 a = nodePos(parent[i]);
        const ImVec2 b = nodePos(i);
        const ImVec2 from(a.x + kNodeW, a.y + kNodeH * 0.5f);
        const ImVec2 to(b.x, b.y + kNodeH * 0.5f);
        const float mid = (from.x + to.x) * 0.5f;
        const bool hot = (i == researchSelected || parent[i] == researchSelected);
        dl->AddBezierCubic(from, ImVec2(mid, from.y), ImVec2(mid, to.y), to,
                           hot ? IM_COL32(240, 170, 60, 255) : IM_COL32(110, 150, 140, 255), hot ? 2.5f : 1.5f);
    }

    for (int i = 0; i < count; ++i) {
        const YAML::Node n = list[static_cast<size_t>(i)];
        const ImVec2 p = nodePos(i);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("node", ImVec2(kNodeW, kNodeH))) researchSelected = i;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const bool selected = i == researchSelected;
        dl->AddRectFilled(p, ImVec2(p.x + kNodeW, p.y + kNodeH),
                          selected ? IM_COL32(52, 96, 84, 255) : (hovered ? IM_COL32(40, 62, 58, 255) : IM_COL32(30, 44, 42, 255)),
                          4.0f);
        dl->AddRect(p, ImVec2(p.x + kNodeW, p.y + kNodeH),
                    selected ? IM_COL32(240, 170, 60, 255) : IM_COL32(90, 130, 120, 255), 4.0f, 0, selected ? 2.0f : 1.0f);

        const int icon = yamlInt(n, "icon", 0);
        if (SDL_Texture* thumb = compositeThumbnail(icon, false)) {
            dl->AddImage(reinterpret_cast<ImTextureID>(thumb), ImVec2(p.x + 4, p.y + 7), ImVec2(p.x + 4 + 60, p.y + 7 + 38));
        }
        const std::string name = yamlString(n, "name", "?");
        const std::string line2 = "#" + std::to_string(i) + "  " + std::to_string(yamlInt(n, "cost", 0)) + " cr";
        dl->PushClipRect(ImVec2(p.x + 68, p.y), ImVec2(p.x + kNodeW - 2, p.y + kNodeH), true);
        dl->AddText(ImVec2(p.x + 68, p.y + 8), IM_COL32(230, 235, 232, 255), _(name.c_str()));
        dl->AddText(ImVec2(p.x + 68, p.y + 28), IM_COL32(150, 175, 168, 255), line2.c_str());
        dl->PopClipRect();
    }

    // Reserve the scrollable area
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(pad * 2 + (maxDepth + 1) * (kNodeW + kGapX), pad * 2 + std::max(1.0f, nextRow) * (kNodeH + kGapY)));
    ImGui::EndChild();
}

void CharacterEditor::drawResearchProperties() {
    const int f = researchFaction;
    if (researchPath[f].empty()) loadResearch(f);
    YAML::Node list = researchDoc[f].IsMap() ? researchDoc[f]["research"] : YAML::Node();
    if (!list || !list.IsSequence() || list.size() == 0) {
        ImGui::TextWrapped("%s", _("The research file could not be loaded."));
        return;
    }
    const int count = static_cast<int>(list.size());
    researchSelected = std::clamp(researchSelected, 0, count - 1);
    const int i = researchSelected;
    YAML::Node n = list[static_cast<size_t>(i)];
    bool changed = false;

    ImGui::TextWrapped("%s", researchPath[f].string().c_str());
    ImGui::Text(_("Technology #%d"), i);
    ImGui::Separator();

    const std::string scope = "research/" + std::to_string(f) + "/" + std::to_string(i);
    changed |= editString(n, "name", "Technology name", scope);
    changed |= editString(n, "description", "Technology description", scope, true);
    changed |= editInt(n, "cost", "Research cost (credits)", 0);

    // Icon
    const int icon = yamlInt(n, "icon", 0);
    ImGui::Text(_("Icon: %d"), icon);
    const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 70.0f));
    fieldHint("Icon");
    ImGui::BeginChild("research-icons", ImVec2(0.0f, 150.0f), true);
    int k = 0;
    for (int comp = OpenArtOfWar::kResearchIconFirst; comp <= OpenArtOfWar::kResearchIconLast; ++comp) {
        if (k++ % perRow != 0) ImGui::SameLine();
        ImGui::PushID(comp);
        const bool selected = comp == icon;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
        SDL_Texture* thumb = compositeThumbnail(comp, false);
        const bool clicked = thumb ? ImGui::ImageButton("icon", reinterpret_cast<ImTextureID>(thumb), ImVec2(60, 38))
                                   : ImGui::Button(std::to_string(comp).c_str(), ImVec2(60, 38));
        if (selected) ImGui::PopStyleColor();
        if (clicked) {
            n["icon"] = comp;
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Effect
    using OpenArtOfWar::ResearchEffect;
    ResearchEffect effect = ResearchEffect::InfantryArmor;
    OpenArtOfWar::parseResearchEffect(yamlString(n, "effect"), effect);
    if (hinted(ImGui::BeginCombo(_("Effect"), OpenArtOfWar::researchEffectName(effect)), "Effect")) {
        for (int e = 0; e < static_cast<int>(ResearchEffect::Count); ++e) {
            const char* label = OpenArtOfWar::researchEffectName(static_cast<ResearchEffect>(e));
            if (ImGui::Selectable(label, e == static_cast<int>(effect))) {
                n["effect"] = std::string(label);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    changed |= editInt(n, "value", "Value", 0);
    changed |= editInt(n, "value2", "Second value", 0);
    ImGui::TextDisabled("%s", _("Value / second value: see the effect list in game/Research.hpp"));

    // Units the effect is about
    auto archPicker = [&](const char* key, const char* label) {
        const int current = yamlInt(n, key, -1);
        const auto* info = OpenArtOfWar::Utility::archetypeInfo(current);
        const std::string preview = info ? "#" + std::to_string(current) + " " + _(info->name) : std::string(_("(none)"));
        if (hinted(ImGui::BeginCombo(_(label), preview.c_str()), label)) {
            if (ImGui::Selectable(_("(none)"), current < 0)) {
                n[key] = -1;
                changed = true;
            }
            for (int a = 0; a < OpenArtOfWar::Utility::kArchetypeCount; ++a) {
                const auto* ai = OpenArtOfWar::Utility::archetypeInfo(a);
                if (!ai || ai->baseFormId >= 0) continue; // siege / baseline forms count as their base unit
                const std::string text = "#" + std::to_string(a) + " " + _(ai->name) + " - " +
                                         _(OpenArtOfWar::Utility::factionName(ai->faction));
                if (ImGui::Selectable(text.c_str(), a == current)) {
                    n[key] = a;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    };
    archPicker("arch_a", "Unit A");
    archPicker("arch_b", "Unit B");

    // Requirement: the tree is built from it
    const int need = yamlInt(n, "needs", -1);
    auto techLabel = [&](int t) {
        return "#" + std::to_string(t) + " " + _(yamlString(list[static_cast<size_t>(t)], "name", "?").c_str());
    };
    const std::string needPreview = (need >= 0 && need < count) ? techLabel(need) : std::string(_("(none)"));
    if (hinted(ImGui::BeginCombo(_("Requires"), needPreview.c_str()), "Requires")) {
        if (ImGui::Selectable(_("(none)"), need < 0)) {
            n["needs"] = -1;
            changed = true;
        }
        for (int t = 0; t < count; ++t) {
            if (t == i || dependsOn(list, t, i)) continue; // would make a loop
            if (ImGui::Selectable(techLabel(t).c_str(), t == need)) {
                n["needs"] = t;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    changed |= editBool(n, "player_only", "Player only", false);

    if (changed) researchDirty[f] = true;
    if (researchDirty[f]) {
        ImGui::Separator();
        if (ImGui::Button(_("Save research"))) saveResearch();
    }
}

} // namespace CharEditor
