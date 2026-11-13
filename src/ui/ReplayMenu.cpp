#include "ui/ReplayMenu.hpp"
#include "ui/UiLayout.hpp"
#include "core/AssetStore.hpp"
#include "core/Localization.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "graphics/Renderer.hpp"
#include "input/InputManager.hpp"
#include "net/MapFiles.hpp"
#include "net/ReplayRecorder.hpp"
#include "audio/SoundRequests.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace OpenArtOfWar
{

namespace
{
namespace fs = std::filesystem;
namespace col = ui::color;

std::string FormatTime(std::int64_t t)
{
    if (t <= 0)
        return "-";
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm local {};
#if defined(_WIN32)
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &local);
    return buf;
}

std::string FormatDuration(std::uint32_t ticks)
{
    const std::uint32_t seconds = ticks / 20;
    char buf[24];
    std::snprintf(buf, sizeof buf, "%u:%02u", seconds / 60, seconds % 60);
    return buf;
}

// "1.4 MB", "820 KB"
std::string FormatSize(std::uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024u * 1024u)
        std::snprintf(buf, sizeof buf, "%.1f %s", static_cast<double>(bytes) / (1024.0 * 1024.0), _("MB"));
    else
        std::snprintf(buf, sizeof buf, "%u %s", static_cast<unsigned>((bytes + 1023u) / 1024u), _("KB"));
    return buf;
}

// What was played: a network match, a custom game of this computer, a campaign mission
std::string ModeName(std::string_view mode)
{
    if (mode == "custom")
        return _("Custom game");
    if (mode == "campaign")
        return _("Campaign");
    return _("Network");
}

SDL_Color ModeColor(std::string_view mode)
{
    if (mode == "custom")
        return ui::color::Ok;
    if (mode == "campaign")
        return ui::color::Info;
    return ui::color::Accent;
}

std::string MapName(std::string_view path)
{
    return fs::path(path).stem().string();
}

const char *FactionName(Faction f)
{
    return f == Faction::Confederation ? "Confederation" : "Resistance";
}
} // namespace

void ReplayMenu::Refresh()
{
    m_entries.clear();
    m_selected = 0;
    m_scroll.Reset();
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(ReplayRecorder::Directory(), ec))
    {
        if (e.is_regular_file(ec) && e.path().extension() == ".awr")
            files.push_back(e.path().string());
    }
    // The names start with the date: newest first
    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
        return fs::path(a).filename().string() > fs::path(b).filename().string();
    });
    if (files.size() > 150)
        files.resize(150);
    for (const std::string &f : files)
    {
        Entry entry;
        entry.name = fs::path(f).filename().string();
        entry.ok = ReplayPlayer::ReadInfo(f, entry.info, entry.error);
        if (entry.ok && !entry.info.carriesData)
            entry.mapMissing = MapFiles::Resolve(entry.info.map, entry.info.mapHash).empty();
        m_entries.push_back(std::move(entry));
    }
}

ReplayMenu::Layout ReplayMenu::MakeLayout(int windowW, int windowH)
{
    Layout l;
    const bool touch = uilayout::MenuProfile(uilayout::Current(), "confirm") == "touch";
    l.dialog = ui::CenteredRect(windowW, windowH, 980, 580);
    const int pad = 18, titleH = 52, footerH = 60;
    const int bodyY = l.dialog.y + titleH + pad;
    const int bodyH = l.dialog.h - titleH - footerH - pad * 2;
    const int listW = (l.dialog.w - pad * 3) * 5 / 11;
    l.rowH = touch ? 50 : 42;
    l.list = {l.dialog.x + pad, bodyY, listW - 10, bodyH};
    l.track = {l.list.Right() + 4, l.list.y, 6, l.list.h};
    l.details = {l.list.Right() + pad + 6, bodyY, l.dialog.w - listW - pad * 3, bodyH};
    l.rows = std::max(1, l.list.h / l.rowH);
    const int by = l.dialog.Bottom() - footerH + 8;
    l.backBtn = {l.dialog.x + pad, by, 160, 44};
    l.openBtn = {l.dialog.Right() - pad - 200, by, 200, 44};
    return l;
}

