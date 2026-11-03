#pragma once

#include "game/MissionController.hpp"
#include "ui/MapPreview.hpp"
#include "ui/UiKit.hpp"
#include "ui/MapFilter.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace OpenArtOfWar
{

class InputManager;
class Renderer;
class BitmapFontRenderer;
class TextureManager;
class NetMatch;
struct Settings;

// A game found on the local network
struct LanGame
{
    std::string address;
    uint16_t port {0};
    std::string name;
    std::string map;
    int players {0};
    int maxPlayers {0};
    uint32_t pingMs {0};
    std::string code;    // a room of the dedicated server
    bool locked {false}; // asks for a password
};

// What the network screen asks the application to do
enum class NetworkAction
{
    None,
    Back,
    Host,
    Join,
    Leave,
    Start,
    ServerRefresh, // the room list of the dedicated server
    ServerCreate,  // a room on the dedicated server
    ServerJoin,    // a listed room of the server (joinPort) or one by code (joinCode)
};

// Main menu "Network game": the browser (player name, local network / internet: join by address or
// create a game) and the room of a joined game (players with their faction, team, colour and start
// position, the map with its start positions to click on, chat, ready / start / leave).
class NetworkMenu
{
public:
    static constexpr int kLayoutCombo = 1000;  // id of the map layout list (the rows use row * 10 + kind)
    static constexpr int kMapCombo = 1001;     // id of the list of maps (the host picks one)
    static constexpr int kCreditsCombo = 1002; // starting credits list
    static constexpr int kTimeCombo = 1003;    // time of day list (day / night)
    static constexpr int kFilterPlayers = 1010, kFilterSize = 1011, kFilterSort = 1012; // the filter lists

    static bool IsOptionCombo(int id)
    {
        return id == kLayoutCombo || id == kCreditsCombo || id == kTimeCombo;
    }

    NetworkMenu() = default;

    // `net`: the session (nullptr while browsing); `lan`: games found on the local network
    // `server`: rooms of the dedicated server, `serverStatus`: what the server connection is doing
    NetworkAction Update(
        InputManager &input,
        int windowW,
        int windowH,
        NetMatch *net,
        Settings &settings,
        const std::vector<LanGame> &lan,
        const std::vector<LanGame> &server);
    void render(
        Renderer &renderer,
        BitmapFontRenderer &font,
        TextureManager &textures,
        int windowW,
        int windowH,
        const NetMatch *net,
        const Settings &settings,
        const std::vector<LanGame> &lan,
        const std::vector<LanGame> &server,
        const std::string &serverStatus);
    bool InternetTab() const
    {
        return m_tab == Tab::Internet;
    }
    // The host's choice: prebuilt bases (true) or start from scratch (only HQ + power plant, no units)
    bool Prebuilt() const
    {
        return m_prebuilt;
    }
    void SetPrebuilt(bool prebuilt)
    {
        m_prebuilt = prebuilt;
    }
    // Sends the owner's match options (fog of war, bases, credits) to the room
    void PushOptions(NetMatch &net);
    // The room password: set for a game created here, sent when joining a room that asks for it (empty: none)
    const std::string &password() const
    {
        return m_password;
    }
    const std::string &JoinCode() const
    {
        return m_joinCode;
    }

    // Where to connect after NetworkAction::Join
    const std::string &JoinAddress() const
    {
        return m_joinAddress;
    }
    uint16_t JoinPort() const
    {
        return m_joinPort;
    }
    // A text field has the focus: the application turns SDL text input on
    bool WantsText() const
    {
        return m_focus != Field::None;
    }
    // A message for the browser (a failed connection, a rejection)
    void SetNotice(std::string text)
    {
        m_notice = std::move(text);
    }
    void Reset();

private:
    enum class Field
    {
        None,
        Name,
        Password,
        Address,
        Code,
        Chat,
        Search // the search box over the list of maps
    };
    enum class Tab
    {
        Lan,
        Internet
    };

    struct BrowserLayout
    {
        ui::UiRect dialog, nameField, passwordField, tabLan, tabInternet, list, addressField, directBtn, serverList,
            refreshBtn, codeField, codeBtn, joinBtn, hostBtn, serverBtn, backBtn;
        ui::UiRect ServerRow(int i) const
        {
            return {serverList.x + 4, serverList.y + 4 + i * 34, serverList.w - 8, 30};
        }
    };
    // The room is laid out like the custom game window (left: search, filters, list of maps and the chat; right: the
    // seats, options and the map picture); everything is a share of the window, nothing has a fixed width
    struct LobbyLayout
    {
        ui::UiRect dialog, details, mapSearch, mapList, players, map, chatLog, chatField, chatSend, readyBtn, startBtn,
            leaveBtn, prebuiltBtn, creditsBtn, timeBtn, fogCheckbox;
        int listRowH {28};
        int rowH {34};
        ui::UiRect Filter(int i) const
        {
            const int w = (mapSearch.w - 2 * 6) / 3;
            return {mapSearch.x + i * (w + 6), mapSearch.y + 36, w, 28};
        }
        int MapRows() const
        {
            return std::max(1, mapList.h / listRowH);
        }
        ui::UiRect MapItem(int i) const
        {
            return {mapList.x, mapList.y + i * listRowH, mapList.w, listRowH - 2};
        }
        ui::UiRect row(int i) const
        {
            return {players.x, players.y + 24 + i * rowH, players.w, rowH - 4};
        }
        // Columns of a row (a row is a start position of the map): what holds it, faction, team, colour, form, army size,
        // ready; each is a share of the width of the table
        ui::UiRect Col(int i, int k) const
        {
            static constexpr int kShare[7] = {24, 17, 8, 8, 12, 13, 18};
            const ui::UiRect r = row(i);
            const int gap = 4, x0 = r.x + 30, total = r.w - 30 - 6 * gap;
            int used = 0;
            for (int j = 0; j < k; ++j)
                used += kShare[j];
            const int x = x0 + total * used / 100 + k * gap;
            const int w = total * kShare[k] / 100;
            return {x, r.y + 4, w, r.h - 8};
        }
        ui::UiRect TypeCell(int i) const
        {
            return Col(i, 0);
        }
        ui::UiRect FactionCell(int i) const
        {
            return Col(i, 1);
        }
        ui::UiRect TeamCell(int i) const
        {
            return Col(i, 2);
        }
        ui::UiRect ColorCell(int i) const
        {
            return Col(i, 3);
        }
        ui::UiRect HandicapCell(int i) const
        {
            return Col(i, 4);
        }
        ui::UiRect ArmyCell(int i) const
        {
            return Col(i, 5);
        }
    };
    static BrowserLayout browserLayout(int w, int h);
    static LobbyLayout lobbyLayout(int w, int h);

    NetworkAction UpdateBrowser(
        InputManager &input,
        int w,
        int h,
        Settings &settings,
        const std::vector<LanGame> &lan,
        const std::vector<LanGame> &server);
    NetworkAction UpdateLobby(InputManager &input, int w, int h, NetMatch &net);
    void EditText(InputManager &input, std::string &text, size_t maxLength);
    bool ParseAddress(const std::string &text);
    // The map picture area: where a start position (tile) lands on the screen
    bool SpotOnScreen(const ui::UiRect &area, int mapW, int mapH, int tileX, int tileY, int &sx, int &sy) const;

    Tab m_tab {Tab::Lan};
    Field m_focus {Field::None};
    int m_lanSelected {-1};
    int m_serverSelected {-1};
    std::string m_joinCode;
    std::string m_joinAddress;
    uint16_t m_joinPort {0};
    std::string m_notice;
    std::string m_chatInput;
    std::vector<std::string> m_chatLog;
    std::vector<SkirmishMapInfo> m_maps;      // every map
    std::vector<SkirmishMapInfo> m_mapsShown; // after the search and the filters
    ui::MapFilter m_mapFilter;
    int m_mapTop {0};                         // first visible row of the map list
    void RebuildMapsShown();
    std::string m_password;
    ui::ConfirmState m_confirm; // "leave the room?"
    ui::ComboState m_combo;     // the open drop-down list of the room
    bool m_fog {true};          // host: fog of war
    int m_credits {1};          // host: index of the starting credits (kStartingCredits)
    bool m_night {false};       // host: the match is played at night
    bool m_optionsSent {false}; // the options of this room went to the room already
    bool m_prebuilt {true};     // host: prebuilt bases (else from scratch: HQ + power plant only)
    int m_mapIndex {0};
    std::string m_spotsMap; // map the start positions below belong to
    std::vector<std::pair<int, int>> m_spots;
    int m_mapW {0}, m_mapH {0};
    MapPreview m_preview;
    int m_mouseX {0}, m_mouseY {0};
    bool m_wasInLobby {false};
};

} // namespace OpenArtOfWar
