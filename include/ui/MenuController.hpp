#pragma once

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
        std::function<void()> applyNow;         // rebuild the window with the configured API
        std::function<bool()> effectsAvailable; // shader effects work with the running API
    };

    class MenuController
    {
    public:
        MenuController() = default;

        // The player's options the settings screen edits; onChanged applies (and stores) one group of them
        // The application shows its loading screen (the map's picture, at least three seconds) around the loading of a game
        void bindLoading(std::function<void(const std::string &, const std::string &)> begin, std::function<void()> end)
        {
            m_beginLoading = std::move(begin);
            m_endLoading = std::move(end);
        }
        void bindSettings(Settings &settings, std::function<void(SettingsArea)> onChanged, std::function<const std::vector<Resolution> &()> resolutions);
        void bindRenderApi(RenderApiBinding binding)
        {
            m_renderApi = std::move(binding);
        }
        // Opens the settings screen on `tab` (0 video, 1 graphics, 2 audio, 3 game)
        void showSettingsTab(int tab)
        {
            m_settingsTab = tab;
            m_settingsSelected = 0;
        }

        // Menu States
        bool isInGameMenuOpen() const
        {
            return m_inGameMenuOpen;
        }
        void setInGameMenuOpen(bool open)
        {
            m_inGameMenuOpen = open;
        }

        bool isInGameSettingsOpen() const
        {
            return m_inGameSettingsOpen;
        }
        bool isObjectivesDialogOpen() const
        {
            return m_objectivesDialogOpen;
        }
        void setObjectivesDialogOpen(bool open)
        {
            m_objectivesDialogOpen = open;
        }

        bool isCampaignSelectOpen() const
        {
            return m_campaignSelectOpen;
        }
        void setCampaignSelectOpen(bool open)
        {
            m_campaignSelectOpen = open;
        }

        int getSelectedMenuItem() const
        {
            return m_selectedMenuItem;
        }
        void setSelectedMenuItem(int item)
        {
            m_selectedMenuItem = item;
        }

        // Update / Event Handlers
        bool updateInGameMenu(InputManager &input, int mx, int my, bool leftClicked, int windowW, int windowH, SoundEngine &sound, MissionController &mission, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder, Camera &camera, AppState &state);

        bool updateObjectivesDialog(InputManager &input, bool leftClicked);

        bool updateCampaignSelect(InputManager &input, int mx, int my, bool leftClicked, int windowW, int windowH, MissionController &mission, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder, Camera &camera, AppState &state, AIController &ai);

        void updateMainMenu(InputManager &input, int windowW, int windowH, AppState &state, bool &running, SoundEngine &sound, MissionController &mission, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder, Camera &camera, AIController &ai);

        void updateSettings(InputManager &input, int windowW, int windowH, AppState &state);

        // Help / About: any click, ESC or ENTER returns to the main menu
        void updateInfoScreen(InputManager &input, AppState &state);

        void activateMenuItem(int index, AppState &state, bool &running, SoundEngine &sound, MissionController &mission, TileMap &map, EntityManager &entities, Economy &economy, Pathfinder &pathfinder, Camera &camera);

        // Renderers
        void renderSplashLogo(Renderer &renderer, Camera &camera, TextureManager &texManager, int windowW, int windowH);
        void renderSplashTitle(Renderer &renderer, Camera &camera, TextureManager &texManager, int windowW, int windowH);
        void renderMainMenu(Renderer &renderer, TextureManager &texManager, BitmapFontRenderer &fontRenderer, const SoundEngine &sound, int windowW, int windowH);
        void renderMenuHelp(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH);
        void renderMenuAbout(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH);
        void renderMenuSettings(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH);
        void renderBriefing(Renderer &renderer, BitmapFontRenderer &fontRenderer, const MissionController &mission, int windowW, int windowH);
        void renderInGamePauseMenu(Renderer &renderer, BitmapFontRenderer &fontRenderer, const SoundEngine &sound, int windowW, int windowH);
        void renderObjectivesDialog(Renderer &renderer, BitmapFontRenderer &fontRenderer, const MissionController &mission, int windowW, int windowH);
        void renderCampaignMissionSelect(Renderer &renderer, TextureManager &texManager, BitmapFontRenderer &fontRenderer, int windowW, int windowH);

    private:
        MenuBackdropFx m_backdropFx; // soft explosions behind the main menu
        void updateSettingsScreen(InputManager &input, int mx, int my, bool clicked, int windowW, int windowH, AppState &state);

        void renderInfoDialog(Renderer &renderer, BitmapFontRenderer &fontRenderer, int windowW, int windowH, const std::string &title, const std::vector<std::string> &lines);

        std::string m_editorNotice; // why the map editor did not start, shown in the selection dialog
        MapPreview m_mapPreview;    // pictures of the maps in the selection dialog
        int m_selectedMenuItem {0};
        bool m_inGameMenuOpen {false};
        int m_inGameMenuSelected {0};
        bool m_objectivesDialogOpen {false};
        bool m_campaignSelectOpen {false};
        int m_campaignMissionSelected {0};
        // "Custom game": the same dialog lists every map of assets/maps instead of the campaign missions
        bool m_customGame {false};
        std::vector<SkirmishMapInfo> m_skirmishMaps;
        int m_skirmishSelected {0};
        bool m_skirmishPrebuilt {true}; // custom game on a map with buildings of its own: use them (else from scratch)
        int m_skirmishTop {0};
        AIDifficulty m_difficulty {AIDifficulty::Medium}; // shown in the custom game dialog, set in update            // first visible row of the map list
        int m_settingsSelected {0};
        // Ids of the open list: the settings rows use their index, the custom game dialog the ones from kComboSide up
        static constexpr int kComboSide = 100, kComboDifficulty = 101, kComboLayout = 102;
        std::vector<std::string> campaignComboOptions(int id) const;
        int campaignComboCurrent(int id) const;
        void campaignComboApply(int id, int index);
        std::function<void(const std::string &, const std::string &)> m_beginLoading; // loading screen with a map's picture
        std::function<void()> m_endLoading;
        ui::ConfirmState m_confirm;        // the open question of the pause menu (leave / restart)
        ui::ComboState m_combo;            // the open drop-down list of the settings / the custom game dialog
        bool m_inGameSettingsOpen {false}; // the settings screen opened from the pause menu
        int m_settingsTab {0};
        int m_sliderDrag {-1}; // settings row whose slider the mouse is dragging
        Settings *m_settings {nullptr};
        std::function<void(SettingsArea)> m_onSettingsChanged;
        std::function<const std::vector<Resolution> &()> m_resolutions;
        RenderApiBinding m_renderApi;
        int m_scrollDrag {0};      // scroll bar or seek bar held by the mouse: 0 none, 1 map list, 2 About, 3 track position
        ui::UiRect m_infoTrack {}; // About scroll bar of the last frame (empty when the text fits)
        int m_infoTotal {0}, m_infoVisible {0};
        int m_infoScroll {0}; // first visible line of the About dialog
        int m_mouseX {0};
        int m_mouseY {0};
    };

} // namespace OpenArtOfWar
