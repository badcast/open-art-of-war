#include "CharacterEditor.hpp"

#include <algorithm>
#include <cstdio>

namespace CharEditor {

namespace {

const char* const kThemeKeys[3] = {"neutral", "resistance", "confederation"};
const char* const kThemeLabels[3] = {"Menus", "Resistance", "Confederation"};

struct ThemeField {
    const char* key;
    const char* label;
};
const ThemeField kThemeFields[] = {
    {"panel", "Panel"},           {"panel_raised", "Raised panel"}, {"panel_sunken", "Sunken panel"},
    {"border", "Border"},         {"border_strong", "Strong border"}, {"hover", "Hover"},
    {"accent", "Accent"},         {"accent_deep", "Deep accent"},   {"accent_text", "Text on accent"},
};
const ThemeField kFixedFields[] = {
    {"backdrop", "Backdrop"}, {"scrim", "Dimming"}, {"text", "Text"},         {"text_dim", "Secondary text"},
    {"text_muted", "Hints"},  {"danger", "Danger"}, {"danger_deep", "Danger fill"}, {"ok", "OK"}, {"info", "Info"},
};

ImU32 parseColor(const YAML::Node& n, ImU32 fallback) {
    if (!n.IsDefined() || !n.IsScalar()) return fallback;
    const std::string& s = n.Scalar();
    unsigned r = 0, g = 0, b = 0, a = 255;
    if (s.size() < 7 || s[0] != '#' || std::sscanf(s.c_str() + 1, "%2x%2x%2x", &r, &g, &b) != 3) return fallback;
    if (s.size() >= 9) std::sscanf(s.c_str() + 7, "%2x", &a);
    return IM_COL32(r, g, b, a);
}

} // namespace

void CharacterEditor::ensureUi() {
    if (uiTried) return;
    uiTried = true;
    loadYamlFile(uiYaml, "game_ui.yaml");
}

void CharacterEditor::drawGameUi() {
    ensureUi();
    ImGui::TextWrapped("%s", _("Preview of the game interface with the colors of the file. Pick a theme on the right panel."));
    if (!uiYaml.loaded()) return;
    const std::string tp = std::string("themes.") + kThemeKeys[uiTheme];
    auto themeColor = [&](const char* key, ImU32 fb) { return parseColor(uiYaml.at(tp + "." + key), fb); };
    auto fixedColor = [&](const char* key, ImU32 fb) { return parseColor(uiYaml.at(std::string("colors.") + key), fb); };

    const ImU32 backdrop = fixedColor("backdrop", IM_COL32(8, 10, 14, 255));
    const ImU32 text = fixedColor("text", IM_COL32(226, 231, 238, 255));
    const ImU32 textDim = fixedColor("text_dim", IM_COL32(150, 160, 176, 255));
    const ImU32 textMuted = fixedColor("text_muted", IM_COL32(100, 110, 126, 255));
    const ImU32 danger = fixedColor("danger", IM_COL32(226, 88, 74, 255));
    const ImU32 dangerDeep = fixedColor("danger_deep", IM_COL32(70, 26, 22, 255));
    const ImU32 ok = fixedColor("ok", IM_COL32(104, 204, 124, 255));
    const ImU32 info = fixedColor("info", IM_COL32(96, 174, 232, 255));
    const ImU32 scrim = fixedColor("scrim", IM_COL32(4, 6, 9, 190));
    const ImU32 panel = themeColor("panel", IM_COL32(17, 21, 28, 246));
    const ImU32 raised = themeColor("panel_raised", IM_COL32(25, 30, 40, 255));
    const ImU32 sunken = themeColor("panel_sunken", IM_COL32(11, 14, 19, 255));
    const ImU32 border = themeColor("border", IM_COL32(50, 60, 76, 255));
    const ImU32 borderStrong = themeColor("border_strong", IM_COL32(78, 92, 114, 255));
    const ImU32 hover = themeColor("hover", IM_COL32(33, 40, 53, 255));
    const ImU32 accent = themeColor("accent", IM_COL32(232, 176, 74, 255));
    const ImU32 accentDeep = themeColor("accent_deep", IM_COL32(120, 84, 30, 255));
    const ImU32 accentText = themeColor("accent_text", IM_COL32(24, 18, 8, 255));

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("ui-mock", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    dl->PushClipRect(origin, end, true);
    dl->AddRectFilled(origin, end, backdrop);

    auto plate = [&](ImVec2 a, ImVec2 b, ImU32 fill, ImU32 line) {
        dl->AddRectFilled(a, b, fill, 3.0f);
        dl->AddRect(a, b, line, 3.0f);
    };
    auto button = [&](ImVec2 a, ImVec2 b, const char* label, int style, bool hot) {
        ImU32 fill = style == 0 ? accent : style == 1 ? raised : style == 2 ? dangerDeep : 0;
        ImU32 line = style == 0 ? accent : style == 2 ? danger : border;
        ImU32 ink = style == 0 ? accentText : style == 2 ? danger : text;
        if (hot) fill = style == 0 ? accentDeep : hover;
        if (fill) dl->AddRectFilled(a, b, fill, 3.0f);
        dl->AddRect(a, b, hot ? borderStrong : line, 3.0f);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), ink, label);
    };

