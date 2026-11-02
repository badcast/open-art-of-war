#include "graphics/RenderUtility.hpp"
#include "core/EnvironmentConfig.hpp"
#include "game/GameplayController.hpp"
#include "game/MissionDialogs.hpp"
#include "audio/SoundRequests.hpp"
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

static const std::vector<DebugSpawnItem> &GetDebugSpawnCatalog()
{
    static const std::vector<DebugSpawnItem> s_catalog
        = {// HUMANS (Infantry)
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

bool IsLiveOwnUnit(const Entity *e)
{
    return e && e->alive && e->state != ActionState::Die && !e->isBuilding && e->owner == Players::Local();
}

constexpr int kGroupSlotW = 96;
constexpr int kGroupSlotH = 62;
constexpr int kGroupSlotGap = 6;

} // namespace

void GameplayController::PruneGroups(const EntityManager &entities)
{
    for (auto &g : m_groups)
    {
        g.unitIds.erase(
            std::remove_if(
                g.unitIds.begin(), g.unitIds.end(), [&](int id) { return !IsLiveOwnUnit(entities.GetEntity(id)); }),
            g.unitIds.end());
        if (g.unitIds.empty())
        {
            g.leaderId = 0;
            continue;
        }
        if (std::find(g.unitIds.begin(), g.unitIds.end(), g.leaderId) != g.unitIds.end())
            continue;
        // Leader: the toughest unit of the group
        g.leaderId = g.unitIds.front();
        for (int id : g.unitIds)
        {
            if (entities.GetEntity(id)->maxHp > entities.GetEntity(g.leaderId)->maxHp)
                g.leaderId = id;
        }
    }
}

void GameplayController::SelectGroup(int index, EntityManager &entities)
{
    if (index < 0 || index >= static_cast<int>(m_groups.size()) || m_groups[index].unitIds.empty())
        return;
    m_selectedUnits = m_groups[index].unitIds;
    for (auto &e : entities.GetAllEntities())
        e.selected = false;
    for (int id : m_selectedUnits)
        entities.GetEntity(id)->selected = true;
}

void GameplayController::SelectOwnUnits(
    EntityManager &entities, const Camera &camera, bool onScreen, const std::function<bool(const Unit &)> &pick)
{
    const float zoom = std::max(0.01f, camera.GetZoom());
    const float x0 = camera.GetX(), y0 = camera.GetY();
    const float x1 = x0 + camera.GetViewportWidth() / zoom, y1 = y0 + camera.GetViewportHeight() / zoom;
    m_selectedUnits.clear();
    for (auto &e : entities.GetAllEntities())
    {
        e.selected = false;
        const Unit *u = e.AsUnit();
        if (!u || !IsLiveOwnUnit(&e) || !pick(*u))
            continue;
        const float cx = e.GetUnitCenterPixelX(), cy = e.GetUnitCenterPixelY();
        if (onScreen && (cx < x0 || cx > x1 || cy < y0 || cy > y1))
            continue;
        e.selected = true;
        m_selectedUnits.push_back(e.id);
    }
}

void GameplayController::HandleControlGroupKeys(InputManager &input, EntityManager &entities, Camera &camera)
{
    // ~ (the key left of 1): every own unit on the map
    if (input.IsKeyJustPressed(SDL_SCANCODE_GRAVE))
    {
        SelectOwnUnits(entities, camera, false, [](const Unit &) { return true; });
        return;
    }
    static const SDL_Scancode kKeys[10]
        = {SDL_SCANCODE_1,
           SDL_SCANCODE_2,
           SDL_SCANCODE_3,
           SDL_SCANCODE_4,
           SDL_SCANCODE_5,
           SDL_SCANCODE_6,
           SDL_SCANCODE_7,
           SDL_SCANCODE_8,
           SDL_SCANCODE_9,
           SDL_SCANCODE_0};
    const bool ctrl = input.isKeyPressed(SDL_SCANCODE_LCTRL) || input.isKeyPressed(SDL_SCANCODE_RCTRL);
    for (int k = 0; k < 10; ++k)
    {
        if (!input.IsKeyJustPressed(kKeys[k]))
            continue;
        if (ctrl)
        {
            // Ctrl+N: the selected own units become group N (a unit belongs to one group at a time)
            std::vector<int> units;
            for (int id : m_selectedUnits)
            {
                if (IsLiveOwnUnit(entities.GetEntity(id)))
                    units.push_back(id);
            }
            for (auto &g : m_groups)
            {
                g.unitIds.erase(
                    std::remove_if(
                        g.unitIds.begin(),
                        g.unitIds.end(),
                        [&](int id) { return std::find(units.begin(), units.end(), id) != units.end(); }),
                    g.unitIds.end());
            }
            m_groups[k].unitIds = units;
            m_groups[k].leaderId = 0;
            PruneGroups(entities);
        }
        else
        {
            // N selects the group, pressing it twice quickly brings the camera to its leader
            const std::uint64_t now = SDL_GetTicks();
            if (m_lastGroupKey == k && now - m_lastGroupTicks < 400 && m_groups[k].leaderId > 0)
            {
                const Entity *leader = entities.GetEntity(m_groups[k].leaderId);
                if (leader)
                    camera.CenterOn(leader->GetUnitCenterPixelX(), leader->GetUnitCenterPixelY());
            }
            m_lastGroupKey = k;
            m_lastGroupTicks = now;
            SelectGroup(k, entities);
        }
    }
}

int GameplayController::GroupOfUnits(const std::vector<int> &unitIds) const
{
    if (unitIds.empty())
        return -1;
    for (int k = 0; k < static_cast<int>(m_groups.size()); ++k)
    {
        const auto &g = m_groups[k].unitIds;
        bool all = true;
        for (int id : unitIds)
        {
            if (std::find(g.begin(), g.end(), id) == g.end())
            {
                all = false;
                break;
            }
        }
        if (all)
            return k;
    }
    return -1;
}

void GameplayController::CreateMatchAis(const MissionController &mission, EntityManager &entities, TileMap &map)
{
    m_matchAis.clear();
    if (!mission.IsMatch())
        return;
    const MatchSetup &setup = mission.matchSetup();
    auto hqOf = [&](PlayerId p, int &x, int &y) {
        for (const Entity &e : entities.GetAllEntities())
        {
            if (e.alive && e.isBuilding && e.type == UnitType::Headquarters && e.owner == p)
            {
                x = e.tileX;
                y = e.tileY;
                return true;
            }
        }
        return false;
    };
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        const PlayerSlot &slot = setup.slots[p];
        if (!slot.active || slot.human)
            continue;
        int ownX = 0, ownY = 0;
        if (!hqOf(p, ownX, ownY))
            continue;
        // It goes for the nearest enemy headquarters
        int targetX = -1, targetY = -1;
        float best = 1e9f;
        for (PlayerId q = 0; q < kMaxPlayers; ++q)
        {
            int x = 0, y = 0;
            if (!setup.slots[q].active || !Players::Hostile(p, q) || !hqOf(q, x, y))
                continue;
            const float d = DetMath::Hypot(static_cast<float>(x - ownX), static_cast<float>(y - ownY));
            if (d < best)
            {
                best = d;
                targetX = x;
                targetY = y;
            }
        }
        if (targetX < 0)
            continue;
        auto ai = std::make_unique<AIController>();
        ai->SetPlayer(p);
        ai->SetDifficulty(static_cast<AIDifficulty>(std::min<int>(slot.aiLevel, 3)));
        ai->Init(ownX, ownY, targetX, targetY);
        ai->ApplyHandicap(HandicapPercentOf(slot.handicap));
        ai->RecordStartingBase(entities, map);
        ai->SetSkirmish(true);
        m_matchAis.push_back(std::move(ai));
    }
}

