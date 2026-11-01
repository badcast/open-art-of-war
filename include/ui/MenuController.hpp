#pragma once

#include "audio/SoundRequests.hpp"
#include "game/MissionDialogs.hpp"
#include "ui/MapPreview.hpp"
#include "ui/MenuBackdropFx.hpp"
#include "ui/UiKit.hpp"
#include "core/Config.hpp"
#include "core/Settings.hpp"
#include "core/VideoModes.hpp"
#include "graphics/RenderBackend.hpp"
#include "game/AIController.hpp"
#include "game/MissionController.hpp"
#include <functional>
#include <string>
#include <vector>

namespace OpenArtOfWar
{

class Renderer;
class Camera;
class TextureManager;
class BitmapFontRenderer;
class InputManager;
class SoundEngine;
class MissionController;
class TileMap;
class EntityManager;
class Economy;
class Pathfinder;
class AIController;
class SaveManager;

// The render API rows of the graphics settings
struct RenderApiBinding
{
    std::function<const std::vector<RenderApiOption> &()> options; // what this system offers (probed once)
    std::function<std::string()> active;                           // the API drawing right now
    std::function<std::string()> configured;                       // the API of the next start
    std::function<void(const std::string &)> select;
    std::function<void()> applyNow;         // rebuild the window with the configured API (asked for by a dialog)
    std::function<bool()> effectsAvailable; // shader effects work with the running API
};

class MenuController
{
public:
    MenuController() = default;

    // The player's options the settings screen edits; onChanged applies (and stores) one group of them
    // The application shows its loading screen (the map's picture, at least three seconds) around the loading of a game
    void BindLoading(std::function<void(const std::string &, const std::string &)> begin, std::function<void()> end)
    {
        m_beginLoading = std::move(begin);
        m_endLoading = std::move(end);
    }
    void BindSettings(
        Settings &settings,
        std::function<void(SettingsArea)> onChanged,
        std::function<const std::vector<Resolution> &()> resolutions);
    void BindRenderApi(RenderApiBinding binding)
    {
        m_renderApi = std::move(binding);
    }
    // Opens the settings screen on `tab` (0 video, 1 graphics, 2 audio, 3 game)
    void ShowSettingsTab(int tab)
    {
        m_settingsTab = tab;
        m_settingsSelected = 0;
    }

    // Menu States
    bool IsAnyMenuOpen() const
    {
        return m_inGameMenuOpen || m_inGameSettingsOpen || m_objectivesDialogOpen || m_missionDialogOpen;
    }
    bool IsInGameMenuOpen() const
    {
        return m_inGameMenuOpen;
    }
    void SetInGameMenuOpen(bool open)
    {
        if (open && !m_inGameMenuOpen)
            SoundRequests::Play("ui_show_dialog");
        m_inGameMenuOpen = open;
    }

    bool IsInGameSettingsOpen() const
    {
        return m_inGameSettingsOpen;
    }
    bool IsObjectivesDialogOpen() const
    {
        return m_objectivesDialogOpen;
    }
    void SetObjectivesDialogOpen(bool open)
    {
        if (open && !m_objectivesDialogOpen)
            SoundRequests::Play("ui_show_dialog");
        m_objectivesDialogOpen = open;
    }

    // Campaign mission dialogue (MissionDialogs): opened by the gameplay a few seconds into the mission, the game
    // waits while it is shown; closed by its button, Enter or Esc
    void OpenMissionDialog(MissionDialog dialog)
    {
        m_missionDialog = std::move(dialog);
        const bool wasOpen = m_missionDialogOpen;
        m_missionDialogOpen = !m_missionDialog.Empty();
        if (m_missionDialogOpen && !wasOpen)
            SoundRequests::Play("ui_show_dialog");
    }
    bool IsMissionDialogOpen() const
    {
        return m_missionDialogOpen;
    }
    bool UpdateMissionDialog(InputManager &input, int mx, int my, bool leftClicked, int windowW, int windowH);
    void RenderMissionDialog(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH);