    // Menu dialog over the dimmed screen
    dl->AddRectFilled(origin, end, scrim);
    const float dw = std::min(360.0f, size.x * 0.55f);
    const ImVec2 d0(origin.x + 24.0f, origin.y + 24.0f);
    const ImVec2 d1(d0.x + dw, d0.y + 300.0f);
    plate(d0, d1, panel, border);
    dl->AddRectFilled(d0, ImVec2(d1.x, d0.y + 34.0f), raised, 3.0f);
    dl->AddRectFilled(d0, ImVec2(d0.x + 4.0f, d0.y + 34.0f), accent);
    dl->AddText(ImVec2(d0.x + 14.0f, d0.y + 9.0f), text, _("Settings"));
    float y = d0.y + 46.0f;
    dl->AddText(ImVec2(d0.x + 14.0f, y), textDim, _("Secondary text"));
    y += 22.0f;
    dl->AddText(ImVec2(d0.x + 14.0f, y), textMuted, _("Hints and disabled items"));
    y += 28.0f;
    plate(ImVec2(d0.x + 14.0f, y), ImVec2(d1.x - 14.0f, y + 28.0f), sunken, border);
    dl->AddText(ImVec2(d0.x + 22.0f, y + 6.0f), text, _("English"));
    dl->AddTriangleFilled(ImVec2(d1.x - 34.0f, y + 10.0f), ImVec2(d1.x - 22.0f, y + 10.0f), ImVec2(d1.x - 28.0f, y + 18.0f), textDim);
    y += 40.0f;
    const float bw = (dw - 14.0f * 2 - 8.0f) * 0.5f;
    button(ImVec2(d0.x + 14.0f, y), ImVec2(d0.x + 14.0f + bw, y + 32.0f), _("Apply"), 0, false);
    button(ImVec2(d0.x + 22.0f + bw, y), ImVec2(d1.x - 14.0f, y + 32.0f), _("Cancel"), 1, false);
    y += 42.0f;
    button(ImVec2(d0.x + 14.0f, y), ImVec2(d0.x + 14.0f + bw, y + 32.0f), _("Hovered"), 0, true);
    button(ImVec2(d0.x + 22.0f + bw, y), ImVec2(d1.x - 14.0f, y + 32.0f), _("Hovered"), 1, true);
    y += 42.0f;
    button(ImVec2(d0.x + 14.0f, y), ImVec2(d1.x - 14.0f, y + 32.0f), _("Delete"), 2, false);
    y += 46.0f;
    dl->AddRectFilled(ImVec2(d0.x + 14.0f, y), ImVec2(d1.x - 14.0f, y + 8.0f), sunken, 2.0f);
    dl->AddRectFilled(ImVec2(d0.x + 14.0f, y), ImVec2(d0.x + 14.0f + (dw - 28.0f) * 0.62f, y + 8.0f), ok, 2.0f);

    // In-game sidebar: radar, build icons, status chips
    const float sw = std::min(260.0f, size.x * 0.35f);
    const ImVec2 s0(end.x - sw - 24.0f, origin.y + 24.0f);
    const ImVec2 s1(end.x - 24.0f, s0.y + 360.0f);
    plate(s0, s1, panel, borderStrong);
    plate(ImVec2(s0.x + 10.0f, s0.y + 10.0f), ImVec2(s1.x - 10.0f, s0.y + 130.0f), sunken, border);
    dl->AddText(ImVec2(s0.x + 18.0f, s0.y + 18.0f), textMuted, _("Radar"));
    dl->AddText(ImVec2(s0.x + 14.0f, s0.y + 140.0f), accent, "CREDITS 2000");
    const float cell = (sw - 20.0f - 18.0f) / 4.0f;
    for (int i = 0; i < 8; ++i) {
        const float cx = s0.x + 10.0f + static_cast<float>(i % 4) * (cell + 6.0f);
        const float cy = s0.y + 168.0f + static_cast<float>(i / 4) * (cell + 6.0f);
        plate(ImVec2(cx, cy), ImVec2(cx + cell, cy + cell), i == 1 ? hover : raised, i == 1 ? accent : border);
    }
    const float chipY = s0.y + 168.0f + 2.0f * (cell + 6.0f) + 8.0f;
    dl->AddText(ImVec2(s0.x + 14.0f, chipY), ok, _("Ready"));
    dl->AddText(ImVec2(s0.x + 14.0f, chipY + 20.0f), info, _("Researching"));
    dl->AddText(ImVec2(s0.x + 14.0f, chipY + 40.0f), danger, _("No power"));
    dl->PopClipRect();
}

void CharacterEditor::drawGameUiProperties() {
    ensureUi();
    if (!uiYaml.loaded()) {
        ImGui::TextWrapped("%s", _("The interface file could not be loaded."));
        return;
    }
    drawSettingsBar(uiYaml, "game_ui.yaml");
    ImGui::Separator();
    ImGui::TextUnformatted(_("Theme"));
    fieldHint("UI theme");
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        if (hinted(ImGui::RadioButton(_(kThemeLabels[i]), uiTheme == i), kThemeLabels[i])) uiTheme = i;
    }
    ImGui::Spacing();
    const std::string tp = std::string("themes.") + kThemeKeys[uiTheme];
    for (const ThemeField& f : kThemeFields) editYamlScalar(uiYaml, tp + "." + f.key, _(f.label));
    if (ImGui::CollapsingHeader(_("Colors of all themes"), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const ThemeField& f : kFixedFields) editYamlScalar(uiYaml, std::string("colors.") + f.key, _(f.label));
    }
}

} // namespace CharEditor
