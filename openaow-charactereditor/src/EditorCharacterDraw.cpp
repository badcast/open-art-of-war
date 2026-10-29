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

void CharacterEditor::drawCharacter(YAML::Node doc, const VisualOffsets& vo, bool primary) {
    gameRenderer->setDrawOwner(primary ? 0 : 1);   // owner 0 wears the army color of the combo box, the opponent stays classic
    if (showGround) {
        // 5x5 ground tiles centered at world origin (0, 0)
        for (int row = -2; row <= 2; ++row) {
            for (int col = -2; col <= 2; ++col) {
                float tileX = static_cast<float>(col * OpenArtOfWar::TILE_WIDTH) - static_cast<float>(OpenArtOfWar::TILE_WIDTH) * 0.5f;
                float tileY = static_cast<float>(row * OpenArtOfWar::TILE_HEIGHT) - static_cast<float>(OpenArtOfWar::TILE_HEIGHT) * 0.5f;
                gameRenderer->drawTileWorld(0, 0, tileX, tileY, *camera);
            }
        }
        // 2px border around 5x5 ground
        gameRenderer->drawRectWorld(-75.0f, -50.0f, 150.0f, 100.0f, *camera, SDL_Color{85, 140, 105, 200});
        gameRenderer->drawRectWorld(-76.0f, -51.0f, 152.0f, 102.0f, *camera, SDL_Color{85, 140, 105, 120});
    }

    const bool building = yamlString(doc, "type") == "building";
    const OpenArtOfWar::Faction faction = docFaction(doc);
    const bool destroyed = previewHealthPercent == 0;
    const bool deathAnimation = destroyed || previewMode == PreviewMode::Die;
    const float frameClock = (loopPreview || deathAnimation) ? previewTime : 0.0f;
    const int frame = static_cast<int>(frameClock * 10.0f);

    float unitCenterX = 0.0f;
    float unitCenterY = 0.0f;
    int shownHullFacing = facing;                       // hull direction drawn this frame
    int shownArchetype = yamlInt(doc, "archetype_id", 0); // sprite set drawn (siege form included)

    const float* s_dirDx = OpenArtOfWar::Utility::kDir8X;
    const float* s_dirDy = OpenArtOfWar::Utility::kDir8Y;

    // Direction towards target from world origin
    float toMouseX = targetWorldX;
    float toMouseY = targetWorldY;
    const int aimDir8 = OpenArtOfWar::Utility::dir8FromVector(toMouseX, toMouseY);

    YAML::Node combatNode = doc["combat"];
    YAML::Node statsNode = doc["stats"];
    if (testSiegeMode && doc["abilities"] && doc["abilities"]["siege"]) {
        if (doc["abilities"]["siege"]["combat"].IsMap()) {
            combatNode = doc["abilities"]["siege"]["combat"];
        }
        if (doc["abilities"]["siege"]["stats"].IsMap()) {
            statsNode = doc["abilities"]["siege"]["stats"];
        }
    }

    const bool hasRotTurret = yamlBool(combatNode, "has_rotating_turret", false);
    const bool isInf = !building && [&]() {
        const int arch = yamlInt(doc, "archetype_id", 0);
        return OpenArtOfWar::Utility::isInfantryArchetype(arch);
    }();

    if (building) {
        const int buildingIndex = yamlInt(doc, "building_index", -1);
        const YAML::Node size = doc["size"];
        const int width = yamlInt(size, "width", 1);
        const int height = yamlInt(size, "height", 1);
        const float footprintX = -static_cast<float>(width * OpenArtOfWar::TILE_WIDTH) * 0.5f;
        const float footprintY = -static_cast<float>(height * OpenArtOfWar::TILE_HEIGHT) * 0.5f;
        unitCenterX = 0.0f;
        unitCenterY = 0.0f;

        // Draw building footprint rectangle in world space
        gameRenderer->fillRectWorld(footprintX, footprintY,
                                    static_cast<float>(width * OpenArtOfWar::TILE_WIDTH),
                                    static_cast<float>(height * OpenArtOfWar::TILE_HEIGHT),
                                    *camera, SDL_Color{91, 153, 123, 40});
        gameRenderer->drawRectWorld(footprintX, footprintY,
                                    static_cast<float>(width * OpenArtOfWar::TILE_WIDTH),
                                    static_cast<float>(height * OpenArtOfWar::TILE_HEIGHT),
                                    *camera, SDL_Color{118, 176, 146, 180});

        if (buildingIndex >= 0) {
            bool attacking = false;
            if (!destroyed && previewMode == PreviewMode::Attack &&
                yamlBool(combatNode, "can_attack", true)) {
                const YAML::Node gun = mainWeapon(combatNode);
                const int burstCount = std::max(1, yamlInt(gun, "burst_count", 1));
                const float burstDelay = std::max(0.04f, yamlFloat(gun, "burst_delay", 0.1f));
                const float cooldown = std::max(0.15f, weaponCooldown(gun, false, 0.7f));
                const float burstSpan = static_cast<float>(burstCount - 1) * burstDelay;
                const float period = std::max(0.35f, burstSpan + cooldown);
                const float phase = std::fmod(previewTime, period);
                for (int k = 0; k < burstCount; ++k) {
                    float fireTime = static_cast<float>(k) * burstDelay;
                    float dt = phase - fireTime;
                    if (dt >= 0.0f && dt < 0.16f) {
                        attacking = true;
                        break;
                    }
                }
            }
            const int bTurretFacing = (previewMode == PreviewMode::Attack) ? aimDir8 : (hasRotTurret ? turretFacingDir : facing);
            int wreckComp = 0, wreckStages = 0;
            wreckOf(doc, wreckComp, wreckStages);

            if (previewMode == PreviewMode::Wreck) {
                gameRenderer->drawBuildingWorld(buildingIndex, footprintX, footprintY, 0, *camera, faction,
                                                currentWreckStage(wreckStages), 1.0f, true, 1.0f, 0, false, true,
                                                wreckComp, wreckStages);
            } else if (destroyed) {
                // Destruction sequence: explosion starts, and from the middle of destruction the crater appears
                int expAnim = (width <= 2 && height <= 2) ? 37 : 32;
                int expFrames = (expAnim == 37) ? 6 : 8;
                float expDur = expFrames * 0.08f;
                bool showRemnant = (previewTime >= expDur * 0.45f);

                gameRenderer->drawBuildingWorld(buildingIndex, footprintX, footprintY, 0, *camera, faction,
                                                showRemnant ? 0 : frame, buildingProgress, showRemnant,
                                                static_cast<float>(previewHealthPercent) / 100.0f,
                                                bTurretFacing, false, buildingPowered, wreckComp, wreckStages);
                gameRenderer->drawBuildingWorld(buildingIndex, footprintX, footprintY, 1, *camera, faction,
                                                frame, buildingProgress, showRemnant,
                                                static_cast<float>(previewHealthPercent) / 100.0f,
                                                bTurretFacing, false, buildingPowered);

                if (previewTime < expDur) {
                    int expFrame = std::clamp(static_cast<int>(previewTime / 0.08f), 0, expFrames - 1);
                    gameRenderer->drawAnimSequenceWorld(expAnim, expFrame, -16.0f, -16.0f, *camera, faction);
                }
            } else {
                gameRenderer->drawBuildingWorld(buildingIndex, footprintX, footprintY, 0, *camera, faction,
                                                frame, buildingProgress, false,
                                                static_cast<float>(previewHealthPercent) / 100.0f,
                                                bTurretFacing, attacking, buildingPowered);
                gameRenderer->drawBuildingWorld(buildingIndex, footprintX, footprintY, 1, *camera, faction,
                                                frame, buildingProgress, false,
                                                static_cast<float>(previewHealthPercent) / 100.0f,
                                                bTurretFacing, attacking, buildingPowered);
            }
        } else {
            gameRenderer->drawLineWorld(-24.0f, -24.0f, 24.0f, 24.0f, *camera, SDL_Color{255, 160, 120, 255});
            gameRenderer->drawLineWorld(24.0f, -24.0f, -24.0f, 24.0f, *camera, SDL_Color{255, 160, 120, 255});
        }
    } else {
        // Unit drawing: tile top-left is (-15, -10) so center is at (0, 0)
        float unitX = -static_cast<float>(OpenArtOfWar::TILE_WIDTH) * 0.5f;
        float unitY = -static_cast<float>(OpenArtOfWar::TILE_HEIGHT) * 0.5f;
        int unitFacing = facing;

        if (previewMode == PreviewMode::Move && !destroyed) {
            const MoveSample m = sampleMove(yamlInt(doc, "archetype_id", 0), yamlFloat(statsNode, "move_speed", 0.0f), previewTime);
            unitX += m.offsetX;
            unitFacing = m.facing;
        } else if (!hasRotTurret && previewMode == PreviewMode::Attack) {
            unitFacing = targetCustomized ? aimDir8 : facing;
        }

        unitCenterX = unitX + static_cast<float>(OpenArtOfWar::TILE_WIDTH) * 0.5f;
        unitCenterY = unitY + static_cast<float>(OpenArtOfWar::TILE_HEIGHT) * 0.5f;

        int archetypeId = yamlInt(doc, "archetype_id", 0);
        if (testSiegeMode) {
            int sArch = -1;
            if (doc["abilities"]) {
                if (doc["abilities"]["siege"] && doc["abilities"]["siege"].IsMap()) {
                    sArch = yamlInt(doc["abilities"]["siege"], "archetype_id", -1);
                }
                if (sArch < 0) {
                    sArch = yamlInt(doc["abilities"], "siege_archetype_id", -1);
                }
            }
            if (sArch < 0) {
                if (archetypeId == 10) sArch = 15; // Rhino Siege
                else if (archetypeId == 8) sArch = 16; // Grad Deployed
                else if (archetypeId == 4) sArch = 4; // Sniper
            }
            if (sArch >= 0) archetypeId = sArch;
        }

        int action = static_cast<int>(OpenArtOfWar::ActionState::Idle);
        // Driving without a target the turret follows the hull, as in the game
        const int turretFacing = (previewMode == PreviewMode::Attack) ? (targetCustomized ? aimDir8 : turretFacingDir) :
                                 (previewMode == PreviewMode::Move || !hasRotTurret) ? unitFacing : turretFacingDir;
        shownHullFacing = unitFacing;
        shownArchetype = archetypeId;

        if (previewMode == PreviewMode::Wreck) {
            // Remains only: vehicles sink stage by stage, infantry leaves its corpse
            int wreckComp = 0, wreckStages = 0;
            wreckOf(doc, wreckComp, wreckStages);
            const int stage = currentWreckStage(wreckStages);
            if (isInf) {
                const YAML::Node seqs = doc["animations"]["death_sequences"];
                const int corpseSeq = (seqs && seqs.IsSequence() && seqs.size() > 0) ? seqs[0].as<int>(0) : 0;
                gameRenderer->drawUnitWorld(archetypeId, static_cast<int>(OpenArtOfWar::ActionState::Die), unitFacing, 99,
                                            unitX, unitY, *camera, faction, turretFacing, testFlameDeath, 0, 0, corpseSeq);
            } else {
                gameRenderer->drawUnitWorld(archetypeId, static_cast<int>(OpenArtOfWar::ActionState::Die), unitFacing, stage,
                                            unitX, unitY, *camera, faction, turretFacing, true, wreckComp, wreckStages);
            }
        } else if (destroyed) {
            action = static_cast<int>(OpenArtOfWar::ActionState::Die);
            if (!isInf) {
                int expAnim = 11;
                int expFrames = 8;
                float expDur = expFrames * 0.08f;
                bool showWreck = (previewTime >= expDur * 0.45f);

                if (showWreck) {
                    int wreckComp = 0, wreckStages = 0;
                    wreckOf(doc, wreckComp, wreckStages);
                    gameRenderer->drawUnitWorld(archetypeId, action, unitFacing, 0,
                                                unitX, unitY, *camera, faction, turretFacing, true, wreckComp, wreckStages);
                } else {
                    gameRenderer->drawUnitWorld(archetypeId, static_cast<int>(OpenArtOfWar::ActionState::Idle),
                                                unitFacing, frame, unitX, unitY, *camera, faction, turretFacing, true);
                }

                if (previewTime < expDur) {
                    int expFrame = std::clamp(static_cast<int>(previewTime / 0.08f), 0, expFrames - 1);
                    gameRenderer->drawAnimSequenceWorld(expAnim, expFrame, -16.0f, -16.0f, *camera, faction);
                }
            } else {
                gameRenderer->drawUnitWorld(archetypeId, action, unitFacing, frame,
                                            unitX, unitY, *camera, faction, turretFacing, testFlameDeath);
            }
        } else if (previewMode == PreviewMode::Move) {
            action = static_cast<int>(OpenArtOfWar::ActionState::Move);
            gameRenderer->drawUnitWorld(archetypeId, action, unitFacing, frame,
                                        unitX, unitY, *camera, faction, turretFacing, true);
        } else if (previewMode == PreviewMode::Attack && yamlBool(combatNode, "can_attack", true)) {
            const YAML::Node gun = mainWeapon(combatNode);
            const int burstCount = std::max(1, yamlInt(gun, "burst_count", 1));
            const float burstDelay = std::max(0.04f, yamlFloat(gun, "burst_delay", 0.1f));
            const float cooldown = std::max(0.15f, weaponCooldown(gun, false, 0.7f));
            const float burstSpan = static_cast<float>(burstCount - 1) * burstDelay;
            const float period = std::max(0.35f, burstSpan + cooldown);
            const float phase = std::fmod(previewTime, period);
            // Time since the latest shot: the game restarts the firing animation on every shot
            float sinceShot = phase;
            for (int k = 0; k < burstCount; ++k) {
                const float fireTime = static_cast<float>(k) * burstDelay;
                if (phase >= fireTime) sinceShot = phase - fireTime;
            }
            const bool firing = sinceShot < 0.16f;
            // Infantry keeps its crouched firing pose for the whole engagement (no standing up between
            // shots), vehicles show the firing layers only right after a shot - both exactly as in the game
            action = (firing || isInf) ? static_cast<int>(OpenArtOfWar::ActionState::Attack)
                                       : static_cast<int>(OpenArtOfWar::ActionState::Idle);
            const float stepDelay = (archetypeId == 7 || archetypeId == 18) ? 0.04f : 0.07f;
            const int attackFrame = static_cast<int>(sinceShot / stepDelay);
            gameRenderer->drawUnitWorld(archetypeId, action, unitFacing, attackFrame,
                                        unitX, unitY, *camera, faction, turretFacing, true);
        } else {
            action = previewAction();
            bool flaming = (action == static_cast<int>(OpenArtOfWar::ActionState::Die)) ? testFlameDeath : true;
            gameRenderer->drawUnitWorld(archetypeId, action, unitFacing, frame,
                                        unitX, unitY, *camera, faction, turretFacing, flaming);
        }
    }

    // Active firing weapon resolution
    YAML::Node activeWeaponNode;
    int activeWeaponIdx = 0;
    float distTiles = std::hypot(targetWorldX, targetWorldY) / static_cast<float>(OpenArtOfWar::TILE_WIDTH);

    if (combatNode["weapons"] && combatNode["weapons"].IsSequence() && combatNode["weapons"].size() >= 2) {
        bool useW2 = false;
        YAML::Node w2 = combatNode["weapons"][1];
        bool w2Enabled = yamlBool(w2, "enabled", true);
        bool w2ReqResearch = yamlBool(w2, "requires_research", false);
        float w2MaxDist = yamlFloat(w2, "max_distance", 1.5f);

        if (testWeaponSelectMode == 2) {
            useW2 = w2Enabled;
        } else if (testWeaponSelectMode == 1) {
            useW2 = false;
        } else {
            if (w2Enabled && (!w2ReqResearch || simLabUpgradeUnlocked)) {
                if (w2MaxDist <= 0.0f || distTiles <= w2MaxDist) {
                    useW2 = true;
                }
            }
        }

        if (useW2) {
            activeWeaponIdx = 1;
            activeWeaponNode = w2;
        } else {
            activeWeaponIdx = 0;
            activeWeaponNode = combatNode["weapons"][0];
        }
    } else {
        int arch = yamlInt(doc, "archetype_id", 0);
        if ((arch == 7 || arch == 18) && distTiles <= 1.5f && simLabUpgradeUnlocked && testWeaponSelectMode != 1) {
            activeWeaponIdx = 1;
        }
    }
    if (primary) activeFiringWeaponIndex = activeWeaponIdx;

    // Sight and Attack Radius Circles (2px thickness)
    float attackRangeTiles = 0.0f;
    try {
        if (activeWeaponNode.IsDefined() && activeWeaponNode["range"]) {
            attackRangeTiles = activeWeaponNode["range"].as<float>(0.0f);
        } else if (const YAML::Node w0 = mainWeapon(combatNode); w0 && w0["range"]) {
            attackRangeTiles = w0["range"].as<float>(0.0f);
        }
    } catch (...) {}

    float sightRangeTiles = 0.0f;
    try {
        if (statsNode && statsNode["sight_radius"]) {
            sightRangeTiles = statsNode["sight_radius"].as<float>(0.0f);
        }
    } catch (...) {}

    // Green Sight Radius (2px line width)
    if (showSightRange && sightRangeTiles > 0.0f) {
        drawRadiusCircle2px(unitCenterX, unitCenterY,
                            sightRangeTiles * static_cast<float>(OpenArtOfWar::TILE_WIDTH),
                            SDL_Color{50, 230, 80, 240});
    }

    // Red Attack Radius (2px line width)
    if (showAttackRange && attackRangeTiles > 0.0f) {
        drawRadiusCircle2px(unitCenterX, unitCenterY,
                            attackRangeTiles * static_cast<float>(OpenArtOfWar::TILE_WIDTH),
                            SDL_Color{255, 60, 60, 240});
    }

    // Projectile exit point calculation (localePosition active for ALL entities)
    float basePivotX = unitCenterX;
    float basePivotY = unitCenterY;
    int activeFacing = facing;

    if (hasRotTurret) {
        // The turret sits elsewhere on the hull for every hull direction: follow the sprite like the game
        float shiftX = 0.0f, shiftY = 0.0f;
        if (!building) turretShift(shownArchetype, shownHullFacing, shiftX, shiftY);
        basePivotX += vo.turretX + shiftX;
        basePivotY += vo.turretY + shiftY;
        activeFacing = (previewMode == PreviewMode::Attack) ? (targetCustomized ? aimDir8 : turretFacingDir)
                     : (previewMode == PreviewMode::Move ? shownHullFacing : turretFacingDir);
    } else {
        if (!building && isInf) {
            basePivotY -= 4.0f; // Infantry weapon height
        }
        activeFacing = (previewMode == PreviewMode::Attack) ? (targetCustomized ? aimDir8 : facing) : shownHullFacing;
    }
    if (primary) {
        simHullFacing = shownHullFacing;
        simArchetype = shownArchetype;
    }

    float curMuzzleX = vo.muzzleX;
    float curMuzzleY = vo.muzzleY;
    if (activeWeaponNode.IsDefined() && activeWeaponNode["localePosition"] &&
        activeWeaponNode["localePosition"].IsSequence() && activeWeaponNode["localePosition"].size() >= 2) {
        try {
            curMuzzleX = activeWeaponNode["localePosition"][0].as<float>(vo.muzzleX);
            curMuzzleY = activeWeaponNode["localePosition"][1].as<float>(vo.muzzleY);
        } catch (...) {}
    }

    const int tf = (activeFacing % 8 + 8) % 8;
    float muzzleX = basePivotX;
    float muzzleY = basePivotY;
    if (!building || hasRotTurret) {
        muzzleX = basePivotX + s_dirDx[tf] * curMuzzleX - s_dirDy[tf] * curMuzzleY;
        muzzleY = basePivotY + s_dirDy[tf] * curMuzzleX + s_dirDx[tf] * curMuzzleY;
    } else {
        muzzleX = basePivotX + curMuzzleX;
        muzzleY = basePivotY + curMuzzleY;
    }

    // Draw visual Pivot dot, barrel connection line, and yellow/orange Muzzle Crosshair in ALL preview modes!
    if (!destroyed) {
        // Center pivot indicator (cyan dot)
        OpenArtOfWar::RenderUtility::DrawRadius(*gameRenderer, *camera, basePivotX, basePivotY, 2.0f, SDL_Color{80, 220, 255, 220}, 1.5f);
        // Line from pivot to muzzle exit point
        gameRenderer->drawLineWorld(basePivotX, basePivotY, muzzleX, muzzleY, *camera,
                                    activeFiringWeaponIndex == 1 ? SDL_Color{255, 140, 50, 220} : SDL_Color{255, 200, 50, 180});

        // Visual muzzle exit point marker (localePosition)
        SDL_Color muzzleCol = (activeFiringWeaponIndex == 1) ? SDL_Color{255, 120, 30, 255} : SDL_Color{255, 230, 40, 240};
        OpenArtOfWar::RenderUtility::DrawRadius(*gameRenderer, *camera, muzzleX, muzzleY, 3.5f, muzzleCol, 1.5f);
        gameRenderer->drawLineWorld(muzzleX - 4.0f, muzzleY, muzzleX + 4.0f, muzzleY, *camera, muzzleCol);
        gameRenderer->drawLineWorld(muzzleX, muzzleY - 4.0f, muzzleX, muzzleY + 4.0f, *camera, muzzleCol);
    }

    // Projectile & Combat FX (Burst Simulation) - only the edited character runs the projectile sim
    if (primary && !destroyed && previewMode == PreviewMode::Attack &&
        yamlBool(combatNode, "can_attack", true)) {

        // Draw line of fire toward directed target point
        gameRenderer->drawLineWorld(muzzleX, muzzleY, targetWorldX, targetWorldY,
                                    *camera, SDL_Color{236, 111, 77, 90});

        // Muzzle flash on each shot
        const bool ownFlame = isInf && yamlString(mainWeapon(combatNode), "projectile", "bullet") == "flame";
        if (muzzleFlashTimer > 0.0f && !ownFlame) { // the flamethrower's own firing layer is its nozzle flame
            float flashDur = 0.08f / std::max(0.1f, previewFxSpeed);
            int mFrame = std::clamp(static_cast<int>((1.0f - muzzleFlashTimer / flashDur) * 3.0f), 0, 2);
            gameRenderer->drawAnimSequenceWorld(42, mFrame, muzzleFlashX, muzzleFlashY, *camera, faction);
        }

        // Projectiles and impact effects in flight
        // Each projectile flies strictly to its own locked target from the moment of firing!
        for (const auto& p : simProjectiles) {
            if (!p.impacted) {
                if (p.projectileType == "flame") {
                    // Same as the game: the stream is anchored at the nozzle and plays its 12 frames
                    // while the flame travels to the target
                    float angle = std::atan2(p.targetY - p.startY, p.targetX - p.startX);
                    const float prog = std::clamp(p.traveledDist / std::max(1.0f, p.totalDist), 0.0f, 1.0f);
                    const int flameFrame = std::clamp(static_cast<int>(prog * 11.0f), 0, 11);
                    gameRenderer->drawAnimSequenceWorldRotated(59, flameFrame, p.startX, p.startY, angle, *camera, faction);
                } else if (p.projectileType == "rocket") {
                    gameRenderer->drawProjectileWorld(1405, p.dir16, p.curX, p.curY, *camera, faction, 16);
                } else if (p.projectileType == "shell") {
                    gameRenderer->drawProjectileWorld(1341, p.dir32, p.curX, p.curY, *camera, faction, 32);
                } else if (p.projectileType != "none") {
                    gameRenderer->drawProjectileWorld(1309, p.dir32, p.curX, p.curY, *camera, faction, 32);
                }
            } else {
                float impactProgress = 1.0f - (p.impactTimer / p.maxImpactTime);
                int impactFrame = std::clamp(static_cast<int>(impactProgress * 4.0f), 0, 3);
                int impactAnim = 42;
                float offX = -16.0f;
                float offY = -8.0f;
                if (p.projectileType == "rocket") {
                    impactAnim = 18;
                    offX = -16.0f;
                    offY = -2.0f;
                } else if (p.projectileType == "shell") {
                    impactAnim = 15;
                    offX = -16.0f;
                    offY = 0.0f;
                } else if (p.projectileType == "flame") {
                    impactAnim = 59;
                    offX = 0.0f;
                    offY = 0.0f;
                } else { // bullet / other
                    impactAnim = 55; // the same impact sparks as in the game, whatever was hit
                    offX = -16.0f;
                    offY = -6.0f;
                }
                if (p.projectileType == "flame") {
                    // The game draws no impact for flames: the stream itself ends at the target
                } else {
                    gameRenderer->drawAnimSequenceWorld(impactAnim, impactFrame,
                                                        p.targetX + offX, p.targetY + offY, *camera, faction);
                }
            }
        }
    }

}

