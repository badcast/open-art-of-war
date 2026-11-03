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
#include "ui/MatchOptions.hpp"
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
    else if (kind == 7)
    {
        out = {_("None"), "-25%", "-50%", "+25%", "+50%", "+100%"}; // the start credits form, see kHandicapPercent
    }
    else if (kind == 8)
        out = ui::ArmyCapChoices();
    else
        out = {_("PREBUILT BASES"), _("FROM SCRATCH")};
    return out;
}

// The three filter lists over the list of maps (the same choices as in the custom game window)
std::vector<std::string> MapFilterChoices(int id, const std::vector<SkirmishMapInfo> &all)
{
    std::vector<std::string> out;
    if (id == 1003)
    {
        for (int n : ui::MapFilter::PlayerCounts(all))
            out.push_back(std::string(_("Players")) + ": " + (n == 0 ? std::string(_("Any")) : std::to_string(n)));
    }
    else if (id == 1004)
    {
        for (const char *z : {_("Any"), _("Small"), _("Medium"), _("Large")})
            out.push_back(std::string(_("Size")) + ": " + z);
    }
    else
    {
        for (const char *z : {_("Name"), _("Players"), _("Size")})
            out.push_back(std::string(_("Sort")) + ": " + z);
    }
    return out;
}

// What a start position row can hold, as the combo shows it: the choices depend on the viewer and the row
enum SeatCode
{
    kSeatOccupy = 1,
    kSeatOpen = 2,
    kSeatAi = 3, // + level 0..3
    kSeatBlocked = 7
};
struct SeatOption
{
    std::string label;
    int code;
};

// Index into lobby.slots of the player holding the start position (-1: nobody)
int SlotAtSpawn(const Net::LobbyState &lobby, int position)
{
    for (size_t i = 0; i < lobby.slots.size(); ++i)
        if (lobby.slots[i].choice.spawn == position)
            return static_cast<int>(i);
    return -1;
}

std::vector<SeatOption> SeatOptions(const NetMatch &net, const Net::LobbyState &lobby, int position)
{
    std::vector<SeatOption> out;
    const int held = SlotAtSpawn(lobby, position);
    const bool blocked = ((lobby.blockedMask >> position) & 1u) != 0;
    const bool mine = held >= 0 && !lobby.slots[held].ai && lobby.slots[held].slot == net.LocalSlot();
    if (mine)
        return out; // your own place: nothing to choose
    const bool free = held < 0 && !blocked;
    if (net.ControlsRoom())
    {
        if (free)
            out.push_back({_("Occupy"), kSeatOccupy});
        out.push_back({_("Open"), kSeatOpen});
        for (int level = 0; level < 4; ++level)
            out.push_back({std::string(_("AI")) + " - " + AiLevelName(static_cast<uint8_t>(level)), kSeatAi + level});
        out.push_back({_("Blocked"), kSeatBlocked});
    }
    else if (free)
    {
        out.push_back({_("Open"), kSeatOpen});
        out.push_back({_("Occupy"), kSeatOccupy});
    }
    return out;
}