ReplayMenu::Result ReplayMenu::Update(InputManager &input, int windowW, int windowH)
{
    Result result;
    const Layout l = MakeLayout(windowW, windowH);
    m_mouseX = input.GetMouseScreenX();
    m_mouseY = input.GetMouseScreenY();
    float lx, ly;
    bool clicked = input.WasLeftClickIssued(lx, ly);
    const float wheel = input.ConsumeWheelF();
    int pressX = 0, pressY = 0;
    const bool pressed = input.ConsumeLeftPress(pressX, pressY) && input.IsLeftMouseDown();
    const bool down = input.IsLeftMouseDown();
    const int count = static_cast<int>(m_entries.size());
    if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        result.back = true;
        return result;
    }
    bool moved = false;
    if (count > 0)
    {
        if (input.IsKeyJustPressed(SDL_SCANCODE_UP))
        {
            m_selected = std::max(0, m_selected - 1);
            moved = true;
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_DOWN))
        {
            m_selected = std::min(count - 1, m_selected + 1);
            moved = true;
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_PAGEUP))
        {
            m_selected = std::max(0, m_selected - l.rows);
            moved = true;
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_PAGEDOWN))
        {
            m_selected = std::min(count - 1, m_selected + l.rows);
            moved = true;
        }
    }
    m_selected = std::clamp(m_selected, 0, std::max(0, count - 1));
    if (moved)
        m_scroll.EnsureVisible(m_selected, l.rowH, l.list.h, count);
    ui::UpdateSmoothList(
        m_scroll,
        l.list,
        l.track,
        count * l.rowH,
        l.rowH * 2,
        m_mouseX,
        m_mouseY,
        down,
        pressed,
        pressX,
        pressY,
        wheel,
        clicked);
    auto openSelected = [&]() {
        if (count > 0 && m_entries[m_selected].ok)
            result.open = m_entries[m_selected].info.file;
        else if (count > 0)
            m_notice = m_entries[m_selected].error;
    };
    if (clicked)
    {
        if (l.backBtn.Contains(m_mouseX, m_mouseY))
        {
            result.back = true;
            return result;
        }
        if (l.openBtn.Contains(m_mouseX, m_mouseY))
            openSelected();
        else if (l.list.Contains(m_mouseX, m_mouseY))
        {
            const int row = (m_mouseY - l.list.y + m_scroll.PixelOffset()) / l.rowH;
            if (row >= 0 && row < count)
            {
                const std::uint32_t now = SDL_GetTicks();
                if (row == m_lastClickRow && now - m_lastClickMs < 400)
                    openSelected();
                m_selected = row;
                m_scroll.EnsureVisible(m_selected, l.rowH, l.list.h, count);
                m_lastClickRow = row;
                m_lastClickMs = now;
            }
        }
    }
    if (input.IsConfirmJustPressed())
        openSelected();
    return result;
}

