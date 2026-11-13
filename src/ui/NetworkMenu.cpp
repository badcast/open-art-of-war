#include "ui/UiLayout.hpp"
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

constexpr std::uint16_t kDefaultPort = 38100;
constexpr int kChatLines = 6;

std::string AiLevelName(std::uint8_t level)
{
    static const char *const kNames[4] = {"Easy", "Medium", "Hard", "Hardcore"};
    return _(kNames[level & 3]);
}

std::string FactionName(std::uint8_t faction)
{
    if (faction == 0)
        return _("Resistance");
    if (faction == 1)
        return _("Confederation");
    return _("Random");
}

// The choices of the drop-down lists of the room: kind 1 faction, 2 team, 3 colour, 4 level of a computer player,
// 5 how the map starts
std::vector<std::string> ComboChoices(int kind)
{
    std::vector<std::string> choices;
    if (kind == 1)
        choices = {_("Random"), _("Resistance"), _("Confederation")};
    else if (kind == 2)
        for (int i = 1; i <= kMaxPlayers; ++i)
            choices.push_back(std::string(_("Team")) + " " + std::to_string(i));
    else if (kind == 3)
        for (int i = 1; i <= Settings::kArmyColors; ++i)
            choices.push_back(_(ArmyColorName(i)));
    else if (kind == 4)
        for (int i = 0; i < 4; ++i)
            choices.push_back(AiLevelName(static_cast<std::uint8_t>(i)));
    else if (kind == 7)
    {
        choices = {_("None"), "-25%", "-50%", "+25%", "+50%", "+100%"}; // the start credits form, see kHandicapPercent
    }
    else if (kind == 8)
        choices = ui::ArmyCapChoices();
    else if (kind == 9)
        choices = ui::TechLevelChoices(true); // (choice n = the room's value n)
    return choices;
}

// The three filter lists over the list of maps (the same choices as in the custom game window)
std::vector<std::string> MapFilterChoices(int filterId, const std::vector<SkirmishMapInfo> &allMaps)
{
    std::vector<std::string> choices;
    if (filterId == NetworkMenu::kFilterPlayers)
    {
        for (int count : ui::MapFilter::PlayerCounts(allMaps))
            choices.push_back(
                std::string(_("Players")) + ": " + (count == 0 ? std::string(_("Any")) : std::to_string(count)));
    }
    else if (filterId == NetworkMenu::kFilterSize)
    {
        for (const char *label : {_("Any"), _("Small"), _("Medium"), _("Large")})
            choices.push_back(std::string(_("Size")) + ": " + label);
    }
    else
    {
        for (const char *label : {_("Name"), _("Players"), _("Size")})
            choices.push_back(std::string(_("Sort")) + ": " + label);
    }
    return choices;
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
    for (std::size_t i = 0; i < lobby.slots.size(); ++i)
        if (lobby.slots[i].choice.spawn == position)
            return static_cast<int>(i);
    return -1;
}

std::vector<SeatOption> SeatOptions(const NetMatch &net, const Net::LobbyState &lobby, int position)
{
    std::vector<SeatOption> options;
    const int held = SlotAtSpawn(lobby, position);
    const bool blocked = ((lobby.blockedMask >> position) & 1u) != 0;
    const bool mine = held >= 0 && !lobby.slots[held].ai && lobby.slots[held].slot == net.LocalSlot();
    if (mine)
        return options; // your own place: nothing to choose
    const bool free = held < 0 && !blocked;
    if (net.ControlsRoom())
    {
        if (free)
            options.push_back({_("Occupy"), kSeatOccupy});
        options.push_back({_("Open"), kSeatOpen});
        for (int level = 0; level < 4; ++level)
            options.push_back(
                {std::string(_("AI")) + " - " + AiLevelName(static_cast<std::uint8_t>(level)), kSeatAi + level});
        options.push_back({_("Blocked"), kSeatBlocked});
    }
    else if (free)
    {
        options.push_back({_("Open"), kSeatOpen});
        options.push_back({_("Occupy"), kSeatOccupy});
    }
    return options;
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
    for (std::size_t i = 0; i < options.size(); ++i)
        if (options[i].code == code)
            return static_cast<int>(i);
    return -1;
}

// Team colours (index 0 = colour 1) other players of the room already have: nobody may pick them again
std::vector<bool> TakenColors(const Net::LobbyState &lobby, std::uint8_t slot)
{
    std::vector<bool> taken(Settings::kArmyColors, false);
    for (const Net::LobbySlot &otherSlot : lobby.slots)
    {
        if (otherSlot.slot != slot && otherSlot.choice.color >= 1 && otherSlot.choice.color <= Settings::kArmyColors)
            taken[otherSlot.choice.color - 1] = true;
    }
    return taken;
}

SDL_Color SlotColor(std::uint8_t color)
{
    return color == 0 ? col::TextDim : ArmyColor(color);
}

// Removes the last UTF-8 character
void PopUtf8(std::string &text)
{
    while (!text.empty())
    {
        const unsigned char byte = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((byte & 0xC0) != 0x80)
            break;
    }
}

// A small padlock: the room asks for a password
void DrawLock(Renderer &renderer, int x, int y, SDL_Color color)
{
    renderer.DrawRectScreen(x + 2, y, 7, 6, color);
    renderer.FillRectScreen(x, y + 5, 11, 8, color);
}