void GameplayController::ResetForMission(
    const MissionController &mission, HUDController &hud, EntityManager &entities, TileMap &map)
{
    // A new mission reuses entity ids: forget the selection and groups of the previous one
    m_missionSerial = mission.GetLoadSerial();
    m_selectedUnits.clear();
    for (auto &g : m_groups)
        g = ControlGroup {};
    if (mission.IsMatch())
        m_sim.startMatch(mission.matchSetup());
    else
        m_sim.Reset();
    hud.ResetForMission();
    // A campaign mission tells its story a few seconds after it starts (MissionDialogs)
    const int missionId = mission.GetCurrentMissionId();
    const bool campaign = !mission.IsMatch() && !mission.IsSkirmish() && missionId >= 0
                          && missionId < MissionController::kCampaignMissionCount;
    m_pendingDialogId = campaign ? MissionDialogs::IdForMission(PlayerFaction(), missionId) : 0;
    m_dialogTimer = 0.0f;
    entities.RebuildBlueprints(); // build menu of the faction picked for this mission
    CreateMatchAis(mission, entities, map);
}

SimCommand GameplayController::Command(CommandType type) const
{
    SimCommand c;
    c.type = type;
    c.player = Players::Local();
    c.debug = m_debugMode && m_controlAllFactions;
    return c;
}

void GameplayController::orderAt(const std::vector<int> &units, float worldX, float worldY, OrderKind kind, bool queued)
{
    SimCommand c = Command(CommandType::OrderAt);
    c.ids.assign(units.begin(), units.end());
    c.x = static_cast<int32_t>(worldX);
    c.y = static_cast<int32_t>(worldY);
    c.a = static_cast<int32_t>(kind);
    // bit 0: march as a control group; bit 1: Shift - after the orders the units already have
    c.b = (units.size() >= 2 && GroupOfUnits(units) >= 0) ? 1 : 0;
    if (queued)
        c.b |= 2;
    m_sim.Issue(std::move(c));
    SoundRequests::Play(kind == OrderKind::AttackMove ? "@attack" : "@move"); // the units answer
}

int GameplayController::GroupBarHit(int mx, int my, int mapViewW, int windowH) const
{
    int count = 0;
    for (const auto &g : m_groups)
        count += g.unitIds.empty() ? 0 : 1;
    if (count == 0)
        return -1;
    const int totalW = count * kGroupSlotW + (count - 1) * kGroupSlotGap;
    int x = (mapViewW - totalW) / 2;
    const int y = windowH - kGroupSlotH - 10;
    for (int k = 0; k < static_cast<int>(m_groups.size()); ++k)
    {
        if (m_groups[k].unitIds.empty())
            continue;
        if (mx >= x && mx < x + kGroupSlotW && my >= y && my < y + kGroupSlotH)
            return k;
        x += kGroupSlotW + kGroupSlotGap;
    }
    return -1;
}

void GameplayController::RenderGroupLeaders(
    Renderer &renderer, const Camera &camera, const EntityManager &entities) const
{
    const SDL_Color gold {255, 214, 64, 255};
    for (const ControlGroup &g : m_groups)
    {
        if (g.unitIds.empty())
            continue;
        bool anySelected = false;
        for (int id : g.unitIds)
        {
            const Entity *e = entities.GetEntity(id);
            anySelected = anySelected || (e && e->selected);
        }
        int leaderId = g.leaderId;
        const AISquad *squad = nullptr;
        for (const auto &sq : m_sim.Squads(Players::Local()).GetSquads())
        {
            if (std::find(sq.unitIds.begin(), sq.unitIds.end(), g.unitIds.front()) != sq.unitIds.end())
            {
                squad = &sq;
                leaderId = sq.leaderId;
                break;
            }
        }
        if (!anySelected && !squad)
            continue;
        const Entity *leader = entities.GetEntity(leaderId);
        if (!leader)
            continue;
        const float lx = leader->GetUnitCenterPixelX();
        const float ly = leader->GetUnitCenterPixelY();

        // Marching: who follows whom (green inside the leash, yellow beyond it)
        if (squad)
        {
            for (int id : squad->unitIds)
            {
                const Entity *u = entities.GetEntity(id);
                if (!u || id == leaderId)
                    continue;
                const float d = DetMath::Hypot(
                    u->tileX - static_cast<float>(leader->tileX), u->tileY - static_cast<float>(leader->tileY));
                const SDL_Color c
                    = d <= squad->leashRadius ? SDL_Color {90, 220, 120, 90} : SDL_Color {255, 200, 60, 140};
                renderer.DrawLineWorld(u->GetUnitCenterPixelX(), u->GetUnitCenterPixelY(), lx, ly, camera, c);
            }
        }

        // Leader marker: small gold triangle above the unit
        float sx = 0.0f, sy = 0.0f;
        camera.WorldToScreen(lx, ly, sx, sy);
        const int top = static_cast<int>(sy - 26.0f * camera.GetZoom());
        for (int i = 0; i < 6; ++i)
        {
            renderer.FillRectScreen(static_cast<int>(sx) - (5 - i), top + i, 2 * (5 - i) + 1, 1, gold);
        }
    }
}