void CharacterEditor::drawPreviewGrid() {
    float leftWorld = 0.0f;
    float topWorld = 0.0f;
    float rightWorld = 0.0f;
    float bottomWorld = 0.0f;
    camera->screenToWorld(static_cast<float>(previewRect.x), static_cast<float>(previewRect.y),
                          leftWorld, topWorld);
    camera->screenToWorld(static_cast<float>(previewRect.x + previewRect.w),
                          static_cast<float>(previewRect.y + previewRect.h), rightWorld, bottomWorld);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 40, 51, 49, 120);
    const int firstColumn = static_cast<int>(std::floor(leftWorld / OpenArtOfWar::TILE_WIDTH));
    const int lastColumn = static_cast<int>(std::ceil(rightWorld / OpenArtOfWar::TILE_WIDTH));
    for (int column = firstColumn; column <= lastColumn; ++column) {
        float screenX = 0.0f;
        float screenY = 0.0f;
        camera->worldToScreen(static_cast<float>(column * OpenArtOfWar::TILE_WIDTH), topWorld,
                              screenX, screenY);
        SDL_RenderLine(renderer, screenX, static_cast<float>(previewRect.y),
                            screenX, static_cast<float>(previewRect.y + previewRect.h));
    }

    const int firstRow = static_cast<int>(std::floor(topWorld / OpenArtOfWar::TILE_HEIGHT));
    const int lastRow = static_cast<int>(std::ceil(bottomWorld / OpenArtOfWar::TILE_HEIGHT));
    for (int row = firstRow; row <= lastRow; ++row) {
        float screenX = 0.0f;
        float screenY = 0.0f;
        camera->worldToScreen(leftWorld, static_cast<float>(row * OpenArtOfWar::TILE_HEIGHT),
                              screenX, screenY);
        SDL_RenderLine(renderer, static_cast<float>(previewRect.x), screenY,
                            static_cast<float>(previewRect.x + previewRect.w), screenY);
    }
}

