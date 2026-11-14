#pragma once

#include "ui/HUDController.hpp"

#include <vector>

namespace OpenArtOfWar
{

/**
 * @brief The touch interface: no sidebar, the whole screen is the map and the controls float over it.
 *
 * The right-hand grid contains construction and production cards. Map taps choose and confirm a building position;
 * map drags pan the camera and a hold followed by a drag selects units.
 * Used instead of HUDController on touch devices (and with --touch-ui).
 */
class TouchHUD : public HUDController
{
public:
    int ReservedWidth() const override
    {
        return 0;
    }
    bool IsOverHud(int mx, int my, int windowW, int windowH) const override;
    // While placing, map drags pan without starting selection.
    bool TouchPansCamera() const override
    {
        return m_placing || m_buildMode;
    }
    std::vector<SDL_Rect> TouchPanels(int windowW, int windowH) const override;
    SDL_Rect TouchScrollPanel(int windowW, int windowH) const override;
    SDL_Rect TouchPlacementGhost() const override
    {
        return m_placing ? m_ghostRect : SDL_Rect {0, 0, 0, 0};
    }
    void SetPointerPressOrigin(int x, int y) override
    {
        m_pressX = x;
        m_pressY = y;
    }
    bool OwnsMapInput() const override
    {
        return m_placing || m_buildMode;
    }
    void FocusBuilding(int buildingId, const EntityManager &entities) override;

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

    struct ButtonColors
    {
        SDL_Color fill;
        SDL_Color border;
        SDL_Color activeBar;
        SDL_Color iconTint;
        Uint8 iconAlpha {200};
        SDL_Color text;
    };
    static constexpr Uint8 kHudIconAlpha = 200;
    // Adapts button chrome to the local faction/army color while keeping HUD icons crisp white at opacity 200.
    static ButtonColors ResolveButtonColors(
        ui::ButtonStyle style, bool active, bool hovered, bool disabled, bool armed, SDL_Color factionColor);

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
        Stats,
        Tech,
        DockBuild,
        DockUnits,
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
        float fontScale = 1.0F;
    };
    struct Layout
    {
        float u {1.0f};
        ui::UiRect resources, radar, cards, notice;
        std::vector<Button> buttons;
        int cardsFit {1}; // visible cards
    };
    // `hasCombat` / `hasBuilding`: what is selected (own units that fight / own structures)
    Layout MakeLayout(int windowW, int windowH, bool hasCombat, bool hasBuilding, int selectedCount) const;
    ui::UiRect CardRect(const Layout &l, int slot) const;
    int FirstCard(const Layout &l, int scrollSlot) const;
    float MaxCardScroll(const Layout &l, int tab) const;
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
    static int IconSpriteForAction(Act act);

    Panel m_panel {Panel::None};
    int m_unitTab {TabInfantry};
    ui::SmoothListScroll m_cardScroll[3]; // smooth kinetic vertical scroll: build, infantry, vehicles
    bool m_prevLeftDown {false};
    bool m_placing {false};
    int m_ghostX {0};
    int m_ghostY {0};
    int m_grabStartX {0};
    int m_grabStartY {0};
    bool m_grabMoved {false};
    bool m_grabPressedOnGhost {false};
    int m_pressX {0}, m_pressY {0};
    bool m_grab {false};
    bool m_fingerWas {false};
    float m_grabOffX {0.0f}, m_grabOffY {0.0f};
    mutable SDL_Rect m_ghostRect {0, 0, 0, 0};
    bool m_radarHeld {false};
    int m_seenTab {TabBuild};
    mutable bool m_lastHasCombat {false};
    mutable bool m_lastHasBuilding {false};
    mutable bool m_lastHasProducer {false};
    mutable int m_lastSelectedCount {0};
};

} // namespace OpenArtOfWar
