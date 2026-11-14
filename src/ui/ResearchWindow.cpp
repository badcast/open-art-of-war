#include "audio/SoundRequests.hpp"
#include "core/Localization.hpp"
#include "game/Capabilities.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/Players.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "graphics/Renderer.hpp"
#include "input/InputManager.hpp"
#include "ui/HUDController.hpp"
#include "ui/UiKit.hpp"
#include "ui/UiLayout.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>

// The window of the technology tree (button "RESEARCH NEW TECHNOLOGIES" under the objectives): the tree of the
// player's faction as in the character editor, a search box, a filter and the selected technology with its button.

namespace OpenArtOfWar
{

using ui::UiRect;
namespace col = ui::color;

namespace
{

// Sizes of the tree at scale 1: the technologies of the tree (their size and icons) scale by m_researchScale
// while keeping a balanced 3-division schematic inside the viewport on desktop and touch.
constexpr int kNodeW0 = 254;
constexpr int kNodeH0 = 76;
constexpr int kIndentX0 = 18; // dependent child horizontal offset per prerequisite depth
constexpr int kColGapX0 = 16; // gutter between the 3 technology divisions
constexpr int kGapY0 = 10;
constexpr int kHeaderH0 = 32; // division column header bar
constexpr int kHeaderGapY0 = 10;
constexpr int kPadX0 = 14;
constexpr int kPadY0 = 12;
constexpr int kDetailsW0 = 290;
constexpr float kScaleMin = 0.75f, kScaleMax = 4.0f, kScaleStep = 0.125f;
int sz(int v, float scale)
{
    return static_cast<int>(std::lround(v * scale));
}

struct WindowLayout
{
    UiRect dialog, content, search, filter, tree, details, closeBtn, startBtn, zoomIn, zoomOut;
    float u {1.0f};     // the scale of the profile (the touch profile grows with the screen); the tree grows with it
    bool touch {false}; // the touch profile: bigger fonts inside the nodes and the details, no scroll bar grabbing
};

// The built-in numbers (without the `research` screen of ui/layout.yaml)
WindowLayout builtinLayout(int windowW, int windowH, int top)
{
    WindowLayout windowLayout;
    windowLayout.dialog = ui::CenteredRect(windowW, windowH, windowW, windowH);
    windowLayout.dialog.y += top;
    windowLayout.dialog.h -= top;
    windowLayout.content
        = {windowLayout.dialog.x + 16,
           windowLayout.dialog.y + 60,
           windowLayout.dialog.w - 32,
           windowLayout.dialog.h - 60 - 16};
    const int kDetailsW = std::clamp(static_cast<int>(windowLayout.content.w * 0.27f), kDetailsW0, 440);
    windowLayout.search
        = {windowLayout.content.x,
           windowLayout.content.y,
           std::clamp(static_cast<int>(windowLayout.content.w * 0.28f), 280, 460),
           36};
    windowLayout.filter
        = {windowLayout.search.Right() + 10,
           windowLayout.content.y,
           std::clamp(static_cast<int>(windowLayout.content.w * 0.18f), 200, 300),
           36};
    windowLayout.closeBtn = {windowLayout.content.Right() - 144, windowLayout.content.y, 144, 36};
    windowLayout.details = {
        windowLayout.content.Right() - kDetailsW, windowLayout.content.y + 46, kDetailsW, windowLayout.content.h - 46};
    windowLayout.tree
        = {windowLayout.content.x,
           windowLayout.content.y + 46,
           windowLayout.details.x - 12 - windowLayout.content.x,
           windowLayout.content.h - 46};
    windowLayout.startBtn
        = {windowLayout.details.x + 12, windowLayout.details.Bottom() - 58, windowLayout.details.w - 24, 46};
    windowLayout.zoomOut = {windowLayout.tree.Right() - 152, windowLayout.tree.Bottom() - 44, 34, 34};
    windowLayout.zoomIn = {windowLayout.tree.Right() - 44, windowLayout.tree.Bottom() - 44, 34, 34};
    return windowLayout;
}

// Where the parts stand is ui/layout.yaml (screen research: profile pc, or touch with the touch interface: big
// controls); `top`: rows kept free above the dialog (the bar of an observer). A file without the screen, or without
// one of its nodes, gives the built-in numbers.
WindowLayout windowLayout(int windowW, int windowH, int top = 0)
{
    const uilayout::Layout &file = uilayout::Current();
    const std::string profile = uilayout::MenuProfile(file, "research");
    if (file.Has("research", profile))
    {
        const std::map<std::string, float> extra {{"T", static_cast<float>(top)}};
        const uilayout::Solved s = file.Solve("research", profile, windowW, windowH, {}, &extra);
        WindowLayout l;
        l.u = s.u;
        l.touch = profile == "touch";
        l.dialog = s.Rect("dialog");
        l.content = s.Rect("content");
        l.search = s.Rect("search");
        l.filter = s.Rect("filter");
        l.tree = s.Rect("tree");
        l.details = s.Rect("details");
        l.closeBtn = s.Rect("close");
        l.startBtn = s.Rect("start");
        l.zoomIn = s.Rect("zoom_in");
        l.zoomOut = s.Rect("zoom_out");
        const UiRect *all[]
            = {&l.dialog,
               &l.content,
               &l.search,
               &l.filter,
               &l.tree,
               &l.details,
               &l.closeBtn,
               &l.startBtn,
               &l.zoomIn,
               &l.zoomOut};
        bool complete = true;
        for (const UiRect *r : all)
            complete = complete && r->w > 0 && r->h > 0;
        if (complete)
            return l;
    }
    return builtinLayout(windowW, windowH, top);
}

// Technologies are grouped dynamically into 3 tactical divisions (Infantry, Armor & Vehicles, Base & Command) so
// independent root techs fill balanced side-by-side columns while dependent upgrades indent directly under their
// parent.
struct TreeLayout
{
    std::vector<int> parent, depth, division;
    std::vector<float> row;
    std::vector<bool> shown; // the local player's tech level (data/capabilities.yaml) has it
    int colMaxDepth[3] {0, 0, 0};
    int colCount[3] {0, 0, 0};
    int first {-1};
    float maxRows {1.0f};
};

int classifyRootDivision(const ResearchDef &r)
{
    switch (r.effect)
    {
        case ResearchEffect::InfantryArmor:
        case ResearchEffect::InfantryRegen:
        case ResearchEffect::InfantrySpeed:
        case ResearchEffect::TrainTime:
            return 0;
        case ResearchEffect::FireRate:
        case ResearchEffect::Damage:
        case ResearchEffect::VisionRange:
        case ResearchEffect::UnlockUnit:
        case ResearchEffect::Reload:
            return (r.archA >= 0 && r.archA <= 5) ? 0 : 1;
        case ResearchEffect::VehicleSpeed:
        case ResearchEffect::VehicleArmor:
        case ResearchEffect::RepairSpeed:
        case ResearchEffect::FortressFlamethrower:
        case ResearchEffect::SiegeMode:
        case ResearchEffect::RocketTurret:
        case ResearchEffect::RangePercent:
        case ResearchEffect::VehicleTime:
            return 1;
        case ResearchEffect::PowerBonus:
        case ResearchEffect::BuildingArmor:
        case ResearchEffect::WallStrength:
        case ResearchEffect::UnlockRadar:
        case ResearchEffect::Income:
        case ResearchEffect::KillReward:
        case ResearchEffect::BuildRadius:
        case ResearchEffect::Mine:
            return 2;
        default:
            return 1;
    }
}

const char *divisionTitle(int div)
{
    switch (div)
    {
        case 0:
            return "INFANTRY";
        case 1:
            return "ARMOR & VEHICLES";
        default:
            return "BASE & COMMAND";
    }
}

TreeLayout buildTree(Faction faction)
{
    const int count = kResearchCount;
    TreeLayout t;
    t.parent.assign(count, -1);
    t.depth.assign(count, 0);
    t.division.assign(count, 1);
    t.row.assign(count, 0.0f);
    t.shown.assign(count, false);
    std::vector<std::vector<int>> children(count);
    std::vector<int> roots;
    auto dependsOn = [&](int candidate, int index) {
        for (int guard = 0; candidate >= 0 && guard < 64; ++guard)
        {
            if (candidate == index)
                return true;
            candidate = researchDef(faction, candidate).needs;
        }
        return false;
    };
    for (int i = 0; i < count; ++i)
    {
        // Left out by the tech level: not placed (what needs it is left out as well)
        if (!Capabilities::AllowsResearch(Players::Local(), i))
            continue;
        t.shown[i] = true;
        if (t.first < 0)
            t.first = i;
        int need = researchDef(faction, i).needs;
        if (need < 0 || need >= count || need == i || dependsOn(need, i))
            need = -1;
        t.parent[i] = need;
        if (need < 0)
            roots.push_back(i);
        else
            children[need].push_back(i);
    }
    float colNextRow[3] {0.0f, 0.0f, 0.0f};
    std::function<void(int, int, int)> place = [&](int node, int d, int div) {
        t.depth[node] = d;
        t.division[node] = div;
        t.colMaxDepth[div] = std::max(t.colMaxDepth[div], d);
        t.colCount[div] += 1;
        t.row[node] = colNextRow[div];
        colNextRow[div] += 1.0f;
        for (int c : children[node])
            place(c, d + 1, div);
    };
    for (int r : roots)
    {
        const int div = classifyRootDivision(researchDef(faction, r));
        place(r, 0, div);
    }
    t.maxRows = std::max({1.0f, colNextRow[0], colNextRow[1], colNextRow[2]});
    return t;
}

struct TreeMetrics
{
    int colX[3] {0, 0, 0};
    int colW[3] {0, 0, 0};
    bool colShown[3] {false, false, false};
    int headerH {0};
    int headerGapY {0};
    int nodeW {0}, nodeH {0}, indentX {0}, gapY {0}, padY {0};
    float contentW {0.0f}, contentH {0.0f};
    int originX {0};
};

TreeMetrics measureTree(const TreeLayout &tree, const WindowLayout &l, float sc)
{
    TreeMetrics m;
    m.nodeW = sz(kNodeW0, sc);
    m.nodeH = sz(kNodeH0, sc);
    m.indentX = sz(kIndentX0, sc);
    m.gapY = sz(kGapY0, sc);
    m.headerH = sz(kHeaderH0, sc);
    m.headerGapY = sz(kHeaderGapY0, sc);
    m.padY = sz(kPadY0, sc);
    const int padX = sz(kPadX0, sc);
    const int colGap = sz(kColGapX0, sc);
    int x = padX;
    bool any = false;
    for (int c = 0; c < 3; ++c)
    {
        if (tree.colCount[c] <= 0)
            continue;
        any = true;
        m.colShown[c] = true;
        m.colW[c] = m.nodeW + std::min(tree.colMaxDepth[c], 3) * m.indentX;
        m.colX[c] = x;
        x += m.colW[c] + colGap;
    }
    m.contentW = any ? static_cast<float>(x - colGap + padX) : static_cast<float>(l.tree.w);
    const int contentWInt = static_cast<int>(std::lround(m.contentW));
    m.originX = contentWInt < l.tree.w ? (l.tree.w - contentWInt) / 2 : 0;
    const float cardsH
        = tree.maxRows * static_cast<float>(m.nodeH) + std::max(0.0f, tree.maxRows - 1.0f) * static_cast<float>(m.gapY);
    m.contentH = static_cast<float>(m.padY + m.headerH + m.headerGapY) + cardsH + static_cast<float>(sz(50, sc));
    return m;
}

UiRect nodeRect(const TreeLayout &tree, const TreeMetrics &m, int i, const UiRect &view, float scrollX, float scrollY)
{
    const int div = std::clamp(tree.division[i], 0, 2);
    const int indent = std::min(tree.depth[i], 3) * m.indentX;
    return {
        view.x + m.originX + m.colX[div] + indent - static_cast<int>(std::lround(scrollX)),
        view.y + m.padY + m.headerH + m.headerGapY + static_cast<int>(tree.row[i] * (m.nodeH + m.gapY))
            - static_cast<int>(std::lround(scrollY)),
        m.nodeW,
        m.nodeH};
}

UiRect searchClearRect(const WindowLayout &l)
{
    const int side = std::max(20, l.search.h - 8);
    return {l.search.Right() - side - 4, l.search.y + (l.search.h - side) / 2, side, side};
}

UiRect zoomResetRect(const WindowLayout &l)
{
    const int gap = l.zoomIn.x - l.zoomOut.Right();
    if (gap >= l.zoomOut.h && std::abs(l.zoomIn.y - l.zoomOut.y) <= 8)
        return {l.zoomOut.Right() + 4, l.zoomOut.y, gap - 8, l.zoomOut.h};
    return {};
}

UiRect detailsReqRect(const WindowLayout &l)
{
    const int pad = l.touch ? 16 : 14;
    const int x = l.details.x + pad;
    const int w = l.details.w - pad * 2;
    const int headerH = l.touch ? 92 : 82;
    const int tilesY = l.details.y + pad + headerH + 10;
    const int tileH = l.touch ? 60 : 52;
    const int reqY = tilesY + tileH + 10;
    const int reqH = l.touch ? 50 : 42;
    return {x, reqY, w, reqH};
}

[[maybe_unused]] std::string lower(std::string_view s)
{
    std::string out;
    for (unsigned char c : s)
        out += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
    return out;
}

// Case-insensitive for Cyrillic (UTF-8) too
std::string lowerUtf8(std::string_view t)
{
    std::string out;
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(t[i]);
        if (c == 0xD0 && i + 1 < t.size())
        {
            const unsigned char d = static_cast<unsigned char>(t[i + 1]);
            if (d >= 0x90 && d <= 0x9F)
            {
                out += static_cast<char>(0xD0);
                out += static_cast<char>(d + 0x20);
                ++i;
                continue;
            }
            if (d >= 0xA0 && d <= 0xAF)
            {
                out += static_cast<char>(0xD1);
                out += static_cast<char>(d - 0x20);
                ++i;
                continue;
            }
        }
        out += c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    }
    return out;
}

void popUtf8(std::string &s)
{
    while (!s.empty())
    {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80)
            break;
    }
}

