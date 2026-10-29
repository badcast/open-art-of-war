#include "graphics/RenderUtility.hpp"
#include "core/EnvironmentConfig.hpp"
#include "game/GameplayController.hpp"
#include "game/Players.hpp"
#include "core/DetMath.hpp"
#include "game/Sides.hpp"
#include "core/Localization.hpp"
#include "ui/HUDController.hpp"
#include "ui/UiKit.hpp"
#include "ui/MenuController.hpp"
#include "input/InputManager.hpp"
#include "graphics/Camera.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "game/TileMap.hpp"
#include "game/Pathfinder.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/AIController.hpp"
#include "game/MissionController.hpp"
#include "audio/SoundEngine.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>

namespace OpenArtOfWar
{

    enum class DebugSpawnCategory
    {
        Humans,
        Vehicles,
        Buildings
    };

    struct DebugSpawnItem
    {
        std::string name;
        DebugSpawnCategory category;
        Faction faction;
        bool isBuilding;
        bool isWall;
        int archetypeOrBuildingIdx;
        int width {1};
        int height {1};
    };

    static const std::vector<DebugSpawnItem> &getDebugSpawnCatalog()
    {
        static const std::vector<DebugSpawnItem> s_catalog = {
            // HUMANS (Infantry)
            {"Rifleman (Res)", DebugSpawnCategory::Humans, Faction::Resistance, false, false, 0, 1, 1},
            {"Grenadier (Res)", DebugSpawnCategory::Humans, Faction::Resistance, false, false, 2, 1, 1},
            {"Sniper (Res)", DebugSpawnCategory::Humans, Faction::Resistance, false, false, 4, 1, 1},
            {"Rifleman (Conf)", DebugSpawnCategory::Humans, Faction::Confederation, false, false, 1, 1, 1},
            {"Heavy Trooper (Conf)", DebugSpawnCategory::Humans, Faction::Confederation, false, false, 3, 1, 1},
            {"Flamethrower (Conf)", DebugSpawnCategory::Humans, Faction::Confederation, false, false, 5, 1, 1},

            // VEHICLES (Armor)
            {"Coyote (Res)", DebugSpawnCategory::Vehicles, Faction::Resistance, false, false, 6, 1, 1},
            {"Armadillo (Res)", DebugSpawnCategory::Vehicles, Faction::Resistance, false, false, 8, 1, 1},
            {"Rhino (Res)", DebugSpawnCategory::Vehicles, Faction::Resistance, false, false, 10, 1, 1},
            {"Porcupine MLRS (Res)", DebugSpawnCategory::Vehicles, Faction::Resistance, false, false, 12, 1, 1},
            {"TM-40 Fortress (Conf)", DebugSpawnCategory::Vehicles, Faction::Confederation, false, false, 7, 1, 1},
            {"T-21 Hammer (Conf)", DebugSpawnCategory::Vehicles, Faction::Confederation, false, false, 9, 1, 1},
            {"T-22 Zeus (Conf)", DebugSpawnCategory::Vehicles, Faction::Confederation, false, false, 11, 1, 1},
            {"Grad MLRS (Conf)", DebugSpawnCategory::Vehicles, Faction::Confederation, false, false, 13, 1, 1},

            // BUILDINGS
            {"HQ (Res)", DebugSpawnCategory::Buildings, Faction::Resistance, true, false, 8, 3, 4},
            {"Power Plant (Res)", DebugSpawnCategory::Buildings, Faction::Resistance, true, false, 9, 3, 3},
            {"Barracks (Res)", DebugSpawnCategory::Buildings, Faction::Resistance, true, false, 10, 3, 3},
            {"Factory (Res)", DebugSpawnCategory::Buildings, Faction::Resistance, true, false, 11, 3, 3},
            {"Research Lab (Res)", DebugSpawnCategory::Buildings, Faction::Resistance, true, false, 12, 3, 3},
            {"Bunker (Res)", DebugSpawnCategory::Buildings, Faction::Resistance, true, false, 13, 2, 2},
            {"Guard Tower (Res)", DebugSpawnCategory::Buildings, Faction::Resistance, true, false, 14, 2, 2},
            {"Wall (Res)", DebugSpawnCategory::Buildings, Faction::Resistance, false, true, 25, 1, 1},
            {"HQ (Conf)", DebugSpawnCategory::Buildings, Faction::Confederation, true, false, 0, 4, 4},
            {"Power Plant (Conf)", DebugSpawnCategory::Buildings, Faction::Confederation, true, false, 1, 3, 3},
            {"Barracks (Conf)", DebugSpawnCategory::Buildings, Faction::Confederation, true, false, 2, 3, 3},
            {"Factory (Conf)", DebugSpawnCategory::Buildings, Faction::Confederation, true, false, 3, 3, 4},
            {"Research Lab (Conf)", DebugSpawnCategory::Buildings, Faction::Confederation, true, false, 4, 3, 3},
            {"Bunker (Conf)", DebugSpawnCategory::Buildings, Faction::Confederation, true, false, 5, 2, 2},
            {"Guard Tower (Conf)", DebugSpawnCategory::Buildings, Faction::Confederation, true, false, 7, 2, 2},
            {"Wall (Conf)", DebugSpawnCategory::Buildings, Faction::Confederation, false, true, 25, 1, 1}};
        return s_catalog;
    }

    namespace
    {

        bool isLiveOwnUnit(const Entity *e)
        {
            return e && e->alive && e->state != ActionState::Die && !e->isBuilding && e->owner == Players::local();
        }

        constexpr int kGroupSlotW = 96;
        constexpr int kGroupSlotH = 62;
        constexpr int kGroupSlotGap = 6;

    } // namespace

    void GameplayController::pruneGroups(const EntityManager &entities)
    {
        for(auto &g : m_groups)
        {
            g.unitIds.erase(std::remove_if(g.unitIds.begin(), g.unitIds.end(), [&](int id) { return !isLiveOwnUnit(entities.getEntity(id)); }), g.unitIds.end());
            if(g.unitIds.empty())
            {
                g.leaderId = 0;
                continue;
            }
            if(std::find(g.unitIds.begin(), g.unitIds.end(), g.leaderId) != g.unitIds.end())
                continue;
            // Leader: the toughest unit of the group
            g.leaderId = g.unitIds.front();
            for(int id : g.unitIds)
            {
                if(entities.getEntity(id)->maxHp > entities.getEntity(g.leaderId)->maxHp)
                    g.leaderId = id;
            }
        }
    }

    void GameplayController::selectGroup(int index, EntityManager &entities)
    {
        if(index < 0 || index >= static_cast<int>(m_groups.size()) || m_groups[index].unitIds.empty())
            return;
        m_selectedUnits = m_groups[index].unitIds;
        for(auto &e : entities.getAllEntities())
            e.selected = false;
        for(int id : m_selectedUnits)
            entities.getEntity(id)->selected = true;
    }