void GameplayController::RenderGroupBar(
    Renderer &renderer,
    BitmapFontRenderer &font,
    const EntityManager &entities,
    int mapViewW,
    int windowH,
    int mx,
    int my) const
{
    int count = 0;
    for (const auto &g : m_groups)
        count += g.unitIds.empty() ? 0 : 1;
    if (count == 0)
        return;

    const int totalW = count * kGroupSlotW + (count - 1) * kGroupSlotGap;
    int x = (mapViewW - totalW) / 2;
    const int y = windowH - kGroupSlotH - 10;
    const SDL_Color top {28, 70, 92, 235};
    const SDL_Color bottom {12, 30, 44, 235};
    const SDL_Color edge {70, 150, 180, 255};

    for (int k = 0; k < static_cast<int>(m_groups.size()); ++k)
    {
        const ControlGroup &g = m_groups[k];
        if (g.unitIds.empty())
            continue;
        const ui::UiRect slot {x, y, kGroupSlotW, kGroupSlotH};
        x += kGroupSlotW + kGroupSlotGap;

        const bool selected = (m_selectedUnits.size() == g.unitIds.size() && GroupOfUnits(m_selectedUnits) == k);
        const bool hovered = slot.Contains(mx, my);
        int leaderId = g.leaderId;
        bool marching = false;
        for (const auto &sq : m_sim.Squads(Players::Local()).GetSquads())
        {
            if (std::find(sq.unitIds.begin(), sq.unitIds.end(), g.leaderId) != sq.unitIds.end()
                || std::find(sq.unitIds.begin(), sq.unitIds.end(), g.unitIds.front()) != sq.unitIds.end())
            {
                leaderId = sq.leaderId;
                marching = true;
                break;
            }
        }

        ui::FillVerticalGradient(renderer, slot, hovered ? ui::Mix(top, edge, 0.25f) : top, bottom);
        const Unit *leader = entities.GetUnit(leaderId);
        if (leader)
        {
            int comp = UnitCameoCompId(leader->archetypeId);
            if (comp >= 0)
                renderer.SetDrawOwner(leader->owner);
            renderer.SetStructureFaction(leader->faction);
            renderer.DrawCompositeScreen(comp, slot.x + (slot.w - 60) / 2 + 6, slot.y + 6, 1.0f, leader->faction);
            renderer.SetStructureFaction(Faction::Neutral);
            renderer.SetDrawOwner(kNoPlayer);
        }

        // Group health
        int hp = 0, maxHp = 0;
        for (int id : g.unitIds)
        {
            // A unit may have died since the groups were pruned at the start of the frame
            const Entity *e = entities.GetEntity(id);
            if (!e)
                continue;
            hp += e->currentHp;
            maxHp += e->maxHp;
        }
        const float ratio = maxHp > 0 ? static_cast<float>(hp) / static_cast<float>(maxHp) : 0.0f;
        const SDL_Color hpCol = ratio > 0.6f ? ui::color::Ok : (ratio > 0.3f ? ui::color::Accent : ui::color::Danger);
        renderer.FillRectScreen(slot.x + 4, slot.Bottom() - 7, slot.w - 8, 3, {6, 10, 14, 220});
        renderer.FillRectScreen(slot.x + 4, slot.Bottom() - 7, static_cast<int>((slot.w - 8) * ratio), 3, hpCol);

        // Key chip, unit count, march indicator
        const std::string key = std::to_string((k + 1) % 10);
        renderer.FillRectScreen(
            slot.x + 3, slot.y + 3, 18, 18, selected ? ui::color::Accent : SDL_Color {8, 20, 30, 230});
        font.DrawString(
            key,
            slot.x + 12,
            slot.y + 12,
            ui::FONT_SMALL_BOLD,
            selected ? ui::color::AccentText : ui::color::Text,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
        font.DrawString(
            "\xC3\x97" + std::to_string(g.unitIds.size()),
            slot.Right() - 6,
            slot.Bottom() - 16,
            ui::FONT_SMALL_BOLD,
            ui::color::Text,
            true,
            Anchor::RIGHT | Anchor::VCENTER);
        if (marching)
        {
            font.DrawString(
                _("MARCH"),
                slot.x + 6,
                slot.Bottom() - 16,
                ui::FONT_SMALL_BOLD,
                ui::color::Info,
                true,
                Anchor::LEFT | Anchor::VCENTER);
        }

        SDL_Color border = selected ? ui::color::Accent : (hovered ? edge : SDL_Color {40, 90, 112, 255});
        renderer.DrawRectScreen(slot.x, slot.y, slot.w, slot.h, border);
        if (selected)
            renderer.DrawRectScreen(
                slot.x + 1, slot.y + 1, slot.w - 2, slot.h - 2, ui::WithAlpha(ui::color::Accent, 110));
    }
}

void GameplayController::SetDebugMode(bool enabled)
{
    m_debugMode = enabled;
    if (enabled)
    {
        m_showNavLines = true;
        m_controlAllFactions = true;
        m_revealFog = true;
        m_showDebugGrid = true;
    }
}

void GameplayController::Update(
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
    input.Update(deltaTime, camera);
    camera.ClampToBounds(map.GetWidth(), map.GetHeight());
    hud.BindSimulation(&m_sim);
    map.SetViewerTeam(Players::TeamOf(Players::Local())); // the fog that is drawn is the local player's

    if (mission.GetLoadSerial() != m_missionSerial)
        ResetForMission(mission, hud, entities, map);
    PruneGroups(entities);

    // Wheel steps of this frame: over the map they zoom (below, only when no window covers the map); steps that
    // arrive while a window is open are dropped so nothing jumps when it closes
    const int mapWheel = input.ConsumeMapWheel();
    const bool windowOpen = menu.IsAnyMenuOpen();
    input.SetMenuOpen(windowOpen);
    if (windowOpen)
        input.ConsumeUiWheel();
    else
        input.ConsumeWheel(); // the generic counter is for menu lists only

    int mx = input.GetMouseScreenX();
    int my = input.GetMouseScreenY();
    float lx = 0.0f, ly = 0.0f;
    bool leftClicked = input.WasLeftClickIssued(lx, ly);
    float rx = 0.0f, ry = 0.0f;
    bool rightClicked = input.WasRightClickIssued(rx, ry);

    // 1. Objectives Dialog
    if (menu.IsObjectivesDialogOpen())
    {
        if (menu.UpdateObjectivesDialog(input, leftClicked))
        {
            // A network match goes on while a player reads its menus
            if (mission.IsOnlineMatch())
                SimulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
            return;
        }
    }

    // 1b. Campaign mission dialogue: the mission waits while it is read
    if (menu.IsMissionDialogOpen() && menu.UpdateMissionDialog(input, mx, my, leftClicked, windowW, windowH))
        return;

    // 2. In-Game Pause Menu
    if (menu.IsInGameMenuOpen())
    {
        if (menu.UpdateInGameMenu(
                input,
                mx,
                my,
                leftClicked,
                windowW,
                windowH,
                sound,
                mission,
                map,
                entities,
                economy,
                pathfinder,
                camera,
                appState))
        {
            if (mission.IsOnlineMatch() && appState == AppState::SkirmishPlaying)
                SimulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
            return;
        }
    }

    // 4. Command sidebar click handling (right edge); the debug toolbar sits above it
    hud.SetTopInset(m_debugMode ? 32 : 0);
    bool openMenu = false;
    bool openObjectives = false;
    bool mapInputConsumed = hud.HandleHudClick(
        mx,
        my,
        leftClicked,
        rightClicked,
        input.IsLeftMouseDown(),
        input.ConsumeUiWheel(),
        windowW,
        windowH,
        economy,
        entities,
        map,
        camera,
        openMenu,
        openObjectives,
        m_selectedUnits);

    if (openMenu)
    {
        menu.SetInGameMenuOpen(true);
    }
    if (openObjectives)
    {
        menu.SetObjectivesDialogOpen(true);
    }

    // Everything left of the sidebar is the map viewport
    const int mapViewW = camera.GetViewportWidth();

    // Control group bar at the bottom of the map: click selects the group
    if (!mapInputConsumed && (leftClicked || rightClicked || input.IsLeftMouseDown()))
    {
        int slot = GroupBarHit(mx, my, mapViewW, windowH);
        if (slot >= 0)
        {
            if (leftClicked)
                SelectGroup(slot, entities);
            mapInputConsumed = true;
            input.CancelSelectionBox();
        }
    }

    // Camera zoom: wheel over the open map, not over the group bar or the debug spawn list
    if (mapWheel != 0 && mx < mapViewW && GroupBarHit(mx, my, mapViewW, windowH) < 0
        && !(m_debugMode && m_spawnDropdownOpen))
    {
        const float zoomDelta = mapWheel > 0 ? 1.15f : (1.0f / 1.15f);
        float zoom = camera.GetZoom();
        for (int i = 0; i < std::abs(mapWheel); ++i)
            zoom *= zoomDelta;
        camera.SetZoom(zoom, static_cast<float>(mx), static_cast<float>(my));
    }

    // 4.4. Debug Spawn Dropdown Click Handling (--debug mode)
    if (m_debugMode && m_spawnDropdownOpen)
    {
        if (leftClicked)
        {
            int dropX = 450;
            int dropY = 32;
            int colW = 170;
            int dropW = colW * 3;
            int dropH = 370;

            bool itemSelected = false;
            if (mx >= dropX && mx <= dropX + dropW && my >= dropY && my <= dropY + dropH)
            {
                // Column 1: HUMANS (items 0..5)
                if (mx < dropX + colW)
                {
                    int itemIdx = (my - (dropY + 24)) / 22;
                    if (itemIdx >= 0 && itemIdx < 6)
                    {
                        m_selectedSpawnIndex = itemIdx;
                        m_spawnMode = true;
                        m_spawnDropdownOpen = false;
                        itemSelected = true;
                    }
                }
                // Column 2: VEHICLES (items 6..13)
                else if (mx < dropX + colW * 2)
                {
                    int itemIdx = (my - (dropY + 24)) / 22;
                    if (itemIdx >= 0 && itemIdx < 8)
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
                    if (itemIdx >= 0 && itemIdx < 16)
                    {
                        m_selectedSpawnIndex = 14 + itemIdx;
                        m_spawnMode = true;
                        m_spawnDropdownOpen = false;
                        itemSelected = true;
                    }
                }
            }
            if (!itemSelected)
            {
                m_spawnDropdownOpen = false;
            }
            mapInputConsumed = true;
        }
        else if (rightClicked || input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            m_spawnDropdownOpen = false;
            mapInputConsumed = true;
        }
    }

    // 4.5. Top Debug Toolbar Click Handling (--debug mode)
    if (m_debugMode && leftClicked && my <= 30)
    {
        if (mx >= 98 && mx <= 198)
        {
            m_showNavLines = !m_showNavLines;
        }
        else if (mx >= 202 && mx <= 322)
        {
            m_controlAllFactions = !m_controlAllFactions;
        }
        else if (mx >= 326 && mx <= 436)
        {
            m_pauseAI = !m_pauseAI;
        }
        else if (mx >= 440 && mx <= 545)
        {
            m_revealFog = !m_revealFog;
        }
        else if (mx >= 549 && mx <= 624)
        {
            SimCommand c = Command(CommandType::DebugCredits);
            c.debug = true;
            c.a = 1000;
            m_sim.Issue(std::move(c));
        }
        else if (mx >= 628 && mx <= 748)
        {
            m_spawnMode = !m_spawnMode;
            if (m_spawnMode)
            {
                m_spawnDropdownOpen = false;
            }
        }
        else if (mx >= 752 && mx <= 960)
        {
            m_spawnDropdownOpen = !m_spawnDropdownOpen;
        }
        else if (mx >= 964 && mx <= 1049)
        {
            SimCommand c = Command(CommandType::ToggleSiege);
            c.ids.assign(m_selectedUnits.begin(), m_selectedUnits.end());
            m_sim.Issue(std::move(c));
        }
        else if (mx >= 1053 && mx <= 1153)
        {
            m_showDebugGrid = !m_showDebugGrid;
        }
        else if (mx >= 1157 && mx <= 1237)
        {
            m_debugOverlayToggleRequested = true;
        }
        mapInputConsumed = true;
    }

    // 4.5.5. Debug Spawn Mode Map Placement
    if (!mapInputConsumed && m_debugMode && m_spawnMode)
    {
        if (rightClicked || input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            m_spawnMode = false;
            mapInputConsumed = true;
        }
        else if (leftClicked && my >= 32 && mx < mapViewW)
        {
            const auto &catalog = GetDebugSpawnCatalog();
            if (m_selectedSpawnIndex >= 0 && m_selectedSpawnIndex < static_cast<int>(catalog.size()))
            {
                const auto &item = catalog[m_selectedSpawnIndex];
                int curTileX = static_cast<int>(lx / TILE_WIDTH);
                int curTileY = static_cast<int>(ly / TILE_HEIGHT);

                SimCommand c = Command(CommandType::DebugSpawn);
                c.debug = true;
                c.player = Players::OwnerOf(item.faction);
                c.a = item.archetypeOrBuildingIdx;
                c.b = static_cast<int32_t>(
                    item.isWall ? DebugSpawnKind::Wall
                                : (item.isBuilding ? DebugSpawnKind::Building : DebugSpawnKind::Unit));
                c.c = item.width;
                c.d = item.height;
                c.x = curTileX;
                c.y = curTileY;
                m_sim.Issue(std::move(c));
            }
            mapInputConsumed = true;
        }
    }

    // 5. Building Placement Mode on the map
    if (!mapInputConsumed && hud.IsBuildMode())
    {
        if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE) || rightClicked)
        {
            hud.SetBuildMode(false);
            mapInputConsumed = true;
        }
        else
        {
            const auto &blueprints = EntityManager::GetBlueprints();
            int selectedBp = hud.GetSelectedBlueprint();
            if (selectedBp >= 0 && selectedBp < static_cast<int>(blueprints.size()))
            {
                const auto &bp = blueprints[selectedBp];
                int curTileX = static_cast<int>(input.GetMouseWorldX() / TILE_WIDTH);
                int curTileY = static_cast<int>(input.GetMouseWorldY() / TILE_HEIGHT);

                if (leftClicked && mx < mapViewW)
                {
                    bool canPlace
                        = entities.CanPlaceBuilding(curTileX, curTileY, bp.width, bp.height, map, Players::Local());
                    bool canAfford = economy.CanAfford(bp.creditCost);
                    if (!canAfford)
                    {
                        SoundRequests::Announce("ann_insufficient_funds");
                    }
                    else if (!canPlace)
                    {
                        SoundRequests::Announce("ann_cannot_build");
                        if (!entities.HasBuildingType(UnitType::Headquarters, Players::Local(), true))
                            hud.SetNotification(_("NO HEADQUARTERS: NOTHING CAN BE BUILT!"));
                    }
                    if (canPlace && canAfford)
                    {
                        // The simulation checks both again when it places it (a network turn later)
                        SimCommand c = Command(CommandType::PlaceBuilding);
                        c.a = bp.blueprintId; // canonical slot: the same on every peer
                        c.x = curTileX;
                        c.y = curTileY;
                        m_sim.Issue(std::move(c));
                        SoundRequests::Play("build_place");
                        if (!bp.isWall)
                            SoundRequests::Announce("ann_building");
                        // Walls are laid one after the other; a building ends the placement
                        if (!bp.isWall)
                            hud.SetBuildMode(false);
                    }
                }
            }
            mapInputConsumed = true;
        }
    }

    // Dragging a selection box ends the attack-move mode: the box then selects as usual
    {
        const auto &dragBox = input.GetSelectionBox();
        if (hud.IsAttackMoveMode() && dragBox.active && dragBox.HasSignificantArea())
            hud.SetAttackMoveMode(false);
    }

    // 6. Attack-Move targeting mode active (triggered by 'Q' key, game does NOT pause!)
    if (!mapInputConsumed && hud.IsAttackMoveMode())
    {
        if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE) || rightClicked)
        {
            hud.SetAttackMoveMode(false);
            mapInputConsumed = true;
        }
        else if (leftClicked && mx < mapViewW)
        {
            std::vector<int> combatUnits;
            for (int uid : m_selectedUnits)
            {
                const Entity *e = entities.GetEntity(uid);
                if (e && e->alive && !e->isBuilding)
                {
                    combatUnits.push_back(uid);
                }
            }

            if (!combatUnits.empty())
            {
                const bool shift = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
                orderAt(combatUnits, lx, ly, OrderKind::AttackMove, shift);
            }
            // With Shift the attack-move mode stays on for the next point of the route
            if (!(input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT)))
                hud.SetAttackMoveMode(false);
            mapInputConsumed = true;
        }
    }

    // 7. Keyboard shortcuts (camera keys are handled by InputManager) and map orders
    if (!mapInputConsumed)
    {
        // 'Q' toggles Attack-Move targeting
        if (input.IsKeyJustPressed(SDL_SCANCODE_Q))
        {
            bool hasCombatUnits = false;
            for (int uid : m_selectedUnits)
            {
                const Entity *e = entities.GetEntity(uid);
                if (e && e->alive && !e->isBuilding)
                {
                    hasCombatUnits = true;
                    break;
                }
            }
            if (hasCombatUnits)
            {
                hud.SetAttackMoveMode(!hud.IsAttackMoveMode());
                hud.SetBuildMode(false);
            }
        }

        HandleControlGroupKeys(input, entities, camera);

        // 'G' - Siege mode toggle
        if (input.IsKeyJustPressed(SDL_SCANCODE_G))
        {
            // A unit whose siege mode waits for a technology says so instead of ignoring the key
            bool locked = false;
            for (int id : m_selectedUnits)
            {
                const Unit *su = entities.GetUnit(id);
                if (su && su->owner == Players::Local() && !entities.CanDeploySiege(*su))
                    locked = true;
            }
            if (locked)
            {
                SoundRequests::Play("ui_error");
                hud.SetNotification(_("RESEARCH SIEGE MODE FIRST!"));
            }
            SimCommand c = Command(CommandType::ToggleSiege);
            c.ids.assign(m_selectedUnits.begin(), m_selectedUnits.end());
            m_sim.Issue(std::move(c));
        }

        // Z / X / C - stances: guard (chase and return), hold ground, pursue
        {
            static const SDL_Scancode kStanceKeys[] = {SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C};
            for (int i = 0; i < 3; ++i)
            {
                if (!input.IsKeyJustPressed(kStanceKeys[i]) || m_selectedUnits.empty())
                    continue;
                SimCommand c = Command(CommandType::SetStance);
                c.a = i;
                c.ids.assign(m_selectedUnits.begin(), m_selectedUnits.end());
                m_sim.Issue(std::move(c));
                SoundRequests::Play("@hold");
            }
        }

        // R - repair on / off for the selected own buildings
        if (input.IsKeyJustPressed(SDL_SCANCODE_R))
        {
            SimCommand c = Command(CommandType::ToggleRepair);
            for (int id : m_selectedUnits)
            {
                const Building *b = entities.GetBuilding(id);
                if (b && b->owner == Players::Local())
                    c.ids.push_back(static_cast<uint32_t>(id));
            }
            if (!c.ids.empty())
                m_sim.Issue(std::move(c));
        }

        // ESC - In-game Pause Menu
        if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            menu.SetInGameMenuOpen(true);
            if (mission.IsOnlineMatch())
                SimulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
            return;
        }

        // Right-Click orders on map (Move / Attack / Attack Wall / Siege Context Menu)
        if (rightClicked && mx < mapViewW && (!m_debugMode || my > 30))
        {
            std::vector<int> combatUnits;
            for (int uid : m_selectedUnits)
            {
                const Entity *e = entities.GetEntity(uid);
                if (e && e->alive && !e->isBuilding)
                {
                    combatUnits.push_back(uid);
                }
            }

            // Only Barracks / Factories selected: right click moves their rally point
            if (combatUnits.empty())
            {
                SimCommand c = Command(CommandType::SetRally);
                c.ids.assign(m_selectedUnits.begin(), m_selectedUnits.end());
                c.x = static_cast<int32_t>(rx);
                c.y = static_cast<int32_t>(ry);
                if (!c.ids.empty())
                    m_sim.Issue(std::move(c));
            }
            else
            {
                // Shift: the point is queued after the current orders (a route, as in other RTS games)
                const bool shift = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
                orderAt(combatUnits, rx, ry, OrderKind::Move, shift);
            }
        }

        // Left-Click single unit selection on map
        if (!hud.IsAttackMoveMode() && leftClicked && mx < mapViewW && (!m_debugMode || my > 30))
        {
            int clickedId = entities.GetUnitAt(lx, ly);
            const Building *clickedBuilding = entities.GetBuilding(clickedId);
            const bool ownProducer
                = clickedBuilding && clickedBuilding->owner == Players::Local() && clickedBuilding->IsUnitProducer();
            const bool shift = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
            const Uint64 now = SDL_GetTicks();
            // Twice on the same unit, or twice on the same spot of empty ground
            const bool samePlace = std::abs(static_cast<int>(mx) - m_lastClickX) <= 8
                                   && std::abs(static_cast<int>(my) - m_lastClickY) <= 8;
            const bool doubleClick
                = clickedId == m_lastClickId && now - m_lastClickTicks < 350 && (clickedId > 0 || samePlace);
            m_lastClickId = clickedId;
            m_lastClickTicks = now;
            m_lastClickX = static_cast<int>(mx);
            m_lastClickY = static_cast<int>(my);
            const Entity *clickedEntity = clickedId > 0 ? entities.GetEntity(clickedId) : nullptr;
            const Unit *clickedUnit = clickedEntity ? clickedEntity->AsUnit() : nullptr;
            const bool ownClickedUnit = clickedUnit && IsLiveOwnUnit(clickedEntity);

            // Shift+click on a Barracks / Factory adds it to the selected ones of the same type
            bool sameTypeSelected = ownProducer && shift;
            if (sameTypeSelected)
            {
                for (int id : m_selectedUnits)
                {
                    const Building *b = entities.GetBuilding(id);
                    if (!b || b->type != clickedBuilding->type)
                        sameTypeSelected = false;
                }
            }
            if (sameTypeSelected)
            {
                Entity *e = entities.GetEntity(clickedId);
                if (e->selected)
                {
                    e->selected = false;
                    m_selectedUnits.erase(
                        std::remove(m_selectedUnits.begin(), m_selectedUnits.end(), clickedId), m_selectedUnits.end());
                }
                else
                {
                    e->selected = true;
                    m_selectedUnits.push_back(clickedId);
                    hud.FocusBuilding(clickedId, entities);
                }
            }
            else
            {
                m_selectedUnits.clear();
                for (auto &e : entities.GetAllEntities())
                    e.selected = false;
                if (doubleClick && ownClickedUnit)
                {
                    // Double click on a unit: every own unit of that kind on the screen
                    const int archetype = clickedUnit->archetypeId;
                    SelectOwnUnits(
                        entities, camera, true, [archetype](const Unit &u) { return u.archetypeId == archetype; });
                }
                else if (doubleClick && clickedId == 0)
                {
                    // Double click on empty ground: every own unit on the screen
                    SelectOwnUnits(entities, camera, true, [](const Unit &) { return true; });
                }
                else if (ownProducer && doubleClick)
                {
                    // Double click: every own building of that type
                    for (auto &e : entities.GetAllEntities())
                    {
                        const Building *b = e.AsBuilding();
                        if (b && b->alive && b->state != ActionState::Die && b->owner == Players::Local()
                            && b->type == clickedBuilding->type)
                        {
                            e.selected = true;
                            m_selectedUnits.push_back(e.id);
                        }
                    }
                    hud.FocusBuilding(clickedId, entities);
                }
                else if (clickedId > 0)
                {
                    Entity *e = entities.GetEntity(clickedId);
                    if (e && (e->owner == Players::Local() || (m_debugMode && m_controlAllFactions)))
                    {
                        e->selected = true;
                        m_selectedUnits.push_back(clickedId);
                        if (e->isBuilding)
                            hud.FocusBuilding(clickedId, entities);
                    }
                }
            }
        }

        // Box selection on map
        const auto &sbox = input.GetSelectionBox();
        if (!hud.IsAttackMoveMode() && sbox.active && sbox.HasSignificantArea() && sbox.startScreenX < mapViewW
            && (!m_debugMode || sbox.startScreenY > 30))
        {
            float minX = std::min(sbox.startWorldX, sbox.currentWorldX);
            float maxX = std::max(sbox.startWorldX, sbox.currentWorldX);
            float minY = std::min(sbox.startWorldY, sbox.currentWorldY);
            float maxY = std::max(sbox.startWorldY, sbox.currentWorldY);
            // The local player's own units: by player, not by faction (two players of one faction would
            // otherwise select the first one's units, whose orders the simulation then refuses)
            const PlayerId selOwner
                = (m_debugMode && m_controlAllFactions) ? Players::OwnerOf(AiFaction()) : Players::Local();
            entities.GetUnitsInBox(minX, minY, maxX, maxY, selOwner, m_selectedUnits);
            if (m_debugMode && m_controlAllFactions && m_selectedUnits.empty())
            {
                entities.GetUnitsInBox(minX, minY, maxX, maxY, Players::Local(), m_selectedUnits);
            }
        }
    }

    // The mission dialogue opens after its delay of playing time (menus and pauses do not count)
    if (m_pendingDialogId != 0)
    {
        m_dialogTimer += deltaTime;
        if (m_dialogTimer >= MissionDialogs::kDelaySeconds)
        {
            menu.OpenMissionDialog(MissionDialogs::Get(m_pendingDialogId));
            m_pendingDialogId = 0;
        }
    }

    if (!m_simHold)
        SimulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
}