std::vector<std::string> filterChoices()
{
    return {_("All"), _("Available"), _("Researched"), _("Locked"), _("In progress")};
}

enum class State
{
    Done,
    Active,
    Available,
    Locked
};

State stateOf(int index, const ResearchProgress *progress)
{
    if (IsResearched(Players::Local(), index))
        return State::Done;
    if (progress && progress->active == index)
        return State::Active;
    return IsResearchAvailable(Players::Local(), index) ? State::Available : State::Locked;
}

} // namespace

void HUDController::OpenResearchWindow()
{
    m_researchOpen = true;
    m_researchSearchFocus = false;
    m_researchCombo.open = -1;
    m_researchScroll.dragging = m_researchScroll.dragMoved = false;
    m_researchScroll.StopGlide();
    m_researchScroll.lastTickNs = SDL_GetTicksNS();
    m_researchScrollDrag = 0;
    m_researchClickNode = -1;
    m_buildMode = false;
    m_attackMoveMode = false;
    SoundRequests::Play("ui_show_dialog");
}

void HUDController::CloseResearchWindow()
{
    m_researchOpen = false;
    m_researchSearchFocus = false;
    m_researchCombo.open = -1;
    m_researchScroll.dragging = m_researchScroll.dragMoved = false;
    m_researchScroll.StopGlide();
    m_researchScrollDrag = 0;
}

