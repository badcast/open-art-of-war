#include "graphics/RenderUtility.hpp"
#include "core/EnvironmentConfig.hpp"
#include "game/GameplayController.hpp"
#include "game/BuildLimits.hpp"
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
#include "game/Mines.hpp"
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

bool IsLiveOwnUnit(const Entity *entity)
{
    return entity && entity->alive && entity->state != ActionState::Die && !entity->isBuilding
           && entity->owner == Players::Local();
}

constexpr int kGroupSlotWidth = 96;
constexpr int kGroupSlotHeight = 62;
constexpr int kGroupSlotGap = 6;

} // namespace

void GameplayController::PruneGroups(const EntityManager &entities)
{
    for (auto &group : m_groups)
    {
        group.unitIds.erase(
            std::remove_if(
                group.unitIds.begin(),
                group.unitIds.end(),
                [&](int unitId) { return !IsLiveOwnUnit(entities.GetEntity(unitId)); }),
            group.unitIds.end());
        if (group.unitIds.empty())
        {
            group.leaderId = 0;
            continue;
        }
        if (std::find(group.unitIds.begin(), group.unitIds.end(), group.leaderId) != group.unitIds.end())
            continue;
        // Leader: the toughest unit of the group
        group.leaderId = group.unitIds.front();
        for (int unitId : group.unitIds)
        {
            if (entities.GetEntity(unitId)->maxHp > entities.GetEntity(group.leaderId)->maxHp)
                group.leaderId = unitId;
        }
    }
}

void GameplayController::SelectGroup(int index, EntityManager &entities)
{
    if (index < 0 || index >= static_cast<int>(m_groups.size()) || m_groups[index].unitIds.empty())
        return;
    m_selectedUnits = m_groups[index].unitIds;
    for (auto &entity : entities.GetAllEntities())
        entity.selected = false;
    for (int unitId : m_selectedUnits)
        entities.GetEntity(unitId)->selected = true;
}

