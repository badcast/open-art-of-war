#pragma once

#include "core/Config.hpp"
#include "game/Simulation.hpp"
#include <string>
#include <vector>

namespace OpenArtOfWar
{

    class Renderer;
    class Camera;
    class TextureManager;
    class BitmapFontRenderer;
    class TileMap;
    class EntityManager;
    class Economy;
    class SoundEngine;

    // Sidebar cameo (composite id) of a trainable unit archetype, -1 when there is none
    int unitCameoCompId(int archetype);

    /**
     * @brief In-game command sidebar docked to the right edge of the window.
     *
     * Top to bottom: credits & power, radar, menu buttons, category tabs
     * (Build / Infantry / Vehicles / Research / Orders), a scrollable cameo grid and an info box.
     */
    class HUDController
    {
    public:
        static constexpr int kSidebarWidth = 300;

        enum Tab : int
        {
            TabBuild = 0,
            TabInfantry,
            TabVehicles,
            TabResearch,
            TabOrders,
            TabCount
        };

        HUDController() = default;

        // `events`: what the simulation reported this tick (research done, ...)
        void update(float deltaTime, EntityManager &entities, Economy &economy, const std::vector<SimEvent> &events);
        // The match the sidebar issues its commands to and reads research progress from
        void bindSimulation(Simulation *sim)
        {
            m_sim = sim;
        }

        void announceResearch(int upgradeIndex);

        // A building of the player was selected: the sidebar switches to what it does
        // (Barracks -> infantry, Factory -> vehicles, lab -> research) and it becomes the primary one
        void focusBuilding(int buildingId, const EntityManager &entities);

        // Drops per-mission state (running research, primary buildings, build mode)
        void resetForMission();

        // Returns true when the mouse is over the sidebar (map input must be ignored)
        bool handleHUDClick(int mx, int my, bool leftClicked, bool rightClicked, bool leftMouseDown, int wheelSteps, int windowW, int windowH, Economy &economy, EntityManager &entities, TileMap &map, Camera &camera, bool &outOpenMenu, bool &outOpenObjectives, std::vector<int> &selectedUnits);

        // Production at Barracks / Factory: the selected buildings of that type (or the primary one) train the unit;
        // the order goes to the one with the shortest queue, cancelling refunds the latest matching order
        std::vector<int> productionTargets(UnitType buildingType, const EntityManager &entities, const std::vector<int> &selected) const;
        bool orderUnit(int archetype, UnitType buildingType, EntityManager &entities, Economy &economy, const std::vector<int> &selected);
        bool cancelUnitOrder(int archetype, UnitType buildingType, EntityManager &entities, Economy &economy, const std::vector<int> &selected);

        // Progress bars / queue size above producing buildings, rally lines of the selected ones
        void renderProductionOverlay(Renderer &renderer, BitmapFontRenderer &fontRenderer, const EntityManager &entities, const TileMap &map, const Camera &camera) const;

        void renderGameHUD(Renderer &renderer, BitmapFontRenderer &fontRenderer, const Economy &economy, const EntityManager &entities, const TileMap &map, const Camera &camera, int mx, int my, int windowW, int windowH, const std::vector<int> &selected);

        void renderBuildGhost(Renderer &renderer, const TileMap &map, const Camera &camera, int mouseTileX, int mouseTileY, const Economy &economy, const EntityManager &entities);

        static bool isOverSidebar(int mx, int windowW)
        {
            return mx >= windowW - kSidebarWidth;
        }

        // Getters and setters
        bool isBuildMode() const
        {
            return m_buildMode;
        }
        void setBuildMode(bool bm)
        {
            m_buildMode = bm;
        }

        int getSelectedBlueprint() const
        {
            return m_selectedBlueprint;
        }
        void setSelectedBlueprint(int bp)
        {
            m_selectedBlueprint = bp;
        }

        bool isAttackMoveMode() const
        {
            return m_attackMoveMode;
        }
        void setAttackMoveMode(bool am)
        {
            m_attackMoveMode = am;
        }

        // Vertical space reserved above the sidebar (debug toolbar)
        void setTopInset(int inset)
        {
            m_topInset = inset;
        }
        // Radar hides what the player has not seen (off while the debug fog reveal is on)
        void setRadarFog(bool fog)
        {
            m_radarFog = fog;
        }
        int getRadarZoom() const
        {
            return m_radarZoom;
        }
        // Every frame, not at the simulation rate: while a drag that began on the radar holds the left button,
        // the camera glides after the point under the mouse (clamped to the radar when the mouse leaves it)
        void updateRadarDrag(int mx, int my, bool leftMouseDown, float frameDeltaTime, int windowW, int windowH, const TileMap &map, Camera &camera);
        static constexpr int kRadarMaxZoom = 4;
        static constexpr float kRadarFollowRate = 20.0f; // 1/s: how fast the camera catches up with a radar drag

        int getActiveTab() const
        {
            return m_activeTab;
        }
        void setActiveTab(int tab);

        int getPrimaryBarracksId() const
        {
            return m_primaryBarracksId;
        }
        void setPrimaryBarracksId(int id)
        {
            m_primaryBarracksId = id;
        }

        int getPrimaryFactoryId() const
        {
            return m_primaryFactoryId;
        }
        void setPrimaryFactoryId(int id)
        {
            m_primaryFactoryId = id;
        }

        const std::string &getNotification() const
        {
            return m_hudNotification;
        }
        void setNotification(const std::string &msg, float time = 3.0f)
        {
            m_hudNotification = msg;
            m_hudNotificationTimer = time;
        }

    private:
        int itemCountForTab(int tab) const;
        int maxScrollRows(int tab, int gridHeight) const;
        void activateCell(int tab, int index, bool cancel, Economy &economy, EntityManager &entities, std::vector<int> &selectedUnits);
        void cyclePrimaryBuilding(int tab, EntityManager &entities, Camera &camera);

        int m_activeTab {TabBuild};
        int m_topInset {0};
        int m_scrollRow[TabCount] {0, 0, 0, 0, 0};
        int m_primaryBarracksId {0};
        int m_primaryFactoryId {0};
        std::string m_hudNotification;
        float m_hudNotificationTimer {0.0f};

        Simulation *m_sim {nullptr};
        // Command of the local player for the bound simulation
        void issue(SimCommand command);

        float m_destroyArmed {0.0f}; // > 0: the next click on "Destroy" really blows the selection up
        bool m_radarFog {true};
        int m_radarZoom {1};                  // 1 = whole map, up to kRadarMaxZoom
        bool m_radarDragging {false};         // camera follows the mouse on the radar: the zoomed view holds still meanwhile
        mutable float m_radarCenterX {-1.0f}; // tile at the middle of a zoomed radar
        mutable float m_radarCenterY {-1.0f};

        bool m_buildMode {false};
        int m_selectedBlueprint {0};
        bool m_attackMoveMode {false};
    };

} // namespace OpenArtOfWar