ResearchProgress HUDController::ResearchShown() const
{
    if (m_researchProbe)
        return m_researchProbe(Players::Local());
    return m_sim ? m_sim->research(Players::Local()) : ResearchProgress {};
}

bool HUDController::TryStartResearch(int index, Economy &economy, EntityManager &entities)
{
    if (m_observer)
        return false; // an observer only looks at the research of the player he watches
    const auto &r = researchDef(PlayerFaction(), index);
    if (!HasResearchBuilding(Players::Local(), entities))
    {
        SoundRequests::Play("ui_error");
        SetNotification(
            PlayerFaction() == Faction::Confederation ? _("TECH CENTER REQUIRED FOR RESEARCH!")
                                                      : _("LABORATORY REQUIRED FOR RESEARCH!"));
    }
    else if (IsResearched(Players::Local(), index))
    {
        SoundRequests::Play("ui_error");
        SetNotification(_("THIS TECHNOLOGY IS ALREADY RESEARCHED!"));
    }
    else if (!IsResearchAvailable(Players::Local(), index))
    {
        SoundRequests::Play("ui_error");
        SetNotification(std::string(_("REQUIRES: ")) + _(researchDef(PlayerFaction(), r.needs).name));
    }
    else if (ResearchShown().active >= 0)
    {
        SoundRequests::Play("ui_error");
        SetNotification(_("ANOTHER RESEARCH IS ALREADY IN PROGRESS!"));
    }
    else if (economy.CanAfford(r.cost))
    {
        SimCommand c;
        c.type = CommandType::StartResearch;
        c.a = index;
        Issue(std::move(c));
        return true;
    }
    else
    {
        SoundRequests::Announce("ann_insufficient_funds");
        SetNotification(_("INSUFFICIENT CREDITS FOR RESEARCH!"));
    }
    return false;
}

namespace
{
// Where the scroll bars of the tree go: inside its right and bottom edge
UiRect researchVTrack(const UiRect &tree)
{
    return {tree.Right() - 8, tree.y + 3, 5, tree.h - 14};
}
UiRect researchHTrack(const UiRect &tree)
{
    return {tree.x + 3, tree.Bottom() - 8, tree.w - 14, 5};
}
} // namespace