void GameplayController::SimulationStep(
    float deltaTime,
    TileMap &map,
    Pathfinder &pathfinder,
    EntityManager &entities,
    Economy &economy,
    AIController &aiController,
    MissionController &mission,
    HUDController &hud,
    AppState &appState)
{
    // 8. Simulation updates: the commands of this tick first, then the world
    SimContext ctx {entities, map, pathfinder, economy, aiController};
    m_sim.ApplyCommands(ctx);
    m_sim.UpdateResearch(deltaTime, ctx);
    hud.Update(deltaTime, entities, economy, m_sim.TakeEvents());
    entities.Update(deltaTime, map, pathfinder);
    // A match: whoever has no building left is out, computer players too (their controllers stop)
    if (mission.IsMatch())
    {
        for (PlayerId out : entities.UpdateElimination(map))
            m_sim.NoteEvent({SimEvent::Type::PlayerEliminated, out, 0});
    }
    m_sim.UpdateOrderQueues(ctx);
    m_sim.UpdateSquads(deltaTime, ctx, aiController.GetChokepoints());
    m_sim.UpdateEconomies(deltaTime, ctx); // the local economy included, in slot order
    // A match has its own computer players (one per computer slot), the mission's commander stays out
    if (mission.IsMatch())
    {
        for (auto &ai : m_matchAis)
        {
            if (!entities.IsEliminated(ai->player()))
                ai->Update(deltaTime, map, entities, pathfinder);
        }
    }
    else if (!m_debugMode || !m_pauseAI)
    {
        aiController.Update(deltaTime, map, entities, pathfinder);
    }
    mission.Update(deltaTime, map, entities, pathfinder);
    m_sim.EndTick();

    // 9. Victory / Defeat check
    if (mission.IsVictory())
    {
        appState = AppState::Victory;
    }
    else if (mission.IsDefeat())
    {
        appState = AppState::Defeat;
    }
}