void DrawField(
    Renderer &renderer,
    BitmapFontRenderer &font,
    const UiRect &rect,
    const std::string &text,
    const std::string &placeholder,
    bool focused)
{
    ui::DrawPanel(renderer, rect, col::PanelSunken, focused ? col::Accent : col::Border);
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
    net.SetOptions(
        m_fog,
        m_prebuilt,
        static_cast<std::uint8_t>(std::clamp(m_credits, 0, 4)),
        m_night,
        static_cast<std::uint8_t>(std::clamp(m_tech, 0, Net::kTechKeep - 1)));
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

NetworkMenu::BrowserLayout NetworkMenu::browserLayout(int windowW, int windowH) const
{
    // Where things stand is ui/layout.yaml (screen network)
    BrowserLayout layout;
    const bool isLan = m_tab == Tab::Lan;
    const uilayout::Solved solved
        = uilayout::Current().Solve("network", "pc", windowW, windowH, [isLan](const std::string &name) {
              return name == (isLan ? "tab_lan" : "tab_internet");
          });
    layout.dialog = solved.Rect("dialog");
    layout.nameField = solved.Rect("name");
    layout.passwordField = solved.Rect("password");
    layout.addressField = solved.Rect("address");
    layout.directBtn = solved.Rect("connect");
    layout.tabLan = solved.Rect("tab_lan");
    layout.tabInternet = solved.Rect("tab_internet");
    layout.lanRefreshBtn = layout.refreshBtn = solved.Rect("refresh");
    layout.list = solved.Rect("lan_list");
    layout.serverList = solved.Rect("server_list");
    layout.codeField = solved.Rect("code");
    layout.codeBtn = solved.Rect("code_join");
    layout.backBtn = solved.Rect("back");
    layout.joinBtn = solved.Rect("join");
    layout.hostBtn = solved.Rect("host");
    layout.serverBtn = solved.Rect("server_create");
    return layout;
}

NetworkMenu::LobbyLayout NetworkMenu::lobbyLayout(int windowW, int windowH)
{
    LobbyLayout layout;
    // ui/layout.yaml (screen map_select, shared with the campaign and the custom game); the numbers below stand in
    // without it
    if (uilayout::Current().Has("map_select", "pc"))
    {
        const uilayout::Solved solved = uilayout::Current().Solve(
            "map_select", uilayout::MenuProfile(uilayout::Current(), "map_select"), windowW, windowH, {});
        layout.dialog = solved.Rect("dialog");
        layout.mapSearch = solved.Rect("room_search");
        layout.mapList = solved.Rect("room_list");
        layout.chatField = solved.Rect("room_chat_field");
        layout.chatSend = solved.Rect("room_chat_send");
        layout.chatLog = solved.Rect("room_chat_log");
        layout.details = solved.Rect("details");
        const ui::UiRect &detailsRect = layout.details;
        const int rowHeight = std::clamp((detailsRect.h - 74 - 24 - 150 - 30) / 8, 30, 40);
        layout.rowH = rowHeight;
        layout.players = {detailsRect.x + 20, detailsRect.y + 74, detailsRect.w - 40, 24 + 8 * rowHeight};
        const int belowHeight = detailsRect.Bottom() - 16 - (layout.players.Bottom() + 12);
        const int mapSize = std::clamp(std::min(belowHeight, static_cast<int>(detailsRect.w * 0.45f)), 120, 460);
        layout.map = {detailsRect.x + 20, layout.players.Bottom() + 12, mapSize, mapSize};
        const int optionsX = layout.map.Right() + 20;
        const int optionsW = detailsRect.Right() - 20 - optionsX;
        layout.creditsBtn = solved.Rect("credits");
        layout.timeBtn = solved.Rect("time");
        layout.techBtn = solved.Rect("tech");
        layout.fogCheckbox = {optionsX, layout.map.y, optionsW, 28};
        layout.prebuiltBtn = {optionsX, layout.map.y + 32, optionsW, 28};
        layout.leaveBtn = solved.Rect("room_leave");
        layout.startBtn = solved.Rect("room_start");
        layout.readyBtn = solved.Rect("room_ready");
        return layout;
    }
    layout.dialog = ui::CenteredRect(windowW, windowH, windowW, windowH); // the whole window
    const int contentX = layout.dialog.x + 20;
    const int contentY = layout.dialog.y + 52 + 16;
    const int footerY = layout.dialog.Bottom() - 64;
    const int columnH = footerY - contentY - 8;
    const int leftWidth = std::clamp(static_cast<int>(layout.dialog.w * 0.30f), 330, 480);
    // Left: search and filters, the list of maps, the chat
    const int chatHeight = std::max(170, static_cast<int>(columnH * 0.36f));
    layout.mapSearch = {contentX, contentY, leftWidth, 30};
    layout.mapList = {contentX, contentY + 72, leftWidth, columnH - 72 - chatHeight - 10};
    layout.chatField = {contentX, contentY + columnH - 36, leftWidth - 52, 36};
    layout.chatSend = {layout.chatField.Right() + 8, layout.chatField.y, 44, 36};
    layout.chatLog
        = {contentX, layout.mapList.Bottom() + 10, leftWidth, layout.chatField.y - 6 - (layout.mapList.Bottom() + 10)};
    // Right: the details
    layout.details
        = {contentX + leftWidth + 16, contentY, layout.dialog.Right() - 20 - (contentX + leftWidth + 16), columnH};
    const ui::UiRect &detailsRect = layout.details;
    // The seat table takes the whole width of the details (its cells share it); the map picture sits under it on the
    // left, the options and the hint on its right
    const int rowHeight = std::clamp((detailsRect.h - 74 - 24 - 150 - 30) / 8, 30, 40);
    layout.rowH = rowHeight;
    layout.players = {detailsRect.x + 20, detailsRect.y + 74, detailsRect.w - 40, 24 + 8 * rowHeight};
    const int belowHeight = detailsRect.Bottom() - 16 - (layout.players.Bottom() + 12);
    const int mapSize = std::clamp(std::min(belowHeight, static_cast<int>(detailsRect.w * 0.45f)), 120, 460);
    layout.map = {detailsRect.x + 20, layout.players.Bottom() + 12, mapSize, mapSize};
    const int optionsX = layout.map.Right() + 20;
    const int optionsW = detailsRect.Right() - 20 - optionsX;
    // Match options: the time of day and the credits next to the title, the check boxes beside the picture
    layout.creditsBtn = {detailsRect.Right() - 20 - 200, detailsRect.y + 12, 200, 32};
    layout.timeBtn = {layout.creditsBtn.x - 12 - 160, detailsRect.y + 12, 160, 32};
    layout.techBtn = {layout.timeBtn.x - 12 - 180, detailsRect.y + 12, 180, 32};
    layout.fogCheckbox = {optionsX, layout.map.y, optionsW, 28};
    layout.prebuiltBtn = {optionsX, layout.map.y + 32, optionsW, 28}; // a check box: start with a base
    // Footer: Leave on the left, Ready and Start on the right - never on top of each other
    layout.leaveBtn = {contentX, footerY + 10, 140, 40};
    layout.startBtn = {layout.dialog.Right() - 20 - 200, footerY + 10, 200, 40};
    layout.readyBtn = {layout.startBtn.x - 12 - 200, footerY + 10, 200, 40};
    return layout;
}

void NetworkMenu::RebuildMapsShown()
{
    if (m_maps.empty())
        m_maps = MissionController::ListSkirmishMaps();
    m_mapsShown = m_mapFilter.Apply(m_maps);
    m_mapTop = 0;
}

void NetworkMenu::EditText(InputManager &input, std::string &text, std::size_t maxLength)
{
    for (int i = input.TakeBackspaces(); i > 0; --i)
        PopUtf8(text);
    text += input.TakeTypedText();
    while (text.size() > maxLength)
        PopUtf8(text);
}

bool NetworkMenu::ParseAddress(const std::string &text)
{
    std::string address = text;
    address.erase(std::remove(address.begin(), address.end(), ' '), address.end());
    if (address.empty())
        return false;
    m_joinPort = kDefaultPort;
    const std::size_t colonPos = address.rfind(':');
    if (colonPos != std::string::npos && address.find(':') == colonPos)
    { // one colon: host:port (IPv6 is not supported)
        const std::string portStr = address.substr(colonPos + 1);
        if (portStr.empty() || portStr.size() > 5 || !std::all_of(portStr.begin(), portStr.end(), ::isdigit))
            return false;
        const int portValue = std::stoi(portStr);
        if (portValue <= 0 || portValue > 65535)
            return false;
        m_joinPort = static_cast<std::uint16_t>(portValue);
        address.resize(colonPos);
    }
    m_joinAddress = address;
    return !address.empty();
}

bool NetworkMenu::FocusedFieldRect(int windowW, int windowH, bool inRoom, SDL_Rect &out) const
{
    ui::UiRect fieldRect;
    if (inRoom)
    {
        const LobbyLayout layout = lobbyLayout(windowW, windowH);
        if (m_focus == Field::Chat)
            fieldRect = layout.chatField;
        else if (m_focus == Field::Search)
            fieldRect = layout.mapSearch;
    }
    else
    {
        const BrowserLayout layout = browserLayout(windowW, windowH);
        if (m_focus == Field::Name)
            fieldRect = layout.nameField;
        else if (m_focus == Field::Password)
            fieldRect = layout.passwordField;
        else if (m_focus == Field::Address)
            fieldRect = layout.addressField;
        else if (m_focus == Field::Code)
            fieldRect = layout.codeField;
    }
    if (fieldRect.w <= 0)
        return false;
    out = {fieldRect.x, fieldRect.y, fieldRect.w, fieldRect.h};
    return true;
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
        float clickX, clickY;
        const BrowserLayout layout = browserLayout(windowW, windowH);
        if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE)
            || (input.WasLeftClickIssued(clickX, clickY) && layout.backBtn.Contains(m_mouseX, m_mouseY)))
        {
            return NetworkAction::Leave;
        }
        return NetworkAction::None;
    }
    return UpdateBrowser(input, windowW, windowH, settings, lan, server);
}

