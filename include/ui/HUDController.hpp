#pragma once

#include "core/Config.hpp"
#include "game/Simulation.hpp"
#include "ui/RadarAlert.hpp"
#include "ui/UiKit.hpp"
#include <functional>
#include <string>
#include <vector>

namespace OpenArtOfWar
{

class Renderer;
class InputManager;
class Camera;
class TextureManager;
class BitmapFontRenderer;
class TileMap;
class EntityManager;
class Economy;
class SoundEngine;

// Sidebar cameo (composite id) of a trainable unit archetype, -1 when there is none
int UnitCameoCompId(int archetype);

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
        TabOrders,
        TabCount
    };

    HUDController() = default;

    // Window of the technology tree: search, filter, the tree, the selected technology and its start button
    bool IsResearchWindowOpen() const
    {
        return m_researchOpen;
    }
    // The search box has the keyboard (SDL text input must be on)
    bool WantsText() const
    {
        return m_researchOpen && m_researchSearchFocus;
    }
    // Returns true while the window takes the input of this frame
    bool UpdateResearchWindow(
        InputManager &input, int mx, int my, bool leftClicked, int windowW, int windowH, Economy &economy,
        EntityManager &entities);
    void RenderResearchWindow(
        Renderer &renderer, BitmapFontRenderer &fontRenderer, const Economy &economy, const EntityManager &entities,
        int mx, int my, int windowW, int windowH);

    // `events`: what the simulation reported this tick (research done, ...)
    void Update(float deltaTime, EntityManager &entities, Economy &economy, const std::vector<SimEvent> &events);
    // The match the sidebar issues its commands to and reads research progress from
    void BindSimulation(Simulation *sim)
    {
        m_sim = sim;
    }

    void AnnounceResearch(int upgradeIndex);

    // A building of the player was selected: the sidebar switches to what it does
    // (Barracks -> infantry, Factory -> vehicles, lab -> research) and it becomes the primary one
    void FocusBuilding(int buildingId, const EntityManager &entities);

    // Drops per-mission state (running research, primary buildings, build mode)
    void ResetForMission();

    // Returns true when the mouse is over the sidebar (map input must be ignored)
    bool HandleHudClick(
        int mx,
        int my,
        bool leftClicked,
        bool rightClicked,
        bool leftMouseDown,
        int wheelSteps,
        int windowW,
        int windowH,
        Economy &economy,
        EntityManager &entities,
        TileMap &map,
        Camera &camera,
        bool &outOpenMenu,
        bool &outOpenObjectives,
        std::vector<int> &selectedUnits);

    // Production at Barracks / Factory: the selected buildings of that type (or the primary one) train the unit;
    // the order goes to the one with the shortest queue, cancelling refunds the latest matching order
    std::vector<int> ProductionTargets(
        UnitType buildingType, const EntityManager &entities, const std::vector<int> &selected) const;
    // The infinity button of a unit cameo: that unit is trained again and again by the producers (on / off)
    void ToggleRepeat(int archetype, UnitType buildingType, EntityManager &entities, const std::vector<int> &selected);
    bool OrderUnit(
        int archetype,
        UnitType buildingType,
        EntityManager &entities,
        Economy &economy,
        const std::vector<int> &selected);
    bool CancelUnitOrder(
        int archetype,
        UnitType buildingType,
        EntityManager &entities,
        Economy &economy,
        const std::vector<int> &selected);

    // Progress bars / queue size above producing buildings, rally lines of the selected ones
    void RenderProductionOverlay(
        Renderer &renderer,
        BitmapFontRenderer &fontRenderer,
        const EntityManager &entities,
        const TileMap &map,
        const Camera &camera) const;
    // Not enough power: a blinking lightning over every finished building of the player that needs power
    void RenderPowerAlerts(
        Renderer &renderer, const EntityManager &entities, const Economy &economy, const Camera &camera) const;
    // The lightning icon of the game (sprite of the sheets) and its blinking, shared with the power header
    static constexpr int kPowerIconSprite = 1778;
    static bool PowerIconBlinkOn();

    void RenderGameHud(
        Renderer &renderer,
        BitmapFontRenderer &fontRenderer,
        const Economy &economy,
        const EntityManager &entities,
        const TileMap &map,
        const Camera &camera,
        int mx,
        int my,
        int windowW,
        int windowH,
        const std::vector<int> &selected);

    void RenderBuildGhost(
        Renderer &renderer,
        const TileMap &map,
        const Camera &camera,
        int mouseTileX,
        int mouseTileY,
        const Economy &economy,
        const EntityManager &entities);

    static bool IsOverSidebar(int mx, int windowW)
    {
        return mx >= windowW - kSidebarWidth;
    }

    // Getters and setters
    bool IsBuildMode() const
    {
        return m_buildMode;
    }
    void SetBuildMode(bool bm)
    {
        m_buildMode = bm;
    }

    int GetSelectedBlueprint() const
    {
        return m_selectedBlueprint;
    }
    void SetSelectedBlueprint(int bp)
    {
        m_selectedBlueprint = bp;
    }

    bool IsAttackMoveMode() const
    {
        return m_attackMoveMode;
    }
    void SetAttackMoveMode(bool am)
    {
        m_attackMoveMode = am;
    }

    // Vertical space reserved above the sidebar (debug toolbar)
    void SetTopInset(int inset)
    {
        m_topInset = inset;
    }
    // Radar hides what the player has not seen (off while the debug fog reveal is on)
    void SetRadarFog(bool fog)
    {
        m_radarFog = fog;
    }
    // Watching a replay: no building interface (tabs, build grid, info box), the radar and the menu stay
    // Where the research window reads the research under way (a computer player keeps its own)
    void SetResearchProbe(std::function<ResearchProgress(PlayerId)> probe)
    {
        m_researchProbe = std::move(probe);
    }
    bool IsObserver() const
    {
        return m_observer;
    }
    void SetObserver(bool on)
    {
        m_observer = on;
    }
    // A replay's observer: the radar shows the whole map
    void SetShowAll(bool on)
    {
        m_showAll = on;
    }
    int GetRadarZoom() const
    {
        return m_radarZoom;
    }
    // Radar alarm settings (section "radar_alert" of game_environment.json)
    void SetRadarAlertConfig(const nlohmann::json &section)
    {
        m_radarAlert.config.load(section);
    }
    const RadarAlert &radarAlert() const
    {
        return m_radarAlert;
    }
    // Every frame, not at the simulation rate: while a drag that began on the radar holds the left button,
    // the camera glides after the point under the mouse (clamped to the radar when the mouse leaves it)
    void UpdateRadarDrag(
        int mx,
        int my,
        bool leftMouseDown,
        float frameDeltaTime,
        int windowW,
        int windowH,
        const TileMap &map,
        Camera &camera);
    static constexpr int kRadarMaxZoom = 4;
    static constexpr float kRadarFollowRate = 20.0f; // 1/s: how fast the camera catches up with a radar drag

    int GetActiveTab() const
    {
        return m_activeTab;
    }
    void SetActiveTab(int tab);

    int GetPrimaryBarracksId() const
    {
        return m_primaryBarracksId;
    }
    void SetPrimaryBarracksId(int id)
    {
        m_primaryBarracksId = id;
    }

    int GetPrimaryFactoryId() const
    {
        return m_primaryFactoryId;
    }
    void SetPrimaryFactoryId(int id)
    {
        m_primaryFactoryId = id;
    }

    const std::string &GetNotification() const
    {
        return m_hudNotification;
    }
    // The window of the technology tree (opened by the button under OBJECTIVES, or by the observer bar)
    void OpenResearchWindow();
    void CloseResearchWindow();
    void SetNotification(const std::string &msg, float time = 3.0f)
    {
        m_hudNotification = msg;
        m_hudNotificationTimer = time;
    }

