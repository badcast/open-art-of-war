#pragma once

#include "game/MissionController.hpp"
#include "ui/MapPreview.hpp"
#include "ui/UiKit.hpp"

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
        NetworkMenu() = default;

        // `net`: the session (nullptr while browsing); `lan`: games found on the local network
        // `server`: rooms of the dedicated server, `serverStatus`: what the server connection is doing
        NetworkAction update(InputManager &input, int windowW, int windowH, NetMatch *net, Settings &settings, const std::vector<LanGame> &lan, const std::vector<LanGame> &server);
        void render(Renderer &renderer, BitmapFontRenderer &font, TextureManager &textures, int windowW, int windowH, const NetMatch *net, const Settings &settings, const std::vector<LanGame> &lan, const std::vector<LanGame> &server, const std::string &serverStatus);
        bool internetTab() const
        {
            return m_tab == Tab::Internet;
        }
        // The host's choice for a map that has buildings of its own: play with them (true) or start from scratch
        bool prebuilt() const
        {
            return m_prebuilt && m_mapHasPrebuilt;
        }
        // The room password: set for a game created here, sent when joining a room that asks for it (empty: none)
        const std::string &password() const
        {
            return m_password;
        }
        const std::string &joinCode() const
        {
            return m_joinCode;
        }

        // Where to connect after NetworkAction::Join
        const std::string &joinAddress() const
        {
            return m_joinAddress;
        }
        uint16_t joinPort() const
        {
            return m_joinPort;
        }
        // A text field has the focus: the application turns SDL text input on
        bool wantsText() const
        {
            return m_focus != Field::None;
        }
        // A message for the browser (a failed connection, a rejection)
        void setNotice(std::string text)
        {
            m_notice = std::move(text);
        }
        void reset();

    private:
        enum class Field
        {
            None,
            Name,
            Password,
            Address,
            Code,
            Chat
        };
        enum class Tab
        {
            Lan,
            Internet
        };

        struct BrowserLayout
        {
            ui::UiRect dialog, nameField, passwordField, tabLan, tabInternet, list, addressField, directBtn, serverList, refreshBtn, codeField, codeBtn, joinBtn, hostBtn, serverBtn, backBtn;
            ui::UiRect serverRow(int i) const
            {
                return {serverList.x + 4, serverList.y + 4 + i * 34, serverList.w - 8, 30};
            }
        };
        struct LobbyLayout
        {
            ui::UiRect dialog, players, map, mapPrev, mapNext, chatLog, chatField, readyBtn, startBtn, leaveBtn, prebuiltBtn;
            ui::UiRect row(int i) const
            {
                return {players.x, players.y + 30 + i * 40, players.w, 36};
            }
            // Columns of a row: name, faction, team, colour, start
            ui::UiRect factionCell(int i) const
            {
                const ui::UiRect r = row(i);
                return {r.x + 170, r.y + 4, 120, r.h - 8};
            }
            ui::UiRect teamCell(int i) const
            {
                const ui::UiRect r = row(i);
                return {r.x + 296, r.y + 4, 70, r.h - 8};
            }
            ui::UiRect colorCell(int i) const
            {
                const ui::UiRect r = row(i);
                return {r.x + 372, r.y + 4, 40, r.h - 8};
            }
            ui::UiRect removeCell(int i) const
            {
                const ui::UiRect r = row(i);
                return {r.right() - 30, r.y + 6, 24, r.h - 12};
            }
            ui::UiRect nameCell(int i) const
            {
                const ui::UiRect r = row(i);
                return {r.x, r.y, 165, r.h};
            }
            // "+ AI" under the last row
            ui::UiRect addAi(int rows) const
            {
                const ui::UiRect r = row(rows);
                return {r.x, r.y + 2, 150, 30};
            }
        };
        static BrowserLayout browserLayout(int w, int h);
        static LobbyLayout lobbyLayout(int w, int h);

        NetworkAction updateBrowser(InputManager &input, int w, int h, Settings &settings, const std::vector<LanGame> &lan, const std::vector<LanGame> &server);
        NetworkAction updateLobby(InputManager &input, int w, int h, NetMatch &net);
        void editText(InputManager &input, std::string &text, size_t maxLength);
        bool parseAddress(const std::string &text);
        // The map picture area: where a start position (tile) lands on the screen
        bool spotOnScreen(const ui::UiRect &area, int mapW, int mapH, int tileX, int tileY, int &sx, int &sy) const;

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
        std::vector<SkirmishMapInfo> m_maps;
        static constexpr int kLayoutCombo = 1000; // id of the map layout list (the rows use row * 10 + kind)
        std::string m_password;
        ui::ConfirmState m_confirm;    // "leave the room?"
        ui::ComboState m_combo;        // the open drop-down list of the room
        bool m_prebuilt {true};        // host: use the map's own buildings (when it has some)
        bool m_mapHasPrebuilt {false}; // the room's map has buildings or units of its own
        int m_mapIndex {0};
        std::string m_spotsMap; // map the start positions below belong to
        std::vector<std::pair<int, int>> m_spots;
        int m_mapW {0}, m_mapH {0};
        MapPreview m_preview;
        int m_mouseX {0}, m_mouseY {0};
    };

} // namespace OpenArtOfWar