    void GameplayController::handleControlGroupKeys(InputManager &input, EntityManager &entities, Camera &camera)
    {
        static const SDL_Scancode kKeys[10] = {SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0};
        const bool ctrl = input.isKeyPressed(SDL_SCANCODE_LCTRL) || input.isKeyPressed(SDL_SCANCODE_RCTRL);
        for(int k = 0; k < 10; ++k)
        {
            if(!input.isKeyJustPressed(kKeys[k]))
                continue;
            if(ctrl)
            {
                // Ctrl+N: the selected own units become group N (a unit belongs to one group at a time)
                std::vector<int> units;
                for(int id : m_selectedUnits)
                {
                    if(isLiveOwnUnit(entities.getEntity(id)))
                        units.push_back(id);
                }
                for(auto &g : m_groups)
                {
                    g.unitIds.erase(std::remove_if(g.unitIds.begin(), g.unitIds.end(), [&](int id) { return std::find(units.begin(), units.end(), id) != units.end(); }), g.unitIds.end());
                }
                m_groups[k].unitIds = units;
                m_groups[k].leaderId = 0;
                pruneGroups(entities);
            }
            else
            {
                // N selects the group, pressing it twice quickly brings the camera to its leader
                const std::uint64_t now = SDL_GetTicks();
                if(m_lastGroupKey == k && now - m_lastGroupTicks < 400 && m_groups[k].leaderId > 0)
                {
                    const Entity *leader = entities.getEntity(m_groups[k].leaderId);
                    if(leader)
                        camera.centerOn(leader->getUnitCenterPixelX(), leader->getUnitCenterPixelY());
                }
                m_lastGroupKey = k;
                m_lastGroupTicks = now;
                selectGroup(k, entities);
            }
        }
    }

    int GameplayController::groupOfUnits(const std::vector<int> &unitIds) const
    {
        if(unitIds.empty())
            return -1;
        for(int k = 0; k < static_cast<int>(m_groups.size()); ++k)
        {
            const auto &g = m_groups[k].unitIds;
            bool all = true;
            for(int id : unitIds)
            {
                if(std::find(g.begin(), g.end(), id) == g.end())
                {
                    all = false;
                    break;
                }
            }
            if(all)
                return k;
        }
        return -1;
    }

    void GameplayController::createMatchAis(const MissionController &mission, EntityManager &entities, TileMap &map)
    {
        m_matchAis.clear();
        if(!mission.isMatch())
            return;
        const MatchSetup &setup = mission.matchSetup();
        auto hqOf = [&](PlayerId p, int &x, int &y)
        {
            for(const Entity &e : entities.getAllEntities())
            {
                if(e.alive && e.isBuilding && e.type == UnitType::Headquarters && e.owner == p)
                {
                    x = e.tileX;
                    y = e.tileY;
                    return true;
                }
            }
            return false;
        };
        for(PlayerId p = 0; p < kMaxPlayers; ++p)
        {
            const PlayerSlot &slot = setup.slots[p];
            if(!slot.active || slot.human)
                continue;
            int ownX = 0, ownY = 0;
            if(!hqOf(p, ownX, ownY))
                continue;
            // It goes for the nearest enemy headquarters
            int targetX = -1, targetY = -1;
            float best = 1e9f;
            for(PlayerId q = 0; q < kMaxPlayers; ++q)
            {
                int x = 0, y = 0;
                if(!setup.slots[q].active || !Players::hostile(p, q) || !hqOf(q, x, y))
                    continue;
                const float d = DetMath::hypot(static_cast<float>(x - ownX), static_cast<float>(y - ownY));
                if(d < best)
                {
                    best = d;
                    targetX = x;
                    targetY = y;
                }
            }
            if(targetX < 0)
                continue;
            auto ai = std::make_unique<AIController>();
            ai->setPlayer(p);
            ai->setDifficulty(static_cast<AIDifficulty>(std::min<int>(slot.aiLevel, 3)));
            ai->init(ownX, ownY, targetX, targetY);
            ai->recordStartingBase(entities, map);
            ai->setSkirmish(true);
            m_matchAis.push_back(std::move(ai));
        }
    }

    void GameplayController::resetForMission(const MissionController &mission, HUDController &hud, EntityManager &entities, TileMap &map)
    {
        // A new mission reuses entity ids: forget the selection and groups of the previous one
        m_missionSerial = mission.getLoadSerial();
        m_selectedUnits.clear();
        for(auto &g : m_groups)
            g = ControlGroup {};
        if(mission.isMatch())
            m_sim.startMatch(mission.matchSetup());
        else
            m_sim.reset();
        hud.resetForMission();
        entities.rebuildBlueprints(); // build menu of the faction picked for this mission
        createMatchAis(mission, entities, map);
    }

    SimCommand GameplayController::command(CommandType type) const
    {
        SimCommand c;
        c.type = type;
        c.player = Players::local();
        c.debug = m_debugMode && m_controlAllFactions;
        return c;
    }

    void GameplayController::orderAt(const std::vector<int> &units, float worldX, float worldY, OrderKind kind)
    {
        SimCommand c = command(CommandType::OrderAt);
        c.ids.assign(units.begin(), units.end());
        c.x = static_cast<int32_t>(worldX);
        c.y = static_cast<int32_t>(worldY);
        c.a = static_cast<int32_t>(kind);
        c.b = (units.size() >= 2 && groupOfUnits(units) >= 0) ? 1 : 0;
        m_sim.issue(std::move(c));
    }

    int GameplayController::groupBarHit(int mx, int my, int mapViewW, int windowH) const
    {
        int count = 0;
        for(const auto &g : m_groups)
            count += g.unitIds.empty() ? 0 : 1;
        if(count == 0)
            return -1;
        const int totalW = count * kGroupSlotW + (count - 1) * kGroupSlotGap;
        int x = (mapViewW - totalW) / 2;
        const int y = windowH - kGroupSlotH - 10;
        for(int k = 0; k < static_cast<int>(m_groups.size()); ++k)
        {
            if(m_groups[k].unitIds.empty())
                continue;
            if(mx >= x && mx < x + kGroupSlotW && my >= y && my < y + kGroupSlotH)
                return k;
            x += kGroupSlotW + kGroupSlotGap;
        }
        return -1;
    }

    void GameplayController::renderGroupLeaders(Renderer &renderer, const Camera &camera, const EntityManager &entities) const
    {
        const SDL_Color gold {255, 214, 64, 255};
        for(const ControlGroup &g : m_groups)
        {
            if(g.unitIds.empty())
                continue;
            bool anySelected = false;
            for(int id : g.unitIds)
            {
                const Entity *e = entities.getEntity(id);
                anySelected = anySelected || (e && e->selected);
            }
            int leaderId = g.leaderId;
            const AISquad *squad = nullptr;
            for(const auto &sq : m_sim.squads(Players::local()).getSquads())
            {
                if(std::find(sq.unitIds.begin(), sq.unitIds.end(), g.unitIds.front()) != sq.unitIds.end())
                {
                    squad = &sq;
                    leaderId = sq.leaderId;
                    break;
                }
            }
            if(!anySelected && !squad)
                continue;
            const Entity *leader = entities.getEntity(leaderId);
            if(!leader)
                continue;
            const float lx = leader->getUnitCenterPixelX();
            const float ly = leader->getUnitCenterPixelY();

            // Marching: who follows whom (green inside the leash, yellow beyond it)
            if(squad)
            {
                for(int id : squad->unitIds)
                {
                    const Entity *u = entities.getEntity(id);
                    if(!u || id == leaderId)
                        continue;
                    const float d = DetMath::hypot(u->tileX - static_cast<float>(leader->tileX), u->tileY - static_cast<float>(leader->tileY));
                    const SDL_Color c = d <= squad->leashRadius ? SDL_Color {90, 220, 120, 90} : SDL_Color {255, 200, 60, 140};
                    renderer.drawLineWorld(u->getUnitCenterPixelX(), u->getUnitCenterPixelY(), lx, ly, camera, c);
                }
            }

            // Leader marker: small gold triangle above the unit
            float sx = 0.0f, sy = 0.0f;
            camera.worldToScreen(lx, ly, sx, sy);
            const int top = static_cast<int>(sy - 26.0f * camera.getZoom());
            for(int i = 0; i < 6; ++i)
            {
                renderer.fillRectScreen(static_cast<int>(sx) - (5 - i), top + i, 2 * (5 - i) + 1, 1, gold);
            }
        }
    }

