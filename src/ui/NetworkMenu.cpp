#include "ui/NetworkMenu.hpp"
#include "audio/SoundRequests.hpp"
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

std::string AiLevelName(uint8_t level)
{
    static const char *const kNames[4] = {"Easy", "Medium", "Hard", "Hardcore"};
    return _(kNames[level & 3]);
}

std::string FactionName(uint8_t f)
{
    if (f == 0)
        return _("Resistance");
    if (f == 1)
        return _("Confederation");
    return _("Random");
}

// The choices of the drop-down lists of the room: kind 1 faction, 2 team, 3 colour, 4 level of a computer player,
// 5 how the map starts
std::vector<std::string> ComboChoices(int kind)
{
    std::vector<std::string> out;
    if (kind == 1)
        out = {_("Random"), _("Resistance"), _("Confederation")};
    else if (kind == 2)
        for (int i = 1; i <= kMaxPlayers; ++i)
            out.push_back(std::string(_("Team")) + " " + std::to_string(i));
    else if (kind == 3)
        for (int i = 1; i <= Settings::kArmyColors; ++i)
            out.push_back(_(ArmyColorName(i)));
    else if (kind == 4)
        for (int i = 0; i < 4; ++i)
            out.push_back(AiLevelName(static_cast<uint8_t>(i)));
    else
        out = {_("PREBUILT BASES"), _("FROM SCRATCH")};
    return out;
}

// Team colours (index 0 = colour 1) other players of the room already have: nobody may pick them again
std::vector<bool> TakenColors(const Net::LobbyState &lobby, uint8_t slot)
{
    std::vector<bool> taken(Settings::kArmyColors, false);
    for (const Net::LobbySlot &o : lobby.slots)
    {
        if (o.slot != slot && o.choice.color >= 1 && o.choice.color <= Settings::kArmyColors)
            taken[o.choice.color - 1] = true;
    }
    return taken;
}

SDL_Color SlotColor(uint8_t color)
{
    return color == 0 ? col::TextDim : ArmyColor(color);
}

// Removes the last UTF-8 character
void PopUtf8(std::string &s)
{
    while (!s.empty())
    {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80)
            break;
    }
}

// A small padlock: the room asks for a password
void DrawLock(Renderer &r, int x, int y, SDL_Color color)
{
    r.DrawRectScreen(x + 2, y, 7, 6, color);
    r.FillRectScreen(x, y + 5, 11, 8, color);
}

void DrawField(
    Renderer &r,
    BitmapFontRenderer &font,
    const UiRect &rect,
    const std::string &text,
    const std::string &placeholder,
    bool focused)
{
    ui::DrawPanel(r, rect, col::PanelSunken, focused ? col::Accent : col::Border);
    const bool empty = text.empty();
    std::string shown = ui::FitText(font, empty ? placeholder : text, ui::FONT_BODY, rect.w - 24);
    if (focused)
        shown += (SDL_GetTicks() / 500) % 2 ? "_" : " ";
    font.DrawString(
        shown,
        rect.x + 10,
        rect.CenterY(),
        ui::FONT_BODY,
        empty && !focused ? col::TextMuted : col::Text,
        false,
        Anchor::LEFT | Anchor::VCENTER);
}

} // namespace

void NetworkMenu::Reset()
{
    m_focus = Field::None;
    m_combo = ui::ComboState {};
    m_confirm.open = false;
    m_chatLog.clear();
    m_chatInput.clear();
    m_spotsMap.clear();
    m_preview.Clear();
    m_wasInLobby = false;
}

NetworkMenu::BrowserLayout NetworkMenu::browserLayout(int w, int h)
{
    BrowserLayout l;
    l.dialog = ui::CenteredRect(w, h, 760, 560);
    const int x = l.dialog.x + 24, inner = l.dialog.w - 48;
    l.nameField = {x + 140, l.dialog.y + 70, 220, 36};
    l.passwordField = {x + 540, l.dialog.y + 70, inner - 540, 36};
    l.tabLan = {x, l.dialog.y + 126, inner / 2 - 3, 36};
    l.tabInternet = {x + inner / 2 + 3, l.dialog.y + 126, inner / 2 - 3, 36};
    l.list = {x, l.dialog.y + 172, inner, l.dialog.h - 172 - 80};
    // Internet tab: direct address, then the server's rooms, then a room code
    l.addressField = {x, l.list.y + 22, inner - 180, 36};
    l.directBtn = {l.addressField.Right() + 10, l.addressField.y, 170, 36};
    l.refreshBtn = {l.list.Right() - 140, l.addressField.Bottom() + 14, 140, 28};
    l.serverList = {x, l.refreshBtn.Bottom() + 6, inner, 4 * 34 + 8};
    l.codeField = {x + 150, l.serverList.Bottom() + 10, 130, 34};
    l.codeBtn = {l.codeField.Right() + 10, l.codeField.y, 170, 34};
    // Bottom row: Back on the left, the three actions share the rest; a narrow window shrinks them all, so they
    // never overlap
    constexpr int kGap = 12;
    const int backW = std::min(120, inner / 6);
    const int actionW = std::min(210, (inner - backW - 3 * kGap) / 3);
    l.joinBtn = {l.dialog.Right() - 24 - actionW, l.dialog.Bottom() - 60, actionW, 40};
    l.hostBtn = {l.joinBtn.x - kGap - actionW, l.joinBtn.y, actionW, 40};
    l.serverBtn = {l.hostBtn.x - kGap - actionW, l.joinBtn.y, actionW, 40};
    l.backBtn = {x, l.joinBtn.y, backW, 40};
    return l;
}

NetworkMenu::LobbyLayout NetworkMenu::lobbyLayout(int w, int h)
{
    LobbyLayout l;
    l.dialog = ui::CenteredRect(w, h, 1000, 640);
    const int x = l.dialog.x + 20, y = l.dialog.y + 60;
    l.players = {x, y, 520, 30 + 8 * 40};
    l.map = {x + 540, y + 30, l.dialog.Right() - 20 - (x + 540), 300};
    l.mapPrev = {l.map.x, y - 4, 32, 26};
    l.mapNext = {l.map.Right() - 32, y - 4, 32, 26};
    l.mapName = {l.mapPrev.Right() + 4, y - 4, l.mapNext.x - 4 - (l.mapPrev.Right() + 4), 26};
    l.chatLog = {x, l.players.Bottom() + 10, 520, l.dialog.Bottom() - 70 - (l.players.Bottom() + 10) - 44};
    l.chatField = {x, l.chatLog.Bottom() + 6, 520 - 52, 36};
    l.chatSend = {l.chatField.Right() + 8, l.chatField.y, 44, 36};
    l.prebuiltBtn = {l.map.Right() - 8 - 190, l.map.Bottom() - 8 - 28, 190, 28};
    // Bottom row: Leave on the left (under the chat), Ready and Start on the right - never on top of each other
    l.leaveBtn = {x, l.dialog.Bottom() - 60, 140, 40};
    l.startBtn = {l.dialog.Right() - 20 - 150, l.leaveBtn.y, 150, 40};
    l.readyBtn = {l.startBtn.x - 12 - 150, l.leaveBtn.y, 150, 40};
    return l;
}

