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

namespace CharEditor
{

CharacterEditor::MoveSample CharacterEditor::SampleMove(int archetype, float moveSpeed, float time) const
{
    const float leg = 60.0f;                               // pixels per drive
    const float speed = std::max(1.0f, moveSpeed * 60.0f); // the game moves moveSpeed px per 1/60 s
    const float driveTime = leg / speed;
    const float turnDelay
        = YamlFloat(document, "turn_step_delay", OpenArtOfWar::Utility::DefaultTurnStepDelay(archetype));
    auto turnSteps = [](int from, int to) {
        std::vector<int> steps;
        for (int f = from, guard = 0; f != to && guard < 8; ++guard)
        {
            f = (f + OpenArtOfWar::Utility::TurnStep8(f, to) + 8) % 8;
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
    if (t < driveTime)
    { // driving right
        m.offsetX = -leg * 0.5f + speed * t;
        m.facing = 0;
        return m;
    }
    t -= driveTime;
    if (t < turnTimeL)
    { // turning around at the right end
        m.offsetX = leg * 0.5f;
        m.facing = turnLeft[std::min(turnLeft.size() - 1, static_cast<std::size_t>(t / turnDelay))];
        return m;
    }
    t -= turnTimeL;
    if (t < driveTime)
    { // driving left
        m.offsetX = leg * 0.5f - speed * t;
        m.facing = 4;
        return m;
    }
    t -= driveTime;
    m.offsetX = -leg * 0.5f; // turning back at the left end
    m.facing
        = turnRight.empty()
              ? 0
              : turnRight[std::min(turnRight.size() - 1, static_cast<std::size_t>(t / std::max(0.001f, turnDelay)))];
    return m;
}

void CharacterEditor::TurretShift(int archetype, int hullFacing, float &dx, float &dy) const
{
    dx = dy = 0.0f;
    if (!textures)
        return;
    const bool fixedHull = (archetype >= 14 && archetype <= 16);
    float ax = 0.0f, ay = 0.0f, rx = 0.0f, ry = 0.0f;
    if (textures->GetTurretAnchor(archetype, fixedHull ? 0 : hullFacing, ax, ay)
        && textures->GetTurretAnchor(archetype, fixedHull ? 0 : 2, rx, ry))
    {
        dx = ax - rx;
        dy = ay - ry;
    }
}

int CharacterEditor::CurrentWreckStage(int stages) const
{
    if (wreckPreviewStage >= 0)
        return std::min(wreckPreviewStage, stages - 1);
    return static_cast<int>(previewTime / 0.7f) % std::max(1, stages);
}

SDL_Texture *CharacterEditor::CompositeThumbnail(int composite, bool centred)
{
    const int key = centred ? composite : -composite;
    auto it = wreckThumbs.find(key);
    if (it != wreckThumbs.end())
        return it->second;
    const int kW = centred ? 96 : 64, kH = centred ? 64 : 42;
    SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, kW, kH);
    if (tex)
    {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_Texture *previous = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, tex);
        SDL_SetRenderClipRect(renderer, nullptr);
        SDL_SetRenderDrawColor(renderer, 28, 34, 30, 255);
        SDL_RenderClear(renderer);
        if (centred)
        {
            // Composites are 180x120 canvases centred on the object
            const float scale = 0.55f;
            gameRenderer->DrawCompositeScreen(
                composite,
                static_cast<int>(kW * 0.5f - 90.0f * scale),
                static_cast<int>(kH * 0.5f - 60.0f * scale),
                scale);
        }
        else
        {
            gameRenderer->DrawCompositeScreen(composite, 2, 2, 1.0f, OpenArtOfWar::Faction::Resistance);
        }
        SDL_SetRenderTarget(renderer, previous);
    }
    wreckThumbs[key] = tex;
    return tex;
}

void CharacterEditor::DrawTileRadius(float centerX, float centerY, float rangeTiles, SDL_Color color, float thickness)
{
    if (rangeTiles <= 0.0f)
        return;
    OpenArtOfWar::RenderUtility::DrawTileRadius(*gameRenderer, *camera, centerX, centerY, rangeTiles, color, thickness);
}

void CharacterEditor::DrawRadiusCircle2px(float centerX, float centerY, float radiusPixels, SDL_Color color)
{
    if (radiusPixels <= 1.0f)
        return;
    const float tiles = radiusPixels / static_cast<float>(OpenArtOfWar::TILE_WIDTH);
    DrawTileRadius(centerX, centerY, tiles, color, 2.0f);
}

void CharacterEditor::DrawPreviewControls()
{
    const bool isBuilding = document.IsDefined() && (YamlString(document, "type") == "building");

    // The tabs wrap into a new line when the panel is narrow
    struct ModeTab
    {
        EditorMode mode;
        const char *label;
    };
    static const ModeTab kTabs[] = {
        {EditorMode::Preview, "Preview"},
        {EditorMode::Compare, "Compare"},
        {EditorMode::Simulation, "Simulation"},
        {EditorMode::Research, "Research"},
        {EditorMode::Explosions, "Explosions"},
        {EditorMode::Wrecks, "Wrecks"},
        {EditorMode::Projectiles, "Projectiles"},
        {EditorMode::Environment, "Environment"},
        {EditorMode::AiDifficulty, "AI Difficulty"},
        {EditorMode::Animator, "Animator"},
        {EditorMode::SpriteTools, "Sprite Tools"},
        {EditorMode::SpriteMerge, "Sprite Merge"},
    };
    const ImGuiStyle &style = ImGui::GetStyle();
    const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - style.WindowPadding.x;
    bool first = true;
    for (const ModeTab &tab : kTabs)
    {
        const float w = ImGui::CalcTextSize(_(tab.label)).x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;
        if (!first && ImGui::GetItemRectMax().x + style.ItemSpacing.x + w <= rightEdge)
            ImGui::SameLine();
        first = false;
        if (Hinted(ImGui::RadioButton(_(tab.label), editorMode == tab.mode), tab.label))
            SetEditorMode(tab.mode);
    }
    ImGui::Separator();
    if (editorMode == EditorMode::Research || editorMode == EditorMode::SpriteTools
        || editorMode == EditorMode::SpriteMerge
        || editorMode == EditorMode::Environment || editorMode == EditorMode::AiDifficulty)
        return;
    // Background of the canvas (black by default)
    Hinted(ImGui::ColorEdit3(_("Background"), previewBackground, ImGuiColorEditFlags_NoInputs), "Canvas background");
    ImGui::SameLine();
    {
        // A layer over the colour, for when a sprite blends into it (only the preview)
        const char *backdrops[] = {_("No layer"), _("Checkerboard"), _("Grid"), _("Gradient"), _("Stripes")};
        ImGui::SetNextItemWidth(120.0f);
        Hinted(ImGui::Combo("##backdrop", &previewBackdrop, backdrops, IM_ARRAYSIZE(backdrops)), "Background layer");
    }
    if (editorMode == EditorMode::Preview || editorMode == EditorMode::Compare || editorMode == EditorMode::Wrecks
        || editorMode == EditorMode::Animator)
    {
        ImGui::SameLine();
        DrawArmyColorCombo();
    }
    if (editorMode == EditorMode::Explosions)
    {
        DrawExplosionControls();
        return;
    }
    if (editorMode == EditorMode::Wrecks)
    {
        DrawWreckControls();
        return;
    }
    if (editorMode == EditorMode::Projectiles)
    {
        DrawProjectileControls();
        return;
    }
    if (editorMode == EditorMode::Simulation)
    {
        DrawSimulationControls();
        return;
    }
    if (editorMode == EditorMode::Compare)
    {
        DrawCompareControls();
    }

    if (Hinted(ImGui::Button(_("Idle")), "Idle"))
        SetMode(PreviewMode::Idle);
    if (!isBuilding)
    {
        ImGui::SameLine();
        if (Hinted(ImGui::Button(_("Move")), "Move"))
            SetMode(PreviewMode::Move);
    }
    else if (previewMode == PreviewMode::Move)
    {
        SetMode(PreviewMode::Idle);
    }
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Attack")), "Attack"))
        SetMode(PreviewMode::Attack);
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Death")), "Death"))
        SetMode(PreviewMode::Die);
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Wreck")), "Wreck"))
        SetMode(PreviewMode::Wreck);
    if (previewMode == PreviewMode::Die && !isBuilding)
    {
        ImGui::SameLine();
        Hinted(ImGui::Checkbox(_("Flame Death"), &testFlameDeath), "Flame Death");
    }
    if (!isBuilding)
    {
        ImGui::SameLine();
        if (testSiegeMode)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.55f, 0.2f, 1.0f));
            if (Hinted(ImGui::Button(_("Undeploy [G]")), "Undeploy [G]"))
            {
                testSiegeMode = false;
            }
            ImGui::PopStyleColor(2);
        }
        else
        {
            if (Hinted(ImGui::Button(_("Deploy Siege [G]")), "Deploy Siege [G]"))
            {
                testSiegeMode = true;
            }
        }
    }
    ImGui::SameLine();
    Hinted(ImGui::Checkbox(_("Loop"), &loopPreview), "Loop");
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Center View")), "Center View"))
    {
        cameraPanX = 0.0f;
        cameraPanY = 0.0f;
        previewZoom = 2.0f;
    }
    ImGui::Separator();
    YAML::Node cNode = document["combat"];
    const bool hasRotTurret = YamlBool(cNode, "has_rotating_turret", false);

    ImGui::SetNextItemWidth(105.0f);
    if (Hinted(
            ImGui::SliderInt("##facing", &facing, 0, 7, hasRotTurret ? _("Hull %d/7") : _("Facing %d/7")), "##facing"))
    {
        desiredHullFacingDir = facing;
        previewHullTurnTimer = 0.0f;
        if (!hasRotTurret || linkTurretToHull)
        {
            turretFacingDir = facing;
            desiredTurretFacingDir = facing;
            previewTurretTurnTimer = 0.0f;
            float ang = static_cast<float>(facing) * (static_cast<float>(M_PI) / 4.0f);
            float dist = std::max(60.0f, std::hypot(targetWorldX, targetWorldY));
            targetWorldX = std::cos(ang) * dist;
            targetWorldY = std::sin(ang) * dist;
        }
    }
    if (hasRotTurret)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(115.0f);
        if (Hinted(ImGui::SliderInt("##turretFacing", &turretFacingDir, 0, 7, _("Turret %d/7")), "##turretFacing"))
        {
            desiredTurretFacingDir = turretFacingDir;
            previewTurretTurnTimer = 0.0f;
            float ang = static_cast<float>(turretFacingDir) * (static_cast<float>(M_PI) / 4.0f);
            float dist = std::max(60.0f, std::hypot(targetWorldX, targetWorldY));
            targetWorldX = std::cos(ang) * dist;
            targetWorldY = std::sin(ang) * dist;
            targetCustomized = true;
        }
        ImGui::SameLine();
        Hinted(ImGui::Checkbox(_("Link"), &linkTurretToHull), "Link");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    Hinted(ImGui::SliderFloat("##speed", &playbackSpeed, 0.1f, 2.0f, _("Speed %.1fx")), "##speed");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(125.0f);
    if (Hinted(
            ImGui::SliderFloat("##bulletSpeed", &previewBulletSpeed, 100.0f, 2000.0f, _("Bullet: %.0f px/s")),
            "##bulletSpeed"))
    {
        YAML::Node combat = Section("combat");
        combat["bullet_speed"] = previewBulletSpeed;
        dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    if (Hinted(ImGui::SliderFloat("##fxSpeed", &previewFxSpeed, 0.2f, 3.0f, _("FX: %.1fx")), "##fxSpeed"))
    {
        YAML::Node combat = Section("combat");
        combat["fx_speed"] = previewFxSpeed;
        dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    Hinted(
        ImGui::SliderFloat(
            _("Zoom"), &previewZoom, kPreviewZoomMin, kPreviewZoomMax, "%.2fx", ImGuiSliderFlags_Logarithmic),
        "Zoom");
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Reset Aim")), "Reset Aim"))
    {
        float ang = static_cast<float>(facing) * (static_cast<float>(M_PI) / 4.0f);
        targetWorldX = std::cos(ang) * 120.0f;
        targetWorldY = std::sin(ang) * 120.0f;
        targetCustomized = false;
        turretFacingDir = facing;
        desiredHullFacingDir = facing;
        desiredTurretFacingDir = facing;
        previewHullTurnTimer = 0.0f;
        previewTurretTurnTimer = 0.0f;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Aim: (%.0f, %.0f)", targetWorldX, targetWorldY);
    if (hasRotTurret)
    {
        ImGui::SameLine();
        ImGui::TextColored(
            ImVec4(1.0f, 0.88f, 0.2f, 1.0f),
            "Muzzle: (%.1f, %.1f) Dome: (%.1f, %.1f)",
            muzzleOffsetX,
            muzzleOffsetY,
            turretOffsetX,
            turretOffsetY);
    }
    else
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.2f, 1.0f), "Muzzle: (%.1f, %.1f)", muzzleOffsetX, muzzleOffsetY);
    }

    Hinted(ImGui::Checkbox(_("No Gizmos"), &noGizmos), "No Gizmos");
    ImGui::SameLine();
    ImGui::BeginDisabled(noGizmos);
    Hinted(ImGui::Checkbox(_("Attack range (Red)"), &showAttackRange), "Attack range (Red)");
    ImGui::SameLine();
    Hinted(ImGui::Checkbox(_("Sight range (Green)"), &showSightRange), "Sight range (Green)");
    ImGui::SameLine();
    Hinted(ImGui::Checkbox(_("Build range (Purple)"), &showBuildRange), "Build range (Purple)");
    ImGui::SameLine();
    Hinted(ImGui::Checkbox(_("Radar range (Cyan)"), &showRadarRange), "Radar range (Cyan)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    Hinted(ImGui::Checkbox(_("Show ground (5x5)"), &showGround), "Show ground (5x5)");
    ImGui::SameLine();
    Hinted(ImGui::Checkbox(_("Combat HUD"), &showCombatHud), "Combat HUD");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (Hinted(ImGui::SliderInt(_("Health"), &previewHealthPercent, 0, 100, "%d%%"), "Health")
        && previewHealthPercent == 0)
    {
        previewTime = 0.0f;
    }
    if (document.IsDefined() && YamlString(document, "type") == "building")
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        Hinted(ImGui::SliderFloat(_("Construction"), &buildingProgress, 0.0f, 1.0f, "%.0f%%"), "Construction");
        ImGui::SameLine();
        Hinted(ImGui::Checkbox(_("Powered"), &buildingPowered), "Powered");
    }
    else if (
        document.IsDefined()
        && (YamlBool(document["abilities"], "siege_mode", false)
            || (document["abilities"] && document["abilities"]["siege"].IsMap())))
    {
        ImGui::SameLine();
        Hinted(ImGui::Checkbox(_("Test Siege Mode"), &testSiegeMode), "Test Siege Mode");
    }
    if (editorMode == EditorMode::Compare)
    {
        DrawCompareTable();
    }
}

