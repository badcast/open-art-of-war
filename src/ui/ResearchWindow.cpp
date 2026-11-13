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

// Sizes of the tree at scale 1: the technologies of the tree (their size and icons) are drawn bigger by
// m_researchScale, the window itself keeps its size
constexpr int kNodeW0 = 270;
constexpr int kNodeH0 = 66;
constexpr float kIconScale0 = 0.9f;       // the icon of a technology in the tree (the picture is about 64 px)
constexpr int kIconBox0 = 62;             // ... and the room it takes on the left of the node
constexpr float kDetailsIconScale = 1.3f; // the icon of the selected technology
constexpr int kGapX0 = 34;
constexpr int kGapY0 = 10;
constexpr int kDetailsW0 = 270;
constexpr float kScaleMin = 0.75f, kScaleMax = 4.0f, kScaleStep = 0.125f;
// The glide of a flick on the tree after the finger lifts: px/s limit, the speed below which it stops, how fast it
// slows
constexpr float kGlideMax = 6000.0f, kGlideStop = 30.0f, kGlideFriction = 3.5f;
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
    windowLayout.dialog = ui::CenteredRect(windowW, windowH, windowW, windowH); // the whole window
    windowLayout.dialog.y += top;
    windowLayout.dialog.h -= top;
    windowLayout.content
        = {windowLayout.dialog.x + 16,
           windowLayout.dialog.y + 60,
           windowLayout.dialog.w - 32,
           windowLayout.dialog.h - 60 - 16};
    const int kDetailsW = std::clamp(static_cast<int>(windowLayout.content.w * 0.26f), kDetailsW0, 440);
    windowLayout.search
        = {windowLayout.content.x,
           windowLayout.content.y,
           std::clamp(static_cast<int>(windowLayout.content.w * 0.28f), 280, 460),
           30};
    windowLayout.filter
        = {windowLayout.search.Right() + 10,
           windowLayout.content.y,
           std::clamp(static_cast<int>(windowLayout.content.w * 0.18f), 200, 300),
           30};
    windowLayout.closeBtn = {windowLayout.content.Right() - 130, windowLayout.content.y, 130, 30};
    windowLayout.details = {
        windowLayout.content.Right() - kDetailsW, windowLayout.content.y + 42, kDetailsW, windowLayout.content.h - 42};
    windowLayout.tree
        = {windowLayout.content.x,
           windowLayout.content.y + 42,
           windowLayout.details.x - 12 - windowLayout.content.x,
           windowLayout.content.h - 42};
    windowLayout.startBtn
        = {windowLayout.details.x + 12, windowLayout.details.Bottom() - 52, windowLayout.details.w - 24, 40};
    // + / - at the bottom right of the tree, like the radar's zoom buttons
    windowLayout.zoomOut = {windowLayout.tree.Right() - 30, windowLayout.tree.Bottom() - 30, 24, 24};
    windowLayout.zoomIn = {windowLayout.tree.Right() - 30, windowLayout.tree.Bottom() - 58, 24, 24};
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

// Columns are the depth in the chain of requirements, rows are leaves in tree order, a parent sits in the middle of
// its children (as in the character editor)
struct TreeLayout
{
    std::vector<int> parent, depth;
    std::vector<float> row;
    std::vector<bool> shown; // the local player's tech level (data/capabilities.yaml) has it
    int first {-1};          // the first one shown
    int maxDepth {0};
    float rows {1.0f};
};

TreeLayout buildTree(Faction faction)
{
    const int count = kResearchCount;
    TreeLayout t;
    t.parent.assign(count, -1);
    t.depth.assign(count, 0);
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
    float nextRow = 0.0f;
    std::function<void(int, int)> place = [&](int node, int d) {
        t.depth[node] = d;
        if (children[node].empty())
        {
            t.row[node] = nextRow;
            nextRow += 1.0f;
            return;
        }
        for (int c : children[node])
            place(c, d + 1);
        t.row[node] = (t.row[children[node].front()] + t.row[children[node].back()]) * 0.5f;
    };
    for (int r : roots)
        place(r, 0);
    for (int d : t.depth)
        t.maxDepth = std::max(t.maxDepth, d);
    t.rows = std::max(1.0f, nextRow);
    return t;
}

[[maybe_unused]] std::string lower(const std::string &s)
{
    std::string out;
    for (unsigned char c : s)
        out += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
    return out;
}