void NetworkMenu::EditText(InputManager &input, std::string &text, size_t maxLength)
{
    for (int i = input.TakeBackspaces(); i > 0; --i)
        PopUtf8(text);
    text += input.TakeTypedText();
    while (text.size() > maxLength)
        PopUtf8(text);
}

bool NetworkMenu::ParseAddress(const std::string &text)
{
    std::string a = text;
    a.erase(std::remove(a.begin(), a.end(), ' '), a.end());
    if (a.empty())
        return false;
    m_joinPort = kDefaultPort;
    const size_t colon = a.rfind(':');
    if (colon != std::string::npos && a.find(':') == colon)
    { // one colon: host:port (IPv6 is not supported)
        const std::string port = a.substr(colon + 1);
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), ::isdigit))
            return false;
        const int p = std::stoi(port);
        if (p <= 0 || p > 65535)
            return false;
        m_joinPort = static_cast<uint16_t>(p);
        a.resize(colon);
    }
    m_joinAddress = a;
    return !a.empty();
}

NetworkAction NetworkMenu::Update(
    InputManager &input,
    int windowW,
    int windowH,
    NetMatch *net,
    Settings &settings,
    const std::vector<LanGame> &lan,
    const std::vector<LanGame> &server)
{
    m_mouseX = input.GetMouseScreenX();
    m_mouseY = input.GetMouseScreenY();
    const bool inLobby = net && net->connected();
    if (inLobby && !m_wasInLobby)
        SoundRequests::Play("ui_show_dialog");
    m_wasInLobby = inLobby;

    if (inLobby)
        return UpdateLobby(input, windowW, windowH, *net);
    if (net)
    {
        // Connecting: only cancel
        float lx, ly;
        const BrowserLayout l = browserLayout(windowW, windowH);
        if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE)
            || (input.WasLeftClickIssued(lx, ly) && l.backBtn.Contains(m_mouseX, m_mouseY)))
        {
            return NetworkAction::Leave;
        }
        return NetworkAction::None;
    }
    return UpdateBrowser(input, windowW, windowH, settings, lan, server);
}

NetworkAction NetworkMenu::UpdateBrowser(
    InputManager &input,
    int w,
    int h,
    Settings &settings,
    const std::vector<LanGame> &lan,
    const std::vector<LanGame> &server)
{
    const BrowserLayout l = browserLayout(w, h);
    float lx, ly;
    const bool clicked = input.WasLeftClickIssued(lx, ly);
    const int mx = m_mouseX, my = m_mouseY;

    if (m_focus == Field::Name)
        EditText(input, settings.playerName, 24);
    if (m_focus == Field::Password)
        EditText(input, m_password, 24);
    if (m_focus == Field::Address)
        EditText(input, settings.lastAddress, 120);
    if (m_focus == Field::Code)
    {
        EditText(input, m_joinCode, 6);
        for (char &c : m_joinCode)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    auto joinCode = [&]() -> NetworkAction {
        if (m_joinCode.size() != 6)
        {
            m_notice = _("A room code has six characters");
            return NetworkAction::None;
        }
        m_joinPort = 0;
        return NetworkAction::ServerJoin;
    };
    auto joinDirect = [&]() -> NetworkAction {
        if (!ParseAddress(settings.lastAddress))
        {
            m_notice = _("Enter an address: host or host:port");
            return NetworkAction::None;
        }
        return NetworkAction::Join;
    };
    auto join = [&]() -> NetworkAction {
        if (m_tab == Tab::Internet && m_serverSelected >= 0 && m_serverSelected < static_cast<int>(server.size()))
        {
            m_joinCode = server[m_serverSelected].code;
            m_joinPort = server[m_serverSelected].port;
            return NetworkAction::ServerJoin;
        }
        if (m_tab == Tab::Lan)
        {
            if (m_lanSelected < 0 || m_lanSelected >= static_cast<int>(lan.size()))
                return NetworkAction::None;
            m_joinAddress = lan[m_lanSelected].address;
            m_joinPort = lan[m_lanSelected].port;
            return NetworkAction::Join;
        }
        if (!ParseAddress(settings.lastAddress))
        {
            m_notice = _("Enter an address: host or host:port");
            return NetworkAction::None;
        }
        return NetworkAction::Join;
    };

    if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        if (m_focus != Field::None)
            m_focus = Field::None;
        else
            return NetworkAction::Back;
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_RETURN) || input.IsKeyJustPressed(SDL_SCANCODE_KP_ENTER))
    {
        if (m_focus == Field::Address)
            return joinDirect();
        if (m_focus == Field::Code)
            return joinCode();
        m_focus = Field::None;
    }
    if (!clicked)
        return NetworkAction::None;

    m_focus = Field::None;
    if (l.nameField.Contains(mx, my))
        m_focus = Field::Name;
    else if (l.passwordField.Contains(mx, my))
        m_focus = Field::Password;
    if (l.tabLan.Contains(mx, my))
        m_tab = Tab::Lan;
    if (l.tabInternet.Contains(mx, my))
        m_tab = Tab::Internet;
    if (m_tab == Tab::Internet)
    {
        if (l.addressField.Contains(mx, my))
            m_focus = Field::Address;
        if (l.codeField.Contains(mx, my))
            m_focus = Field::Code;
        if (l.directBtn.Contains(mx, my) && !settings.lastAddress.empty())
        {
            m_notice.clear();
            return joinDirect();
        }
        if (l.codeBtn.Contains(mx, my))
            return joinCode();
        if (l.refreshBtn.Contains(mx, my))
            return NetworkAction::ServerRefresh;
        for (int i = 0; i < static_cast<int>(server.size()) && i < 4; ++i)
        {
            if (!l.ServerRow(i).Contains(mx, my))
                continue;
            if (i == m_serverSelected)
                return join(); // a second click joins
            m_serverSelected = i;
        }
    }
    if (m_tab == Tab::Internet && l.serverBtn.Contains(mx, my))
    {
        m_notice.clear();
        return NetworkAction::ServerCreate;
    }
    if (m_tab == Tab::Lan && l.list.Contains(mx, my))
    {
        const int row = (my - l.list.y - 4) / 40;
        if (row >= 0 && row < static_cast<int>(lan.size()))
        {
            if (row == m_lanSelected)
                return join(); // a second click joins
            m_lanSelected = row;
        }
    }
    if (l.backBtn.Contains(mx, my))
        return NetworkAction::Back;
    if (l.hostBtn.Contains(mx, my))
    {
        m_notice.clear();
        return NetworkAction::Host;
    }
    if (l.joinBtn.Contains(mx, my))
    {
        // Join is the listed game picked above; a direct address has its own Connect button
        const bool picked = m_tab == Tab::Internet
                                ? (m_serverSelected >= 0 && m_serverSelected < static_cast<int>(server.size()))
                                : (m_lanSelected >= 0 && m_lanSelected < static_cast<int>(lan.size()));
        if (!picked)
        {
            m_notice = _("Pick a game in the list first");
            return NetworkAction::None;
        }
        m_notice.clear();
        return join();
    }
    return NetworkAction::None;
}

