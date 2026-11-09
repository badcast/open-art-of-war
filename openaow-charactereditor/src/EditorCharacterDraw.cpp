#include "CharacterEditor.hpp"
#include "game/Effects.hpp"
#include "game/Projectiles.hpp"

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

void CharacterEditor::DrawCharacter(YAML::Node doc, const VisualOffsets &vo, bool primary)
{
    gameRenderer->SetDrawOwner(
        primary ? 0 : 1); // owner 0 wears the army color of the combo box, the opponent stays classic
    if (showGround)
    {
        // 5x5 ground tiles centered at world origin (0, 0)
        for (int row = -2; row <= 2; ++row)
        {
            for (int col = -2; col <= 2; ++col)
            {
                float tileX = static_cast<float>(col * OpenArtOfWar::TILE_WIDTH)
                              - static_cast<float>(OpenArtOfWar::TILE_WIDTH) * 0.5f;
                float tileY = static_cast<float>(row * OpenArtOfWar::TILE_HEIGHT)
                              - static_cast<float>(OpenArtOfWar::TILE_HEIGHT) * 0.5f;
                gameRenderer->DrawTileWorld(0, 0, tileX, tileY, *camera);
            }
        }
        // 2px border around 5x5 ground
        gameRenderer->DrawRectWorld(-75.0f, -50.0f, 150.0f, 100.0f, *camera, SDL_Color {85, 140, 105, 200});
        gameRenderer->DrawRectWorld(-76.0f, -51.0f, 152.0f, 102.0f, *camera, SDL_Color {85, 140, 105, 120});
    }

    const bool building = YamlString(doc, "type") == "building";
    const OpenArtOfWar::Faction faction = DocFaction(doc);
    const bool destroyed = previewHealthPercent == 0;
    const bool deathAnimation = destroyed || previewMode == PreviewMode::Die;
    const float frameClock = (loopPreview || deathAnimation) ? previewTime : 0.0f;
    const int frame = static_cast<int>(frameClock * 10.0f);

    float unitCenterX = 0.0f;
    float unitCenterY = 0.0f;
    int shownHullFacing = facing;                         // hull direction drawn this frame
    int shownArchetype = YamlInt(doc, "archetype_id", 0); // sprite set drawn (siege form included)

    const float *s_dirDx = OpenArtOfWar::Utility::kDir8X;
    const float *s_dirDy = OpenArtOfWar::Utility::kDir8Y;

    // Direction towards target from world origin
    float toMouseX = targetWorldX;
    float toMouseY = targetWorldY;
    const int aimDir8 = OpenArtOfWar::Utility::Dir8FromVector(toMouseX, toMouseY);

    YAML::Node combatNode = doc["combat"];
    YAML::Node statsNode = doc["stats"];
    if (testSiegeMode && doc["abilities"] && doc["abilities"]["siege"])
    {
        if (doc["abilities"]["siege"]["combat"].IsMap())
        {
            combatNode.reset(doc["abilities"]["siege"]["combat"]);
        }
        if (doc["abilities"]["siege"]["stats"].IsMap())
        {
            statsNode.reset(doc["abilities"]["siege"]["stats"]);
        }
    }

    const bool hasRotTurret = YamlBool(combatNode, "has_rotating_turret", false);
    const bool isInf = !building && [&]() {
        const int arch = YamlInt(doc, "archetype_id", 0);
        return OpenArtOfWar::Utility::IsInfantryArchetype(arch);
    }();

    if (building)
    {
        const int buildingIndex = YamlInt(doc, "building_index", -1);
        const YAML::Node size = doc["size"];
        const int width = YamlInt(size, "width", 1);
        const int height = YamlInt(size, "height", 1);
        const float footprintX = -static_cast<float>(width * OpenArtOfWar::TILE_WIDTH) * 0.5f;
        const float footprintY = -static_cast<float>(height * OpenArtOfWar::TILE_HEIGHT) * 0.5f;
        unitCenterX = 0.0f;
        unitCenterY = 0.0f;

        // Draw building footprint rectangle in world space
        gameRenderer->FillRectWorld(
            footprintX,
            footprintY,
            static_cast<float>(width * OpenArtOfWar::TILE_WIDTH),
            static_cast<float>(height * OpenArtOfWar::TILE_HEIGHT),
            *camera,
            SDL_Color {91, 153, 123, 40});
        gameRenderer->DrawRectWorld(
            footprintX,
            footprintY,
            static_cast<float>(width * OpenArtOfWar::TILE_WIDTH),
            static_cast<float>(height * OpenArtOfWar::TILE_HEIGHT),
            *camera,
            SDL_Color {118, 176, 146, 180});

        if (buildingIndex >= 0)
        {
            bool attacking = false;
            if (!destroyed && previewMode == PreviewMode::Attack && YamlBool(combatNode, "can_attack", true))
            {
                const YAML::Node gun = MainWeapon(combatNode);
                const int burstCount = std::max(1, YamlInt(gun, "burst_count", 1));
                const float burstDelay = std::max(0.04f, YamlFloat(gun, "burst_delay", 0.1f));
                const float cooldown = std::max(0.15f, WeaponCooldown(gun, false, 0.7f));
                const float burstSpan = static_cast<float>(burstCount - 1) * burstDelay;
                const float period = std::max(0.35f, burstSpan + cooldown);
                const float phase = std::fmod(previewTime, period);
                for (int k = 0; k < burstCount; ++k)
                {
                    float fireTime = static_cast<float>(k) * burstDelay;
                    float dt = phase - fireTime;
                    if (dt >= 0.0f && dt < 0.16f)
                    {
                        attacking = true;
                        break;
                    }
                }
            }
            const int bTurretFacing
                = hasRotTurret ? turretFacingDir : ((previewMode == PreviewMode::Attack) ? aimDir8 : facing);
            int wreckComp = -1, wreckStages = 0;
            WreckOf(doc, wreckComp, wreckStages);

            if (previewMode == PreviewMode::Wreck)
            {
                gameRenderer->DrawBuildingWorld(
                    buildingIndex,
                    footprintX,
                    footprintY,
                    0,
                    *camera,
                    faction,
                    CurrentWreckStage(wreckStages),
                    1.0f,
                    true,
                    1.0f,
                    0,
                    false,
                    true,
                    wreckComp,
                    wreckStages);
            }
            else if (destroyed)
            {
                // Destruction sequence: explosion starts, and from the middle of destruction the crater appears
                const auto &fx = OpenArtOfWar::EffectsConfig::Get();
                const int expAnim = (width <= 2 && height <= 2) ? fx.buildingSmallMain
                                    : (buildingIndex == 2 || buildingIndex == 4 || buildingIndex == 12)
                                        ? fx.buildingPowerMain
                                        : fx.buildingLargeMain;
                const auto expRange = textures->GetAnimRange(expAnim);
                const int expFrames = std::max(0, expRange.end - expRange.start + 1);
                const float expDur = expFrames * fx.frameTime;
                bool showRemnant = (previewTime >= expDur * 0.45f);

                gameRenderer->DrawBuildingWorld(
                    buildingIndex,
                    footprintX,
                    footprintY,
                    0,
                    *camera,
                    faction,
                    showRemnant ? 0 : frame,
                    buildingProgress,
                    showRemnant,
                    static_cast<float>(previewHealthPercent) / 100.0f,
                    bTurretFacing,
                    false,
                    buildingPowered,
                    wreckComp,
                    wreckStages);
                gameRenderer->DrawBuildingWorld(
                    buildingIndex,
                    footprintX,
                    footprintY,
                    1,
                    *camera,
                    faction,
                    frame,
                    buildingProgress,
                    showRemnant,
                    static_cast<float>(previewHealthPercent) / 100.0f,
                    bTurretFacing,
                    false,
                    buildingPowered);

                if (expFrames > 0 && previewTime < expDur)
                {
                    int expFrame = std::clamp(static_cast<int>(previewTime / fx.frameTime), 0, expFrames - 1);
                    float ox = 0.0f, oy = 0.0f;
                    OpenArtOfWar::ExplosionCentering(expAnim, ox, oy);
                    gameRenderer->DrawAnimSequenceWorld(expAnim, expFrame, ox, oy, *camera, faction);
                }
            }
            else
            {
                gameRenderer->DrawBuildingWorld(
                    buildingIndex,
                    footprintX,
                    footprintY,
                    0,
                    *camera,
                    faction,
                    frame,
                    buildingProgress,
                    false,
                    static_cast<float>(previewHealthPercent) / 100.0f,
                    bTurretFacing,
                    attacking,
                    buildingPowered);
                gameRenderer->DrawBuildingWorld(
                    buildingIndex,
                    footprintX,
                    footprintY,
                    1,
                    *camera,
                    faction,
                    frame,
                    buildingProgress,
                    false,
                    static_cast<float>(previewHealthPercent) / 100.0f,
                    bTurretFacing,
                    attacking,
                    buildingPowered);
            }
        }
        else
        {
            gameRenderer->DrawLineWorld(-24.0f, -24.0f, 24.0f, 24.0f, *camera, SDL_Color {255, 160, 120, 255});
            gameRenderer->DrawLineWorld(24.0f, -24.0f, -24.0f, 24.0f, *camera, SDL_Color {255, 160, 120, 255});
        }
    }
    else
    {
        // Unit drawing: tile top-left is (-15, -10) so center is at (0, 0)
        float unitX = -static_cast<float>(OpenArtOfWar::TILE_WIDTH) * 0.5f;
        float unitY = -static_cast<float>(OpenArtOfWar::TILE_HEIGHT) * 0.5f;
        int unitFacing = facing;

        if (previewMode == PreviewMode::Move && !destroyed)
        {
            const MoveSample m
                = SampleMove(YamlInt(doc, "archetype_id", 0), YamlFloat(statsNode, "move_speed", 0.0f), previewTime);
            unitX += m.offsetX;
            unitFacing = m.facing;
        }
        else if (!hasRotTurret && previewMode == PreviewMode::Attack)
        {
            unitFacing = facing;
        }

        unitCenterX = unitX + static_cast<float>(OpenArtOfWar::TILE_WIDTH) * 0.5f;
        unitCenterY = unitY + static_cast<float>(OpenArtOfWar::TILE_HEIGHT) * 0.5f;

        int archetypeId = YamlInt(doc, "archetype_id", 0);
        if (testSiegeMode)
        {
            int sArch = -1;
            if (doc["abilities"])
            {
                if (doc["abilities"]["siege"] && doc["abilities"]["siege"].IsMap())
                {
                    sArch = YamlInt(doc["abilities"]["siege"], "archetype_id", -1);
                }
                if (sArch < 0)
                {
                    sArch = YamlInt(doc["abilities"], "siege_archetype_id", -1);
                }
            }
            if (sArch < 0)
            {
                if (archetypeId == 10)
                    sArch = 15; // Rhino Siege
                else if (archetypeId == 8)
                    sArch = 16; // Grad Deployed
                else if (archetypeId == 4)
                    sArch = 4; // Sniper
            }
            if (sArch >= 0)
                archetypeId = sArch;
        }

        int action = static_cast<int>(OpenArtOfWar::ActionState::Idle);
        // Turret rotates step-by-step via turretFacingDir (updated in updateCombatSim)
        const int turretFacing = hasRotTurret ? turretFacingDir : unitFacing;
        shownHullFacing = unitFacing;
        shownArchetype = archetypeId;

        if (previewMode == PreviewMode::Wreck)
        {
            // Remains only: vehicles sink stage by stage, infantry leaves its corpse
            int wreckComp = -1, wreckStages = 0;
            WreckOf(doc, wreckComp, wreckStages);
            const int stage = CurrentWreckStage(wreckStages);
            if (isInf)
            {
                const YAML::Node seqs = doc["animations"]["death_sequences"];
                const int corpseSeq = (seqs && seqs.IsSequence() && seqs.size() > 0)
                                          ? SpriteRefHandle(seqs[0], OpenArtOfWar::SpriteKind::Sequence)
                                          : -1;
                gameRenderer->DrawUnitWorld(
                    archetypeId,
                    static_cast<int>(OpenArtOfWar::ActionState::Die),
                    unitFacing,
                    99,
                    unitX,
                    unitY,
                    *camera,
                    faction,
                    turretFacing,
                    testFlameDeath,
                    -1,
                    0,
                    corpseSeq);
            }
            else
            {
                gameRenderer->DrawUnitWorld(
                    archetypeId,
                    static_cast<int>(OpenArtOfWar::ActionState::Die),
                    unitFacing,
                    stage,
                    unitX,
                    unitY,
                    *camera,
                    faction,
                    turretFacing,
                    true,
                    wreckComp,
                    wreckStages);
            }
        }
        else if (destroyed)
        {
            action = static_cast<int>(OpenArtOfWar::ActionState::Die);
            if (!isInf)
            {
                const auto &fx = OpenArtOfWar::EffectsConfig::Get();
                const int expAnim = fx.vehicleMain.empty() ? -1 : fx.vehicleMain.front();
                const auto expRange = textures->GetAnimRange(expAnim);
                const int expFrames = std::max(0, expRange.end - expRange.start + 1);
                const float expDur = expFrames * fx.frameTime;
                bool showWreck = (previewTime >= expDur * 0.45f);

                if (showWreck)
                {
                    int wreckComp = -1, wreckStages = 0;
                    WreckOf(doc, wreckComp, wreckStages);
                    gameRenderer->DrawUnitWorld(
                        archetypeId,
                        action,
                        unitFacing,
                        0,
                        unitX,
                        unitY,
                        *camera,
                        faction,
                        turretFacing,
                        true,
                        wreckComp,
                        wreckStages);
                }
                else
                {
                    gameRenderer->DrawUnitWorld(
                        archetypeId,
                        static_cast<int>(OpenArtOfWar::ActionState::Idle),
                        unitFacing,
                        frame,
                        unitX,
                        unitY,
                        *camera,
                        faction,
                        turretFacing,
                        true);
                }

                if (expFrames > 0 && previewTime < expDur)
                {
                    int expFrame = std::clamp(static_cast<int>(previewTime / fx.frameTime), 0, expFrames - 1);
                    float ox = 0.0f, oy = 0.0f;
                    OpenArtOfWar::ExplosionCentering(expAnim, ox, oy);
                    gameRenderer->DrawAnimSequenceWorld(expAnim, expFrame, ox, oy, *camera, faction);
                }
            }
            else
            {
                gameRenderer->DrawUnitWorld(
                    archetypeId,
                    action,
                    unitFacing,
                    frame,
                    unitX,
                    unitY,
                    *camera,
                    faction,
                    turretFacing,
                    testFlameDeath);
            }
        }
        else if (previewMode == PreviewMode::Move)
        {
            action = static_cast<int>(OpenArtOfWar::ActionState::Move);
            const AnimTiming timing = ReadAnimTiming(doc);
            const int moveFrame = static_cast<int>(frameClock / std::max(0.02f, timing.moveFrame));
            gameRenderer->DrawUnitWorld(
                archetypeId, action, unitFacing, moveFrame, unitX, unitY, *camera, faction, turretFacing, true);
        }
        else if (previewMode == PreviewMode::Attack && YamlBool(combatNode, "can_attack", true))
        {
            const YAML::Node gun = MainWeapon(combatNode);
            const int burstCount = std::max(1, YamlInt(gun, "burst_count", 1));
            const float burstDelay = std::max(0.04f, YamlFloat(gun, "burst_delay", 0.1f));
            const float cooldown = std::max(0.15f, WeaponCooldown(gun, false, 0.7f));
            const float burstSpan = static_cast<float>(burstCount - 1) * burstDelay;
            // The game counts the cooldown from the last shot of a burst
            const float period = std::max(0.35f, burstSpan + cooldown);
            const float phase = std::fmod(previewTime, period);
            const AnimTiming timing = ReadAnimTiming(doc);
            const float firstPose = timing.attackPose >= 0.0f
                                        ? timing.attackPose
                                        : DefaultAttackPose(YamlString(gun, "projectile", "bullet"));
            // Time since the latest shot (the game restarts the firing animation on every shot) and the length of
            // the pose that shot started
            float sinceShot = phase;
            float pose = firstPose;
            for (int k = 0; k < burstCount; ++k)
            {
                const float fireTime = static_cast<float>(k) * burstDelay;
                if (phase >= fireTime)
                {
                    sinceShot = phase - fireTime;
                    pose = (k == 0) ? firstPose : timing.burstPose;
                }
            }
            const bool firing = sinceShot < pose;
            // Infantry keeps its crouched firing pose for the whole engagement (no standing up between
            // shots), vehicles show the firing layers only right after a shot - both exactly as in the game
            action = (firing || isInf) ? static_cast<int>(OpenArtOfWar::ActionState::Attack)
                                       : static_cast<int>(OpenArtOfWar::ActionState::Idle);
            const float stepDelay = AttackFrameTime(timing, archetypeId);
            const int attackFrame = static_cast<int>(sinceShot / stepDelay);
            gameRenderer->DrawUnitWorld(
                archetypeId,
                action,
                unitFacing,
                attackFrame,
                unitX,
                unitY,
                *camera,
                faction,
                turretFacing,
                true,
                -1,
                -1,
                -1,
                timing.flashMode,
                timing.flashFrames);
        }
        else
        {
            action = PreviewAction();
            bool flaming = (action == static_cast<int>(OpenArtOfWar::ActionState::Die)) ? testFlameDeath : true;
            gameRenderer->DrawUnitWorld(
                archetypeId, action, unitFacing, frame, unitX, unitY, *camera, faction, turretFacing, flaming);
        }
    }

    // Active firing weapon resolution
    YAML::Node activeWeaponNode;
    int activeWeaponIdx = 0;
    float distTiles = std::hypot(targetWorldX, targetWorldY) / static_cast<float>(OpenArtOfWar::TILE_WIDTH);

    if (combatNode["weapons"] && combatNode["weapons"].IsSequence() && combatNode["weapons"].size() >= 2)
    {
        bool useW2 = false;
        YAML::Node w2 = combatNode["weapons"][1];
        bool w2Enabled = YamlBool(w2, "enabled", true);
        bool w2ReqResearch = YamlBool(w2, "requires_research", false);
        float w2MaxDist = YamlFloat(w2, "max_distance", 1.5f);

        if (testWeaponSelectMode == 2)
        {
            useW2 = w2Enabled;
        }
        else if (testWeaponSelectMode == 1)
        {
            useW2 = false;
        }
        else
        {
            if (w2Enabled && (!w2ReqResearch || simLabUpgradeUnlocked))
            {
                if (w2MaxDist <= 0.0f || distTiles <= w2MaxDist)
                {
                    useW2 = true;
                }
            }
        }

        if (useW2)
        {
            activeWeaponIdx = 1;
            activeWeaponNode = w2;
        }
        else
        {
            activeWeaponIdx = 0;
            activeWeaponNode = combatNode["weapons"][0];
        }
    }
    else
    {
        int arch = YamlInt(doc, "archetype_id", 0);
        if ((arch == 7 || arch == 18) && distTiles <= 1.5f && simLabUpgradeUnlocked && testWeaponSelectMode != 1)
        {
            activeWeaponIdx = 1;
        }
    }
    if (primary)
        activeFiringWeaponIndex = activeWeaponIdx;

    // Sight and Attack Radius Circles (2px thickness)
    float attackRangeTiles = 0.0f;
    try
    {
        if (activeWeaponNode.IsDefined() && activeWeaponNode["range"])
        {
            attackRangeTiles = activeWeaponNode["range"].as<float>(0.0f);
        }
        else if (const YAML::Node w0 = MainWeapon(combatNode); w0 && w0["range"])
        {
            attackRangeTiles = w0["range"].as<float>(0.0f);
        }
    }
    catch (...)
    {}

    float sightRangeTiles = 0.0f;
    try
    {
        if (statsNode && statsNode["sight_radius"])
        {
            sightRangeTiles = statsNode["sight_radius"].as<float>(0.0f);
        }
    }
    catch (...)
    {}

    // Green Sight Radius (2px line width, tile isometric ground ellipse via RenderUtility::DrawTileRadius)
    if (!noGizmos && showSightRange && sightRangeTiles > 0.0f)
    {
        DrawTileRadius(unitCenterX, unitCenterY, sightRangeTiles, SDL_Color {50, 230, 80, 240}, 2.0f);
    }

    // Purple Build Radius of a headquarters / command center (same ground ellipse as in the game's HUD)
    if (!noGizmos && showBuildRange && building)
    {
        float buildRangeTiles = 0.0f;
        try
        {
            if (statsNode && statsNode["build_radius"])
                buildRangeTiles = statsNode["build_radius"].as<float>(0.0f);
        }
        catch (...)
        {}
        if (buildRangeTiles > 0.0f)
        {
            DrawTileRadius(unitCenterX, unitCenterY, buildRangeTiles, SDL_Color {160, 80, 230, 240}, 2.0f);
        }
    }

    // Cyan radar range of a Radar Locator: how far the finished building sees (sight radius + radar range)
    if (!noGizmos && showRadarRange && building)
    {
        float radarTiles = 0.0f;
        try
        {
            if (statsNode && statsNode["radar_range"])
                radarTiles = statsNode["radar_range"].as<float>(0.0f);
        }
        catch (...)
        {}
        if (radarTiles > 0.0f)
        {
            DrawTileRadius(unitCenterX, unitCenterY, sightRangeTiles + radarTiles, SDL_Color {80, 200, 255, 240}, 2.0f);
        }
    }

    // Red Attack Radius (2px line width, tile isometric ground ellipse via RenderUtility::DrawTileRadius)
    if (!noGizmos && showAttackRange && attackRangeTiles > 0.0f)
    {
        DrawTileRadius(unitCenterX, unitCenterY, attackRangeTiles, SDL_Color {255, 60, 60, 240}, 2.0f);
    }

    // Projectile exit point calculation (localePosition active for ALL entities)
    float basePivotX = unitCenterX;
    float basePivotY = unitCenterY;
    int activeFacing = facing;

    if (hasRotTurret)
    {
        // The turret sits elsewhere on the hull for every hull direction: follow the sprite like the game
        float shiftX = 0.0f, shiftY = 0.0f;
        if (!building)
            TurretShift(shownArchetype, shownHullFacing, shiftX, shiftY);
        basePivotX += vo.turretX + shiftX;
        basePivotY += vo.turretY + shiftY;
        activeFacing = turretFacingDir;
    }
    else
    {
        if (!building && isInf)
        {
            basePivotY -= 4.0f; // Infantry weapon height
        }
        activeFacing = shownHullFacing;
    }
    if (primary)
    {
        simHullFacing = shownHullFacing;
        simArchetype = shownArchetype;
    }

    float curMuzzleX = vo.muzzleX;
    float curMuzzleY = vo.muzzleY;
    if (activeWeaponNode.IsDefined() && activeWeaponNode["localePosition"]
        && activeWeaponNode["localePosition"].IsSequence() && activeWeaponNode["localePosition"].size() >= 2)
    {
        try
        {
            curMuzzleX = activeWeaponNode["localePosition"][0].as<float>(vo.muzzleX);
            curMuzzleY = activeWeaponNode["localePosition"][1].as<float>(vo.muzzleY);
        }
        catch (...)
        {}
    }

    const int tf = (activeFacing % 8 + 8) % 8;
    float muzzleX = basePivotX;
    float muzzleY = basePivotY;
    if (!building || hasRotTurret)
    {
        muzzleX = basePivotX + s_dirDx[tf] * curMuzzleX - s_dirDy[tf] * curMuzzleY;
        muzzleY = basePivotY + s_dirDy[tf] * curMuzzleX + s_dirDx[tf] * curMuzzleY;
    }
    else
    {
        muzzleX = basePivotX + curMuzzleX;
        muzzleY = basePivotY + curMuzzleY;
    }

    // Draw visual Pivot dot, barrel connection line, and yellow/orange Muzzle Crosshair in ALL preview modes!
    if (!noGizmos && !destroyed)
    {
        // Center pivot indicator (cyan dot)
        OpenArtOfWar::RenderUtility::DrawRadius(
            *gameRenderer, *camera, basePivotX, basePivotY, 2.0f, SDL_Color {80, 220, 255, 220}, 1.5f);
        // Line from pivot to muzzle exit point
        gameRenderer->DrawLineWorld(
            basePivotX,
            basePivotY,
            muzzleX,
            muzzleY,
            *camera,
            activeFiringWeaponIndex == 1 ? SDL_Color {255, 140, 50, 220} : SDL_Color {255, 200, 50, 180});

        // Visual muzzle exit point marker (localePosition)
        SDL_Color muzzleCol
            = (activeFiringWeaponIndex == 1) ? SDL_Color {255, 120, 30, 255} : SDL_Color {255, 230, 40, 240};
        OpenArtOfWar::RenderUtility::DrawRadius(*gameRenderer, *camera, muzzleX, muzzleY, 3.5f, muzzleCol, 1.5f);
        gameRenderer->DrawLineWorld(muzzleX - 4.0f, muzzleY, muzzleX + 4.0f, muzzleY, *camera, muzzleCol);
        gameRenderer->DrawLineWorld(muzzleX, muzzleY - 4.0f, muzzleX, muzzleY + 4.0f, *camera, muzzleCol);
    }

    // Projectile & Combat FX (Burst Simulation) - only the edited character runs the projectile sim
    if (primary && !destroyed && previewMode == PreviewMode::Attack && YamlBool(combatNode, "can_attack", true))
    {
        // Draw line of fire toward directed target point
        if (!noGizmos)
        {
            gameRenderer->DrawLineWorld(
                muzzleX, muzzleY, targetWorldX, targetWorldY, *camera, SDL_Color {236, 111, 77, 90});
        }

        // Muzzle flashes are part of the character firing layers drawn above.
        // Registry animation handles are dynamic; a numeric ID can draw another unit.

        // Projectiles and impact effects in flight
        // Each projectile flies strictly to its own locked target from the moment of firing!
        for (const auto &p : simProjectiles)
        {
            if (!p.impacted)
            {
                if (p.projectileType == "flame")
                {
                    // Same as the game: the stream is anchored at the nozzle and plays its 12 frames
                    // while the flame travels to the target
                    float angle = std::atan2(p.targetY - p.startY, p.targetX - p.startX);
                    const float prog = std::clamp(p.traveledDist / std::max(1.0f, p.totalDist), 0.0f, 1.0f);
                    const int flameFrame = std::clamp(static_cast<int>(prog * 11.0f), 0, 11);
                    gameRenderer->DrawAnimSequenceWorldRotated(
                        59, flameFrame, p.startX, p.startY, angle, *camera, faction);
                }
                else if (p.projectileType != "none")
                {
                    // The sprite of the projectile kind (projectiles.yaml), as the game draws it
                    const auto &spec
                        = OpenArtOfWar::Projectiles::of(OpenArtOfWar::Projectiles::FromKey(p.projectileType));
                    if (spec.sprite > 0)
                        gameRenderer->DrawProjectileWorld(
                            spec.sprite,
                            spec.directions == 16 ? p.dir16 : p.dir32,
                            p.curX,
                            p.curY,
                            *camera,
                            faction,
                            spec.directions);
                }
            }
            else
            {
                float impactProgress = 1.0f - (p.impactTimer / p.maxImpactTime);
                int impactFrame = std::clamp(static_cast<int>(impactProgress * 4.0f), 0, 3);
                // The hit animation of the projectile kind, centred like the game does
                const auto &spec = OpenArtOfWar::Projectiles::of(OpenArtOfWar::Projectiles::FromKey(p.projectileType));
                const int impactAnim = spec.PickImpact(0);
                float offX = 0.0f, offY = 0.0f;
                OpenArtOfWar::ExplosionCentering(impactAnim, offX, offY);
                const OpenArtOfWar::IndexRange impactRange = textures->GetAnimRange(std::max(0, impactAnim));
                impactFrame = std::clamp(
                    static_cast<int>(
                        impactProgress * static_cast<float>(std::max(1, impactRange.end - impactRange.start + 1))),
                    0,
                    std::max(0, impactRange.end - impactRange.start));
                if (p.projectileType == "flame" || impactAnim < 0)
                {
                    // The game draws no impact for flames: the stream itself ends at the target
                }
                else
                {
                    gameRenderer->DrawAnimSequenceWorld(
                        impactAnim, impactFrame, p.targetX + offX, p.targetY + offY, *camera, faction);
                }
            }
        }
    }
}