    void GameplayController::renderGroupBar(Renderer &renderer, BitmapFontRenderer &font, const EntityManager &entities, int mapViewW, int windowH, int mx, int my) const
    {
        int count = 0;
        for(const auto &g : m_groups)
            count += g.unitIds.empty() ? 0 : 1;
        if(count == 0)
            return;

        const int totalW = count * kGroupSlotW + (count - 1) * kGroupSlotGap;
        int x = (mapViewW - totalW) / 2;
        const int y = windowH - kGroupSlotH - 10;
        const SDL_Color top {28, 70, 92, 235};
        const SDL_Color bottom {12, 30, 44, 235};
        const SDL_Color edge {70, 150, 180, 255};

        for(int k = 0; k < static_cast<int>(m_groups.size()); ++k)
        {
            const ControlGroup &g = m_groups[k];
            if(g.unitIds.empty())
                continue;
            const ui::UiRect slot {x, y, kGroupSlotW, kGroupSlotH};
            x += kGroupSlotW + kGroupSlotGap;

            const bool selected = (m_selectedUnits.size() == g.unitIds.size() && groupOfUnits(m_selectedUnits) == k);
            const bool hovered = slot.contains(mx, my);
            int leaderId = g.leaderId;
            bool marching = false;
            for(const auto &sq : m_sim.squads(Players::local()).getSquads())
            {
                if(std::find(sq.unitIds.begin(), sq.unitIds.end(), g.leaderId) != sq.unitIds.end() || std::find(sq.unitIds.begin(), sq.unitIds.end(), g.unitIds.front()) != sq.unitIds.end())
                {
                    leaderId = sq.leaderId;
                    marching = true;
                    break;
                }
            }

            ui::fillVerticalGradient(renderer, slot, hovered ? ui::mix(top, edge, 0.25f) : top, bottom);
            const Unit *leader = entities.getUnit(leaderId);
            if(leader)
            {
                int comp = unitCameoCompId(leader->archetypeId);
                if(comp >= 0)
                    renderer.drawCompositeScreen(comp, slot.x + (slot.w - 60) / 2 + 6, slot.y + 6, 1.0f, leader->faction);
            }

            // Group health
            int hp = 0, maxHp = 0;
            for(int id : g.unitIds)
            {
                // A unit may have died since the groups were pruned at the start of the frame
                const Entity *e = entities.getEntity(id);
                if(!e)
                    continue;
                hp += e->currentHp;
                maxHp += e->maxHp;
            }
            const float ratio = maxHp > 0 ? static_cast<float>(hp) / static_cast<float>(maxHp) : 0.0f;
            const SDL_Color hpCol = ratio > 0.6f ? ui::color::Ok : (ratio > 0.3f ? ui::color::Accent : ui::color::Danger);
            renderer.fillRectScreen(slot.x + 4, slot.bottom() - 7, slot.w - 8, 3, {6, 10, 14, 220});
            renderer.fillRectScreen(slot.x + 4, slot.bottom() - 7, static_cast<int>((slot.w - 8) * ratio), 3, hpCol);

            // Key chip, unit count, march indicator
            const std::string key = std::to_string((k + 1) % 10);
            renderer.fillRectScreen(slot.x + 3, slot.y + 3, 18, 18, selected ? ui::color::Accent : SDL_Color {8, 20, 30, 230});
            font.drawString(key, slot.x + 12, slot.y + 12, ui::FONT_SMALL_BOLD, selected ? ui::color::AccentText : ui::color::Text, false, Anchor::HCENTER | Anchor::VCENTER);
            font.drawString("\xC3\x97" + std::to_string(g.unitIds.size()), slot.right() - 6, slot.bottom() - 16, ui::FONT_SMALL_BOLD, ui::color::Text, true, Anchor::RIGHT | Anchor::VCENTER);
            if(marching)
            {
                font.drawString(_("MARCH"), slot.x + 6, slot.bottom() - 16, ui::FONT_SMALL_BOLD, ui::color::Info, true, Anchor::LEFT | Anchor::VCENTER);
            }

            SDL_Color border = selected ? ui::color::Accent : (hovered ? edge : SDL_Color {40, 90, 112, 255});
            renderer.drawRectScreen(slot.x, slot.y, slot.w, slot.h, border);
            if(selected)
                renderer.drawRectScreen(slot.x + 1, slot.y + 1, slot.w - 2, slot.h - 2, ui::withAlpha(ui::color::Accent, 110));
        }
    }

    void GameplayController::setDebugMode(bool enabled)
    {
        m_debugMode = enabled;
        if(enabled)
        {
            m_showNavLines = true;
            m_controlAllFactions = true;
            m_revealFog = true;
            m_showDebugGrid = true;
        }
    }