void ReplayMenu::Render(Renderer &renderer, BitmapFontRenderer &font, int windowW, int windowH)
{
    const Layout l = MakeLayout(windowW, windowH);
    ui::DrawDialog(renderer, font, windowW, windowH, l.dialog, _("REPLAYS"));
    const int count = static_cast<int>(m_entries.size());
    ui::DrawPanel(renderer, l.list, col::PanelSunken);
    ui::DrawPanel(renderer, l.details, col::PanelSunken);
    renderer.SetClipRect({l.list.x, l.list.y, l.list.w, l.list.h});
    if (count == 0)
    {
        font.DrawString(
            _("No replays yet. Every network game you play is saved here."),
            l.list.x + 14,
            l.list.y + 16,
            ui::FONT_BODY,
            col::TextDim,
            false);
    }
    const int scrollOff = m_scroll.PixelOffset();
    for (int i = 0; i < count; ++i)
    {
        const ui::UiRect r {l.list.x + 2, l.list.y + i * l.rowH - scrollOff, l.list.w - 4, l.rowH - 2};
        if (r.Bottom() <= l.list.y || r.y >= l.list.Bottom())
            continue;
        const Entry &e = m_entries[i];
        const bool selected = i == m_selected;
        const bool hovered = l.list.Contains(m_mouseX, m_mouseY) && r.Contains(m_mouseX, m_mouseY);
        if (selected)
            ui::FillHorizontalGradient(renderer, r, ui::WithAlpha(col::Hover, 255), ui::WithAlpha(col::Hover, 40));
        else if (hovered)
            renderer.FillRectScreen(r.x, r.y, r.w, r.h, ui::WithAlpha(col::Hover, 140));
        if (selected)
            renderer.FillRectScreen(r.x, r.y, 3, r.h, col::Accent);
        std::string title = e.ok ? FormatTime(e.info.recordedAt) + "  " + MapName(e.info.map) : e.name;
        // The kind of match: a small tag at the left
        int titleX = r.x + 12;
        if (e.ok)
        {
            const std::string tag = ModeName(e.info.mode);
            const SDL_Color tc = ModeColor(e.info.mode);
            const int tw = font.GetTextWidth(tag, ui::FONT_SMALL) + 10;
            renderer.FillRectScreen(titleX, r.CenterY() - 9, tw, 18, ui::WithAlpha(tc, 50));
            renderer.DrawRectScreen(titleX, r.CenterY() - 9, tw, 18, tc);
            font.DrawString(tag, titleX + 5, r.CenterY(), ui::FONT_SMALL, tc, false, Anchor::LEFT | Anchor::VCENTER);
            titleX += tw + 8;
        }
        font.DrawString(
            ui::FitText(font, title, ui::FONT_BODY_BOLD, r.Right() - 150 - titleX),
            titleX,
            r.CenterY(),
            ui::FONT_BODY_BOLD,
            e.ok ? (selected ? col::Text : col::TextDim) : col::Danger,
            false,
            Anchor::LEFT | Anchor::VCENTER);
        if (e.ok)
            font.DrawString(
                FormatDuration(e.info.ticks) + "   " + FormatSize(e.info.fileBytes),
                r.Right() - 10,
                r.CenterY(),
                ui::FONT_SMALL,
                col::TextMuted,
                false,
                Anchor::RIGHT | Anchor::VCENTER);
    }
    renderer.ResetClipRect();
    ui::DrawScrollBar(
        renderer,
        l.track,
        m_scroll.offset,
        static_cast<float>(count * l.rowH),
        static_cast<float>(l.list.h),
        m_scroll.barDrag || ui::OnScrollTrack(l.track, m_mouseX, m_mouseY));

    // The match of the selected replay
    if (count > 0)
    {
        const Entry &e = m_entries[m_selected];
        int y = l.details.y + 14;
        const int x = l.details.x + 16;
        auto line = [&](std::string_view text, SDL_Color color, int scale = ui::FONT_BODY) {
            font.DrawString(ui::FitText(font, text, scale, l.details.w - 32), x, y, scale, color, false);
            y += font.GetTextHeight(scale) + 6;
        };
        if (!e.ok)
        {
            line(e.name, col::Text, ui::FONT_BODY_BOLD);
            line(e.error, col::Danger);
        }
        else
        {
            const ReplayPlayer::Info &in = e.info;
            line(MapName(in.map), col::Accent, ui::FONT_H2);
            line(ModeName(in.mode), ModeColor(in.mode), ui::FONT_BODY_BOLD);
            line(std::string(_("Played")) + ": " + FormatDuration(in.ticks), col::Text, ui::FONT_BODY_BOLD);
            line(
                FormatTime(in.recordedAt) + "   " + std::string(_("Size")) + ": " + FormatSize(in.fileBytes),
                col::TextDim);
            std::string result = in.outcome == "victory"  ? _("Victory")
                                 : in.outcome == "defeat" ? _("Defeat")
                                 : in.outcome.empty()     ? ""
                                                          : in.outcome;
            line(
                std::string(in.source == "server" ? _("Recorded by the server") : _("Recorded by you"))
                    + (result.empty() ? "" : "  -  " + result),
                col::TextDim);
            line(std::string(_("Game version")) + ": " + in.gameVersion, col::TextMuted, ui::FONT_SMALL);
            // The game logic of this build is not the one that recorded it: it plays, but not necessarily as it went
            if (!in.checksumsComparable)
                line(_("Recorded by another game build: it may play out differently."), col::Accent, ui::FONT_SMALL);
            if (!in.mods.empty())
            {
                std::string mods;
                for (const std::string &m : in.mods)
                    mods += (mods.empty() ? "" : ", ") + m;
                line(std::string(_("Mods")) + ": " + mods, col::TextMuted, ui::FONT_SMALL);
            }
            y += 8;
            ui::DrawCaption(font, renderer, x, y, _("PLAYERS"));
            y += 24;
            for (const ReplayPlayer::Player &p : in.players)
            {
                std::string text
                    = p.name + "  -  " + FactionName(p.faction) + ", " + _("team") + " " + std::to_string(p.team + 1);
                if (!p.human)
                    text += "  (" + std::string(_("computer")) + ")";
                line(text, p.slot == in.localSlot ? col::Text : col::TextDim, ui::FONT_SMALL_BOLD);
                for (const std::string &a : p.addresses)
                    line("    " + a, col::TextMuted, ui::FONT_SMALL);
            }
            if (e.mapMissing)
            {
                y += 8;
                line(_("The map of this replay is not on this computer"), col::Danger, ui::FONT_SMALL_BOLD);
            }
        }
    }
    if (!m_notice.empty())
        font.DrawString(
            ui::FitText(font, m_notice, ui::FONT_BODY, l.dialog.w - 400),
            l.dialog.CenterX(),
            l.backBtn.CenterY(),
            ui::FONT_BODY,
            col::Danger,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
    ui::DrawButton(
        renderer, font, l.backBtn, _("BACK"), ui::ButtonStyle::Secondary, l.backBtn.Contains(m_mouseX, m_mouseY));
    ui::DrawButton(
        renderer,
        font,
        l.openBtn,
        _("WATCH"),
        ui::ButtonStyle::Primary,
        l.openBtn.Contains(m_mouseX, m_mouseY),
        count > 0 && m_entries[m_selected].ok);
    if (l.backBtn.Contains(m_mouseX, m_mouseY) || l.openBtn.Contains(m_mouseX, m_mouseY)
        || l.list.Contains(m_mouseX, m_mouseY))
        SoundRequests::MarkHover();
}

} // namespace OpenArtOfWar