void CharacterEditor::updateCombatSim(float dt) {
    if (!document.IsDefined() || previewMode != PreviewMode::Attack) {
        simProjectiles.clear();
        simShotCooldown = 0.0f;
        simBurstIndex = 0;
        simBurstDelayTimer = 0.0f;
        simShotCooldown2 = 0.0f;
        simBurstIndex2 = 0;
        simBurstDelayTimer2 = 0.0f;
        muzzleFlashTimer = 0.0f;
        return;
    }

    YAML::Node combatNode = document["combat"];
    if (testSiegeMode && document["abilities"] && document["abilities"]["siege"] &&
        document["abilities"]["siege"]["combat"].IsMap()) {
        combatNode = document["abilities"]["siege"]["combat"];
    }
    if (!yamlBool(combatNode, "can_attack", true)) {
        simProjectiles.clear();
        return;
    }

    const bool building = (yamlString(document, "type") == "building");
    const bool isInf = !building && [&]() {
        const int arch = yamlInt(document, "archetype_id", 0);
        return OpenArtOfWar::Utility::isInfantryArchetype(arch);
    }();
    const bool hasRotTurret = yamlBool(combatNode, "has_rotating_turret", false);

    const int aimDir8 = OpenArtOfWar::Utility::dir8FromVector(targetWorldX, targetWorldY);
    const float* s_dirDx = OpenArtOfWar::Utility::kDir8X;
    const float* s_dirDy = OpenArtOfWar::Utility::kDir8Y;
    float tPivotX = 0.0f, tPivotY = 0.0f;
    if (hasRotTurret) {
        float shiftX = 0.0f, shiftY = 0.0f;
        if (!building) turretShift(simArchetype, simHullFacing, shiftX, shiftY);
        tPivotX = turretOffsetX + shiftX;
        tPivotY = turretOffsetY + shiftY;
    }

    float basePivotX = 0.0f;
    float basePivotY = 0.0f;
    if (hasRotTurret) {
        basePivotX = tPivotX;
        basePivotY = tPivotY;
    } else {
        if (!building && isInf) {
            basePivotY = -4.0f;
        }
    }

    const int activeSimFacing = hasRotTurret ? (targetCustomized ? aimDir8 : turretFacingDir) :
                                               (targetCustomized ? aimDir8 : facing);
    const int tf = (activeSimFacing % 8 + 8) % 8;

    if (muzzleFlashTimer > 0.0f) {
        muzzleFlashTimer -= dt;
    }

    float distTiles = std::hypot(targetWorldX, targetWorldY) / static_cast<float>(OpenArtOfWar::TILE_WIDTH);

    struct SimWeaponUnit {
        YAML::Node node;
        int idx{0};
        float& cd;
        int& bIdx;
        float& bDelay;
    };
    std::vector<SimWeaponUnit> weaponsToFire;

    if (combatNode["weapons"] && combatNode["weapons"].IsSequence() && combatNode["weapons"].size() >= 2) {
        YAML::Node w0 = combatNode["weapons"][0];
        YAML::Node w1 = combatNode["weapons"][1];
        bool w0Enabled = yamlBool(w0, "enabled", true);
        bool w1Enabled = yamlBool(w1, "enabled", true);
        bool w1ReqResearch = yamlBool(w1, "requires_research", false);
        float w0Range = yamlFloat(w0, "range", 5.0f);
        float w1Range = yamlFloat(w1, "range", 1.5f);
        float w1MaxDist = yamlFloat(w1, "max_distance", 1.5f);

        bool can0 = w0Enabled && (distTiles <= w0Range);
        bool can1 = w1Enabled && (distTiles <= (w1MaxDist > 0.0f ? w1MaxDist : w1Range)) &&
                    (!w1ReqResearch || simLabUpgradeUnlocked);

        if (testWeaponSelectMode == 1) {
            if (w0Enabled) weaponsToFire.push_back({w0, 0, simShotCooldown, simBurstIndex, simBurstDelayTimer});
        } else if (testWeaponSelectMode == 2) {
            if (w1Enabled) weaponsToFire.push_back({w1, 1, simShotCooldown2, simBurstIndex2, simBurstDelayTimer2});
        } else {
            // Auto / Both: BOTH WEAPONS ALWAYS IN ACTION SIMULTANEOUSLY!
            if (can0) weaponsToFire.push_back({w0, 0, simShotCooldown, simBurstIndex, simBurstDelayTimer});
            if (can1) weaponsToFire.push_back({w1, 1, simShotCooldown2, simBurstIndex2, simBurstDelayTimer2});
        }
    } else {
        if (const YAML::Node w0 = mainWeapon(combatNode)) {
            weaponsToFire.push_back({w0, 0, simShotCooldown, simBurstIndex, simBurstDelayTimer});
        }
    }

    for (auto& sw : weaponsToFire) {
        const int burstCount = std::max(1, yamlInt(sw.node, "burst_count", 1));
        const float burstDelay = std::max(0.04f, yamlFloat(sw.node, "burst_delay", 0.1f));
        const float cooldown = std::max(0.15f, weaponCooldown(sw.node, true, 0.7f)); // random mode: a new delay every cycle
        std::string projectile = yamlString(sw.node, "projectile", "bullet");

        float curMuzzleX = muzzleOffsetX;
        float curMuzzleY = muzzleOffsetY;
        if (sw.node["localePosition"] && sw.node["localePosition"].IsSequence() && sw.node["localePosition"].size() >= 2) {
            try {
                curMuzzleX = sw.node["localePosition"][0].as<float>(muzzleOffsetX);
                curMuzzleY = sw.node["localePosition"][1].as<float>(muzzleOffsetY);
            } catch (...) {}
        }

        float muzzleX = basePivotX;
        float muzzleY = basePivotY;
        if (!building || hasRotTurret) {
            muzzleX = basePivotX + s_dirDx[tf] * curMuzzleX - s_dirDy[tf] * curMuzzleY;
            muzzleY = basePivotY + s_dirDy[tf] * curMuzzleX + s_dirDx[tf] * curMuzzleY;
        } else {
            muzzleX = basePivotX + curMuzzleX;
            muzzleY = basePivotY + curMuzzleY;
        }

        if (sw.bIdx < burstCount) {
            sw.bDelay -= dt;
            if (sw.bDelay <= 0.0f) {
                SimProjectile p;
                p.startX = muzzleX;
                p.startY = muzzleY;
                p.curX = muzzleX;
                p.curY = muzzleY;
                float toTgtX = targetWorldX - p.startX;
                float toTgtY = targetWorldY - p.startY;
                p.totalDist = std::max(1.0f, std::hypot(toTgtX, toTgtY));

                const float normRng = OpenArtOfWar::Utility::randomSigned();

                float maxSpread = 0.0f;
                if (sw.node["spread_angle"].IsDefined()) {
                    float sDeg = yamlFloat(sw.node, "spread_angle", 0.0f);
                    maxSpread = std::max(0.0f, sDeg * (static_cast<float>(M_PI) / 180.0f));
                } else if (combatNode["spread_angle"].IsDefined()) {
                    float sDeg = yamlFloat(combatNode, "spread_angle", 0.0f);
                    maxSpread = std::max(0.0f, sDeg * (static_cast<float>(M_PI) / 180.0f));
                } else {
                    if (projectile == "bullet") maxSpread = 0.065f;
                    else if (projectile == "shell") maxSpread = 0.035f;
                    else if (projectile == "rocket") maxSpread = 0.080f;
                    else if (projectile == "flame") maxSpread = 0.120f;
                    else maxSpread = 0.055f;
                }

                if (maxSpread <= 0.0001f) {
                    p.targetX = targetWorldX;
                    p.targetY = targetWorldY;
                    p.dirX = toTgtX / p.totalDist;
                    p.dirY = toTgtY / p.totalDist;
                } else {
                    float baseAng = std::atan2(toTgtY, toTgtX);
                    float spreadAng = baseAng + normRng * maxSpread;
                    p.targetX = p.startX + std::cos(spreadAng) * p.totalDist;
                    p.targetY = p.startY + std::sin(spreadAng) * p.totalDist;
                    p.dirX = std::cos(spreadAng);
                    p.dirY = std::sin(spreadAng);
                }

                p.dir16 = OpenArtOfWar::Utility::dirFromNorth(p.dirX, p.dirY, 16);
                p.dir32 = OpenArtOfWar::Utility::dirFromNorth(p.dirX, p.dirY, 32);

                p.projectileType = projectile;
                if (p.projectileType == "flame") p.speed = 240.0f;
                else if (p.projectileType == "rocket") p.speed = 420.0f;
                else p.speed = previewBulletSpeed;
                p.isInfantry = isInf;
                p.active = true;
                p.impacted = false;
                p.traveledDist = 0.0f;
                p.maxImpactTime = 0.16f / std::max(0.1f, previewFxSpeed);
                p.impactTimer = p.maxImpactTime;

                simProjectiles.push_back(p);

                muzzleFlashX = muzzleX;
                muzzleFlashY = muzzleY;
                muzzleFlashTimer = 0.08f / std::max(0.1f, previewFxSpeed);

                sw.bIdx++;
                sw.bDelay = burstDelay;
                if (sw.bIdx >= burstCount) {
                    sw.cd = cooldown;
                }
            }
        } else {
            sw.cd -= dt;
            if (sw.cd <= 0.0f) {
                sw.bIdx = 0;
                sw.bDelay = 0.0f;
            }
        }
    }

    // Advance all active flying projectiles and impact animations
    for (auto& p : simProjectiles) {
        if (!p.active) continue;
        if (!p.impacted) {
            float step = p.speed * dt;
            p.traveledDist += step;
            p.curX += p.dirX * step;
            p.curY += p.dirY * step;
            if (p.traveledDist >= p.totalDist) {
                p.curX = p.targetX;
                p.curY = p.targetY;
                p.impacted = true;
            }
        } else {
            p.impactTimer -= dt;
            if (p.impactTimer <= 0.0f) {
                p.active = false;
            }
        }
    }

    simProjectiles.erase(std::remove_if(simProjectiles.begin(), simProjectiles.end(),
                         [](const SimProjectile& p) { return !p.active; }),
                         simProjectiles.end());
}

} // namespace CharEditor