void CharacterEditor::DrawPreviewGrid()
{
    float leftWorld = 0.0f;
    float topWorld = 0.0f;
    float rightWorld = 0.0f;
    float bottomWorld = 0.0f;
    camera->ScreenToWorld(static_cast<float>(previewRect.x), static_cast<float>(previewRect.y), leftWorld, topWorld);
    camera->ScreenToWorld(
        static_cast<float>(previewRect.x + previewRect.w),
        static_cast<float>(previewRect.y + previewRect.h),
        rightWorld,
        bottomWorld);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 40, 51, 49, 120);
    const int firstColumn = static_cast<int>(std::floor(leftWorld / OpenArtOfWar::TILE_WIDTH));
    const int lastColumn = static_cast<int>(std::ceil(rightWorld / OpenArtOfWar::TILE_WIDTH));
    for (int column = firstColumn; column <= lastColumn; ++column)
    {
        float screenX = 0.0f;
        float screenY = 0.0f;
        camera->WorldToScreen(static_cast<float>(column * OpenArtOfWar::TILE_WIDTH), topWorld, screenX, screenY);
        SDL_RenderLine(
            renderer,
            screenX,
            static_cast<float>(previewRect.y),
            screenX,
            static_cast<float>(previewRect.y + previewRect.h));
    }

    const int firstRow = static_cast<int>(std::floor(topWorld / OpenArtOfWar::TILE_HEIGHT));
    const int lastRow = static_cast<int>(std::ceil(bottomWorld / OpenArtOfWar::TILE_HEIGHT));
    for (int row = firstRow; row <= lastRow; ++row)
    {
        float screenX = 0.0f;
        float screenY = 0.0f;
        camera->WorldToScreen(leftWorld, static_cast<float>(row * OpenArtOfWar::TILE_HEIGHT), screenX, screenY);
        SDL_RenderLine(
            renderer,
            static_cast<float>(previewRect.x),
            screenY,
            static_cast<float>(previewRect.x + previewRect.w),
            screenY);
    }
}

