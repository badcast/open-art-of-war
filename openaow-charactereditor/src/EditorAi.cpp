#include "CharacterEditor.hpp"

#include <algorithm>
#include <cmath>

namespace CharEditor {

namespace {

const char* const kLevels[4] = {"easy", "medium", "hard", "hardcore"};
const char* const kLevelLabels[4] = {"Easy", "Medium", "Hard", "Hardcore"};
const ImU32 kLevelColors[4] = {IM_COL32(104, 204, 124, 255), IM_COL32(232, 200, 74, 255), IM_COL32(236, 140, 60, 255),
                               IM_COL32(226, 80, 70, 255)};

double num(const CommentedYaml& y, const std::string& path, double fallback) {
    const YAML::Node n = y.at(path);
    try {
        return n.IsScalar() ? n.as<double>() : fallback;
    } catch (...) {
        return fallback;
    }
}

// A small line chart: one series per difficulty, x = 0..count-1
void drawChart(const char* id, const char* title, const std::vector<double> series[4], int count, ImVec2 size, const char* xLabel) {
    ImGui::TextUnformatted(title);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, IM_COL32(16, 20, 24, 255));
    dl->AddRect(p0, p1, IM_COL32(60, 70, 80, 255));
    double top = 1.0;
    for (int s = 0; s < 4; ++s) for (double v : series[s]) top = std::max(top, v);
    const float padL = 44.0f, padB = 18.0f, padT = 8.0f, padR = 8.0f;
    const ImVec2 a(p0.x + padL, p0.y + padT), b(p1.x - padR, p1.y - padB);
    for (int g = 0; g <= 4; ++g) {
        const float y = b.y - (b.y - a.y) * static_cast<float>(g) / 4.0f;
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(50, 58, 66, 255));
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f", top * g / 4.0);
        dl->AddText(ImVec2(p0.x + 4.0f, y - 7.0f), IM_COL32(150, 160, 176, 255), buf);
    }
    dl->AddText(ImVec2(a.x, p1.y - 16.0f), IM_COL32(150, 160, 176, 255), xLabel);
    for (int s = 0; s < 4; ++s) {
        const auto& v = series[s];
        for (int i = 1; i < count && i < static_cast<int>(v.size()); ++i) {
            auto pt = [&](int k) {
                return ImVec2(a.x + (b.x - a.x) * static_cast<float>(k) / static_cast<float>(std::max(1, count - 1)),
                              b.y - (b.y - a.y) * static_cast<float>(v[k] / top));
            };
            dl->AddLine(pt(i - 1), pt(i), kLevelColors[s], 2.0f);
        }
    }
    // Legend
    float lx = a.x + 8.0f;
    for (int s = 0; s < 4; ++s) {
        dl->AddRectFilled(ImVec2(lx, a.y + 4.0f), ImVec2(lx + 10.0f, a.y + 14.0f), kLevelColors[s]);
        dl->AddText(ImVec2(lx + 14.0f, a.y + 2.0f), IM_COL32(210, 216, 224, 255), _(kLevelLabels[s]));
        lx += 14.0f + ImGui::CalcTextSize(_(kLevelLabels[s])).x + 14.0f;
    }
}

} // namespace

void CharacterEditor::ensureAi() {
    if (aiTried) return;
    aiTried = true;
    loadYamlFile(aiYaml, "difficulty.yaml");
}