    bool IsCampaignSelectOpen() const
    {
        return m_campaignSelectOpen;
    }
    void SetCampaignSelectOpen(bool open)
    {
        if (open && !m_campaignSelectOpen)
            SoundRequests::Play("ui_show_dialog");
        m_campaignSelectOpen = open;
    }

    bool IsSkirmishFogOfWar() const
    {
        return m_skirmishFogOfWar;
    }
    void SetSkirmishFogOfWar(bool enabled)
    {
        m_skirmishFogOfWar = enabled;
    }

    int GetSelectedMenuItem() const
    {
        return m_selectedMenuItem;
    }
    void SetSelectedMenuItem(int item)
    {
        m_selectedMenuItem = item;
    }

    // Update / Event Handlers
    bool UpdateInGameMenu(
        InputManager &input,
        int mx,
        int my,
        bool leftClicked,
        int windowW,
        int windowH,
        SoundEngine &sound,
        MissionController &mission,
        TileMap &map,
        EntityManager &entities,
        Economy &economy,
        Pathfinder &pathfinder,
        Camera &camera,
        AppState &state);

    bool UpdateObjectivesDialog(InputManager &input, bool leftClicked);

    bool UpdateCampaignSelect(
        InputManager &input,
        int mx,
        int my,
        bool leftClicked,
        int windowW,
        int windowH,
        MissionController &mission,
        TileMap &map,
        EntityManager &entities,
        Economy &economy,
        Pathfinder &pathfinder,
        Camera &camera,
        AppState &state,
        AIController &ai);

    void UpdateMainMenu(
        InputManager &input,
        int windowW,
        int windowH,
        AppState &state,
        bool &running,
        SoundEngine &sound,
        MissionController &mission,
        TileMap &map,
        EntityManager &entities,
        Economy &economy,
        Pathfinder &pathfinder,
        Camera &camera,
        AIController &ai);

    void UpdateSettings(InputManager &input, int windowW, int windowH, AppState &state);

    // Help / About: any click, ESC or ENTER returns to the main menu
    void UpdateInfoScreen(InputManager &input, AppState &state);

    void ActivateMenuItem(
        int index,
        AppState &state,
        bool &running,
        SoundEngine &sound,
        MissionController &mission,
        TileMap &map,
        EntityManager &entities,
        Economy &economy,
        Pathfinder &pathfinder,
        Camera &camera);

    // Renderers
    void RenderSplashLogo(Renderer &renderer, Camera &camera, TextureManager &texManager, int windowW, int windowH);
    void RenderSplashTitle(Renderer &renderer, Camera &camera, TextureManager &texManager, int windowW, int windowH);
    void RenderMainMenu(
        Renderer &renderer,
        TextureManager &texManager,
        BitmapFontRenderer &fontRenderer,
        const SoundEngine &sound,
        int windowW,
        int windowH);
    void RenderMenuHelp(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH);
    void RenderMenuAbout(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH);
    void RenderMenuSettings(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH);
    void RenderBriefing(
        Renderer &renderer,
        BitmapFontRenderer &fontRenderer,
        const MissionController &mission,
        int windowW,
        int windowH);
    void RenderInGamePauseMenu(
        Renderer &renderer, BitmapFontRenderer &fontRenderer, const SoundEngine &sound, int windowW, int windowH);
    // In a campaign mission it also tells the difficulty the computer plays at
    void RenderObjectivesDialog(
        Renderer &renderer,
        BitmapFontRenderer &fontRenderer,
        const MissionController &mission,
        AIDifficulty difficulty,
        int windowW,
        int windowH);
    void RenderCampaignMissionSelect(
        Renderer &renderer, TextureManager &texManager, BitmapFontRenderer &fontRenderer, int windowW, int windowH);

private:
    MenuBackdropFx m_backdropFx; // soft explosions behind the main menu
    void UpdateSettingsScreen(
        InputManager &input, int mx, int my, bool clicked, int windowW, int windowH, AppState &state);