bool HUDController::UpdateResearchWindow(const FrameContext &ctx, int mx, int my, bool leftClicked)
{
    InputManager &input = ctx.input;
    const int windowW = ctx.windowW;
    const int windowH = ctx.windowH;
    Economy &economy = ctx.economy;
    EntityManager &entities = ctx.entities;
    if (!m_researchOpen)
        return false;
    const WindowLayout l = windowLayout(windowW, windowH, m_observer ? m_observerBarHeight : 0);
    const float sc = m_researchScale * l.u;                       // (the touch profile: bigger with the screen)
    const Faction faction = Players::FactionOf(Players::Local()); // (an observer: the watched player's)
    const TreeLayout tree = buildTree(faction);
    TreeMetrics metrics = measureTree(tree, l, sc);
    const ResearchProgress researchNow = ResearchShown();
    const ResearchProgress *progress = &researchNow;
    m_researchSel = std::clamp(m_researchSel, 0, kResearchCount - 1);
    if (!tree.shown[m_researchSel] && tree.first >= 0)
        m_researchSel = tree.first;

    // The window covers the map as well: every wheel step and left press belongs to it cleanly once per frame
    const float wheel = input.ConsumeWheelF();
    const float wheelX = input.ConsumeWheelXF();
    input.ConsumeUiWheelF();
    const bool down = input.IsLeftMouseDown();
    int pressX = 0, pressY = 0;
    const bool pressed = input.ConsumeLeftPress(pressX, pressY) && down;

    // The open filter list takes priority over underlying widgets
    if (m_researchCombo.open == 0)
    {
        int picked = -1;
        if (ui::ComboUpdate(
                m_researchCombo,
                l.filter,
                static_cast<int>(filterChoices().size()),
                windowH,
                mx,
                my,
                leftClicked,
                wheel,
                picked,
                down,
                pressed,
                pressX,
                pressY))
        {
            if (picked >= 0)
                m_researchFilter = picked;
            return true;
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            m_researchCombo.open = -1;
            return true;
        }
    }

    // The search box
    if (m_researchSearchFocus)
    {
        for (int n = input.TakeBackspaces(); n > 0; --n)
            popUtf8(m_researchSearch);
        m_researchSearch += input.TakeTypedText();
        while (m_researchSearch.size() > 40)
            popUtf8(m_researchSearch);
        if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE) || input.IsKeyJustPressed(SDL_SCANCODE_RETURN)
            || input.IsKeyJustPressed(SDL_SCANCODE_KP_ENTER))
        {
            m_researchSearchFocus = false;
            return true;
        }
    }
    else if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        CloseResearchWindow();
        return true;
    }

    // The tree scrolls with the wheel (Shift: sideways), the scroll bars and by dragging the background
    float treeW = metrics.contentW;
    float treeH = metrics.contentH;
    float maxX = std::max(0.0f, treeW - l.tree.w);
    float maxY = std::max(0.0f, treeH - l.tree.h);
    auto applyZoom = [&](float nextScale) {
        const float before = m_researchScale;
        m_researchScale = std::clamp(std::round(nextScale / kScaleStep) * kScaleStep, kScaleMin, kScaleMax);
        const float k = m_researchScale / before;
        m_researchScroll.offsetX *= k;
        m_researchScroll.offsetY *= k;
        m_researchScroll.targetX *= k;
        m_researchScroll.targetY *= k;
        m_researchScroll.StopGlide();
        metrics = measureTree(tree, l, m_researchScale * l.u);
        treeW = metrics.contentW;
        treeH = metrics.contentH;
        maxX = std::max(0.0f, treeW - l.tree.w);
        maxY = std::max(0.0f, treeH - l.tree.h);
    };

    const UiRect vTrack = researchVTrack(l.tree), hTrack = researchHTrack(l.tree);
    const UiRect zoomReset = zoomResetRect(l);
    const bool onZoomWidget = l.zoomIn.Contains(pressX, pressY) || l.zoomOut.Contains(pressX, pressY)
                              || (zoomReset.w > 0 && zoomReset.Contains(pressX, pressY));
    if (pressed)
    {
        if (!l.touch && maxY > 0.0f && ui::OnScrollTrack(vTrack, pressX, pressY))
            m_researchScrollDrag = 1;
        else if (!l.touch && maxX > 0.0f && ui::OnScrollTrack(hTrack, pressX, pressY))
            m_researchScrollDrag = 2;
        if (m_researchScrollDrag != 0)
        {
            m_researchScroll.StopGlide();
            m_researchScroll.dragging = false;
            m_researchScroll.dragMoved = false;
        }
    }
    const bool scrollDragFrame = m_researchScrollDrag != 0;
    if (scrollDragFrame)
    {
        if (m_researchScrollDrag == 1)
        {
            m_researchScroll.offsetY = m_researchScroll.targetY
                = std::clamp(ui::ScrollPosFromMouse(vTrack, my, treeH, static_cast<float>(l.tree.h)), 0.0f, maxY);
        }
        else if (m_researchScrollDrag == 2)
        {
            m_researchScroll.offsetX = m_researchScroll.targetX
                = std::clamp(ui::ScrollPosFromMouse(hTrack, mx, treeW, static_cast<float>(l.tree.w)), 0.0f, maxX);
        }
        m_researchScroll.lastTickNs = SDL_GetTicksNS();
        if (!down)
            m_researchScrollDrag = 0;
        return true;
    }

    const bool shift = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
    const bool ctrl = input.isKeyPressed(SDL_SCANCODE_LCTRL) || input.isKeyPressed(SDL_SCANCODE_RCTRL);
    float scrollWheelX = wheelX;
    float scrollWheelY = wheel;
    if (ctrl && l.tree.Contains(mx, my))
    {
        if (wheel != 0.0f)
            applyZoom(m_researchScale + wheel * kScaleStep);
        scrollWheelX = 0.0f;
        scrollWheelY = 0.0f;
    }
    else if (shift || (maxY <= 0.0f && maxX > 0.0f))
    {
        scrollWheelX += wheel;
        scrollWheelY = 0.0f;
    }

    const bool wasDragging = m_researchScroll.dragging;
    ui::StepKineticScroll2D(
        m_researchScroll,
        l.tree,
        maxX,
        maxY,
        60.0f,
        60.0f,
        mx,
        my,
        down,
        pressed,
        pressX,
        pressY,
        scrollWheelX,
        scrollWheelY,
        !onZoomWidget);
    if (m_researchScroll.dragMoved)
        leftClicked = false;
    if (wasDragging && !m_researchScroll.dragging && m_researchScroll.dragMoved)
        m_researchScroll.dragMoved = false;

    if (leftClicked)
    {
        const UiRect clearBtn = searchClearRect(l);
        if (!m_researchSearch.empty() && clearBtn.Contains(mx, my))
        {
            m_researchSearch.clear();
            m_researchSearchFocus = true;
            SoundRequests::Play("ui_click");
            return true;
        }
        m_researchSearchFocus = l.search.Contains(mx, my);
        if (l.closeBtn.Contains(mx, my))
        {
            CloseResearchWindow();
            SoundRequests::Play("ui_confirm");
            return true;
        }
        if (l.zoomIn.Contains(mx, my) || l.zoomOut.Contains(mx, my))
        {
            applyZoom(m_researchScale + (l.zoomIn.Contains(mx, my) ? kScaleStep : -kScaleStep));
            m_researchScroll.offsetX = m_researchScroll.targetX = std::clamp(m_researchScroll.offsetX, 0.0f, maxX);
            m_researchScroll.offsetY = m_researchScroll.targetY = std::clamp(m_researchScroll.offsetY, 0.0f, maxY);
            SoundRequests::Play("ui_click");
            return true;
        }
        if (zoomReset.w > 0 && zoomReset.Contains(mx, my))
        {
            applyZoom(1.0f);
            m_researchScroll.offsetX = m_researchScroll.targetX = std::clamp(m_researchScroll.offsetX, 0.0f, maxX);
            m_researchScroll.offsetY = m_researchScroll.targetY = std::clamp(m_researchScroll.offsetY, 0.0f, maxY);
            SoundRequests::Play("ui_click");
            return true;
        }
        if (l.filter.Contains(mx, my))
        {
            m_researchCombo.open = 0;
            m_researchCombo.scroll = 0;
            return true;
        }
        if (l.startBtn.Contains(mx, my))
        {
            TryStartResearch(m_researchSel, economy, entities);
            return true;
        }
        if (tree.shown[m_researchSel])
        {
            const int need = researchDef(faction, m_researchSel).needs;
            if (need >= 0 && need < kResearchCount && tree.shown[need] && detailsReqRect(l).Contains(mx, my))
            {
                m_researchSel = need;
                const UiRect n0 = nodeRect(tree, metrics, need, l.tree, 0.0f, 0.0f);
                m_researchScroll.offsetX = m_researchScroll.targetX
                    = std::clamp(static_cast<float>(n0.CenterX() - l.tree.CenterX()), 0.0f, maxX);
                m_researchScroll.offsetY = m_researchScroll.targetY
                    = std::clamp(static_cast<float>(n0.CenterY() - l.tree.CenterY()), 0.0f, maxY);
                m_researchScroll.StopGlide();
                m_researchClickNode = -1;
                SoundRequests::Play("ui_click");
                return true;
            }
        }
        if (l.tree.Contains(mx, my))
        {
            for (int i = 0; i < kResearchCount; ++i)
            {
                if (!tree.shown[i])
                    continue;
                const UiRect n = nodeRect(tree, metrics, i, l.tree, m_researchScroll.offsetX, m_researchScroll.offsetY);
                if (!n.Contains(mx, my))
                    continue;
                const std::uint32_t now = SDL_GetTicks();
                m_researchSel = i;
                if (m_researchClickNode == i && now - m_researchClickMs < 400 && TryStartResearch(i, economy, entities))
                {
                    m_researchClickNode = -1;
                    CloseResearchWindow();
                    return true;
                }
                m_researchClickNode = i;
                m_researchClickMs = now;
                SoundRequests::Play("ui_click");
                break;
            }
        }
        else if (!l.dialog.Contains(mx, my))
        {
            CloseResearchWindow();
        }
    }
    (void) progress;
    return true;
}

namespace
{
struct CardPalette
{
    SDL_Color topFill;
    SDL_Color bottomFill;
    SDL_Color border;
    SDL_Color spine;
    SDL_Color badgeFill;
    SDL_Color badgeText;
};

CardPalette paletteForState(State st, bool affordable)
{
    switch (st)
    {
        case State::Done:
            return {{30, 52, 36, 255}, {20, 35, 24, 255}, col::Ok, {96, 214, 132, 255}, {22, 48, 30, 255}, col::Ok};
        case State::Active:
            return {
                {58, 43, 21, 255}, {36, 27, 14, 255}, col::Accent, {246, 200, 88, 255}, {64, 46, 18, 255}, col::Accent};
        case State::Available:
            return {
                {40, 31, 23, 255},
                {26, 20, 15, 255},
                affordable ? SDL_Color {148, 110, 70, 255} : SDL_Color {145, 84, 72, 255},
                affordable ? col::Accent : col::Danger,
                {24, 18, 14, 255},
                affordable ? col::Accent : col::Danger};
        case State::Locked:
        default:
            return {
                {25, 20, 16, 255},
                {17, 14, 11, 255},
                {68, 52, 39, 255},
                {82, 65, 50, 255},
                {19, 15, 12, 255},
                col::TextMuted};
    }
}

void drawTitle2Lines(
    BitmapFontRenderer &font, std::string_view text, int x, int y, int boxH, int maxW, int fontId, SDL_Color color)
{
    const int lineH = std::max(14, font.GetTextHeight(fontId));
    if (font.GetTextWidth(text, fontId) <= maxW)
    {
        font.DrawString(text, x, y + boxH / 2, fontId, color, false, Anchor::LEFT | Anchor::VCENTER);
        return;
    }
    std::string line1;
    std::string_view rem = text;
    std::size_t bestSplit = std::string_view::npos;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != ' ')
            continue;
        if (font.GetTextWidth(text.substr(0, i), fontId) <= maxW)
            bestSplit = i;
        else
            break;
    }
    if (bestSplit != std::string_view::npos)
    {
        line1 = std::string(text.substr(0, bestSplit));
        rem = text.substr(bestSplit + 1);
    }
    else
    {
        line1 = ui::FitText(font, text, fontId, maxW);
        rem = {};
    }
    const std::string line2 = rem.empty() ? std::string {} : ui::FitText(font, rem, fontId, maxW);
    const int totalH = line2.empty() ? lineH : lineH * 2 + 1;
    const int topY = y + std::max(0, (boxH - totalH) / 2);
    font.DrawString(line1, x, topY, fontId, color, false);
    if (!line2.empty())
        font.DrawString(line2, x, topY + lineH + 1, fontId, color, false);
}

