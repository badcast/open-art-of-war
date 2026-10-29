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

void CharacterEditor::setEditorMode(EditorMode mode) {
    if (editorMode == mode) return;
    editorMode = mode;
    if (mode == EditorMode::Simulation) {
        if (battle.active()) frameBattleCamera();
    } else {
        cameraPanX = 0.0f;
        cameraPanY = 0.0f;
        previewZoom = 2.0f;
    }
}

bool CharacterEditor::loadCompare(int index) {
    compareIndex = index;
    compareDoc = YAML::Node();
    comparePath.clear();
    if (index < 0 || index >= static_cast<int>(files.size())) return false;
    try {
        compareDoc = YAML::LoadFile(files[static_cast<size_t>(index)].path.string());
        comparePath = files[static_cast<size_t>(index)].path;
        compareOffsets = readVisualOffsets(compareDoc);
        return true;
    } catch (const std::exception& error) {
        compareDoc = YAML::Node();
        status = std::string(_("Could not load YAML: ")) + error.what();
        return false;
    }
}

void CharacterEditor::drawOpponentPicker(bool allowMirror) {
    std::string current = _("(none)");
    if (compareDoc.IsDefined()) {
        current = yamlString(compareDoc, "name", comparePath.stem().string()) + " (" +
                  OpenArtOfWar::Utility::factionName(docFaction(compareDoc)) + ")";
    } else if (allowMirror) {
        current = _("Mirror match (same as A)");
    }
    ImGui::SetNextItemWidth(280.0f);
    if (hinted(ImGui::BeginCombo(_("Opponent B"), current.c_str()), "Opponent B")) {
        if (allowMirror && ImGui::Selectable(_("Mirror match (same as A)"), !compareDoc.IsDefined())) {
            loadCompare(-1);
        }
        for (size_t i = 0; i < files.size(); ++i) {
            const auto& f = files[i];
            std::string label = (f.type == "building" ? "[B#" : "[#") + std::to_string(f.numericId) + "] " +
                                f.name + " (" + f.faction + ")";
            if (ImGui::Selectable(label.c_str(), compareIndex == static_cast<int>(i))) {
                loadCompare(static_cast<int>(i));
            }
        }
        ImGui::EndCombo();
    }
}