void GameplayController::RenderSelectionBox(Renderer &renderer, const Camera &camera, const InputManager &input)
{
    (void) camera;
    const auto &sbox = input.GetSelectionBox();
    if (sbox.active && sbox.HasSignificantArea())
    {
        int bx = std::min(sbox.startScreenX, sbox.currentScreenX);
        int by = std::min(sbox.startScreenY, sbox.currentScreenY);
        int bw = std::abs(sbox.currentScreenX - sbox.startScreenX);
        int bh = std::abs(sbox.currentScreenY - sbox.startScreenY);
        renderer.FillRectScreen(bx, by, bw, bh, {0, 255, 0, 45});
        renderer.DrawRectScreen(bx, by, bw, bh, {0, 255, 0, 255});
    }
}

void GameplayController::LoadOverlayStyle(const EnvironmentConfig &env)
{
    const nlohmann::json &o = env.Section("overlay");
    m_overlay = OverlayStyle {};
    if (!o.is_object())
        return;
    m_overlay.opacity = std::clamp(o.value("line_opacity", m_overlay.opacity), 0.0f, 1.0f);
    auto read = [&](const char *key, LineStyle &style) {
        if (!o.contains(key) || !o[key].is_object())
            return;
        style.color = RenderUtility::ParseHexColor(o[key].value("color", std::string()), style.color);
        style.width = std::clamp(o[key].value("width", style.width), 0.5f, 12.0f);
    };
    read("move_line", m_overlay.move);
    read("attack_line", m_overlay.attack);
    read("wall_line", m_overlay.wall);
}