void CharacterEditor::DrawCanvasGuides(const ImVec2 &min, const ImVec2 &size)
{
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    // The canvas background: black unless picked otherwise (Background, above the canvas)
    drawList->AddRectFilled(
        min,
        max,
        ImGui::ColorConvertFloat4ToU32(ImVec4(previewBackground[0], previewBackground[1], previewBackground[2], 1.0f)));
    // The layer over the colour: a lighter / darker shade of it, so it shows whatever the colour is
    if (previewBackdrop > 0)
    {
        const float lum = 0.299f * previewBackground[0] + 0.587f * previewBackground[1] + 0.114f * previewBackground[2];
        const float d = lum < 0.5f ? 0.14f : -0.14f;
        auto shade = [&](float k, int alpha = 255) {
            return IM_COL32(
                static_cast<int>(std::clamp(previewBackground[0] + d * k, 0.0f, 1.0f) * 255.0f),
                static_cast<int>(std::clamp(previewBackground[1] + d * k, 0.0f, 1.0f) * 255.0f),
                static_cast<int>(std::clamp(previewBackground[2] + d * k, 0.0f, 1.0f) * 255.0f),
                alpha);
        };
        const float cell = 32.0f;
        drawList->PushClipRect(min, max, true);
        if (previewBackdrop == 1)
        {
            const ImU32 c = shade(1.0f);
            for (int row = 0; min.y + row * cell < max.y; ++row)
                for (int col = (row & 1); min.x + col * cell < max.x; col += 2)
                    drawList->AddRectFilled(
                        ImVec2(min.x + col * cell, min.y + row * cell),
                        ImVec2(min.x + (col + 1) * cell, min.y + (row + 1) * cell),
                        c);
        }
        else if (previewBackdrop == 2)
        {
            const ImU32 c = shade(1.6f);
            for (float x = min.x; x < max.x; x += cell)
                drawList->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), c);
            for (float y = min.y; y < max.y; y += cell)
                drawList->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), c);
        }
        else if (previewBackdrop == 3)
        {
            drawList->AddRectFilledMultiColor(min, max, shade(0.0f, 0), shade(0.0f, 0), shade(3.0f), shade(3.0f));
        }
        else if (previewBackdrop == 4)
        {
            const ImU32 c = shade(1.0f);
            for (float x = min.x - size.y; x < max.x; x += cell * 2.0f)
                drawList->AddQuadFilled(
                    ImVec2(x, max.y),
                    ImVec2(x + cell, max.y),
                    ImVec2(x + cell + size.y, min.y),
                    ImVec2(x + size.y, min.y),
                    c);
        }
        drawList->PopClipRect();
    }
    // The scene (ground, characters) is drawn here, in the order of the canvas' draw list: what the UI puts on the
    // canvas (guides, banners, popups, other windows) stays above it
    drawList->AddCallback(
        [](const ImDrawList *, const ImDrawCmd *cmd) {
            static_cast<CharacterEditor *>(cmd->UserCallbackData)->DrawPreviewScene();
        },
        this);
    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    // Effect tabs show no character: no turret / target guides and no combat card of the character
    if (editorMode == EditorMode::Explosions || editorMode == EditorMode::Wrecks
        || editorMode == EditorMode::Projectiles)
    {
        drawList->AddText(
            ImVec2(min.x + 10.0f, min.y + size.y - 20.0f),
            IM_COL32(173, 190, 182, 220),
            _("MMB Drag: Pan | Wheel: Zoom | MMB 2x: Reset"));
        return;
    }
    if (editorMode == EditorMode::Simulation)
    {
        drawList->AddText(
            ImVec2(min.x + 10.0f, min.y + size.y - 20.0f),
            IM_COL32(173, 190, 182, 220),
            _("MMB Drag: Pan | Wheel: Zoom | MMB 2x: Fit arena"));
        return;
    }
    const bool comparing = editorMode == EditorMode::Compare && compareDoc.IsDefined();

    // Screen position of world origin (0, 0)
    if (!noGizmos)
    {
        float originScreenX = 0.0f;
        float originScreenY = 0.0f;
        camera->WorldToScreen(0.0f, 0.0f, originScreenX, originScreenY);

        if (originScreenX >= min.x && originScreenX <= max.x)
        {
            drawList->AddLine(ImVec2(originScreenX, min.y), ImVec2(originScreenX, max.y), IM_COL32(109, 151, 130, 80));
        }
        if (originScreenY >= min.y && originScreenY <= max.y)
        {
            drawList->AddLine(ImVec2(min.x, originScreenY), ImVec2(max.x, originScreenY), IM_COL32(109, 151, 130, 80));
        }
    }

    // Prominent status banners on canvas
    if (document.IsDefined() && testSiegeMode)
    {
        drawList->AddRectFilled(
            ImVec2(min.x + 10.0f, min.y + 10.0f),
            ImVec2(min.x + 320.0f, min.y + 36.0f),
            IM_COL32(35, 25, 10, 230),
            4.0f);
        drawList->AddRect(
            ImVec2(min.x + 10.0f, min.y + 10.0f),
            ImVec2(min.x + 320.0f, min.y + 36.0f),
            IM_COL32(245, 165, 30, 255),
            4.0f,
            0,
            1.5f);
        drawList->AddText(
            ImVec2(min.x + 18.0f, min.y + 14.0f),
            IM_COL32(255, 215, 60, 255),
            _("★ SIEGE MODE ACTIVE (Press G to toggle)"));
    }
    if (document.IsDefined() && previewMode == PreviewMode::Attack && activeFiringWeaponIndex == 1)
    {
        float bY = (testSiegeMode ? 42.0f : 10.0f);
        drawList->AddRectFilled(
            ImVec2(min.x + 10.0f, min.y + bY),
            ImVec2(min.x + 300.0f, min.y + bY + 26.0f),
            IM_COL32(45, 20, 10, 230),
            4.0f);
        drawList->AddRect(
            ImVec2(min.x + 10.0f, min.y + bY),
            ImVec2(min.x + 300.0f, min.y + bY + 26.0f),
            IM_COL32(255, 100, 30, 255),
            4.0f,
            0,
            1.5f);
        drawList->AddText(
            ImVec2(min.x + 18.0f, min.y + bY + 4.0f),
            IM_COL32(255, 150, 50, 255),
            _("🔥 WEAPON 2: FLAME TOWER ACTIVE"));
    }

    // A / B captions under the two compared characters
    if (comparing)
    {
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        camera->WorldToScreen(-compareSpacing, 58.0f, ax, ay);
        camera->WorldToScreen(compareSpacing, 58.0f, bx, by);
        const std::string la = "A: " + YamlString(document, "name", "?");
        const std::string lb = "B: " + YamlString(compareDoc, "name", "?");
        const ImVec2 sa = ImGui::CalcTextSize(la.c_str());
        const ImVec2 sb = ImGui::CalcTextSize(lb.c_str());
        drawList->AddText(ImVec2(ax - sa.x * 0.5f, ay), IM_COL32(255, 170, 150, 255), la.c_str());
        drawList->AddText(ImVec2(bx - sb.x * 0.5f, by), IM_COL32(150, 200, 255, 255), lb.c_str());
    }

    // Targeting reticle for Attack mode
    if (!noGizmos && !comparing && document.IsDefined() && previewMode == PreviewMode::Attack)
    {
        float targetScreenX = 0.0f;
        float targetScreenY = 0.0f;
        camera->WorldToScreen(targetWorldX, targetWorldY, targetScreenX, targetScreenY);
        const ImVec2 target(targetScreenX, targetScreenY);

        drawList->AddCircle(target, 16.0f, IM_COL32(236, 111, 77, 230), 24, 2.0f);
        drawList->AddCircle(target, 4.0f, IM_COL32(255, 170, 100, 255), 12, 1.5f);
        drawList->AddLine(
            ImVec2(target.x - 22.0f, target.y), ImVec2(target.x - 6.0f, target.y), IM_COL32(236, 111, 77, 200), 2.0f);
        drawList->AddLine(
            ImVec2(target.x + 6.0f, target.y), ImVec2(target.x + 22.0f, target.y), IM_COL32(236, 111, 77, 200), 2.0f);
        drawList->AddLine(
            ImVec2(target.x, target.y - 22.0f), ImVec2(target.x, target.y - 6.0f), IM_COL32(236, 111, 77, 200), 2.0f);
        drawList->AddLine(
            ImVec2(target.x, target.y + 6.0f), ImVec2(target.x, target.y + 22.0f), IM_COL32(236, 111, 77, 200), 2.0f);

        char label[64];
        std::snprintf(label, sizeof(label), "AIM (%.0f, %.0f)", targetWorldX, targetWorldY);
        drawList->AddText(ImVec2(target.x + 18.0f, target.y - 10.0f), IM_COL32(236, 140, 100, 220), label);
    }

    // Combat info HUD card in top-right of preview canvas (Compare mode shows the A vs B table instead)
    if (showCombatHud && !comparing && document.IsDefined())
    {
        YAML::Node combatNode = document["combat"];
        if (testSiegeMode && document["abilities"] && document["abilities"]["siege"]
            && document["abilities"]["siege"]["combat"].IsMap())
        {
            combatNode.reset(document["abilities"]["siege"]["combat"]);
        }
        const YAML::Node hudWeapon = MainWeapon(combatNode);
        const int damage = YamlInt(hudWeapon, "damage", 0);
        const float range = YamlFloat(hudWeapon, "range", 0.0f);
        const float minRange = YamlFloat(hudWeapon, "min_range", 0.0f);
        const float cooldown = std::max(0.05f, WeaponCooldown(hudWeapon, false));
        const int burstCount = std::max(1, YamlInt(hudWeapon, "burst_count", 1));
        const float burstDelay = std::max(0.01f, YamlFloat(hudWeapon, "burst_delay", 0.1f));
        const int totalBurstDmg = damage * burstCount;
        const float burstSpan = static_cast<float>(burstCount - 1) * burstDelay;
        const float cycleTime = cooldown + burstSpan;
        const float dps = (cycleTime > 0.01f) ? (static_cast<float>(totalBurstDmg) / cycleTime) : 0.0f;
        const std::string proj = YamlString(hudWeapon, "projectile", "bullet");
        const bool canAttack = YamlBool(combatNode, "can_attack", true);

        const float cardWidth = 230.0f;
        const float cardHeight = (burstCount > 1 ? 140.0f : 120.0f);
        const ImVec2 cardMin(min.x + size.x - cardWidth - 12.0f, min.y + 12.0f);
        const ImVec2 cardMax(cardMin.x + cardWidth, cardMin.y + cardHeight);

        drawList->AddRectFilled(cardMin, cardMax, IM_COL32(18, 24, 25, 225), 4.0f);
        drawList->AddRect(cardMin, cardMax, IM_COL32(65, 115, 95, 210), 4.0f);

        float ty = cardMin.y + 8.0f;
        float tx = cardMin.x + 10.0f;
        std::string name = YamlString(document, "name", "Unit");
        drawList->AddText(ImVec2(tx, ty), IM_COL32(230, 245, 238, 255), name.c_str());
        ty += 18.0f;

        if (!canAttack)
        {
            drawList->AddText(ImVec2(tx, ty), IM_COL32(160, 160, 160, 200), _("Combat: Disarmed"));
        }
        else
        {
            char buf[128];
            if (burstCount > 1)
            {
                std::snprintf(buf, sizeof(buf), _("Damage: %d (%d x %d burst)"), totalBurstDmg, burstCount, damage);
            }
            else
            {
                std::snprintf(buf, sizeof(buf), _("Damage: %d per shot"), damage);
            }
            drawList->AddText(ImVec2(tx, ty), IM_COL32(255, 130, 100, 240), buf);
            ty += 16.0f;

            std::snprintf(buf, sizeof(buf), _("DPS: %.1f dmg/s | Cycle: %.2fs"), dps, cycleTime);
            drawList->AddText(ImVec2(tx, ty), IM_COL32(240, 210, 120, 230), buf);
            ty += 16.0f;

            if (minRange > 0.0f)
            {
                std::snprintf(buf, sizeof(buf), _("Range: %.1f - %.1f tiles"), minRange, range);
            }
            else
            {
                std::snprintf(buf, sizeof(buf), _("Range: %.1f tiles (%.0f px)"), range, range * 30.0f);
            }
            drawList->AddText(ImVec2(tx, ty), IM_COL32(110, 220, 140, 230), buf);
            ty += 16.0f;

            if (burstCount > 1)
            {
                std::snprintf(buf, sizeof(buf), _("Burst: %d shots @ %.2fs interval"), burstCount, burstDelay);
                drawList->AddText(ImVec2(tx, ty), IM_COL32(200, 200, 140, 230), buf);
                ty += 16.0f;
            }

            std::snprintf(buf, sizeof(buf), _("Weapon: %s"), proj.c_str());
            drawList->AddText(ImVec2(tx, ty), IM_COL32(180, 210, 225, 230), buf);
        }
    }

    drawList->AddText(
        ImVec2(min.x + 10.0f, min.y + size.y - 20.0f),
        IM_COL32(173, 190, 182, 220),
        _("LMB: Turn Turret | RMB: Turn Hull | MMB Drag: Pan | Wheel: Zoom | MMB 2x: Reset"));
}