bool NetworkMenu::SpotOnScreen(const UiRect &area, int mapW, int mapH, int tileX, int tileY, int &sx, int &sy) const
{
    if (mapW <= 0 || mapH <= 0)
        return false;
    const float scale = std::min(static_cast<float>(area.w) / mapW, static_cast<float>(area.h) / mapH);
    const int ox = area.x + (area.w - static_cast<int>(mapW * scale)) / 2;
    const int oy = area.y + (area.h - static_cast<int>(mapH * scale)) / 2;
    // The base is set around the spot: show its middle
    sx = ox + static_cast<int>((tileX + 5) * scale);
    sy = oy + static_cast<int>((tileY + 5) * scale);
    return true;
}

NetworkAction NetworkMenu::UpdateLobby(InputManager &input, int w, int h, NetMatch &net)
{
    const LobbyLayout l = lobbyLayout(w, h);
    const Net::LobbyState *lobby = net.Lobby();
    float lx, ly;
    const bool clicked = input.WasLeftClickIssued(lx, ly);
    const int mx = m_mouseX, my = m_mouseY;

    for (Net::Chat &c : net.TakeChat())
    {
        std::string who = "?";
        if (const Net::Roster *roster = net.roster())
        {
            for (const Net::RosterEntry &e : roster->players)
            {
                if (e.slot == c.fromSlot)
                    who = e.name;
            }
        }
        m_chatLog.push_back(who + ": " + c.text);
        if (m_chatLog.size() > 50)
            m_chatLog.erase(m_chatLog.begin());
    }

    // The host picks the first map when the room opens
    if (net.ControlsRoom() && lobby && lobby->map.empty())
    {
        if (m_maps.empty())
            m_maps = MissionController::ListSkirmishMaps();
        if (!m_maps.empty())
            net.SetLobbyMap(m_maps[std::clamp(m_mapIndex, 0, static_cast<int>(m_maps.size()) - 1)].path);
    }
    // Start positions from the room's map as this computer has it (the own file or the downloaded copy)
    if (net.LobbyMapFile() != m_spotsMap)
    {
        m_spotsMap = net.LobbyMapFile();
        m_spots
            = m_spotsMap.empty() ? std::vector<std::pair<int, int>> {} : MissionController::StartPositions(m_spotsMap);
    }

    // Own picks
    Net::LobbyChoice mine;
    bool haveMine = false;
    int myRow = -1;
    if (lobby)
    {
        for (size_t i = 0; i < lobby->slots.size(); ++i)
        {
            if (lobby->slots[i].slot == net.LocalSlot())
            {
                mine = lobby->slots[i].choice;
                net.LocalChoice(mine); // what was last sent, not the room's lagging echo
                haveMine = true;
                myRow = static_cast<int>(i);
            }
        }
    }

    // The question "leave the room?" takes everything while it is open
    if (m_confirm.open)
    {
        const int answer = ui::ConfirmUpdate(
            m_confirm,
            w,
            h,
            mx,
            my,
            clicked,
            input.IsKeyJustPressed(SDL_SCANCODE_RETURN) || input.IsKeyJustPressed(SDL_SCANCODE_KP_ENTER),
            input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE));
        if (answer < 0)
            return NetworkAction::None;
        m_confirm.open = false;
        return answer == 1 ? NetworkAction::Leave : NetworkAction::None;
    }
    auto askLeave = [&]() {
        m_confirm.open = true;
        SoundRequests::Play("ui_show_dialog");
        m_confirm.title = _("LEAVE THE ROOM?");
        m_confirm.text = net.ControlsRoom()
                             ? _("You are the room owner. If you leave, the game may end for everybody in it.")
                             : _("You leave the room. You can join it again while it is open.");
    };
    // An open drop-down list takes the mouse first; a pick changes the value, a click elsewhere closes the list
    auto comboField = [&](int id) -> ui::UiRect {
        if (id == kLayoutCombo)
            return l.prebuiltBtn;
        if (id == kMapCombo)
            return l.mapName;
        const int row = id / 10, kind = id % 10;
        return kind == 1   ? l.FactionCell(row)
               : kind == 2 ? l.TeamCell(row)
               : kind == 3 ? l.ColorCell(row)
                           : l.NameCell(row);
    };
    if (m_combo.open >= 0)
    {
        const int id = m_combo.open, row = id / 10, kind = id == kLayoutCombo ? 5 : id % 10;
        const bool isMapList = id == kMapCombo;
        const bool valid = id == kLayoutCombo || isMapList ? net.ControlsRoom()
                                                           : (lobby && row < static_cast<int>(lobby->slots.size()));
        if (!valid)
        {
            m_combo.open = -1;
        }
        else
        {
            int picked = -1;
            if (ui::ComboUpdate(
                    m_combo,
                    comboField(id),
                    isMapList ? static_cast<int>(m_maps.size()) : static_cast<int>(ComboChoices(kind).size()),
                    h,
                    mx,
                    my,
                    clicked,
                    input.ConsumeWheel(),
                    picked))
            {
                if (picked >= 0 && isMapList)
                {
                    // Only a map with a start position for everybody in the room
                    const size_t inRoom = lobby ? lobby->slots.size() : 1;
                    if (picked < static_cast<int>(m_maps.size())
                        && MissionController::StartPositions(m_maps[picked].path).size() >= inRoom)
                    {
                        m_mapIndex = picked;
                        net.SetLobbyMap(m_maps[picked].path);
                    }
                    else
                    {
                        m_chatLog.push_back(_("This map has no start position for everybody in the room."));
                        if (m_chatLog.size() > 50)
                            m_chatLog.erase(m_chatLog.begin());
                    }
                }
                else if (picked >= 0 && id == kLayoutCombo)
                {
                    m_prebuilt = picked == 0;
                }
                else if (picked >= 0)
                {
                    const Net::LobbySlot &s = lobby->slots[row];
                    Net::LobbyChoice choice = s.choice;
                    if (!s.ai && s.slot == net.LocalSlot())
                        net.LocalChoice(choice); // edit what was last sent, not the room's lagging echo
                    uint8_t level = s.aiLevel;
                    if (kind == 1)
                    {
                        choice.faction = picked == 0 ? Net::kRandom : static_cast<uint8_t>(picked - 1);
                        choice.ready = false;
                    }
                    if (kind == 2)
                    {
                        choice.team = static_cast<uint8_t>(picked);
                        // Teams are the host's decision: a player's team goes through the room directly
                        if (!s.ai)
                        {
                            if (net.ControlsRoom())
                                net.SetTeam(s.slot, choice.team);
                            return NetworkAction::None;
                        }
                    }
                    if (kind == 3 && TakenColors(*lobby, s.slot)[static_cast<size_t>(picked)]
                        && picked + 1 != choice.color)
                        return NetworkAction::None; // somebody else's colour: nothing changes
                    if (kind == 3)
                    {
                        choice.color = static_cast<uint8_t>(picked + 1);
                        choice.ready = false; // like faction and team: the owner never starts with an old colour
                    }
                    if (kind == 4)
                        level = static_cast<uint8_t>(picked);
                    if (s.ai && net.ControlsRoom())
                    {
                        Net::OwnerSetAi change;
                        change.slot = s.slot;
                        change.level = level;
                        change.choice = choice;
                        net.SetAi(change);
                    }
                    else if (!s.ai && s.slot == net.LocalSlot())
                    {
                        net.SetChoice(choice);
                    }
                }
                return NetworkAction::None;
            }
            if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
            {
                m_combo.open = -1;
                return NetworkAction::None;
            }
        }
    }
    auto openCombo = [&](int id) {
        m_combo.open = id;
        m_combo.scroll = 0;
        return NetworkAction::None;
    };

    if (m_focus == Field::Chat)
        EditText(input, m_chatInput, Net::kMaxChatLength);
    auto sendChat = [&]() {
        if (m_chatInput.empty())
            return;
        net.SendChat(m_chatInput);
        m_chatInput.clear();
    };
    if (input.IsKeyJustPressed(SDL_SCANCODE_RETURN) || input.IsKeyJustPressed(SDL_SCANCODE_KP_ENTER))
    {
        if (m_focus == Field::Chat && !m_chatInput.empty())
            sendChat();
        else
            m_focus = Field::Chat;
    }
    if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        if (m_focus != Field::None)
            m_focus = Field::None;
        else
        {
            askLeave();
            return NetworkAction::None;
        }
    }
    if (!clicked)
        return NetworkAction::None;

    m_focus = l.chatField.Contains(mx, my) ? Field::Chat : Field::None;
    if (l.chatSend.Contains(mx, my))
    {
        sendChat();
        m_focus = Field::Chat; // keep typing
        return NetworkAction::None;
    }
    if (l.leaveBtn.Contains(mx, my))
    {
        askLeave();
        return NetworkAction::None;
    }
    if (l.startBtn.Contains(mx, my) && net.ControlsRoom() && net.CanStart())
        return NetworkAction::Start;
    if (net.ControlsRoom() && l.prebuiltBtn.Contains(mx, my))
        return openCombo(kLayoutCombo);
    if (net.ControlsRoom() && !m_maps.empty() && l.mapName.Contains(mx, my))
        return openCombo(kMapCombo);
    if (net.ControlsRoom() && !m_maps.empty() && (l.mapPrev.Contains(mx, my) || l.mapNext.Contains(mx, my)))
    {
        // The next map in that direction that has a start position for everybody in the room
        const int n = static_cast<int>(m_maps.size());
        const int step = l.mapNext.Contains(mx, my) ? 1 : -1;
        const size_t inRoom = lobby ? lobby->slots.size() : 1;
        int pick = -1;
        for (int k = 1; k <= n && pick < 0; ++k)
        {
            const int i = ((m_mapIndex + step * k) % n + n) % n;
            if (MissionController::StartPositions(m_maps[i].path).size() >= inRoom)
                pick = i;
        }
        if (pick < 0 || pick == m_mapIndex)
        {
            m_chatLog.push_back(_("No other map has a start position for everybody in the room."));
            if (m_chatLog.size() > 50)
                m_chatLog.erase(m_chatLog.begin());
            return NetworkAction::None;
        }
        m_mapIndex = pick;
        net.SetLobbyMap(m_maps[m_mapIndex].path);
        return NetworkAction::None;
    }
    // Computer players: the host / owner adds them and changes them like their own row
    if (net.ControlsRoom() && lobby)
    {
        const int rows = static_cast<int>(lobby->slots.size());
        if (rows < std::max<int>(1, lobby->spawnCount) && l.AddAi(rows).Contains(mx, my))
        {
            Net::OwnerSetAi add;
            add.level = 1;
            net.SetAi(add);
            return NetworkAction::None;
        }
        for (int i = 0; i < rows; ++i)
        {
            const Net::LobbySlot &s = lobby->slots[i];
            // The host / owner sets every player's team
            if (l.TeamCell(i).Contains(mx, my))
                return openCombo(i * 10 + 2);
            if (!s.ai)
                continue;
            if (l.RemoveCell(i).Contains(mx, my))
            {
                Net::OwnerSetAi change;
                change.slot = s.slot;
                change.level = s.aiLevel;
                change.choice = s.choice;
                change.remove = true;
                net.SetAi(change);
                return NetworkAction::None;
            }
            if (l.FactionCell(i).Contains(mx, my))
                return openCombo(i * 10 + 1);
            if (l.TeamCell(i).Contains(mx, my))
                return openCombo(i * 10 + 2);
            if (l.ColorCell(i).Contains(mx, my))
                return openCombo(i * 10 + 3);
            if (l.NameCell(i).Contains(mx, my))
                return openCombo(i * 10 + 4);
        }
    }
    if (!haveMine)
        return NetworkAction::None;

    Net::LobbyChoice next = mine;
    if (l.readyBtn.Contains(mx, my) && net.MapReady())
        next.ready = !mine.ready;
    if (myRow >= 0)
    {
        if (l.FactionCell(myRow).Contains(mx, my))
            return openCombo(myRow * 10 + 1);
        if (l.TeamCell(myRow).Contains(mx, my) && net.ControlsRoom())
            return openCombo(myRow * 10 + 2);
        if (l.ColorCell(myRow).Contains(mx, my))
            return openCombo(myRow * 10 + 3);
    }
    // A start position on the map: take it (again: give it up)
    if (l.map.Contains(mx, my))
    {
        for (size_t i = 0; i < m_spots.size(); ++i)
        {
            int sx = 0, sy = 0;
            if (!SpotOnScreen(l.map, m_mapW, m_mapH, m_spots[i].first, m_spots[i].second, sx, sy))
                continue;
            if (std::abs(mx - sx) <= 14 && std::abs(my - sy) <= 14)
            {
                next.spawn = mine.spawn == i ? Net::kRandom : static_cast<uint8_t>(i);
                next.ready = false;
            }
        }
    }
    if (next.faction != mine.faction || next.team != mine.team || next.color != mine.color || next.spawn != mine.spawn
        || next.ready != mine.ready)
    {
        net.SetChoice(next);
    }
    return NetworkAction::None;
}

