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
#include "ui/UiLayout.hpp"
#include "TouchMenuGeometry.hpp"
#include "core/Localization.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/SpriteRegistry.hpp"
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

// Music player centered at the bottom: previous, track name, next; the main menu adds a seek bar with the time above
struct PlayerLayout
{
    UiRect panel, prev, next, name, seek;
};

inline PlayerLayout playerLayout(int windowW, int windowH, int offsetX = 0, int offsetY = 0)
{
    PlayerLayout l;
    const bool touch = uilayout::MenuProfile(uilayout::Current(), "main_menu") == "touch";
    const int btnSz = touch ? 80 : 40;
    const int w = touch ? TouchPlayerWidth(windowW) : std::min(windowW - 48, 460);
    const int x = std::max(24, (windowW - w) / 2);
    l.panel = {x, windowH - (touch ? 20 : 24) - btnSz, w, btnSz};
    l.prev = {l.panel.x, l.panel.y, btnSz, btnSz};
    l.next = {l.panel.Right() - btnSz, l.panel.y, btnSz, btnSz};
    l.name = {l.prev.Right() + 8, l.panel.y, l.panel.w - 2 * btnSz - 16, btnSz};
    l.seek = {l.panel.x, l.panel.y - (touch ? 34 : 30), l.panel.w, touch ? 26 : 22};
    l.panel.x += offsetX;
    l.prev.x += offsetX;
    l.next.x += offsetX;
    l.name.x += offsetX;
    l.seek.x += offsetX;
    l.panel.y += offsetY;
    l.prev.y += offsetY;
    l.next.y += offsetY;
    l.name.y += offsetY;
    l.seek.y += offsetY;
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
    int windowW,
    int windowH,
    bool withSeek,
    SoundEngine &sound,
    int &drag,
    int offsetX = 0,
    int offsetY = 0)
{
    const PlayerLayout l = playerLayout(windowW, windowH, offsetX, offsetY);
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
    int windowW,
    int windowH,
    bool withSeek,
    int mx,
    int my,
    int offsetX = 0,
    int offsetY = 0)
{
    static const CachedSprite sPrevSprite("ui_music_player.prev", SpriteKind::Frame);
    static const CachedSprite sNextSprite("ui_music_player.next", SpriteKind::Frame);
    static const CachedSprite sNoteSprite("ui_music_player.note", SpriteKind::Frame);

    const PlayerLayout l = playerLayout(windowW, windowH, offsetX, offsetY);
    const bool touch = l.prev.h >= 72;
    renderer.FillRectScreen(l.name.x, l.name.y, l.name.w, l.name.h, col::PanelSunken);
    renderer.DrawRectScreen(l.name.x, l.name.y, l.name.w, l.name.h, col::Border);

    const int noteId = sNoteSprite.Get();
    int textLeft = l.name.x + 8;
    if (noteId >= 0)
    {
        const int noteSz = touch ? 30 : 20;
        renderer.DrawSpritePartScreen(
            noteId,
            {static_cast<float>(l.name.x + 8),
             static_cast<float>(l.name.CenterY() - noteSz / 2),
             static_cast<float>(noteSz),
             static_cast<float>(noteSz)});
        textLeft = l.name.x + 8 + noteSz + 6;
    }
    const int textFont = touch ? ui::FONT_BODY : ui::FONT_SMALL;
    const int textW = std::max(20, l.name.Right() - 8 - textLeft);
    fontRenderer.DrawString(
        ui::FitText(fontRenderer, sound.GetCurrentTrackName(), textFont, textW),
        textLeft + textW / 2,
        l.name.CenterY(),
        textFont,
        col::TextDim,
        false,
        Anchor::HCENTER | Anchor::VCENTER);

    auto drawIconBtn = [&](const UiRect &btn, int spriteId, const char *fallback) {
        ui::DrawButton(
            renderer,
            fontRenderer,
            btn,
            spriteId >= 0 ? "" : fallback,
            ui::ButtonStyle::Secondary,
            btn.Contains(mx, my));
        if (spriteId >= 0)
        {
            const int iconSz = touch ? 44 : 22;
            renderer.DrawSpritePartScreen(
                spriteId,
                {static_cast<float>(btn.CenterX() - iconSz / 2),
                 static_cast<float>(btn.CenterY() - iconSz / 2),
                 static_cast<float>(iconSz),
                 static_cast<float>(iconSz)});
        }
    };
    drawIconBtn(l.prev, sPrevSprite.Get(), "<");
    drawIconBtn(l.next, sNextSprite.Get(), ">");
    if (!withSeek)
        return;

    const double length = sound.GetTrackLength();
    const double ratio = length > 0.0 ? std::clamp(sound.GetTrackPosition() / length, 0.0, 1.0) : 0.0;
    const std::string elapsed = FormatTime(sound.GetTrackPosition());
    const std::string total = FormatTime(length);
    const int labelW = touch ? 52 : 44;
    const int barX = l.seek.x + labelW, barW = l.seek.w - 2 * labelW;
    const int barH = touch ? 6 : 4;
    const int barY = l.seek.CenterY() - barH / 2;
    const int timeFont = touch ? ui::FONT_SMALL_BOLD : ui::FONT_SMALL;
    fontRenderer.DrawString(
        elapsed, l.seek.x, l.seek.CenterY(), timeFont, col::TextDim, false, Anchor::LEFT | Anchor::VCENTER);
    fontRenderer.DrawString(
        total, l.seek.Right(), l.seek.CenterY(), timeFont, col::TextDim, false, Anchor::RIGHT | Anchor::VCENTER);
    renderer.FillRectScreen(barX, barY, barW, barH, col::PanelSunken);
    renderer.FillRectScreen(barX, barY, static_cast<int>(barW * ratio), barH, col::Accent);
    const int knobW = touch ? 10 : 6;
    const int knobH = touch ? 22 : 14;
    renderer.FillRectScreen(
        barX + static_cast<int>(barW * ratio) - knobW / 2, l.seek.CenterY() - knobH / 2, knobW, knobH, col::Text);
}

inline UiRect DialogFooterButton(const UiRect &dialog, int w = 200)
{
    return {dialog.Right() - 20 - w, dialog.Bottom() - 20 - 48, w, 48};
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
    std::string_view caption,
    std::string_view body,
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
