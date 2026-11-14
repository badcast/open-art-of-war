#pragma once

#include "core/Config.hpp"
#include "core/FrameContext.hpp"
#include "game/Simulation.hpp"
#include <array>
#include <memory>
#include <cstdint>
#include <functional>
#include <vector>

namespace OpenArtOfWar
{

class Unit;
class InputManager;
class Camera;
class TileMap;
class Pathfinder;
class EntityManager;
class Economy;
class AIController;
class MissionController;
class HUDController;
class MenuController;
class SoundEngine;
class Renderer;
class BitmapFontRenderer;

// Player control group (Ctrl+1..0 assigns, 1..0 selects)
struct ControlGroup
{
    std::vector<int> unitIds;
    int leaderId {0};
};

class EnvironmentConfig;

class GameplayController
{
public:
    GameplayController() = default;

    void Update(const FrameContext &ctx);

    void RenderSelectionBox(Renderer &renderer, const Camera &camera, const InputManager &input);

    // Order lines: from every selected own unit to its attack target and to where it walks (white); with `showAll`
    // (Alt held) from every own and allied unit (never an enemy's). Look (color #hex, width, opacity) from the
    // "overlay" section of assets/data/game_environment.json
    void LoadOverlayStyle(const EnvironmentConfig &env);
    void RenderOrderLines(
        Renderer &renderer,
        const Camera &camera,
        const EntityManager &entities,
        const TileMap &map,
        bool showAll) const;
    void RenderDebugNavigation(
        Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map);
    void RenderDebugGrid(Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map);
    void RenderDebugToolbar(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW);
    void RenderDebugSpawnDropdown(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowH);
    void RenderDebugSpawnGhost(
        Renderer &renderer,
        const Camera &camera,
        const InputManager &input,
        const TileMap &map,
        BitmapFontRenderer &fontRenderer);

    // Touch interface: with units selected a tap on the ground (or on an enemy) orders them (move; a second tap on the
    // same spot: attack-move) instead of selecting; a tap on an own unit selects it
    void SetTouchMode(bool on)
    {
        m_touchMode = on;
    }
    void SetDebugMode(bool enabled);
    bool IsDebugMode() const
    {
        return m_debugMode;
    }
    // A replay holds the world still (paused) while the interface stays alive
    void SetSimulationHold(bool hold)
    {
        m_simHold = hold;
    }
    // One tick of the world (after the input): commands, research, entities, economies, AI, mission
    void SimulationStep(
        float deltaTime,
        TileMap &map,
        Pathfinder &pathfinder,
        EntityManager &entities,
        Economy &economy,
        AIController &aiController,
        MissionController &mission,
        HUDController &hud,
        AppState &appState);
    bool IsRevealFog() const
    {
        return m_debugMode && m_revealFog;
    }
    bool IsSpawnMode() const
    {
        return m_debugMode && m_spawnMode;
    }
    bool IsShowDebugGrid() const
    {
        return m_debugMode && m_showDebugGrid;
    }
    void SetShowDebugGrid(bool show)
    {
        m_showDebugGrid = show;
    }

    const std::vector<int> &GetSelectedUnits() const
    {
        return m_selectedUnits;
    }
    std::vector<int> &GetSelectedUnits()
    {
        return m_selectedUnits;
    }
    void ClearSelection()
    {
        m_selectedUnits.clear();
    }

    const std::array<ControlGroup, 10> &GetControlGroups() const
    {
        return m_groups;
    }
    // A mission / match was just loaded: fresh simulation, selection and groups (also done by update when
    // it notices; a network match calls it at once, before the first turn's commands are scheduled)
    void ResetForMission(const MissionController &mission, HUDController &hud, EntityManager &entities, TileMap &map);
    // Commands, marching groups and research of the match (the interface only issues and reads)
    Simulation &simulation()
    {
        return m_sim;
    }
    const Simulation &simulation() const
    {
        return m_sim;
    }
    // The research a player has under way: the simulation's for a person, the computer player's own otherwise
    ResearchProgress ResearchOf(PlayerId player) const;
    // The mission load the controller has set itself up for (the simulation is reset on its first update)
    int MissionSerial() const
    {
        return m_missionSerial;
    }
    // The computer player of a match in that slot, null when the slot is a person's (or empty)
    const AIController *MatchAi(PlayerId player) const;
    // Bar of the non-empty control groups at the bottom of the map view
    void RenderGroupBar(
        Renderer &renderer,
        BitmapFontRenderer &font,
        const EntityManager &entities,
        int mapViewW,
        int windowH,
        int mx,
        int my) const;
    // Leader marker of the selected groups and, while marching, lines from the members to the leader
    void RenderGroupLeaders(Renderer &renderer, const Camera &camera, const EntityManager &entities) const;