void NetworkMenu::render(
    Renderer &r,
    BitmapFontRenderer &font,
    TextureManager &textures,
    int w,
    int h,
    const NetMatch *net,
    const Settings &settings,
    const std::vector<LanGame> &lan,
    const std::vector<LanGame> &server,
    const std::string &serverStatus)
{
    r.FillRectScreen(0, 0, w, h, col::Scrim);
    if (net && net->connected())
    {
        // ----- Room -----
        const LobbyLayout l = lobbyLayout(w, h);
        const Net::LobbyState *lobby = net->Lobby();
        const Net::Roster *roster = net->roster();
        std::string title = net->ControlsRoom() ? _("YOUR GAME") : _("GAME ROOM");
        if (!net->PublicCode().empty())
            title += std::string("  -  ") + _("code") + " " + net->PublicCode();
        const UiRect content = ui::DrawDialog(r, font, w, h, l.dialog, title);
        (void) content;
        ui::DrawCaption(font, r, l.players.x, l.players.y, _("PLAYERS"));
        auto nameOf = [&](uint8_t slot) {
            if (roster)
            {
                for (const Net::RosterEntry &e : roster->players)
                    if (e.slot == slot)
                        return e.name;
            }
            return std::string("?");
        };
        int rowIndex = 0;
        if (lobby)
        {
            for (const Net::LobbySlot &s : lobby->slots)
            {
                const UiRect row = l.row(rowIndex);
                const bool me = s.slot == net->LocalSlot();
                const bool editable = me || (s.ai && net->ControlsRoom());
                ui::DrawPanel(r, row, me ? col::Hover : col::PanelRaised, me ? col::Accent : col::Border);
                const std::string name
                    = s.ai ? std::string(_("AI")) + " (" + AiLevelName(s.aiLevel) + ")" : nameOf(s.slot);
                if (s.ai && net->ControlsRoom())
                {
                    // The level of a computer player: a drop-down list
                    const UiRect nc = l.NameCell(rowIndex);
                    ui::DrawComboField(
                        r,
                        font,
                        nc,
                        name,
                        m_combo.open == rowIndex * 10 + 4,
                        nc.Contains(m_mouseX, m_mouseY),
                        true,
                        col::Info);
                }
                else
                {
                    font.DrawString(
                        ui::FitText(font, name, ui::FONT_BODY_BOLD, 150),
                        row.x + 10,
                        row.CenterY(),
                        ui::FONT_BODY_BOLD,
                        s.ai ? col::Info : col::Text,
                        false,
                        Anchor::LEFT | Anchor::VCENTER);
                }
                if (s.ai && net->ControlsRoom())
                {
                    const UiRect x = l.RemoveCell(rowIndex);
                    ui::DrawButton(r, font, x, "x", ui::ButtonStyle::Danger, x.Contains(m_mouseX, m_mouseY));
                }
                auto cell = [&](const UiRect &c, const std::string &text) {
                    const bool hot = editable && c.Contains(m_mouseX, m_mouseY);
                    ui::DrawPanel(r, c, hot ? col::Hover : col::PanelSunken, hot ? col::BorderStrong : col::Border);
                    font.DrawString(
                        text,
                        c.CenterX(),
                        c.CenterY(),
                        ui::FONT_SMALL,
                        col::Text,
                        false,
                        Anchor::HCENTER | Anchor::VCENTER);
                };
                Net::LobbyChoice choice = s.choice;
                if (me && !s.ai)
                    net->LocalChoice(choice);
                const UiRect cc = l.ColorCell(rowIndex);
                if (editable)
                {
                    // Drop-down lists: faction, team, colour
                    auto combo = [&](const UiRect &c, int kind, const std::string &text) {
                        ui::DrawComboField(
                            r, font, c, text, m_combo.open == rowIndex * 10 + kind, c.Contains(m_mouseX, m_mouseY));
                    };
                    combo(l.FactionCell(rowIndex), 1, FactionName(choice.faction));
                    if (net->ControlsRoom())
                        combo(l.TeamCell(rowIndex), 2, std::to_string(choice.team + 1));
                    else
                        cell(l.TeamCell(rowIndex), std::string(_("Team")) + " " + std::to_string(choice.team + 1));
                    combo(cc, 3, "");
                    const UiRect sw {cc.x + 6, cc.y + 6, cc.w - 28, cc.h - 12};
                    r.FillRectScreen(sw.x, sw.y, sw.w, sw.h, SlotColor(choice.color));
                    r.DrawRectScreen(sw.x, sw.y, sw.w, sw.h, col::BorderStrong);
                }
                else
                {
                    cell(l.FactionCell(rowIndex), FactionName(choice.faction));
                    // The host / owner sets the team of every player
                    if (net->ControlsRoom())
                        ui::DrawComboField(
                            r,
                            font,
                            l.TeamCell(rowIndex),
                            std::to_string(choice.team + 1),
                            m_combo.open == rowIndex * 10 + 2,
                            l.TeamCell(rowIndex).Contains(m_mouseX, m_mouseY));
                    else
                        cell(l.TeamCell(rowIndex), std::string(_("Team")) + " " + std::to_string(choice.team + 1));
                    ui::DrawPanel(r, cc, col::PanelSunken, col::Border);
                    const UiRect sw {cc.x + 6, cc.y + 6, cc.w - 12, cc.h - 12};
                    r.FillRectScreen(sw.x, sw.y, sw.w, sw.h, SlotColor(choice.color));
                    r.DrawRectScreen(sw.x, sw.y, sw.w, sw.h, col::BorderStrong);
                }
                const std::string spawn
                    = choice.spawn == Net::kRandom ? std::string("?") : std::to_string(choice.spawn + 1);
                font.DrawString(
                    spawn,
                    row.x + 430,
                    row.CenterY(),
                    ui::FONT_BODY_BOLD,
                    col::TextDim,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
                if (!s.ai)
                {
                    font.DrawString(
                        choice.ready ? _("READY") : "",
                        row.Right() - 10,
                        row.CenterY(),
                        ui::FONT_SMALL_BOLD,
                        col::Ok,
                        false,
                        Anchor::RIGHT | Anchor::VCENTER);
                }
                ++rowIndex;
            }
            if (net->ControlsRoom() && rowIndex < std::max<int>(1, lobby->spawnCount))
            {
                const UiRect add = l.AddAi(rowIndex);
                ui::DrawButton(
                    r, font, add, _("+ AI PLAYER"), ui::ButtonStyle::Ghost, add.Contains(m_mouseX, m_mouseY));
            }
        }

        // Map with its start positions
        // The map's own title (the list of maps is the same on every peer: the assets match)
        if (m_maps.empty())
            m_maps = MissionController::ListSkirmishMaps();
        std::string mapName;
        if (lobby)
        {
            for (const SkirmishMapInfo &m : m_maps)
                if (m.path == lobby->map)
                    mapName = m.DisplayName();
            if (mapName.empty() && !lobby->map.empty())
                mapName = lobby->map.substr(lobby->map.find_last_of('/') + 1);
        }
        if (net->ControlsRoom())
        {
            ui::DrawComboField(
                r, font, l.mapName, mapName, m_combo.open == kMapCombo, l.mapName.Contains(m_mouseX, m_mouseY));
        }
        else
        {
            font.DrawString(
                ui::FitText(font, mapName, ui::FONT_BODY_BOLD, l.mapName.w),
                l.map.CenterX(),
                l.mapPrev.CenterY(),
                ui::FONT_BODY_BOLD,
                col::Text,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }
        if (net->ControlsRoom())
        {
            ui::DrawButton(r, font, l.mapPrev, "<", ui::ButtonStyle::Secondary, l.mapPrev.Contains(m_mouseX, m_mouseY));
            ui::DrawButton(r, font, l.mapNext, ">", ui::ButtonStyle::Secondary, l.mapNext.Contains(m_mouseX, m_mouseY));
        }
        ui::DrawPanel(r, l.map, col::PanelSunken, col::Border);
        if (lobby && !lobby->map.empty() && !net->MapReady())
        {
            font.DrawString(
                _("Downloading the map..."),
                l.map.CenterX(),
                l.map.CenterY(),
                ui::FONT_BODY,
                col::TextDim,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }
        if (lobby && net->MapReady())
        {
            int pw = 0, ph = 0;
            const std::string &file = net->LobbyMapFile();
            SDL_Texture *tex = m_preview.Get(r.GetSdlRenderer(), textures, file, file, {}, pw, ph);
            if (tex)
            {
                m_mapW = pw;
                m_mapH = ph;
                const float scale = std::min(static_cast<float>(l.map.w) / pw, static_cast<float>(l.map.h) / ph);
                const SDL_FRect dst {
                    l.map.x + (l.map.w - pw * scale) / 2.0f,
                    l.map.y + (l.map.h - ph * scale) / 2.0f,
                    pw * scale,
                    ph * scale};
                SDL_RenderTexture(r.GetSdlRenderer(), tex, nullptr, &dst);
            }
            for (size_t i = 0; i < m_spots.size() && i < lobby->spawnCount; ++i)
            {
                int sx = 0, sy = 0;
                if (!SpotOnScreen(l.map, m_mapW, m_mapH, m_spots[i].first, m_spots[i].second, sx, sy))
                    continue;
                SDL_Color c = col::TextDim;
                for (const Net::LobbySlot &s : lobby->slots)
                {
                    Net::LobbyChoice sc = s.choice;
                    if (!s.ai && s.slot == net->LocalSlot())
                        net->LocalChoice(sc);
                    if (sc.spawn == i)
                        c = SlotColor(sc.color);
                }
                const bool hot = std::abs(m_mouseX - sx) <= 14 && std::abs(m_mouseY - sy) <= 14;
                r.FillRectScreen(sx - 12, sy - 12, 24, 24, col::PanelSunken);
                r.FillRectScreen(sx - 10, sy - 10, 20, 20, c);
                r.DrawRectScreen(sx - 12, sy - 12, 24, 24, hot ? col::Accent : col::Text);
                font.DrawString(
                    std::to_string(i + 1),
                    sx,
                    sy,
                    ui::FONT_SMALL_BOLD,
                    col::Backdrop,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
            }
        }
        if (net->ControlsRoom())
        {
            ui::DrawComboField(
                r,
                font,
                l.prebuiltBtn,
                m_prebuilt ? _("PREBUILT BASES") : _("FROM SCRATCH"),
                m_combo.open == kLayoutCombo,
                l.prebuiltBtn.Contains(m_mouseX, m_mouseY));
        }
        if (lobby && !lobby->map.empty())
        {
            // How many players the map is made for (its start positions)
            font.DrawString(
                std::string(_("Players on this map: ")) + std::to_string(lobby->spawnCount),
                l.map.x + 8,
                l.map.Bottom() - 18,
                ui::FONT_SMALL_BOLD,
                col::Accent,
                true);
        }
        font.DrawTextWrapped(
            _("Click a number on the map to start there, again to leave it to chance. Click your faction and colour to "
              "change them; the host sets the teams."),
            l.map.x,
            l.map.Bottom() + 10,
            l.map.w,
            ui::FONT_SMALL,
            col::TextDim,
            false);

        // Chat
        ui::DrawPanel(r, l.chatLog, col::PanelSunken, col::Border);
        const int first = std::max(0, static_cast<int>(m_chatLog.size()) - kChatLines);
        int y = l.chatLog.y + 6;
        for (int i = first; i < static_cast<int>(m_chatLog.size()); ++i)
        {
            font.DrawString(
                ui::FitText(font, m_chatLog[i], ui::FONT_SMALL, l.chatLog.w - 16),
                l.chatLog.x + 8,
                y,
                ui::FONT_SMALL,
                col::Text,
                false);
            y += 18;
        }
        DrawField(r, font, l.chatField, m_chatInput, _("Type a message"), m_focus == Field::Chat);
        ui::DrawButton(
            r,
            font,
            l.chatSend,
            "->",
            ui::ButtonStyle::Primary,
            l.chatSend.Contains(m_mouseX, m_mouseY),
            !m_chatInput.empty());

        bool ready = false;
        if (lobby)
        {
            for (const Net::LobbySlot &s : lobby->slots)
            {
                if (s.slot == net->LocalSlot())
                {
                    Net::LobbyChoice sc = s.choice;
                    net->LocalChoice(sc);
                    ready = sc.ready;
                }
            }
        }
        ui::DrawButton(
            r, font, l.leaveBtn, _("LEAVE"), ui::ButtonStyle::Secondary, l.leaveBtn.Contains(m_mouseX, m_mouseY));
        ui::DrawButton(
            r,
            font,
            l.readyBtn,
            ready ? _("NOT READY") : _("READY"),
            ready ? ui::ButtonStyle::Secondary : ui::ButtonStyle::Primary,
            l.readyBtn.Contains(m_mouseX, m_mouseY),
            net->MapReady());
        if (net->ControlsRoom())
        {
            ui::DrawButton(
                r,
                font,
                l.startBtn,
                _("START"),
                ui::ButtonStyle::Primary,
                l.startBtn.Contains(m_mouseX, m_mouseY),
                net->CanStart());
        }
        else
        {
            font.DrawString(
                _("The host starts the game"),
                l.startBtn.CenterX(),
                l.startBtn.CenterY(),
                ui::FONT_SMALL,
                col::TextDim,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }
        // The open drop-down list, over everything
        if (m_combo.open >= 0)
        {
            const int id = m_combo.open, row = id / 10, kind = id == kLayoutCombo ? 5 : id % 10;
            if (id == kMapCombo)
            {
                std::vector<std::string> names;
                std::vector<bool> unfit;
                int current = 0;
                const size_t inRoom = lobby ? lobby->slots.size() : 1;
                for (size_t i = 0; i < m_maps.size(); ++i)
                {
                    names.push_back(m_maps[i].DisplayName());
                    unfit.push_back(MissionController::StartPositions(m_maps[i].path).size() < inRoom);
                    if (lobby && m_maps[i].path == lobby->map)
                        current = static_cast<int>(i);
                }
                ui::DrawComboList(r, font, l.mapName, names, current, m_combo, h, m_mouseX, m_mouseY, &unfit);
            }
            else if (id == kLayoutCombo)
            {
                ui::DrawComboList(
                    r, font, l.prebuiltBtn, ComboChoices(5), m_prebuilt ? 0 : 1, m_combo, h, m_mouseX, m_mouseY);
            }
            else if (lobby && row < static_cast<int>(lobby->slots.size()))
            {
                const Net::LobbySlot &s = lobby->slots[row];
                Net::LobbyChoice sc = s.choice;
                if (!s.ai && s.slot == net->LocalSlot())
                    net->LocalChoice(sc);
                const ui::UiRect field = kind == 1   ? l.FactionCell(row)
                                         : kind == 2 ? l.TeamCell(row)
                                         : kind == 3 ? l.ColorCell(row)
                                                     : l.NameCell(row);
                const int current = kind == 1   ? (sc.faction == Net::kRandom ? 0 : sc.faction + 1)
                                    : kind == 2 ? sc.team
                                    : kind == 3 ? std::max(0, sc.color - 1)
                                                : s.aiLevel;
                std::vector<std::string> items = ComboChoices(kind);
                std::vector<bool> taken;
                std::vector<SDL_Color> swatches;
                if (kind == 3)
                {
                    taken = TakenColors(*lobby, s.slot);
                    for (int i = 1; i <= Settings::kArmyColors; ++i)
                        swatches.push_back(ArmyColor(i));
                    for (size_t i = 0; i < items.size() && i < taken.size(); ++i)
                    {
                        if (taken[i] && static_cast<int>(i) != current)
                            items[i] += std::string(" (") + _("taken") + ")";
                    }
                }
                ui::DrawComboList(
                    r,
                    font,
                    field,
                    items,
                    current,
                    m_combo,
                    h,
                    m_mouseX,
                    m_mouseY,
                    kind == 3 ? &taken : nullptr,
                    kind == 3 ? &swatches : nullptr);
            }
        }
        ui::DrawConfirm(r, font, w, h, m_confirm, m_mouseX, m_mouseY);
        return;
    }

    // ----- Browser / connecting -----
    const BrowserLayout l = browserLayout(w, h);
    if (net)
    {
        // Connecting: the loading screen with what the connection is doing, and a way out
        ui::DrawLoadingScreen(
            r, font, w, h, _("NETWORK GAME"), net->ConnectingText(SDL_GetTicks()), -1.0f, SDL_GetTicks());
        ui::DrawButton(
            r, font, l.backBtn, _("CANCEL"), ui::ButtonStyle::Secondary, l.backBtn.Contains(m_mouseX, m_mouseY));
        return;
    }
    ui::DrawDialog(r, font, w, h, l.dialog, _("NETWORK GAME"));
    font.DrawString(
        _("Your name"),
        l.dialog.x + 24,
        l.nameField.CenterY(),
        ui::FONT_BODY_BOLD,
        col::Text,
        false,
        Anchor::LEFT | Anchor::VCENTER);
    DrawField(r, font, l.nameField, settings.playerName, _("Your name in the game"), m_focus == Field::Name);
    font.DrawString(
        _("Password"),
        l.nameField.Right() + 16,
        l.nameField.CenterY(),
        ui::FONT_BODY_BOLD,
        col::Text,
        false,
        Anchor::LEFT | Anchor::VCENTER);
    DrawField(r, font, l.passwordField, std::string(m_password.size(), '*'), _("Optional"), m_focus == Field::Password);
    ui::DrawButton(
        r,
        font,
        l.tabLan,
        _("LOCAL NETWORK"),
        m_tab == Tab::Lan ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
        l.tabLan.Contains(m_mouseX, m_mouseY));
    ui::DrawButton(
        r,
        font,
        l.tabInternet,
        _("INTERNET"),
        m_tab == Tab::Internet ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
        l.tabInternet.Contains(m_mouseX, m_mouseY));
    if (m_tab == Tab::Lan)
    {
        ui::DrawPanel(r, l.list, col::PanelSunken, col::Border);
        if (lan.empty())
        {
            font.DrawString(
                _("Looking for games on the local network..."),
                l.list.CenterX(),
                l.list.CenterY(),
                ui::FONT_BODY,
                col::TextDim,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }
        for (size_t i = 0; i < lan.size() && i < 8; ++i)
        {
            const UiRect row {l.list.x + 4, l.list.y + 4 + static_cast<int>(i) * 40, l.list.w - 8, 36};
            const bool sel = static_cast<int>(i) == m_lanSelected;
            ui::DrawPanel(r, row, sel ? col::Hover : col::PanelRaised, sel ? col::Accent : col::Border);
            const LanGame &g = lan[i];
            font.DrawString(
                ui::FitText(font, g.name, ui::FONT_BODY_BOLD, 220),
                row.x + 10,
                row.CenterY(),
                ui::FONT_BODY_BOLD,
                col::Text,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                ui::FitText(font, g.map, ui::FONT_SMALL, 160),
                row.x + 250,
                row.CenterY(),
                ui::FONT_SMALL,
                col::TextDim,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                std::to_string(g.players) + "/" + std::to_string(g.maxPlayers),
                row.x + 440,
                row.CenterY(),
                ui::FONT_SMALL,
                col::Text,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                std::to_string(g.pingMs) + " ms",
                row.Right() - 10,
                row.CenterY(),
                ui::FONT_SMALL,
                g.pingMs < 60 ? col::Ok : col::Accent,
                false,
                Anchor::RIGHT | Anchor::VCENTER);
            if (g.locked)
                DrawLock(r, row.x + 412, row.CenterY() - 7, col::Accent);
        }
    }
    else
    {
        font.DrawString(
            _("Direct connection (the host forwards UDP port 38100)"),
            l.addressField.x,
            l.addressField.y - 18,
            ui::FONT_SMALL,
            col::TextDim,
            false);
        DrawField(
            r, font, l.addressField, settings.lastAddress, _("IP address or hostname"), m_focus == Field::Address);
        ui::DrawButton(
            r,
            font,
            l.directBtn,
            _("CONNECT"),
            ui::ButtonStyle::Secondary,
            l.directBtn.Contains(m_mouseX, m_mouseY),
            !settings.lastAddress.empty());
        ui::DrawCaption(font, r, l.serverList.x, l.refreshBtn.y + 6, std::string(_("SERVER")) + "  " + serverStatus);
        ui::DrawButton(
            r, font, l.refreshBtn, _("REFRESH"), ui::ButtonStyle::Ghost, l.refreshBtn.Contains(m_mouseX, m_mouseY));
        ui::DrawPanel(r, l.serverList, col::PanelSunken, col::Border);
        if (server.empty())
        {
            font.DrawString(
                _("No open rooms: create one, or join by code"),
                l.serverList.CenterX(),
                l.serverList.CenterY(),
                ui::FONT_SMALL,
                col::TextDim,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }
        for (int i = 0; i < static_cast<int>(server.size()) && i < 4; ++i)
        {
            const UiRect row = l.ServerRow(i);
            const bool sel = i == m_serverSelected;
            const LanGame &g = server[i];
            ui::DrawPanel(r, row, sel ? col::Hover : col::PanelRaised, sel ? col::Accent : col::Border);
            font.DrawString(
                g.code,
                row.x + 10,
                row.CenterY(),
                ui::FONT_SMALL_BOLD,
                g.port == 0 ? col::Info : col::Accent,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                ui::FitText(font, g.name, ui::FONT_BODY_BOLD, 200),
                row.x + 90,
                row.CenterY(),
                ui::FONT_BODY_BOLD,
                col::Text,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                ui::FitText(font, g.map, ui::FONT_SMALL, 160),
                row.x + 310,
                row.CenterY(),
                ui::FONT_SMALL,
                col::TextDim,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                std::to_string(g.players) + "/" + std::to_string(g.maxPlayers),
                row.Right() - 10,
                row.CenterY(),
                ui::FONT_SMALL,
                col::Text,
                false,
                Anchor::RIGHT | Anchor::VCENTER);
            if (g.locked)
                DrawLock(r, row.Right() - 56, row.CenterY() - 7, col::Accent);
        }
        font.DrawString(
            _("Room code"),
            l.serverList.x,
            l.codeField.CenterY(),
            ui::FONT_BODY_BOLD,
            col::Text,
            false,
            Anchor::LEFT | Anchor::VCENTER);
        DrawField(r, font, l.codeField, m_joinCode, _("6 characters"), m_focus == Field::Code);
        ui::DrawButton(
            r,
            font,
            l.codeBtn,
            _("JOIN BY CODE"),
            ui::ButtonStyle::Secondary,
            l.codeBtn.Contains(m_mouseX, m_mouseY),
            m_joinCode.size() == 6);
    }
    if (!m_notice.empty())
    {
        font.DrawString(
            m_notice,
            l.dialog.CenterX(),
            l.joinBtn.y - 18,
            ui::FONT_SMALL_BOLD,
            col::Danger,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
    }
    ui::DrawButton(r, font, l.backBtn, _("BACK"), ui::ButtonStyle::Secondary, l.backBtn.Contains(m_mouseX, m_mouseY));
    ui::DrawButton(
        r, font, l.hostBtn, _("CREATE GAME"), ui::ButtonStyle::Secondary, l.hostBtn.Contains(m_mouseX, m_mouseY));
    if (m_tab == Tab::Internet)
    {
        ui::DrawButton(
            r, font, l.serverBtn, _("CREATE"), ui::ButtonStyle::Secondary, l.serverBtn.Contains(m_mouseX, m_mouseY));
    }
    const bool canJoin = m_tab == Tab::Internet
                             ? (m_serverSelected >= 0 && m_serverSelected < static_cast<int>(server.size()))
                             : (m_lanSelected >= 0 && m_lanSelected < static_cast<int>(lan.size()));
    ui::DrawButton(
        r, font, l.joinBtn, _("JOIN"), ui::ButtonStyle::Primary, l.joinBtn.Contains(m_mouseX, m_mouseY), canJoin);
}

} // namespace OpenArtOfWar