int CharacterEditor::PreviewAction() const
{
    switch (previewMode)
    {
        case PreviewMode::Move:
            return static_cast<int>(OpenArtOfWar::ActionState::Move);
        case PreviewMode::Attack:
            return static_cast<int>(OpenArtOfWar::ActionState::Attack);
        case PreviewMode::Die:
        case PreviewMode::Wreck:
            return static_cast<int>(OpenArtOfWar::ActionState::Die);
        case PreviewMode::Idle:
        default:
            return static_cast<int>(OpenArtOfWar::ActionState::Idle);
    }
}

void CharacterEditor::DrawPreviewScene()
{
    if (previewRect.w < 1 || previewRect.h < 1)
        return;
    SDL_Rect clip = previewRect;
    SDL_SetRenderClipRect(renderer, &clip);
    gameRenderer->SetArmyColor(0, previewArmyColor > 0, OpenArtOfWar::ArmyColor(previewArmyColor));
    gameRenderer->SetArmyColor(1, false, SDL_Color {255, 255, 255, 255});
    gameRenderer->SetDrawOwner(0);

    if (editorMode == EditorMode::Explosions || editorMode == EditorMode::Wrecks)
    {
        DrawPreviewGrid();
        DrawEffectsScene();
    }
    else if (editorMode == EditorMode::Projectiles)
    {
        DrawPreviewGrid();
        DrawProjectilesScene();
    }
    else if (editorMode == EditorMode::Simulation)
    {
        DrawPreviewGrid();
        battle.render(*gameRenderer, *camera, static_cast<int>(previewTime * 8.0f));
    }
    else if (document.IsDefined())
    {
        DrawPreviewGrid();
        const VisualOffsets primaryOffsets {
            muzzleOffsetX, muzzleOffsetY, turretOffsetX, turretOffsetY, hasCustomTurretOffset};
        if (editorMode == EditorMode::Compare && compareDoc.IsDefined())
        {
            // Everything is drawn in world space, so shifting the camera places A left and B right of the origin
            const float camX = camera->GetX();
            const float camY = camera->GetY();
            camera->SetPosition(camX + compareSpacing, camY);
            DrawCharacter(document, primaryOffsets, true);
            camera->SetPosition(camX - compareSpacing, camY);
            DrawCharacter(compareDoc, compareOffsets, false);
            camera->SetPosition(camX, camY);
        }
        else
        {
            DrawCharacter(document, primaryOffsets, true);
        }
    }

    gameRenderer->EndWorldPass();
    SDL_SetRenderClipRect(renderer, nullptr);
}

void CharacterEditor::DrawArmyColorCombo()
{
    const bool available = shaderEffects && shaderEffects->Available();
    ImGui::BeginDisabled(!available);
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::BeginCombo(_("Army color"), _(OpenArtOfWar::ArmyColorName(previewArmyColor))))
    {
        for (int i = 0; i <= 8; ++i)
        {
            const SDL_Color c = OpenArtOfWar::ArmyColor(i);
            if (i > 0)
            {
                ImGui::ColorButton(
                    ("##swatch" + std::to_string(i)).c_str(),
                    ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f),
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                    ImVec2(14, 14));
                ImGui::SameLine();
            }
            if (ImGui::Selectable(_(OpenArtOfWar::ArmyColorName(i)), i == previewArmyColor))
                previewArmyColor = i;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (available)
        FieldHint("Army color");
    else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", _("Needs the SDL GPU renderer (shader effects)"));
}

} // namespace CharEditor