// Index in SeatOptions of what the position holds now (-1: a person)
int SeatCurrent(const std::vector<SeatOption> &options, const Net::LobbyState &lobby, int position)
{
    const int held = SlotAtSpawn(lobby, position);
    int code = kSeatOpen;
    if (held >= 0)
        code = lobby.slots[held].ai ? kSeatAi + (lobby.slots[held].aiLevel & 3) : 0;
    else if ((lobby.blockedMask >> position) & 1u)
        code = kSeatBlocked;
    for (size_t i = 0; i < options.size(); ++i)
        if (options[i].code == code)
            return static_cast<int>(i);
    return -1;
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

void NetworkMenu::PushOptions(NetMatch &net)
{
    net.SetOptions(m_fog, m_prebuilt, static_cast<uint8_t>(std::clamp(m_credits, 0, 4)), m_night);
}

void NetworkMenu::Reset()
{
    m_optionsSent = false;
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
    l.dialog = ui::CenteredRect(w, h, w, h); // the whole window
    const int x = l.dialog.x + 20;
    const int contentY = l.dialog.y + 52 + 16;
    const int footerY = l.dialog.Bottom() - 64;
    const int colH = footerY - contentY - 8;
    const int leftW = std::clamp(static_cast<int>(l.dialog.w * 0.30f), 330, 480);
    // Left: search and filters, the list of maps, the chat
    const int chatH = std::max(170, static_cast<int>(colH * 0.36f));
    l.mapSearch = {x, contentY, leftW, 30};
    l.mapList = {x, contentY + 72, leftW, colH - 72 - chatH - 10};
    l.chatField = {x, contentY + colH - 36, leftW - 52, 36};
    l.chatSend = {l.chatField.Right() + 8, l.chatField.y, 44, 36};
    l.chatLog = {x, l.mapList.Bottom() + 10, leftW, l.chatField.y - 6 - (l.mapList.Bottom() + 10)};
    // Right: the details
    l.details = {x + leftW + 16, contentY, l.dialog.Right() - 20 - (x + leftW + 16), colH};
    const ui::UiRect &d = l.details;
    // The seat table takes the whole width of the details (its cells share it); the map picture sits under it on the
    // left, the options and the hint on its right
    const int rowH = std::clamp((d.h - 74 - 24 - 150 - 30) / 8, 30, 40);
    l.rowH = rowH;
    l.players = {d.x + 20, d.y + 74, d.w - 40, 24 + 8 * rowH};
    const int below = d.Bottom() - 16 - (l.players.Bottom() + 12);
    const int size = std::clamp(std::min(below, static_cast<int>(d.w * 0.45f)), 120, 460);
    l.map = {d.x + 20, l.players.Bottom() + 12, size, size};
    const int optX = l.map.Right() + 20;
    const int optW = d.Right() - 20 - optX;
    // Match options: the time of day and the credits next to the title, the check boxes beside the picture
    l.creditsBtn = {d.Right() - 20 - 200, d.y + 12, 200, 32};
    l.timeBtn = {l.creditsBtn.x - 12 - 160, d.y + 12, 160, 32};
    l.fogCheckbox = {optX, l.map.y, optW, 28};
    l.prebuiltBtn = {optX, l.map.y + 32, optW, 28}; // a check box: start with a base
    // Footer: Leave on the left, Ready and Start on the right - never on top of each other
    l.leaveBtn = {x, footerY + 10, 140, 40};
    l.startBtn = {l.dialog.Right() - 20 - 200, footerY + 10, 200, 40};
    l.readyBtn = {l.startBtn.x - 12 - 200, footerY + 10, 200, 40};
    return l;
}

void NetworkMenu::RebuildMapsShown()
{
    if (m_maps.empty())
        m_maps = MissionController::ListSkirmishMaps();
    m_mapsShown = m_mapFilter.Apply(m_maps);
    m_mapTop = 0;
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
    sx = std::clamp(ox + static_cast<int>((tileX + 5) * scale), area.x + 14, area.Right() - 14);
    sy = std::clamp(oy + static_cast<int>((tileY + 5) * scale), area.y + 14, area.Bottom() - 14);
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
    if (m_maps.empty())
        RebuildMapsShown();
    if (net.ControlsRoom() && lobby && lobby->map.empty())
    {
        if (m_maps.empty())
            m_maps = MissionController::ListSkirmishMaps();
        if (!m_maps.empty())
            net.SetLobbyMap(m_maps[std::clamp(m_mapIndex, 0, static_cast<int>(m_maps.size()) - 1)].path);
    }
    if (!m_optionsSent && net.ControlsRoom() && lobby)
    {
        m_optionsSent = true;
        PushOptions(net); // the room shows the owner's options from the start
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
        if (id == kCreditsCombo)
            return l.creditsBtn;
        if (id == kTimeCombo)
            return l.timeBtn;
        if (id >= kFilterPlayers && id <= kFilterSort)
            return l.Filter(id - kFilterPlayers);
        const int row = id / 10, kind = id % 10;
        return kind == 1   ? l.FactionCell(row)
               : kind == 2 ? l.TeamCell(row)
               : kind == 3 ? l.ColorCell(row)
               : kind == 7 ? l.HandicapCell(row)
               : kind == 8 ? l.ArmyCell(row)
                           : l.TypeCell(row);
    };
    // The filter lists over the maps work for everybody in the room
    if (m_combo.open >= kFilterPlayers && m_combo.open <= kFilterSort)
    {
        const int id = m_combo.open;
        int picked = -1;
        if (ui::ComboUpdate(
                m_combo,
                comboField(id),
                static_cast<int>(MapFilterChoices(id, m_maps).size()),
                h,
                mx,
                my,
                clicked,
                input.ConsumeWheel(),
                picked))
        {
            if (picked >= 0)
            {
                if (id == kFilterPlayers)
                {
                    const std::vector<int> counts = ui::MapFilter::PlayerCounts(m_maps);
                    m_mapFilter.players = counts[std::clamp(picked, 0, static_cast<int>(counts.size()) - 1)];
                }
                else if (id == kFilterSize)
                    m_mapFilter.size = std::clamp(picked, 0, 3);
                else
                    m_mapFilter.sort = std::clamp(picked, 0, 2);
                RebuildMapsShown();
            }
            return NetworkAction::None;
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            m_combo.open = -1;
            return NetworkAction::None;
        }
    }
    if (m_combo.open >= 0 && !(m_combo.open >= kFilterPlayers && m_combo.open <= kFilterSort))
    {
        const bool optionList = IsOptionCombo(m_combo.open);
        const int id = m_combo.open, row = id / 10, kind = optionList ? 5 : id % 10;
        const bool isMapList = false;
        const bool valid = optionList || isMapList ? net.ControlsRoom()
                                                           : (lobby && row < std::min<int>(lobby->spawnCount, kMaxPlayers));
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
                    isMapList  ? static_cast<int>(m_maps.size())
                    : id == kTimeCombo ? static_cast<int>(ui::TimeOfDayChoices().size())
                    : optionList ? static_cast<int>(ui::CreditsChoices().size())
                    : kind == 0 ? static_cast<int>(SeatOptions(net, *lobby, row).size())
                                : static_cast<int>(ComboChoices(kind).size()),
                    h,
                    mx,
                    my,
                    clicked,
                    input.ConsumeWheel(),
                    picked))
            {
                if (false)
                {
                }
                else if (picked >= 0 && id == kLayoutCombo)
                {
                    m_prebuilt = picked == 0;
                    PushOptions(net);
                }
                else if (picked >= 0 && id == kCreditsCombo)
                {
                    m_credits = std::clamp(picked, 0, 4);
                    PushOptions(net);
                }
                else if (picked >= 0 && id == kTimeCombo)
                {
                    m_night = picked == 1;
                    PushOptions(net);
                }
                else if (picked >= 0 && kind == 0)
                {
                    // What the start position holds: a seat for the player himself, or the owner's decision
                    const std::vector<SeatOption> options = SeatOptions(net, *lobby, row);
                    if (picked >= static_cast<int>(options.size()))
                        return NetworkAction::None;
                    const int code = options[picked].code;
                    Net::LobbyChoice own;
                    if (code == kSeatOccupy)
                    {
                        if (net.LocalChoice(own))
                        {
                            own.spawn = static_cast<uint8_t>(row);
                            own.ready = false;
                            net.SetChoice(own);
                        }
                    }
                    else if (code != SeatCurrent(options, *lobby, row) || SlotAtSpawn(*lobby, row) >= 0)
                    {
                        Net::OwnerSetSeat seat;
                        seat.spawn = static_cast<uint8_t>(row);
                        seat.kind = code == kSeatOpen ? Net::OwnerSetSeat::Open
                                    : code == kSeatBlocked ? Net::OwnerSetSeat::Blocked
                                                           : Net::OwnerSetSeat::Ai;
                        seat.level = code >= kSeatAi && code < kSeatBlocked ? static_cast<uint8_t>(code - kSeatAi) : 1;
                        net.SetSeat(seat);
                    }
                }
                else if (picked >= 0)
                {
                    const int si = SlotAtSpawn(*lobby, row);
                    if (si < 0)
                        return NetworkAction::None;
                    const Net::LobbySlot &s = lobby->slots[si];
                    Net::LobbyChoice choice = s.choice;
                    if (!s.ai && s.slot == net.LocalSlot())
                        net.LocalChoice(choice); // edit what was last sent, not the room's lagging echo
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
                                net.SetTeam(s.slot, choice.team, choice.handicap, choice.armyCap);
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
                    if (kind == 7 || kind == 8)
                    {
                        // The form (start credits) and the army size limit are the owner's decision, for every player
                        if (kind == 7)
                            choice.handicap = static_cast<uint8_t>(picked);
                        else
                            choice.armyCap = static_cast<uint8_t>(picked);
                        if (!s.ai)
                        {
                            if (net.ControlsRoom())
                                net.SetTeam(s.slot, choice.team, choice.handicap, choice.armyCap);
                            return NetworkAction::None;
                        }
                    }
                    if (s.ai && net.ControlsRoom())
                    {
                        Net::OwnerSetAi change;
                        change.slot = s.slot;
                        change.level = s.aiLevel;
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
    if (m_focus == Field::Search)
    {
        const std::string before = m_mapFilter.search;
        EditText(input, m_mapFilter.search, 40);
        if (m_mapFilter.search != before)
            RebuildMapsShown();
        if (input.IsKeyJustPressed(SDL_SCANCODE_RETURN) || input.IsKeyJustPressed(SDL_SCANCODE_KP_ENTER))
        {
            m_focus = Field::None;
            return NetworkAction::None;
        }
    }
    // The wheel over the list of maps scrolls it
    if (m_combo.open < 0 && l.mapList.Contains(mx, my))
        m_mapTop = std::clamp(
            m_mapTop - input.ConsumeWheel(), 0, std::max(0, static_cast<int>(m_mapsShown.size()) - l.MapRows()));
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

    m_focus = l.chatField.Contains(mx, my) ? Field::Chat : (l.mapSearch.Contains(mx, my) ? Field::Search : Field::None);
    if (m_focus == Field::Search)
        return NetworkAction::None;
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
    {
        m_prebuilt = !m_prebuilt;
        PushOptions(net);
        SoundRequests::Play("ui_confirm");
        return NetworkAction::None;
    }
    if (net.ControlsRoom() && l.creditsBtn.Contains(mx, my))
        return openCombo(kCreditsCombo);
    if (net.ControlsRoom() && l.timeBtn.Contains(mx, my))
        return openCombo(kTimeCombo);
    if (net.ControlsRoom() && l.fogCheckbox.Contains(mx, my))
    {
        m_fog = !m_fog;
        SoundRequests::Play("ui_confirm");
        PushOptions(net);
        return NetworkAction::None;
    }
    // The filters over the maps (everybody may look through the list) and the list: only the owner picks the map
    for (int k = 0; k < 3; ++k)
        if (l.Filter(k).Contains(mx, my))
            return openCombo(kFilterPlayers + k);
    if (net.ControlsRoom() && l.mapList.Contains(mx, my))
    {
        const int i = m_mapTop + (my - l.mapList.y) / l.listRowH;
        if (i >= 0 && i < static_cast<int>(m_mapsShown.size()))
        {
            // Only a map with a start position for everybody in the room
            const size_t inRoom = lobby ? lobby->slots.size() : 1;
            if (MissionController::StartPositions(m_mapsShown[i].path).size() >= inRoom)
            {
                for (size_t k = 0; k < m_maps.size(); ++k)
                    if (m_maps[k].path == m_mapsShown[i].path)
                        m_mapIndex = static_cast<int>(k);
                net.SetLobbyMap(m_mapsShown[i].path);
            }
            else
            {
                m_chatLog.push_back(_("This map has no start position for everybody in the room."));
                if (m_chatLog.size() > 50)
                    m_chatLog.erase(m_chatLog.begin());
            }
        }
        return NetworkAction::None;
    }
    // The rows are the start positions of the map: what holds each (a drop-down list), and the picks of who holds it
    if (lobby)
    {
        const int rows = std::min<int>(lobby->spawnCount, kMaxPlayers);
        for (int i = 0; i < rows; ++i)
        {
            if (!SeatOptions(net, *lobby, i).empty() && l.TypeCell(i).Contains(mx, my))
                return openCombo(i * 10);
            const int si = SlotAtSpawn(*lobby, i);
            if (si < 0)
                continue;
            const Net::LobbySlot &s = lobby->slots[si];
            const bool editable = s.ai ? net.ControlsRoom() : s.slot == net.LocalSlot();
            if (editable && l.FactionCell(i).Contains(mx, my))
                return openCombo(i * 10 + 1);
            if (net.ControlsRoom() && l.TeamCell(i).Contains(mx, my))
                return openCombo(i * 10 + 2);
            if (editable && l.ColorCell(i).Contains(mx, my))
                return openCombo(i * 10 + 3);
            if (net.ControlsRoom() && l.HandicapCell(i).Contains(mx, my))
                return openCombo(i * 10 + 7);
            if (net.ControlsRoom() && l.ArmyCell(i).Contains(mx, my))
                return openCombo(i * 10 + 8);
        }
    }
    if (!haveMine)
        return NetworkAction::None;

    Net::LobbyChoice next = mine;
    if (l.readyBtn.Contains(mx, my) && net.MapReady())
        next.ready = !mine.ready;
    // An open start position on the map: take it
    if (l.map.Contains(mx, my) && lobby)
    {
        for (size_t i = 0; i < m_spots.size(); ++i)
        {
            int sx = 0, sy = 0;
            if (!SpotOnScreen(l.map, m_mapW, m_mapH, m_spots[i].first, m_spots[i].second, sx, sy))
                continue;
            if (std::abs(mx - sx) <= 14 && std::abs(my - sy) <= 14 && SlotAtSpawn(*lobby, static_cast<int>(i)) < 0
                && !((lobby->blockedMask >> i) & 1u))
            {
                next.spawn = static_cast<uint8_t>(i);
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
        ui::DrawCaption(font, r, l.players.x, l.players.y - 22, _("PLAYERS"));
        // Column captions: a row is a start position of the map; they sit over the same cells as the rows
        {
            const char *names[6] = {_("Place"), _("Faction"), _("Team"), _("Colour"), _("Handicap"), _("Army")};
            for (int k = 0; k < 6; ++k)
            {
                const UiRect c = l.Col(0, k);
                font.DrawString(
                    ui::FitText(font, names[k], ui::FONT_SMALL, c.w), c.x + 2, l.players.y + 2, ui::FONT_SMALL, col::TextMuted, false);
            }
        }
        auto nameOf = [&](uint8_t slot) {
            if (roster)
            {
                for (const Net::RosterEntry &e : roster->players)
                    if (e.slot == slot)
                        return e.name;
            }
            return std::string("?");
        };
        if (lobby)
        {
            const int rows = std::min<int>(lobby->spawnCount, kMaxPlayers);
            for (int i = 0; i < rows; ++i)
            {
                const UiRect row = l.row(i);
                const int si = SlotAtSpawn(*lobby, i);
                const bool blocked = ((lobby->blockedMask >> i) & 1u) != 0;
                const Net::LobbySlot *s = si >= 0 ? &lobby->slots[si] : nullptr;
                const bool me = s && !s->ai && s->slot == net->LocalSlot();
                const bool editable = s && (me || (s->ai && net->ControlsRoom()));
                ui::DrawPanel(
                    r, row, me ? col::Hover : (blocked ? col::PanelSunken : col::PanelRaised), me ? col::Accent : col::Border);
                font.DrawString(
                    std::to_string(i + 1),
                    row.x + 15,
                    row.CenterY(),
                    ui::FONT_BODY_BOLD,
                    s ? SlotColor(s->choice.color) : col::TextMuted,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
                // The type of the place: a drop-down list when this viewer may change it
                const UiRect tc = l.TypeCell(i);
                const std::vector<SeatOption> options = SeatOptions(*net, *lobby, i);
                const std::string typeText = s ? (s->ai ? std::string(_("AI")) + " - " + AiLevelName(s->aiLevel) : nameOf(s->slot))
                                               : (blocked ? std::string(_("Blocked")) : std::string(_("Open")));
                const SDL_Color typeColor = s ? (s->ai ? col::Info : col::Text) : (blocked ? col::Danger : col::TextDim);
                if (!options.empty())
                {
                    ui::DrawComboField(
                        r,
                        font,
                        tc,
                        ui::FitText(font, typeText, ui::FONT_SMALL, tc.w - 36),
                        m_combo.open == i * 10,
                        tc.Contains(m_mouseX, m_mouseY),
                        true,
                        typeColor);
                }
                else
                {
                    font.DrawString(
                        ui::FitText(font, typeText, ui::FONT_BODY_BOLD, tc.w - 8),
                        tc.x + 8,
                        row.CenterY(),
                        ui::FONT_BODY_BOLD,
                        typeColor,
                        false,
                        Anchor::LEFT | Anchor::VCENTER);
                }
                if (!s)
                    continue;
                auto cell = [&](const UiRect &c, const std::string &text) {
                    ui::DrawPanel(r, c, col::PanelSunken, col::Border);
                    font.DrawString(
                        text, c.CenterX(), c.CenterY(), ui::FONT_SMALL, col::Text, false, Anchor::HCENTER | Anchor::VCENTER);
                };
                auto combo = [&](const UiRect &c, int kind, const std::string &text) {
                    ui::DrawComboField(r, font, c, text, m_combo.open == i * 10 + kind, c.Contains(m_mouseX, m_mouseY));
                };
                Net::LobbyChoice choice = s->choice;
                if (me)
                    net->LocalChoice(choice);
                const UiRect cc = l.ColorCell(i);
                if (editable)
                {
                    combo(l.FactionCell(i), 1, FactionName(choice.faction));
                    combo(cc, 3, "");
                    const UiRect sw {cc.x + 6, cc.y + 6, cc.w - 28, cc.h - 12};
                    r.FillRectScreen(sw.x, sw.y, sw.w, sw.h, SlotColor(choice.color));
                    r.DrawRectScreen(sw.x, sw.y, sw.w, sw.h, col::BorderStrong);
                }
                else
                {
                    cell(l.FactionCell(i), FactionName(choice.faction));
                    ui::DrawPanel(r, cc, col::PanelSunken, col::Border);
                    const UiRect sw {cc.x + 6, cc.y + 6, cc.w - 12, cc.h - 12};
                    r.FillRectScreen(sw.x, sw.y, sw.w, sw.h, SlotColor(choice.color));
                    r.DrawRectScreen(sw.x, sw.y, sw.w, sw.h, col::BorderStrong);
                }
                const std::string form = ComboChoices(7)[std::min<int>(choice.handicap, 5)];
                const std::string cap = ui::ArmyCapChoices()[std::min<int>(choice.armyCap, kArmyCapCount - 1)];
                if (net->ControlsRoom())
                {
                    combo(l.TeamCell(i), 2, std::to_string(choice.team + 1));
                    combo(l.HandicapCell(i), 7, form);
                    combo(l.ArmyCell(i), 8, cap);
                }
                else
                {
                    cell(l.TeamCell(i), std::to_string(choice.team + 1));
                    cell(l.HandicapCell(i), form);
                    cell(l.ArmyCell(i), cap);
                }
                if (!s->ai && choice.ready)
                {
                    font.DrawString(
                        _("READY"),
                        row.Right() - 8,
                        row.CenterY(),
                        ui::FONT_SMALL_BOLD,
                        col::Ok,
                        false,
                        Anchor::RIGHT | Anchor::VCENTER);
                }
            }
        }

        // The list of maps (search, filters), the title of the room's map over the details
        if (m_maps.empty())
            RebuildMapsShown();
        std::string mapName;
        if (lobby)
        {
            for (const SkirmishMapInfo &m : m_maps)
                if (m.path == lobby->map)
                    mapName = m.DisplayName();
            if (mapName.empty() && !lobby->map.empty())
                mapName = lobby->map.substr(lobby->map.find_last_of('/') + 1);
        }
        font.DrawString(
            ui::FitText(font, mapName, ui::FONT_H2, l.timeBtn.x - l.details.x - 40),
            l.details.x + 20,
            l.details.y + 18,
            ui::FONT_H2,
            col::Accent,
            true);
        {
            const bool searching = m_focus == Field::Search;
            DrawField(r, font, l.mapSearch, m_mapFilter.search, _("Search map..."), searching);
            const std::vector<int> counts = ui::MapFilter::PlayerCounts(m_maps);
            const auto cur = std::find(counts.begin(), counts.end(), m_mapFilter.players);
            for (int k = 0; k < 3; ++k)
            {
                const std::vector<std::string> items = MapFilterChoices(kFilterPlayers + k, m_maps);
                const int idx = k == 0 ? (cur == counts.end() ? 0 : static_cast<int>(cur - counts.begin()))
                                       : (k == 1 ? m_mapFilter.size : m_mapFilter.sort);
                ui::DrawComboField(
                    r,
                    font,
                    l.Filter(k),
                    ui::FitText(font, items[std::clamp(idx, 0, static_cast<int>(items.size()) - 1)], ui::FONT_SMALL, l.Filter(k).w - 34),
                    m_combo.open == kFilterPlayers + k,
                    l.Filter(k).Contains(m_mouseX, m_mouseY));
            }
            ui::DrawPanel(r, l.mapList, col::PanelSunken, col::Border);
            const size_t inRoom = lobby ? lobby->slots.size() : 1;
            const int rows = l.MapRows();
            r.SetClipRect({l.mapList.x, l.mapList.y, l.mapList.w, l.mapList.h});
            for (int row = 0; row < rows && m_mapTop + row < static_cast<int>(m_mapsShown.size()); ++row)
            {
                const SkirmishMapInfo &m = m_mapsShown[m_mapTop + row];
                const ui::UiRect it = l.MapItem(row);
                const bool selected = lobby && m.path == lobby->map;
                const bool unfit = MissionController::StartPositions(m.path).size() < inRoom;
                const bool hot = net->ControlsRoom() && it.Contains(m_mouseX, m_mouseY);
                r.FillRectScreen(it.x + 2, it.y, it.w - 4, it.h, selected ? col::Hover : (hot ? col::PanelRaised : col::PanelSunken));
                if (selected)
                    r.FillRectScreen(it.x + 2, it.y, 3, it.h, col::Accent);
                font.DrawString(
                    ui::FitText(font, m.DisplayName(), ui::FONT_BODY, it.w - 60),
                    it.x + 14,
                    it.CenterY(),
                    ui::FONT_BODY,
                    selected ? col::Text : (unfit ? col::TextMuted : col::TextDim),
                    false,
                    Anchor::LEFT | Anchor::VCENTER);
                font.DrawString(
                    std::to_string(m.players),
                    it.Right() - 10,
                    it.CenterY(),
                    ui::FONT_SMALL,
                    col::TextMuted,
                    false,
                    Anchor::RIGHT | Anchor::VCENTER);
            }
            r.ResetClipRect();
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
                const bool blockedSpot = ((lobby->blockedMask >> i) & 1u) != 0;
                r.FillRectScreen(sx - 10, sy - 10, 20, 20, blockedSpot ? SDL_Color {40, 20, 20, 255} : c);
                r.DrawRectScreen(sx - 12, sy - 12, 24, 24, hot ? col::Accent : (blockedSpot ? col::Danger : col::Text));
                if (blockedSpot)
                {
                    r.DrawLineScreen(sx - 9, sy - 9, sx + 9, sy + 9, col::Danger);
                    r.DrawLineScreen(sx - 9, sy + 9, sx + 9, sy - 9, col::Danger);
                }
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
        {
            // The owner edits the options, everybody sees them (the room's values)
            const bool owner = net->ControlsRoom();
            const bool prebuilt = owner ? m_prebuilt : (lobby ? lobby->prebuilt : true);
            const int credits = owner ? m_credits : (lobby ? lobby->credits : kDefaultCreditsIndex);
            const bool fog = owner ? m_fog : (lobby ? lobby->fogOfWar : true);
            const bool night = owner ? m_night : (lobby && lobby->night);
            ui::DrawComboField(
                r,
                font,
                l.timeBtn,
                ui::TimeOfDayChoices()[night ? 1 : 0],
                m_combo.open == kTimeCombo,
                owner && l.timeBtn.Contains(m_mouseX, m_mouseY),
                owner);
            ui::DrawCheckbox(
                r,
                font,
                l.prebuiltBtn,
                _("START WITH A BASE"),
                prebuilt,
                owner && l.prebuiltBtn.Contains(m_mouseX, m_mouseY),
                owner);
            ui::DrawComboField(
                r,
                font,
                l.creditsBtn,
                ui::CreditsLabel(credits),
                m_combo.open == kCreditsCombo,
                owner && l.creditsBtn.Contains(m_mouseX, m_mouseY),
                owner);
            ui::DrawCheckbox(
                r, font, l.fogCheckbox, _("FOG OF WAR"), fog, owner && l.fogCheckbox.Contains(m_mouseX, m_mouseY), owner);
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
            l.fogCheckbox.x,
            l.prebuiltBtn.Bottom() + 10,
            l.fogCheckbox.w,
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
            const int id = m_combo.open, row = id / 10, kind = IsOptionCombo(id) ? 5 : id % 10;
            if (id >= kFilterPlayers && id <= kFilterSort)
            {
                const std::vector<int> counts = ui::MapFilter::PlayerCounts(m_maps);
                const auto cur = std::find(counts.begin(), counts.end(), m_mapFilter.players);
                const int current = id == kFilterPlayers ? (cur == counts.end() ? 0 : static_cast<int>(cur - counts.begin()))
                                    : id == kFilterSize  ? m_mapFilter.size
                                                         : m_mapFilter.sort;
                ui::DrawComboList(
                    r, font, l.Filter(id - kFilterPlayers), MapFilterChoices(id, m_maps), current, m_combo, h, m_mouseX, m_mouseY);
            }
            else if (id == kLayoutCombo)
            {
                ui::DrawComboList(
                    r, font, l.prebuiltBtn, ui::LayoutChoices(), m_prebuilt ? 0 : 1, m_combo, h, m_mouseX, m_mouseY);
            }
            else if (id == kCreditsCombo)
            {
                ui::DrawComboList(r, font, l.creditsBtn, ui::CreditsChoices(), m_credits, m_combo, h, m_mouseX, m_mouseY);
            }
            else if (id == kTimeCombo)
            {
                ui::DrawComboList(
                    r, font, l.timeBtn, ui::TimeOfDayChoices(), m_night ? 1 : 0, m_combo, h, m_mouseX, m_mouseY);
            }
            else if (lobby && kind == 0 && row < std::min<int>(lobby->spawnCount, kMaxPlayers))
            {
                const std::vector<SeatOption> options = SeatOptions(*net, *lobby, row);
                std::vector<std::string> items;
                for (const SeatOption &o : options)
                    items.push_back(o.label);
                ui::DrawComboList(
                    r, font, l.TypeCell(row), items, SeatCurrent(options, *lobby, row), m_combo, h, m_mouseX, m_mouseY);
            }
            else if (lobby && SlotAtSpawn(*lobby, row) >= 0)
            {
                const Net::LobbySlot &s = lobby->slots[SlotAtSpawn(*lobby, row)];
                Net::LobbyChoice sc = s.choice;
                if (!s.ai && s.slot == net->LocalSlot())
                    net->LocalChoice(sc);
                const ui::UiRect field = kind == 1   ? l.FactionCell(row)
                                         : kind == 2 ? l.TeamCell(row)
                                         : kind == 3 ? l.ColorCell(row)
                                         : kind == 7 ? l.HandicapCell(row)
                                                     : l.ArmyCell(row);
                const int current = kind == 1   ? (sc.faction == Net::kRandom ? 0 : sc.faction + 1)
                                    : kind == 2 ? sc.team
                                    : kind == 3 ? std::max(0, sc.color - 1)
                                    : kind == 7 ? std::min<int>(sc.handicap, 5)
                                                : std::min<int>(sc.armyCap, kArmyCapCount - 1);
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
