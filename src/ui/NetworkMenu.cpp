#include "ui/NetworkMenu.hpp"
#include "core/Localization.hpp"
#include "core/Settings.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/ShaderEffects.hpp"
#include "graphics/TextureManager.hpp"
#include "input/InputManager.hpp"
#include "net/NetMatch.hpp"
#include "openaow/net/Protocol.hpp"

#include <algorithm>

namespace OpenArtOfWar
{

    using ui::UiRect;
    namespace col = ui::color;

    namespace
    {

        constexpr uint16_t kDefaultPort = 38100;
        constexpr int kChatLines = 6;

        std::string aiLevelName(uint8_t level)
        {
            static const char *const kNames[4] = {"Easy", "Medium", "Hard", "Hardcore"};
            return _(kNames[level & 3]);
        }

        std::string factionName(uint8_t f)
        {
            if(f == 0)
                return _("Resistance");
            if(f == 1)
                return _("Confederation");
            return _("Random");
        }

        // The choices of the drop-down lists of the room: kind 1 faction, 2 team, 3 colour, 4 level of a computer player,
        // 5 how the map starts
        std::vector<std::string> comboChoices(int kind)
        {
            std::vector<std::string> out;
            if(kind == 1)
                out = {_("Random"), _("Resistance"), _("Confederation")};
            else if(kind == 2)
                for(int i = 1; i <= kMaxPlayers; ++i)
                    out.push_back(std::string(_("Team")) + " " + std::to_string(i));
            else if(kind == 3)
                for(int i = 1; i <= Settings::kArmyColors; ++i)
                    out.push_back(std::string(_("Color")) + " " + std::to_string(i));
            else if(kind == 4)
                for(int i = 0; i < 4; ++i)
                    out.push_back(aiLevelName(static_cast<uint8_t>(i)));
            else
                out = {_("PREBUILT BASES"), _("FROM SCRATCH")};
            return out;
        }

        SDL_Color slotColor(uint8_t color)
        {
            return color == 0 ? col::TextDim : armyColor(color);
        }

        // Removes the last UTF-8 character
        void popUtf8(std::string &s)
        {
            while(!s.empty())
            {
                const unsigned char c = static_cast<unsigned char>(s.back());
                s.pop_back();
                if((c & 0xC0) != 0x80)
                    break;
            }
        }

        // A small padlock: the room asks for a password
        void drawLock(Renderer &r, int x, int y, SDL_Color color)
        {
            r.drawRectScreen(x + 2, y, 7, 6, color);
            r.fillRectScreen(x, y + 5, 11, 8, color);
        }

        void drawField(Renderer &r, BitmapFontRenderer &font, const UiRect &rect, const std::string &text, const std::string &placeholder, bool focused)
        {
            ui::drawPanel(r, rect, col::PanelSunken, focused ? col::Accent : col::Border);
            const bool empty = text.empty();
            std::string shown = ui::fitText(font, empty ? placeholder : text, ui::FONT_BODY, rect.w - 24);
            if(focused)
                shown += (SDL_GetTicks() / 500) % 2 ? "_" : " ";
            font.drawString(shown, rect.x + 10, rect.centerY(), ui::FONT_BODY, empty && !focused ? col::TextMuted : col::Text, false, Anchor::LEFT | Anchor::VCENTER);
        }

    } // namespace

    void NetworkMenu::reset()
    {
        m_focus = Field::None;
        m_combo = ui::ComboState {};
        m_confirm.open = false;
        m_chatLog.clear();
        m_chatInput.clear();
        m_spotsMap.clear();
        m_preview.clear();
    }

    NetworkMenu::BrowserLayout NetworkMenu::browserLayout(int w, int h)
    {
        BrowserLayout l;
        l.dialog = ui::centeredRect(w, h, 760, 560);
        const int x = l.dialog.x + 24, inner = l.dialog.w - 48;
        l.nameField = {x + 140, l.dialog.y + 70, 220, 36};
        l.passwordField = {x + 540, l.dialog.y + 70, inner - 540, 36};
        l.tabLan = {x, l.dialog.y + 126, inner / 2 - 3, 36};
        l.tabInternet = {x + inner / 2 + 3, l.dialog.y + 126, inner / 2 - 3, 36};
        l.list = {x, l.dialog.y + 172, inner, l.dialog.h - 172 - 80};
        // Internet tab: direct address, then the server's rooms, then a room code
        l.addressField = {x, l.list.y + 22, inner - 180, 36};
        l.directBtn = {l.addressField.right() + 10, l.addressField.y, 170, 36};
        l.refreshBtn = {l.list.right() - 140, l.addressField.bottom() + 14, 140, 28};
        l.serverList = {x, l.refreshBtn.bottom() + 6, inner, 4 * 34 + 8};
        l.codeField = {x + 150, l.serverList.bottom() + 10, 130, 34};
        l.codeBtn = {l.codeField.right() + 10, l.codeField.y, 170, 34};
        l.joinBtn = {l.dialog.right() - 24 - 150, l.dialog.bottom() - 60, 150, 40};
        l.hostBtn = {l.joinBtn.x - 12 - 170, l.joinBtn.y, 170, 40};
        l.serverBtn = {l.hostBtn.x - 12 - 200, l.joinBtn.y, 200, 40};
        l.backBtn = {x, l.joinBtn.y, 120, 40};
        return l;
    }