// Case-insensitive for Cyrillic (UTF-8) too
std::string lowerUtf8(const std::string &t)
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
    m_researchDrag = m_researchDragMoved = false;
    m_researchGlideX = m_researchGlideY = 0.0f;
    m_researchTickMs = SDL_GetTicks();
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
    m_researchDrag = m_researchDragMoved = false;
    m_researchGlideX = m_researchGlideY = 0.0f;
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
    const WindowLayout l = windowLayout(windowW, windowH, m_observer ? kObserverBarHeight : 0);
    const float sc = m_researchScale * l.u; // (the touch profile: bigger with the screen)
    const int kNodeW = sz(kNodeW0, sc), kNodeH = sz(kNodeH0, sc), kGapX = sz(kGapX0, sc), kGapY = sz(kGapY0, sc);
    const Faction faction = Players::FactionOf(Players::Local()); // (an observer: the watched player's)
    const TreeLayout tree = buildTree(faction);
    const ResearchProgress researchNow = ResearchShown();
    const ResearchProgress *progress = &researchNow;
    m_researchSel = std::clamp(m_researchSel, 0, kResearchCount - 1);
    if (!tree.shown[m_researchSel] && tree.first >= 0)
        m_researchSel = tree.first;
    // The window covers the map as well: every wheel step is its own, over the map or the sidebar
    const int wheel = input.ConsumeWheel();
    input.ConsumeUiWheel();

    // The open filter list takes the mouse first
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
                picked))
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
    const float treeW = 16.0f + (tree.maxDepth + 1) * (kNodeW + kGapX);
    const float treeH = 16.0f + tree.rows * (kNodeH + kGapY);
    const float maxX = std::max(0.0f, treeW - l.tree.w), maxY = std::max(0.0f, treeH - l.tree.h);
    auto setScale = [&](float v) {
        m_researchScale = std::clamp(std::round(v / kScaleStep) * kScaleStep, kScaleMin, kScaleMax);
    };
    if (wheel != 0 && l.tree.Contains(mx, my))
    {
        // Over the tree the wheel scrolls it (Shift: sideways); Ctrl + wheel scales its contents (the window keeps
        // its size; the + / - buttons do that too)
        const bool shift = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
        const bool ctrl = input.isKeyPressed(SDL_SCANCODE_LCTRL) || input.isKeyPressed(SDL_SCANCODE_RCTRL);
        if (ctrl)
        {
            const float before = m_researchScale;
            setScale(m_researchScale + wheel * kScaleStep);
            const float k = m_researchScale / before;
            m_researchScrollX *= k;
            m_researchScrollY *= k;
        }
        else if (shift)
            m_researchScrollX -= wheel * 60.0f;
        else
            m_researchScrollY -= wheel * 60.0f;
    }
    // The tree follows the pointer (a finger, or the mouse held on it); the scroll bars along the right and the bottom
    // edge are grabbed by a press (not with the touch profile: the finger pans the tree itself), or a click on the
    // track jumps there
    const UiRect vTrack = researchVTrack(l.tree), hTrack = researchHTrack(l.tree);
    const std::uint32_t nowMs = SDL_GetTicks();
    const float dtS = std::clamp(static_cast<float>(nowMs - m_researchTickMs) * 0.001f, 0.0f, 0.1f);
    m_researchTickMs = nowMs;
    const bool down = input.IsLeftMouseDown();
    int pressX = 0, pressY = 0;
    if (input.ConsumeLeftPress(pressX, pressY) && down) // (a press from before the window opened is long over)
    {
        m_researchGlideX = m_researchGlideY = 0.0f; // a finger put on the tree stops the glide
        m_researchDragMoved = false;
        if (!l.touch && maxY > 0.0f && ui::OnScrollTrack(vTrack, pressX, pressY))
            m_researchScrollDrag = 1;
        else if (!l.touch && maxX > 0.0f && ui::OnScrollTrack(hTrack, pressX, pressY))
            m_researchScrollDrag = 2;
        m_researchDrag = m_researchScrollDrag == 0 && l.tree.Contains(pressX, pressY);
        m_researchDragX = pressX;
        m_researchDragY = pressY;
    }
    const bool wasDragging = m_researchDrag;
    if (m_researchDrag)
    {
        // Within the slop it is a tap (the touch input counts 12 px as well); past it the tree follows, the travel
        // since the press included, and the speed is kept (smoothed over a few frames) for the glide
        constexpr float kSlop = 12.0f;
        bool justMoved = false;
        if (!m_researchDragMoved
            && std::hypot(static_cast<float>(mx - m_researchDragX), static_cast<float>(my - m_researchDragY)) > kSlop)
        {
            m_researchDragMoved = true;
            justMoved = true;
        }
        if (m_researchDragMoved)
        {
            const float dx = static_cast<float>(mx - m_researchDragX), dy = static_cast<float>(my - m_researchDragY);
            m_researchScrollX -= dx;
            m_researchScrollY -= dy;
            m_researchDragX = mx;
            m_researchDragY = my;
            if (dtS > 0.0f && !justMoved)
            {
                const float k = std::min(1.0f, dtS / 0.08f);
                m_researchGlideX += (std::clamp(-dx / dtS, -kGlideMax, kGlideMax) - m_researchGlideX) * k;
                m_researchGlideY += (std::clamp(-dy / dtS, -kGlideMax, kGlideMax) - m_researchGlideY) * k;
            }
        }
        if (!down)
            m_researchDrag = false;
    }
    else if (m_researchGlideX != 0.0f || m_researchGlideY != 0.0f)
    {
        // After the lift the tree glides on at the speed of the flick and slows down
        if (std::hypot(m_researchGlideX, m_researchGlideY) < kGlideStop)
        {
            m_researchGlideX = m_researchGlideY = 0.0f;
        }
        else
        {
            m_researchScrollX += m_researchGlideX * dtS;
            m_researchScrollY += m_researchGlideY * dtS;
            const float keep = std::exp(-dtS * kGlideFriction);
            m_researchGlideX *= keep;
            m_researchGlideY *= keep;
        }
    }
    // A scroll bar that is held follows the pointer (also on the frame of the release)
    const bool scrollDragFrame = m_researchScrollDrag != 0;
    if (m_researchScrollDrag == 1)
        m_researchScrollY = ui::ScrollPosFromMouse(vTrack, my, treeH, static_cast<float>(l.tree.h));
    else if (m_researchScrollDrag == 2)
        m_researchScrollX = ui::ScrollPosFromMouse(hTrack, mx, treeW, static_cast<float>(l.tree.w));
    if (!down)
        m_researchScrollDrag = 0;
    // Against an edge the glide stops
    const float scrollX = std::clamp(m_researchScrollX, 0.0f, maxX),
                scrollY = std::clamp(m_researchScrollY, 0.0f, maxY);
    if (scrollX != m_researchScrollX)
        m_researchGlideX = 0.0f;
    if (scrollY != m_researchScrollY)
        m_researchGlideY = 0.0f;
    m_researchScrollX = scrollX;
    m_researchScrollY = scrollY;
    if (scrollDragFrame)
        return true;
    if (wasDragging && !m_researchDrag && m_researchDragMoved)
        leftClicked = false; // the finger lifted at the end of a pan: not a tap on whatever it stopped over

    if (leftClicked)
    {
        m_researchSearchFocus = l.search.Contains(mx, my);
        if (l.closeBtn.Contains(mx, my))
        {
            CloseResearchWindow();
            SoundRequests::Play("ui_confirm");
            return true;
        }
        if (l.zoomIn.Contains(mx, my) || l.zoomOut.Contains(mx, my))
        {
            const float before = m_researchScale;
            setScale(m_researchScale + (l.zoomIn.Contains(mx, my) ? kScaleStep : -kScaleStep));
            m_researchScrollX *= m_researchScale / before;
            m_researchScrollY *= m_researchScale / before;
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
        if (l.tree.Contains(mx, my))
        {
            for (int i = 0; i < kResearchCount; ++i)
            {
                if (!tree.shown[i])
                    continue;
                const UiRect n {
                    l.tree.x + 8 + tree.depth[i] * (kNodeW + kGapX) - static_cast<int>(m_researchScrollX),
                    l.tree.y + 8 + static_cast<int>(tree.row[i] * (kNodeH + kGapY))
                        - static_cast<int>(m_researchScrollY),
                    kNodeW,
                    kNodeH};
                if (!n.Contains(mx, my))
                    continue;
                const std::uint32_t now = SDL_GetTicks();
                m_researchSel = i;
                // Twice on the same technology starts it, and the window closes (when it was not started, the reason
                // shows in the window)
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
            CloseResearchWindow(); // a click outside the window closes it
        }
    }
    (void) progress;
    return true; // while the window is open nothing else reacts
}

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
    const WindowLayout l = windowLayout(windowW, windowH, m_observer ? kObserverBarHeight : 0);
    const float sc = m_researchScale * l.u; // (the touch profile: bigger with the screen)
    const int kNodeW = sz(kNodeW0, sc), kNodeH = sz(kNodeH0, sc), kGapX = sz(kGapX0, sc), kGapY = sz(kGapY0, sc);
    const Faction faction = Players::FactionOf(Players::Local()); // (an observer: the watched player's)
    const TreeLayout tree = buildTree(faction);
    const ResearchProgress researchNow = ResearchShown();
    const ResearchProgress *progress = &researchNow;
    const bool hasLab = HasResearchBuilding(Players::Local(), entities);
    // A finger leaves no hovering pointer behind; the touch profile also reads bigger text in the nodes and the details
    auto over = [&](const UiRect &r) {
        return !l.touch && r.Contains(mx, my);
    };
    const int nodeNameFont = l.touch ? ui::FONT_BODY_BOLD : ui::FONT_SMALL_BOLD;
    const int nodeStateFont = l.touch ? ui::FONT_BODY : ui::FONT_SMALL;
    const int textFont = l.touch ? ui::FONT_BODY : ui::FONT_SMALL;

    ui::DrawDialog(
        renderer,
        font,
        windowW,
        windowH,
        l.dialog,
        faction == Faction::Confederation ? _("TECH CENTER - RESEARCH") : _("LABORATORY - RESEARCH"));

    // Search and filter
    ui::DrawPanel(renderer, l.search, col::PanelSunken, m_researchSearchFocus ? col::Accent : col::Border);
    const bool blink = (SDL_GetTicks() / 500) % 2 == 0;
    const std::string shown = m_researchSearch.empty() && !m_researchSearchFocus
                                  ? std::string(_("Search technology..."))
                                  : m_researchSearch + (m_researchSearchFocus && blink ? "|" : "");
    font.DrawString(
        ui::FitText(font, shown, ui::FONT_BODY, l.search.w - 20),
        l.search.x + 10,
        l.search.CenterY(),
        ui::FONT_BODY,
        m_researchSearch.empty() && !m_researchSearchFocus ? col::TextMuted : col::Text,
        false,
        Anchor::LEFT | Anchor::VCENTER);
    const std::vector<std::string> filters = filterChoices();
    ui::DrawComboField(
        renderer,
        font,
        l.filter,
        std::string(_("Show")) + ": " + filters[std::clamp(m_researchFilter, 0, 4)],
        m_researchCombo.open == 0,
        over(l.filter));
    ui::DrawButton(renderer, font, l.closeBtn, _("CLOSE"), ui::ButtonStyle::Secondary, over(l.closeBtn));
    // Credits
    font.DrawString(
        "$ " + std::to_string(economy.GetCredits()),
        l.closeBtn.x - 16,
        l.closeBtn.CenterY(),
        ui::FONT_H2,
        col::Accent,
        false,
        Anchor::RIGHT | Anchor::VCENTER);

    // Which technologies stand out under the search and the filter
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

    // The tree
    ui::DrawPanel(renderer, l.tree, col::PanelSunken, col::Border);
    // Touch: the scissor of some devices lands in the wrong place, so the tree is drawn into its own picture
    const bool softClip
        = uilayout::MenuProfile(uilayout::Current(), "confirm") == "touch"
          && renderer.BeginClipTarget({l.tree.x + 1, l.tree.y + 1, l.tree.w - 2, l.tree.h - 2}, col::PanelSunken);
    if (!softClip)
        renderer.SetClipRect({l.tree.x + 1, l.tree.y + 1, l.tree.w - 2, l.tree.h - 2});
    auto nodeRect = [&](int i) {
        return UiRect {
            l.tree.x + 8 + tree.depth[i] * (kNodeW + kGapX) - static_cast<int>(m_researchScrollX),
            l.tree.y + 8 + static_cast<int>(tree.row[i] * (kNodeH + kGapY)) - static_cast<int>(m_researchScrollY),
            kNodeW,
            kNodeH};
    };
    // Links first
    for (int i = 0; i < kResearchCount; ++i)
    {
        if (!tree.shown[i] || tree.parent[i] < 0)
            continue;
        const UiRect a = nodeRect(tree.parent[i]), b = nodeRect(i);
        const int midX = (a.Right() + b.x) / 2;
        const bool hot = i == m_researchSel || tree.parent[i] == m_researchSel;
        const bool done = IsResearched(Players::Local(), tree.parent[i]);
        const SDL_Color c = hot ? col::Accent : (done ? SDL_Color {70, 150, 100, 255} : SDL_Color {90, 110, 120, 255});
        renderer.DrawLineScreen(a.Right(), a.CenterY(), midX, a.CenterY(), c);
        renderer.DrawLineScreen(midX, a.CenterY(), midX, b.CenterY(), c);
        renderer.DrawLineScreen(midX, b.CenterY(), b.x, b.CenterY(), c);
    }
    for (int i = 0; i < kResearchCount; ++i)
    {
        if (!tree.shown[i])
            continue;
        const UiRect n = nodeRect(i);
        if (n.Right() < l.tree.x || n.x > l.tree.Right() || n.Bottom() < l.tree.y || n.y > l.tree.Bottom())
            continue;
        const auto &r = researchDef(faction, i);
        const State st = stateOf(i, progress);
        const bool selected = i == m_researchSel;
        const bool hovered = over(n) && l.tree.Contains(mx, my);
        const bool lit = matches(i);
        SDL_Color fill = st == State::Done        ? SDL_Color {28, 52, 38, 255}
                         : st == State::Active    ? SDL_Color {26, 44, 66, 255}
                         : st == State::Available ? SDL_Color {36, 40, 30, 255}
                                                  : SDL_Color {24, 26, 32, 255};
        if (hovered)
            fill = {
                static_cast<Uint8>(fill.r + 14), static_cast<Uint8>(fill.g + 14), static_cast<Uint8>(fill.b + 14), 255};
        SDL_Color edge = st == State::Done
                             ? col::Ok
                             : (st == State::Active ? col::Info : (st == State::Available ? col::Accent : col::Border));
        renderer.FillRectScreen(n.x, n.y, n.w, n.h, fill);
        renderer.DrawRectScreen(n.x, n.y, n.w, n.h, selected ? col::Text : edge);
        if (selected)
            renderer.DrawRectScreen(n.x + 1, n.y + 1, n.w - 2, n.h - 2, edge);
        renderer.SetDrawOwner(Players::Local());
        renderer.SetStructureFaction(faction);
        const int iconBox = sz(kIconBox0, sc);
        renderer.DrawCompositeScreen(r.compId, n.x + 4, n.y + (n.h - iconBox) / 2, kIconScale0 * sc, faction);
        renderer.SetStructureFaction(Faction::Neutral);
        renderer.SetDrawOwner(kNoPlayer);
        const std::string state = st == State::Done     ? std::string(_("RESEARCHED"))
                                  : st == State::Active ? std::string(_("RESEARCHING"))
                                  : st == State::Locked ? std::string(_("LOCKED"))
                                                        : "$" + std::to_string(r.cost);
        const int textX = n.x + 4 + iconBox + 6;
        font.DrawString(
            ui::FitText(font, _(r.name), nodeNameFont, n.Right() - 6 - textX),
            textX,
            n.CenterY() - 20,
            nodeNameFont,
            lit ? col::Text : col::TextMuted,
            false);
        font.DrawString(
            state,
            textX,
            n.CenterY() + 2,
            nodeStateFont,
            st == State::Done     ? col::Ok
            : st == State::Active ? col::Info
            : st == State::Locked ? col::TextDim
                                  : (economy.CanAfford(r.cost) ? col::Accent : col::Danger),
            false);
        if (st == State::Active && progress)
            ui::DrawProgressBar(
                renderer,
                {n.x + 4, n.Bottom() - 6, n.w - 8, 4},
                progress->timer / std::max(0.1f, progress->duration),
                col::Info);
        if (!lit)
            renderer.FillRectScreen(n.x, n.y, n.w, n.h, {8, 10, 14, 170}); // not what the search / filter asks for
    }
    if (softClip)
        renderer.EndClipTarget();
    else
        renderer.ResetClipRect();
    {
        const float treeW = 16.0f + (tree.maxDepth + 1) * (kNodeW + kGapX);
        const float treeH = 16.0f + tree.rows * (kNodeH + kGapY);
        const UiRect vTrack = researchVTrack(l.tree), hTrack = researchHTrack(l.tree);
        ui::DrawScrollBar(
            renderer,
            vTrack,
            m_researchScrollY,
            treeH,
            static_cast<float>(l.tree.h),
            m_researchScrollDrag == 1 || ui::OnScrollTrack(vTrack, mx, my));
        ui::DrawScrollBar(
            renderer,
            hTrack,
            m_researchScrollX,
            treeW,
            static_cast<float>(l.tree.w),
            m_researchScrollDrag == 2 || ui::OnScrollTrack(hTrack, mx, my));
    }
    ui::DrawButton(renderer, font, l.zoomIn, "+", ui::ButtonStyle::Secondary, over(l.zoomIn));
    ui::DrawButton(renderer, font, l.zoomOut, "-", ui::ButtonStyle::Secondary, over(l.zoomOut));

    // The selected technology
    ui::DrawPanel(renderer, l.details, col::PanelRaised, col::Border);
    if (const int i = std::clamp(m_researchSel, 0, kResearchCount - 1); tree.shown[i])
    {
        const auto &r = researchDef(faction, i);
        const State st = stateOf(i, progress);
        const int x = l.details.x + 12, w = l.details.w - 24;
        int y = l.details.y + 12;
        renderer.SetDrawOwner(Players::Local());
        renderer.SetStructureFaction(faction);
        renderer.DrawCompositeScreen(r.compId, x, y, kDetailsIconScale, faction);
        renderer.SetStructureFaction(Faction::Neutral);
        renderer.SetDrawOwner(kNoPlayer);
        const int iconW = static_cast<int>(66 * kDetailsIconScale);
        font.DrawTextWrapped(_(r.name), x + iconW + 8, y, w - iconW - 8, ui::FONT_BODY_BOLD, col::Accent, true);
        y += iconW + 6;
        renderer.FillRectScreen(x, y, w, 1, col::Border);
        y += 10;
        font.DrawString(std::string(_("Cost")) + ": $" + std::to_string(r.cost), x, y, ui::FONT_BODY, col::Text, false);
        y += 22;
        font.DrawString(
            std::string(_("Time")) + ": " + std::to_string(static_cast<int>(r.seconds)) + " " + _("s"),
            x,
            y,
            ui::FONT_BODY,
            col::Text,
            false);
        y += 22;
        if (r.needs >= 0)
        {
            font.DrawTextWrapped(
                std::string(_("Requires")) + ": " + _(researchDef(faction, r.needs).name),
                x,
                y,
                w,
                textFont,
                IsResearched(Players::Local(), r.needs) ? col::Ok : col::Danger,
                false);
            y += l.touch ? 48 : 36;
        }
        font.DrawTextWrapped(_(r.desc), x, y, w, textFont, col::TextDim, false);
        // The state and the button
        const std::string stateText = st == State::Done     ? std::string(_("RESEARCHED"))
                                      : st == State::Active ? std::string(_("RESEARCHING"))
                                      : st == State::Locked ? std::string(_("LOCKED"))
                                                            : std::string(_("AVAILABLE"));
        font.DrawString(
            stateText,
            l.details.CenterX(),
            l.startBtn.y - 40,
            ui::FONT_BODY_BOLD,
            st == State::Done ? col::Ok
                              : (st == State::Active ? col::Info : (st == State::Locked ? col::TextDim : col::Accent)),
            false,
            Anchor::HCENTER | Anchor::VCENTER);
        if (st == State::Active && progress)
            ui::DrawProgressBar(
                renderer,
                {x, l.startBtn.y - 28, w, 8},
                progress->timer / std::max(0.1f, progress->duration),
                col::Info);
        const bool canStart
            = st == State::Available && hasLab && economy.CanAfford(r.cost) && progress && progress->active < 0;
        ui::DrawButton(
            renderer, font, l.startBtn, _("START RESEARCH"), ui::ButtonStyle::Primary, over(l.startBtn), canStart);
    }
    // What the last click said (no laboratory, not enough credits, ...)
    if (!m_hudNotification.empty())
        font.DrawString(
            ui::FitText(font, m_hudNotification, ui::FONT_SMALL, l.tree.w - 20),
            l.tree.x + 10,
            l.tree.Bottom() - 14,
            ui::FONT_SMALL,
            col::Danger,
            false);
    else if (!hasLab)
        font.DrawString(
            faction == Faction::Confederation ? _("Build a Tech Center to start research")
                                              : _("Build a Laboratory to start research"),
            l.tree.x + 10,
            l.tree.Bottom() - 14,
            ui::FONT_SMALL,
            col::TextMuted,
            false);
    if (m_researchCombo.open == 0)
    {
        ui::DrawComboList(
            renderer, font, l.filter, filters, std::clamp(m_researchFilter, 0, 4), m_researchCombo, windowH, mx, my);
    }
}

} // namespace OpenArtOfWar
