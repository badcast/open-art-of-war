#include "ui/ModsMenu.hpp"
#include "audio/SoundRequests.hpp"
#include "core/Localization.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "graphics/Renderer.hpp"
#include "input/InputManager.hpp"

#include <algorithm>

namespace OpenArtOfWar
{

namespace
{
namespace col = ui::color;
}

void ModsMenu::Refresh(const std::vector<std::string> &enabled)
{
    m_entries.clear();
    m_applied = enabled;
    m_dirs = Mods::SearchDirs();
    std::vector<Mods::ModInfo> found = Mods::Scan();
    // The enabled ones first, in their order; the rest by name
    for (const std::string &id : enabled)
    {
        const auto it = std::find_if(found.begin(), found.end(), [&](const Mods::ModInfo &m) { return m.id == id; });
        if (it == found.end())
            continue;
        m_entries.push_back({*it, it->error.empty()});
        found.erase(it);
    }
    for (Mods::ModInfo &m : found)
        m_entries.push_back({std::move(m), false});
    m_selected = std::clamp(m_selected, 0, std::max(0, static_cast<int>(m_entries.size()) - 1));
}

void ModsMenu::SetReport(const Mods::ActivateReport &report, const std::string &notice)
{
    m_report = report;
    m_notice = notice;
}

std::vector<std::string> ModsMenu::EnabledIds() const
{
    std::vector<std::string> out;
    for (const Entry &e : m_entries)
        if (e.enabled)
            out.push_back(e.info.id);
    return out;
}

void ModsMenu::Toggle(int index)
{
    if (index < 0 || index >= static_cast<int>(m_entries.size()))
        return;
    Entry &e = m_entries[index];
    if (!e.info.error.empty())
    {
        m_notice = e.info.name + ": " + e.info.error;
        return;
    }
    e.enabled = !e.enabled;
    // Enabled ones stay together on top: a newly enabled one goes to the end of them (it wins over the others)
    Entry moved = e;
    m_entries.erase(m_entries.begin() + index);
    const auto firstOff
        = std::find_if(m_entries.begin(), m_entries.end(), [](const Entry &x) { return !x.enabled; });
    const auto at = m_entries.insert(moved.enabled ? firstOff : firstOff, moved);
    m_selected = static_cast<int>(at - m_entries.begin());
}

void ModsMenu::Move(int index, int by)
{
    const int to = index + by;
    if (index < 0 || to < 0 || index >= static_cast<int>(m_entries.size()) || to >= static_cast<int>(m_entries.size()))
        return;
    if (!m_entries[index].enabled || !m_entries[to].enabled)
        return; // the order is the order of the enabled ones
    std::swap(m_entries[index], m_entries[to]);
    m_selected = to;
}

ModsMenu::Layout ModsMenu::MakeLayout(int windowW, int windowH)
{
    Layout l;
    l.dialog = ui::CenteredRect(windowW, windowH, 980, 580);
    const int pad = 18, titleH = 48, footerH = 56;
    const int bodyY = l.dialog.y + titleH + pad;
    const int bodyH = l.dialog.h - titleH - footerH - pad * 2;
    const int listW = (l.dialog.w - pad * 3) * 5 / 11;
    l.list = {l.dialog.x + pad, bodyY, listW, bodyH};
    l.details = {l.list.Right() + pad, bodyY, l.dialog.w - listW - pad * 3, bodyH};
    l.rows = std::max(1, l.list.h / l.rowH);
    const int by = l.dialog.Bottom() - footerH + 8;
    l.backBtn = {l.dialog.x + pad, by, 150, 38};
    l.upBtn = {l.backBtn.Right() + 12, by, 110, 38};
    l.downBtn = {l.upBtn.Right() + 8, by, 110, 38};
    l.applyBtn = {l.dialog.Right() - pad - 190, by, 190, 38};
    return l;
}

ModsMenu::Result ModsMenu::Update(InputManager &input, int windowW, int windowH)
{
    Result result;
    const Layout l = MakeLayout(windowW, windowH);
    m_mouseX = input.GetMouseScreenX();
    m_mouseY = input.GetMouseScreenY();
    float lx, ly;
    const bool clicked = input.WasLeftClickIssued(lx, ly);
    const int wheel = input.ConsumeWheel();
    const int count = static_cast<int>(m_entries.size());
    if (input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE))
    {
        result.back = true;
        return result;
    }
    auto apply = [&]() {
        result.apply = true;
        result.enabled = EnabledIds();
    };
    if (count > 0)
    {
        const bool shift = input.IsKeyPressed(SDL_SCANCODE_LSHIFT) || input.IsKeyPressed(SDL_SCANCODE_RSHIFT);
        if (input.IsKeyJustPressed(SDL_SCANCODE_UP))
        {
            if (shift)
                Move(m_selected, -1);
            else
                m_selected = std::max(0, m_selected - 1);
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_DOWN))
        {
            if (shift)
                Move(m_selected, 1);
            else
                m_selected = std::min(count - 1, m_selected + 1);
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_SPACE))
            Toggle(m_selected);
        if (wheel != 0 && l.list.Contains(m_mouseX, m_mouseY))
            m_top = std::clamp(m_top - wheel * 3, 0, std::max(0, count - l.rows));
    }
    m_selected = std::clamp(m_selected, 0, std::max(0, count - 1));
    if (m_selected < m_top)
        m_top = m_selected;
    if (m_selected >= m_top + l.rows)
        m_top = m_selected - l.rows + 1;
    if (clicked)
    {
        if (l.backBtn.Contains(m_mouseX, m_mouseY))
        {
            result.back = true;
            return result;
        }
        if (l.applyBtn.Contains(m_mouseX, m_mouseY))
            apply();
        else if (l.upBtn.Contains(m_mouseX, m_mouseY))
            Move(m_selected, -1);
        else if (l.downBtn.Contains(m_mouseX, m_mouseY))
            Move(m_selected, 1);
        else if (l.list.Contains(m_mouseX, m_mouseY))
        {
            const int row = m_top + (m_mouseY - l.list.y) / l.rowH;
            if (row >= 0 && row < count)
            {
                m_selected = row;
                if (m_mouseX < l.list.x + 40) // the box
                    Toggle(row);
            }
        }
    }
    if (input.IsConfirmJustPressed())
        apply();
    return result;
}

