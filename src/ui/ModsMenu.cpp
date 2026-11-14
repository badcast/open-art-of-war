#include "ui/ModsMenu.hpp"
#include "ui/UiLayout.hpp"
#include "audio/SoundRequests.hpp"
#include "core/Localization.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "graphics/Renderer.hpp"
#include "input/InputManager.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

namespace OpenArtOfWar
{

namespace
{
namespace col = ui::color;
}

ModsMenu::ModsMenu() = default;

ModsMenu::~ModsMenu()
{
    ClearPreview();
}

void ModsMenu::ClearPreview()
{
    if (m_previewTexture)
    {
        SDL_DestroyTexture(m_previewTexture);
        m_previewTexture = nullptr;
    }
    m_previewW = 0;
    m_previewH = 0;
    m_previewSelected = -1;
    m_previewModId.clear();
}

void ModsMenu::Refresh(const std::vector<std::string> &enabled)
{
    ClearPreview();
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

void ModsMenu::SetReport(const Mods::ActivateReport &report, std::string_view notice)
{
    m_report = report;
    m_notice = std::string(notice);
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
    const auto firstOff = std::find_if(m_entries.begin(), m_entries.end(), [](const Entry &x) { return !x.enabled; });
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
    const bool touch = uilayout::MenuProfile(uilayout::Current(), "confirm") == "touch";
    l.dialog = ui::CenteredRect(windowW, windowH, 980, 580);
    const int pad = 20, titleH = 52, btnH = 44;
    const int bodyY = l.dialog.y + titleH + 16;
    const int by = l.dialog.Bottom() - pad - btnH;
    const int bodyH = std::max(80, by - 12 - bodyY);
    const int listW = (l.dialog.w - pad * 3) * 5 / 11;
    l.rowH = touch ? 50 : 42;
    l.list = {l.dialog.x + pad, bodyY, listW - 10, bodyH};
    l.track = {l.list.Right() + 4, l.list.y, 6, l.list.h};
    const int detailsX = l.track.Right() + 12;
    l.details = {detailsX, bodyY, std::max(80, l.dialog.Right() - pad - detailsX), bodyH};
    l.rows = std::max(1, l.list.h / l.rowH);
    l.backBtn = {l.dialog.x + pad, by, 160, btnH};
    l.upBtn = {l.backBtn.Right() + 12, by, 120, btnH};
    l.downBtn = {l.upBtn.Right() + 8, by, 120, btnH};
    l.applyBtn = {l.dialog.Right() - pad - 200, by, 200, btnH};
    return l;
}

ModsMenu::Result ModsMenu::Update(InputManager &input, int windowW, int windowH)
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
    auto apply = [&]() {
        result.apply = true;
        result.enabled = EnabledIds();
    };
    bool moved = false;
    if (count > 0)
    {
        const bool shift = input.IsKeyPressed(SDL_SCANCODE_LSHIFT) || input.IsKeyPressed(SDL_SCANCODE_RSHIFT);
        if (input.IsKeyJustPressed(SDL_SCANCODE_UP))
        {
            if (shift)
                Move(m_selected, -1);
            else
                m_selected = std::max(0, m_selected - 1);
            moved = true;
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_DOWN))
        {
            if (shift)
                Move(m_selected, 1);
            else
                m_selected = std::min(count - 1, m_selected + 1);
            moved = true;
        }
        if (input.IsKeyJustPressed(SDL_SCANCODE_SPACE))
            Toggle(m_selected);
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
            const int row = (m_mouseY - l.list.y + m_scroll.PixelOffset()) / l.rowH;
            if (row >= 0 && row < count)
            {
                m_selected = row;
                m_scroll.EnsureVisible(m_selected, l.rowH, l.list.h, count);
                if (m_mouseX < l.list.x + 48) // the box
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

    // Update / load preview texture for selected mod
    const std::string curId = (m_selected >= 0 && m_selected < count) ? m_entries[m_selected].info.id : "";
    if (m_previewSelected != m_selected || m_previewModId != curId)
    {
        ClearPreview();
        m_previewSelected = m_selected;
        m_previewModId = curId;
        if (!curId.empty())
        {
            std::vector<std::uint8_t> bytes;
            if (Mods::LoadPreview(m_entries[m_selected].info, bytes) && !bytes.empty())
            {
                SDL_Surface *surf = SDL_LoadPNG_IO(SDL_IOFromConstMem(bytes.data(), bytes.size()), true);
                if (!surf)
                    surf = SDL_LoadBMP_IO(SDL_IOFromConstMem(bytes.data(), bytes.size()), true);
                if (surf)
                {
                    m_previewTexture = SDL_CreateTextureFromSurface(renderer.GetSdlRenderer(), surf);
                    if (m_previewTexture)
                    {
                        SDL_SetTextureScaleMode(m_previewTexture, SDL_SCALEMODE_LINEAR);
                        m_previewW = surf->w;
                        m_previewH = surf->h;
                    }
                    SDL_DestroySurface(surf);
                }
            }
        }
    }

    renderer.SetClipRect({l.list.x, l.list.y, l.list.w, l.list.h});
    if (count == 0)
    {
        int y = l.list.y + 16;
        font.DrawString(
            _("No mods found. Put them (.mod files or folders) into:"),
            l.list.x + 14,
            y,
            ui::FONT_BODY,
            col::TextDim,
            false);
        for (const std::string &d : m_dirs)
        {
            y += font.GetTextHeight(ui::FONT_BODY) + 8;
            font.DrawString(
                ui::FitText(font, d, ui::FONT_SMALL, l.list.w - 28),
                l.list.x + 14,
                y,
                ui::FONT_SMALL,
                col::TextMuted,
                false);
        }
    }
    int order = 0;
    const int scrollOff = m_scroll.PixelOffset();
    for (int i = 0; i < count; ++i)
    {
        if (m_entries[i].enabled)
            ++order;
        const ui::UiRect r {l.list.x + 2, l.list.y + i * l.rowH - scrollOff, l.list.w - 4, l.rowH - 2};
        const int clipTop = std::max(r.y, l.list.y + 1);
        const int clipBot = std::min(r.Bottom(), l.list.Bottom() - 1);
        if (clipBot <= clipTop)
            continue;
        const Entry &e = m_entries[i];
        const bool selected = i == m_selected;
        const bool hovered = l.list.Contains(m_mouseX, m_mouseY) && r.Contains(m_mouseX, m_mouseY);
        const ui::UiRect bgRect {r.x, clipTop, r.w, clipBot - clipTop};
        if (selected)
            renderer.FillRectScreen(bgRect.x, bgRect.y, bgRect.w, bgRect.h, col::Hover);
        else if (hovered)
            renderer.FillRectScreen(bgRect.x, bgRect.y, bgRect.w, bgRect.h, ui::WithAlpha(col::Hover, 140));
        if (r.CenterY() - 10 < l.list.y || r.CenterY() + 10 > l.list.Bottom())
            continue;
        ui::DrawCheckbox(
            renderer,
            font,
            {r.x + 10, r.CenterY() - 12, 24, 24},
            "",
            e.enabled,
            hovered && m_mouseX < l.list.x + 48,
            e.info.error.empty());
        std::string title = (e.enabled ? std::to_string(order) + ". " : std::string()) + e.info.name;
        font.DrawString(
            ui::FitText(font, title, ui::FONT_BODY_BOLD, r.w - 150),
            r.x + 46,
            r.CenterY(),
            ui::FONT_BODY_BOLD,
            e.info.error.empty() ? (selected ? col::Text : col::TextDim) : col::Danger,
            false,
            Anchor::LEFT | Anchor::VCENTER);
        font.DrawString(
            e.info.version,
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

    // The selected mod
    int y = l.details.y + 14;
    const int x = l.details.x + 16;
    auto line = [&](std::string_view text, SDL_Color color, int scale = ui::FONT_BODY) {
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
        line(
            std::string(m.fromUser ? _("In the user folder") : _("Next to the game")) + ", "
                + (m.archive ? _("archive") : _("folder")),
            col::TextMuted,
            ui::FONT_SMALL);
        line(m.path, col::TextMuted, ui::FONT_SMALL);
        if (!m.error.empty())
            line(m.error, col::Danger, ui::FONT_SMALL_BOLD);
        const bool hasConflicts = !m_report.problems.empty() || !m_report.conflicts.empty();
        const int conflictsReserve = hasConflicts ? 110 : 0;
        const int previewReserve = m_previewTexture ? 170 : 0;

        // The description, wrapped by words
        if (!m.description.empty())
        {
            y += 6;
            const int maxDescY = l.details.Bottom() - conflictsReserve - previewReserve - 20;
            std::string rest = m.description, row;
            while (!rest.empty() && y < maxDescY)
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
            if (!row.empty() && y < maxDescY + font.GetTextHeight(ui::FONT_SMALL) + 6)
                line(row, col::TextDim, ui::FONT_SMALL);
        }

        // Preview image
        if (m_previewTexture && m_previewW > 0 && m_previewH > 0)
        {
            const int availH = (l.details.Bottom() - conflictsReserve) - y - 16;
            if (availH >= 50)
            {
                const int boxH = std::min(availH, 170);
                const int boxW = std::min(l.details.w - 32, 280);
                const float scale = std::min(
                    static_cast<float>(boxW) / static_cast<float>(m_previewW),
                    static_cast<float>(boxH) / static_cast<float>(m_previewH));
                const int drawW = std::max(1, static_cast<int>(m_previewW * scale));
                const int drawH = std::max(1, static_cast<int>(m_previewH * scale));
                const int drawX = l.details.x + (l.details.w - drawW) / 2;
                const int drawY = y + 8 + (boxH - drawH) / 2;

                const ui::UiRect frameRect {drawX - 2, drawY - 2, drawW + 4, drawH + 4};
                renderer.FillRectScreen(frameRect.x, frameRect.y, frameRect.w, frameRect.h, col::PanelSunken);
                renderer.DrawRectScreen(frameRect.x, frameRect.y, frameRect.w, frameRect.h, col::Border);
                renderer.DrawTextureScreen(m_previewTexture, drawX, drawY, drawW, drawH);
                y = drawY + drawH + 8;
            }
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
        font.DrawString(
            ui::FitText(font, m_notice, ui::FONT_SMALL, l.applyBtn.x - l.downBtn.Right() - 24),
            (l.downBtn.Right() + l.applyBtn.x) / 2,
            l.backBtn.CenterY(),
            ui::FONT_SMALL,
            col::TextDim,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
    const bool canMove = count > 0 && m_entries[m_selected].enabled;
    ui::DrawButton(
        renderer, font, l.backBtn, _("BACK"), ui::ButtonStyle::Secondary, l.backBtn.Contains(m_mouseX, m_mouseY));
    ui::DrawButton(
        renderer, font, l.upBtn, _("UP"), ui::ButtonStyle::Ghost, l.upBtn.Contains(m_mouseX, m_mouseY), canMove);
    ui::DrawButton(
        renderer, font, l.downBtn, _("DOWN"), ui::ButtonStyle::Ghost, l.downBtn.Contains(m_mouseX, m_mouseY), canMove);
    const bool changed = EnabledIds() != m_applied;
    ui::DrawButton(
        renderer,
        font,
        l.applyBtn,
        _("APPLY"),
        ui::ButtonStyle::Primary,
        l.applyBtn.Contains(m_mouseX, m_mouseY),
        changed);
    if (l.backBtn.Contains(m_mouseX, m_mouseY) || l.applyBtn.Contains(m_mouseX, m_mouseY)
        || l.list.Contains(m_mouseX, m_mouseY))
        SoundRequests::MarkHover();
}

} // namespace OpenArtOfWar