void drawBlueprintBackdrop(Renderer &renderer, const UiRect &view, float scrollX, float scrollY, float sc)
{
    ui::FillVerticalGradient(renderer, view, {23, 18, 14, 255}, {12, 10, 8, 255});
    const int minorStep = std::max(28, sz(40, sc));
    const int majorStep = minorStep * 4;
    const int offX = static_cast<int>(scrollX);
    const int offY = static_cast<int>(scrollY);
    auto firstGridCoord = [](int startScreen, int scroll, int step) {
        const int rem = scroll % step;
        return startScreen - (rem >= 0 ? rem : rem + step);
    };
    const int xStart = firstGridCoord(view.x, offX, minorStep);
    const int yStart = firstGridCoord(view.y, offY, minorStep);
    const SDL_Color minorColor {38, 30, 23, 85};
    const SDL_Color majorColor {56, 43, 31, 130};
    for (int x = xStart; x <= view.Right(); x += minorStep)
    {
        const bool major = ((x - view.x + offX) % majorStep) == 0;
        renderer.DrawLineScreen(x, view.y, x, view.Bottom(), major ? majorColor : minorColor);
    }
    for (int y = yStart; y <= view.Bottom(); y += minorStep)
    {
        const bool major = ((y - view.y + offY) % majorStep) == 0;
        renderer.DrawLineScreen(view.x, y, view.Right(), y, major ? majorColor : minorColor);
    }
    const int xMaj = firstGridCoord(view.x, offX, majorStep);
    const int yMaj = firstGridCoord(view.y, offY, majorStep);
    const SDL_Color tickColor {108, 82, 56, 155};
    for (int x = xMaj; x <= view.Right(); x += majorStep)
    {
        for (int y = yMaj; y <= view.Bottom(); y += majorStep)
        {
            renderer.DrawLineScreen(x - 4, y, x + 4, y, tickColor);
            renderer.DrawLineScreen(x, y - 4, x, y + 4, tickColor);
        }
    }
}

void drawStatusBadge(
    Renderer &renderer,
    BitmapFontRenderer &font,
    const UiRect &rect,
    std::string_view label,
    int fontId,
    SDL_Color fill,
    SDL_Color border,
    SDL_Color textColor)
{
    renderer.FillRectScreen(rect.x, rect.y, rect.w, rect.h, fill);
    renderer.DrawRectScreen(rect.x, rect.y, rect.w, rect.h, border);
    font.DrawString(label, rect.CenterX(), rect.CenterY(), fontId, textColor, false, Anchor::HCENTER | Anchor::VCENTER);
}

void drawStatTile(
    Renderer &renderer,
    BitmapFontRenderer &font,
    const UiRect &rect,
    std::string_view caption,
    std::string_view value,
    SDL_Color valueColor,
    bool touch)
{
    ui::FillVerticalGradient(renderer, rect, {26, 20, 15, 255}, {17, 14, 11, 255});
    renderer.DrawRectScreen(rect.x, rect.y, rect.w, rect.h, col::Border);
    renderer.FillRectScreen(rect.x + 1, rect.y + 1, rect.w - 2, 1, {72, 54, 40, 255});
    font.DrawString(caption, rect.x + 10, rect.y + 7, ui::FONT_SMALL, col::TextMuted, false);
    font.DrawString(
        value,
        rect.x + 10,
        rect.Bottom() - (touch ? 28 : 22),
        touch ? ui::FONT_H2 : ui::FONT_BODY_BOLD,
        valueColor,
        false);
}

std::string buildStatusCallout(
    std::string_view hudNotification,
    Faction faction,
    State st,
    bool hasLab,
    bool otherActive,
    bool canAfford,
    const ResearchDef &r)
{
    if (!hudNotification.empty())
        return std::string(hudNotification);
    if (!hasLab)
        return faction == Faction::Confederation ? std::string(_("Build a Tech Center to start research"))
                                                 : std::string(_("Build a Laboratory to start research"));
    if (st == State::Done)
        return std::string(_("THIS TECHNOLOGY IS ALREADY RESEARCHED!"));
    if (st == State::Active)
        return std::string(_("RESEARCHING"));
    if (st == State::Locked && r.needs >= 0)
        return std::string(_("REQUIRES: ")) + _(researchDef(faction, r.needs).name);
    if (otherActive)
        return std::string(_("ANOTHER RESEARCH IS ALREADY IN PROGRESS!"));
    if (!canAfford)
        return std::string(_("INSUFFICIENT CREDITS FOR RESEARCH!"));
    return {};
}
} // namespace