    void GameplayController::update(
        float deltaTime,
        InputManager &input,
        Camera &camera,
        TileMap &map,
        Pathfinder &pathfinder,
        EntityManager &entities,
        Economy &economy,
        AIController &aiController,
        MissionController &mission,
        HUDController &hud,
        MenuController &menu,
        SoundEngine &sound,
        AppState &appState,
        int windowW,
        int windowH)
    {
        input.update(deltaTime, camera);
        camera.clampToBounds(map.getWidth(), map.getHeight());
        hud.bindSimulation(&m_sim);
        map.setViewerTeam(Players::teamOf(Players::local())); // the fog that is drawn is the local player's

        if(mission.getLoadSerial() != m_missionSerial)
            resetForMission(mission, hud, entities, map);
        pruneGroups(entities);

        int mx = input.getMouseScreenX();
        int my = input.getMouseScreenY();
        float lx = 0.0f, ly = 0.0f;
        bool leftClicked = input.wasLeftClickIssued(lx, ly);
        float rx = 0.0f, ry = 0.0f;
        bool rightClicked = input.wasRightClickIssued(rx, ry);

        // 1. Objectives Dialog
        if(menu.isObjectivesDialogOpen())
        {
            if(menu.updateObjectivesDialog(input, leftClicked))
            {
                // A network match goes on while a player reads its menus
                if(mission.isMatch())
                    simulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
                return;
            }
        }

        // 2. In-Game Pause Menu
        if(menu.isInGameMenuOpen())
        {
            if(menu.updateInGameMenu(input, mx, my, leftClicked, windowW, windowH, sound, mission, map, entities, economy, pathfinder, camera, appState))
            {
                if(mission.isMatch() && appState == AppState::SkirmishPlaying)
                    simulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
                return;
            }
        }

        // 4. Command sidebar click handling (right edge); the debug toolbar sits above it
        hud.setTopInset(m_debugMode ? 32 : 0);
        bool openMenu = false;
        bool openObjectives = false;
        bool mapInputConsumed = hud.handleHUDClick(mx, my, leftClicked, rightClicked, input.isLeftMouseDown(), input.consumeUiWheel(), windowW, windowH, economy, entities, map, camera, openMenu, openObjectives, m_selectedUnits);

        if(openMenu)
        {
            menu.setInGameMenuOpen(true);
        }
        if(openObjectives)
        {
            menu.setObjectivesDialogOpen(true);
        }

        // Everything left of the sidebar is the map viewport
        const int mapViewW = camera.getViewportWidth();

        // Control group bar at the bottom of the map: click selects the group
        if(!mapInputConsumed && (leftClicked || rightClicked || input.isLeftMouseDown()))
        {
            int slot = groupBarHit(mx, my, mapViewW, windowH);
            if(slot >= 0)
            {
                if(leftClicked)
                    selectGroup(slot, entities);
                mapInputConsumed = true;
                input.cancelSelectionBox();
            }
        }

        // 4.4. Debug Spawn Dropdown Click Handling (--debug mode)
        if(m_debugMode && m_spawnDropdownOpen)
        {
            if(leftClicked)
            {
                int dropX = 450;
                int dropY = 32;
                int colW = 170;
                int dropW = colW * 3;
                int dropH = 370;

                bool itemSelected = false;
                if(mx >= dropX && mx <= dropX + dropW && my >= dropY && my <= dropY + dropH)
                {
                    // Column 1: HUMANS (items 0..5)
                    if(mx < dropX + colW)
                    {
                        int itemIdx = (my - (dropY + 24)) / 22;
                        if(itemIdx >= 0 && itemIdx < 6)
                        {
                            m_selectedSpawnIndex = itemIdx;
                            m_spawnMode = true;
                            m_spawnDropdownOpen = false;
                            itemSelected = true;
                        }
                    }
                    // Column 2: VEHICLES (items 6..13)
                    else if(mx < dropX + colW * 2)
                    {
                        int itemIdx = (my - (dropY + 24)) / 22;
                        if(itemIdx >= 0 && itemIdx < 8)
                        {
                            m_selectedSpawnIndex = 6 + itemIdx;
                            m_spawnMode = true;
                            m_spawnDropdownOpen = false;
                            itemSelected = true;
                        }
                    }
                    // Column 3: BUILDINGS (items 14..29)
                    else
                    {
                        int itemIdx = (my - (dropY + 24)) / 20;
                        if(itemIdx >= 0 && itemIdx < 16)
                        {
                            m_selectedSpawnIndex = 14 + itemIdx;
                            m_spawnMode = true;
                            m_spawnDropdownOpen = false;
                            itemSelected = true;
                        }
                    }
                }
                if(!itemSelected)
                {
                    m_spawnDropdownOpen = false;
                }
                mapInputConsumed = true;
            }
            else if(rightClicked || input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
            {
                m_spawnDropdownOpen = false;
                mapInputConsumed = true;
            }
        }

        // 4.5. Top Debug Toolbar Click Handling (--debug mode)
        if(m_debugMode && leftClicked && my <= 30)
        {
            if(mx >= 98 && mx <= 198)
            {
                m_showNavLines = !m_showNavLines;
            }
            else if(mx >= 202 && mx <= 322)
            {
                m_controlAllFactions = !m_controlAllFactions;
            }
            else if(mx >= 326 && mx <= 436)
            {
                m_pauseAI = !m_pauseAI;
            }
            else if(mx >= 440 && mx <= 545)
            {
                m_revealFog = !m_revealFog;
            }
            else if(mx >= 549 && mx <= 624)
            {
                SimCommand c = command(CommandType::DebugCredits);
                c.debug = true;
                c.a = 1000;
                m_sim.issue(std::move(c));
            }
            else if(mx >= 628 && mx <= 748)
            {
                m_spawnMode = !m_spawnMode;
                if(m_spawnMode)
                {
                    m_spawnDropdownOpen = false;
                }
            }
            else if(mx >= 752 && mx <= 960)
            {
                m_spawnDropdownOpen = !m_spawnDropdownOpen;
            }
            else if(mx >= 964 && mx <= 1049)
            {
                SimCommand c = command(CommandType::ToggleSiege);
                c.ids.assign(m_selectedUnits.begin(), m_selectedUnits.end());
                m_sim.issue(std::move(c));
            }
            else if(mx >= 1053 && mx <= 1153)
            {
                m_showDebugGrid = !m_showDebugGrid;
            }
            else if(mx >= 1157 && mx <= 1237)
            {
                m_debugOverlayToggleRequested = true;
            }
            mapInputConsumed = true;
        }

        // 4.5.5. Debug Spawn Mode Map Placement
        if(!mapInputConsumed && m_debugMode && m_spawnMode)
        {
            if(rightClicked || input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
            {
                m_spawnMode = false;
                mapInputConsumed = true;
            }
            else if(leftClicked && my >= 32 && mx < mapViewW)
            {
                const auto &catalog = getDebugSpawnCatalog();
                if(m_selectedSpawnIndex >= 0 && m_selectedSpawnIndex < static_cast<int>(catalog.size()))
                {
                    const auto &item = catalog[m_selectedSpawnIndex];
                    int curTileX = static_cast<int>(lx / TILE_WIDTH);
                    int curTileY = static_cast<int>(ly / TILE_HEIGHT);

                    SimCommand c = command(CommandType::DebugSpawn);
                    c.debug = true;
                    c.player = Players::ownerOf(item.faction);
                    c.a = item.archetypeOrBuildingIdx;
                    c.b = static_cast<int32_t>(item.isWall ? DebugSpawnKind::Wall : (item.isBuilding ? DebugSpawnKind::Building : DebugSpawnKind::Unit));
                    c.c = item.width;
                    c.d = item.height;
                    c.x = curTileX;
                    c.y = curTileY;
                    m_sim.issue(std::move(c));
                }
                mapInputConsumed = true;
            }
        }

        // 5. Building Placement Mode on the map
        if(!mapInputConsumed && hud.isBuildMode())
        {
            if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE) || rightClicked)
            {
                hud.setBuildMode(false);
                mapInputConsumed = true;
            }
            else
            {
                const auto &blueprints = EntityManager::getBlueprints();
                int selectedBp = hud.getSelectedBlueprint();
                if(selectedBp >= 0 && selectedBp < static_cast<int>(blueprints.size()))
                {
                    const auto &bp = blueprints[selectedBp];
                    int curTileX = static_cast<int>(input.getMouseWorldX() / TILE_WIDTH);
                    int curTileY = static_cast<int>(input.getMouseWorldY() / TILE_HEIGHT);

                    if(leftClicked && mx < mapViewW)
                    {
                        bool canPlace = entities.canPlaceBuilding(curTileX, curTileY, bp.width, bp.height, map, Players::local());
                        bool canAfford = economy.canAfford(bp.creditCost);
                        if(canPlace && canAfford)
                        {
                            // The simulation checks both again when it places it (a network turn later)
                            SimCommand c = command(CommandType::PlaceBuilding);
                            c.a = selectedBp;
                            c.x = curTileX;
                            c.y = curTileY;
                            m_sim.issue(std::move(c));
                            // Walls are laid one after the other; a building ends the placement
                            if(!bp.isWall)
                                hud.setBuildMode(false);
                        }
                    }
                }
                mapInputConsumed = true;
            }
        }

        // 6. Attack-Move targeting mode active (triggered by 'Q' key, game does NOT pause!)
        if(!mapInputConsumed && hud.isAttackMoveMode())
        {
            if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE) || rightClicked)
            {
                hud.setAttackMoveMode(false);
                mapInputConsumed = true;
            }
            else if(leftClicked && mx < mapViewW)
            {
                std::vector<int> combatUnits;
                for(int uid : m_selectedUnits)
                {
                    const Entity *e = entities.getEntity(uid);
                    if(e && e->alive && !e->isBuilding)
                    {
                        combatUnits.push_back(uid);
                    }
                }

                if(!combatUnits.empty())
                    orderAt(combatUnits, lx, ly, OrderKind::AttackMove);
                hud.setAttackMoveMode(false);
                mapInputConsumed = true;
            }
        }

        // 7. Keyboard shortcuts (camera keys are handled by InputManager) and map orders
        if(!mapInputConsumed)
        {
            // 'Q' toggles Attack-Move targeting
            if(input.isKeyJustPressed(SDL_SCANCODE_Q))
            {
                bool hasCombatUnits = false;
                for(int uid : m_selectedUnits)
                {
                    const Entity *e = entities.getEntity(uid);
                    if(e && e->alive && !e->isBuilding)
                    {
                        hasCombatUnits = true;
                        break;
                    }
                }
                if(hasCombatUnits)
                {
                    hud.setAttackMoveMode(!hud.isAttackMoveMode());
                    hud.setBuildMode(false);
                }
            }

            handleControlGroupKeys(input, entities, camera);

            // 'G' - Siege mode toggle
            if(input.isKeyJustPressed(SDL_SCANCODE_G))
            {
                SimCommand c = command(CommandType::ToggleSiege);
                c.ids.assign(m_selectedUnits.begin(), m_selectedUnits.end());
                m_sim.issue(std::move(c));
            }

            // ESC - In-game Pause Menu
            if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
            {
                menu.setInGameMenuOpen(true);
                if(mission.isMatch())
                    simulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
                return;
            }

            // Right-Click orders on map (Move / Attack / Attack Wall / Siege Context Menu)
            if(rightClicked && mx < mapViewW && (!m_debugMode || my > 30))
            {
                std::vector<int> combatUnits;
                for(int uid : m_selectedUnits)
                {
                    const Entity *e = entities.getEntity(uid);
                    if(e && e->alive && !e->isBuilding)
                    {
                        combatUnits.push_back(uid);
                    }
                }

                // Only Barracks / Factories selected: right click moves their rally point
                if(combatUnits.empty())
                {
                    SimCommand c = command(CommandType::SetRally);
                    c.ids.assign(m_selectedUnits.begin(), m_selectedUnits.end());
                    c.x = static_cast<int32_t>(rx);
                    c.y = static_cast<int32_t>(ry);
                    if(!c.ids.empty())
                        m_sim.issue(std::move(c));
                }
                else
                {
                    orderAt(combatUnits, rx, ry, OrderKind::Move);
                }
            }

            // Left-Click single unit selection on map
            if(!hud.isAttackMoveMode() && leftClicked && mx < mapViewW && (!m_debugMode || my > 30))
            {
                int clickedId = entities.getUnitAt(lx, ly);
                const Building *clickedBuilding = entities.getBuilding(clickedId);
                const bool ownProducer = clickedBuilding && clickedBuilding->faction == playerFaction() && clickedBuilding->isUnitProducer();
                const bool shift = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
                const Uint64 now = SDL_GetTicks();
                const bool doubleClick = clickedId > 0 && clickedId == m_lastClickId && now - m_lastClickTicks < 350;
                m_lastClickId = clickedId;
                m_lastClickTicks = now;

                // Shift+click on a Barracks / Factory adds it to the selected ones of the same type
                bool sameTypeSelected = ownProducer && shift;
                if(sameTypeSelected)
                {
                    for(int id : m_selectedUnits)
                    {
                        const Building *b = entities.getBuilding(id);
                        if(!b || b->type != clickedBuilding->type)
                            sameTypeSelected = false;
                    }
                }
                if(sameTypeSelected)
                {
                    Entity *e = entities.getEntity(clickedId);
                    if(e->selected)
                    {
                        e->selected = false;
                        m_selectedUnits.erase(std::remove(m_selectedUnits.begin(), m_selectedUnits.end(), clickedId), m_selectedUnits.end());
                    }
                    else
                    {
                        e->selected = true;
                        m_selectedUnits.push_back(clickedId);
                        hud.focusBuilding(clickedId, entities);
                    }
                }
                else
                {
                    m_selectedUnits.clear();
                    for(auto &e : entities.getAllEntities())
                        e.selected = false;
                    if(ownProducer && doubleClick)
                    {
                        // Double click: every own building of that type
                        for(auto &e : entities.getAllEntities())
                        {
                            const Building *b = e.asBuilding();
                            if(b && b->alive && b->state != ActionState::Die && b->owner == Players::local() && b->type == clickedBuilding->type)
                            {
                                e.selected = true;
                                m_selectedUnits.push_back(e.id);
                            }
                        }
                        hud.focusBuilding(clickedId, entities);
                    }
                    else if(clickedId > 0)
                    {
                        Entity *e = entities.getEntity(clickedId);
                        if(e && (e->owner == Players::local() || (m_debugMode && m_controlAllFactions)))
                        {
                            e->selected = true;
                            m_selectedUnits.push_back(clickedId);
                            if(e->isBuilding)
                                hud.focusBuilding(clickedId, entities);
                        }
                    }
                }
            }

            // Box selection on map
            const auto &sbox = input.getSelectionBox();
            if(!hud.isAttackMoveMode() && sbox.active && sbox.hasSignificantArea() && sbox.startScreenX < mapViewW && (!m_debugMode || sbox.startScreenY > 30))
            {
                float minX = std::min(sbox.startWorldX, sbox.currentWorldX);
                float maxX = std::max(sbox.startWorldX, sbox.currentWorldX);
                float minY = std::min(sbox.startWorldY, sbox.currentWorldY);
                float maxY = std::max(sbox.startWorldY, sbox.currentWorldY);
                Faction selFac = (m_debugMode && m_controlAllFactions) ? aiFaction() : playerFaction();
                entities.getUnitsInBox(minX, minY, maxX, maxY, Players::ownerOf(selFac), m_selectedUnits);
                if(m_debugMode && m_controlAllFactions && m_selectedUnits.empty())
                {
                    entities.getUnitsInBox(minX, minY, maxX, maxY, Players::local(), m_selectedUnits);
                }
            }
        }

        simulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
    }

    void GameplayController::simulationStep(float deltaTime, TileMap &map, Pathfinder &pathfinder, EntityManager &entities, Economy &economy, AIController &aiController, MissionController &mission, HUDController &hud, AppState &appState)
    {
        // 8. Simulation updates: the commands of this tick first, then the world
        SimContext ctx {entities, map, pathfinder, economy, aiController};
        m_sim.applyCommands(ctx);
        m_sim.updateResearch(deltaTime, ctx);
        hud.update(deltaTime, entities, economy, m_sim.takeEvents());
        entities.update(deltaTime, map, pathfinder);
        m_sim.updateSquads(deltaTime, ctx, aiController.getChokepoints());
        economy.update(deltaTime, entities, map, pathfinder);
        m_sim.updateEconomies(deltaTime, ctx);
        // A match has its own computer players (one per computer slot), the mission's commander stays out
        if(mission.isMatch())
        {
            for(auto &ai : m_matchAis)
                ai->update(deltaTime, map, entities, pathfinder);
        }
        else if(!m_debugMode || !m_pauseAI)
        {
            aiController.update(deltaTime, map, entities, pathfinder);
        }
        mission.update(deltaTime, map, entities, pathfinder);
        m_sim.endTick();

        // 9. Victory / Defeat check
        if(mission.isVictory())
        {
            appState = AppState::Victory;
        }
        else if(mission.isDefeat())
        {
            appState = AppState::Defeat;
        }
    }

    void GameplayController::renderSelectionBox(Renderer &renderer, const Camera &camera, const InputManager &input)
    {
        (void) camera;
        const auto &sbox = input.getSelectionBox();
        if(sbox.active && sbox.hasSignificantArea())
        {
            int bx = std::min(sbox.startScreenX, sbox.currentScreenX);
            int by = std::min(sbox.startScreenY, sbox.currentScreenY);
            int bw = std::abs(sbox.currentScreenX - sbox.startScreenX);
            int bh = std::abs(sbox.currentScreenY - sbox.startScreenY);
            renderer.fillRectScreen(bx, by, bw, bh, {0, 255, 0, 45});
            renderer.drawRectScreen(bx, by, bw, bh, {0, 255, 0, 255});
        }
    }

    void GameplayController::loadOverlayStyle(const EnvironmentConfig &env)
    {
        const nlohmann::json &o = env.section("overlay");
        m_overlay = OverlayStyle {};
        if(!o.is_object())
            return;
        m_overlay.opacity = std::clamp(o.value("line_opacity", m_overlay.opacity), 0.0f, 1.0f);
        auto read = [&](const char *key, LineStyle &style)
        {
            if(!o.contains(key) || !o[key].is_object())
                return;
            style.color = RenderUtility::ParseHexColor(o[key].value("color", std::string()), style.color);
            style.width = std::clamp(o[key].value("width", style.width), 0.5f, 12.0f);
        };
        read("move_line", m_overlay.move);
        read("attack_line", m_overlay.attack);
        read("wall_line", m_overlay.wall);
    }

    void GameplayController::renderOrderLines(Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map, bool showAll) const
    {
        auto drawLine = [&](float x0, float y0, float x1, float y1, const LineStyle &style)
        {
            SDL_Color c = style.color;
            c.a = static_cast<Uint8>(std::clamp(c.a * m_overlay.opacity, 0.0f, 255.0f));
            RenderUtility::DrawLine(renderer, camera, x0, y0, x1, y1, c, style.width);
        };
        auto drawOrders = [&](const Entity &e)
        {
            const Unit *u = e.asUnit();
            if(!u || !u->alive || u->state == ActionState::Die)
                return;
            const float x = u->getUnitCenterPixelX(), y = u->getUnitCenterPixelY();
            if(u->targetEntityId != 0)
            {
                const Entity *target = entities.getEntity(u->targetEntityId);
                if(target && target->alive && target->state != ActionState::Die && (target->owner == Players::local() || entities.isEntityVisibleToPlayer(*target, Players::local(), map)))
                {
                    drawLine(x, y, target->getUnitCenterPixelX(), target->getUnitCenterPixelY(), m_overlay.attack);
                }
            }
            if(u->targetWallTileX >= 0 && u->targetWallTileY >= 0)
            {
                drawLine(x, y, u->targetWallTileX * TILE_WIDTH + TILE_WIDTH * 0.5f, u->targetWallTileY * TILE_HEIGHT + TILE_HEIGHT * 0.5f, m_overlay.wall);
            }
            if(u->hasMoveTarget())
            {
                drawLine(x, y, u->targetTileX * TILE_WIDTH + TILE_WIDTH * 0.5f, u->targetTileY * TILE_HEIGHT + TILE_HEIGHT * 0.5f, m_overlay.move);
            }
        };

        if(showAll)
        {
            for(const Entity &e : entities.getAllEntities())
            {
                // Only the player's own and allied units: the orders of an enemy are not for the eyes of the other side
                if(!e.isUnitEntity() || e.owner == kNoPlayer || Players::hostile(e.owner, Players::local()))
                    continue;
                drawOrders(e);
            }
            return;
        }
        for(int uid : m_selectedUnits)
        {
            const Entity *e = entities.getEntity(uid);
            if(e && e->owner == Players::local())
                drawOrders(*e);
        }
    }

    void GameplayController::renderDebugNavigation(Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map)
    {
        (void) map;
        if(!m_debugMode || !m_showNavLines)
            return;

        for(int uid : m_selectedUnits)
        {
            const Entity *e = entities.getEntity(uid);
            if(!e || !e->alive || e->state == ActionState::Die)
                continue;

            float wx = e->getUnitCenterPixelX();
            float wy = e->getUnitCenterPixelY();

            // 1. Attack Range Ellipse
            SDL_Color rangeCol = (e->owner == Players::local()) ? SDL_Color {0, 220, 255, 90} : SDL_Color {255, 60, 60, 90};
            RenderUtility::DrawTileRadius(renderer, camera, wx, wy, e->attackRange, rangeCol, 1.5f);

            // 2. Navigation Path Lines & Waypoints
            const Unit *u = e->asUnit();
            if(u && !u->path.empty() && u->pathIndex < u->path.size())
            {
                float prevX = wx;
                float prevY = wy;
                for(size_t i = u->pathIndex; i < u->path.size(); ++i)
                {
                    float nodeX = u->path[i].x * TILE_WIDTH + (TILE_WIDTH * 0.5f);
                    float nodeY = u->path[i].y * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
                    renderer.drawLineWorld(prevX, prevY, nodeX, nodeY, camera, {0, 255, 200, 220});
                    renderer.drawRectWorld(nodeX - 3.0f, nodeY - 3.0f, 6.0f, 6.0f, camera, {0, 255, 200, 255});
                    prevX = nodeX;
                    prevY = nodeY;
                }
                // Destination flag / marker
                RenderUtility::DrawRadius(renderer, camera, prevX, prevY, 5.0f, SDL_Color {255, 255, 0, 255}, 1.5f);
            }

            // 3. Combat Target Line (Red)
            if(e->targetEntityId != 0)
            {
                const Entity *tgt = entities.getEntity(e->targetEntityId);
                if(tgt && tgt->alive && tgt->state != ActionState::Die)
                {
                    float twx = tgt->getUnitCenterPixelX();
                    float twy = tgt->getUnitCenterPixelY();
                    renderer.drawLineWorld(wx, wy, twx, twy, camera, {255, 30, 30, 255});
                    renderer.drawRectWorld(twx - 8.0f, twy - 8.0f, 16.0f, 16.0f, camera, {255, 30, 30, 255});
                }
            }

            // 4. Wall Target Line (Orange)
            if(e->targetWallTileX >= 0 && e->targetWallTileY >= 0)
            {
                float twx = e->targetWallTileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
                float twy = e->targetWallTileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
                renderer.drawLineWorld(wx, wy, twx, twy, camera, {255, 140, 0, 255});
                renderer.drawRectWorld(twx - 6.0f, twy - 6.0f, 12.0f, 12.0f, camera, {255, 140, 0, 255});
            }

            // 5. Waiting/Blocked indicator
            if(u && u->waitingForPath)
            {
                renderer.fillRectWorld(wx - 10.0f, wy - 24.0f, 20.0f, 6.0f, camera, {255, 180, 0, 220});
            }
        }
    }

    void GameplayController::renderDebugToolbar(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW)
    {
        if(!m_debugMode)
            return;

        // Background bar
        renderer.fillRectScreen(0, 0, windowW, 30, {16, 20, 28, 240});
        renderer.drawLineScreen(0, 30, windowW, 30, {0, 200, 255, 180});

        // Button 1: Badge
        renderer.fillRectScreen(4, 3, 90, 24, {20, 70, 40, 255});
        renderer.drawRectScreen(4, 3, 90, 24, {40, 220, 90, 255});
        fontRenderer.drawString("--DEBUG--", 12, 7, 1, {40, 255, 90, 255});

        // Button 2: Nav Lines
        SDL_Color navBg = m_showNavLines ? SDL_Color {30, 80, 100, 255} : SDL_Color {40, 40, 50, 255};
        renderer.fillRectScreen(98, 3, 100, 24, navBg);
        renderer.drawRectScreen(98, 3, 100, 24, {0, 200, 255, 255});
        fontRenderer.drawString(m_showNavLines ? "NAV: ON" : "NAV: OFF", 108, 7, 1, {255, 255, 255, 255});

        // Button 3: Enemy Control
        SDL_Color ctrlBg = m_controlAllFactions ? SDL_Color {110, 40, 40, 255} : SDL_Color {40, 40, 50, 255};
        renderer.fillRectScreen(202, 3, 120, 24, ctrlBg);
        renderer.drawRectScreen(202, 3, 120, 24, {255, 100, 100, 255});
        fontRenderer.drawString(m_controlAllFactions ? "CTRL: ALL" : "CTRL: PLAYER", 210, 7, 1, {255, 255, 255, 255});

        // Button 4: AI Pause
        SDL_Color aiBg = m_pauseAI ? SDL_Color {100, 80, 20, 255} : SDL_Color {40, 40, 50, 255};
        renderer.fillRectScreen(326, 3, 110, 24, aiBg);
        renderer.drawRectScreen(326, 3, 110, 24, {255, 200, 50, 255});
        fontRenderer.drawString(m_pauseAI ? "AI: PAUSED" : "AI: ACTIVE", 336, 7, 1, {255, 255, 255, 255});

        // Button 5: Fog of War
        SDL_Color fogBg = m_revealFog ? SDL_Color {40, 80, 60, 255} : SDL_Color {40, 40, 50, 255};
        renderer.fillRectScreen(440, 3, 105, 24, fogBg);
        renderer.drawRectScreen(440, 3, 105, 24, {100, 255, 150, 255});
        fontRenderer.drawString(m_revealFog ? "FOG: REVEAL" : "FOG: ON", 448, 7, 1, {255, 255, 255, 255});

        // Button 6: +$1000
        renderer.fillRectScreen(549, 3, 75, 24, {30, 60, 30, 255});
        renderer.drawRectScreen(549, 3, 75, 24, {100, 255, 100, 255});
        fontRenderer.drawString("+$1000", 559, 7, 1, {120, 255, 120, 255});

        // Button 7: SPAWN MODE Toggle
        SDL_Color spawnBg = m_spawnMode ? SDL_Color {25, 100, 50, 255} : SDL_Color {40, 40, 50, 255};
        SDL_Color spawnBorder = m_spawnMode ? SDL_Color {40, 255, 100, 255} : SDL_Color {120, 140, 160, 255};
        renderer.fillRectScreen(628, 3, 120, 24, spawnBg);
        renderer.drawRectScreen(628, 3, 120, 24, spawnBorder);
        fontRenderer.drawString(m_spawnMode ? "SPAWN: ON" : "SPAWN: OFF", 638, 7, 1, {255, 255, 255, 255});

        // Button 8: SPAWN Dropdown Combobox
        const auto &catalog = getDebugSpawnCatalog();
        std::string selName = "SELECT ENTITY";
        Faction selFac = Faction::Resistance;
        if(m_selectedSpawnIndex >= 0 && m_selectedSpawnIndex < static_cast<int>(catalog.size()))
        {
            selName = catalog[m_selectedSpawnIndex].name;
            selFac = catalog[m_selectedSpawnIndex].faction;
        }
        renderer.fillRectScreen(752, 3, 208, 24, {28, 36, 52, 255});
        renderer.drawRectScreen(752, 3, 208, 24, {0, 200, 255, 255});
        // Faction indicator pip
        SDL_Color facColor = (selFac == Faction::Confederation) ? SDL_Color {0, 200, 255, 255} : SDL_Color {255, 80, 80, 255};
        renderer.fillRectScreen(758, 10, 8, 10, facColor);
        std::string comboLabel = selName + " v";
        fontRenderer.drawString(comboLabel, 772, 7, 1, {255, 255, 255, 255});

        // Button 9: Siege Mode Toggle
        renderer.fillRectScreen(964, 3, 85, 24, {60, 50, 20, 255});
        renderer.drawRectScreen(964, 3, 85, 24, {255, 200, 80, 255});
        fontRenderer.drawString("SIEGE(G)", 972, 7, 1, {255, 220, 100, 255});

        // Button 10: Passability Grid (70% Transparency Green/Red)
        SDL_Color gridBg = m_showDebugGrid ? SDL_Color {25, 90, 45, 255} : SDL_Color {40, 40, 50, 255};
        SDL_Color gridBorder = m_showDebugGrid ? SDL_Color {40, 255, 100, 255} : SDL_Color {120, 140, 160, 255};
        renderer.fillRectScreen(1053, 3, 100, 24, gridBg);
        renderer.drawRectScreen(1053, 3, 100, 24, gridBorder);
        fontRenderer.drawString(m_showDebugGrid ? "GRID: ON" : "GRID: OFF", 1063, 7, 1, {255, 255, 255, 255});

        // Button 11: Dear ImGui inspector overlay
        renderer.fillRectScreen(1157, 3, 80, 24, {40, 40, 50, 255});
        renderer.drawRectScreen(1157, 3, 80, 24, {180, 140, 255, 255});
        fontRenderer.drawString("IMGUI", 1175, 7, 1, {255, 255, 255, 255});
    }

    void GameplayController::renderDebugGrid(Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map)
    {
        if(!m_debugMode || !m_showDebugGrid)
            return;

        float startWorldX, startWorldY, endWorldX, endWorldY;
        camera.screenToWorld(0, 0, startWorldX, startWorldY);
        camera.screenToWorld(camera.getViewportWidth(), camera.getViewportHeight(), endWorldX, endWorldY);

        int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, map.getWidth() - 1);
        int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, map.getWidth());
        int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, map.getHeight() - 1);
        int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, map.getHeight());

        // User requirement: "зеленый 70% прозрачность - проходимое поле, и красный 70% не проходимые пути (учитывая постройки)"
        // 70% transparency = 30% alpha (~76)
        const SDL_Color passableCol {0, 220, 0, 76};
        const SDL_Color impassableCol {220, 0, 0, 76};

        for(int ty = startTileY; ty < endTileY; ++ty)
        {
            for(int tx = startTileX; tx < endTileX; ++tx)
            {
                bool passable = map.isPassable(tx, ty) && (map.getWall(tx, ty) == 0);
                if(passable)
                {
                    // Check if tile is covered by any stationary living building
                    for(const auto &e : entities.getAllEntities())
                    {
                        const Building *b = e.asBuilding();
                        if(!b || !b->alive || b->state == ActionState::Die)
                            continue;
                        if(tx >= b->tileX && tx < b->tileX + b->buildWidth && ty >= b->tileY && ty < b->tileY + b->buildHeight)
                        {
                            passable = false;
                            break;
                        }
                    }
                }

                SDL_Color color = passable ? passableCol : impassableCol;
                // "он отображается заполненным прямоугольником но сжатый со сторон на 2 пикселя (как бы сетка)"
                float rx = tx * TILE_WIDTH + 2.0f;
                float ry = ty * TILE_HEIGHT + 2.0f;
                float rw = TILE_WIDTH - 4.0f;
                float rh = TILE_HEIGHT - 4.0f;
                renderer.fillRectWorld(rx, ry, rw, rh, camera, color);
            }
        }
    }

    void GameplayController::renderDebugSpawnDropdown(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowH)
    {
        (void) windowH;
        if(!m_debugMode || !m_spawnDropdownOpen)
            return;

        int dropX = 450;
        int dropY = 32;
        int colW = 170;
        int dropW = colW * 3;
        int dropH = 370;

        // Dropdown background box
        renderer.fillRectScreen(dropX, dropY, dropW, dropH, {16, 22, 32, 252});
        renderer.drawRectScreen(dropX, dropY, dropW, dropH, {0, 200, 255, 255});
        renderer.drawLineScreen(dropX + colW, dropY, dropX + colW, dropY + dropH, {40, 60, 80, 200});
        renderer.drawLineScreen(dropX + colW * 2, dropY, dropX + colW * 2, dropY + dropH, {40, 60, 80, 200});

        const auto &catalog = getDebugSpawnCatalog();

        // Column 1: HUMANS (items 0..5)
        fontRenderer.drawString("-- HUMANS --", dropX + 10, dropY + 6, 1, {255, 220, 80, 255});
        for(int i = 0; i < 6 && i < static_cast<int>(catalog.size()); ++i)
        {
            int itemY = dropY + 26 + i * 22;
            if(i == m_selectedSpawnIndex)
            {
                renderer.fillRectScreen(dropX + 4, itemY - 2, colW - 8, 20, {20, 70, 90, 255});
                renderer.drawRectScreen(dropX + 4, itemY - 2, colW - 8, 20, {0, 255, 220, 255});
            }
            SDL_Color fc = (catalog[i].faction == Faction::Confederation) ? SDL_Color {0, 200, 255, 255} : SDL_Color {255, 80, 80, 255};
            renderer.fillRectScreen(dropX + 8, itemY + 4, 6, 8, fc);
            fontRenderer.drawString(catalog[i].name, dropX + 18, itemY, 1, {230, 240, 255, 255});
        }

        // Column 2: VEHICLES (items 6..13)
        fontRenderer.drawString("-- VEHICLES --", dropX + colW + 10, dropY + 6, 1, {255, 220, 80, 255});
        for(int i = 6; i < 14 && i < static_cast<int>(catalog.size()); ++i)
        {
            int itemY = dropY + 26 + (i - 6) * 22;
            if(i == m_selectedSpawnIndex)
            {
                renderer.fillRectScreen(dropX + colW + 4, itemY - 2, colW - 8, 20, {20, 70, 90, 255});
                renderer.drawRectScreen(dropX + colW + 4, itemY - 2, colW - 8, 20, {0, 255, 220, 255});
            }
            SDL_Color fc = (catalog[i].faction == Faction::Confederation) ? SDL_Color {0, 200, 255, 255} : SDL_Color {255, 80, 80, 255};
            renderer.fillRectScreen(dropX + colW + 8, itemY + 4, 6, 8, fc);
            fontRenderer.drawString(catalog[i].name, dropX + colW + 18, itemY, 1, {230, 240, 255, 255});
        }

        // Column 3: BUILDINGS (items 14..29)
        fontRenderer.drawString("-- BUILDINGS --", dropX + colW * 2 + 10, dropY + 6, 1, {255, 220, 80, 255});
        for(int i = 14; i < 30 && i < static_cast<int>(catalog.size()); ++i)
        {
            int itemY = dropY + 26 + (i - 14) * 20;
            if(i == m_selectedSpawnIndex)
            {
                renderer.fillRectScreen(dropX + colW * 2 + 4, itemY - 1, colW - 8, 18, {20, 70, 90, 255});
                renderer.drawRectScreen(dropX + colW * 2 + 4, itemY - 1, colW - 8, 18, {0, 255, 220, 255});
            }
            SDL_Color fc = (catalog[i].faction == Faction::Confederation) ? SDL_Color {0, 200, 255, 255} : SDL_Color {255, 80, 80, 255};
            renderer.fillRectScreen(dropX + colW * 2 + 8, itemY + 3, 6, 8, fc);
            fontRenderer.drawString(catalog[i].name, dropX + colW * 2 + 18, itemY, 1, {230, 240, 255, 255});
        }
    }

    void GameplayController::renderDebugSpawnGhost(Renderer &renderer, const Camera &camera, const InputManager &input, const TileMap &map, BitmapFontRenderer &fontRenderer)
    {
        if(!m_debugMode || !m_spawnMode)
            return;

        int msY = input.getMouseScreenY();
        if(msY < 32 || input.getMouseScreenX() >= camera.getViewportWidth())
            return;

        const auto &catalog = getDebugSpawnCatalog();
        if(m_selectedSpawnIndex < 0 || m_selectedSpawnIndex >= static_cast<int>(catalog.size()))
            return;

        const auto &item = catalog[m_selectedSpawnIndex];
        int tileX = static_cast<int>(input.getMouseWorldX() / TILE_WIDTH);
        int tileY = static_cast<int>(input.getMouseWorldY() / TILE_HEIGHT);

        if(tileX < 0 || tileX + item.width > map.getWidth() || tileY < 0 || tileY + item.height > map.getHeight())
        {
            return;
        }

        float wx = tileX * TILE_WIDTH;
        float wy = tileY * TILE_HEIGHT;
        float ww = item.width * TILE_WIDTH;
        float wh = item.height * TILE_HEIGHT;

        renderer.fillRectWorld(wx, wy, ww, wh, camera, {0, 220, 255, 70});
        renderer.drawRectWorld(wx, wy, ww, wh, camera, {0, 255, 220, 240});

        // Cursor tag
        fontRenderer.drawString("[SPAWN] " + item.name, input.getMouseScreenX() + 14, input.getMouseScreenY() - 10, 1, {255, 240, 100, 255});
    }

} // namespace OpenArtOfWar
