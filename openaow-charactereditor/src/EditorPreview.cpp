#include "CharacterEditor.hpp"

#include "graphics/RenderUtility.hpp"

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

CharacterEditor::MoveSample CharacterEditor::sampleMove(int archetype, float moveSpeed, float time) const {
    const float leg = 60.0f;                                    // pixels per drive
    const float speed = std::max(1.0f, moveSpeed * 60.0f);      // the game moves moveSpeed px per 1/60 s
    const float driveTime = leg / speed;
    const float turnDelay = yamlFloat(document, "turn_step_delay", OpenArtOfWar::Utility::defaultTurnStepDelay(archetype));
    auto turnSteps = [](int from, int to) {
        std::vector<int> steps;
        for (int f = from, guard = 0; f != to && guard < 8; ++guard) {
            f = (f + OpenArtOfWar::Utility::turnStep8(f, to) + 8) % 8;
            steps.push_back(f);
        }
        return steps;
    };
    const std::vector<int> turnLeft = turnSteps(0, 4), turnRight = turnSteps(4, 0);
    const float turnTimeL = turnDelay * static_cast<float>(turnLeft.size());
    const float turnTimeR = turnDelay * static_cast<float>(turnRight.size());
    const float cycle = 2.0f * driveTime + turnTimeL + turnTimeR;
    float t = std::fmod(time, std::max(0.01f, cycle));
    MoveSample m;
    if (t < driveTime) {                                        // driving right
        m.offsetX = -leg * 0.5f + speed * t;
        m.facing = 0;
        return m;
    }
    t -= driveTime;
    if (t < turnTimeL) {                                        // turning around at the right end
        m.offsetX = leg * 0.5f;
        m.facing = turnLeft[std::min(turnLeft.size() - 1, static_cast<size_t>(t / turnDelay))];
        return m;
    }
    t -= turnTimeL;
    if (t < driveTime) {                                        // driving left
        m.offsetX = leg * 0.5f - speed * t;
        m.facing = 4;
        return m;
    }
    t -= driveTime;
    m.offsetX = -leg * 0.5f;                                    // turning back at the left end
    m.facing = turnRight.empty() ? 0 : turnRight[std::min(turnRight.size() - 1, static_cast<size_t>(t / std::max(0.001f, turnDelay)))];
    return m;
}

void CharacterEditor::turretShift(int archetype, int hullFacing, float& dx, float& dy) const {
    dx = dy = 0.0f;
    if (!textures) return;
    const bool fixedHull = (archetype >= 14 && archetype <= 16);
    float ax = 0.0f, ay = 0.0f, rx = 0.0f, ry = 0.0f;
    if (textures->getTurretAnchor(archetype, fixedHull ? 0 : hullFacing, ax, ay) &&
        textures->getTurretAnchor(archetype, fixedHull ? 0 : 2, rx, ry)) {
        dx = ax - rx;
        dy = ay - ry;
    }
}

int CharacterEditor::currentWreckStage(int stages) const {
    if (wreckPreviewStage >= 0) return std::min(wreckPreviewStage, stages - 1);
    return static_cast<int>(previewTime / 0.7f) % std::max(1, stages);
}

SDL_Texture* CharacterEditor::compositeThumbnail(int composite, bool centred) {
    const int key = centred ? composite : -composite;
    auto it = wreckThumbs.find(key);
    if (it != wreckThumbs.end()) return it->second;
    const int kW = centred ? 96 : 64, kH = centred ? 64 : 42;
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, kW, kH);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_Texture* previous = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, tex);
        SDL_SetRenderClipRect(renderer, nullptr);
        SDL_SetRenderDrawColor(renderer, 28, 34, 30, 255);
        SDL_RenderClear(renderer);
        if (centred) {
            // Composites are 180x120 canvases centred on the object
            const float scale = 0.55f;
            gameRenderer->drawCompositeScreen(composite, static_cast<int>(kW * 0.5f - 90.0f * scale),
                                              static_cast<int>(kH * 0.5f - 60.0f * scale), scale);
        } else {
            gameRenderer->drawCompositeScreen(composite, 2, 2, 1.0f, OpenArtOfWar::Faction::Resistance);
        }
        SDL_SetRenderTarget(renderer, previous);
    }
    wreckThumbs[key] = tex;
    return tex;
}