void HUDController::RenderResearchWindow(
    Renderer &renderer,
    BitmapFontRenderer &font,
    const Economy &economy,
    const EntityManager &entities,
    int mx,
    int my,
    int windowW,
    int windowH)
{
    if (!m_researchOpen)
        return;
    const WindowLayout l = windowLayout(windowW, windowH, m_observer ? m_observerBarHeight : 0);
    const float sc = m_researchScale * l.u;                       // (the touch profile: bigger with the screen)
    const Faction faction = Players::FactionOf(Players::Local()); // (an observer: the watched player's)
    const TreeLayout tree = buildTree(faction);
    const TreeMetrics metrics = measureTree(tree, l, sc);
    const ResearchProgress researchNow = ResearchShown();
    const ResearchProgress *progress = &researchNow;
    const bool hasLab = HasResearchBuilding(Players::Local(), entities);
    auto over = [&](const UiRect &r) {
        return !l.touch && r.Contains(mx, my);
    };
    const int nodeNameFont = (l.touch || sc >= 1.15f) ? ui::FONT_BODY_BOLD : ui::FONT_SMALL_BOLD;
    const int nodeStateFont = l.touch ? ui::FONT_BODY_BOLD : ui::FONT_SMALL_BOLD;
    const int textFont = ui::FONT_BODY;

    ui::DrawDialog(
        renderer,
        font,
        windowW,
        windowH,
        l.dialog,
        faction == Faction::Confederation ? _("TECH CENTER - RESEARCH") : _("LABORATORY - RESEARCH"));

    // Search bar with clear 'X' button
    ui::FillVerticalGradient(renderer, l.search, {20, 16, 12, 255}, {14, 11, 9, 255});
    renderer.DrawRectScreen(
        l.search.x, l.search.y, l.search.w, l.search.h, m_researchSearchFocus ? col::Accent : col::BorderStrong);
    const UiRect clearBtn = searchClearRect(l);
    const int searchMaxW = l.search.w - 20 - (m_researchSearch.empty() ? 0 : clearBtn.w + 8);
    const bool blink = (SDL_GetTicks() / 500) % 2 == 0;
    const std::string shown = m_researchSearch.empty() && !m_researchSearchFocus
                                  ? std::string(_("Search technology..."))
                                  : m_researchSearch + (m_researchSearchFocus && blink ? "|" : "");
    font.DrawString(
        ui::FitText(font, shown, ui::FONT_BODY, std::max(40, searchMaxW)),
        l.search.x + 10,
        l.search.CenterY(),
        ui::FONT_BODY,
        m_researchSearch.empty() && !m_researchSearchFocus ? col::TextMuted : col::Text,
        false,
        Anchor::LEFT | Anchor::VCENTER);
    if (!m_researchSearch.empty())
    {
        const bool clearHot = over(clearBtn);
        renderer.FillRectScreen(
            clearBtn.x, clearBtn.y, clearBtn.w, clearBtn.h, clearHot ? col::Hover : SDL_Color {38, 29, 22, 255});
        renderer.DrawRectScreen(clearBtn.x, clearBtn.y, clearBtn.w, clearBtn.h, clearHot ? col::Accent : col::Border);
        font.DrawString(
            "X",
            clearBtn.CenterX(),
            clearBtn.CenterY(),
            ui::FONT_SMALL_BOLD,
            col::Text,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
    }

    const std::vector<std::string> filters = filterChoices();
    ui::DrawComboField(
        renderer,
        font,
        l.filter,
        std::string(_("Show")) + ": " + filters[std::clamp(m_researchFilter, 0, 4)],
        m_researchCombo.open == 0,
        over(l.filter));
    ui::DrawButton(renderer, font, l.closeBtn, _("CLOSE"), ui::ButtonStyle::Secondary, over(l.closeBtn));

    // Credits pill cleanly positioned right of the filter combo
    const std::string credStr = "$ " + std::to_string(economy.GetCredits());
    const int credW = std::max(106, font.GetTextWidth(credStr, ui::FONT_H2) + 28);
    const int credSpace = l.closeBtn.x - 12 - (l.filter.Right() + 12);
    if (credSpace >= 76)
    {
        const UiRect credRect {
            l.closeBtn.x - 12 - std::min(credW, credSpace), l.closeBtn.y, std::min(credW, credSpace), l.closeBtn.h};
        ui::FillVerticalGradient(renderer, credRect, {30, 23, 17, 255}, {18, 14, 10, 255});
        renderer.DrawRectScreen(credRect.x, credRect.y, credRect.w, credRect.h, col::BorderStrong);
        font.DrawString(
            credStr,
            credRect.CenterX(),
            credRect.CenterY(),
            ui::FONT_H2,
            col::Accent,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
    }

    // Filter matcher
    const std::string needle = lowerUtf8(m_researchSearch);
    auto matches = [&](int i) {
        const auto &r = researchDef(faction, i);
        if (!needle.empty() && lowerUtf8(_(r.name)).find(needle) == std::string::npos
            && lowerUtf8(_(r.desc)).find(needle) == std::string::npos)
            return false;
        const State st = stateOf(i, progress);
        switch (m_researchFilter)
        {
            case 1:
                return st == State::Available;
            case 2:
                return st == State::Done;
            case 3:
                return st == State::Locked;
            case 4:
                return st == State::Active;
            default:
                return true;
        }
    };

    // Tactical blueprint viewport
    const SDL_Rect innerTree {l.tree.x + 1, l.tree.y + 1, l.tree.w - 2, l.tree.h - 2};
    const bool softClip = uilayout::MenuProfile(uilayout::Current(), "confirm") == "touch"
                          && renderer.BeginClipTarget(innerTree, {16, 13, 10, 255});
    if (!softClip)
        renderer.SetClipRect(innerTree);

    drawBlueprintBackdrop(
        renderer,
        {innerTree.x, innerTree.y, innerTree.w, innerTree.h},
        m_researchScroll.offsetX,
        m_researchScroll.offsetY,
        sc);

    // Division headers & completion progress
    int colDone[3] {0, 0, 0};
    for (int i = 0; i < kResearchCount; ++i)
    {
        if (tree.shown[i] && IsResearched(Players::Local(), i))
            colDone[std::clamp(tree.division[i], 0, 2)] += 1;
    }
    const int scrollOffX = static_cast<int>(std::lround(m_researchScroll.offsetX));
    const int scrollOffY = static_cast<int>(std::lround(m_researchScroll.offsetY));
    for (int c = 0; c < 3; ++c)
    {
        if (!metrics.colShown[c])
            continue;
        const UiRect hdr {
            l.tree.x + metrics.originX + metrics.colX[c] - scrollOffX,
            l.tree.y + metrics.padY - scrollOffY,
            metrics.colW[c],
            metrics.headerH};
        if (hdr.Right() < l.tree.x || hdr.x > l.tree.Right() || hdr.Bottom() < l.tree.y || hdr.y > l.tree.Bottom())
            continue;
        ui::DrawButtonChrome(
            renderer, hdr, {44, 34, 24, 255}, {28, 22, 16, 255}, col::BorderStrong, col::Accent, col::BorderStrong);
        font.DrawString(
            _(divisionTitle(c)),
            hdr.x + 12,
            hdr.CenterY(),
            l.touch ? ui::FONT_BODY_BOLD : ui::FONT_SMALL_BOLD,
            col::Accent,
            false,
            Anchor::LEFT | Anchor::VCENTER);
        const std::string tally = std::to_string(colDone[c]) + "/" + std::to_string(tree.colCount[c]);
        font.DrawString(
            tally,
            hdr.Right() - 10,
            hdr.CenterY(),
            ui::FONT_SMALL_BOLD,
            colDone[c] == tree.colCount[c] ? col::Ok : col::TextDim,
            false,
            Anchor::RIGHT | Anchor::VCENTER);
    }

    // Circuit prerequisite traces (orthogonal 2px bus from parent bottom-left port into indented child left port)
    const float linkWidth = std::max(2.0f, 2.0f * sc);
    for (int i = 0; i < kResearchCount; ++i)
    {
        if (!tree.shown[i] || tree.parent[i] < 0)
            continue;
        const UiRect a = nodeRect(tree, metrics, tree.parent[i], l.tree, m_researchScroll.offsetX, m_researchScroll.offsetY);
        const UiRect b = nodeRect(tree, metrics, i, l.tree, m_researchScroll.offsetX, m_researchScroll.offsetY);
        const int busX = a.x + std::min(metrics.indentX / 2, std::max(7, sz(9, sc)));
        const bool hot = i == m_researchSel || tree.parent[i] == m_researchSel;
        const bool done = IsResearched(Players::Local(), tree.parent[i]);
        const SDL_Color c = hot ? col::Accent : (done ? col::Ok : SDL_Color {112, 86, 63, 255});
        renderer.DrawThickLineScreen(
            static_cast<float>(busX),
            static_cast<float>(a.Bottom()),
            static_cast<float>(busX),
            static_cast<float>(b.CenterY()),
            linkWidth,
            c);
        renderer.DrawThickLineScreen(
            static_cast<float>(busX),
            static_cast<float>(b.CenterY()),
            static_cast<float>(b.x),
            static_cast<float>(b.CenterY()),
            linkWidth,
            c);
        renderer.FillRectScreen(busX - 2, a.Bottom() - 1, 5, 5, c);
        renderer.FillRectScreen(busX - 2, b.CenterY() - 2, 4, 4, c);
        renderer.FillRectScreen(b.x - 3, b.CenterY() - 2, 5, 5, c);
    }

    // Technology cards
    const int iconPad = std::max(5, sz(6, sc));
    const int pillH = std::max(18, sz(20, sc));
    for (int i = 0; i < kResearchCount; ++i)
    {
        if (!tree.shown[i])
            continue;
        const UiRect n = nodeRect(tree, metrics, i, l.tree, m_researchScroll.offsetX, m_researchScroll.offsetY);
        if (n.Right() < l.tree.x || n.x > l.tree.Right() || n.Bottom() < l.tree.y || n.y > l.tree.Bottom())
            continue;
        const auto &r = researchDef(faction, i);
        const State st = stateOf(i, progress);
        const bool affordable = economy.CanAfford(r.cost);
        const bool selected = i == m_researchSel;
        const bool hovered = over(n) && l.tree.Contains(mx, my);
        const bool lit = matches(i);

        CardPalette pal = paletteForState(st, affordable);
        if (hovered)
        {
            pal.topFill = ui::Mix(pal.topFill, col::Accent, 0.14f);
            pal.bottomFill = ui::Mix(pal.bottomFill, col::Accent, 0.08f);
        }
        ui::DrawButtonChrome(
            renderer,
            n,
            pal.topFill,
            pal.bottomFill,
            selected ? col::Text : pal.border,
            pal.spine,
            selected ? col::Accent : pal.border);
        if (selected)
            renderer.DrawRectScreen(n.x + 1, n.y + 1, n.w - 2, n.h - 2, pal.border);

        const int iconSize = std::max(28, n.h - iconPad * 2);
        const UiRect iconFrame {n.x + iconPad + 4, n.y + iconPad, iconSize, iconSize};
        renderer.FillRectScreen(iconFrame.x, iconFrame.y, iconFrame.w, iconFrame.h, {12, 10, 8, 255});
        renderer.DrawRectScreen(iconFrame.x, iconFrame.y, iconFrame.w, iconFrame.h, col::Border);
        const int innerPad = std::clamp(sz(4, sc), 3, std::max(3, iconSize / 8));
        renderer.SetDrawOwner(Players::Local());
        renderer.SetStructureFaction(faction);
        ui::DrawCompositeFitted(renderer, r.compId, iconFrame, innerPad, faction);
        renderer.SetStructureFaction(Faction::Neutral);
        renderer.SetDrawOwner(kNoPlayer);

        const int textX = iconFrame.Right() + std::max(8, sz(9, sc));
        const int textW = std::max(36, n.Right() - 8 - textX);
        const int pillY = n.Bottom() - iconPad - pillH;
        const int titleH = std::max(16, pillY - (n.y + iconPad) - 2);
        drawTitle2Lines(
            font, _(r.name), textX, n.y + iconPad, titleH, textW, nodeNameFont, lit ? col::Text : col::TextMuted);

        std::string badgeLabel;
        if (st == State::Done)
            badgeLabel = std::string(_("RESEARCHED"));
        else if (st == State::Active)
        {
            const int pct = progress ? std::clamp(
                                           static_cast<int>(std::lround(
                                               100.0f * progress->timer / std::max(0.1f, progress->duration))),
                                           0,
                                           100)
                                     : 0;
            badgeLabel = std::string(_("RESEARCHING")) + " " + std::to_string(pct) + "%";
        }
        else if (st == State::Locked)
            badgeLabel = std::string(_("LOCKED"));
        else
            badgeLabel = "$" + std::to_string(r.cost);

        const int timeW
            = st == State::Done
                  ? 0
                  : font.GetTextWidth(std::to_string(static_cast<int>(r.seconds)) + "s", ui::FONT_SMALL) + 6;
        const int badgeW
            = std::clamp(font.GetTextWidth(badgeLabel, nodeStateFont) + 12, 42, std::max(42, textW - timeW));
        drawStatusBadge(
            renderer,
            font,
            {textX, pillY, badgeW, pillH},
            ui::FitText(font, badgeLabel, nodeStateFont, badgeW - 6),
            nodeStateFont,
            pal.badgeFill,
            pal.border,
            pal.badgeText);

        if (st != State::Done && textW - badgeW >= 34)
        {
            font.DrawString(
                std::to_string(static_cast<int>(r.seconds)) + " " + _("s"),
                n.Right() - 8,
                pillY + pillH / 2,
                ui::FONT_SMALL,
                col::TextMuted,
                false,
                Anchor::RIGHT | Anchor::VCENTER);
        }

        if (st == State::Active && progress)
        {
            ui::DrawProgressBar(
                renderer,
                {iconFrame.x, n.Bottom() - 4, n.Right() - 6 - iconFrame.x, 3},
                progress->timer / std::max(0.1f, progress->duration),
                col::Accent);
        }
        if (!lit)
            renderer.FillRectScreen(n.x, n.y, n.w, n.h, {10, 8, 6, 185});
    }

    if (softClip)
        renderer.EndClipTarget();
    else
        renderer.ResetClipRect();

    ui::DrawPanelFrame(renderer, l.tree, col::BorderStrong);

    // Scroll bars
    {
        const UiRect vTrack = researchVTrack(l.tree), hTrack = researchHTrack(l.tree);
        ui::DrawScrollBar(
            renderer,
            vTrack,
            m_researchScroll.offsetY,
            metrics.contentH,
            static_cast<float>(l.tree.h),
            m_researchScrollDrag == 1 || ui::OnScrollTrack(vTrack, mx, my));
        ui::DrawScrollBar(
            renderer,
            hTrack,
            m_researchScroll.offsetX,
            metrics.contentW,
            static_cast<float>(l.tree.w),
            m_researchScrollDrag == 2 || ui::OnScrollTrack(hTrack, mx, my));
    }

    // Floating zoom bar [ - ] [ 100% ] [ + ]
    const UiRect zoomReset = zoomResetRect(l);
    if (zoomReset.w > 0)
    {
        const UiRect dock {l.zoomOut.x - 4, l.zoomOut.y - 4, l.zoomIn.Right() - l.zoomOut.x + 8, l.zoomOut.h + 8};
        ui::FillVerticalGradient(renderer, dock, {26, 20, 16, 238}, {15, 12, 9, 238});
        renderer.DrawRectScreen(dock.x, dock.y, dock.w, dock.h, col::BorderStrong);
        const bool pctHot = over(zoomReset);
        ui::FillVerticalGradient(
            renderer,
            zoomReset,
            pctHot ? SDL_Color {56, 42, 30, 255} : SDL_Color {28, 22, 17, 255},
            pctHot ? SDL_Color {38, 28, 20, 255} : SDL_Color {18, 14, 11, 255});
        renderer.DrawRectScreen(zoomReset.x, zoomReset.y, zoomReset.w, zoomReset.h, pctHot ? col::Accent : col::Border);
        font.DrawString(
            std::to_string(static_cast<int>(std::lround(m_researchScale * 100.0f))) + "%",
            zoomReset.CenterX(),
            zoomReset.CenterY(),
            l.touch ? ui::FONT_BODY_BOLD : ui::FONT_SMALL_BOLD,
            pctHot ? col::Accent : col::Text,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
    }
    ui::DrawButton(renderer, font, l.zoomOut, "-", ui::ButtonStyle::Secondary, over(l.zoomOut));
    ui::DrawButton(renderer, font, l.zoomIn, "+", ui::ButtonStyle::Secondary, over(l.zoomIn));

    // Selected Technology Dossier Panel
    ui::DrawPanel(renderer, l.details, col::PanelRaised, col::BorderStrong);
    if (const int i = std::clamp(m_researchSel, 0, kResearchCount - 1); tree.shown[i])
    {
        const auto &r = researchDef(faction, i);
        const State st = stateOf(i, progress);
        const bool affordable = economy.CanAfford(r.cost);
        const CardPalette pal = paletteForState(st, affordable);
        const int pad = l.touch ? 16 : 14;
        const int x = l.details.x + pad;
        const int w = l.details.w - pad * 2;
        int y = l.details.y + pad;

        // 1. Header block (pedestal + division + title + state pill)
        const int iconSize = l.touch ? 84 : 74;
        const int headerH = l.touch ? 92 : 82;
        const UiRect iconFrame {x, y, iconSize, iconSize};
        ui::FillVerticalGradient(renderer, iconFrame, {22, 17, 13, 255}, {12, 10, 8, 255});
        renderer.DrawRectScreen(iconFrame.x, iconFrame.y, iconFrame.w, iconFrame.h, pal.border);
        renderer.SetDrawOwner(Players::Local());
        renderer.SetStructureFaction(faction);
        ui::DrawCompositeFitted(renderer, r.compId, iconFrame, 5, faction);
        renderer.SetStructureFaction(Faction::Neutral);
        renderer.SetDrawOwner(kNoPlayer);

        const int hx = iconFrame.Right() + 12;
        const int hw = std::max(60, w - iconSize - 12);
        font.DrawString(
            ui::FitText(font, _(divisionTitle(tree.division[i])), ui::FONT_SMALL_BOLD, hw),
            hx,
            y,
            ui::FONT_SMALL_BOLD,
            col::Accent,
            false);
        const int titleFont = l.touch ? ui::FONT_H2 : ui::FONT_BODY_BOLD;
        const int pillDossierH = l.touch ? 24 : 20;
        drawTitle2Lines(
            font, _(r.name), hx, y + 16, std::max(24, headerH - 16 - pillDossierH - 4), hw, titleFont, col::Text);
        const std::string stateText = st == State::Done     ? std::string(_("RESEARCHED"))
                                      : st == State::Active ? std::string(_("RESEARCHING"))
                                      : st == State::Locked ? std::string(_("LOCKED"))
                                                            : std::string(_("AVAILABLE"));
        const int stateBadgeW = std::min(hw, std::max(84, font.GetTextWidth(stateText, ui::FONT_SMALL_BOLD) + 18));
        drawStatusBadge(
            renderer,
            font,
            {hx, y + headerH - pillDossierH, stateBadgeW, pillDossierH},
            stateText,
            ui::FONT_SMALL_BOLD,
            pal.badgeFill,
            pal.border,
            pal.badgeText);

        // 2. Stat Tiles (Cost + Time)
        y += headerH + 10;
        const int tileGap = 8;
        const int tileW = (w - tileGap) / 2;
        const int tileH = l.touch ? 60 : 52;
        drawStatTile(
            renderer,
            font,
            {x, y, tileW, tileH},
            _("Cost"),
            "$" + std::to_string(r.cost),
            (affordable || st == State::Done) ? col::Accent : col::Danger,
            l.touch);
        drawStatTile(
            renderer,
            font,
            {x + tileW + tileGap, y, w - tileW - tileGap, tileH},
            _("Time"),
            std::to_string(static_cast<int>(r.seconds)) + " " + _("s"),
            col::Text,
            l.touch);
        y += tileH + 10;

        // 3. Clickable Prerequisite Chip (matches detailsReqRect(l) exactly!)
        if (r.needs >= 0)
        {
            const UiRect reqRect = detailsReqRect(l);
            const bool reqDone = IsResearched(Players::Local(), r.needs);
            const bool reqHot = over(reqRect) && tree.shown[r.needs];
            const SDL_Color reqEdge = reqHot ? col::Accent : (reqDone ? col::Ok : col::Danger);
            ui::DrawButtonChrome(
                renderer,
                reqRect,
                reqHot ? SDL_Color {46, 35, 25, 255} : SDL_Color {26, 20, 15, 255},
                reqHot ? SDL_Color {32, 24, 17, 255} : SDL_Color {17, 14, 11, 255},
                reqEdge,
                reqDone ? col::Ok : col::Danger,
                reqEdge);
            font.DrawString(_("Requires"), reqRect.x + 12, reqRect.y + 5, ui::FONT_SMALL, col::TextMuted, false);
            if (tree.shown[r.needs])
                font.DrawString(
                    ">",
                    reqRect.Right() - 10,
                    reqRect.CenterY(),
                    ui::FONT_SMALL_BOLD,
                    reqEdge,
                    false,
                    Anchor::RIGHT | Anchor::VCENTER);
            font.DrawString(
                ui::FitText(
                    font,
                    _(researchDef(faction, r.needs).name),
                    l.touch ? ui::FONT_BODY_BOLD : ui::FONT_SMALL_BOLD,
                    reqRect.w - 34),
                reqRect.x + 12,
                reqRect.Bottom() - (l.touch ? 22 : 18),
                l.touch ? ui::FONT_BODY_BOLD : ui::FONT_SMALL_BOLD,
                reqDone ? col::Ok : col::Danger,
                false);
            y = reqRect.Bottom() + 10;
        }

        // 4. Footer layout from Start button upwards (progress bar + callout banner)
        const bool otherActive = progress && progress->active >= 0 && progress->active != i;
        const bool canStart = st == State::Available && hasLab && affordable && progress && progress->active < 0;
        const std::string callout
            = buildStatusCallout(m_hudNotification, faction, st, hasLab, otherActive, affordable, r);
        int footerTop = l.startBtn.y - 10;
        if (st == State::Active && progress)
        {
            const int barH = 10;
            const UiRect barRect {x, footerTop - barH, w, barH};
            ui::DrawProgressBar(renderer, barRect, progress->timer / std::max(0.1f, progress->duration), col::Accent);
            footerTop = barRect.y - 8;
        }
        if (!callout.empty())
        {
            const int bannerH = l.touch ? 48 : 38;
            const UiRect banner {x, footerTop - bannerH, w, bannerH};
            const bool isError = !m_hudNotification.empty() || (!canStart && st != State::Done && st != State::Active);
            const SDL_Color bFill = st == State::Done
                                        ? SDL_Color {22, 42, 28, 255}
                                        : (isError ? SDL_Color {46, 22, 20, 255} : SDL_Color {42, 32, 22, 255});
            const SDL_Color bEdge = st == State::Done ? col::Ok : (isError ? col::Danger : col::Accent);
            renderer.FillRectScreen(banner.x, banner.y, banner.w, banner.h, bFill);
            renderer.DrawRectScreen(banner.x, banner.y, banner.w, banner.h, bEdge);
            font.DrawString(
                ui::FitText(font, callout, l.touch ? ui::FONT_BODY : ui::FONT_SMALL_BOLD, banner.w - 16),
                banner.CenterX(),
                banner.CenterY(),
                l.touch ? ui::FONT_BODY : ui::FONT_SMALL_BOLD,
                bEdge,
                false,
                Anchor::HCENTER | Anchor::VCENTER);
            footerTop = banner.y - 10;
        }

        // 5. Framed description box cleanly filling remaining dossier height
        const UiRect descBox {x, y, w, std::max(48, footerTop - y)};
        ui::FillVerticalGradient(renderer, descBox, {22, 17, 13, 255}, {15, 12, 9, 255});
        renderer.DrawRectScreen(descBox.x, descBox.y, descBox.w, descBox.h, col::Border);
        renderer.SetClipRect({descBox.x + 2, descBox.y + 2, descBox.w - 4, descBox.h - 4});
        font.DrawTextWrapped(_(r.desc), descBox.x + 10, descBox.y + 10, descBox.w - 20, textFont, col::Text, false);
        renderer.ResetClipRect();

        ui::DrawButton(
            renderer, font, l.startBtn, _("START RESEARCH"), ui::ButtonStyle::Primary, over(l.startBtn), canStart);
    }

    if (m_researchCombo.open == 0)
    {
        ui::DrawComboList(
            renderer, font, l.filter, filters, std::clamp(m_researchFilter, 0, 4), m_researchCombo, windowH, mx, my);
    }
}

} // namespace OpenArtOfWar