void GameplayController::RenderOrderLines(
    Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map, bool showAll) const
{
    auto drawLine = [&](float x0, float y0, float x1, float y1, const LineStyle &style) {
        SDL_Color c = style.color;
        c.a = static_cast<Uint8>(std::clamp(c.a * m_overlay.opacity, 0.0f, 255.0f));
        RenderUtility::DrawLine(renderer, camera, x0, y0, x1, y1, c, style.width);
    };
    auto drawOrders = [&](const Entity &e) {
        const Unit *u = e.AsUnit();
        if (!u || !u->alive || u->state == ActionState::Die)
            return;
        const float x = u->GetUnitCenterPixelX(), y = u->GetUnitCenterPixelY();
        if (u->targetEntityId != 0)
        {
            const Entity *target = entities.GetEntity(u->targetEntityId);
            if (target && target->alive && target->state != ActionState::Die
                && (target->owner == Players::Local()
                    || entities.IsEntityVisibleToPlayer(*target, Players::Local(), map)))
            {
                drawLine(x, y, target->GetUnitCenterPixelX(), target->GetUnitCenterPixelY(), m_overlay.attack);
            }
        }
        if (u->targetWallTileX >= 0 && u->targetWallTileY >= 0)
        {
            drawLine(
                x,
                y,
                u->targetWallTileX * TILE_WIDTH + TILE_WIDTH * 0.5f,
                u->targetWallTileY * TILE_HEIGHT + TILE_HEIGHT * 0.5f,
                m_overlay.wall);
        }
        // A move / attack-move order: the line ends at the clicked point (not at the unit's own formation tile),
        // then the Shift-queued points follow, each with a small mark
        auto mark = [&](float px, float py, const LineStyle &style) {
            if (&style == &m_overlay.attack)
            {
                // The auto-attack point looks like a rally point (a filled square in a frame), in red
                float sx = 0, sy = 0;
                camera.WorldToScreen(px, py, sx, sy);
                SDL_Color red {255, 60, 60, 150};
                const int m = std::max(4, static_cast<int>(5 * camera.GetZoom()));
                renderer.FillRectScreen(static_cast<int>(sx) - m / 2, static_cast<int>(sy) - m / 2, m, m, red);
                renderer.DrawRectScreen(static_cast<int>(sx) - m, static_cast<int>(sy) - m, 2 * m, 2 * m, red);
                return;
            }
            drawLine(px - 4.0f, py - 4.0f, px + 4.0f, py + 4.0f, style);
            drawLine(px - 4.0f, py + 4.0f, px + 4.0f, py - 4.0f, style);
        };
        float fromX = x, fromY = y;
        if (u->orderX >= 0 && u->orderY >= 0)
        {
            const LineStyle &style = u->isAttackMoving ? m_overlay.attack : m_overlay.move;
            const float px = static_cast<float>(u->orderX), py = static_cast<float>(u->orderY);
            if (u->targetEntityId == 0)
                drawLine(x, y, px, py, style);
            mark(px, py, style);
            fromX = px;
            fromY = py;
        }
        else if (u->HasMoveTarget())
        {
            fromX = u->targetTileX * TILE_WIDTH + TILE_WIDTH * 0.5f;
            fromY = u->targetTileY * TILE_HEIGHT + TILE_HEIGHT * 0.5f;
            drawLine(x, y, fromX, fromY, m_overlay.move);
        }
        for (const Unit::QueuedOrder &q : u->orderQueue)
        {
            const LineStyle &style
                = static_cast<OrderKind>(q.kind) == OrderKind::AttackMove ? m_overlay.attack : m_overlay.move;
            const float px = static_cast<float>(q.x), py = static_cast<float>(q.y);
            drawLine(fromX, fromY, px, py, style);
            mark(px, py, style);
            fromX = px;
            fromY = py;
        }
    };

    if (showAll)
    {
        for (const Entity &e : entities.GetAllEntities())
        {
            // Only the player's own and allied units: the orders of an enemy are not for the eyes of the other side
            if (!e.IsUnitEntity() || e.owner == kNoPlayer || Players::Hostile(e.owner, Players::Local()))
                continue;
            drawOrders(e);
        }
        return;
    }
    for (int uid : m_selectedUnits)
    {
        const Entity *e = entities.GetEntity(uid);
        if (e && e->owner == Players::Local())
            drawOrders(*e);
    }
}