void CharacterEditor::drawCompareControls() {
    drawOpponentPicker(false);
    ImGui::SameLine();
    ImGui::BeginDisabled(!compareDoc.IsDefined() || dirty);
    if (hinted(ImGui::Button(_("Swap A <-> B")), "Swap A <-> B")) {
        const fs::path previousA = activePath;
        const fs::path previousB = comparePath;
        if (loadFile(previousB)) {
            for (size_t i = 0; i < files.size(); ++i) {
                if (files[i].path == previousA) loadCompare(static_cast<int>(i));
            }
        }
    }
    ImGui::EndDisabled();
    if (dirty && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", _("Save A first: swapping reloads both files"));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    hinted(ImGui::SliderFloat("##spacing", &compareSpacing, 40.0f, 200.0f, _("Spacing %.0f px")), "##spacing");
}

void CharacterEditor::drawCompareTable() {
    if (!compareDoc.IsDefined()) {
        ImGui::TextDisabled("%s", _("Pick unit B to compare."));
        return;
    }
    if (!hinted(ImGui::CollapsingHeader(_("A vs B"), ImGuiTreeNodeFlags_DefaultOpen), "A vs B")) return;

    const CharacterStats a = computeStats(document, testSiegeMode);
    const CharacterStats b = computeStats(compareDoc, testSiegeMode);

    // better: +1 higher is better, -1 lower is better, 0 informational
    struct Row {
        const char* label;
        float va;
        float vb;
        int better;
        const char* fmt;
    };
    auto ttk = [](float hp, float dps) { return dps > 0.01f ? hp / dps : -1.0f; };
    auto per100 = [](float v, float cost) { return cost > 0.0f ? v * 100.0f / cost : 0.0f; };
    std::vector<Row> rows = {
        {_("Max HP"), a.hp, b.hp, 1, "%.0f"},
        {_("Damage per shot"), a.damage, b.damage, 1, "%.0f"},
        {_("Shots per burst"), static_cast<float>(a.burst), static_cast<float>(b.burst), 0, "%.0f"},
        {_("Cooldown, s"), a.cooldown, b.cooldown, -1, "%.2f"},
        {_("DPS"), a.dps, b.dps, 1, "%.1f"},
        {_("Attack range, tiles"), a.range, b.range, 1, "%.1f"},
        {_("Sight radius, tiles"), a.sight, b.sight, 1, "%.0f"},
        {_("Move speed"), a.speed, b.speed, 1, "%.2f"},
        {_("Cost"), a.cost, b.cost, -1, "%.0f"},
        {_("Build time, s"), a.buildTime, b.buildTime, -1, "%.1f"},
        {_("HP per 100 credits"), per100(a.hp, a.cost), per100(b.hp, b.cost), 1, "%.0f"},
        {_("DPS per 100 credits"), per100(a.dps, a.cost), per100(b.dps, b.cost), 1, "%.1f"},
        {_("Time to kill the other, s"), ttk(b.hp, a.dps), ttk(a.hp, b.dps), -1, "%.1f"},
    };
    if (a.minRange > 0.0f || b.minRange > 0.0f) {
        rows.insert(rows.begin() + 6, Row{_("Min range, tiles"), a.minRange, b.minRange, -1, "%.1f"});
    }
    if (a.building || b.building) {
        rows.push_back({_("Power, kW"), a.power, b.power, 0, "%+.0f"});
        rows.push_back({_("Footprint, tiles"), static_cast<float>(a.width * a.height),
                        static_cast<float>(b.width * b.height), 0, "%.0f"});
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("compare-table", 4, flags)) {
        ImGui::TableSetupColumn(_("Parameter"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn((std::string("A: ") + a.name).c_str(), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn((std::string("B: ") + b.name).c_str(), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(_("Difference"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        const ImVec4 good(0.45f, 0.9f, 0.5f, 1.0f);
        const ImVec4 bad(0.95f, 0.45f, 0.4f, 1.0f);
        const ImVec4 plain(0.85f, 0.88f, 0.9f, 1.0f);
        for (const Row& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.label);

            const bool na = r.va < 0.0f || r.vb < 0.0f; // e.g. no damage -> no time to kill
            ImVec4 ca = plain, cb = plain;
            if (r.better != 0 && !na && std::abs(r.va - r.vb) > 0.001f) {
                const bool aBetter = (r.better > 0) ? (r.va > r.vb) : (r.va < r.vb);
                ca = aBetter ? good : bad;
                cb = aBetter ? bad : good;
            }
            char buf[64];
            ImGui::TableNextColumn();
            if (r.va < 0.0f) ImGui::TextDisabled("-");
            else { std::snprintf(buf, sizeof(buf), r.fmt, r.va); ImGui::TextColored(ca, "%s", buf); }
            ImGui::TableNextColumn();
            if (r.vb < 0.0f) ImGui::TextDisabled("-");
            else { std::snprintf(buf, sizeof(buf), r.fmt, r.vb); ImGui::TextColored(cb, "%s", buf); }
            ImGui::TableNextColumn();
            if (na) {
                ImGui::TextDisabled("-");
            } else if (std::abs(r.vb) > 0.001f) {
                ImGui::TextDisabled("%+.0f%%", (r.va - r.vb) / std::abs(r.vb) * 100.0f);
            } else {
                ImGui::TextDisabled("%+.2f", r.va - r.vb);
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", _("A uses the current (unsaved) values; colors: green = better, red = worse."));
}

void CharacterEditor::frameBattleCamera() {
    const SDL_FRect area = battle.battleAreaPx();
    cameraPanX = area.x + area.w * 0.5f;
    cameraPanY = area.y + area.h * 0.5f;
    const float fit = std::min(static_cast<float>(previewRect.w) / area.w,
                               static_cast<float>(previewRect.h) / area.h);
    previewZoom = std::clamp(fit, 0.75f, 3.0f);
}

void CharacterEditor::startBattle() {
    std::string error;
    YAML::Node opponent = compareDoc.IsDefined() ? compareDoc : document;
    if (battle.start(battleSetup, assetsRoot, textures.get(), document, opponent, error)) {
        battlePaused = false;
        frameBattleCamera();
        status = _("Battle started");
    } else {
        status = error;
    }
}

void CharacterEditor::runBattleBatch() {
    YAML::Node opponent = compareDoc.IsDefined() ? compareDoc : document;
    BattleBatchResult r;
    for (int i = 0; i < battleBatchRuns; ++i) {
        BattleSim sim;
        std::string error;
        if (!sim.start(battleSetup, assetsRoot, textures.get(), document, opponent, error)) {
            status = error;
            return;
        }
        sim.runToEnd();
        ++r.runs;
        if (sim.winner() == 1) ++r.winsA;
        else if (sim.winner() == 2) ++r.winsB;
        else ++r.draws;
        r.avgTime += sim.time();
        r.avgAliveA += static_cast<float>(sim.sideA().alive);
        r.avgAliveB += static_cast<float>(sim.sideB().alive);
    }
    if (r.runs > 0) {
        r.avgTime /= static_cast<float>(r.runs);
        r.avgAliveA /= static_cast<float>(r.runs);
        r.avgAliveB /= static_cast<float>(r.runs);
    }
    batchResult = r;
}

void CharacterEditor::drawSimulationControls() {
    if (!document.IsDefined()) {
        ImGui::TextWrapped("%s", _("Select a YAML configuration from the list."));
        return;
    }
    ImGui::Text("A: %s (%s)", yamlString(document, "name", "?").c_str(), _("current values, unsaved edits included"));
    ImGui::TextDisabled("%s", _("A fights in red team colors on the left, B in blue on the right."));
    drawOpponentPicker(true);

    ImGui::SetNextItemWidth(130.0f);
    hinted(ImGui::SliderInt("##countA", &battleSetup.countA, 1, 20, _("A units: %d")), "##countA");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    hinted(ImGui::SliderInt("##countB", &battleSetup.countB, 1, 20, _("B units: %d")), "##countB");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    hinted(ImGui::SliderInt("##distance", &battleSetup.distance, 2, 40, _("Distance: %d tiles")), "##distance");
    ImGui::SameLine();
    hinted(ImGui::Checkbox(_("B advances"), &battleSetup.bAdvances), "B advances");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    hinted(ImGui::SliderFloat("##timeLimit", &battleSetup.timeLimit, 30.0f, 600.0f, _("Time limit %.0f s")), "##timeLimit");

    if (hinted(ImGui::Button(battle.active() ? _("Restart") : _("Start battle")), battle.active() ? "Restart" : "Start battle")) startBattle();
    ImGui::SameLine();
    ImGui::BeginDisabled(!battle.active());
    if (hinted(ImGui::Button(battlePaused ? _("Resume") : _("Pause")), battlePaused ? "Resume" : "Pause")) battlePaused = !battlePaused;
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Step 0.1 s")), "Step 0.1 s")) {
        battlePaused = true;
        battle.update(0.1f);
    }
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Stop")), "Stop")) battle.stop();
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Fit view")), "Fit view")) frameBattleCamera();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    hinted(ImGui::SliderFloat("##simSpeed", &playbackSpeed, 0.1f, 4.0f, _("Speed %.1fx")), "##simSpeed");

    if (battle.active()) {
        const auto& a = battle.sideA();
        const auto& b = battle.sideB();
        if (ImGui::BeginTable("battle-table", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn(_("Side"));
            ImGui::TableSetupColumn(_("Alive"));
            ImGui::TableSetupColumn(_("HP left"));
            ImGui::TableSetupColumn(_("Damage dealt"));
            ImGui::TableSetupColumn(_("DPS"));
            ImGui::TableHeadersRow();
            auto row = [&](const char* label, const BattleSim::Side& s, ImVec4 color) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(color, "%s", label);
                ImGui::TableNextColumn();
                ImGui::Text("%d / %d", s.alive, s.total);
                ImGui::TableNextColumn();
                ImGui::Text("%d / %d", s.hpNow, s.hpStart);
                ImGui::TableNextColumn();
                ImGui::Text("%d", s.damageDealt);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", battle.time() > 0.0f ? static_cast<float>(s.damageDealt) / battle.time() : 0.0f);
            };
            const std::string labelA = "A: " + yamlString(document, "name", "?");
            const std::string labelB = "B: " + yamlString(compareDoc.IsDefined() ? compareDoc : document, "name", "?");
            row(labelA.c_str(), a, ImVec4(1.0f, 0.55f, 0.5f, 1.0f));
            row(labelB.c_str(), b, ImVec4(0.5f, 0.75f, 1.0f, 1.0f));
            ImGui::EndTable();
        }
        if (battle.finished()) {
            const char* result = battle.winner() == 1 ? _("A wins") : battle.winner() == 2 ? _("B wins") : _("Draw");
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "%s  (%.1f s)", result, battle.time());
        } else {
            ImGui::Text(_("Battle time: %.1f s"), battle.time());
        }
    }

    ImGui::SetNextItemWidth(150.0f);
    hinted(ImGui::SliderInt("##batchRuns", &battleBatchRuns, 5, 100, _("Battles: %d")), "##batchRuns");
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Run batch (no rendering)")), "Run batch (no rendering)")) runBattleBatch();
    if (batchResult.runs > 0) {
        const float n = static_cast<float>(batchResult.runs);
        ImGui::Text(_("A wins %d (%.0f%%) | B wins %d (%.0f%%) | draws %d | avg %.1f s | survivors A %.1f, B %.1f"),
                    batchResult.winsA, batchResult.winsA * 100.0f / n, batchResult.winsB, batchResult.winsB * 100.0f / n,
                    batchResult.draws, batchResult.avgTime, batchResult.avgAliveA, batchResult.avgAliveB);
    }
}

} // namespace CharEditor