void CharacterEditor::UpdateCombatSim(float dt)
{
    if (document.IsDefined())
    {
        const bool building = (YamlString(document, "type") == "building");
        const int arch = YamlInt(document, "archetype_id", 0);
        const bool isInf = !building && OpenArtOfWar::Utility::IsInfantryArchetype(arch);
        YAML::Node cNode = document["combat"];
        if (testSiegeMode && document["abilities"] && document["abilities"]["siege"]
            && document["abilities"]["siege"]["combat"].IsMap())
        {
            cNode.reset(document["abilities"]["siege"]["combat"]);
        }
        const bool hasRotTurret = YamlBool(cNode, "has_rotating_turret", false);
        const int effArch = (simArchetype >= 0) ? simArchetype : arch;
        const float hullDelay
            = (building || isInf)
                  ? 0.0f
                  : std::max(
                        0.0f,
                        YamlFloat(document, "turn_step_delay", OpenArtOfWar::Utility::DefaultTurnStepDelay(effArch)));
        const float defTurretDelay = document["turn_step_delay"].IsDefined()
                                         ? hullDelay
                                         : OpenArtOfWar::Utility::DefaultTurretTurnStepDelay(effArch);
        const float turretDelay
            = isInf ? 0.0f
                    : (building
                           ? (hasRotTurret ? std::max(0.0f, YamlFloat(cNode, "turret_turn_step_delay", 0.08f)) : 0.0f)
                           : std::max(
                                 0.0f,
                                 YamlFloat(
                                     cNode,
                                     "turret_turn_step_delay",
                                     YamlFloat(document, "turret_turn_step_delay", defTurretDelay))));

        const int aimDir8 = OpenArtOfWar::Utility::Dir8FromVector(targetWorldX, targetWorldY);
        int targetHullDir = desiredHullFacingDir;
        if (!hasRotTurret && previewMode == PreviewMode::Attack && targetCustomized)
        {
            targetHullDir = aimDir8;
        }
        targetHullDir = (targetHullDir % 8 + 8) % 8;
        facing = (facing % 8 + 8) % 8;
        if (facing == targetHullDir || hullDelay <= 0.0f)
        {
            facing = targetHullDir;
            previewHullTurnTimer = 0.0f;
        }
        else
        {
            previewHullTurnTimer += dt;
            while (previewHullTurnTimer >= hullDelay && facing != targetHullDir)
            {
                previewHullTurnTimer -= hullDelay;
                facing = (facing + OpenArtOfWar::Utility::TurnStep8(facing, targetHullDir) + 8) % 8;
            }
            if (facing == targetHullDir)
                previewHullTurnTimer = 0.0f;
        }

        int targetTurretDir = desiredTurretFacingDir;
        if (previewMode == PreviewMode::Attack && targetCustomized)
        {
            targetTurretDir = aimDir8;
        }
        else if (previewMode == PreviewMode::Move && (!hasRotTurret || (linkTurretToHull && !targetCustomized)))
        {
            targetTurretDir = simHullFacing;
        }
        else if (!hasRotTurret)
        {
            targetTurretDir = facing;
        }
        targetTurretDir = (targetTurretDir % 8 + 8) % 8;
        turretFacingDir = (turretFacingDir % 8 + 8) % 8;
        if (!hasRotTurret)
        {
            turretFacingDir = (previewMode == PreviewMode::Move) ? simHullFacing : facing;
            previewTurretTurnTimer = 0.0f;
        }
        else if (turretFacingDir == targetTurretDir || turretDelay <= 0.0f)
        {
            turretFacingDir = targetTurretDir;
            previewTurretTurnTimer = 0.0f;
        }
        else
        {
            previewTurretTurnTimer += dt;
            while (previewTurretTurnTimer >= turretDelay && turretFacingDir != targetTurretDir)
            {
                previewTurretTurnTimer -= turretDelay;
                turretFacingDir
                    = (turretFacingDir + OpenArtOfWar::Utility::TurnStep8(turretFacingDir, targetTurretDir) + 8) % 8;
            }
            if (turretFacingDir == targetTurretDir)
                previewTurretTurnTimer = 0.0f;
        }
    }

    if (!document.IsDefined() || previewMode != PreviewMode::Attack)
    {
        simProjectiles.clear();
        simShotCooldown = 0.0f;
        simBurstIndex = 0;
        simBurstDelayTimer = 0.0f;
        simShotCooldown2 = 0.0f;
        simBurstIndex2 = 0;
        simBurstDelayTimer2 = 0.0f;
        return;
    }

    YAML::Node combatNode = document["combat"];
    if (testSiegeMode && document["abilities"] && document["abilities"]["siege"]
        && document["abilities"]["siege"]["combat"].IsMap())
    {
        combatNode.reset(document["abilities"]["siege"]["combat"]);
    }
    if (!YamlBool(combatNode, "can_attack", true))
    {
        simProjectiles.clear();
        return;
    }

    const bool building = (YamlString(document, "type") == "building");
    const bool isInf = !building && [&]() {
        const int arch = YamlInt(document, "archetype_id", 0);
        return OpenArtOfWar::Utility::IsInfantryArchetype(arch);
    }();
    const bool hasRotTurret = YamlBool(combatNode, "has_rotating_turret", false);

    const int aimDir8 = OpenArtOfWar::Utility::Dir8FromVector(targetWorldX, targetWorldY);
    const float *s_dirDx = OpenArtOfWar::Utility::kDir8X;
    const float *s_dirDy = OpenArtOfWar::Utility::kDir8Y;
    float tPivotX = 0.0f, tPivotY = 0.0f;
    if (hasRotTurret)
    {
        float shiftX = 0.0f, shiftY = 0.0f;
        if (!building)
            TurretShift(simArchetype, simHullFacing, shiftX, shiftY);
        tPivotX = turretOffsetX + shiftX;
        tPivotY = turretOffsetY + shiftY;
    }

    float basePivotX = 0.0f;
    float basePivotY = 0.0f;
    if (hasRotTurret)
    {
        basePivotX = tPivotX;
        basePivotY = tPivotY;
    }
    else
    {
        if (!building && isInf)
        {
            basePivotY = -4.0f;
        }
    }

    const int activeSimFacing = hasRotTurret ? turretFacingDir : facing;
    const int tf = (activeSimFacing % 8 + 8) % 8;
    const int expectedAimDir
        = targetCustomized ? aimDir8 : (hasRotTurret ? desiredTurretFacingDir : desiredHullFacingDir);
    const bool aimAligned = (!building && !hasRotTurret && isInf) || (tf == (expectedAimDir % 8 + 8) % 8);

    float distTiles = std::hypot(targetWorldX, targetWorldY) / static_cast<float>(OpenArtOfWar::TILE_WIDTH);

    struct SimWeaponUnit
    {
        YAML::Node node;
        int idx {0};
        float &cd;
        int &bIdx;
        float &bDelay;
    };
    std::vector<SimWeaponUnit> weaponsToFire;

    if (combatNode["weapons"] && combatNode["weapons"].IsSequence() && combatNode["weapons"].size() >= 2)
    {
        YAML::Node w0 = combatNode["weapons"][0];
        YAML::Node w1 = combatNode["weapons"][1];
        bool w0Enabled = YamlBool(w0, "enabled", true);
        bool w1Enabled = YamlBool(w1, "enabled", true);
        bool w1ReqResearch = YamlBool(w1, "requires_research", false);
        float w0Range = YamlFloat(w0, "range", 5.0f);
        float w1Range = YamlFloat(w1, "range", 1.5f);
        float w1MaxDist = YamlFloat(w1, "max_distance", 1.5f);

        bool can0 = w0Enabled && (distTiles <= w0Range);
        bool can1 = w1Enabled && (distTiles <= (w1MaxDist > 0.0f ? w1MaxDist : w1Range))
                    && (!w1ReqResearch || simLabUpgradeUnlocked);

        if (testWeaponSelectMode == 1)
        {
            if (w0Enabled)
                weaponsToFire.push_back({w0, 0, simShotCooldown, simBurstIndex, simBurstDelayTimer});
        }
        else if (testWeaponSelectMode == 2)
        {
            if (w1Enabled)
                weaponsToFire.push_back({w1, 1, simShotCooldown2, simBurstIndex2, simBurstDelayTimer2});
        }
        else
        {
            // Auto / Both: BOTH WEAPONS ALWAYS IN ACTION SIMULTANEOUSLY!
            if (can0)
                weaponsToFire.push_back({w0, 0, simShotCooldown, simBurstIndex, simBurstDelayTimer});
            if (can1)
                weaponsToFire.push_back({w1, 1, simShotCooldown2, simBurstIndex2, simBurstDelayTimer2});
        }
    }
    else
    {
        if (const YAML::Node w0 = MainWeapon(combatNode))
        {
            weaponsToFire.push_back({w0, 0, simShotCooldown, simBurstIndex, simBurstDelayTimer});
        }
    }

    for (auto &sw : weaponsToFire)
    {
        const int burstCount = std::max(1, YamlInt(sw.node, "burst_count", 1));
        const float burstDelay = std::max(0.04f, YamlFloat(sw.node, "burst_delay", 0.1f));
        const float cooldown
            = std::max(0.15f, WeaponCooldown(sw.node, true, 0.7f)); // random mode: a new delay every cycle
        std::string projectile = YamlString(sw.node, "projectile", "bullet");

        float curMuzzleX = muzzleOffsetX;
        float curMuzzleY = muzzleOffsetY;
        if (sw.node["localePosition"] && sw.node["localePosition"].IsSequence()
            && sw.node["localePosition"].size() >= 2)
        {
            try
            {
                curMuzzleX = sw.node["localePosition"][0].as<float>(muzzleOffsetX);
                curMuzzleY = sw.node["localePosition"][1].as<float>(muzzleOffsetY);
            }
            catch (...)
            {}
        }

        float muzzleX = basePivotX;
        float muzzleY = basePivotY;
        if (!building || hasRotTurret)
        {
            muzzleX = basePivotX + s_dirDx[tf] * curMuzzleX - s_dirDy[tf] * curMuzzleY;
            muzzleY = basePivotY + s_dirDy[tf] * curMuzzleX + s_dirDx[tf] * curMuzzleY;
        }
        else
        {
            muzzleX = basePivotX + curMuzzleX;
            muzzleY = basePivotY + curMuzzleY;
        }

        if (!aimAligned)
        {
            continue;
        }

        if (sw.bIdx < burstCount)
        {
            sw.bDelay -= dt;
            if (sw.bDelay <= 0.0f)
            {
                SimProjectile p;
                p.startX = muzzleX;
                p.startY = muzzleY;
                p.curX = muzzleX;
                p.curY = muzzleY;
                float toTgtX = targetWorldX - p.startX;
                float toTgtY = targetWorldY - p.startY;
                p.totalDist = std::max(1.0f, std::hypot(toTgtX, toTgtY));

                const float normRng = OpenArtOfWar::Utility::RandomSigned();

                float maxSpread = 0.0f;
                if (sw.node["spread_angle"].IsDefined())
                {
                    float sDeg = YamlFloat(sw.node, "spread_angle", 0.0f);
                    maxSpread = std::max(0.0f, sDeg * (static_cast<float>(M_PI) / 180.0f));
                }
                else if (combatNode["spread_angle"].IsDefined())
                {
                    float sDeg = YamlFloat(combatNode, "spread_angle", 0.0f);
                    maxSpread = std::max(0.0f, sDeg * (static_cast<float>(M_PI) / 180.0f));
                }
                else
                {
                    maxSpread
                        = projectile == "none"
                              ? 0.055f
                              : OpenArtOfWar::Projectiles::of(OpenArtOfWar::Projectiles::FromKey(projectile)).spread;
                }

                if (maxSpread <= 0.0001f)
                {
                    p.targetX = targetWorldX;
                    p.targetY = targetWorldY;
                    p.dirX = toTgtX / p.totalDist;
                    p.dirY = toTgtY / p.totalDist;
                }
                else
                {
                    float baseAng = std::atan2(toTgtY, toTgtX);
                    float spreadAng = baseAng + normRng * maxSpread;
                    p.targetX = p.startX + std::cos(spreadAng) * p.totalDist;
                    p.targetY = p.startY + std::sin(spreadAng) * p.totalDist;
                    p.dirX = std::cos(spreadAng);
                    p.dirY = std::sin(spreadAng);
                }

                p.dir16 = OpenArtOfWar::Utility::DirFromNorth(p.dirX, p.dirY, 16);
                p.dir32 = OpenArtOfWar::Utility::DirFromNorth(p.dirX, p.dirY, 32);

                p.projectileType = projectile;
                if (p.projectileType == "flame")
                    p.speed = 240.0f;
                else if (p.projectileType == "rocket")
                    p.speed = 420.0f;
                else
                    p.speed = previewBulletSpeed;
                p.isInfantry = isInf;
                p.active = true;
                p.impacted = false;
                p.traveledDist = 0.0f;
                p.maxImpactTime = 0.16f / std::max(0.1f, previewFxSpeed);
                p.impactTimer = p.maxImpactTime;

                simProjectiles.push_back(p);

                sw.bIdx++;
                sw.bDelay = burstDelay;
                if (sw.bIdx >= burstCount)
                {
                    sw.cd = cooldown;
                }
            }
        }
        else
        {
            sw.cd -= dt;
            if (sw.cd <= 0.0f)
            {
                sw.bIdx = 0;
                sw.bDelay = 0.0f;
            }
        }
    }

    // Advance all active flying projectiles and impact animations
    for (auto &p : simProjectiles)
    {
        if (!p.active)
            continue;
        if (!p.impacted)
        {
            float step = p.speed * dt;
            p.traveledDist += step;
            p.curX += p.dirX * step;
            p.curY += p.dirY * step;
            if (p.traveledDist >= p.totalDist)
            {
                p.curX = p.targetX;
                p.curY = p.targetY;
                p.impacted = true;
            }
        }
        else
        {
            p.impactTimer -= dt;
            if (p.impactTimer <= 0.0f)
            {
                p.active = false;
            }
        }
    }

    simProjectiles.erase(
        std::remove_if(simProjectiles.begin(), simProjectiles.end(), [](const SimProjectile &p) { return !p.active; }),
        simProjectiles.end());
}

} // namespace CharEditor