    void RenderInfoDialog(
        Renderer &renderer,
        BitmapFontRenderer &fontRenderer,
        int windowW,
        int windowH,
        const std::string &title,
        const std::vector<std::string> &lines);

    std::string m_editorNotice; // why the map editor did not start, shown in the selection dialog
    MapPreview m_mapPreview;    // pictures of the maps in the selection dialog
    int m_selectedMenuItem {0};
    bool m_inGameMenuOpen {false};
    int m_inGameMenuSelected {0};
    bool m_objectivesDialogOpen {false};
    bool m_missionDialogOpen {false};
    MissionDialog m_missionDialog;
    bool m_campaignSelectOpen {false};
    int m_campaignMissionSelected {0};
    // "Custom game": the same dialog lists every map of assets/maps instead of the campaign missions
    bool m_customGame {false};
    std::vector<SkirmishMapInfo> m_skirmishMaps;
    int m_skirmishSelected {0};
    bool m_skirmishPrebuilt {true}; // custom game: prebuilt bases (else from scratch: HQ + power plant only)
    bool m_skirmishFogOfWar {true}; // custom game: fog of war enabled
    // Custom game players, as the network room has them: [0] is the person, the rest are computer players
    struct CustomSlot
    {
        int faction {0}; // computer players: 0 random, 1 Resistance, 2 Confederation (the person's comes from the side)
        int team {0};
        int color {1}; // 1..8
        int level {1}; // computer player: 0 easy .. 3 hardcore
    };
    std::vector<CustomSlot> m_customSlots;
    void EnsureCustomSlots();
    std::vector<bool> CustomTakenColors(int row) const;
    int m_skirmishTop {0};
    AIDifficulty m_difficulty {AIDifficulty::Medium}; // shown in the custom game dialog, set in update            //
                                                      // first visible row of the map list
    int m_settingsSelected {0};
    // Ids of the open list: the settings rows use their index, the custom game dialog the ones from kComboSide up
    static constexpr int kComboSide = 100, kComboDifficulty = 101, kComboLayout = 102;
    static constexpr int kComboSlot = 200; // + row * 10 + kind (1 faction, 2 team, 3 colour, 4 level)
    std::vector<std::string> CampaignComboOptions(int id) const;
    int CampaignComboCurrent(int id) const;
    void CampaignComboApply(int id, int index);
    std::function<void(const std::string &, const std::string &)> m_beginLoading; // loading screen with a map's picture
    std::function<void()> m_endLoading;
    ui::ConfirmState m_apiConfirm;     // "switch the render API?" of the graphics settings
    std::string m_apiPending;          // the API that question is about
    ui::ConfirmState m_confirm;        // the open question of the pause menu (leave / restart)
    ui::ComboState m_combo;            // the open drop-down list of the settings / the custom game dialog
    bool m_inGameSettingsOpen {false}; // the settings screen opened from the pause menu
    int m_settingsTab {0};
    int m_sliderDrag {-1}; // settings row whose slider the mouse is dragging
    Settings *m_settings {nullptr};
    std::function<void(SettingsArea)> m_onSettingsChanged;
    std::function<const std::vector<Resolution> &()> m_resolutions;
    RenderApiBinding m_renderApi;
    int m_scrollDrag {0}; // scroll bar or seek bar held by the mouse: 0 none, 1 map list, 2 About, 3 track position
    ui::UiRect m_infoTrack {}; // About scroll bar of the last frame (empty when the text fits)
    int m_infoTotal {0}, m_infoVisible {0};
    int m_infoScroll {0}; // first visible line of the About dialog
    int m_mouseX {0};
    int m_mouseY {0};
};

} // namespace OpenArtOfWar
