// Layout helpers shared by the MenuController*.cpp files (not part of the public interface)
#pragma once

#include "ui/MenuController.hpp"
#include "ui/MapFilter.hpp"
#include "ui/MatchOptions.hpp"
#include "audio/SoundRequests.hpp"
#include <nlohmann/json.hpp>
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "graphics/ShaderEffects.hpp"
#include "game/Sides.hpp"
#include "ui/UiKit.hpp"
#include "core/Localization.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/TextureManager.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "input/InputManager.hpp"
#include "audio/SoundEngine.hpp"
#include "game/MissionController.hpp"
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/Pathfinder.hpp"
#include "game/AIController.hpp"
#include "storage/SaveManager.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>
#include <sstream>
#include <filesystem>
#include <vector>

namespace OpenArtOfWar
{

using ui::LowerText;
using ui::PopLastUtf8;
using ui::UiRect;
namespace col = ui::color;

namespace menu_detail
{

// Music player in the bottom left corner: previous, track name, next; the main menu adds a seek bar with the time above
struct PlayerLayout
{
    UiRect panel, prev, next, name, seek;
};

inline PlayerLayout playerLayout(int windowH)
{
    PlayerLayout l;
    l.panel = {24, windowH - 24 - 40, 340, 40};
    l.prev = {l.panel.x, l.panel.y, 40, 40};
    l.next = {l.panel.Right() - 40, l.panel.y, 40, 40};
    l.name = {l.prev.Right() + 8, l.panel.y, l.panel.w - 2 * 40 - 16, 40};
    l.seek = {l.panel.x, l.panel.y - 30, l.panel.w, 22};
    return l;
}

inline std::string FormatTime(double seconds)
{
    const int s = std::max(0, static_cast<int>(seconds));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

// First visible line for a thumb dragged to mouse height `my` along the scroll bar `track`
inline int ScrollTopFromMouse(const UiRect &track, int my, int total, int visible)
{
    if (total <= visible)
        return 0;
    const int thumbH = std::max(20, track.h * visible / total);
    const float ratio
        = std::clamp(static_cast<float>(my - track.y - thumbH / 2) / std::max(1, track.h - thumbH), 0.0f, 1.0f);
    return static_cast<int>(ratio * (total - visible) + 0.5f);
}

// Scroll bars are 4px wide: grab them from a wider strip
inline bool OnScrollBar(const UiRect &track, int mx, int my)
{
    return track.w > 0 && mx >= track.x - 8 && mx <= track.Right() + 8 && my >= track.y && my <= track.Bottom();
}

// Returns true when the mouse took the click (or holds the seek bar)
inline bool UpdateMusicPlayer(
    bool clicked,
    bool pressed,
    int px,
    int py,
    bool down,
    int mx,
    int my,
    int windowH,
    bool withSeek,
    SoundEngine &sound,
    int &drag)
{
    const PlayerLayout l = playerLayout(windowH);
    if (drag == 3)
    {
        if (!down)
        {
            drag = 0;
            return clicked;
        } // the release click ends the drag, it presses nothing
        // Seeking re-reads the stream: follow the mouse a few times a second, not every frame
        static Uint64 lastSeek = 0;
        const Uint64 now = SDL_GetTicks();
        if (now - lastSeek >= 120)
        {
            lastSeek = now;
            sound.SeekTrack(static_cast<double>(mx - l.seek.x) / l.seek.w);
        }
        return true;
    }
    if (withSeek && pressed && down && l.seek.Contains(px, py))
    {
        sound.SeekTrack(static_cast<double>(px - l.seek.x) / l.seek.w);
        drag = 3;
        return true;
    }
    if (!clicked)
        return false;
    if (l.prev.Contains(mx, my))
    {
        sound.PrevTrack();
        return true;
    }
    if (l.next.Contains(mx, my))
    {
        sound.NextTrack();
        return true;
    }
    if (withSeek && l.seek.Contains(mx, my))
        return true;
    return l.panel.Contains(mx, my);
}

inline void RenderMusicPlayer(
    Renderer &renderer,
    BitmapFontRenderer &fontRenderer,
    const SoundEngine &sound,
    int windowH,
    bool withSeek,
    int mx,
    int my)
{
    const PlayerLayout l = playerLayout(windowH);
    renderer.FillRectScreen(l.name.x, l.name.y, l.name.w, l.name.h, col::PanelSunken);
    renderer.DrawRectScreen(l.name.x, l.name.y, l.name.w, l.name.h, col::Border);
    fontRenderer.DrawString(
        ui::FitText(fontRenderer, sound.GetCurrentTrackName(), ui::FONT_SMALL, l.name.w - 16),
        l.name.CenterX(),
        l.name.CenterY(),
        ui::FONT_SMALL,
        col::TextDim,
        false,
        Anchor::HCENTER | Anchor::VCENTER);
    ui::DrawButton(renderer, fontRenderer, l.prev, "<", ui::ButtonStyle::Secondary, l.prev.Contains(mx, my));
    ui::DrawButton(renderer, fontRenderer, l.next, ">", ui::ButtonStyle::Secondary, l.next.Contains(mx, my));
    if (!withSeek)
        return;

    const double length = sound.GetTrackLength();
    const double ratio = length > 0.0 ? std::clamp(sound.GetTrackPosition() / length, 0.0, 1.0) : 0.0;
    const std::string elapsed = FormatTime(sound.GetTrackPosition());
    const std::string total = FormatTime(length);
    const int labelW = 44;
    const int barX = l.seek.x + labelW, barW = l.seek.w - 2 * labelW;
    const int barY = l.seek.CenterY() - 2;
    fontRenderer.DrawString(
        elapsed, l.seek.x, l.seek.CenterY(), ui::FONT_SMALL, col::TextDim, false, Anchor::LEFT | Anchor::VCENTER);
    fontRenderer.DrawString(
        total, l.seek.Right(), l.seek.CenterY(), ui::FONT_SMALL, col::TextDim, false, Anchor::RIGHT | Anchor::VCENTER);
    renderer.FillRectScreen(barX, barY, barW, 4, col::PanelSunken);
    renderer.FillRectScreen(barX, barY, static_cast<int>(barW * ratio), 4, col::Accent);
    renderer.FillRectScreen(barX + static_cast<int>(barW * ratio) - 3, l.seek.CenterY() - 7, 6, 14, col::Text);
}

inline UiRect DialogFooterButton(const UiRect &dialog, int w = 180)
{
    return {dialog.Right() - 20 - w, dialog.Bottom() - 20 - 40, w, 40};
}

inline std::string DifficultyName(AIDifficulty d)
{
    switch (d)
    {
        case AIDifficulty::Easy:
            return _("Easy");
        case AIDifficulty::Medium:
            return _("Medium");
        case AIDifficulty::Hard:
            return _("Hard");
        case AIDifficulty::Hardcore:
            return _("Hardcore");
    }
    return {};
}

// Section heading + body text block used by briefing / objectives / campaign details
inline int DrawTextSection(
    Renderer &r,
    BitmapFontRenderer &font,
    int x,
    int y,
    int w,
    const std::string &caption,
    const std::string &body,
    SDL_Color bodyColor,
    int bodyScale)
{
    ui::DrawCaption(font, r, x, y, caption);
    y += 24;
    y += font.DrawTextWrapped(body, x, y, w, bodyScale, bodyColor, false);
    return y + 18;
}

} // namespace menu_detail

using namespace menu_detail;

} // namespace OpenArtOfWar