    NetworkMenu::LobbyLayout NetworkMenu::lobbyLayout(int w, int h)
    {
        LobbyLayout l;
        l.dialog = ui::centeredRect(w, h, 1000, 640);
        const int x = l.dialog.x + 20, y = l.dialog.y + 60;
        l.players = {x, y, 520, 30 + 8 * 40};
        l.map = {x + 540, y + 30, l.dialog.right() - 20 - (x + 540), 300};
        l.mapPrev = {l.map.x, y - 4, 32, 26};
        l.mapNext = {l.map.right() - 32, y - 4, 32, 26};
        l.chatLog = {x, l.players.bottom() + 10, 520, l.dialog.bottom() - 70 - (l.players.bottom() + 10) - 44};
        l.chatField = {x, l.chatLog.bottom() + 6, 520, 36};
        l.prebuiltBtn = {l.map.right() - 8 - 190, l.map.bottom() - 8 - 28, 190, 28};
        l.leaveBtn = {l.map.x, l.dialog.bottom() - 60, 140, 40};
        l.startBtn = {l.dialog.right() - 20 - 150, l.leaveBtn.y, 150, 40};
        l.readyBtn = {l.startBtn.x - 12 - 150, l.leaveBtn.y, 150, 40};
        return l;
    }

    void NetworkMenu::editText(InputManager &input, std::string &text, size_t maxLength)
    {
        for(int i = input.takeBackspaces(); i > 0; --i)
            popUtf8(text);
        text += input.takeTypedText();
        while(text.size() > maxLength)
            popUtf8(text);
    }