void GameplayController::RenderDebugNavigation(
    Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map)
{
    (void) map;
    if (!m_debugMode || !m_showNavLines)
        return;

    for (int uid : m_selectedUnits)
    {
        const Entity *e = entities.GetEntity(uid);
        if (!e || !e->alive || e->state == ActionState::Die)
            continue;

        float wx = e->GetUnitCenterPixelX();
        float wy = e->GetUnitCenterPixelY();

        // 1. Attack Range Ellipse
        SDL_Color rangeCol = (e->owner == Players::Local()) ? SDL_Color {0, 220, 255, 90} : SDL_Color {255, 60, 60, 90};
        RenderUtility::DrawTileRadius(renderer, camera, wx, wy, e->attackRange, rangeCol, 1.5f);

        // 2. Navigation Path Lines & Waypoints
        const Unit *u = e->AsUnit();
        if (u && !u->path.empty() && u->pathIndex < u->path.size())
        {
            float prevX = wx;
            float prevY = wy;
            for (size_t i = u->pathIndex; i < u->path.size(); ++i)
            {
                float nodeX = u->path[i].x * TILE_WIDTH + (TILE_WIDTH * 0.5f);
                float nodeY = u->path[i].y * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
                renderer.DrawLineWorld(prevX, prevY, nodeX, nodeY, camera, {0, 255, 200, 220});
                renderer.DrawRectWorld(nodeX - 3.0f, nodeY - 3.0f, 6.0f, 6.0f, camera, {0, 255, 200, 255});
                prevX = nodeX;
                prevY = nodeY;
            }
            // Destination flag / marker
            RenderUtility::DrawRadius(renderer, camera, prevX, prevY, 5.0f, SDL_Color {255, 255, 0, 255}, 1.5f);
        }

        // 3. Combat Target Line (Red)
        if (e->targetEntityId != 0)
        {
            const Entity *tgt = entities.GetEntity(e->targetEntityId);
            if (tgt && tgt->alive && tgt->state != ActionState::Die)
            {
                float twx = tgt->GetUnitCenterPixelX();
                float twy = tgt->GetUnitCenterPixelY();
                renderer.DrawLineWorld(wx, wy, twx, twy, camera, {255, 30, 30, 255});
                renderer.DrawRectWorld(twx - 8.0f, twy - 8.0f, 16.0f, 16.0f, camera, {255, 30, 30, 255});
            }
        }

        // 4. Wall Target Line (Orange)
        if (e->targetWallTileX >= 0 && e->targetWallTileY >= 0)
        {
            float twx = e->targetWallTileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
            float twy = e->targetWallTileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
            renderer.DrawLineWorld(wx, wy, twx, twy, camera, {255, 140, 0, 255});
            renderer.DrawRectWorld(twx - 6.0f, twy - 6.0f, 12.0f, 12.0f, camera, {255, 140, 0, 255});
        }

        // 5. Waiting/Blocked indicator
        if (u && u->waitingForPath)
        {
            renderer.FillRectWorld(wx - 10.0f, wy - 24.0f, 20.0f, 6.0f, camera, {255, 180, 0, 220});
        }
    }
}

void GameplayController::RenderDebugToolbar(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW)
{
    if (!m_debugMode)
        return;

    // Background bar
    renderer.FillRectScreen(0, 0, windowW, 30, {16, 20, 28, 240});
    renderer.DrawLineScreen(0, 30, windowW, 30, {0, 200, 255, 180});

    // Button 1: Badge
    renderer.FillRectScreen(4, 3, 90, 24, {20, 70, 40, 255});
    renderer.DrawRectScreen(4, 3, 90, 24, {40, 220, 90, 255});
    fontRenderer.DrawString("--DEBUG--", 12, 7, 1, {40, 255, 90, 255});

    // Button 2: Nav Lines
    SDL_Color navBg = m_showNavLines ? SDL_Color {30, 80, 100, 255} : SDL_Color {40, 40, 50, 255};
    renderer.FillRectScreen(98, 3, 100, 24, navBg);
    renderer.DrawRectScreen(98, 3, 100, 24, {0, 200, 255, 255});
    fontRenderer.DrawString(m_showNavLines ? "NAV: ON" : "NAV: OFF", 108, 7, 1, {255, 255, 255, 255});

    // Button 3: Enemy Control
    SDL_Color ctrlBg = m_controlAllFactions ? SDL_Color {110, 40, 40, 255} : SDL_Color {40, 40, 50, 255};
    renderer.FillRectScreen(202, 3, 120, 24, ctrlBg);
    renderer.DrawRectScreen(202, 3, 120, 24, {255, 100, 100, 255});
    fontRenderer.DrawString(m_controlAllFactions ? "CTRL: ALL" : "CTRL: PLAYER", 210, 7, 1, {255, 255, 255, 255});

    // Button 4: AI Pause
    SDL_Color aiBg = m_pauseAI ? SDL_Color {100, 80, 20, 255} : SDL_Color {40, 40, 50, 255};
    renderer.FillRectScreen(326, 3, 110, 24, aiBg);
    renderer.DrawRectScreen(326, 3, 110, 24, {255, 200, 50, 255});
    fontRenderer.DrawString(m_pauseAI ? "AI: PAUSED" : "AI: ACTIVE", 336, 7, 1, {255, 255, 255, 255});

    // Button 5: Fog of War
    SDL_Color fogBg = m_revealFog ? SDL_Color {40, 80, 60, 255} : SDL_Color {40, 40, 50, 255};
    renderer.FillRectScreen(440, 3, 105, 24, fogBg);
    renderer.DrawRectScreen(440, 3, 105, 24, {100, 255, 150, 255});
    fontRenderer.DrawString(m_revealFog ? "FOG: REVEAL" : "FOG: ON", 448, 7, 1, {255, 255, 255, 255});

    // Button 6: +$1000
    renderer.FillRectScreen(549, 3, 75, 24, {30, 60, 30, 255});
    renderer.DrawRectScreen(549, 3, 75, 24, {100, 255, 100, 255});
    fontRenderer.DrawString("+$1000", 559, 7, 1, {120, 255, 120, 255});

    // Button 7: SPAWN MODE Toggle
    SDL_Color spawnBg = m_spawnMode ? SDL_Color {25, 100, 50, 255} : SDL_Color {40, 40, 50, 255};
    SDL_Color spawnBorder = m_spawnMode ? SDL_Color {40, 255, 100, 255} : SDL_Color {120, 140, 160, 255};
    renderer.FillRectScreen(628, 3, 120, 24, spawnBg);
    renderer.DrawRectScreen(628, 3, 120, 24, spawnBorder);
    fontRenderer.DrawString(m_spawnMode ? "SPAWN: ON" : "SPAWN: OFF", 638, 7, 1, {255, 255, 255, 255});

    // Button 8: SPAWN Dropdown Combobox
    const auto &catalog = GetDebugSpawnCatalog();
    std::string selName = "SELECT ENTITY";
    Faction selFac = Faction::Resistance;
    if (m_selectedSpawnIndex >= 0 && m_selectedSpawnIndex < static_cast<int>(catalog.size()))
    {
        selName = catalog[m_selectedSpawnIndex].name;
        selFac = catalog[m_selectedSpawnIndex].faction;
    }
    renderer.FillRectScreen(752, 3, 208, 24, {28, 36, 52, 255});
    renderer.DrawRectScreen(752, 3, 208, 24, {0, 200, 255, 255});
    // Faction indicator pip
    SDL_Color facColor
        = (selFac == Faction::Confederation) ? SDL_Color {0, 200, 255, 255} : SDL_Color {255, 80, 80, 255};
    renderer.FillRectScreen(758, 10, 8, 10, facColor);
    std::string comboLabel = selName + " v";
    fontRenderer.DrawString(comboLabel, 772, 7, 1, {255, 255, 255, 255});

    // Button 9: Siege Mode Toggle
    renderer.FillRectScreen(964, 3, 85, 24, {60, 50, 20, 255});
    renderer.DrawRectScreen(964, 3, 85, 24, {255, 200, 80, 255});
    fontRenderer.DrawString("SIEGE(G)", 972, 7, 1, {255, 220, 100, 255});

    // Button 10: Passability Grid (70% Transparency Green/Red)
    SDL_Color gridBg = m_showDebugGrid ? SDL_Color {25, 90, 45, 255} : SDL_Color {40, 40, 50, 255};
    SDL_Color gridBorder = m_showDebugGrid ? SDL_Color {40, 255, 100, 255} : SDL_Color {120, 140, 160, 255};
    renderer.FillRectScreen(1053, 3, 100, 24, gridBg);
    renderer.DrawRectScreen(1053, 3, 100, 24, gridBorder);
    fontRenderer.DrawString(m_showDebugGrid ? "GRID: ON" : "GRID: OFF", 1063, 7, 1, {255, 255, 255, 255});

    // Button 11: Dear ImGui inspector overlay
    renderer.FillRectScreen(1157, 3, 80, 24, {40, 40, 50, 255});
    renderer.DrawRectScreen(1157, 3, 80, 24, {180, 140, 255, 255});
    fontRenderer.DrawString("IMGUI", 1175, 7, 1, {255, 255, 255, 255});
}