private:
    int ItemCountForTab(int tab) const;
    int MaxScrollRows(int tab, int gridHeight) const;
    void ActivateCell(
        int tab, int index, bool cancel, Economy &economy, EntityManager &entities, std::vector<int> &selectedUnits);
    void CyclePrimaryBuilding(int tab, EntityManager &entities, Camera &camera);

    bool m_researchOpen {false};
    int m_researchSel {0};
    std::string m_researchSearch;
    bool m_researchSearchFocus {false};
    int m_researchFilter {0}; // 0 all, 1 available, 2 researched, 3 locked, 4 in progress
    ui::ComboState m_researchCombo;
    float m_researchScrollX {0.0f}, m_researchScrollY {0.0f};
    float m_researchScale {1.25f}; // size of the whole window and its icons (wheel, + / - buttons)
    bool m_researchPanning {false};
    int m_researchScrollDrag {0}; // 1: the vertical scroll bar is held, 2: the horizontal one
    int m_researchPanX {0}, m_researchPanY {0};
    uint32_t m_researchClickMs {0};
    int m_researchClickNode {-1};
    // Starts the technology (messages as the cameo click gave them)
    void TryStartResearch(int index, Economy &economy, EntityManager &entities);

    int m_activeTab {TabBuild};
    int m_topInset {0};
    int m_scrollRow[TabCount] {0, 0, 0, 0};
    int m_primaryBarracksId {0};
    int m_primaryFactoryId {0};
    std::string m_hudNotification;
    float m_hudNotificationTimer {0.0f};

    Simulation *m_sim {nullptr};
    std::function<ResearchProgress(PlayerId)> m_researchProbe; // whose research is shown (observers: any player)
    ResearchProgress ResearchShown() const;
    // Command of the local player for the bound simulation
    void Issue(SimCommand command);

    float m_destroyArmed {0.0f}; // > 0: the next click on "Destroy" really blows the selection up
    bool m_radarFog {true};
    bool m_observer {false}; // watching a replay
    static constexpr int kObserverBarHeight = 64; // the bar of a replay / a watched match over the screen
    bool m_showAll {false}; // a replay's observer sees the whole map
    mutable RadarAlert m_radarAlert; // updated while the radar is drawn
    int m_radarZoom {1};             // 1 = whole map, up to kRadarMaxZoom
    bool m_radarDragging {false};    // camera follows the mouse on the radar: the zoomed view holds still meanwhile
    mutable float m_radarCenterX {-1.0f}; // tile at the middle of a zoomed radar
    mutable float m_radarCenterY {-1.0f};

    bool m_buildMode {false};
    int m_selectedBlueprint {0};
    bool m_attackMoveMode {false};
};

} // namespace OpenArtOfWar