    bool NetworkMenu::parseAddress(const std::string &text)
    {
        std::string a = text;
        a.erase(std::remove(a.begin(), a.end(), ' '), a.end());
        if(a.empty())
            return false;
        m_joinPort = kDefaultPort;
        const size_t colon = a.rfind(':');
        if(colon != std::string::npos && a.find(':') == colon)
        { // one colon: host:port (IPv6 is not supported)
            const std::string port = a.substr(colon + 1);
            if(port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), ::isdigit))
                return false;
            const int p = std::stoi(port);
            if(p <= 0 || p > 65535)
                return false;
            m_joinPort = static_cast<uint16_t>(p);
            a.resize(colon);
        }
        m_joinAddress = a;
        return !a.empty();
    }

    NetworkAction NetworkMenu::update(InputManager &input, int windowW, int windowH, NetMatch *net, Settings &settings, const std::vector<LanGame> &lan, const std::vector<LanGame> &server)
    {
        m_mouseX = input.getMouseScreenX();
        m_mouseY = input.getMouseScreenY();
        if(net && net->connected())
            return updateLobby(input, windowW, windowH, *net);
        if(net)
        {
            // Connecting: only cancel
            float lx, ly;
            const BrowserLayout l = browserLayout(windowW, windowH);
            if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE) || (input.wasLeftClickIssued(lx, ly) && l.backBtn.contains(m_mouseX, m_mouseY)))
            {
                return NetworkAction::Leave;
            }
            return NetworkAction::None;
        }
        return updateBrowser(input, windowW, windowH, settings, lan, server);
    }

    NetworkAction NetworkMenu::updateBrowser(InputManager &input, int w, int h, Settings &settings, const std::vector<LanGame> &lan, const std::vector<LanGame> &server)
    {
        const BrowserLayout l = browserLayout(w, h);
        float lx, ly;
        const bool clicked = input.wasLeftClickIssued(lx, ly);
        const int mx = m_mouseX, my = m_mouseY;

        if(m_focus == Field::Name)
            editText(input, settings.playerName, 24);
        if(m_focus == Field::Password)
            editText(input, m_password, 24);
        if(m_focus == Field::Address)
            editText(input, settings.lastAddress, 120);
        if(m_focus == Field::Code)
        {
            editText(input, m_joinCode, 6);
            for(char &c : m_joinCode)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        auto joinCode = [&]() -> NetworkAction
        {
            if(m_joinCode.size() != 6)
            {
                m_notice = _("A room code has six characters");
                return NetworkAction::None;
            }
            m_joinPort = 0;
            return NetworkAction::ServerJoin;
        };
        auto joinDirect = [&]() -> NetworkAction
        {
            if(!parseAddress(settings.lastAddress))
            {
                m_notice = _("Enter an address: host or host:port");
                return NetworkAction::None;
            }
            return NetworkAction::Join;
        };
        auto join = [&]() -> NetworkAction
        {
            if(m_tab == Tab::Internet && m_serverSelected >= 0 && m_serverSelected < static_cast<int>(server.size()))
            {
                m_joinCode = server[m_serverSelected].code;
                m_joinPort = server[m_serverSelected].port;
                return NetworkAction::ServerJoin;
            }
            if(m_tab == Tab::Lan)
            {
                if(m_lanSelected < 0 || m_lanSelected >= static_cast<int>(lan.size()))
                    return NetworkAction::None;
                m_joinAddress = lan[m_lanSelected].address;
                m_joinPort = lan[m_lanSelected].port;
                return NetworkAction::Join;
            }
            if(!parseAddress(settings.lastAddress))
            {
                m_notice = _("Enter an address: host or host:port");
                return NetworkAction::None;
            }
            return NetworkAction::Join;
        };

        if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            if(m_focus != Field::None)
                m_focus = Field::None;
            else
                return NetworkAction::Back;
        }
        if(input.isKeyJustPressed(SDL_SCANCODE_RETURN) || input.isKeyJustPressed(SDL_SCANCODE_KP_ENTER))
        {
            if(m_focus == Field::Address)
                return joinDirect();
            if(m_focus == Field::Code)
                return joinCode();
            m_focus = Field::None;
        }
        if(!clicked)
            return NetworkAction::None;

        m_focus = Field::None;
        if(l.nameField.contains(mx, my))
            m_focus = Field::Name;
        else if(l.passwordField.contains(mx, my))
            m_focus = Field::Password;
        if(l.tabLan.contains(mx, my))
            m_tab = Tab::Lan;
        if(l.tabInternet.contains(mx, my))
            m_tab = Tab::Internet;
        if(m_tab == Tab::Internet)
        {
            if(l.addressField.contains(mx, my))
                m_focus = Field::Address;
            if(l.codeField.contains(mx, my))
                m_focus = Field::Code;
            if(l.directBtn.contains(mx, my))
                return joinDirect();
            if(l.codeBtn.contains(mx, my))
                return joinCode();
            if(l.refreshBtn.contains(mx, my))
                return NetworkAction::ServerRefresh;
            for(int i = 0; i < static_cast<int>(server.size()) && i < 4; ++i)
            {
                if(!l.serverRow(i).contains(mx, my))
                    continue;
                if(i == m_serverSelected)
                    return join(); // a second click joins
                m_serverSelected = i;
            }
        }
        if(m_tab == Tab::Internet && l.serverBtn.contains(mx, my))
        {
            m_notice.clear();
            return NetworkAction::ServerCreate;
        }
        if(m_tab == Tab::Lan && l.list.contains(mx, my))
        {
            const int row = (my - l.list.y - 4) / 40;
            if(row >= 0 && row < static_cast<int>(lan.size()))
            {
                if(row == m_lanSelected)
                    return join(); // a second click joins
                m_lanSelected = row;
            }
        }
        if(l.backBtn.contains(mx, my))
            return NetworkAction::Back;
        if(l.hostBtn.contains(mx, my))
        {
            m_notice.clear();
            return NetworkAction::Host;
        }
        if(l.joinBtn.contains(mx, my))
        {
            m_notice.clear();
            return join();
        }
        return NetworkAction::None;
    }

    bool NetworkMenu::spotOnScreen(const UiRect &area, int mapW, int mapH, int tileX, int tileY, int &sx, int &sy) const
    {
        if(mapW <= 0 || mapH <= 0)
            return false;
        const float scale = std::min(static_cast<float>(area.w) / mapW, static_cast<float>(area.h) / mapH);
        const int ox = area.x + (area.w - static_cast<int>(mapW * scale)) / 2;
        const int oy = area.y + (area.h - static_cast<int>(mapH * scale)) / 2;
        // The base is set around the spot: show its middle
        sx = ox + static_cast<int>((tileX + 5) * scale);
        sy = oy + static_cast<int>((tileY + 5) * scale);
        return true;
    }

    NetworkAction NetworkMenu::updateLobby(InputManager &input, int w, int h, NetMatch &net)
    {
        const LobbyLayout l = lobbyLayout(w, h);
        const Net::LobbyState *lobby = net.lobby();
        float lx, ly;
        const bool clicked = input.wasLeftClickIssued(lx, ly);
        const int mx = m_mouseX, my = m_mouseY;

        for(Net::Chat &c : net.takeChat())
        {
            std::string who = "?";
            if(const Net::Roster *roster = net.roster())
            {
                for(const Net::RosterEntry &e : roster->players)
                {
                    if(e.slot == c.fromSlot)
                        who = e.name;
                }
            }
            m_chatLog.push_back(who + ": " + c.text);
            if(m_chatLog.size() > 50)
                m_chatLog.erase(m_chatLog.begin());
        }

        // The host picks the first map when the room opens
        if(net.controlsRoom() && lobby && lobby->map.empty())
        {
            if(m_maps.empty())
                m_maps = MissionController::listSkirmishMaps();
            if(!m_maps.empty())
                net.setLobbyMap(m_maps[std::clamp(m_mapIndex, 0, static_cast<int>(m_maps.size()) - 1)].path);
        }
        // Does the room's map bring buildings of its own (the host chooses whether they are used)
        m_mapHasPrebuilt = false;
        if(lobby && !lobby->map.empty())
        {
            if(m_maps.empty())
                m_maps = MissionController::listSkirmishMaps();
            for(const SkirmishMapInfo &m : m_maps)
            {
                if(m.path == lobby->map)
                    m_mapHasPrebuilt = m.hasPrebuilt;
            }
        }
        // Start positions from the room's map as this computer has it (the own file or the downloaded copy)
        if(net.lobbyMapFile() != m_spotsMap)
        {
            m_spotsMap = net.lobbyMapFile();
            m_spots = m_spotsMap.empty() ? std::vector<std::pair<int, int>> {} : MissionController::startPositions(m_spotsMap);
        }

        // Own picks
        Net::LobbyChoice mine;
        bool haveMine = false;
        int myRow = -1;
        if(lobby)
        {
            for(size_t i = 0; i < lobby->slots.size(); ++i)
            {
                if(lobby->slots[i].slot == net.localSlot())
                {
                    mine = lobby->slots[i].choice;
                    haveMine = true;
                    myRow = static_cast<int>(i);
                }
            }
        }

        // The question "leave the room?" takes everything while it is open
        if(m_confirm.open)
        {
            const int answer = ui::confirmUpdate(m_confirm, w, h, mx, my, clicked, input.isKeyJustPressed(SDL_SCANCODE_RETURN) || input.isKeyJustPressed(SDL_SCANCODE_KP_ENTER), input.isKeyJustPressed(SDL_SCANCODE_ESCAPE));
            if(answer < 0)
                return NetworkAction::None;
            m_confirm.open = false;
            return answer == 1 ? NetworkAction::Leave : NetworkAction::None;
        }
        auto askLeave = [&]()
        {
            m_confirm.open = true;
            m_confirm.title = _("LEAVE THE ROOM?");
            m_confirm.text = net.controlsRoom() ? _("You are the room owner. If you leave, the game may end for everybody in it.") : _("You leave the room. You can join it again while it is open.");
        };
        // An open drop-down list takes the mouse first; a pick changes the value, a click elsewhere closes the list
        auto comboField = [&](int id) -> ui::UiRect
        {
            if(id == kLayoutCombo)
                return l.prebuiltBtn;
            const int row = id / 10, kind = id % 10;
            return kind == 1 ? l.factionCell(row) : kind == 2 ? l.teamCell(row) : kind == 3 ? l.colorCell(row) : l.nameCell(row);
        };
        if(m_combo.open >= 0)
        {
            const int id = m_combo.open, row = id / 10, kind = id == kLayoutCombo ? 5 : id % 10;
            const bool valid = id == kLayoutCombo ? (net.controlsRoom() && m_mapHasPrebuilt) : (lobby && row < static_cast<int>(lobby->slots.size()));
            if(!valid)
            {
                m_combo.open = -1;
            }
            else
            {
                int picked = -1;
                if(ui::comboUpdate(m_combo, comboField(id), static_cast<int>(comboChoices(kind).size()), h, mx, my, clicked, input.consumeWheel(), picked))
                {
                    if(picked >= 0 && id == kLayoutCombo)
                    {
                        m_prebuilt = picked == 0;
                    }
                    else if(picked >= 0)
                    {
                        const Net::LobbySlot &s = lobby->slots[row];
                        Net::LobbyChoice choice = s.choice;
                        uint8_t level = s.aiLevel;
                        if(kind == 1)
                        {
                            choice.faction = picked == 0 ? Net::kRandom : static_cast<uint8_t>(picked - 1);
                            choice.ready = false;
                        }
                        if(kind == 2)
                        {
                            choice.team = static_cast<uint8_t>(picked);
                            choice.ready = false;
                        }
                        if(kind == 3)
                            choice.color = static_cast<uint8_t>(picked + 1);
                        if(kind == 4)
                            level = static_cast<uint8_t>(picked);
                        if(s.ai && net.controlsRoom())
                        {
                            Net::OwnerSetAi change;
                            change.slot = s.slot;
                            change.level = level;
                            change.choice = choice;
                            net.setAi(change);
                        }
                        else if(!s.ai && s.slot == net.localSlot())
                        {
                            net.setChoice(choice);
                        }
                    }
                    return NetworkAction::None;
                }
                if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
                {
                    m_combo.open = -1;
                    return NetworkAction::None;
                }
            }
        }
        auto openCombo = [&](int id)
        {
            m_combo.open = id;
            m_combo.scroll = 0;
            return NetworkAction::None;
        };

        if(m_focus == Field::Chat)
            editText(input, m_chatInput, Net::kMaxChatLength);
        if(input.isKeyJustPressed(SDL_SCANCODE_RETURN) || input.isKeyJustPressed(SDL_SCANCODE_KP_ENTER))
        {
            if(m_focus == Field::Chat && !m_chatInput.empty())
            {
                net.sendChat(m_chatInput);
                m_chatInput.clear();
            }
            else
            {
                m_focus = Field::Chat;
            }
        }
        if(input.isKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            if(m_focus != Field::None)
                m_focus = Field::None;
            else
            {
                askLeave();
                return NetworkAction::None;
            }
        }
        if(!clicked)
            return NetworkAction::None;

        m_focus = l.chatField.contains(mx, my) ? Field::Chat : Field::None;
        if(l.leaveBtn.contains(mx, my))
        {
            askLeave();
            return NetworkAction::None;
        }
        if(l.startBtn.contains(mx, my) && net.controlsRoom() && net.canStart())
            return NetworkAction::Start;
        if(net.controlsRoom() && m_mapHasPrebuilt && l.prebuiltBtn.contains(mx, my))
            return openCombo(kLayoutCombo);
        if(net.controlsRoom() && !m_maps.empty() && (l.mapPrev.contains(mx, my) || l.mapNext.contains(mx, my)))
        {
            // The next map in that direction that has a start position for everybody in the room
            const int n = static_cast<int>(m_maps.size());
            const int step = l.mapNext.contains(mx, my) ? 1 : -1;
            const size_t inRoom = lobby ? lobby->slots.size() : 1;
            int pick = -1;
            for(int k = 1; k <= n && pick < 0; ++k)
            {
                const int i = ((m_mapIndex + step * k) % n + n) % n;
                if(MissionController::startPositions(m_maps[i].path).size() >= inRoom)
                    pick = i;
            }
            if(pick < 0 || pick == m_mapIndex)
            {
                m_chatLog.push_back(_("No other map has a start position for everybody in the room."));
                if(m_chatLog.size() > 50)
                    m_chatLog.erase(m_chatLog.begin());
                return NetworkAction::None;
            }
            m_mapIndex = pick;
            net.setLobbyMap(m_maps[m_mapIndex].path);
            return NetworkAction::None;
        }
        // Computer players: the host / owner adds them and changes them like their own row
        if(net.controlsRoom() && lobby)
        {
            const int rows = static_cast<int>(lobby->slots.size());
            if(rows < std::max<int>(1, lobby->spawnCount) && l.addAi(rows).contains(mx, my))
            {
                Net::OwnerSetAi add;
                add.level = 1;
                net.setAi(add);
                return NetworkAction::None;
            }
            for(int i = 0; i < rows; ++i)
            {
                const Net::LobbySlot &s = lobby->slots[i];
                if(!s.ai)
                    continue;
                if(l.removeCell(i).contains(mx, my))
                {
                    Net::OwnerSetAi change;
                    change.slot = s.slot;
                    change.level = s.aiLevel;
                    change.choice = s.choice;
                    change.remove = true;
                    net.setAi(change);
                    return NetworkAction::None;
                }
                if(l.factionCell(i).contains(mx, my))
                    return openCombo(i * 10 + 1);
                if(l.teamCell(i).contains(mx, my))
                    return openCombo(i * 10 + 2);
                if(l.colorCell(i).contains(mx, my))
                    return openCombo(i * 10 + 3);
                if(l.nameCell(i).contains(mx, my))
                    return openCombo(i * 10 + 4);
            }
        }
        if(!haveMine)
            return NetworkAction::None;

        Net::LobbyChoice next = mine;
        if(l.readyBtn.contains(mx, my) && net.mapReady())
            next.ready = !mine.ready;
        if(myRow >= 0)
        {
            if(l.factionCell(myRow).contains(mx, my))
                return openCombo(myRow * 10 + 1);
            if(l.teamCell(myRow).contains(mx, my))
                return openCombo(myRow * 10 + 2);
            if(l.colorCell(myRow).contains(mx, my))
                return openCombo(myRow * 10 + 3);
        }
        // A start position on the map: take it (again: give it up)
        if(l.map.contains(mx, my))
        {
            for(size_t i = 0; i < m_spots.size(); ++i)
            {
                int sx = 0, sy = 0;
                if(!spotOnScreen(l.map, m_mapW, m_mapH, m_spots[i].first, m_spots[i].second, sx, sy))
                    continue;
                if(std::abs(mx - sx) <= 14 && std::abs(my - sy) <= 14)
                {
                    next.spawn = mine.spawn == i ? Net::kRandom : static_cast<uint8_t>(i);
                    next.ready = false;
                }
            }
        }
        if(next.faction != mine.faction || next.team != mine.team || next.color != mine.color || next.spawn != mine.spawn || next.ready != mine.ready)
        {
            net.setChoice(next);
        }
        return NetworkAction::None;
    }

    void NetworkMenu::render(Renderer &r, BitmapFontRenderer &font, TextureManager &textures, int w, int h, const NetMatch *net, const Settings &settings, const std::vector<LanGame> &lan, const std::vector<LanGame> &server, const std::string &serverStatus)
    {
        r.fillRectScreen(0, 0, w, h, col::Scrim);
        if(net && net->connected())
        {
            // ----- Room -----
            const LobbyLayout l = lobbyLayout(w, h);
            const Net::LobbyState *lobby = net->lobby();
            const Net::Roster *roster = net->roster();
            std::string title = net->controlsRoom() ? _("YOUR GAME") : _("GAME ROOM");
            if(!net->publicCode().empty())
                title += std::string("  -  ") + _("code") + " " + net->publicCode();
            const UiRect content = ui::drawDialog(r, font, w, h, l.dialog, title);
            (void) content;
            ui::drawCaption(font, r, l.players.x, l.players.y, _("PLAYERS"));
            auto nameOf = [&](uint8_t slot)
            {
                if(roster)
                {
                    for(const Net::RosterEntry &e : roster->players)
                        if(e.slot == slot)
                            return e.name;
                }
                return std::string("?");
            };
            int rowIndex = 0;
            if(lobby)
            {
                for(const Net::LobbySlot &s : lobby->slots)
                {
                    const UiRect row = l.row(rowIndex);
                    const bool me = s.slot == net->localSlot();
                    const bool editable = me || (s.ai && net->controlsRoom());
                    ui::drawPanel(r, row, me ? col::Hover : col::PanelRaised, me ? col::Accent : col::Border);
                    const std::string name = s.ai ? std::string(_("AI")) + " (" + aiLevelName(s.aiLevel) + ")" : nameOf(s.slot);
                    if(s.ai && net->controlsRoom())
                    {
                        // The level of a computer player: a drop-down list
                        const UiRect nc = l.nameCell(rowIndex);
                        ui::drawComboField(r, font, nc, name, m_combo.open == rowIndex * 10 + 4, nc.contains(m_mouseX, m_mouseY), true, col::Info);
                    }
                    else
                    {
                        font.drawString(ui::fitText(font, name, ui::FONT_BODY_BOLD, 150), row.x + 10, row.centerY(), ui::FONT_BODY_BOLD, s.ai ? col::Info : col::Text, false, Anchor::LEFT | Anchor::VCENTER);
                    }
                    if(s.ai && net->controlsRoom())
                    {
                        const UiRect x = l.removeCell(rowIndex);
                        ui::drawButton(r, font, x, "x", ui::ButtonStyle::Danger, x.contains(m_mouseX, m_mouseY));
                    }
                    auto cell = [&](const UiRect &c, const std::string &text)
                    {
                        const bool hot = editable && c.contains(m_mouseX, m_mouseY);
                        ui::drawPanel(r, c, hot ? col::Hover : col::PanelSunken, hot ? col::BorderStrong : col::Border);
                        font.drawString(text, c.centerX(), c.centerY(), ui::FONT_SMALL, col::Text, false, Anchor::HCENTER | Anchor::VCENTER);
                    };
                    const UiRect cc = l.colorCell(rowIndex);
                    if(editable)
                    {
                        // Drop-down lists: faction, team, colour
                        auto combo = [&](const UiRect &c, int kind, const std::string &text) { ui::drawComboField(r, font, c, text, m_combo.open == rowIndex * 10 + kind, c.contains(m_mouseX, m_mouseY)); };
                        combo(l.factionCell(rowIndex), 1, factionName(s.choice.faction));
                        combo(l.teamCell(rowIndex), 2, std::to_string(s.choice.team + 1));
                        combo(cc, 3, "");
                        r.fillRectScreen(cc.x + 5, cc.y + 6, 14, cc.h - 12, slotColor(s.choice.color));
                    }
                    else
                    {
                        cell(l.factionCell(rowIndex), factionName(s.choice.faction));
                        cell(l.teamCell(rowIndex), std::string(_("Team")) + " " + std::to_string(s.choice.team + 1));
                        r.fillRectScreen(cc.x + 6, cc.y + 6, cc.w - 12, cc.h - 12, slotColor(s.choice.color));
                    }
                    const std::string spawn = s.choice.spawn == Net::kRandom ? std::string("?") : std::to_string(s.choice.spawn + 1);
                    font.drawString(spawn, row.x + 430, row.centerY(), ui::FONT_BODY_BOLD, col::TextDim, false, Anchor::HCENTER | Anchor::VCENTER);
                    if(!s.ai)
                    {
                        font.drawString(s.choice.ready ? _("READY") : "", row.right() - 10, row.centerY(), ui::FONT_SMALL_BOLD, col::Ok, false, Anchor::RIGHT | Anchor::VCENTER);
                    }
                    ++rowIndex;
                }
                if(net->controlsRoom() && rowIndex < std::max<int>(1, lobby->spawnCount))
                {
                    const UiRect add = l.addAi(rowIndex);
                    ui::drawButton(r, font, add, _("+ AI PLAYER"), ui::ButtonStyle::Ghost, add.contains(m_mouseX, m_mouseY));
                }
            }

            // Map with its start positions
            // The map's own title (the list of maps is the same on every peer: the assets match)
            if(m_maps.empty())
                m_maps = MissionController::listSkirmishMaps();
            std::string mapName;
            if(lobby)
            {
                for(const SkirmishMapInfo &m : m_maps)
                    if(m.path == lobby->map)
                        mapName = m.displayName();
                if(mapName.empty() && !lobby->map.empty())
                    mapName = lobby->map.substr(lobby->map.find_last_of('/') + 1);
            }
            font.drawString(mapName, l.map.centerX(), l.mapPrev.centerY(), ui::FONT_BODY_BOLD, col::Text, false, Anchor::HCENTER | Anchor::VCENTER);
            if(net->controlsRoom())
            {
                ui::drawButton(r, font, l.mapPrev, "<", ui::ButtonStyle::Secondary, l.mapPrev.contains(m_mouseX, m_mouseY));
                ui::drawButton(r, font, l.mapNext, ">", ui::ButtonStyle::Secondary, l.mapNext.contains(m_mouseX, m_mouseY));
            }
            ui::drawPanel(r, l.map, col::PanelSunken, col::Border);
            if(lobby && !lobby->map.empty() && !net->mapReady())
            {
                font.drawString(_("Downloading the map..."), l.map.centerX(), l.map.centerY(), ui::FONT_BODY, col::TextDim, false, Anchor::HCENTER | Anchor::VCENTER);
            }
            if(lobby && net->mapReady())
            {
                int pw = 0, ph = 0;
                const std::string &file = net->lobbyMapFile();
                SDL_Texture *tex = m_preview.get(r.getSDLRenderer(), textures, file, file, {}, pw, ph);
                if(tex)
                {
                    m_mapW = pw;
                    m_mapH = ph;
                    const float scale = std::min(static_cast<float>(l.map.w) / pw, static_cast<float>(l.map.h) / ph);
                    const SDL_FRect dst {l.map.x + (l.map.w - pw * scale) / 2.0f, l.map.y + (l.map.h - ph * scale) / 2.0f, pw * scale, ph * scale};
                    SDL_RenderTexture(r.getSDLRenderer(), tex, nullptr, &dst);
                }
                for(size_t i = 0; i < m_spots.size() && i < lobby->spawnCount; ++i)
                {
                    int sx = 0, sy = 0;
                    if(!spotOnScreen(l.map, m_mapW, m_mapH, m_spots[i].first, m_spots[i].second, sx, sy))
                        continue;
                    SDL_Color c = col::TextDim;
                    for(const Net::LobbySlot &s : lobby->slots)
                    {
                        if(s.choice.spawn == i)
                            c = slotColor(s.choice.color);
                    }
                    const bool hot = std::abs(m_mouseX - sx) <= 14 && std::abs(m_mouseY - sy) <= 14;
                    r.fillRectScreen(sx - 12, sy - 12, 24, 24, col::PanelSunken);
                    r.fillRectScreen(sx - 10, sy - 10, 20, 20, c);
                    r.drawRectScreen(sx - 12, sy - 12, 24, 24, hot ? col::Accent : col::Text);
                    font.drawString(std::to_string(i + 1), sx, sy, ui::FONT_SMALL_BOLD, col::Backdrop, false, Anchor::HCENTER | Anchor::VCENTER);
                }
            }
            if(net->controlsRoom() && m_mapHasPrebuilt)
            {
                ui::drawComboField(r, font, l.prebuiltBtn, m_prebuilt ? _("PREBUILT BASES") : _("FROM SCRATCH"), m_combo.open == kLayoutCombo, l.prebuiltBtn.contains(m_mouseX, m_mouseY));
            }
            if(lobby && !lobby->map.empty())
            {
                // How many players the map is made for (its start positions)
                font.drawString(std::string(_("Players on this map: ")) + std::to_string(lobby->spawnCount), l.map.x + 8, l.map.bottom() - 18, ui::FONT_SMALL_BOLD, col::Accent, true);
            }
            font.drawTextWrapped(_("Click a number on the map to start there, again to leave it to chance. Click your faction, team and colour to change them."), l.map.x, l.map.bottom() + 10, l.map.w, ui::FONT_SMALL, col::TextDim, false);

            // Chat
            ui::drawPanel(r, l.chatLog, col::PanelSunken, col::Border);
            const int first = std::max(0, static_cast<int>(m_chatLog.size()) - kChatLines);
            int y = l.chatLog.y + 6;
            for(int i = first; i < static_cast<int>(m_chatLog.size()); ++i)
            {
                font.drawString(ui::fitText(font, m_chatLog[i], ui::FONT_SMALL, l.chatLog.w - 16), l.chatLog.x + 8, y, ui::FONT_SMALL, col::Text, false);
                y += 18;
            }
            drawField(r, font, l.chatField, m_chatInput, _("Enter: chat"), m_focus == Field::Chat);

            bool ready = false;
            if(lobby)
            {
                for(const Net::LobbySlot &s : lobby->slots)
                    if(s.slot == net->localSlot())
                        ready = s.choice.ready;
            }
            ui::drawButton(r, font, l.leaveBtn, _("LEAVE"), ui::ButtonStyle::Secondary, l.leaveBtn.contains(m_mouseX, m_mouseY));
            ui::drawButton(r, font, l.readyBtn, ready ? _("NOT READY") : _("READY"), ready ? ui::ButtonStyle::Secondary : ui::ButtonStyle::Primary, l.readyBtn.contains(m_mouseX, m_mouseY), net->mapReady());
            if(net->controlsRoom())
            {
                ui::drawButton(r, font, l.startBtn, _("START"), ui::ButtonStyle::Primary, l.startBtn.contains(m_mouseX, m_mouseY), net->canStart());
            }
            else
            {
                font.drawString(_("The host starts the game"), l.startBtn.centerX(), l.startBtn.centerY(), ui::FONT_SMALL, col::TextDim, false, Anchor::HCENTER | Anchor::VCENTER);
            }
            // The open drop-down list, over everything
            if(m_combo.open >= 0)
            {
                const int id = m_combo.open, row = id / 10, kind = id == kLayoutCombo ? 5 : id % 10;
                if(id == kLayoutCombo)
                {
                    ui::drawComboList(r, font, l.prebuiltBtn, comboChoices(5), m_prebuilt ? 0 : 1, m_combo, h, m_mouseX, m_mouseY);
                }
                else if(lobby && row < static_cast<int>(lobby->slots.size()))
                {
                    const Net::LobbySlot &s = lobby->slots[row];
                    const ui::UiRect field = kind == 1 ? l.factionCell(row) : kind == 2 ? l.teamCell(row) : kind == 3 ? l.colorCell(row) : l.nameCell(row);
                    const int current = kind == 1 ? (s.choice.faction == Net::kRandom ? 0 : s.choice.faction + 1) : kind == 2 ? s.choice.team : kind == 3 ? std::max(0, s.choice.color - 1) : s.aiLevel;
                    ui::drawComboList(r, font, field, comboChoices(kind), current, m_combo, h, m_mouseX, m_mouseY);
                }
            }
            ui::drawConfirm(r, font, w, h, m_confirm, m_mouseX, m_mouseY);
            return;
        }

        // ----- Browser / connecting -----
        const BrowserLayout l = browserLayout(w, h);
        if(net)
        {
            // Connecting: the loading screen with what the connection is doing, and a way out
            ui::drawLoadingScreen(r, font, w, h, _("NETWORK GAME"), std::string(_("Connecting...")) + " (" + net->statusText() + ")", -1.0f, SDL_GetTicks());
            ui::drawButton(r, font, l.backBtn, _("CANCEL"), ui::ButtonStyle::Secondary, l.backBtn.contains(m_mouseX, m_mouseY));
            return;
        }
        ui::drawDialog(r, font, w, h, l.dialog, _("NETWORK GAME"));
        font.drawString(_("Your name"), l.dialog.x + 24, l.nameField.centerY(), ui::FONT_BODY_BOLD, col::Text, false, Anchor::LEFT | Anchor::VCENTER);
        drawField(r, font, l.nameField, settings.playerName, "", m_focus == Field::Name);
        font.drawString(_("Password"), l.nameField.right() + 16, l.nameField.centerY(), ui::FONT_BODY_BOLD, col::Text, false, Anchor::LEFT | Anchor::VCENTER);
        drawField(r, font, l.passwordField, std::string(m_password.size(), '*'), _("optional"), m_focus == Field::Password);
        ui::drawButton(r, font, l.tabLan, _("LOCAL NETWORK"), m_tab == Tab::Lan ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary, l.tabLan.contains(m_mouseX, m_mouseY));
        ui::drawButton(r, font, l.tabInternet, _("INTERNET"), m_tab == Tab::Internet ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary, l.tabInternet.contains(m_mouseX, m_mouseY));
        if(m_tab == Tab::Lan)
        {
            ui::drawPanel(r, l.list, col::PanelSunken, col::Border);
            if(lan.empty())
            {
                font.drawString(_("Looking for games on the local network..."), l.list.centerX(), l.list.centerY(), ui::FONT_BODY, col::TextDim, false, Anchor::HCENTER | Anchor::VCENTER);
            }
            for(size_t i = 0; i < lan.size() && i < 8; ++i)
            {
                const UiRect row {l.list.x + 4, l.list.y + 4 + static_cast<int>(i) * 40, l.list.w - 8, 36};
                const bool sel = static_cast<int>(i) == m_lanSelected;
                ui::drawPanel(r, row, sel ? col::Hover : col::PanelRaised, sel ? col::Accent : col::Border);
                const LanGame &g = lan[i];
                font.drawString(ui::fitText(font, g.name, ui::FONT_BODY_BOLD, 220), row.x + 10, row.centerY(), ui::FONT_BODY_BOLD, col::Text, false, Anchor::LEFT | Anchor::VCENTER);
                font.drawString(ui::fitText(font, g.map, ui::FONT_SMALL, 160), row.x + 250, row.centerY(), ui::FONT_SMALL, col::TextDim, false, Anchor::LEFT | Anchor::VCENTER);
                font.drawString(std::to_string(g.players) + "/" + std::to_string(g.maxPlayers), row.x + 440, row.centerY(), ui::FONT_SMALL, col::Text, false, Anchor::LEFT | Anchor::VCENTER);
                font.drawString(std::to_string(g.pingMs) + " ms", row.right() - 10, row.centerY(), ui::FONT_SMALL, g.pingMs < 60 ? col::Ok : col::Accent, false, Anchor::RIGHT | Anchor::VCENTER);
                if(g.locked)
                    drawLock(r, row.x + 412, row.centerY() - 7, col::Accent);
            }
        }
        else
        {
            font.drawString(_("Direct connection: host or host:port (port 38100 by default; the host forwards UDP 38100)"), l.addressField.x, l.addressField.y - 18, ui::FONT_SMALL, col::TextDim, false);
            drawField(r, font, l.addressField, settings.lastAddress, "192.168.1.10 / example.org:38100", m_focus == Field::Address);
            ui::drawButton(r, font, l.directBtn, _("CONNECT"), ui::ButtonStyle::Secondary, l.directBtn.contains(m_mouseX, m_mouseY), !settings.lastAddress.empty());
            ui::drawCaption(font, r, l.serverList.x, l.refreshBtn.y + 6, std::string(_("SERVER")) + "  " + serverStatus);
            ui::drawButton(r, font, l.refreshBtn, _("REFRESH"), ui::ButtonStyle::Ghost, l.refreshBtn.contains(m_mouseX, m_mouseY));
            ui::drawPanel(r, l.serverList, col::PanelSunken, col::Border);
            if(server.empty())
            {
                font.drawString(_("No open rooms: create one, or join by code"), l.serverList.centerX(), l.serverList.centerY(), ui::FONT_SMALL, col::TextDim, false, Anchor::HCENTER | Anchor::VCENTER);
            }
            for(int i = 0; i < static_cast<int>(server.size()) && i < 4; ++i)
            {
                const UiRect row = l.serverRow(i);
                const bool sel = i == m_serverSelected;
                const LanGame &g = server[i];
                ui::drawPanel(r, row, sel ? col::Hover : col::PanelRaised, sel ? col::Accent : col::Border);
                font.drawString(g.code, row.x + 10, row.centerY(), ui::FONT_SMALL_BOLD, g.port == 0 ? col::Info : col::Accent, false, Anchor::LEFT | Anchor::VCENTER);
                font.drawString(ui::fitText(font, g.name, ui::FONT_BODY_BOLD, 200), row.x + 90, row.centerY(), ui::FONT_BODY_BOLD, col::Text, false, Anchor::LEFT | Anchor::VCENTER);
                font.drawString(ui::fitText(font, g.map, ui::FONT_SMALL, 160), row.x + 310, row.centerY(), ui::FONT_SMALL, col::TextDim, false, Anchor::LEFT | Anchor::VCENTER);
                font.drawString(std::to_string(g.players) + "/" + std::to_string(g.maxPlayers), row.right() - 10, row.centerY(), ui::FONT_SMALL, col::Text, false, Anchor::RIGHT | Anchor::VCENTER);
                if(g.locked)
                    drawLock(r, row.right() - 56, row.centerY() - 7, col::Accent);
            }
            font.drawString(_("Room code"), l.serverList.x, l.codeField.centerY(), ui::FONT_BODY_BOLD, col::Text, false, Anchor::LEFT | Anchor::VCENTER);
            drawField(r, font, l.codeField, m_joinCode, "ABC123", m_focus == Field::Code);
            ui::drawButton(r, font, l.codeBtn, _("JOIN BY CODE"), ui::ButtonStyle::Secondary, l.codeBtn.contains(m_mouseX, m_mouseY), m_joinCode.size() == 6);
        }
        if(!m_notice.empty())
        {
            font.drawString(m_notice, l.dialog.centerX(), l.joinBtn.y - 18, ui::FONT_SMALL_BOLD, col::Danger, false, Anchor::HCENTER | Anchor::VCENTER);
        }
        ui::drawButton(r, font, l.backBtn, _("BACK"), ui::ButtonStyle::Secondary, l.backBtn.contains(m_mouseX, m_mouseY));
        ui::drawButton(r, font, l.hostBtn, _("CREATE GAME"), ui::ButtonStyle::Secondary, l.hostBtn.contains(m_mouseX, m_mouseY));
        if(m_tab == Tab::Internet)
        {
            ui::drawButton(r, font, l.serverBtn, _("CREATE ON SERVER"), ui::ButtonStyle::Secondary, l.serverBtn.contains(m_mouseX, m_mouseY));
        }
        const bool canJoin = m_tab == Tab::Internet ? (m_serverSelected >= 0 && m_serverSelected < static_cast<int>(server.size())) : (m_lanSelected >= 0 && m_lanSelected < static_cast<int>(lan.size()));
        ui::drawButton(r, font, l.joinBtn, _("JOIN"), ui::ButtonStyle::Primary, l.joinBtn.contains(m_mouseX, m_mouseY), canJoin);
    }

} // namespace OpenArtOfWar