void ModsMenu::Render(Renderer &renderer, BitmapFontRenderer &font, int windowW, int windowH)
{
    const Layout l = MakeLayout(windowW, windowH);
    ui::DrawDialog(renderer, font, windowW, windowH, l.dialog, _("MODS"));
    const int count = static_cast<int>(m_entries.size());
    ui::DrawPanel(renderer, l.list, col::PanelSunken);
    ui::DrawPanel(renderer, l.details, col::PanelSunken);
    renderer.SetClipRect({l.list.x, l.list.y, l.list.w, l.list.h});
    if (count == 0)
    {
        int y = l.list.y + 16;
        font.DrawString(_("No mods found. Put them (.mod files or folders) into:"), l.list.x + 14, y, ui::FONT_BODY,
            col::TextDim, false);
        for (const std::string &d : m_dirs)
        {
            y += font.GetTextHeight(ui::FONT_BODY) + 8;
            font.DrawString(ui::FitText(font, d, ui::FONT_SMALL, l.list.w - 28), l.list.x + 14, y, ui::FONT_SMALL,
                col::TextMuted, false);
        }
    }
    int order = 0;
    for (int i = 0; i < count; ++i)
    {
        if (m_entries[i].enabled)
            ++order;
        if (i < m_top || i >= m_top + l.rows)
            continue;
        const Entry &e = m_entries[i];
        const ui::UiRect r {l.list.x + 2, l.list.y + (i - m_top) * l.rowH, l.list.w - 4, l.rowH};
        const bool selected = i == m_selected;
        if (selected)
            ui::FillHorizontalGradient(renderer, r, ui::WithAlpha(col::Hover, 255), ui::WithAlpha(col::Hover, 40));
        else if (r.Contains(m_mouseX, m_mouseY))
            renderer.FillRectScreen(r.x, r.y, r.w, r.h, ui::WithAlpha(col::Hover, 120));
        ui::DrawCheckbox(renderer, font, {r.x + 8, r.CenterY() - 10, 20, 20}, "", e.enabled,
            r.Contains(m_mouseX, m_mouseY) && m_mouseX < l.list.x + 40, e.info.error.empty());
        std::string title = (e.enabled ? std::to_string(order) + ". " : std::string()) + e.info.name;
        font.DrawString(ui::FitText(font, title, ui::FONT_BODY, r.w - 150), r.x + 40, r.CenterY(), ui::FONT_BODY,
            e.info.error.empty() ? (selected ? col::Text : col::TextDim) : col::Danger, false,
            Anchor::LEFT | Anchor::VCENTER);
        font.DrawString(e.info.version, r.Right() - 10, r.CenterY(), ui::FONT_SMALL, col::TextMuted, false,
            Anchor::RIGHT | Anchor::VCENTER);
    }
    renderer.ResetClipRect();

    // The selected mod
    int y = l.details.y + 14;
    const int x = l.details.x + 16;
    auto line = [&](const std::string &text, SDL_Color color, int scale = ui::FONT_BODY) {
        font.DrawString(ui::FitText(font, text, scale, l.details.w - 32), x, y, scale, color, false);
        y += font.GetTextHeight(scale) + 6;
    };
    if (count > 0)
    {
        const Mods::ModInfo &m = m_entries[m_selected].info;
        line(m.name, col::Accent, ui::FONT_H2);
        line(std::string(_("Version")) + ": " + m.version + "   id: " + m.id, col::Text, ui::FONT_BODY_BOLD);
        if (!m.author.empty())
            line(std::string(_("Author")) + ": " + m.author, col::TextDim);
        if (!m.gameVersion.empty())
            line(std::string(_("Game version")) + ": " + m.gameVersion, col::TextDim);
        line(std::string(m.fromUser ? _("In the user folder") : _("Next to the game")) + ", "
                 + (m.archive ? _("archive") : _("folder")),
            col::TextMuted, ui::FONT_SMALL);
        line(m.path, col::TextMuted, ui::FONT_SMALL);
        if (!m.error.empty())
            line(m.error, col::Danger, ui::FONT_SMALL_BOLD);
        // The description, wrapped by words
        if (!m.description.empty())
        {
            y += 6;
            std::string rest = m.description, row;
            while (!rest.empty() && y < l.details.Bottom() - 120)
            {
                const auto sp = rest.find(' ');
                const std::string word = rest.substr(0, sp);
                rest = sp == std::string::npos ? "" : rest.substr(sp + 1);
                const std::string next = row.empty() ? word : row + " " + word;
                if (font.GetTextWidth(next, ui::FONT_SMALL) > l.details.w - 32 && !row.empty())
                {
                    line(row, col::TextDim, ui::FONT_SMALL);
                    row = word;
                }
                else
                    row = next;
            }
            if (!row.empty())
                line(row, col::TextDim, ui::FONT_SMALL);
        }
    }
    // What the last Apply found: problems and files several mods bring (the lower one in the list wins)
    if (!m_report.problems.empty() || !m_report.conflicts.empty())
    {
        y = std::max(y + 8, l.details.Bottom() - 110);
        ui::DrawCaption(font, renderer, x, y, _("CONFLICTS"));
        y += 24;
        for (const std::string &p : m_report.problems)
            if (y < l.details.Bottom() - 18)
                line(p, col::Danger, ui::FONT_SMALL);
        for (const auto &[file, ids] : m_report.conflicts)
        {
            if (y >= l.details.Bottom() - 18)
                break;
            std::string who;
            for (const std::string &id : ids)
                who += (who.empty() ? "" : ", ") + id;
            line(file + ": " + who + " (" + ids.back() + _(" wins") + ")", col::Accent, ui::FONT_SMALL);
        }
    }
    if (!m_notice.empty())
        font.DrawString(ui::FitText(font, m_notice, ui::FONT_SMALL, l.applyBtn.x - l.downBtn.Right() - 24),
            (l.downBtn.Right() + l.applyBtn.x) / 2, l.backBtn.CenterY(), ui::FONT_SMALL, col::TextDim, false,
            Anchor::HCENTER | Anchor::VCENTER);
    const bool canMove = count > 0 && m_entries[m_selected].enabled;
    ui::DrawButton(renderer, font, l.backBtn, _("BACK"), ui::ButtonStyle::Secondary, l.backBtn.Contains(m_mouseX, m_mouseY));
    ui::DrawButton(renderer, font, l.upBtn, _("UP"), ui::ButtonStyle::Ghost, l.upBtn.Contains(m_mouseX, m_mouseY), canMove);
    ui::DrawButton(
        renderer, font, l.downBtn, _("DOWN"), ui::ButtonStyle::Ghost, l.downBtn.Contains(m_mouseX, m_mouseY), canMove);
    const bool changed = EnabledIds() != m_applied;
    ui::DrawButton(renderer, font, l.applyBtn, _("APPLY"), ui::ButtonStyle::Primary,
        l.applyBtn.Contains(m_mouseX, m_mouseY), changed);
    if (l.backBtn.Contains(m_mouseX, m_mouseY) || l.applyBtn.Contains(m_mouseX, m_mouseY)
        || l.list.Contains(m_mouseX, m_mouseY))
        SoundRequests::MarkHover();
}

} // namespace OpenArtOfWar