void GameplayController::RenderDebugGrid(
    Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map)
{
    if (!m_debugMode || !m_showDebugGrid)
        return;

    float startWorldX, startWorldY, endWorldX, endWorldY;
    camera.ScreenToWorld(0, 0, startWorldX, startWorldY);
    camera.ScreenToWorld(camera.GetViewportWidth(), camera.GetViewportHeight(), endWorldX, endWorldY);

    int startTileX = std::clamp(static_cast<int>(startWorldX / TILE_WIDTH) - 1, 0, map.GetWidth() - 1);
    int endTileX = std::clamp(static_cast<int>(endWorldX / TILE_WIDTH) + 2, 0, map.GetWidth());
    int startTileY = std::clamp(static_cast<int>(startWorldY / TILE_HEIGHT) - 1, 0, map.GetHeight() - 1);
    int endTileY = std::clamp(static_cast<int>(endWorldY / TILE_HEIGHT) + 2, 0, map.GetHeight());

    // User requirement: "green at 70% opacity - walkable ground, red at 70% - blocked paths (buildings included)"
    // 70% transparency = 30% alpha (~76)
    const SDL_Color passableCol {0, 220, 0, 76};
    const SDL_Color impassableCol {220, 0, 0, 76};

    for (int ty = startTileY; ty < endTileY; ++ty)
    {
        for (int tx = startTileX; tx < endTileX; ++tx)
        {
            bool passable = map.IsPassable(tx, ty) && (map.GetWall(tx, ty) == 0);
            if (passable)
            {
                // Check if tile is covered by any stationary living building
                for (const auto &e : entities.GetAllEntities())
                {
                    const Building *b = e.AsBuilding();
                    if (!b || !b->alive || b->state == ActionState::Die)
                        continue;
                    if (tx >= b->tileX && tx < b->tileX + b->buildWidth && ty >= b->tileY
                        && ty < b->tileY + b->buildHeight)
                    {
                        passable = false;
                        break;
                    }
                }
            }

            SDL_Color color = passable ? passableCol : impassableCol;
            // "drawn as a filled rectangle shrunk by 2 pixels on every side (like a grid)"
            float rx = tx * TILE_WIDTH + 2.0f;
            float ry = ty * TILE_HEIGHT + 2.0f;
            float rw = TILE_WIDTH - 4.0f;
            float rh = TILE_HEIGHT - 4.0f;
            renderer.FillRectWorld(rx, ry, rw, rh, camera, color);
        }
    }
}

void GameplayController::RenderDebugSpawnDropdown(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowH)
{
    (void) windowH;
    if (!m_debugMode || !m_spawnDropdownOpen)
        return;

    int dropX = 450;
    int dropY = 32;
    int colW = 170;
    int dropW = colW * 3;
    int dropH = 370;

    // Dropdown background box
    renderer.FillRectScreen(dropX, dropY, dropW, dropH, {16, 22, 32, 252});
    renderer.DrawRectScreen(dropX, dropY, dropW, dropH, {0, 200, 255, 255});
    renderer.DrawLineScreen(dropX + colW, dropY, dropX + colW, dropY + dropH, {40, 60, 80, 200});
    renderer.DrawLineScreen(dropX + colW * 2, dropY, dropX + colW * 2, dropY + dropH, {40, 60, 80, 200});

    const auto &catalog = GetDebugSpawnCatalog();

    // Column 1: HUMANS (items 0..5)
    fontRenderer.DrawString("-- HUMANS --", dropX + 10, dropY + 6, 1, {255, 220, 80, 255});
    for (int i = 0; i < 6 && i < static_cast<int>(catalog.size()); ++i)
    {
        int itemY = dropY + 26 + i * 22;
        if (i == m_selectedSpawnIndex)
        {
            renderer.FillRectScreen(dropX + 4, itemY - 2, colW - 8, 20, {20, 70, 90, 255});
            renderer.DrawRectScreen(dropX + 4, itemY - 2, colW - 8, 20, {0, 255, 220, 255});
        }
        SDL_Color fc = (catalog[i].faction == Faction::Confederation) ? SDL_Color {0, 200, 255, 255}
                                                                      : SDL_Color {255, 80, 80, 255};
        renderer.FillRectScreen(dropX + 8, itemY + 4, 6, 8, fc);
        fontRenderer.DrawString(catalog[i].name, dropX + 18, itemY, 1, {230, 240, 255, 255});
    }

    // Column 2: VEHICLES (items 6..13)
    fontRenderer.DrawString("-- VEHICLES --", dropX + colW + 10, dropY + 6, 1, {255, 220, 80, 255});
    for (int i = 6; i < 14 && i < static_cast<int>(catalog.size()); ++i)
    {
        int itemY = dropY + 26 + (i - 6) * 22;
        if (i == m_selectedSpawnIndex)
        {
            renderer.FillRectScreen(dropX + colW + 4, itemY - 2, colW - 8, 20, {20, 70, 90, 255});
            renderer.DrawRectScreen(dropX + colW + 4, itemY - 2, colW - 8, 20, {0, 255, 220, 255});
        }
        SDL_Color fc = (catalog[i].faction == Faction::Confederation) ? SDL_Color {0, 200, 255, 255}
                                                                      : SDL_Color {255, 80, 80, 255};
        renderer.FillRectScreen(dropX + colW + 8, itemY + 4, 6, 8, fc);
        fontRenderer.DrawString(catalog[i].name, dropX + colW + 18, itemY, 1, {230, 240, 255, 255});
    }

    // Column 3: BUILDINGS (items 14..29)
    fontRenderer.DrawString("-- BUILDINGS --", dropX + colW * 2 + 10, dropY + 6, 1, {255, 220, 80, 255});
    for (int i = 14; i < 30 && i < static_cast<int>(catalog.size()); ++i)
    {
        int itemY = dropY + 26 + (i - 14) * 20;
        if (i == m_selectedSpawnIndex)
        {
            renderer.FillRectScreen(dropX + colW * 2 + 4, itemY - 1, colW - 8, 18, {20, 70, 90, 255});
            renderer.DrawRectScreen(dropX + colW * 2 + 4, itemY - 1, colW - 8, 18, {0, 255, 220, 255});
        }
        SDL_Color fc = (catalog[i].faction == Faction::Confederation) ? SDL_Color {0, 200, 255, 255}
                                                                      : SDL_Color {255, 80, 80, 255};
        renderer.FillRectScreen(dropX + colW * 2 + 8, itemY + 3, 6, 8, fc);
        fontRenderer.DrawString(catalog[i].name, dropX + colW * 2 + 18, itemY, 1, {230, 240, 255, 255});
    }
}

void GameplayController::RenderDebugSpawnGhost(
    Renderer &renderer,
    const Camera &camera,
    const InputManager &input,
    const TileMap &map,
    BitmapFontRenderer &fontRenderer)
{
    if (!m_debugMode || !m_spawnMode)
        return;

    int msY = input.GetMouseScreenY();
    if (msY < 32 || input.GetMouseScreenX() >= camera.GetViewportWidth())
        return;

    const auto &catalog = GetDebugSpawnCatalog();
    if (m_selectedSpawnIndex < 0 || m_selectedSpawnIndex >= static_cast<int>(catalog.size()))
        return;

    const auto &item = catalog[m_selectedSpawnIndex];
    int tileX = static_cast<int>(input.GetMouseWorldX() / TILE_WIDTH);
    int tileY = static_cast<int>(input.GetMouseWorldY() / TILE_HEIGHT);

    if (tileX < 0 || tileX + item.width > map.GetWidth() || tileY < 0 || tileY + item.height > map.GetHeight())
    {
        return;
    }

    float wx = tileX * TILE_WIDTH;
    float wy = tileY * TILE_HEIGHT;
    float ww = item.width * TILE_WIDTH;
    float wh = item.height * TILE_HEIGHT;

    renderer.FillRectWorld(wx, wy, ww, wh, camera, {0, 220, 255, 70});
    renderer.DrawRectWorld(wx, wy, ww, wh, camera, {0, 255, 220, 240});

    // Cursor tag
    fontRenderer.DrawString(
        "[SPAWN] " + item.name, input.GetMouseScreenX() + 14, input.GetMouseScreenY() - 10, 1, {255, 240, 100, 255});
}

} // namespace OpenArtOfWar