void CharacterEditor::drawRadiusCircle2px(float centerX, float centerY, float radiusPixels, SDL_Color color) {
    if (radiusPixels <= 1.0f) return;
    OpenArtOfWar::RenderUtility::DrawRadius(*gameRenderer, *camera, centerX, centerY, radiusPixels, color, 2.0f);
}

void CharacterEditor::drawPreviewControls() {
    const bool isBuilding = document.IsDefined() && (yamlString(document, "type") == "building");

    // The tabs wrap into a new line when the panel is narrow
    struct ModeTab {
        EditorMode mode;
        const char* label;
    };
    static const ModeTab kTabs[] = {
        {EditorMode::Preview, "Preview"},         {EditorMode::Compare, "Compare"},
        {EditorMode::Simulation, "Simulation"},   {EditorMode::Research, "Research"},
        {EditorMode::Explosions, "Explosions"},   {EditorMode::Wrecks, "Wrecks"},
        {EditorMode::GameUi, "Game UI"},          {EditorMode::Environment, "Environment"},
        {EditorMode::AiDifficulty, "AI Difficulty"}, {EditorMode::SpriteTools, "Sprite Tools"},
    };
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - style.WindowPadding.x;
    bool first = true;
    for (const ModeTab& tab : kTabs) {
        const float w = ImGui::CalcTextSize(_(tab.label)).x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;
        if (!first && ImGui::GetItemRectMax().x + style.ItemSpacing.x + w <= rightEdge) ImGui::SameLine();
        first = false;
        if (hinted(ImGui::RadioButton(_(tab.label), editorMode == tab.mode), tab.label)) setEditorMode(tab.mode);
    }
    ImGui::Separator();
    if (editorMode == EditorMode::Research || editorMode == EditorMode::SpriteTools || editorMode == EditorMode::GameUi ||
        editorMode == EditorMode::Environment || editorMode == EditorMode::AiDifficulty) return;
    if (editorMode == EditorMode::Preview || editorMode == EditorMode::Compare || editorMode == EditorMode::Wrecks) {
        drawArmyColorCombo();
    }
    if (editorMode == EditorMode::Explosions) {
        drawExplosionControls();
        return;
    }
    if (editorMode == EditorMode::Wrecks) {
        drawWreckControls();
        return;
    }
    if (editorMode == EditorMode::Simulation) {
        drawSimulationControls();
        return;
    }
    if (editorMode == EditorMode::Compare) {
        drawCompareControls();
    }

    if (hinted(ImGui::Button(_("Idle")), "Idle")) setMode(PreviewMode::Idle);
    if (!isBuilding) {
        ImGui::SameLine();
        if (hinted(ImGui::Button(_("Move")), "Move")) setMode(PreviewMode::Move);
    } else if (previewMode == PreviewMode::Move) {
        setMode(PreviewMode::Idle);
    }
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Attack")), "Attack")) setMode(PreviewMode::Attack);
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Death")), "Death")) setMode(PreviewMode::Die);
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Wreck")), "Wreck")) setMode(PreviewMode::Wreck);
    if (previewMode == PreviewMode::Die && !isBuilding) {
        ImGui::SameLine();
        hinted(ImGui::Checkbox(_("Flame Death"), &testFlameDeath), "Flame Death");
    }
    if (!isBuilding) {
        ImGui::SameLine();
        if (testSiegeMode) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.55f, 0.2f, 1.0f));
            if (hinted(ImGui::Button(_("Undeploy [G]")), "Undeploy [G]")) {
                testSiegeMode = false;
            }
            ImGui::PopStyleColor(2);
        } else {
            if (hinted(ImGui::Button(_("Deploy Siege [G]")), "Deploy Siege [G]")) {
                testSiegeMode = true;
            }
        }
    }
    ImGui::SameLine();
    hinted(ImGui::Checkbox(_("Loop"), &loopPreview), "Loop");
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Center View")), "Center View")) {
        cameraPanX = 0.0f;
        cameraPanY = 0.0f;
        previewZoom = 2.0f;
    }
    ImGui::Separator();
    YAML::Node cNode = document["combat"];
    const bool hasRotTurret = yamlBool(cNode, "has_rotating_turret", false);

    ImGui::SetNextItemWidth(105.0f);
    if (hinted(ImGui::SliderInt("##facing", &facing, 0, 7, hasRotTurret ? _("Hull %d/7") : _("Facing %d/7")), "##facing")) {
        if (!hasRotTurret || linkTurretToHull) {
            turretFacingDir = facing;
            float ang = static_cast<float>(facing) * (static_cast<float>(M_PI) / 4.0f);
            float dist = std::max(60.0f, std::hypot(targetWorldX, targetWorldY));
            targetWorldX = std::cos(ang) * dist;
            targetWorldY = std::sin(ang) * dist;
        }
    }
    if (hasRotTurret) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(115.0f);
        if (hinted(ImGui::SliderInt("##turretFacing", &turretFacingDir, 0, 7, _("Turret %d/7")), "##turretFacing")) {
            float ang = static_cast<float>(turretFacingDir) * (static_cast<float>(M_PI) / 4.0f);
            float dist = std::max(60.0f, std::hypot(targetWorldX, targetWorldY));
            targetWorldX = std::cos(ang) * dist;
            targetWorldY = std::sin(ang) * dist;
            targetCustomized = true;
        }
        ImGui::SameLine();
        hinted(ImGui::Checkbox(_("Link"), &linkTurretToHull), "Link");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    hinted(ImGui::SliderFloat("##speed", &playbackSpeed, 0.1f, 2.0f, _("Speed %.1fx")), "##speed");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(125.0f);
    if (hinted(ImGui::SliderFloat("##bulletSpeed", &previewBulletSpeed, 100.0f, 2000.0f, _("Bullet: %.0f px/s")), "##bulletSpeed")) {
        YAML::Node combat = section("combat");
        combat["bullet_speed"] = previewBulletSpeed;
        dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    if (hinted(ImGui::SliderFloat("##fxSpeed", &previewFxSpeed, 0.2f, 3.0f, _("FX: %.1fx")), "##fxSpeed")) {
        YAML::Node combat = section("combat");
        combat["fx_speed"] = previewFxSpeed;
        dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    hinted(ImGui::SliderFloat(_("Zoom"), &previewZoom, 0.5f, 4.0f, "%.2fx"), "Zoom");
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Reset Aim")), "Reset Aim")) {
        float ang = static_cast<float>(facing) * (static_cast<float>(M_PI) / 4.0f);
        targetWorldX = std::cos(ang) * 120.0f;
        targetWorldY = std::sin(ang) * 120.0f;
        targetCustomized = false;
        turretFacingDir = facing;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Aim: (%.0f, %.0f)", targetWorldX, targetWorldY);
    if (hasRotTurret) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.2f, 1.0f), "Muzzle: (%.1f, %.1f) Dome: (%.1f, %.1f)", muzzleOffsetX, muzzleOffsetY, turretOffsetX, turretOffsetY);
    } else {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.2f, 1.0f), "Muzzle: (%.1f, %.1f)", muzzleOffsetX, muzzleOffsetY);
    }

    hinted(ImGui::Checkbox(_("Attack range (Red)"), &showAttackRange), "Attack range (Red)");
    ImGui::SameLine();
    hinted(ImGui::Checkbox(_("Sight range (Green)"), &showSightRange), "Sight range (Green)");
    ImGui::SameLine();
    hinted(ImGui::Checkbox(_("Show ground (5x5)"), &showGround), "Show ground (5x5)");
    ImGui::SameLine();
    hinted(ImGui::Checkbox(_("Combat HUD"), &showCombatHud), "Combat HUD");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (hinted(ImGui::SliderInt(_("Health"), &previewHealthPercent, 0, 100, "%d%%"), "Health") &&
        previewHealthPercent == 0) {
        previewTime = 0.0f;
    }
    if (document.IsDefined() && yamlString(document, "type") == "building") {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        hinted(ImGui::SliderFloat(_("Construction"), &buildingProgress, 0.0f, 1.0f, "%.0f%%"), "Construction");
        ImGui::SameLine();
        hinted(ImGui::Checkbox(_("Powered"), &buildingPowered), "Powered");
    } else if (document.IsDefined() && (yamlBool(document["abilities"], "siege_mode", false) ||
                                       (document["abilities"] && document["abilities"]["siege"].IsMap()))) {
        ImGui::SameLine();
        hinted(ImGui::Checkbox(_("Test Siege Mode"), &testSiegeMode), "Test Siege Mode");
    }
    if (editorMode == EditorMode::Compare) {
        drawCompareTable();
    }
}