    // True once after the debug toolbar's IMGUI button was clicked
    bool ConsumeDebugOverlayToggle()
    {
        bool v = m_debugOverlayToggleRequested;
        m_debugOverlayToggleRequested = false;
        return v;
    }

private:
    struct LineStyle
    {
        SDL_Color color {255, 255, 255, 255};
        float width {1.5f};
    };
    struct OverlayStyle
    {
        float opacity {0.5f}; // order lines at half opacity: they mark where the units were sent
        LineStyle move {{255, 255, 255, 255}, 1.5f};
        LineStyle attack {{255, 60, 60, 255}, 1.5f};
        LineStyle wall {{255, 140, 0, 255}, 1.5f};
    };
    OverlayStyle m_overlay;

    void PruneGroups(const EntityManager &entities);
    void SelectGroup(int index, EntityManager &entities);
    void HandleControlGroupKeys(InputManager &input, EntityManager &entities, Camera &camera);
    int GroupBarHit(int mx, int my, int mapViewW, int windowH) const;
    int GroupOfUnits(const std::vector<int> &unitIds) const;
    // Order for the selected units at a world point: a whole control group marches as one squad
    // (leash, chokepoints), anything else gets a plain order
    void OrderAt(const std::vector<int> &units, float worldX, float worldY, OrderKind kind, bool queued = false);
    inline void orderAt(const std::vector<int> &units, float worldX, float worldY, OrderKind kind, bool queued = false)
    {
        OrderAt(units, worldX, worldY, kind, queued);
    }
    // Command of the local player (debug: may command any faction)
    SimCommand Command(CommandType type) const;

    std::vector<int> m_selectedUnits;
    std::array<ControlGroup, 10> m_groups;
    Simulation m_sim;
    // The computer players of a match, in slot order (every peer runs them alike)
    std::vector<std::unique_ptr<AIController>> m_matchAis;
    void CreateMatchAis(const MissionController &mission, EntityManager &entities, TileMap &map);
    int m_missionSerial {-1};
    int m_pendingDialogId {0}; // campaign dialogue still to open (MissionDialogs id), 0 = none
    float m_dialogTimer {0.0f};
    int m_lastGroupKey {-1};
    std::uint64_t m_lastGroupTicks {0};
    int m_lastClickId {0};
    bool m_touchMode {false};
    std::uint64_t m_touchOrderTicks {0}; // the last tap that ordered (a second one close by is an attack-move)
    int m_touchOrderX {-1000};
    int m_touchOrderY {-1000};
    std::vector<int> m_touchRallyBuildings; // producers remembered on 1st tap so 2nd tap sets their rally point
    std::uint64_t m_touchRallyTicks {0};
    int m_touchRallyX {-1000};
    int m_touchRallyY {-1000};
    std::uint64_t m_lastClickTicks {0};
    int m_lastClickX {-100}; // screen position of the last click (a double click on empty ground)
    int m_lastClickY {-100};
    // Selects the player's own living units for which `pick` is true; `onScreen`: only those inside the view
    void SelectOwnUnits(
        EntityManager &entities, const Camera &camera, bool onScreen, const std::function<bool(const Unit &)> &pick);

    bool m_debugMode {false};
    bool m_showNavLines {true};
    bool m_controlAllFactions {false};
    bool m_pauseAI {false};
    bool m_revealFog {false};
    bool m_simHold {false};
    bool m_spawnMode {false};
    bool m_spawnDropdownOpen {false};
    int m_selectedSpawnIndex {8}; // Default: Rhino (Res)
    bool m_showDebugGrid {false};
    bool m_debugOverlayToggleRequested {false};
};

} // namespace OpenArtOfWar