NetworkAction NetworkMenu::UpdateBrowser(
    InputManager &input,
    int windowW,
    int windowH,
    Settings &settings,
    const std::vector<LanGame> &lan,
    const std::vector<LanGame> &server)
{
    const BrowserLayout layout = browserLayout(windowW, windowH);
    float clickX, clickY;
    const bool clicked = input.WasLeftClickIssued(clickX, clickY);
    const int mouseX = m_mouseX, mouseY = m_mouseY;

    if (m_focus == Field::Name)
        EditText(input, settings.playerName, 24);
    if (m_focus == Field::Password)
        EditText(input, m_password, 24);
    if (m_focus == Field::Address)
        EditText(input, settings.lastAddress, 120);
    if (m_focus == Field::Code)
    {
        EditText(input, m_joinCode, 6);
        for (char &ch : m_joinCode)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
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
        if (m_tab == Tab::Lan && m_lanSelected >= 0 && m_lanSelected < static_cast<int>(lan.size()))
        {
            m_joinAddress = lan[m_lanSelected].address;
            m_joinPort = lan[m_lanSelected].port;
            return NetworkAction::Join;
        }
        return joinDirect();
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
    if (layout.nameField.Contains(mouseX, mouseY))
        m_focus = Field::Name;
    else if (layout.passwordField.Contains(mouseX, mouseY))
        m_focus = Field::Password;
    if (layout.tabLan.Contains(mouseX, mouseY))
        m_tab = Tab::Lan;
    if (layout.tabInternet.Contains(mouseX, mouseY))
        m_tab = Tab::Internet;
    if (layout.addressField.Contains(mouseX, mouseY))
        m_focus = Field::Address;
    if (layout.codeField.Contains(mouseX, mouseY))
        m_focus = Field::Code;
    if (layout.directBtn.Contains(mouseX, mouseY) && !settings.lastAddress.empty())
    {
        m_notice.clear();
        return joinDirect();
    }
    if (layout.codeBtn.Contains(mouseX, mouseY))
        return joinCode();
    if (layout.refreshBtn.Contains(mouseX, mouseY))
    {
        if (m_tab == Tab::Internet)
            return NetworkAction::ServerRefresh;
        m_lanSelected = -1;
        return NetworkAction::LanRefresh;
    }
    const int serverRows = std::max(1, (layout.serverList.h - 8) / 34);
    for (int i = 0; m_tab == Tab::Internet && i < static_cast<int>(server.size()) && i < serverRows; ++i)
    {
        if (!layout.ServerRow(i).Contains(mouseX, mouseY))
            continue;
        if (i == m_serverSelected)
            return join(); // a second click joins
        m_serverSelected = i;
        m_lanSelected = -1;
    }
    if (layout.serverBtn.Contains(mouseX, mouseY))
    {
        m_notice.clear();
        return NetworkAction::ServerCreate;
    }
    if (layout.list.Contains(mouseX, mouseY))
    {
        const int row = (mouseY - layout.list.y - 4) / 40;
        if (row >= 0 && row < static_cast<int>(lan.size()) && row < std::max(1, (layout.list.h - 8) / 40))
        {
            if (row == m_lanSelected)
                return join(); // a second click joins
            m_lanSelected = row;
            m_serverSelected = -1;
        }
    }
    if (layout.backBtn.Contains(mouseX, mouseY))
        return NetworkAction::Back;
    if (layout.hostBtn.Contains(mouseX, mouseY))
    {
        m_notice.clear();
        return NetworkAction::Host;
    }
    if (layout.joinBtn.Contains(mouseX, mouseY))
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

bool NetworkMenu::SpotOnScreen(
    const UiRect &area, int mapW, int mapH, int tileX, int tileY, int &screenX, int &screenY) const
{
    if (mapW <= 0 || mapH <= 0)
        return false;
    const float scale = std::min(static_cast<float>(area.w) / mapW, static_cast<float>(area.h) / mapH);
    const int originX = area.x + (area.w - static_cast<int>(mapW * scale)) / 2;
    const int originY = area.y + (area.h - static_cast<int>(mapH * scale)) / 2;
    // The base is set around the spot: show its middle
    screenX = std::clamp(originX + static_cast<int>((tileX + 5) * scale), area.x + 14, area.Right() - 14);
    screenY = std::clamp(originY + static_cast<int>((tileY + 5) * scale), area.y + 14, area.Bottom() - 14);
    return true;
}

NetworkAction NetworkMenu::UpdateLobby(InputManager &input, int windowW, int windowH, NetMatch &net)
{
    const LobbyLayout layout = lobbyLayout(windowW, windowH);
    const Net::LobbyState *lobby = net.Lobby();
    float clickX, clickY;
    const bool clicked = input.WasLeftClickIssued(clickX, clickY);
    const int mouseX = m_mouseX, mouseY = m_mouseY;

    for (Net::Chat &chatMsg : net.TakeChat())
    {
        std::string who = "?";
        if (const Net::Roster *roster = net.roster())
        {
            for (const Net::RosterEntry &entry : roster->players)
            {
                if (entry.slot == chatMsg.fromSlot)
                    who = entry.name;
            }
        }
        m_chatLog.push_back(who + ": " + chatMsg.text);
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
    if (lobby && !net.ControlsRoom())
    {
        m_fog = lobby->fogOfWar;
        m_prebuilt = lobby->prebuilt;
        m_credits = lobby->credits;
        m_night = lobby->night;
        m_tech = lobby->techLevel;
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
    if (lobby)
    {
        for (std::size_t i = 0; i < lobby->slots.size(); ++i)
        {
            if (lobby->slots[i].slot == net.LocalSlot())
            {
                mine = lobby->slots[i].choice;
                net.LocalChoice(mine); // what was last sent, not the room's lagging echo
                haveMine = true;
            }
        }
    }

    // The question "leave the room?" takes everything while it is open
    if (m_confirm.open)
    {
        const int answer = ui::ConfirmUpdate(
            m_confirm,
            windowW,
            windowH,
            mouseX,
            mouseY,
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
        m_confirm.modalResult = -1;
        SoundRequests::Play("ui_show_dialog");
        m_confirm.title = _("LEAVE THE ROOM?");
        m_confirm.text = net.ControlsRoom()
                             ? _("You are the room owner. If you leave, the game may end for everybody in it.")
                             : _("You leave the room. You can join it again while it is open.");
    };
    // An open drop-down list takes the mouse first; a pick changes the value, a click elsewhere closes the list
    auto comboField = [&](int comboId) -> ui::UiRect {
        if (comboId == kCreditsCombo)
            return layout.creditsBtn;
        if (comboId == kTimeCombo)
            return layout.timeBtn;
        if (comboId == kTechCombo)
            return layout.techBtn;
        if (comboId >= kFilterPlayers && comboId <= kFilterSort)
            return layout.Filter(comboId - kFilterPlayers);
        const int row = comboId / 10, kind = comboId % 10;
        return kind == 1   ? layout.FactionCell(row)
               : kind == 2 ? layout.TeamCell(row)
               : kind == 3 ? layout.ColorCell(row)
               : kind == 7 ? layout.HandicapCell(row)
               : kind == 8 ? layout.ArmyCell(row)
               : kind == 9 ? layout.TechCell(row)
                           : layout.TypeCell(row);
    };
    // The filter lists over the maps work for everybody in the room
    if (m_combo.open >= kFilterPlayers && m_combo.open <= kFilterSort)
    {
        const int comboId = m_combo.open;
        int picked = -1;
        if (ui::ComboUpdate(
                m_combo,
                comboField(comboId),
                static_cast<int>(MapFilterChoices(comboId, m_maps).size()),
                windowH,
                mouseX,
                mouseY,
                clicked,
                input.ConsumeWheel(),
                picked))
        {
            if (picked >= 0)
            {
                if (comboId == kFilterPlayers)
                {
                    const std::vector<int> counts = ui::MapFilter::PlayerCounts(m_maps);
                    m_mapFilter.players = counts[std::clamp(picked, 0, static_cast<int>(counts.size()) - 1)];
                }
                else if (comboId == kFilterSize)
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
        const int comboId = m_combo.open, row = comboId / 10, kind = comboId % 10;
        const bool isMapList = false;
        const bool valid = optionList || isMapList ? net.ControlsRoom()
                                                   : (lobby && row < std::min<int>(lobby->spawnCount, kMaxPlayers));
        if (!valid)
        {
            m_combo.open = -1;
            return NetworkAction::None;
        }
        else
        {
            int picked = -1;
            if (ui::ComboUpdate(
                    m_combo,
                    comboField(comboId),
                    isMapList                  ? static_cast<int>(m_maps.size())
                    : comboId == kTimeCombo    ? static_cast<int>(ui::TimeOfDayChoices().size())
                    : comboId == kTechCombo    ? static_cast<int>(ui::TechLevelChoices(false).size())
                    : comboId == kCreditsCombo ? static_cast<int>(ui::CreditsChoices().size())
                    : kind == 0                ? static_cast<int>(SeatOptions(net, *lobby, row).size())
                                               : static_cast<int>(ComboChoices(kind).size()),
                    windowH,
                    mouseX,
                    mouseY,
                    clicked,
                    input.ConsumeWheel(),
                    picked))
            {
                if (picked >= 0 && comboId == kCreditsCombo)
                {
                    m_credits = std::clamp(picked, 0, 4);
                    PushOptions(net);
                }
                else if (picked >= 0 && comboId == kTimeCombo)
                {
                    m_night = picked == 1;
                    PushOptions(net);
                }
                else if (picked >= 0 && comboId == kTechCombo)
                {
                    m_tech = picked; // (choice n = the room's value n)
                    PushOptions(net);
                }
                else if (picked >= 0 && kind == 0)
                {
                    // What the start position holds: a seat for the player himself, or the owner's decision
                    const std::vector<SeatOption> options = SeatOptions(net, *lobby, row);
                    if (picked >= static_cast<int>(options.size()))
                        return NetworkAction::None;
                    const int code = options[picked].code;
                    Net::LobbyChoice ownChoice;
                    if (code == kSeatOccupy)
                    {
                        if (net.LocalChoice(ownChoice))
                        {
                            ownChoice.spawn = static_cast<std::uint8_t>(row);
                            ownChoice.ready = false;
                            net.SetChoice(ownChoice);
                        }
                    }
                    else if (code != SeatCurrent(options, *lobby, row) || SlotAtSpawn(*lobby, row) >= 0)
                    {
                        Net::OwnerSetSeat seat;
                        seat.spawn = static_cast<std::uint8_t>(row);
                        seat.kind = code == kSeatOpen      ? Net::OwnerSetSeat::Open
                                    : code == kSeatBlocked ? Net::OwnerSetSeat::Blocked
                                                           : Net::OwnerSetSeat::Ai;
                        seat.level
                            = code >= kSeatAi && code < kSeatBlocked ? static_cast<std::uint8_t>(code - kSeatAi) : 1;
                        net.SetSeat(seat);
                    }
                }
                else if (picked >= 0)
                {
                    const int slotIndex = SlotAtSpawn(*lobby, row);
                    if (slotIndex < 0)
                        return NetworkAction::None;
                    const Net::LobbySlot &lobbySlot = lobby->slots[slotIndex];
                    Net::LobbyChoice choice = lobbySlot.choice;
                    if (!lobbySlot.ai && lobbySlot.slot == net.LocalSlot())
                        net.LocalChoice(choice); // edit what was last sent, not the room's lagging echo
                    if (kind == 1)
                    {
                        choice.faction = picked == 0 ? Net::kRandom : static_cast<std::uint8_t>(picked - 1);
                        choice.ready = false;
                    }
                    if (kind == 2)
                    {
                        choice.team = static_cast<std::uint8_t>(picked);
                        // Teams are the host's decision: a player's team goes through the room directly
                        if (!lobbySlot.ai)
                        {
                            if (net.ControlsRoom())
                                net.SetTeam(
                                    lobbySlot.slot, choice.team, choice.handicap, choice.armyCap, choice.techLevel);
                            return NetworkAction::None;
                        }
                    }
                    if (kind == 3 && TakenColors(*lobby, lobbySlot.slot)[static_cast<std::size_t>(picked)]
                        && picked + 1 != choice.color)
                        return NetworkAction::None; // somebody else's colour: nothing changes
                    if (kind == 3)
                    {
                        choice.color = static_cast<std::uint8_t>(picked + 1);
                        choice.ready = false; // like faction and team: the owner never starts with an old colour
                    }
                    if (kind == 7 || kind == 8 || kind == 9)
                    {
                        // The form (start credits), the army size limit and the tech level are the owner's decision,
                        // for every player
                        if (kind == 7)
                            choice.handicap = static_cast<std::uint8_t>(picked);
                        else if (kind == 8)
                            choice.armyCap = static_cast<std::uint8_t>(picked);
                        else
                            choice.techLevel = static_cast<std::uint8_t>(std::clamp(picked, 0, Net::kTechKeep - 1));
                        if (!lobbySlot.ai)
                        {
                            if (net.ControlsRoom())
                                net.SetTeam(
                                    lobbySlot.slot, choice.team, choice.handicap, choice.armyCap, choice.techLevel);
                            return NetworkAction::None;
                        }
                    }
                    if (lobbySlot.ai && net.ControlsRoom())
                    {
                        Net::OwnerSetAi change;
                        change.slot = lobbySlot.slot;
                        change.level = lobbySlot.aiLevel;
                        change.choice = choice;
                        net.SetAi(change);
                    }
                    else if (!lobbySlot.ai && lobbySlot.slot == net.LocalSlot())
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
    auto openCombo = [&](int comboId) {
        m_combo.open = comboId;
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
    if (m_combo.open < 0 && layout.mapList.Contains(mouseX, mouseY))
        m_mapTop = std::clamp(
            m_mapTop - input.ConsumeWheel(), 0, std::max(0, static_cast<int>(m_mapsShown.size()) - layout.MapRows()));
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

    m_focus = layout.chatField.Contains(mouseX, mouseY)
                  ? Field::Chat
                  : (layout.mapSearch.Contains(mouseX, mouseY) ? Field::Search : Field::None);
    if (m_focus == Field::Search)
        return NetworkAction::None;
    if (layout.chatSend.Contains(mouseX, mouseY))
    {
        sendChat();
        m_focus = Field::Chat; // keep typing
        return NetworkAction::None;
    }
    if (layout.leaveBtn.Contains(mouseX, mouseY))
    {
        askLeave();
        return NetworkAction::None;
    }
    if (layout.startBtn.Contains(mouseX, mouseY) && net.ControlsRoom() && net.CanStart())
        return NetworkAction::Start;
    if (net.ControlsRoom() && layout.timeBtn.Contains(mouseX, mouseY))
        return openCombo(kTimeCombo);
    if (net.ControlsRoom() && layout.techBtn.Contains(mouseX, mouseY))
        return openCombo(kTechCombo);
    if (net.ControlsRoom() && layout.creditsBtn.Contains(mouseX, mouseY))
        return openCombo(kCreditsCombo);
    if (net.ControlsRoom() && layout.fogCheckbox.Contains(mouseX, mouseY))
    {
        m_fog = !m_fog;
        SoundRequests::Play("ui_confirm");
        PushOptions(net);
        return NetworkAction::None;
    }
    if (net.ControlsRoom() && layout.prebuiltBtn.Contains(mouseX, mouseY))
    {
        m_prebuilt = !m_prebuilt;
        PushOptions(net);
        SoundRequests::Play("ui_confirm");
        return NetworkAction::None;
    }
    // The filters over the maps (everybody may look through the list) and the list: only the owner picks the map
    for (int k = 0; k < 3; ++k)
        if (layout.Filter(k).Contains(mouseX, mouseY))
            return openCombo(kFilterPlayers + k);
    if (net.ControlsRoom() && layout.mapList.Contains(mouseX, mouseY))
    {
        const int mapIdx = m_mapTop + (mouseY - layout.mapList.y) / layout.listRowH;
        if (mapIdx >= 0 && mapIdx < static_cast<int>(m_mapsShown.size()))
        {
            // Only a map with a start position for everybody in the room
            const std::size_t inRoom = lobby ? lobby->slots.size() : 1;
            if (MissionController::StartPositions(m_mapsShown[mapIdx].path).size() >= inRoom)
            {
                for (std::size_t k = 0; k < m_maps.size(); ++k)
                    if (m_maps[k].path == m_mapsShown[mapIdx].path)
                        m_mapIndex = static_cast<int>(k);
                net.SetLobbyMap(m_mapsShown[mapIdx].path);
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
            if (!SeatOptions(net, *lobby, i).empty() && layout.TypeCell(i).Contains(mouseX, mouseY))
                return openCombo(i * 10);
            const int slotIndex = SlotAtSpawn(*lobby, i);
            if (slotIndex < 0)
                continue;
            const Net::LobbySlot &lobbySlot = lobby->slots[slotIndex];
            const bool editable = lobbySlot.ai ? net.ControlsRoom() : lobbySlot.slot == net.LocalSlot();
            if (editable && layout.FactionCell(i).Contains(mouseX, mouseY))
                return openCombo(i * 10 + 1);
            if (net.ControlsRoom() && layout.TeamCell(i).Contains(mouseX, mouseY))
                return openCombo(i * 10 + 2);
            if (editable && layout.ColorCell(i).Contains(mouseX, mouseY))
                return openCombo(i * 10 + 3);
            if (net.ControlsRoom() && layout.HandicapCell(i).Contains(mouseX, mouseY))
                return openCombo(i * 10 + 7);
            if (net.ControlsRoom() && layout.ArmyCell(i).Contains(mouseX, mouseY))
                return openCombo(i * 10 + 8);
            if (net.ControlsRoom() && layout.TechCell(i).Contains(mouseX, mouseY))
                return openCombo(i * 10 + 9);
        }
    }
    if (!haveMine)
        return NetworkAction::None;

    Net::LobbyChoice next = mine;
    if (layout.readyBtn.Contains(mouseX, mouseY) && net.MapReady())
        next.ready = !mine.ready;
    // An open start position on the map: take it
    if (layout.map.Contains(mouseX, mouseY) && lobby)
    {
        for (std::size_t i = 0; i < m_spots.size(); ++i)
        {
            int spotX = 0, spotY = 0;
            if (!SpotOnScreen(layout.map, m_mapW, m_mapH, m_spots[i].first, m_spots[i].second, spotX, spotY))
                continue;
            if (std::abs(mouseX - spotX) <= 14 && std::abs(mouseY - spotY) <= 14
                && SlotAtSpawn(*lobby, static_cast<int>(i)) < 0 && !((lobby->blockedMask >> i) & 1u))
            {
                next.spawn = static_cast<std::uint8_t>(i);
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

void NetworkMenu::Render(
    Renderer &renderer,
    BitmapFontRenderer &font,
    TextureManager &textures,
    int windowW,
    int windowH,
    const NetMatch *net,
    const Settings &settings,
    const std::vector<LanGame> &lan,
    const std::vector<LanGame> &server,
    const std::string &serverStatus)
{
    renderer.FillRectScreen(0, 0, windowW, windowH, col::Scrim);
    if (net && net->connected())
    {
        // ----- Room -----
        const LobbyLayout layout = lobbyLayout(windowW, windowH);
        const Net::LobbyState *lobby = net->Lobby();
        const Net::Roster *roster = net->roster();
        std::string title = net->ControlsRoom() ? _("YOUR GAME") : _("GAME ROOM");
        if (!net->PublicCode().empty())
            title += std::string("  -  ") + _("code") + " " + net->PublicCode();
        const UiRect content = ui::DrawDialog(renderer, font, windowW, windowH, layout.dialog, title);
        (void) content;
        ui::DrawCaption(font, renderer, layout.players.x, layout.players.y - 22, _("PLAYERS"));
        // Column captions: a row is a start position of the map; they sit over the same cells as the rows
        {
            const char *names[7]
                = {_("Place"), _("Faction"), _("Team"), _("Colour"), _("Handicap"), _("Army"), _("Tech")};
            for (int k = 0; k < 7; ++k)
            {
                const UiRect colRect = layout.Col(0, k);
                font.DrawString(
                    ui::FitText(font, names[k], ui::FONT_SMALL, colRect.w),
                    colRect.x + 2,
                    layout.players.y + 2,
                    ui::FONT_SMALL,
                    col::TextMuted,
                    false);
            }
        }
        auto nameOf = [&](std::uint8_t slot) {
            if (roster)
            {
                for (const Net::RosterEntry &entry : roster->players)
                    if (entry.slot == slot)
                        return entry.name;
            }
            return std::string("?");
        };
        if (lobby)
        {
            const int rows = std::min<int>(lobby->spawnCount, kMaxPlayers);
            for (int i = 0; i < rows; ++i)
            {
                const UiRect row = layout.row(i);
                const int slotIndex = SlotAtSpawn(*lobby, i);
                const bool blocked = ((lobby->blockedMask >> i) & 1u) != 0;
                const Net::LobbySlot *lobbySlot = slotIndex >= 0 ? &lobby->slots[slotIndex] : nullptr;
                const bool isLocal = lobbySlot && !lobbySlot->ai && lobbySlot->slot == net->LocalSlot();
                const bool editable = lobbySlot && (isLocal || (lobbySlot->ai && net->ControlsRoom()));
                ui::DrawPanel(
                    renderer,
                    row,
                    isLocal ? col::Hover : (blocked ? col::PanelSunken : col::PanelRaised),
                    isLocal ? col::Accent : col::Border);
                font.DrawString(
                    std::to_string(i + 1),
                    row.x + 15,
                    row.CenterY(),
                    ui::FONT_BODY_BOLD,
                    lobbySlot ? SlotColor(lobbySlot->choice.color) : col::TextMuted,
                    false,
                    Anchor::HCENTER | Anchor::VCENTER);
                // The type of the place: a drop-down list when this viewer may change it
                const UiRect typeCell = layout.TypeCell(i);
                const std::vector<SeatOption> options = SeatOptions(*net, *lobby, i);
                const std::string typeText
                    = lobbySlot ? (lobbySlot->ai ? std::string(_("AI")) + " - " + AiLevelName(lobbySlot->aiLevel)
                                                 : nameOf(lobbySlot->slot))
                                : (blocked ? std::string(_("Blocked")) : std::string(_("Open")));
                const SDL_Color typeColor
                    = lobbySlot ? (lobbySlot->ai ? col::Info : col::Text) : (blocked ? col::Danger : col::TextDim);
                if (!options.empty())
                {
                    ui::DrawComboField(
                        renderer,
                        font,
                        typeCell,
                        ui::FitText(font, typeText, ui::FONT_SMALL, typeCell.w - 36),
                        m_combo.open == i * 10,
                        typeCell.Contains(m_mouseX, m_mouseY),
                        true,
                        typeColor);
                }
                else
                {
                    font.DrawString(
                        ui::FitText(font, typeText, ui::FONT_BODY_BOLD, typeCell.w - 8),
                        typeCell.x + 8,
                        row.CenterY(),
                        ui::FONT_BODY_BOLD,
                        typeColor,
                        false,
                        Anchor::LEFT | Anchor::VCENTER);
                }
                if (!lobbySlot)
                    continue;
                auto cell = [&](const UiRect &cellRect, const std::string &text) {
                    ui::DrawPanel(renderer, cellRect, col::PanelSunken, col::Border);
                    font.DrawString(
                        text,
                        cellRect.CenterX(),
                        cellRect.CenterY(),
                        ui::FONT_SMALL,
                        col::Text,
                        false,
                        Anchor::HCENTER | Anchor::VCENTER);
                };
                auto combo = [&](const UiRect &comboRect, int kind, const std::string &text) {
                    ui::DrawComboField(
                        renderer,
                        font,
                        comboRect,
                        text,
                        m_combo.open == i * 10 + kind,
                        m_combo.open < 0 && comboRect.Contains(m_mouseX, m_mouseY));
                };
                Net::LobbyChoice choice = lobbySlot->choice;
                if (isLocal)
                    net->LocalChoice(choice);
                const UiRect colorCell = layout.ColorCell(i);
                if (editable)
                {
                    combo(layout.FactionCell(i), 1, FactionName(choice.faction));
                    combo(colorCell, 3, "");
                    const UiRect swatchRect {colorCell.x + 6, colorCell.y + 6, colorCell.w - 28, colorCell.h - 12};
                    renderer.FillRectScreen(
                        swatchRect.x, swatchRect.y, swatchRect.w, swatchRect.h, SlotColor(choice.color));
                    renderer.DrawRectScreen(swatchRect.x, swatchRect.y, swatchRect.w, swatchRect.h, col::BorderStrong);
                }
                else
                {
                    cell(layout.FactionCell(i), FactionName(choice.faction));
                    ui::DrawPanel(renderer, colorCell, col::PanelSunken, col::Border);
                    const UiRect swatchRect {colorCell.x + 6, colorCell.y + 6, colorCell.w - 12, colorCell.h - 12};
                    renderer.FillRectScreen(
                        swatchRect.x, swatchRect.y, swatchRect.w, swatchRect.h, SlotColor(choice.color));
                    renderer.DrawRectScreen(swatchRect.x, swatchRect.y, swatchRect.w, swatchRect.h, col::BorderStrong);
                }
                const std::string handicapText = ComboChoices(7)[std::min<int>(choice.handicap, 5)];
                const std::string armyCapText = ui::ArmyCapChoices()[std::min<int>(choice.armyCap, kArmyCapCount - 1)];
                const std::string techText = ui::TechLevelLabel(static_cast<int>(choice.techLevel) - 1, true);
                if (net->ControlsRoom())
                {
                    combo(layout.TeamCell(i), 2, std::to_string(choice.team + 1));
                    combo(layout.HandicapCell(i), 7, handicapText);
                    combo(layout.ArmyCell(i), 8, armyCapText);
                    combo(layout.TechCell(i), 9, techText);
                }
                else
                {
                    cell(layout.TeamCell(i), std::to_string(choice.team + 1));
                    cell(layout.HandicapCell(i), handicapText);
                    cell(layout.ArmyCell(i), armyCapText);
                    cell(layout.TechCell(i), techText);
                }
                if (!lobbySlot->ai && choice.ready)
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
            for (const SkirmishMapInfo &mapInfo : m_maps)
                if (mapInfo.path == lobby->map)
                    mapName = mapInfo.DisplayName();
            if (mapName.empty() && !lobby->map.empty())
                mapName = lobby->map.substr(lobby->map.find_last_of('/') + 1);
        }
        font.DrawString(
            ui::FitText(font, mapName, ui::FONT_H2, layout.techBtn.x - layout.details.x - 40),
            layout.details.x + 20,
            layout.details.y + 18,
            ui::FONT_H2,
            col::Accent,
            true);
        {
            const bool searching = m_focus == Field::Search;
            DrawField(renderer, font, layout.mapSearch, m_mapFilter.search, _("Search map..."), searching);
            const std::vector<int> counts = ui::MapFilter::PlayerCounts(m_maps);
            const auto currentIt = std::find(counts.begin(), counts.end(), m_mapFilter.players);
            for (int k = 0; k < 3; ++k)
            {
                const std::vector<std::string> items = MapFilterChoices(kFilterPlayers + k, m_maps);
                const int selectedIdx
                    = k == 0 ? (currentIt == counts.end() ? 0 : static_cast<int>(currentIt - counts.begin()))
                             : (k == 1 ? m_mapFilter.size : m_mapFilter.sort);
                ui::DrawComboField(
                    renderer,
                    font,
                    layout.Filter(k),
                    ui::FitText(
                        font,
                        items[std::clamp(selectedIdx, 0, static_cast<int>(items.size()) - 1)],
                        ui::FONT_SMALL,
                        layout.Filter(k).w - 34),
                    m_combo.open == kFilterPlayers + k,
                    layout.Filter(k).Contains(m_mouseX, m_mouseY));
            }
            ui::DrawPanel(renderer, layout.mapList, col::PanelSunken, col::Border);
            const std::size_t inRoom = lobby ? lobby->slots.size() : 1;
            const int rows = layout.MapRows();
            renderer.SetClipRect({layout.mapList.x, layout.mapList.y, layout.mapList.w, layout.mapList.h});
            for (int row = 0; row < rows && m_mapTop + row < static_cast<int>(m_mapsShown.size()); ++row)
            {
                const SkirmishMapInfo &mapInfo = m_mapsShown[m_mapTop + row];
                const ui::UiRect itemRect = layout.MapItem(row);
                const bool selected = lobby && mapInfo.path == lobby->map;
                const bool unfit = MissionController::StartPositions(mapInfo.path).size() < inRoom;
                const bool hot = net->ControlsRoom() && itemRect.Contains(m_mouseX, m_mouseY);
                renderer.FillRectScreen(
                    itemRect.x + 2,
                    itemRect.y,
                    itemRect.w - 4,
                    itemRect.h,
                    selected ? col::Hover : (hot ? col::PanelRaised : col::PanelSunken));
                if (selected)
                    renderer.FillRectScreen(itemRect.x + 2, itemRect.y, 3, itemRect.h, col::Accent);
                font.DrawString(
                    ui::FitText(font, mapInfo.DisplayName(), ui::FONT_BODY, itemRect.w - 60),
                    itemRect.x + 14,
                    itemRect.CenterY(),
                    ui::FONT_BODY,
                    selected ? col::Text : (unfit ? col::TextMuted : col::TextDim),
                    false,
                    Anchor::LEFT | Anchor::VCENTER);
                font.DrawString(
                    std::to_string(mapInfo.players),
                    itemRect.Right() - 10,
                    itemRect.CenterY(),
                    ui::FONT_SMALL,
                    col::TextMuted,
                    false,
                    Anchor::RIGHT | Anchor::VCENTER);
            }
            renderer.ResetClipRect();
        }
        ui::DrawPanel(renderer, layout.map, col::PanelSunken, col::Border);
        if (lobby && !lobby->map.empty() && !net->MapReady())
        {
            font.DrawString(
                _("Downloading the map..."),
                layout.map.CenterX(),
                layout.map.CenterY(),
                ui::FONT_BODY,
                col::TextDim,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }
        if (lobby && net->MapReady())
        {
            int previewW = 0, previewH = 0;
            const std::string &file = net->LobbyMapFile();
            SDL_Texture *texture
                = m_preview.Get(renderer.GetSdlRenderer(), textures, file, file, {}, previewW, previewH);
            if (texture)
            {
                m_mapW = previewW;
                m_mapH = previewH;
                const float scale = std::min(
                    static_cast<float>(layout.map.w) / previewW, static_cast<float>(layout.map.h) / previewH);
                const SDL_FRect destRect {
                    layout.map.x + (layout.map.w - previewW * scale) / 2.0f,
                    layout.map.y + (layout.map.h - previewH * scale) / 2.0f,
                    previewW * scale,
                    previewH * scale};
                SDL_RenderTexture(renderer.GetSdlRenderer(), texture, nullptr, &destRect);
            }
            for (std::size_t i = 0; i < m_spots.size() && i < lobby->spawnCount; ++i)
            {
                int spotX = 0, spotY = 0;
                if (!SpotOnScreen(layout.map, m_mapW, m_mapH, m_spots[i].first, m_spots[i].second, spotX, spotY))
                    continue;
                SDL_Color spotColor = col::TextDim;
                for (const Net::LobbySlot &lobbySlot : lobby->slots)
                {
                    Net::LobbyChoice slotChoice = lobbySlot.choice;
                    if (!lobbySlot.ai && lobbySlot.slot == net->LocalSlot())
                        net->LocalChoice(slotChoice);
                    if (slotChoice.spawn == i)
                        spotColor = SlotColor(slotChoice.color);
                }
                const bool hot = std::abs(m_mouseX - spotX) <= 14 && std::abs(m_mouseY - spotY) <= 14;
                renderer.FillRectScreen(spotX - 12, spotY - 12, 24, 24, col::PanelSunken);
                const bool blockedSpot = ((lobby->blockedMask >> i) & 1u) != 0;
                renderer.FillRectScreen(
                    spotX - 10, spotY - 10, 20, 20, blockedSpot ? SDL_Color {40, 20, 20, 255} : spotColor);
                renderer.DrawRectScreen(
                    spotX - 12, spotY - 12, 24, 24, hot ? col::Accent : (blockedSpot ? col::Danger : col::Text));
                if (blockedSpot)
                {
                    renderer.DrawLineScreen(spotX - 9, spotY - 9, spotX + 9, spotY + 9, col::Danger);
                    renderer.DrawLineScreen(spotX - 9, spotY + 9, spotX + 9, spotY - 9, col::Danger);
                }
                font.DrawString(
                    std::to_string(i + 1),
                    spotX,
                    spotY,
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
            const int tech = owner ? m_tech : (lobby ? lobby->techLevel : 0);
            const bool hoverOk = m_combo.open < 0;
            ui::DrawComboField(
                renderer,
                font,
                layout.techBtn,
                ui::TechLevelLabel(tech - 1, false),
                m_combo.open == kTechCombo,
                owner && hoverOk && layout.techBtn.Contains(m_mouseX, m_mouseY),
                owner);
            ui::DrawComboField(
                renderer,
                font,
                layout.timeBtn,
                ui::TimeOfDayChoices()[night ? 1 : 0],
                m_combo.open == kTimeCombo,
                owner && hoverOk && layout.timeBtn.Contains(m_mouseX, m_mouseY),
                owner);
            ui::DrawComboField(
                renderer,
                font,
                layout.creditsBtn,
                ui::CreditsLabel(credits),
                m_combo.open == kCreditsCombo,
                owner && hoverOk && layout.creditsBtn.Contains(m_mouseX, m_mouseY),
                owner);
            ui::DrawCheckbox(
                renderer,
                font,
                layout.fogCheckbox,
                _("FOG OF WAR"),
                fog,
                owner && hoverOk && layout.fogCheckbox.Contains(m_mouseX, m_mouseY),
                owner);
            ui::DrawCheckbox(
                renderer,
                font,
                layout.prebuiltBtn,
                _("START WITH A BASE"),
                prebuilt,
                owner && hoverOk && layout.prebuiltBtn.Contains(m_mouseX, m_mouseY),
                owner);
        }
        if (lobby && !lobby->map.empty())
        {
            // How many players the map is made for (its start positions)
            font.DrawString(
                std::string(_("Players on this map: ")) + std::to_string(lobby->spawnCount),
                layout.map.x + 8,
                layout.map.Bottom() - 18,
                ui::FONT_SMALL_BOLD,
                col::Accent,
                true);
        }
        font.DrawTextWrapped(
            _("Click a number on the map to start there, again to leave it to chance. Click your faction and colour to "
              "change them; the host sets the teams."),
            layout.fogCheckbox.x,
            layout.prebuiltBtn.Bottom() + 10,
            layout.fogCheckbox.w,
            ui::FONT_SMALL,
            col::TextDim,
            false);

        // Chat
        ui::DrawPanel(renderer, layout.chatLog, col::PanelSunken, col::Border);
        const int first = std::max(0, static_cast<int>(m_chatLog.size()) - kChatLines);
        int chatY = layout.chatLog.y + 6;
        for (int i = first; i < static_cast<int>(m_chatLog.size()); ++i)
        {
            font.DrawString(
                ui::FitText(font, m_chatLog[i], ui::FONT_SMALL, layout.chatLog.w - 16),
                layout.chatLog.x + 8,
                chatY,
                ui::FONT_SMALL,
                col::Text,
                false);
            chatY += 18;
        }
        DrawField(renderer, font, layout.chatField, m_chatInput, _("Type a message"), m_focus == Field::Chat);
        ui::DrawButton(
            renderer,
            font,
            layout.chatSend,
            "->",
            ui::ButtonStyle::Primary,
            layout.chatSend.Contains(m_mouseX, m_mouseY),
            !m_chatInput.empty());

        bool ready = false;
        if (lobby)
        {
            for (const Net::LobbySlot &lobbySlot : lobby->slots)
            {
                if (lobbySlot.slot == net->LocalSlot())
                {
                    Net::LobbyChoice slotChoice = lobbySlot.choice;
                    net->LocalChoice(slotChoice);
                    ready = slotChoice.ready;
                }
            }
        }
        ui::DrawButton(
            renderer,
            font,
            layout.leaveBtn,
            _("LEAVE"),
            ui::ButtonStyle::Secondary,
            layout.leaveBtn.Contains(m_mouseX, m_mouseY));
        ui::DrawButton(
            renderer,
            font,
            layout.readyBtn,
            ready ? _("NOT READY") : _("READY"),
            ready ? ui::ButtonStyle::Secondary : ui::ButtonStyle::Primary,
            layout.readyBtn.Contains(m_mouseX, m_mouseY),
            net->MapReady());
        if (net->ControlsRoom())
        {
            ui::DrawButton(
                renderer,
                font,
                layout.startBtn,
                _("START"),
                ui::ButtonStyle::Primary,
                layout.startBtn.Contains(m_mouseX, m_mouseY),
                net->CanStart());
        }
        else
        {
            font.DrawString(
                _("The host starts the game"),
                layout.startBtn.CenterX(),
                layout.startBtn.CenterY(),
                ui::FONT_SMALL,
                col::TextDim,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }
        // The open drop-down list, over everything
        if (m_combo.open >= 0)
        {
            const int comboId = m_combo.open, row = comboId / 10, kind = IsOptionCombo(comboId) ? 5 : comboId % 10;
            if (comboId >= kFilterPlayers && comboId <= kFilterSort)
            {
                const std::vector<int> counts = ui::MapFilter::PlayerCounts(m_maps);
                const auto currentIt = std::find(counts.begin(), counts.end(), m_mapFilter.players);
                const int current = comboId == kFilterPlayers
                                        ? (currentIt == counts.end() ? 0 : static_cast<int>(currentIt - counts.begin()))
                                    : comboId == kFilterSize ? m_mapFilter.size
                                                             : m_mapFilter.sort;
                ui::DrawComboList(
                    renderer,
                    font,
                    layout.Filter(comboId - kFilterPlayers),
                    MapFilterChoices(comboId, m_maps),
                    current,
                    m_combo,
                    windowH,
                    m_mouseX,
                    m_mouseY);
            }
            else if (comboId == kCreditsCombo)
            {
                ui::DrawComboList(
                    renderer,
                    font,
                    layout.creditsBtn,
                    ui::CreditsChoices(),
                    m_credits,
                    m_combo,
                    windowH,
                    m_mouseX,
                    m_mouseY);
            }
            else if (comboId == kTimeCombo)
            {
                ui::DrawComboList(
                    renderer,
                    font,
                    layout.timeBtn,
                    ui::TimeOfDayChoices(),
                    m_night ? 1 : 0,
                    m_combo,
                    windowH,
                    m_mouseX,
                    m_mouseY);
            }
            else if (comboId == kTechCombo)
            {
                ui::DrawComboList(
                    renderer,
                    font,
                    layout.techBtn,
                    ui::TechLevelChoices(false),
                    m_tech,
                    m_combo,
                    windowH,
                    m_mouseX,
                    m_mouseY);
            }
            else if (lobby && kind == 0 && row < std::min<int>(lobby->spawnCount, kMaxPlayers))
            {
                const std::vector<SeatOption> options = SeatOptions(*net, *lobby, row);
                std::vector<std::string> items;
                for (const SeatOption &option : options)
                    items.push_back(option.label);
                ui::DrawComboList(
                    renderer,
                    font,
                    layout.TypeCell(row),
                    items,
                    SeatCurrent(options, *lobby, row),
                    m_combo,
                    windowH,
                    m_mouseX,
                    m_mouseY);
            }
            else if (lobby && SlotAtSpawn(*lobby, row) >= 0)
            {
                const Net::LobbySlot &lobbySlot = lobby->slots[SlotAtSpawn(*lobby, row)];
                Net::LobbyChoice slotChoice = lobbySlot.choice;
                if (!lobbySlot.ai && lobbySlot.slot == net->LocalSlot())
                    net->LocalChoice(slotChoice);
                const ui::UiRect field = kind == 1   ? layout.FactionCell(row)
                                         : kind == 2 ? layout.TeamCell(row)
                                         : kind == 3 ? layout.ColorCell(row)
                                         : kind == 7 ? layout.HandicapCell(row)
                                         : kind == 9 ? layout.TechCell(row)
                                                     : layout.ArmyCell(row);
                const int current = kind == 1   ? (slotChoice.faction == Net::kRandom ? 0 : slotChoice.faction + 1)
                                    : kind == 2 ? slotChoice.team
                                    : kind == 3 ? std::max(0, slotChoice.color - 1)
                                    : kind == 7 ? std::min<int>(slotChoice.handicap, 5)
                                    : kind == 9 ? static_cast<int>(slotChoice.techLevel)
                                                : std::min<int>(slotChoice.armyCap, kArmyCapCount - 1);
                std::vector<std::string> items = ComboChoices(kind);
                std::vector<bool> taken;
                std::vector<SDL_Color> swatches;
                if (kind == 3)
                {
                    taken = TakenColors(*lobby, lobbySlot.slot);
                    for (int i = 1; i <= Settings::kArmyColors; ++i)
                        swatches.push_back(ArmyColor(i));
                    for (std::size_t i = 0; i < items.size() && i < taken.size(); ++i)
                    {
                        if (taken[i] && static_cast<int>(i) != current)
                            items[i] += std::string(" (") + _("taken") + ")";
                    }
                }
                ui::DrawComboList(
                    renderer,
                    font,
                    field,
                    items,
                    current,
                    m_combo,
                    windowH,
                    m_mouseX,
                    m_mouseY,
                    kind == 3 ? &taken : nullptr,
                    kind == 3 ? &swatches : nullptr);
            }
        }
        ui::DrawConfirm(renderer, font, windowW, windowH, m_confirm, m_mouseX, m_mouseY);
        return;
    }

    // ----- Browser / connecting -----
    const BrowserLayout layout = browserLayout(windowW, windowH);
    if (net)
    {
        // Connecting: the loading screen with what the connection is doing, and a way out
        ui::DrawLoadingScreen(
            renderer,
            font,
            windowW,
            windowH,
            _("NETWORK GAME"),
            net->ConnectingText(SDL_GetTicks()),
            -1.0f,
            SDL_GetTicks());
        ui::DrawButton(
            renderer,
            font,
            layout.backBtn,
            _("CANCEL"),
            ui::ButtonStyle::Secondary,
            layout.backBtn.Contains(m_mouseX, m_mouseY));
        return;
    }
    ui::DrawDialog(renderer, font, windowW, windowH, layout.dialog, _("NETWORK GAME"));
    font.DrawString(
        _("Your name"),
        layout.dialog.x + 24,
        layout.nameField.CenterY(),
        ui::FONT_BODY_BOLD,
        col::Text,
        false,
        Anchor::LEFT | Anchor::VCENTER);
    DrawField(
        renderer, font, layout.nameField, settings.playerName, _("Your name in the game"), m_focus == Field::Name);
    font.DrawString(
        _("Password"),
        layout.nameField.Right() + 16,
        layout.nameField.CenterY(),
        ui::FONT_BODY_BOLD,
        col::Text,
        false,
        Anchor::LEFT | Anchor::VCENTER);
    DrawField(
        renderer,
        font,
        layout.passwordField,
        std::string(m_password.size(), '*'),
        _("Optional"),
        m_focus == Field::Password);
    font.DrawString(
        _("Connect by address (the host forwards UDP port 38100)"),
        layout.addressField.x,
        layout.addressField.y - 18,
        ui::FONT_SMALL,
        col::TextDim,
        false);
    DrawField(
        renderer,
        font,
        layout.addressField,
        settings.lastAddress,
        _("IP address or hostname"),
        m_focus == Field::Address);
    ui::DrawButton(
        renderer,
        font,
        layout.directBtn,
        _("CONNECT"),
        ui::ButtonStyle::Secondary,
        layout.directBtn.Contains(m_mouseX, m_mouseY),
        !settings.lastAddress.empty());
    ui::DrawButton(
        renderer,
        font,
        layout.tabLan,
        _("LOCAL NETWORK"),
        m_tab == Tab::Lan ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
        layout.tabLan.Contains(m_mouseX, m_mouseY));
    ui::DrawButton(
        renderer,
        font,
        layout.tabInternet,
        _("INTERNET"),
        m_tab == Tab::Internet ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
        layout.tabInternet.Contains(m_mouseX, m_mouseY));
    if (m_tab == Tab::Lan)
    {
        ui::DrawCaption(font, renderer, layout.list.x, layout.refreshBtn.y + 6, _("LOCAL NETWORK"));
        ui::DrawButton(
            renderer,
            font,
            layout.refreshBtn,
            _("REFRESH"),
            ui::ButtonStyle::Ghost,
            layout.refreshBtn.Contains(m_mouseX, m_mouseY));
        ui::DrawPanel(renderer, layout.list, col::PanelSunken, col::Border);
        if (lan.empty())
        {
            font.DrawString(
                _("Looking for games on the local network..."),
                layout.list.CenterX(),
                layout.list.CenterY(),
                ui::FONT_BODY,
                col::TextDim,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }
        for (std::size_t i = 0; i < lan.size() && static_cast<int>(i) < std::max(1, (layout.list.h - 8) / 40); ++i)
        {
            const UiRect row {layout.list.x + 4, layout.list.y + 4 + static_cast<int>(i) * 40, layout.list.w - 8, 36};
            const bool isSelected = static_cast<int>(i) == m_lanSelected;
            ui::DrawPanel(
                renderer, row, isSelected ? col::Hover : col::PanelRaised, isSelected ? col::Accent : col::Border);
            const LanGame &game = lan[i];
            font.DrawString(
                ui::FitText(font, game.name, ui::FONT_BODY_BOLD, row.w * 26 / 100),
                row.x + 10,
                row.CenterY(),
                ui::FONT_BODY_BOLD,
                col::Text,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                ui::FitText(font, game.map, ui::FONT_SMALL, row.w * 30 / 100),
                row.x + row.w * 28 / 100,
                row.CenterY(),
                ui::FONT_SMALL,
                col::TextDim,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                std::to_string(game.players) + "/" + std::to_string(game.maxPlayers),
                row.x + row.w * 62 / 100,
                row.CenterY(),
                ui::FONT_SMALL,
                col::Text,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                std::to_string(game.pingMs) + " ms",
                row.Right() - 10,
                row.CenterY(),
                ui::FONT_SMALL,
                game.pingMs < 60 ? col::Ok : col::Accent,
                false,
                Anchor::RIGHT | Anchor::VCENTER);
            if (game.locked)
                DrawLock(renderer, row.x + row.w * 62 / 100 - 28, row.CenterY() - 7, col::Accent);
        }
    }
    else
    {
        ui::DrawCaption(
            font,
            renderer,
            layout.serverList.x,
            layout.refreshBtn.y + 6,
            std::string(_("SERVER")) + "  " + serverStatus);
        ui::DrawButton(
            renderer,
            font,
            layout.refreshBtn,
            _("REFRESH"),
            ui::ButtonStyle::Ghost,
            layout.refreshBtn.Contains(m_mouseX, m_mouseY));
        ui::DrawPanel(renderer, layout.serverList, col::PanelSunken, col::Border);
        if (server.empty())
        {
            font.DrawString(
                _("No open rooms: create one, or join by code"),
                layout.serverList.CenterX(),
                layout.serverList.CenterY(),
                ui::FONT_SMALL,
                col::TextDim,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
        }
        for (int i = 0; i < static_cast<int>(server.size()) && i < std::max(1, (layout.serverList.h - 8) / 34); ++i)
        {
            const UiRect row = layout.ServerRow(i);
            const bool isSelected = i == m_serverSelected;
            const LanGame &game = server[i];
            ui::DrawPanel(
                renderer, row, isSelected ? col::Hover : col::PanelRaised, isSelected ? col::Accent : col::Border);
            font.DrawString(
                game.code,
                row.x + 10,
                row.CenterY(),
                ui::FONT_SMALL_BOLD,
                game.port == 0 ? col::Info : col::Accent,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                ui::FitText(font, game.name, ui::FONT_BODY_BOLD, row.w * 30 / 100),
                row.x + 110,
                row.CenterY(),
                ui::FONT_BODY_BOLD,
                col::Text,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                ui::FitText(font, game.map, ui::FONT_SMALL, row.w * 30 / 100),
                row.x + row.w * 45 / 100,
                row.CenterY(),
                ui::FONT_SMALL,
                col::TextDim,
                false,
                Anchor::LEFT | Anchor::VCENTER);
            font.DrawString(
                std::to_string(game.players) + "/" + std::to_string(game.maxPlayers),
                row.Right() - 10,
                row.CenterY(),
                ui::FONT_SMALL,
                col::Text,
                false,
                Anchor::RIGHT | Anchor::VCENTER);
            if (game.locked)
                DrawLock(renderer, row.Right() - 56, row.CenterY() - 7, col::Accent);
        }
        font.DrawString(
            _("Room code"),
            layout.serverList.x,
            layout.codeField.CenterY(),
            ui::FONT_BODY_BOLD,
            col::Text,
            false,
            Anchor::LEFT | Anchor::VCENTER);
        DrawField(renderer, font, layout.codeField, m_joinCode, _("6 characters"), m_focus == Field::Code);
        ui::DrawButton(
            renderer,
            font,
            layout.codeBtn,
            _("JOIN BY CODE"),
            ui::ButtonStyle::Secondary,
            layout.codeBtn.Contains(m_mouseX, m_mouseY),
            m_joinCode.size() == 6);
    }
    if (!m_notice.empty())
    {
        font.DrawString(
            m_notice,
            layout.dialog.CenterX(),
            layout.joinBtn.y - 18,
            ui::FONT_SMALL_BOLD,
            col::Danger,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
    }
    ui::DrawButton(
        renderer,
        font,
        layout.backBtn,
        _("BACK"),
        ui::ButtonStyle::Secondary,
        layout.backBtn.Contains(m_mouseX, m_mouseY));
    ui::DrawButton(
        renderer,
        font,
        layout.hostBtn,
        _("CREATE GAME"),
        ui::ButtonStyle::Secondary,
        layout.hostBtn.Contains(m_mouseX, m_mouseY));
    if (m_tab == Tab::Internet)
        ui::DrawButton(
            renderer,
            font,
            layout.serverBtn,
            _("CREATE ROOM"),
            ui::ButtonStyle::Secondary,
            layout.serverBtn.Contains(m_mouseX, m_mouseY));
    const bool canJoin = m_tab == Tab::Internet
                             ? (m_serverSelected >= 0 && m_serverSelected < static_cast<int>(server.size()))
                             : (m_lanSelected >= 0 && m_lanSelected < static_cast<int>(lan.size()));
    ui::DrawButton(
        renderer,
        font,
        layout.joinBtn,
        _("JOIN"),
        ui::ButtonStyle::Primary,
        layout.joinBtn.Contains(m_mouseX, m_mouseY),
        canJoin);
}

} // namespace OpenArtOfWar
