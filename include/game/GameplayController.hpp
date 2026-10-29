#pragma once

#include "core/Config.hpp"
#include "game/Simulation.hpp"
#include <array>
#include <memory>
#include <cstdint>
#include <vector>

namespace OpenArtOfWar
{

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
        friend class HeadlessTest;

    public:
        GameplayController() = default;

        void update(
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
            int windowH);

        void renderSelectionBox(Renderer &renderer, const Camera &camera, const InputManager &input);

        // Order lines: from every selected own unit to its attack target and to where it walks (white); with `showAll`
        // (Alt held) from every own and allied unit (never an enemy's). Look (color #hex, width, opacity) from the "overlay" section of
        // assets/data/game_environment.json
        void loadOverlayStyle(const EnvironmentConfig &env);
        void renderOrderLines(Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map, bool showAll) const;
        void renderDebugNavigation(Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map);
        void renderDebugGrid(Renderer &renderer, const Camera &camera, const EntityManager &entities, const TileMap &map);
        void renderDebugToolbar(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW);
        void renderDebugSpawnDropdown(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowH);
        void renderDebugSpawnGhost(Renderer &renderer, const Camera &camera, const InputManager &input, const TileMap &map, BitmapFontRenderer &fontRenderer);

        void setDebugMode(bool enabled);
        bool isDebugMode() const
        {
            return m_debugMode;
        }
        bool isRevealFog() const
        {
            return m_debugMode && m_revealFog;
        }
        bool isSpawnMode() const
        {
            return m_debugMode && m_spawnMode;
        }
        bool isShowDebugGrid() const
        {
            return m_debugMode && m_showDebugGrid;
        }
        void setShowDebugGrid(bool show)
        {
            m_showDebugGrid = show;
        }

        const std::vector<int> &getSelectedUnits() const
        {
            return m_selectedUnits;
        }
        std::vector<int> &getSelectedUnits()
        {
            return m_selectedUnits;
        }
        void clearSelection()
        {
            m_selectedUnits.clear();
        }

        const std::array<ControlGroup, 10> &getControlGroups() const
        {
            return m_groups;
        }
        // A mission / match was just loaded: fresh simulation, selection and groups (also done by update when
        // it notices; a network match calls it at once, before the first turn's commands are scheduled)
        void resetForMission(const MissionController &mission, HUDController &hud, EntityManager &entities, TileMap &map);
        // Commands, marching groups and research of the match (the interface only issues and reads)
        Simulation &simulation()
        {
            return m_sim;
        }
        const Simulation &simulation() const
        {
            return m_sim;
        }
        // Bar of the non-empty control groups at the bottom of the map view
        void renderGroupBar(Renderer &renderer, BitmapFontRenderer &font, const EntityManager &entities, int mapViewW, int windowH, int mx, int my) const;
        // Leader marker of the selected groups and, while marching, lines from the members to the leader
        void renderGroupLeaders(Renderer &renderer, const Camera &camera, const EntityManager &entities) const;

        // True once after the debug toolbar's IMGUI button was clicked
        bool consumeDebugOverlayToggle()
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
            float opacity {0.8f};
            LineStyle move {{255, 255, 255, 255}, 1.5f};
            LineStyle attack {{255, 60, 60, 255}, 1.5f};
            LineStyle wall {{255, 140, 0, 255}, 1.5f};
        };
        OverlayStyle m_overlay;

        void pruneGroups(const EntityManager &entities);
        void selectGroup(int index, EntityManager &entities);
        void handleControlGroupKeys(InputManager &input, EntityManager &entities, Camera &camera);
        int groupBarHit(int mx, int my, int mapViewW, int windowH) const;
        int groupOfUnits(const std::vector<int> &unitIds) const;
        // Order for the selected units at a world point: a whole control group marches as one squad
        // (leash, chokepoints), anything else gets a plain order
        void orderAt(const std::vector<int> &units, float worldX, float worldY, OrderKind kind);
        // Command of the local player (debug: may command any faction)
        SimCommand command(CommandType type) const;

        std::vector<int> m_selectedUnits;
        std::array<ControlGroup, 10> m_groups;
        Simulation m_sim;
        // The computer players of a match, in slot order (every peer runs them alike)
        std::vector<std::unique_ptr<AIController>> m_matchAis;
        void createMatchAis(const MissionController &mission, EntityManager &entities, TileMap &map);
        // One tick of the world (after the input): commands, research, entities, economies, AI, mission
        void simulationStep(float deltaTime, TileMap &map, Pathfinder &pathfinder, EntityManager &entities, Economy &economy, AIController &aiController, MissionController &mission, HUDController &hud, AppState &appState);
        int m_missionSerial {-1};
        int m_lastGroupKey {-1};
        std::uint64_t m_lastGroupTicks {0};
        int m_lastClickId {0};
        std::uint64_t m_lastClickTicks {0};

        bool m_debugMode {false};
        bool m_showNavLines {true};
        bool m_controlAllFactions {false};
        bool m_pauseAI {false};
        bool m_revealFog {false};
        bool m_spawnMode {false};
        bool m_spawnDropdownOpen {false};
        int m_selectedSpawnIndex {8}; // Default: Rhino (Res)
        bool m_showDebugGrid {false};
        bool m_debugOverlayToggleRequested {false};
    };

} // namespace OpenArtOfWar