void CharacterEditor::drawCanvasGuides(const ImVec2& min, const ImVec2& size) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    drawList->AddRectFilled(min, max, IM_COL32(24, 31, 31, 255));

    if (editorMode == EditorMode::Simulation) {
        drawList->AddText(ImVec2(min.x + 10.0f, min.y + size.y - 20.0f), IM_COL32(173, 190, 182, 220),
                          _("MMB Drag: Pan | Wheel: Zoom | MMB 2x: Fit arena"));
        return;
    }
    const bool comparing = editorMode == EditorMode::Compare && compareDoc.IsDefined();

    // Screen position of world origin (0, 0)
    float originScreenX = 0.0f;
    float originScreenY = 0.0f;
    camera->worldToScreen(0.0f, 0.0f, originScreenX, originScreenY);

    if (originScreenX >= min.x && originScreenX <= max.x) {
        drawList->AddLine(ImVec2(originScreenX, min.y),
                          ImVec2(originScreenX, max.y), IM_COL32(109, 151, 130, 80));
    }
    if (originScreenY >= min.y && originScreenY <= max.y) {
        drawList->AddLine(ImVec2(min.x, originScreenY),
                          ImVec2(max.x, originScreenY), IM_COL32(109, 151, 130, 80));
    }

    // Prominent status banners on canvas
    if (document.IsDefined() && testSiegeMode) {
        drawList->AddRectFilled(ImVec2(min.x + 10.0f, min.y + 10.0f),
                                ImVec2(min.x + 320.0f, min.y + 36.0f),
                                IM_COL32(35, 25, 10, 230), 4.0f);
        drawList->AddRect(ImVec2(min.x + 10.0f, min.y + 10.0f),
                          ImVec2(min.x + 320.0f, min.y + 36.0f),
                          IM_COL32(245, 165, 30, 255), 4.0f, 0, 1.5f);
        drawList->AddText(ImVec2(min.x + 18.0f, min.y + 14.0f),
                          IM_COL32(255, 215, 60, 255),
                          _("★ SIEGE MODE ACTIVE (Press G to toggle)"));
    }
    if (document.IsDefined() && previewMode == PreviewMode::Attack && activeFiringWeaponIndex == 1) {
        float bY = (testSiegeMode ? 42.0f : 10.0f);
        drawList->AddRectFilled(ImVec2(min.x + 10.0f, min.y + bY),
                                ImVec2(min.x + 300.0f, min.y + bY + 26.0f),
                                IM_COL32(45, 20, 10, 230), 4.0f);
        drawList->AddRect(ImVec2(min.x + 10.0f, min.y + bY),
                          ImVec2(min.x + 300.0f, min.y + bY + 26.0f),
                          IM_COL32(255, 100, 30, 255), 4.0f, 0, 1.5f);
        drawList->AddText(ImVec2(min.x + 18.0f, min.y + bY + 4.0f),
                          IM_COL32(255, 150, 50, 255),
                          _("🔥 WEAPON 2: FLAME TOWER ACTIVE"));
    }

    // A / B captions under the two compared characters
    if (comparing) {
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        camera->worldToScreen(-compareSpacing, 58.0f, ax, ay);
        camera->worldToScreen(compareSpacing, 58.0f, bx, by);
        const std::string la = "A: " + yamlString(document, "name", "?");
        const std::string lb = "B: " + yamlString(compareDoc, "name", "?");
        const ImVec2 sa = ImGui::CalcTextSize(la.c_str());
        const ImVec2 sb = ImGui::CalcTextSize(lb.c_str());
        drawList->AddText(ImVec2(ax - sa.x * 0.5f, ay), IM_COL32(255, 170, 150, 255), la.c_str());
        drawList->AddText(ImVec2(bx - sb.x * 0.5f, by), IM_COL32(150, 200, 255, 255), lb.c_str());
    }

    // Targeting reticle for Attack mode
    if (!comparing && document.IsDefined() && previewMode == PreviewMode::Attack) {
        float targetScreenX = 0.0f;
        float targetScreenY = 0.0f;
        camera->worldToScreen(targetWorldX, targetWorldY, targetScreenX, targetScreenY);
        const ImVec2 target(targetScreenX, targetScreenY);

        drawList->AddCircle(target, 16.0f, IM_COL32(236, 111, 77, 230), 24, 2.0f);
        drawList->AddCircle(target, 4.0f, IM_COL32(255, 170, 100, 255), 12, 1.5f);
        drawList->AddLine(ImVec2(target.x - 22.0f, target.y), ImVec2(target.x - 6.0f, target.y),
                          IM_COL32(236, 111, 77, 200), 2.0f);
        drawList->AddLine(ImVec2(target.x + 6.0f, target.y), ImVec2(target.x + 22.0f, target.y),
                          IM_COL32(236, 111, 77, 200), 2.0f);
        drawList->AddLine(ImVec2(target.x, target.y - 22.0f), ImVec2(target.x, target.y - 6.0f),
                          IM_COL32(236, 111, 77, 200), 2.0f);
        drawList->AddLine(ImVec2(target.x, target.y + 6.0f), ImVec2(target.x, target.y + 22.0f),
                          IM_COL32(236, 111, 77, 200), 2.0f);

        char label[64];
        std::snprintf(label, sizeof(label), "AIM (%.0f, %.0f)", targetWorldX, targetWorldY);
        drawList->AddText(ImVec2(target.x + 18.0f, target.y - 10.0f), IM_COL32(236, 140, 100, 220), label);
    }

    // Combat info HUD card in top-right of preview canvas (Compare mode shows the A vs B table instead)
    if (showCombatHud && !comparing && document.IsDefined()) {
        YAML::Node combatNode = document["combat"];
        if (testSiegeMode && document["abilities"] && document["abilities"]["siege"] &&
            document["abilities"]["siege"]["combat"].IsMap()) {
            combatNode = document["abilities"]["siege"]["combat"];
        }
        const YAML::Node hudWeapon = mainWeapon(combatNode);
        const int damage = yamlInt(hudWeapon, "damage", 0);
        const float range = yamlFloat(hudWeapon, "range", 0.0f);
        const float minRange = yamlFloat(hudWeapon, "min_range", 0.0f);
        const float cooldown = std::max(0.05f, weaponCooldown(hudWeapon, false));
        const int burstCount = std::max(1, yamlInt(hudWeapon, "burst_count", 1));
        const float burstDelay = std::max(0.01f, yamlFloat(hudWeapon, "burst_delay", 0.1f));
        const int totalBurstDmg = damage * burstCount;
        const float burstSpan = static_cast<float>(burstCount - 1) * burstDelay;
        const float cycleTime = cooldown + burstSpan;
        const float dps = (cycleTime > 0.01f) ? (static_cast<float>(totalBurstDmg) / cycleTime) : 0.0f;
        const std::string proj = yamlString(hudWeapon, "projectile", "bullet");
        const bool canAttack = yamlBool(combatNode, "can_attack", true);

        const float cardWidth = 230.0f;
        const float cardHeight = (burstCount > 1 ? 140.0f : 120.0f);
        const ImVec2 cardMin(min.x + size.x - cardWidth - 12.0f, min.y + 12.0f);
        const ImVec2 cardMax(cardMin.x + cardWidth, cardMin.y + cardHeight);

        drawList->AddRectFilled(cardMin, cardMax, IM_COL32(18, 24, 25, 225), 4.0f);
        drawList->AddRect(cardMin, cardMax, IM_COL32(65, 115, 95, 210), 4.0f);

        float ty = cardMin.y + 8.0f;
        float tx = cardMin.x + 10.0f;
        std::string name = yamlString(document, "name", "Unit");
        drawList->AddText(ImVec2(tx, ty), IM_COL32(230, 245, 238, 255), name.c_str());
        ty += 18.0f;

        if (!canAttack) {
            drawList->AddText(ImVec2(tx, ty), IM_COL32(160, 160, 160, 200), _("Combat: Disarmed"));
        } else {
            char buf[128];
            if (burstCount > 1) {
                std::snprintf(buf, sizeof(buf), _("Damage: %d (%d x %d burst)"), totalBurstDmg, burstCount, damage);
            } else {
                std::snprintf(buf, sizeof(buf), _("Damage: %d per shot"), damage);
            }
            drawList->AddText(ImVec2(tx, ty), IM_COL32(255, 130, 100, 240), buf);
            ty += 16.0f;

            std::snprintf(buf, sizeof(buf), _("DPS: %.1f dmg/s | Cycle: %.2fs"), dps, cycleTime);
            drawList->AddText(ImVec2(tx, ty), IM_COL32(240, 210, 120, 230), buf);
            ty += 16.0f;

            if (minRange > 0.0f) {
                std::snprintf(buf, sizeof(buf), _("Range: %.1f - %.1f tiles"), minRange, range);
            } else {
                std::snprintf(buf, sizeof(buf), _("Range: %.1f tiles (%.0f px)"), range, range * 30.0f);
            }
            drawList->AddText(ImVec2(tx, ty), IM_COL32(110, 220, 140, 230), buf);
            ty += 16.0f;

            if (burstCount > 1) {
                std::snprintf(buf, sizeof(buf), _("Burst: %d shots @ %.2fs interval"), burstCount, burstDelay);
                drawList->AddText(ImVec2(tx, ty), IM_COL32(200, 200, 140, 230), buf);
                ty += 16.0f;
            }

            std::snprintf(buf, sizeof(buf), _("Weapon: %s"), proj.c_str());
            drawList->AddText(ImVec2(tx, ty), IM_COL32(180, 210, 225, 230), buf);
        }
    }

    drawList->AddText(ImVec2(min.x + 10.0f, min.y + size.y - 20.0f), IM_COL32(173, 190, 182, 220),
                      _("LMB: Turn Turret (Башня) | RMB: Turn Unit (Корпус) | MMB Drag: Pan | Wheel: Zoom | MMB 2x: Reset"));
}

