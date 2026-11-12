#pragma once

#include "ui/HUDController.hpp"

#include <vector>

namespace OpenArtOfWar
{

/**
 * @brief The touch interface: no sidebar, the whole screen is the map and the controls float over it.
 *
 * Bottom left the radar, bottom right the dock (BUILD, UNITS, BOX), between them the orders of what is selected;
 * the cards of what can be built or trained open above the dock. A building is placed with a ghost: pick the card,
 * drag the ghost with a finger, confirm with the green button (walls and mines keep the placement going). Taps on the
 * map are handled by GameplayController (tap = move, second tap = attack-move, tap on an own unit = select).
 * Used instead of HUDController on Android (and with --touch-ui).
 */
class AndroidHUD : public HUDController
{
public:
    int ReservedWidth() const override
    {
        return 0;
    }
    bool IsOverHud(int mx, int my, int windowW, int windowH) const override;
    // Not in the box-select mode (a finger drag is the box then). While a building is placed a drag that starts on
    // the ghost moves it (the ghost is a panel of TouchPanels), one that starts elsewhere moves the camera
    bool TouchPansCamera() const override
    {
        return !m_boxSelect;
    }
    std::vector<SDL_Rect> TouchPanels(int windowW, int windowH) const override;
    bool OwnsMapInput() const override
    {
        return m_placing;
    }

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
        std::vector<int> &selectedUnits) override;

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
        const std::vector<int> &selected) override;

private:
    enum class Panel
    {
        None,
        Build,
        Units
    };
    enum class Act
    {
        None,
        Menu,
        Goals,
        Tech,
        DockBuild,
        DockUnits,
        DockBox,
        AttackMove,
        Stop,
        Hold,
        Siege,
        ClearSel,
        SelectAll,
        Repair,
        Sell,
        Confirm,
        Cancel,
        UnitsInfantry,
        UnitsVehicles,
        ScrollLeft,
        ScrollRight,
        RadarZoomIn,
        RadarZoomOut
    };
    struct Button
    {
        ui::UiRect r;
        Act act {Act::None};
        std::string label;
        bool active {false};
        bool disabled {false};
        ui::ButtonStyle style {ui::ButtonStyle::Secondary};
    };
    struct Layout
    {
        float u {1.0f};
        ui::UiRect resources, radar, cards, notice;
        std::vector<Button> buttons;
        int cardsFit {1}; // cards that fit side by side
    };
    // `hasCombat` / `hasBuilding`: what is selected (own units that fight / own structures)
    Layout MakeLayout(int windowW, int windowH, bool hasCombat, bool hasBuilding, int selectedCount) const;
    ui::UiRect CardRect(const Layout &l, int slot) const;
    bool OverPanels(const Layout &l, int mx, int my) const;
    void Press(
        Act act,
        const Layout &l,
        Economy &economy,
        EntityManager &entities,
        TileMap &map,
        Camera &camera,
        bool &outOpenMenu,
        bool &outOpenObjectives,
        std::vector<int> &selectedUnits);
    void UpdateGhostRect(const Camera &camera);
    void StartPlacing(const Camera &camera);
    bool ConfirmPlacing(Economy &economy, const EntityManager &entities, const TileMap &map);
    void SyncPanelWithTab();

    Panel m_panel {Panel::None};
    int m_unitTab {TabInfantry};
    int m_scroll[3] {0, 0, 0}; // first card shown: build, infantry, vehicles
    bool m_placing {false};
    int m_ghostX {0};
    int m_ghostY {0};
    bool m_boxSelect {false};
    bool m_grab {false};                        // the finger holds the ghost
    bool m_fingerWas {false};                   // the finger was down on the last call
    float m_grabOffX {0.0f}, m_grabOffY {0.0f}; // ghost corner minus finger, world pixels, from where it was taken
    mutable SDL_Rect m_ghostRect {0, 0, 0, 0};  // the ghost on the screen (with room for a finger)
    bool m_radarHeld {false};
    int m_seenTab {TabBuild};
    // The panels of the last frame, for the input layer (TouchPanels)
    mutable std::vector<SDL_Rect> m_panels;
};

} // namespace OpenArtOfWar