void GameplayController::SelectOwnUnits(
    EntityManager &entities, const Camera &camera, bool onScreen, const std::function<bool(const Unit &)> &pick)
{
    const float zoom = std::max(0.01f, camera.GetZoom());
    const float viewLeft = camera.GetX(), viewTop = camera.GetY();
    const float viewRight = viewLeft + camera.GetViewportWidth() / zoom;
    const float viewBottom = viewTop + camera.GetViewportHeight() / zoom;
    m_selectedUnits.clear();
    for (auto &entity : entities.GetAllEntities())
    {
        entity.selected = false;
        const Unit *unit = entity.AsUnit();
        if (!unit || !IsLiveOwnUnit(&entity) || !pick(*unit))
            continue;
        const float centerX = entity.GetUnitCenterPixelX(), centerY = entity.GetUnitCenterPixelY();
        if (onScreen && (centerX < viewLeft || centerX > viewRight || centerY < viewTop || centerY > viewBottom))
            continue;
        entity.selected = true;
        m_selectedUnits.push_back(entity.id);
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
    static const SDL_Scancode kGroupKeys[10]
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
    const bool ctrlHeld = input.isKeyPressed(SDL_SCANCODE_LCTRL) || input.isKeyPressed(SDL_SCANCODE_RCTRL);
    for (int groupIndex = 0; groupIndex < 10; ++groupIndex)
    {
        if (!input.IsKeyJustPressed(kGroupKeys[groupIndex]))
            continue;
        if (ctrlHeld)
        {
            // Ctrl+N: the selected own units become group N (a unit belongs to one group at a time)
            std::vector<int> units;
            for (int unitId : m_selectedUnits)
            {
                if (IsLiveOwnUnit(entities.GetEntity(unitId)))
                    units.push_back(unitId);
            }
            for (auto &group : m_groups)
            {
                group.unitIds.erase(
                    std::remove_if(
                        group.unitIds.begin(),
                        group.unitIds.end(),
                        [&](int unitId) { return std::find(units.begin(), units.end(), unitId) != units.end(); }),
                    group.unitIds.end());
            }
            m_groups[groupIndex].unitIds = units;
            m_groups[groupIndex].leaderId = 0;
            PruneGroups(entities);
        }
        else
        {
            // N selects the group, pressing it twice quickly brings the camera to its leader
            const std::uint64_t nowTicks = SDL_GetTicks();
            if (m_lastGroupKey == groupIndex && nowTicks - m_lastGroupTicks < 400 && m_groups[groupIndex].leaderId > 0)
            {
                const Entity *leader = entities.GetEntity(m_groups[groupIndex].leaderId);
                if (leader)
                    camera.CenterOn(leader->GetUnitCenterPixelX(), leader->GetUnitCenterPixelY());
            }
            m_lastGroupKey = groupIndex;
            m_lastGroupTicks = nowTicks;
            SelectGroup(groupIndex, entities);
        }
    }
}

int GameplayController::GroupOfUnits(const std::vector<int> &unitIds) const
{
    if (unitIds.empty())
        return -1;
    for (int groupIndex = 0; groupIndex < static_cast<int>(m_groups.size()); ++groupIndex)
    {
        const auto &groupUnitIds = m_groups[groupIndex].unitIds;
        bool allInGroup = true;
        for (int unitId : unitIds)
        {
            if (std::find(groupUnitIds.begin(), groupUnitIds.end(), unitId) == groupUnitIds.end())
            {
                allInGroup = false;
                break;
            }
        }
        if (allInGroup)
            return groupIndex;
    }
    return -1;
}

void GameplayController::CreateMatchAis(const MissionController &mission, EntityManager &entities, TileMap &map)
{
    m_matchAis.clear();
    m_sim.SetComputerPlayers({}, mission.IsMatch());
    if (!mission.IsMatch() || !m_sim.AdvancesWorld())
        return;
    const MatchSetup &setup = mission.matchSetup();
    auto findHqTile = [&](PlayerId playerId, int &hqTileX, int &hqTileY) {
        for (const Entity &entity : entities.GetAllEntities())
        {
            if (entity.alive && entity.isBuilding && entity.type == UnitType::Headquarters && entity.owner == playerId)
            {
                hqTileX = entity.tileX;
                hqTileY = entity.tileY;
                return true;
            }
        }
        return false;
    };
    for (PlayerId playerId = 0; playerId < kMaxPlayers; ++playerId)
    {
        const PlayerSlot &slot = setup.slots[playerId];
        if (!slot.active || slot.human)
            continue;
        int ownX = 0, ownY = 0;
        if (!findHqTile(playerId, ownX, ownY))
            continue;
        // It goes for the nearest enemy headquarters
        int targetX = -1, targetY = -1;
        float bestDist = 1e9f;
        for (PlayerId otherPlayerId = 0; otherPlayerId < kMaxPlayers; ++otherPlayerId)
        {
            int otherHqX = 0, otherHqY = 0;
            if (!setup.slots[otherPlayerId].active || !Players::Hostile(playerId, otherPlayerId)
                || !findHqTile(otherPlayerId, otherHqX, otherHqY))
                continue;
            const float dist = DetMath::Hypot(static_cast<float>(otherHqX - ownX), static_cast<float>(otherHqY - ownY));
            if (dist < bestDist)
            {
                bestDist = dist;
                targetX = otherHqX;
                targetY = otherHqY;
            }
        }
        if (targetX < 0)
            continue;
        auto ai = std::make_unique<AIController>();
        ai->SetPlayer(playerId);
        ai->SetDifficulty(static_cast<AIDifficulty>(std::min<int>(slot.aiLevel, 3)));
        ai->Init(ownX, ownY, targetX, targetY);
        // Its own style of base: the seed of the match mixed with the slot (two computer players build differently)
        ai->SetSeed(setup.seed ^ (static_cast<std::uint32_t>(playerId) + 1u) * 0x9E3779B9u);
        ai->ApplyHandicap(HandicapPercentOf(slot.handicap));
        ai->RecordStartingBase(entities, map);
        ai->SetSkirmish(true);
        m_matchAis.push_back(std::move(ai));
    }
    std::vector<const AIController *> players;
    for (const auto &ai : m_matchAis)
        players.push_back(ai.get());
    m_sim.SetComputerPlayers(std::move(players), true);
}

void GameplayController::ResetForMission(
    const MissionController &mission, HUDController &hud, EntityManager &entities, TileMap &map)
{
    // A new mission reuses entity ids: forget the selection and groups of the previous one
    m_missionSerial = mission.GetLoadSerial();
    m_selectedUnits.clear();
    for (auto &group : m_groups)
        group = ControlGroup {};
    if (mission.IsMatch())
        m_sim.startMatch(mission.matchSetup());
    else
        m_sim.Reset();
    hud.ResetForMission();
    // A campaign mission tells its story a few seconds after it starts (MissionDialogs)
    const int missionId = mission.GetCurrentMissionId();
    const bool isCampaign = !mission.IsMatch() && !mission.IsSkirmish() && missionId >= 0
                            && missionId < MissionController::kCampaignMissionCount;
    m_pendingDialogId = isCampaign ? MissionDialogs::IdForMission(PlayerFaction(), missionId) : 0;
    m_dialogTimer = 0.0f;
    entities.RebuildBlueprints(); // build menu of the faction picked for this mission
    CreateMatchAis(mission, entities, map);
}

const AIController *GameplayController::MatchAi(PlayerId player) const
{
    for (const auto &ai : m_matchAis)
        if (ai && ai->player() == player)
            return ai.get();
    return nullptr;
}

ResearchProgress GameplayController::ResearchOf(PlayerId player) const
{
    if (const AIController *ai = m_sim.AdvancesWorld() ? MatchAi(player) : nullptr)
    {
        ResearchProgress progress;
        if (ai->IsResearchActive())
        {
            progress.active = ai->GetResearchIndex();
            progress.duration = 1.0f;
            progress.timer = ai->GetResearchProgress();
        }
        return progress;
    }
    return m_sim.research(player);
}

SimCommand GameplayController::Command(CommandType type) const
{
    SimCommand command;
    command.type = type;
    command.player = Players::Local();
    command.debug = m_debugMode && m_controlAllFactions;
    return command;
}

void GameplayController::OrderAt(const std::vector<int> &units, float worldX, float worldY, OrderKind kind, bool queued)
{
    SimCommand command = Command(CommandType::OrderAt);
    command.ids.assign(units.begin(), units.end());
    command.x = static_cast<std::int32_t>(worldX);
    command.y = static_cast<std::int32_t>(worldY);
    command.a = static_cast<std::int32_t>(kind);
    // bit 0: march as a group (only a control group: the column keeps together at the pace of its slowest,
    // stragglers catch up at their own speed); bit 1: Shift - after the orders the units have
    command.b = (units.size() >= 2 && GroupOfUnits(units) >= 0) ? 1 : 0;
    if (queued)
        command.b |= 2;
    m_sim.Issue(std::move(command));
    SoundRequests::Play(kind == OrderKind::AttackMove ? "@attack" : "@move"); // the units answer
}

int GameplayController::GroupBarHit(int mouseX, int mouseY, int mapViewWidth, int windowHeight) const
{
    int count = 0;
    for (const auto &group : m_groups)
        count += group.unitIds.empty() ? 0 : 1;
    if (count == 0)
        return -1;
    const int totalWidth = count * kGroupSlotWidth + (count - 1) * kGroupSlotGap;
    int slotX = (mapViewWidth - totalWidth) / 2;
    const int slotY = windowHeight - kGroupSlotHeight - 10;
    for (int groupIndex = 0; groupIndex < static_cast<int>(m_groups.size()); ++groupIndex)
    {
        if (m_groups[groupIndex].unitIds.empty())
            continue;
        if (mouseX >= slotX && mouseX < slotX + kGroupSlotWidth && mouseY >= slotY && mouseY < slotY + kGroupSlotHeight)
            return groupIndex;
        slotX += kGroupSlotWidth + kGroupSlotGap;
    }
    return -1;
}

void GameplayController::RenderGroupLeaders(
    Renderer &renderer, const Camera &camera, const EntityManager &entities) const
{
    const SDL_Color gold {255, 214, 64, 255};
    for (const ControlGroup &group : m_groups)
    {
        if (group.unitIds.empty())
            continue;
        bool anySelected = false;
        for (int unitId : group.unitIds)
        {
            const Entity *entity = entities.GetEntity(unitId);
            anySelected = anySelected || (entity && entity->selected);
        }
        int leaderId = group.leaderId;
        const AISquad *squad = nullptr;
        for (const auto &candidateSquad : m_sim.Squads(Players::Local()).GetSquads())
        {
            if (std::find(candidateSquad.unitIds.begin(), candidateSquad.unitIds.end(), group.unitIds.front())
                != candidateSquad.unitIds.end())
            {
                squad = &candidateSquad;
                leaderId = candidateSquad.leaderId;
                break;
            }
        }
        if (!anySelected && !squad)
            continue;
        const Entity *leader = entities.GetEntity(leaderId);
        if (!leader)
            continue;
        const float leaderCenterX = leader->GetUnitCenterPixelX();
        const float leaderCenterY = leader->GetUnitCenterPixelY();

        // Marching: who follows whom (green inside the leash, yellow beyond it)
        if (squad)
        {
            for (int unitId : squad->unitIds)
            {
                const Entity *memberEntity = entities.GetEntity(unitId);
                if (!memberEntity || unitId == leaderId)
                    continue;
                const float dist = DetMath::Hypot(
                    memberEntity->tileX - static_cast<float>(leader->tileX),
                    memberEntity->tileY - static_cast<float>(leader->tileY));
                const SDL_Color lineColor
                    = dist <= squad->leashRadius ? SDL_Color {90, 220, 120, 90} : SDL_Color {255, 200, 60, 140};
                renderer.DrawLineWorld(
                    memberEntity->GetUnitCenterPixelX(),
                    memberEntity->GetUnitCenterPixelY(),
                    leaderCenterX,
                    leaderCenterY,
                    camera,
                    lineColor);
            }
        }

        // Leader marker: small gold triangle above the unit
        float screenX = 0.0f, screenY = 0.0f;
        camera.WorldToScreen(leaderCenterX, leaderCenterY, screenX, screenY);
        const int markerTopY = static_cast<int>(screenY - 26.0f * camera.GetZoom());
        for (int row = 0; row < 6; ++row)
        {
            renderer.FillRectScreen(
                static_cast<int>(screenX) - (5 - row), markerTopY + row, 2 * (5 - row) + 1, 1, gold);
        }
    }
}

void GameplayController::RenderGroupBar(
    Renderer &renderer,
    BitmapFontRenderer &font,
    const EntityManager &entities,
    int mapViewWidth,
    int windowHeight,
    int mouseX,
    int mouseY) const
{
    int count = 0;
    for (const auto &group : m_groups)
        count += group.unitIds.empty() ? 0 : 1;
    if (count == 0)
        return;

    const int totalWidth = count * kGroupSlotWidth + (count - 1) * kGroupSlotGap;
    int slotX = (mapViewWidth - totalWidth) / 2;
    const int slotY = windowHeight - kGroupSlotHeight - 10;
    const SDL_Color topColor {28, 70, 92, 235};
    const SDL_Color bottomColor {12, 30, 44, 235};
    const SDL_Color edgeColor {70, 150, 180, 255};

    for (int groupIndex = 0; groupIndex < static_cast<int>(m_groups.size()); ++groupIndex)
    {
        const ControlGroup &group = m_groups[groupIndex];
        if (group.unitIds.empty())
            continue;
        const ui::UiRect slot {slotX, slotY, kGroupSlotWidth, kGroupSlotHeight};
        slotX += kGroupSlotWidth + kGroupSlotGap;

        const bool selected
            = (m_selectedUnits.size() == group.unitIds.size() && GroupOfUnits(m_selectedUnits) == groupIndex);
        const bool hovered = slot.Contains(mouseX, mouseY);
        int leaderId = group.leaderId;
        bool marching = false;
        for (const auto &squad : m_sim.Squads(Players::Local()).GetSquads())
        {
            if (std::find(squad.unitIds.begin(), squad.unitIds.end(), group.leaderId) != squad.unitIds.end()
                || std::find(squad.unitIds.begin(), squad.unitIds.end(), group.unitIds.front()) != squad.unitIds.end())
            {
                leaderId = squad.leaderId;
                marching = true;
                break;
            }
        }

        ui::FillVerticalGradient(renderer, slot, hovered ? ui::Mix(topColor, edgeColor, 0.25f) : topColor, bottomColor);
        const Unit *leader = entities.GetUnit(leaderId);
        if (leader)
        {
            int cameoCompId = UnitCameoCompId(leader->archetypeId);
            if (cameoCompId >= 0)
                renderer.SetDrawOwner(leader->owner);
            renderer.SetStructureFaction(leader->faction);
            renderer.DrawCompositeScreen(
                cameoCompId, slot.x + (slot.w - 60) / 2 + 6, slot.y + 6, 1.0f, leader->faction);
            renderer.SetStructureFaction(Faction::Neutral);
            renderer.SetDrawOwner(kNoPlayer);
        }

        // Group health
        int totalHp = 0, totalMaxHp = 0;
        for (int unitId : group.unitIds)
        {
            // A unit may have died since the groups were pruned at the start of the frame
            const Entity *entity = entities.GetEntity(unitId);
            if (!entity)
                continue;
            totalHp += entity->currentHp;
            totalMaxHp += entity->maxHp;
        }
        const float hpRatio = totalMaxHp > 0 ? static_cast<float>(totalHp) / static_cast<float>(totalMaxHp) : 0.0f;
        const SDL_Color hpColor
            = hpRatio > 0.6f ? ui::color::Ok : (hpRatio > 0.3f ? ui::color::Accent : ui::color::Danger);
        renderer.FillRectScreen(slot.x + 4, slot.Bottom() - 7, slot.w - 8, 3, {6, 10, 14, 220});
        renderer.FillRectScreen(slot.x + 4, slot.Bottom() - 7, static_cast<int>((slot.w - 8) * hpRatio), 3, hpColor);

        // Key chip, unit count, march indicator
        const std::string keyLabel = std::to_string((groupIndex + 1) % 10);
        renderer.FillRectScreen(
            slot.x + 3, slot.y + 3, 18, 18, selected ? ui::color::Accent : SDL_Color {8, 20, 30, 230});
        font.DrawString(
            keyLabel,
            slot.x + 12,
            slot.y + 12,
            ui::FONT_SMALL_BOLD,
            selected ? ui::color::AccentText : ui::color::Text,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
        font.DrawString(
            "\xC3\x97" + std::to_string(group.unitIds.size()),
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

        SDL_Color borderColor = selected ? ui::color::Accent : (hovered ? edgeColor : SDL_Color {40, 90, 112, 255});
        renderer.DrawRectScreen(slot.x, slot.y, slot.w, slot.h, borderColor);
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

void GameplayController::Update(const FrameContext &context)
{
    const float deltaTime = context.deltaTime;
    const int windowWidth = context.windowW;
    const int windowHeight = context.windowH;
    InputManager &input = context.input;
    Camera &camera = context.camera;
    TileMap &map = context.map;
    Pathfinder &pathfinder = context.pathfinder;
    EntityManager &entities = context.entities;
    Economy &economy = context.economy;
    AIController &aiController = context.ai;
    MissionController &mission = context.mission;
    HUDController &hud = context.hud;
    MenuController &menu = context.menu;
    AppState &appState = context.state;

    input.Update(deltaTime, camera);
    camera.ClampToBounds(map.GetWidth(), map.GetHeight());
    hud.BindSimulation(&m_sim);
    map.SetViewerTeam(Players::TeamOf(Players::Local())); // the fog that is drawn is the local player's

    if (mission.GetLoadSerial() != m_missionSerial)
        ResetForMission(mission, hud, entities, map);
    PruneGroups(entities);

    // Replicas skip SimulationStep, so keep HUD timers, primary producers, and replicated events updated here.
    auto updateReplicaHud = [&]() {
        if (!m_sim.AdvancesWorld())
            hud.Update(deltaTime, entities, economy, m_sim.TakeEvents());
    };

    // Wheel steps of this frame: over the map they zoom (below, only when no window covers the map); steps that
    // arrive while a window is open are dropped so nothing jumps when it closes
    const int mapWheel = input.ConsumeMapWheel();
    const bool windowOpen = menu.IsAnyMenuOpen() || hud.IsResearchWindowOpen();
    input.SetMenuOpen(windowOpen);
    if (windowOpen)
        input.ConsumeUiWheel();
    else if (!hud.IsResearchWindowOpen())
        input.ConsumeWheel(); // the generic counter is for menu lists only (and the technology tree window: it
                              // covers the map, its steps come from anywhere)

    int mouseX = input.GetMouseScreenX();
    int mouseY = input.GetMouseScreenY();
    float leftClickWorldX = 0.0f, leftClickWorldY = 0.0f;
    bool leftClicked = input.WasLeftClickIssued(leftClickWorldX, leftClickWorldY);
    float rightClickWorldX = 0.0f, rightClickWorldY = 0.0f;
    bool rightClicked = input.WasRightClickIssued(rightClickWorldX, rightClickWorldY);

    // 1. Objectives Dialog
    if (menu.IsObjectivesDialogOpen())
    {
        if (menu.UpdateObjectivesDialog(input, leftClicked))
        {
            // A network match or observer playback goes on while a player reads its menus
            if ((mission.IsOnlineMatch() || hud.IsObserver()) && !m_simHold)
                SimulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
            updateReplicaHud();
            return;
        }
    }

    // 1a. The technology tree window: it takes the input, the game stands still behind it (a network match goes on)
    if (hud.IsResearchWindowOpen())
    {
        hud.SetTopInset(m_debugMode ? 32 : 0);
        hud.UpdateResearchWindow(context, mouseX, mouseY, leftClicked);
        // (an observer's window stops nothing; a held tick - a replay frame without one - stays held)
        if ((mission.IsOnlineMatch() || hud.IsObserver()) && !m_simHold)
        {
            SimulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
        }
        else if (!mission.IsOnlineMatch() && !hud.IsObserver())
        {
            // The game stands still, but what the window ordered (a research started) takes effect now instead of
            // waiting for the window to close: the command carries this tick, so it is the same as applying it at
            // the start of the tick that runs next (a recording replays it alike)
            SimContext simContext {entities, map, pathfinder, economy, aiController};
            m_sim.ApplyCommands(simContext);
        }
        updateReplicaHud();
        return;
    }

    // 1b. Campaign mission dialogue: the mission waits while it is read
    if (menu.IsMissionDialogOpen()
        && menu.UpdateMissionDialog(input, mouseX, mouseY, leftClicked, windowWidth, windowHeight))
    {
        updateReplicaHud();
        return;
    }

    // 2. In-Game Pause Menu
    if (menu.IsInGameMenuOpen())
    {
        if (menu.UpdateInGameMenu(context, mouseX, mouseY, leftClicked))
        {
            if ((mission.IsOnlineMatch() || hud.IsObserver()) && appState == AppState::SkirmishPlaying && !m_simHold)
                SimulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
            updateReplicaHud();
            return;
        }
    }

    // 4. Command sidebar click handling (right edge); the debug toolbar sits above it
    hud.SetTopInset(m_debugMode ? 32 : 0);
    bool openMenu = false;
    bool openObjectives = false;
    hud.SetPointerPressOrigin(input.GetLeftPressX(), input.GetLeftPressY());
    bool mapInputConsumed = hud.HandleHudClick(
        mouseX,
        mouseY,
        leftClicked,
        rightClicked,
        input.IsLeftMouseDown(),
        input.ConsumeUiWheel(),
        windowWidth,
        windowHeight,
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
    const int mapViewWidth = camera.GetViewportWidth();

    // Control group bar at the bottom of the map: click selects the group
    if (!mapInputConsumed && (leftClicked || rightClicked || input.IsLeftMouseDown()))
    {
        int groupSlot = GroupBarHit(mouseX, mouseY, mapViewWidth, windowHeight);
        if (groupSlot >= 0)
        {
            if (leftClicked)
                SelectGroup(groupSlot, entities);
            mapInputConsumed = true;
            input.CancelSelectionBox();
        }
    }

    // Camera zoom: wheel over the open map, not over the group bar or the debug spawn list
    if (mapWheel != 0 && mouseX < mapViewWidth && GroupBarHit(mouseX, mouseY, mapViewWidth, windowHeight) < 0
        && !(m_debugMode && m_spawnDropdownOpen))
    {
        const float zoomDelta = mapWheel > 0 ? 1.15f : (1.0f / 1.15f);
        float zoom = camera.GetZoom();
        for (int i = 0; i < std::abs(mapWheel); ++i)
            zoom *= zoomDelta;
        camera.SetZoom(zoom, static_cast<float>(mouseX), static_cast<float>(mouseY));
    }

    // 4.4. Debug Spawn Dropdown Click Handling (--debug mode)
    if (m_debugMode && m_spawnDropdownOpen)
    {
        if (leftClicked)
        {
            int dropdownX = 450;
            int dropdownY = 32;
            int columnWidth = 170;
            int dropdownWidth = columnWidth * 3;
            int dropdownHeight = 370;

            bool itemSelected = false;
            if (mouseX >= dropdownX && mouseX <= dropdownX + dropdownWidth && mouseY >= dropdownY
                && mouseY <= dropdownY + dropdownHeight)
            {
                // Column 1: HUMANS (items 0..5)
                if (mouseX < dropdownX + columnWidth)
                {
                    int itemIndex = (mouseY - (dropdownY + 24)) / 22;
                    if (itemIndex >= 0 && itemIndex < 6)
                    {
                        m_selectedSpawnIndex = itemIndex;
                        m_spawnMode = true;
                        m_spawnDropdownOpen = false;
                        itemSelected = true;
                    }
                }
                // Column 2: VEHICLES (items 6..13)
                else if (mouseX < dropdownX + columnWidth * 2)
                {
                    int itemIndex = (mouseY - (dropdownY + 24)) / 22;
                    if (itemIndex >= 0 && itemIndex < 8)
                    {
                        m_selectedSpawnIndex = 6 + itemIndex;
                        m_spawnMode = true;
                        m_spawnDropdownOpen = false;
                        itemSelected = true;
                    }
                }
                // Column 3: BUILDINGS (items 14..29)
                else
                {
                    int itemIndex = (mouseY - (dropdownY + 24)) / 20;
                    if (itemIndex >= 0 && itemIndex < 16)
                    {
                        m_selectedSpawnIndex = 14 + itemIndex;
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
    if (m_debugMode && leftClicked && mouseY <= 30)
    {
        if (mouseX >= 98 && mouseX <= 198)
        {
            m_showNavLines = !m_showNavLines;
        }
        else if (mouseX >= 202 && mouseX <= 322)
        {
            m_controlAllFactions = !m_controlAllFactions;
        }
        else if (mouseX >= 326 && mouseX <= 436)
        {
            m_pauseAI = !m_pauseAI;
        }
        else if (mouseX >= 440 && mouseX <= 545)
        {
            m_revealFog = !m_revealFog;
        }
        else if (mouseX >= 549 && mouseX <= 624)
        {
            SimCommand command = Command(CommandType::DebugCredits);
            command.debug = true;
            command.a = 1000;
            m_sim.Issue(std::move(command));
        }
        else if (mouseX >= 628 && mouseX <= 748)
        {
            m_spawnMode = !m_spawnMode;
            if (m_spawnMode)
            {
                m_spawnDropdownOpen = false;
            }
        }
        else if (mouseX >= 752 && mouseX <= 960)
        {
            m_spawnDropdownOpen = !m_spawnDropdownOpen;
        }
        else if (mouseX >= 964 && mouseX <= 1049)
        {
            SimCommand command = Command(CommandType::ToggleSiege);
            command.ids.assign(m_selectedUnits.begin(), m_selectedUnits.end());
            m_sim.Issue(std::move(command));
        }
        else if (mouseX >= 1053 && mouseX <= 1153)
        {
            m_showDebugGrid = !m_showDebugGrid;
        }
        else if (mouseX >= 1157 && mouseX <= 1237)
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
        else if (leftClicked && mouseY >= 32 && mouseX < mapViewWidth)
        {
            const auto &catalog = GetDebugSpawnCatalog();
            if (m_selectedSpawnIndex >= 0 && m_selectedSpawnIndex < static_cast<int>(catalog.size()))
            {
                const auto &item = catalog[m_selectedSpawnIndex];
                int tileX = static_cast<int>(leftClickWorldX / TILE_WIDTH);
                int tileY = static_cast<int>(leftClickWorldY / TILE_HEIGHT);

                SimCommand command = Command(CommandType::DebugSpawn);
                command.debug = true;
                command.player = Players::OwnerOf(item.faction);
                command.a = item.archetypeOrBuildingIdx;
                command.b = static_cast<std::int32_t>(
                    item.isWall ? DebugSpawnKind::Wall
                                : (item.isBuilding ? DebugSpawnKind::Building : DebugSpawnKind::Unit));
                command.c = item.width;
                command.d = item.height;
                command.x = tileX;
                command.y = tileY;
                m_sim.Issue(std::move(command));
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
            int selectedBlueprintIndex = hud.GetSelectedBlueprint();
            if (selectedBlueprintIndex >= 0 && selectedBlueprintIndex < static_cast<int>(blueprints.size()))
            {
                const auto &blueprint = blueprints[selectedBlueprintIndex];
                int tileX = static_cast<int>(input.GetMouseWorldX() / TILE_WIDTH);
                int tileY = static_cast<int>(input.GetMouseWorldY() / TILE_HEIGHT);

                if (leftClicked && mouseX < mapViewWidth)
                {
                    const bool isMine = blueprint.mineKind >= 0;
                    bool canPlace = isMine
                                        ? entities.CanPlaceMine(blueprint.mineKind, tileX, tileY, map, Players::Local())
                                        : entities.CanPlaceBuilding(
                                              tileX, tileY, blueprint.width, blueprint.height, map, Players::Local());
                    bool canAfford = economy.CanAfford(blueprint.creditCost);
                    // Hard limits (data/limits.yaml): checked again by the simulation
                    const bool withinLimit
                        = isMine || BuildLimits::CanBuildMore(entities, map, Players::Local(), blueprint.buildingIndex);
                    if (!withinLimit)
                    {
                        SoundRequests::Play("ui_error");
                        hud.SetNotification(
                            BuildLimits::LimitOf(entities, blueprint.buildingIndex) == 0
                                ? _("THIS BUILDING IS NOT ALLOWED!")
                                : _("BUILDING LIMIT REACHED!"));
                        hud.SetBuildMode(false);
                    }
                    else if (!canAfford)
                    {
                        SoundRequests::Announce("ann_insufficient_funds");
                    }
                    else if (!canPlace)
                    {
                        SoundRequests::Announce("ann_cannot_build");
                        if (!entities.HasBuildingType(UnitType::Headquarters, Players::Local(), true))
                            hud.SetNotification(_("NO HEADQUARTERS: NOTHING CAN BE BUILT!"));
                    }
                    if (withinLimit && canPlace && canAfford)
                    {
                        // The simulation checks both again when it places it (a network turn later)
                        SimCommand command = Command(CommandType::PlaceBuilding);
                        command.a = blueprint.blueprintId; // canonical slot: the same on every peer
                        command.x = tileX;
                        command.y = tileY;
                        m_sim.Issue(std::move(command));
                        SoundRequests::Play("build_place");
                        if (!blueprint.isWall && !isMine)
                            SoundRequests::Announce("ann_building");
                        // Walls and mines are laid one after the other; a building ends the placement
                        if (!blueprint.isWall && !isMine)
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
        else if (leftClicked && mouseX < mapViewWidth)
        {
            std::vector<int> combatUnits;
            for (int unitId : m_selectedUnits)
            {
                const Entity *entity = entities.GetEntity(unitId);
                if (entity && entity->alive && !entity->isBuilding)
                {
                    combatUnits.push_back(unitId);
                }
            }

            if (!combatUnits.empty())
            {
                const bool shiftHeld
                    = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
                OrderAt(combatUnits, leftClickWorldX, leftClickWorldY, OrderKind::AttackMove, shiftHeld);
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
            for (int unitId : m_selectedUnits)
            {
                const Entity *entity = entities.GetEntity(unitId);
                if (entity && entity->alive && !entity->isBuilding)
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
            for (int unitId : m_selectedUnits)
            {
                const Unit *unit = entities.GetUnit(unitId);
                if (unit && unit->owner == Players::Local() && !entities.CanDeploySiege(*unit))
                    locked = true;
            }
            if (locked)
            {
                SoundRequests::Play("ui_error");
                hud.SetNotification(_("RESEARCH SIEGE MODE FIRST!"));
            }
            SimCommand command = Command(CommandType::ToggleSiege);
            command.ids.assign(m_selectedUnits.begin(), m_selectedUnits.end());
            m_sim.Issue(std::move(command));
        }

        // Z / X / C - stances: guard (chase and return), hold ground, pursue
        {
            static const SDL_Scancode kStanceKeys[] = {SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C};
            for (int i = 0; i < 3; ++i)
            {
                if (!input.IsKeyJustPressed(kStanceKeys[i]) || m_selectedUnits.empty())
                    continue;
                SimCommand command = Command(CommandType::SetStance);
                command.a = i;
                command.ids.assign(m_selectedUnits.begin(), m_selectedUnits.end());
                m_sim.Issue(std::move(command));
                SoundRequests::Play("@hold");
            }
        }

        // R - repair on / off for the selected own buildings
        if (input.IsKeyJustPressed(SDL_SCANCODE_R))
        {
            SimCommand command = Command(CommandType::ToggleRepair);
            for (int unitId : m_selectedUnits)
            {
                const Building *building = entities.GetBuilding(unitId);
                if (building && building->owner == Players::Local())
                    command.ids.push_back(static_cast<std::uint32_t>(unitId));
            }
            if (!command.ids.empty())
                m_sim.Issue(std::move(command));
        }

        // ESC - In-game Pause Menu
        if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            menu.SetInGameMenuOpen(true);
            if (mission.IsOnlineMatch() && !m_simHold)
                SimulationStep(deltaTime, map, pathfinder, entities, economy, aiController, mission, hud, appState);
            updateReplicaHud();
            return;
        }

        // Right-Click orders on map (Move / Attack / Attack Wall / Siege Context Menu)
        if (rightClicked && mouseX < mapViewWidth && (!m_debugMode || mouseY > 30))
        {
            std::vector<int> combatUnits;
            for (int unitId : m_selectedUnits)
            {
                const Entity *entity = entities.GetEntity(unitId);
                if (entity && entity->alive && !entity->isBuilding)
                {
                    combatUnits.push_back(unitId);
                }
            }

            // Only Barracks / Factories selected: right click moves their rally point
            if (combatUnits.empty())
            {
                SimCommand command = Command(CommandType::SetRally);
                command.ids.assign(m_selectedUnits.begin(), m_selectedUnits.end());
                command.x = static_cast<std::int32_t>(rightClickWorldX);
                command.y = static_cast<std::int32_t>(rightClickWorldY);
                if (!command.ids.empty())
                    m_sim.Issue(std::move(command));
            }
            else
            {
                // Shift: the point is queued after the current orders (a route, as in other RTS games)
                const bool shiftHeld
                    = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
                OrderAt(combatUnits, rightClickWorldX, rightClickWorldY, OrderKind::Move, shiftHeld);
            }
        }

        // Touch: a tap on the ground or an enemy orders the selected units (move; twice on a spot: attack-move)
        bool touchOrdered = false;
        if (m_touchMode && !hud.IsAttackMoveMode() && leftClicked && mouseX < mapViewWidth
            && (!m_debugMode || mouseY > 30))
        {
            std::vector<int> combatUnits;
            for (int unitId : m_selectedUnits)
            {
                const Entity *entity = entities.GetEntity(unitId);
                if (entity && entity->alive && !entity->isBuilding)
                    combatUnits.push_back(unitId);
            }
            const Entity *tappedEntity = entities.GetEntity(entities.GetUnitAt(leftClickWorldX, leftClickWorldY));
            // A tap on an own unit or building selects it (the unit selection ends), it is no order
            const bool tappedOwn = tappedEntity && tappedEntity->alive && tappedEntity->owner == Players::Local();
            const std::uint64_t nowTicks = SDL_GetTicks();
            const bool isSecondOrderTap = nowTicks - m_touchOrderTicks < 450 && std::abs(mouseX - m_touchOrderX) <= 40
                                          && std::abs(mouseY - m_touchOrderY) <= 40;
            if (!combatUnits.empty() && !tappedOwn)
            {
                OrderAt(
                    combatUnits,
                    leftClickWorldX,
                    leftClickWorldY,
                    isSecondOrderTap ? OrderKind::AttackMove : OrderKind::Move,
                    false);
                m_touchOrderTicks = isSecondOrderTap ? 0 : nowTicks;
                m_touchOrderX = mouseX;
                m_touchOrderY = mouseY;
                touchOrdered = true;
            }
            else if (combatUnits.empty() && !tappedOwn)
            {
                const bool isSecondRallyTap = !m_touchRallyBuildings.empty() && nowTicks - m_touchRallyTicks < 450
                                              && std::abs(mouseX - m_touchRallyX) <= 40
                                              && std::abs(mouseY - m_touchRallyY) <= 40;
                if (isSecondRallyTap && m_selectedUnits.empty())
                {
                    SimCommand command = Command(CommandType::SetRally);
                    for (int unitId : m_touchRallyBuildings)
                    {
                        const Building *building = entities.GetBuilding(unitId);
                        if (building && building->alive
                            && (building->owner == Players::Local() || (m_debugMode && m_controlAllFactions))
                            && building->IsUnitProducer())
                        {
                            command.ids.push_back(static_cast<std::uint32_t>(unitId));
                        }
                    }
                    if (!command.ids.empty())
                    {
                        command.x = static_cast<std::int32_t>(leftClickWorldX);
                        command.y = static_cast<std::int32_t>(leftClickWorldY);
                        m_sim.Issue(std::move(command));
                    }
                    m_touchRallyBuildings.clear();
                    m_touchRallyTicks = 0;
                    touchOrdered = true;
                }
                else if (!m_selectedUnits.empty())
                {
                    // 1st tap anywhere on the map resets building focus, and remembers unit-producing
                    // building(s) so a 2nd tap at the same spot sets their rally point.
                    m_touchRallyBuildings.clear();
                    for (int unitId : m_selectedUnits)
                    {
                        const Building *building = entities.GetBuilding(unitId);
                        if (building && building->alive
                            && (building->owner == Players::Local() || (m_debugMode && m_controlAllFactions))
                            && building->IsUnitProducer())
                        {
                            m_touchRallyBuildings.push_back(unitId);
                        }
                    }
                    m_touchRallyTicks = m_touchRallyBuildings.empty() ? 0 : nowTicks;
                    m_touchRallyX = mouseX;
                    m_touchRallyY = mouseY;
                    for (int unitId : m_selectedUnits)
                    {
                        if (Entity *e = entities.GetEntity(unitId))
                            e->selected = false;
                    }
                    m_selectedUnits.clear();
                    touchOrdered = true;
                }
            }
            else if (tappedOwn)
            {
                m_touchRallyBuildings.clear();
                m_touchRallyTicks = 0;
            }
        }

        // Left-Click single unit selection on map
        if (!touchOrdered && !hud.IsAttackMoveMode() && leftClicked && mouseX < mapViewWidth
            && (!m_debugMode || mouseY > 30))
        {
            int clickedEntityId = entities.GetUnitAt(leftClickWorldX, leftClickWorldY);
            const Building *clickedBuilding = entities.GetBuilding(clickedEntityId);
            const bool ownProducer
                = clickedBuilding && clickedBuilding->owner == Players::Local() && clickedBuilding->IsUnitProducer();
            const bool shiftHeld = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
            const Uint64 nowTicks = SDL_GetTicks();
            // Twice on the same unit, or twice on the same spot of empty ground
            const bool isSamePlace = std::abs(static_cast<int>(mouseX) - m_lastClickX) <= 8
                                     && std::abs(static_cast<int>(mouseY) - m_lastClickY) <= 8;
            const bool isDoubleClick = clickedEntityId == m_lastClickId && nowTicks - m_lastClickTicks < 350
                                       && (clickedEntityId > 0 || isSamePlace);
            m_lastClickId = clickedEntityId;
            m_lastClickTicks = nowTicks;
            m_lastClickX = static_cast<int>(mouseX);
            m_lastClickY = static_cast<int>(mouseY);
            const Entity *clickedEntity = clickedEntityId > 0 ? entities.GetEntity(clickedEntityId) : nullptr;
            const Unit *clickedUnit = clickedEntity ? clickedEntity->AsUnit() : nullptr;
            const bool ownClickedUnit = clickedUnit && IsLiveOwnUnit(clickedEntity);

            // Shift+click on a Barracks / Factory adds it to the selected ones of the same type
            bool sameTypeSelected = ownProducer && shiftHeld;
            if (sameTypeSelected)
            {
                for (int unitId : m_selectedUnits)
                {
                    const Building *building = entities.GetBuilding(unitId);
                    if (!building || building->type != clickedBuilding->type)
                        sameTypeSelected = false;
                }
            }
            if (sameTypeSelected)
            {
                Entity *entity = entities.GetEntity(clickedEntityId);
                if (entity->selected)
                {
                    entity->selected = false;
                    m_selectedUnits.erase(
                        std::remove(m_selectedUnits.begin(), m_selectedUnits.end(), clickedEntityId),
                        m_selectedUnits.end());
                }
                else
                {
                    entity->selected = true;
                    m_selectedUnits.push_back(clickedEntityId);
                    hud.FocusBuilding(clickedEntityId, entities);
                }
            }
            else
            {
                m_selectedUnits.clear();
                for (auto &entity : entities.GetAllEntities())
                    entity.selected = false;
                if (isDoubleClick && ownClickedUnit)
                {
                    // Double click on a unit: every own unit of that kind on the screen
                    const int archetype = clickedUnit->archetypeId;
                    SelectOwnUnits(entities, camera, true, [archetype](const Unit &unit) {
                        return unit.archetypeId == archetype;
                    });
                }
                else if (isDoubleClick && clickedEntityId == 0)
                {
                    // Double click on empty ground: every own unit on the screen
                    SelectOwnUnits(entities, camera, true, [](const Unit &) { return true; });
                }
                else if (ownProducer && isDoubleClick)
                {
                    // Double click: every own building of that type
                    for (auto &entity : entities.GetAllEntities())
                    {
                        const Building *building = entity.AsBuilding();
                        if (building && building->alive && building->state != ActionState::Die
                            && building->owner == Players::Local() && building->type == clickedBuilding->type)
                        {
                            entity.selected = true;
                            m_selectedUnits.push_back(entity.id);
                        }
                    }
                    hud.FocusBuilding(clickedEntityId, entities);
                }
                else if (clickedEntityId > 0)
                {
                    Entity *entity = entities.GetEntity(clickedEntityId);
                    if (entity && (entity->owner == Players::Local() || (m_debugMode && m_controlAllFactions)))
                    {
                        entity->selected = true;
                        m_selectedUnits.push_back(clickedEntityId);
                        if (entity->isBuilding)
                            hud.FocusBuilding(clickedEntityId, entities);
                    }
                }
            }
        }

        // Box selection on map
        const auto &selectionBox = input.GetSelectionBox();
        if (!hud.IsAttackMoveMode() && selectionBox.active && selectionBox.HasSignificantArea()
            && selectionBox.startScreenX < mapViewWidth && (!m_debugMode || selectionBox.startScreenY > 30))
        {
            float minX = std::min(selectionBox.startWorldX, selectionBox.currentWorldX);
            float maxX = std::max(selectionBox.startWorldX, selectionBox.currentWorldX);
            float minY = std::min(selectionBox.startWorldY, selectionBox.currentWorldY);
            float maxY = std::max(selectionBox.startWorldY, selectionBox.currentWorldY);
            // The local player's own units: by player, not by faction (two players of one faction would
            // otherwise select the first one's units, whose orders the simulation then refuses)
            const PlayerId selectionOwner
                = (m_debugMode && m_controlAllFactions) ? Players::OwnerOf(AiFaction()) : Players::Local();
            for (auto &entity : entities.GetAllEntities())
                entity.selected = false; // an area selection ends the focus on a building
            entities.GetUnitsInBox(minX, minY, maxX, maxY, selectionOwner, m_selectedUnits);
            if (m_debugMode && m_controlAllFactions && m_selectedUnits.empty())
            {
                entities.GetUnitsInBox(minX, minY, maxX, maxY, Players::Local(), m_selectedUnits);
            }
            if (m_touchMode && !input.IsLeftMouseDown())
                input.CancelSelectionBox();
        }
    }

    // The mission dialogue opens after its delay of playing time (menus and pauses do not count)
    if (m_pendingDialogId != 0 && hud.IsObserver())
        m_pendingDialogId = 0; // a replay of the mission: its story was read when it was played
    if (m_pendingDialogId != 0 && !m_simHold)
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
    updateReplicaHud();
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
    if (!m_sim.AdvancesWorld())
        return;

    // 8. Simulation updates: the commands of this tick first, then the world
    entities.SnapshotRenderState(); // the picture glides from here to where this tick leaves things
    SimContext simContext {entities, map, pathfinder, economy, aiController};
    m_sim.ApplyCommands(simContext);
    m_sim.UpdateResearch(deltaTime, simContext);
    hud.Update(deltaTime, entities, economy, m_sim.TakeEvents());
    entities.Update(deltaTime, map, pathfinder);
    // A match: whoever has no building left is out, computer players too (their controllers stop)
    if (mission.IsMatch())
    {
        for (PlayerId eliminatedPlayer : entities.UpdateElimination(map))
            m_sim.NoteEvent({SimEvent::Type::PlayerEliminated, eliminatedPlayer, 0});
    }
    m_sim.UpdateOrderQueues(simContext);
    m_sim.UpdateSquads(deltaTime, simContext, aiController.GetChokepoints());
    m_sim.UpdateEconomies(deltaTime, simContext); // the local economy included, in slot order
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
    const auto &selectionBox = input.GetSelectionBox();
    if (selectionBox.active && selectionBox.HasSignificantArea())
    {
        int boxX = std::min(selectionBox.startScreenX, selectionBox.currentScreenX);
        int boxY = std::min(selectionBox.startScreenY, selectionBox.currentScreenY);
        int boxWidth = std::abs(selectionBox.currentScreenX - selectionBox.startScreenX);
        int boxHeight = std::abs(selectionBox.currentScreenY - selectionBox.startScreenY);
        renderer.FillRectScreen(boxX, boxY, boxWidth, boxHeight, {0, 255, 0, 45});
        renderer.DrawRectScreen(boxX, boxY, boxWidth, boxHeight, {0, 255, 0, 255});
    }
    else
    {
        int holdX = 0, holdY = 0;
        if (input.IsTouchSelectionHoldReady(holdX, holdY))
        {
            constexpr int half = 12;
            renderer.FillRectScreen(holdX - half, holdY - half, half * 2, half * 2, {0, 255, 0, 45});
            renderer.DrawRectScreen(holdX - half, holdY - half, half * 2, half * 2, {0, 255, 0, 255});
        }
    }
}

void GameplayController::LoadOverlayStyle(const EnvironmentConfig &env)
{
    const nlohmann::json &overlayJson = env.Section("overlay");
    m_overlay = OverlayStyle {};
    if (!overlayJson.is_object())
        return;
    m_overlay.opacity = std::clamp(overlayJson.value("line_opacity", m_overlay.opacity), 0.0f, 1.0f);
    auto readLineStyle = [&](const char *key, LineStyle &style) {
        if (!overlayJson.contains(key) || !overlayJson[key].is_object())
            return;
        style.color = RenderUtility::ParseHexColor(overlayJson[key].value("color", std::string()), style.color);
        style.width = std::clamp(overlayJson[key].value("width", style.width), 0.5f, 12.0f);
    };
    readLineStyle("move_line", m_overlay.move);
    readLineStyle("attack_line", m_overlay.attack);
    readLineStyle("wall_line", m_overlay.wall);
}

void GameplayController::RenderOrderLines(
    Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map, bool showAll) const
{
    auto drawLine = [&](float startX, float startY, float endX, float endY, const LineStyle &style) {
        SDL_Color lineColor = style.color;
        lineColor.a = static_cast<Uint8>(std::clamp(lineColor.a * m_overlay.opacity, 0.0f, 255.0f));
        RenderUtility::DrawLine(renderer, camera, startX, startY, endX, endY, lineColor, style.width);
    };
    auto drawOrders = [&](const Entity &entity) {
        const Unit *unit = entity.AsUnit();
        if (!unit || !unit->alive || unit->state == ActionState::Die)
            return;
        const float unitCenterX = unit->GetUnitCenterPixelX(), unitCenterY = unit->GetUnitCenterPixelY();
        if (unit->targetEntityId != 0)
        {
            const Entity *target = entities.GetEntity(unit->targetEntityId);
            if (target && target->alive && target->state != ActionState::Die
                && (target->owner == Players::Local()
                    || entities.IsEntityVisibleToPlayer(*target, Players::Local(), map)))
            {
                drawLine(
                    unitCenterX,
                    unitCenterY,
                    target->GetUnitCenterPixelX(),
                    target->GetUnitCenterPixelY(),
                    m_overlay.attack);
            }
        }
        if (unit->targetWallTileX >= 0 && unit->targetWallTileY >= 0)
        {
            drawLine(
                unitCenterX,
                unitCenterY,
                unit->targetWallTileX * TILE_WIDTH + TILE_WIDTH * 0.5f,
                unit->targetWallTileY * TILE_HEIGHT + TILE_HEIGHT * 0.5f,
                m_overlay.wall);
        }
        // A move / attack-move order: the line ends at the clicked point (not at the unit's own formation tile),
        // then the Shift-queued points follow, each with a small mark
        auto drawOrderMark = [&](float worldX, float worldY, const LineStyle &style) {
            // The end of an order looks like a rally point (a filled square in a frame), in the colour of its line
            float screenX = 0, screenY = 0;
            camera.WorldToScreen(worldX, worldY, screenX, screenY);
            SDL_Color markColor = style.color;
            markColor.a = static_cast<Uint8>(std::clamp(markColor.a * m_overlay.opacity * 1.5f, 0.0f, 255.0f));
            const int markSize = std::max(4, static_cast<int>(5 * camera.GetZoom()));
            renderer.FillRectScreen(
                static_cast<int>(screenX) - markSize / 2,
                static_cast<int>(screenY) - markSize / 2,
                markSize,
                markSize,
                markColor);
            renderer.DrawRectScreen(
                static_cast<int>(screenX) - markSize,
                static_cast<int>(screenY) - markSize,
                2 * markSize,
                2 * markSize,
                markColor);
        };
        float fromX = unitCenterX, fromY = unitCenterY;
        // Red while the unit has a target to fight (a unit, a building, a wall, or what it shoots at on the move),
        // green otherwise
        const bool hasCombatTarget = unit->targetEntityId != 0 || unit->moveFireTargetId != 0
                                     || (unit->targetWallTileX >= 0 && unit->targetWallTileY >= 0);
        if (unit->orderX >= 0 && unit->orderY >= 0)
        {
            const LineStyle &style = (unit->isAttackMoving || hasCombatTarget) ? m_overlay.attack : m_overlay.move;
            const float orderWorldX = static_cast<float>(unit->orderX);
            const float orderWorldY = static_cast<float>(unit->orderY);
            if (unit->targetEntityId == 0)
                drawLine(unitCenterX, unitCenterY, orderWorldX, orderWorldY, style);
            drawOrderMark(orderWorldX, orderWorldY, style);
            fromX = orderWorldX;
            fromY = orderWorldY;
        }
        else if (unit->HasMoveTarget())
        {
            fromX = unit->targetTileX * TILE_WIDTH + TILE_WIDTH * 0.5f;
            fromY = unit->targetTileY * TILE_HEIGHT + TILE_HEIGHT * 0.5f;
            drawLine(unitCenterX, unitCenterY, fromX, fromY, hasCombatTarget ? m_overlay.attack : m_overlay.move);
        }
        for (const Unit::QueuedOrder &queuedOrder : unit->orderQueue)
        {
            const LineStyle &style
                = static_cast<OrderKind>(queuedOrder.kind) == OrderKind::AttackMove ? m_overlay.attack : m_overlay.move;
            const float orderWorldX = static_cast<float>(queuedOrder.x);
            const float orderWorldY = static_cast<float>(queuedOrder.y);
            drawLine(fromX, fromY, orderWorldX, orderWorldY, style);
            drawOrderMark(orderWorldX, orderWorldY, style);
            fromX = orderWorldX;
            fromY = orderWorldY;
        }
    };

    if (showAll)
    {
        for (const Entity &entity : entities.GetAllEntities())
        {
            // Only the player's own and allied units: the orders of an enemy are not for the eyes of the other side
            if (!entity.IsUnitEntity() || entity.owner == kNoPlayer || Players::Hostile(entity.owner, Players::Local()))
                continue;
            drawOrders(entity);
        }
        return;
    }
    for (int unitId : m_selectedUnits)
    {
        const Entity *entity = entities.GetEntity(unitId);
        if (entity && entity->owner == Players::Local())
            drawOrders(*entity);
    }
}

void GameplayController::RenderDebugNavigation(
    Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map)
{
    (void) map;
    if (!m_debugMode || !m_showNavLines)
        return;

    for (int unitId : m_selectedUnits)
    {
        const Entity *entity = entities.GetEntity(unitId);
        if (!entity || !entity->alive || entity->state == ActionState::Die)
            continue;

        float unitWorldX = entity->GetUnitCenterPixelX();
        float unitWorldY = entity->GetUnitCenterPixelY();

        // 1. Attack Range Ellipse
        const SDL_Color rangeColor {255, 60, 60, 110}; // red
        RenderUtility::DrawRangeRing(renderer, camera, unitWorldX, unitWorldY, entity->attackRange, rangeColor, 1.5f);

        // 2. Navigation Path Lines & Waypoints
        const Unit *unit = entity->AsUnit();
        if (unit && !unit->path.empty() && unit->pathIndex < unit->path.size())
        {
            float prevX = unitWorldX;
            float prevY = unitWorldY;
            for (std::size_t i = unit->pathIndex; i < unit->path.size(); ++i)
            {
                float nodeX = unit->path[i].x * TILE_WIDTH + (TILE_WIDTH * 0.5f);
                float nodeY = unit->path[i].y * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
                renderer.DrawLineWorld(prevX, prevY, nodeX, nodeY, camera, {0, 255, 200, 220});
                renderer.DrawRectWorld(nodeX - 3.0f, nodeY - 3.0f, 6.0f, 6.0f, camera, {0, 255, 200, 255});
                prevX = nodeX;
                prevY = nodeY;
            }
            // Destination flag / marker
            RenderUtility::DrawRadius(renderer, camera, prevX, prevY, 5.0f, SDL_Color {255, 255, 0, 255}, 1.5f);
        }

        // 3. Combat Target Line (Red)
        if (entity->targetEntityId != 0)
        {
            const Entity *targetEntity = entities.GetEntity(entity->targetEntityId);
            if (targetEntity && targetEntity->alive && targetEntity->state != ActionState::Die)
            {
                float targetWorldX = targetEntity->GetUnitCenterPixelX();
                float targetWorldY = targetEntity->GetUnitCenterPixelY();
                renderer.DrawLineWorld(unitWorldX, unitWorldY, targetWorldX, targetWorldY, camera, {255, 30, 30, 255});
                renderer.DrawRectWorld(
                    targetWorldX - 8.0f, targetWorldY - 8.0f, 16.0f, 16.0f, camera, {255, 30, 30, 255});
            }
        }

        // 4. Wall Target Line (Orange)
        if (entity->targetWallTileX >= 0 && entity->targetWallTileY >= 0)
        {
            float wallWorldX = entity->targetWallTileX * TILE_WIDTH + (TILE_WIDTH * 0.5f);
            float wallWorldY = entity->targetWallTileY * TILE_HEIGHT + (TILE_HEIGHT * 0.5f);
            renderer.DrawLineWorld(unitWorldX, unitWorldY, wallWorldX, wallWorldY, camera, {255, 140, 0, 255});
            renderer.DrawRectWorld(wallWorldX - 6.0f, wallWorldY - 6.0f, 12.0f, 12.0f, camera, {255, 140, 0, 255});
        }

        // 5. Waiting/Blocked indicator
        if (unit && unit->waitingForPath)
        {
            renderer.FillRectWorld(unitWorldX - 10.0f, unitWorldY - 24.0f, 20.0f, 6.0f, camera, {255, 180, 0, 220});
        }
    }
}

void GameplayController::RenderDebugToolbar(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowWidth)
{
    if (!m_debugMode)
        return;

    // Background bar
    renderer.FillRectScreen(0, 0, windowWidth, 30, {16, 20, 28, 240});
    renderer.DrawLineScreen(0, 30, windowWidth, 30, {0, 200, 255, 180});

    // Button 1: Badge
    renderer.FillRectScreen(4, 3, 90, 24, {20, 70, 40, 255});
    renderer.DrawRectScreen(4, 3, 90, 24, {40, 220, 90, 255});
    fontRenderer.DrawString("--DEBUG--", 12, 7, 1, {40, 255, 90, 255});

    // Button 2: Nav Lines
    SDL_Color navBgColor = m_showNavLines ? SDL_Color {30, 80, 100, 255} : SDL_Color {40, 40, 50, 255};
    renderer.FillRectScreen(98, 3, 100, 24, navBgColor);
    renderer.DrawRectScreen(98, 3, 100, 24, {0, 200, 255, 255});
    fontRenderer.DrawString(m_showNavLines ? "NAV: ON" : "NAV: OFF", 108, 7, 1, {255, 255, 255, 255});

    // Button 3: Enemy Control
    SDL_Color ctrlBgColor = m_controlAllFactions ? SDL_Color {110, 40, 40, 255} : SDL_Color {40, 40, 50, 255};
    renderer.FillRectScreen(202, 3, 120, 24, ctrlBgColor);
    renderer.DrawRectScreen(202, 3, 120, 24, {255, 100, 100, 255});
    fontRenderer.DrawString(m_controlAllFactions ? "CTRL: ALL" : "CTRL: PLAYER", 210, 7, 1, {255, 255, 255, 255});

    // Button 4: AI Pause
    SDL_Color aiBgColor = m_pauseAI ? SDL_Color {100, 80, 20, 255} : SDL_Color {40, 40, 50, 255};
    renderer.FillRectScreen(326, 3, 110, 24, aiBgColor);
    renderer.DrawRectScreen(326, 3, 110, 24, {255, 200, 50, 255});
    fontRenderer.DrawString(m_pauseAI ? "AI: PAUSED" : "AI: ACTIVE", 336, 7, 1, {255, 255, 255, 255});

    // Button 5: Fog of War
    SDL_Color fogBgColor = m_revealFog ? SDL_Color {40, 80, 60, 255} : SDL_Color {40, 40, 50, 255};
    renderer.FillRectScreen(440, 3, 105, 24, fogBgColor);
    renderer.DrawRectScreen(440, 3, 105, 24, {100, 255, 150, 255});
    fontRenderer.DrawString(m_revealFog ? "FOG: REVEAL" : "FOG: ON", 448, 7, 1, {255, 255, 255, 255});

    // Button 6: +$1000
    renderer.FillRectScreen(549, 3, 75, 24, {30, 60, 30, 255});
    renderer.DrawRectScreen(549, 3, 75, 24, {100, 255, 100, 255});
    fontRenderer.DrawString("+$1000", 559, 7, 1, {120, 255, 120, 255});

    // Button 7: SPAWN MODE Toggle
    SDL_Color spawnBgColor = m_spawnMode ? SDL_Color {25, 100, 50, 255} : SDL_Color {40, 40, 50, 255};
    SDL_Color spawnBorderColor = m_spawnMode ? SDL_Color {40, 255, 100, 255} : SDL_Color {120, 140, 160, 255};
    renderer.FillRectScreen(628, 3, 120, 24, spawnBgColor);
    renderer.DrawRectScreen(628, 3, 120, 24, spawnBorderColor);
    fontRenderer.DrawString(m_spawnMode ? "SPAWN: ON" : "SPAWN: OFF", 638, 7, 1, {255, 255, 255, 255});

    // Button 8: SPAWN Dropdown Combobox
    const auto &catalog = GetDebugSpawnCatalog();
    std::string selectedName = "SELECT ENTITY";
    Faction selectedFaction = Faction::Resistance;
    if (m_selectedSpawnIndex >= 0 && m_selectedSpawnIndex < static_cast<int>(catalog.size()))
    {
        selectedName = catalog[m_selectedSpawnIndex].name;
        selectedFaction = catalog[m_selectedSpawnIndex].faction;
    }
    renderer.FillRectScreen(752, 3, 208, 24, {28, 36, 52, 255});
    renderer.DrawRectScreen(752, 3, 208, 24, {0, 200, 255, 255});
    // Faction indicator pip
    SDL_Color factionColor
        = (selectedFaction == Faction::Confederation) ? SDL_Color {0, 200, 255, 255} : SDL_Color {255, 80, 80, 255};
    renderer.FillRectScreen(758, 10, 8, 10, factionColor);
    std::string comboLabel = selectedName + " v";
    fontRenderer.DrawString(comboLabel, 772, 7, 1, {255, 255, 255, 255});

    // Button 9: Siege Mode Toggle
    renderer.FillRectScreen(964, 3, 85, 24, {60, 50, 20, 255});
    renderer.DrawRectScreen(964, 3, 85, 24, {255, 200, 80, 255});
    fontRenderer.DrawString("SIEGE(G)", 972, 7, 1, {255, 220, 100, 255});

    // Button 10: Passability Grid (70% Transparency Green/Red)
    SDL_Color gridBgColor = m_showDebugGrid ? SDL_Color {25, 90, 45, 255} : SDL_Color {40, 40, 50, 255};
    SDL_Color gridBorderColor = m_showDebugGrid ? SDL_Color {40, 255, 100, 255} : SDL_Color {120, 140, 160, 255};
    renderer.FillRectScreen(1053, 3, 100, 24, gridBgColor);
    renderer.DrawRectScreen(1053, 3, 100, 24, gridBorderColor);
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
    const SDL_Color passableColor {0, 220, 0, 76};
    const SDL_Color impassableColor {220, 0, 0, 76};

    for (int tileY = startTileY; tileY < endTileY; ++tileY)
    {
        for (int tileX = startTileX; tileX < endTileX; ++tileX)
        {
            bool passable = map.IsPassable(tileX, tileY) && (map.GetWall(tileX, tileY) == 0);
            if (passable)
            {
                // Check if tile is covered by any stationary living building
                for (const auto &entity : entities.GetAllEntities())
                {
                    const Building *building = entity.AsBuilding();
                    if (!building || !building->alive || building->state == ActionState::Die)
                        continue;
                    if (tileX >= building->tileX && tileX < building->tileX + building->buildWidth
                        && tileY >= building->tileY && tileY < building->tileY + building->buildHeight)
                    {
                        passable = false;
                        break;
                    }
                }
            }

            SDL_Color color = passable ? passableColor : impassableColor;
            // "drawn as a filled rectangle shrunk by 2 pixels on every side (like a grid)"
            float rectX = tileX * TILE_WIDTH + 2.0f;
            float rectY = tileY * TILE_HEIGHT + 2.0f;
            float rectWidth = TILE_WIDTH - 4.0f;
            float rectHeight = TILE_HEIGHT - 4.0f;
            renderer.FillRectWorld(rectX, rectY, rectWidth, rectHeight, camera, color);
        }
    }
}

void GameplayController::RenderDebugSpawnDropdown(
    Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowHeight)
{
    (void) windowHeight;
    if (!m_debugMode || !m_spawnDropdownOpen)
        return;

    int dropdownX = 450;
    int dropdownY = 32;
    int columnWidth = 170;
    int dropdownWidth = columnWidth * 3;
    int dropdownHeight = 370;

    // Dropdown background box
    renderer.FillRectScreen(dropdownX, dropdownY, dropdownWidth, dropdownHeight, {16, 22, 32, 252});
    renderer.DrawRectScreen(dropdownX, dropdownY, dropdownWidth, dropdownHeight, {0, 200, 255, 255});
    renderer.DrawLineScreen(
        dropdownX + columnWidth, dropdownY, dropdownX + columnWidth, dropdownY + dropdownHeight, {40, 60, 80, 200});
    renderer.DrawLineScreen(
        dropdownX + columnWidth * 2,
        dropdownY,
        dropdownX + columnWidth * 2,
        dropdownY + dropdownHeight,
        {40, 60, 80, 200});

    const auto &catalog = GetDebugSpawnCatalog();

    // Column 1: HUMANS (items 0..5)
    fontRenderer.DrawString("-- HUMANS --", dropdownX + 10, dropdownY + 6, 1, {255, 220, 80, 255});
    for (int i = 0; i < 6 && i < static_cast<int>(catalog.size()); ++i)
    {
        int itemY = dropdownY + 26 + i * 22;
        if (i == m_selectedSpawnIndex)
        {
            renderer.FillRectScreen(dropdownX + 4, itemY - 2, columnWidth - 8, 20, {20, 70, 90, 255});
            renderer.DrawRectScreen(dropdownX + 4, itemY - 2, columnWidth - 8, 20, {0, 255, 220, 255});
        }
        SDL_Color factionColor = (catalog[i].faction == Faction::Confederation) ? SDL_Color {0, 200, 255, 255}
                                                                                : SDL_Color {255, 80, 80, 255};
        renderer.FillRectScreen(dropdownX + 8, itemY + 4, 6, 8, factionColor);
        fontRenderer.DrawString(catalog[i].name, dropdownX + 18, itemY, 1, {230, 240, 255, 255});
    }

    // Column 2: VEHICLES (items 6..13)
    fontRenderer.DrawString("-- VEHICLES --", dropdownX + columnWidth + 10, dropdownY + 6, 1, {255, 220, 80, 255});
    for (int i = 6; i < 14 && i < static_cast<int>(catalog.size()); ++i)
    {
        int itemY = dropdownY + 26 + (i - 6) * 22;
        if (i == m_selectedSpawnIndex)
        {
            renderer.FillRectScreen(dropdownX + columnWidth + 4, itemY - 2, columnWidth - 8, 20, {20, 70, 90, 255});
            renderer.DrawRectScreen(dropdownX + columnWidth + 4, itemY - 2, columnWidth - 8, 20, {0, 255, 220, 255});
        }
        SDL_Color factionColor = (catalog[i].faction == Faction::Confederation) ? SDL_Color {0, 200, 255, 255}
                                                                                : SDL_Color {255, 80, 80, 255};
        renderer.FillRectScreen(dropdownX + columnWidth + 8, itemY + 4, 6, 8, factionColor);
        fontRenderer.DrawString(catalog[i].name, dropdownX + columnWidth + 18, itemY, 1, {230, 240, 255, 255});
    }

    // Column 3: BUILDINGS (items 14..29)
    fontRenderer.DrawString("-- BUILDINGS --", dropdownX + columnWidth * 2 + 10, dropdownY + 6, 1, {255, 220, 80, 255});
    for (int i = 14; i < 30 && i < static_cast<int>(catalog.size()); ++i)
    {
        int itemY = dropdownY + 26 + (i - 14) * 20;
        if (i == m_selectedSpawnIndex)
        {
            renderer.FillRectScreen(dropdownX + columnWidth * 2 + 4, itemY - 1, columnWidth - 8, 18, {20, 70, 90, 255});
            renderer.DrawRectScreen(
                dropdownX + columnWidth * 2 + 4, itemY - 1, columnWidth - 8, 18, {0, 255, 220, 255});
        }
        SDL_Color factionColor = (catalog[i].faction == Faction::Confederation) ? SDL_Color {0, 200, 255, 255}
                                                                                : SDL_Color {255, 80, 80, 255};
        renderer.FillRectScreen(dropdownX + columnWidth * 2 + 8, itemY + 3, 6, 8, factionColor);
        fontRenderer.DrawString(catalog[i].name, dropdownX + columnWidth * 2 + 18, itemY, 1, {230, 240, 255, 255});
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

    int mouseScreenY = input.GetMouseScreenY();
    if (mouseScreenY < 32 || input.GetMouseScreenX() >= camera.GetViewportWidth())
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

    float worldX = tileX * TILE_WIDTH;
    float worldY = tileY * TILE_HEIGHT;
    float worldWidth = item.width * TILE_WIDTH;
    float worldHeight = item.height * TILE_HEIGHT;

    renderer.FillRectWorld(worldX, worldY, worldWidth, worldHeight, camera, {0, 220, 255, 70});
    renderer.DrawRectWorld(worldX, worldY, worldWidth, worldHeight, camera, {0, 255, 220, 240});

    // Cursor tag
    fontRenderer.DrawString(
        "[SPAWN] " + item.name, input.GetMouseScreenX() + 14, input.GetMouseScreenY() - 10, 1, {255, 240, 100, 255});
}

} // namespace OpenArtOfWar