int CharacterEditor::previewAction() const {
    switch (previewMode) {
        case PreviewMode::Move: return static_cast<int>(OpenArtOfWar::ActionState::Move);
        case PreviewMode::Attack: return static_cast<int>(OpenArtOfWar::ActionState::Attack);
        case PreviewMode::Die:
        case PreviewMode::Wreck: return static_cast<int>(OpenArtOfWar::ActionState::Die);
        case PreviewMode::Idle: default: return static_cast<int>(OpenArtOfWar::ActionState::Idle);
    }
}

void CharacterEditor::drawPreviewScene() {
    if (previewRect.w < 1 || previewRect.h < 1) return;
    SDL_Rect clip = previewRect;
    SDL_SetRenderClipRect(renderer, &clip);
    gameRenderer->setArmyColor(0, previewArmyColor > 0, OpenArtOfWar::armyColor(previewArmyColor));
    gameRenderer->setArmyColor(1, false, SDL_Color{255, 255, 255, 255});
    gameRenderer->setDrawOwner(0);

    if (editorMode == EditorMode::Explosions || editorMode == EditorMode::Wrecks) {
        drawPreviewGrid();
        drawEffectsScene();
    } else if (editorMode == EditorMode::Simulation) {
        drawPreviewGrid();
        battle.render(*gameRenderer, *camera, static_cast<int>(previewTime * 8.0f));
    } else if (document.IsDefined()) {
        drawPreviewGrid();
        const VisualOffsets primaryOffsets{muzzleOffsetX, muzzleOffsetY, turretOffsetX, turretOffsetY, hasCustomTurretOffset};
        if (editorMode == EditorMode::Compare && compareDoc.IsDefined()) {
            // Everything is drawn in world space, so shifting the camera places A left and B right of the origin
            const float camX = camera->getX();
            const float camY = camera->getY();
            camera->setPosition(camX + compareSpacing, camY);
            drawCharacter(document, primaryOffsets, true);
            camera->setPosition(camX - compareSpacing, camY);
            drawCharacter(compareDoc, compareOffsets, false);
            camera->setPosition(camX, camY);
        } else {
            drawCharacter(document, primaryOffsets, true);
        }
    }

    gameRenderer->endWorldPass();
    SDL_SetRenderClipRect(renderer, nullptr);
}

void CharacterEditor::drawArmyColorCombo() {
    const bool available = shaderEffects && shaderEffects->available();
    ImGui::BeginDisabled(!available);
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::BeginCombo(_("Army color"), _(OpenArtOfWar::armyColorName(previewArmyColor)))) {
        for (int i = 0; i <= 8; ++i) {
            const SDL_Color c = OpenArtOfWar::armyColor(i);
            if (i > 0) {
                ImGui::ColorButton(("##swatch" + std::to_string(i)).c_str(), ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(14, 14));
                ImGui::SameLine();
            }
            if (ImGui::Selectable(_(OpenArtOfWar::armyColorName(i)), i == previewArmyColor)) previewArmyColor = i;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (available) fieldHint("Army color");
    else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", _("Needs the SDL GPU renderer (shader effects)"));
}

} // namespace CharEditor