void CharacterEditor::drawAiDifficulty() {
    ensureAi();
    if (!aiYaml.loaded()) {
        ImGui::TextWrapped("%s", _("The difficulty file could not be loaded."));
        return;
    }
    // The four levels as a chain, like the technology tree
    const float gap = 28.0f, nodeH = 112.0f;
    const float nodeW = std::clamp((ImGui::GetContentRegionAvail().x - 3.0f * gap) / 4.0f, 150.0f, 230.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 4; ++i) {
        const std::string base = kLevels[i];
        const ImVec2 a(origin.x + static_cast<float>(i) * (nodeW + gap), origin.y);
        const ImVec2 b(a.x + nodeW, a.y + nodeH);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("node", ImVec2(nodeW, nodeH))) aiSelected = i;
        ImGui::PopID();
        const bool selected = aiSelected == i;
        const bool hot = ImGui::IsItemHovered();
        dl->AddRectFilled(a, b, hot ? IM_COL32(38, 46, 56, 255) : IM_COL32(26, 32, 40, 255), 4.0f);
        dl->AddRect(a, b, selected ? kLevelColors[i] : IM_COL32(70, 80, 92, 255), 4.0f, 0, selected ? 3.0f : 1.0f);
        dl->AddRectFilled(a, ImVec2(a.x + 5.0f, b.y), kLevelColors[i]);
        const std::string name = aiYaml.at(base + ".name").IsScalar() ? aiYaml.at(base + ".name").Scalar() : base;
        dl->AddText(ImVec2(a.x + 14.0f, a.y + 8.0f), IM_COL32(236, 240, 246, 255), name.c_str());
        char line[96];
        float y = a.y + 30.0f;
        auto row = [&](const char* fmt, double v1, double v2 = 0.0) {
            std::snprintf(line, sizeof line, fmt, v1, v2);
            dl->AddText(ImVec2(a.x + 14.0f, y), IM_COL32(170, 180, 194, 255), line);
            y += 17.0f;
        };
        row(_("Credits %.0f, +%.0f / s"), num(aiYaml, base + ".starting_credits", 0), num(aiYaml, base + ".credits_per_second", 0));
        row(_("Waves %.0f..%.0f units"), num(aiYaml, base + ".tactics.wave_base_size", 0), num(aiYaml, base + ".tactics.wave_max_size", 0));
        row(_("Every %.0f s, tier %.0f"), num(aiYaml, base + ".tactics.wave_interval_seconds", 0), num(aiYaml, base + ".tactics.max_wave_tier", 0));
        row(_("Groups up to %.0f, research %.0f s"), num(aiYaml, base + ".tactics.max_attack_groups", 0), num(aiYaml, base + ".tactics.research_seconds", 0));
        if (i < 3) {
            const ImVec2 m(b.x, a.y + nodeH * 0.5f);
            dl->AddLine(m, ImVec2(m.x + gap, m.y), IM_COL32(120, 130, 142, 255), 2.0f);
            dl->AddTriangleFilled(ImVec2(m.x + gap, m.y), ImVec2(m.x + gap - 8.0f, m.y - 5.0f), ImVec2(m.x + gap - 8.0f, m.y + 5.0f),
                                  IM_COL32(120, 130, 142, 255));
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + nodeH + 14.0f));

    // Curves of the four levels
    constexpr int kWaves = 14;
    std::vector<double> waves[4], bank[4];
    constexpr int kMinutes = 15;
    for (int s = 0; s < 4; ++s) {
        const std::string base = kLevels[s];
        const double b0 = num(aiYaml, base + ".tactics.wave_base_size", 10), grow = num(aiYaml, base + ".tactics.wave_growth", 1),
                     mx = num(aiYaml, base + ".tactics.wave_max_size", 14);
        for (int n = 0; n < kWaves; ++n) waves[s].push_back(std::min(mx, b0 + grow * n));
        const double c0 = num(aiYaml, base + ".starting_credits", 0), cps = num(aiYaml, base + ".credits_per_second", 0);
        for (int m = 0; m <= kMinutes; ++m) bank[s].push_back(c0 + cps * 60.0 * m);
    }
    const float chartW = std::max(240.0f, (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f);
    drawChart("waves", _("Units in wave N"), waves, kWaves, ImVec2(chartW, 190.0f), _("wave number"));
    ImGui::SameLine();
    ImGui::BeginGroup();
    drawChart("bank", _("Credits earned by minute"), bank, kMinutes + 1, ImVec2(chartW, 190.0f), _("minutes"));
    ImGui::EndGroup();

    // Time between waves, as bars
    ImGui::Spacing();
    ImGui::TextUnformatted(_("Pause between waves (seconds)"));
    double longest = 1.0;
    for (int s = 0; s < 4; ++s) longest = std::max(longest, num(aiYaml, std::string(kLevels[s]) + ".tactics.wave_interval_seconds", 0));
    for (int s = 0; s < 4; ++s) {
        const double v = num(aiYaml, std::string(kLevels[s]) + ".tactics.wave_interval_seconds", 0);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float full = std::max(100.0f, ImGui::GetContentRegionAvail().x - 120.0f);
        ImGui::Dummy(ImVec2(full + 100.0f, 18.0f));
        dl->AddText(p, IM_COL32(210, 216, 224, 255), _(kLevelLabels[s]));
        dl->AddRectFilled(ImVec2(p.x + 80.0f, p.y + 2.0f), ImVec2(p.x + 80.0f + full * static_cast<float>(v / longest), p.y + 16.0f), kLevelColors[s], 2.0f);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.0f", v);
        dl->AddText(ImVec2(p.x + 86.0f, p.y + 1.0f), IM_COL32(10, 12, 14, 255), buf);
    }
}

void CharacterEditor::drawAiProperties() {
    ensureAi();
    if (!aiYaml.loaded()) {
        ImGui::TextWrapped("%s", _("The difficulty file could not be loaded."));
        return;
    }
    drawSettingsBar(aiYaml, "difficulty.yaml");
    ImGui::Separator();
    // Level used when the game starts
    const YAML::Node def = aiYaml.at("default_difficulty");
    const std::string current = def.IsScalar() ? def.Scalar() : "medium";
    ImGui::SetNextItemWidth(180.0f);
    int idx = 1;
    for (int i = 0; i < 4; ++i) if (current == kLevels[i]) idx = i;
    if (ImGui::BeginCombo(_("Default difficulty"), _(kLevelLabels[idx]))) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(_(kLevelLabels[i]), i == idx)) aiYaml.set("default_difficulty", std::string("\"") + kLevels[i] + "\"");
        }
        ImGui::EndCombo();
    }
    fieldHint("Default difficulty");
    ImGui::Spacing();
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (hinted(ImGui::RadioButton(_(kLevelLabels[i]), aiSelected == i), kLevelLabels[i])) aiSelected = i;
    }
    ImGui::Separator();
    editYamlTree(aiYaml, kLevels[aiSelected], 0);
}

} // namespace CharEditor
